// macros - named sequences of application commands with delays: the model, validation, loop protection, a deterministic player, load and save.
//
// STANDALONE, header-only; nothing is wired (docs/feature-records/core-formats.md, "WIRING HAND-OFF"). A macro step names an application command
// by the name core/app_commands.cpp gives it (cmd::opName: "SET_FREQUENCY", "SET_MODE", "RECORD_IQ", "DECODER_START", "APP_..." extension ops)
// with the FoxCommand slots (num[4], ival[2], text up to 255 bytes) and a delay before the NEXT step. Turning a step into a FoxCommand and
// applying it is the wiring lane's job; this header never includes app_commands.hpp, it is handed the list of names (KnownCommands), so the
// vocabulary is the application's, read from app_commands.cpp (the test builds it from cmd::knownOps / cmd::opName and checks every name).
//
// TWO PSEUDO-STEPS belong to the model: WAIT (no command, only the delay) and RUN_MACRO (text = another macro's name).
//
// SAFETY. A macro file is data that may come from anywhere, so its vocabulary is a ALLOW-LIST made by subtraction: every known command EXCEPT
// the denied prefixes in MacroPolicy (default: anything that transmits or keys a transmitter - TX_, APP_TX_ -, grants permissions, installs or
// removes plugins, changes settings or server configuration, reports or uploads, checks for updates, and test hooks). A macro that names a denied
// or unknown command is refused whole, at load and at validation, never skipped.
//
// LOOP PROTECTION. (1) a macro may not run itself, directly or through others: cycles are found by a walk over RUN_MACRO edges and refused;
// (2) nesting depth at most 4; (3) repeat at most 1000, and a macro that repeats must spend at least 100 ms of delay per pass (it cannot spin);
// (4) at most 10000 steps are executed by one start (counting every pass and every nested step); (5) at most 500 steps per macro, 200 macros,
// a delay at most 24 h, and a whole start at most 24 h. The player enforces (2)-(5) at run time as well as validation does ahead of time.
//
// THE PLAYER takes the clock as an argument (milliseconds, any monotonic origin): poll(now) returns the steps that are due, in order; between
// steps it waits the previous step's delay. It never sleeps and holds no threads, so it is exact in tests and the wiring decides which thread
// calls poll. cancel() stops it; a step that is returned is never returned again.
//
// FILE (JSON): {"version":1,"macros":[{"name","description","repeat","steps":[{"cmd","num":[<=4 numbers],"ival":[<=2 integers],"text","delayMs"}]}]}.
// Load refuses the whole file on the first error (naming macro and step): over 1 MiB, nesting deeper than 8 JSON levels, NUL, wrong types, unknown /
// denied commands, non-finite numbers, text over 255 bytes or with control characters, duplicate names, a RUN_MACRO to a macro that does not exist,
// cycles. Save validates, writes a temp file and renames it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cascade::core::macros {

constexpr const char* kWait = "WAIT";
constexpr const char* kRunMacro = "RUN_MACRO";

constexpr std::size_t kMaxMacros = 200;
constexpr std::size_t kMaxSteps = 500;
constexpr std::size_t kMaxNameLen = 64;
constexpr std::size_t kMaxText = 255;
constexpr std::int64_t kMaxDelayMs = 86400000;
constexpr int kMaxRepeat = 1000;
constexpr int kMaxDepth = 4;
constexpr std::uint64_t kMaxExecutedSteps = 10000;
constexpr std::int64_t kMinRepeatPassMs = 100;
constexpr std::size_t kMaxFileBytes = 1u << 20;

struct Step {
    std::string command;                 // an app command name, kWait, or kRunMacro
    double num[4] = {0, 0, 0, 0};
    std::int64_t ival[2] = {0, 0};
    std::string text;                    // RUN_MACRO: the macro's name
    std::int64_t delayMs = 0;            // wait after this step before the next
};

struct Macro {
    std::string name;
    std::string description;
    int repeat = 1;                      // passes over the steps
    std::vector<Step> steps;
};

// The command vocabulary: names only.
class KnownCommands {
public:
    KnownCommands() = default;
    explicit KnownCommands(std::vector<std::string> names) { for (auto& n : names) { names_.insert(std::move(n)); } }
    void add(const std::string& n) { names_.insert(n); }
    bool has(const std::string& n) const { return names_.count(n) != 0; }
    std::size_t size() const { return names_.size(); }
private:
    std::set<std::string> names_;
};

struct MacroPolicy {
    // Prefixes of commands in the vocabulary (cmd::knownOps) that a macro must not run: the transmitter, plugin rights, the plugin store, the
    // server, telemetry, other programs' addresses, test hooks, deleting a group of bookmarks. Commands the engine does not implement for
    // applyCommand (SETTING_SET, PROBLEM_REPORT, FEATURE_REQUEST, UPDATE_CHECK, ...) are not in the vocabulary and so are refused as unknown.
    std::vector<std::string> deniedPrefixes = {"TX_", "APP_TX_", "STORE_", "APP_STORE_", "PLUGIN_GRANT", "APP_PLUGIN_TX_GRANT", "PLUGIN_RESCAN", "SERVER_CONFIG",
                                               "APP_INTERFACE_CONFIG", "TELEMETRY_", "APP_TELEMETRY_", "APP_TEST_", "APP_SET_CATALOGUE_URL", "APP_USER_PRESET_FORGET",
                                               "BOOKMARK_REMOVE_GROUP", "APP_PLUGIN_REENABLE", "APP_SET_RTLTCP_ADDR", "APP_SET_PLUTO_URI"};
};

namespace detail {

inline bool hasControl(const std::string& s) { for (unsigned char c : s) { if (c < 0x20 || c == 0x7F) { return true; } } return false; }

inline bool denied(const MacroPolicy& p, const std::string& cmd) {
    for (const auto& pre : p.deniedPrefixes) { if (cmd.rfind(pre, 0) == 0) { return true; } }
    return false;
}

inline bool validCommandName(const std::string& s) {
    if (s.empty() || s.size() > 64) { return false; }
    for (char c : s) { if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) { return false; } }
    return true;
}

}  // namespace detail

// ---- validation -----------------------------------------------------------------------------------------------------------------------

// One macro on its own (RUN_MACRO targets are checked by validateSet). Returns "" if fine, else the first problem naming the step.
inline std::string validateMacro(const Macro& m, const KnownCommands& known, const MacroPolicy& policy = MacroPolicy{}) {
    if (m.name.empty() || m.name.size() > kMaxNameLen || detail::hasControl(m.name)) { return "the name must be 1 to 64 characters without control characters"; }
    if (m.description.size() > 500 || detail::hasControl(m.description)) { return "the description is over 500 bytes or has a control character"; }
    if (m.repeat < 1 || m.repeat > kMaxRepeat) { return "repeat must be 1 to 1000"; }
    if (m.steps.empty()) { return "a macro needs at least one step"; }
    if (m.steps.size() > kMaxSteps) { return "more than 500 steps"; }
    std::int64_t total = 0;
    for (std::size_t i = 0; i < m.steps.size(); ++i) {
        const Step& s = m.steps[i];
        const std::string at = "step " + std::to_string(i + 1) + ": ";
        if (s.delayMs < 0 || s.delayMs > kMaxDelayMs) { return at + "the delay must be 0 to 86400000 ms (24 h)"; }
        total += s.delayMs;
        for (double d : s.num) { if (!std::isfinite(d)) { return at + "a number is not finite"; } }
        if (s.text.size() > kMaxText || detail::hasControl(s.text)) { return at + "the text is over 255 bytes or has a control character"; }
        if (s.command == kWait) {
            if (!s.text.empty()) { return at + "WAIT takes no text"; }
        } else if (s.command == kRunMacro) {
            if (s.text.empty()) { return at + "RUN_MACRO needs the macro's name in the text"; }
        } else {
            if (!detail::validCommandName(s.command)) { return at + "'" + s.command.substr(0, 40) + "' is not a command name"; }
            if (!known.has(s.command)) { return at + "unknown command " + s.command; }
            if (detail::denied(policy, s.command)) { return at + "the command " + s.command + " is not allowed in a macro"; }
        }
    }
    if (m.repeat > 1 && total < kMinRepeatPassMs) { return "a macro that repeats must spend at least 100 ms of delay per pass (it would spin)"; }
    if (static_cast<std::int64_t>(m.repeat) * total > kMaxDelayMs) { return "the macro would run longer than 24 hours"; }
    if (static_cast<std::uint64_t>(m.repeat) * m.steps.size() > kMaxExecutedSteps) { return "repeat times steps is more than 10000"; }
    return {};
}

// A set: every macro valid, names unique, RUN_MACRO targets exist, no cycles, nesting depth within 4, and the executed step count of each macro
// (all nesting and repeats multiplied out) within 10000.
inline std::string validateSet(const std::vector<Macro>& macros, const KnownCommands& known, const MacroPolicy& policy = MacroPolicy{}) {
    if (macros.size() > kMaxMacros) { return "more than 200 macros"; }
    std::map<std::string, const Macro*> byName;
    for (const Macro& m : macros) {
        const std::string e = validateMacro(m, known, policy);
        if (!e.empty()) { return "macro '" + m.name.substr(0, 64) + "': " + e; }
        if (!byName.emplace(m.name, &m).second) { return "macro '" + m.name + "': the name is used twice"; }
    }
    for (const Macro& m : macros) {
        for (std::size_t i = 0; i < m.steps.size(); ++i) {
            if (m.steps[i].command == kRunMacro && byName.count(m.steps[i].text) == 0) {
                return "macro '" + m.name + "': step " + std::to_string(i + 1) + " runs '" + m.steps[i].text + "', which does not exist";
            }
        }
    }
    // cycles, depth and the executed size, by a depth-first walk with memoised sizes
    std::map<std::string, std::uint64_t> size;
    std::set<std::string> onPath;
    std::string problem;
    std::function<std::uint64_t(const Macro&, int)> walk = [&](const Macro& m, int depth) -> std::uint64_t {
        if (!problem.empty()) { return 0; }
        if (onPath.count(m.name) != 0) { problem = "macro '" + m.name + "' runs itself (a loop through RUN_MACRO)"; return 0; }
        if (depth > kMaxDepth) { problem = "macro '" + m.name + "' is nested more than 4 deep"; return 0; }
        onPath.insert(m.name);
        std::uint64_t once = 0;
        for (const Step& s : m.steps) {
            ++once;
            if (s.command == kRunMacro) { once += walk(*byName[s.text], depth + 1); }
            if (!problem.empty()) { onPath.erase(m.name); return 0; }
        }
        onPath.erase(m.name);
        const std::uint64_t all = once * static_cast<std::uint64_t>(m.repeat);
        if (all > kMaxExecutedSteps) { problem = "macro '" + m.name + "' would execute " + std::to_string(all) + " steps (the most is 10000)"; return 0; }
        size[m.name] = all;
        return all;
    };
    for (const Macro& m : macros) { walk(m, 1); if (!problem.empty()) { return problem; } }
    return {};
}

// ---- the player -------------------------------------------------------------------------------------------------------------------------

struct DueStep {
    std::string macro;                   // the macro the step belongs to (the innermost, for a nested run)
    std::size_t index = 0;               // step number in it, from 0
    int pass = 0;                        // repeat pass, from 0
    Step step;                           // a command step or a WAIT (RUN_MACRO is consumed by the player, never returned)
};

class Player {
public:
    // Starts `name` from `set` (the set must have passed validateSet; it is copied). False if the macro does not exist or a run is active.
    bool start(const std::vector<Macro>& set, const std::string& name, std::int64_t nowMs, std::string* err = nullptr) {
        if (running()) { return fail(err, "a macro is already running"); }
        set_.clear();
        for (const Macro& m : set) { set_[m.name] = m; }
        const auto it = set_.find(name);
        if (it == set_.end()) { return fail(err, "no macro named '" + name + "'"); }
        stack_.clear();
        stack_.push_back(Frame{&it->second, 0, 0});
        nextAt_ = nowMs;
        executed_ = 0;
        startedAt_ = nowMs;
        stopped_ = false;
        return true;
    }

    bool running() const { return !stack_.empty() && !stopped_; }
    void cancel() { stack_.clear(); stopped_ = true; }
    std::uint64_t executed() const { return executed_; }
    // The time of the next due step, or -1 when nothing is running.
    std::int64_t nextDueMs() const { return running() ? nextAt_ : -1; }
    // Why the player stopped on its own, "" if it finished or was cancelled.
    const std::string& stopReason() const { return reason_; }

    // Returns the steps due at `nowMs` (at most `maxSteps`), in order. Step times chain: each step is due its predecessor's delay after the
    // predecessor was DUE (not after it was polled), so a late poll catches up in one call instead of drifting.
    std::vector<DueStep> poll(std::int64_t nowMs, std::size_t maxSteps = 64) {
        std::vector<DueStep> out;
        while (running() && out.size() < maxSteps && nextAt_ <= nowMs) {
            Frame& f = stack_.back();
            if (f.index >= f.macro->steps.size()) {
                if (++f.pass >= f.macro->repeat) { stack_.pop_back(); continue; }
                f.index = 0;
                continue;
            }
            const Step& s = f.macro->steps[f.index];
            if (nextAt_ - startedAt_ > kMaxDelayMs) { stop("the run passed 24 hours"); break; }
            if (++executed_ > kMaxExecutedSteps) { stop("more than 10000 steps executed"); break; }
            const std::size_t idx = f.index++;
            if (s.command == kRunMacro) {
                const auto it = set_.find(s.text);
                if (it == set_.end() || static_cast<int>(stack_.size()) >= kMaxDepth) { stop(it == set_.end() ? "a macro it runs does not exist" : "nested more than 4 deep"); break; }
                for (const Frame& fr : stack_) { if (fr.macro == &it->second) { stop("a macro runs itself"); return out; } }
                nextAt_ += s.delayMs;
                stack_.push_back(Frame{&it->second, 0, 0});
                continue;
            }
            DueStep d;
            d.macro = f.macro->name;
            d.index = idx;
            d.pass = f.pass;
            d.step = s;
            out.push_back(std::move(d));
            nextAt_ += s.delayMs;
        }
        return out;
    }

private:
    struct Frame { const Macro* macro; std::size_t index; int pass; };
    static bool fail(std::string* err, const std::string& m) { if (err != nullptr) { *err = m; } return false; }
    void stop(const char* why) { reason_ = why; stack_.clear(); stopped_ = true; }

    std::map<std::string, Macro> set_;
    std::vector<Frame> stack_;
    std::int64_t nextAt_ = 0, startedAt_ = 0;
    std::uint64_t executed_ = 0;
    bool stopped_ = false;
    std::string reason_;
};

// ---- file ---------------------------------------------------------------------------------------------------------------------------------

struct LoadResult {
    std::vector<Macro> macros;
    std::string error;           // set when the file was refused (macros is then empty)
};

inline LoadResult parseMacros(const std::string& text, const KnownCommands& known, const MacroPolicy& policy = MacroPolicy{}) {
    LoadResult r;
    auto refuse = [&](const std::string& m) { r.macros.clear(); r.error = m; return r; };
    if (text.size() > kMaxFileBytes) { return refuse("the macro file is larger than 1 MiB"); }
    if (text.find('\0') != std::string::npos) { return refuse("the macro file contains a NUL byte"); }
    {
        int depth = 0;
        bool inStr = false, esc = false;
        for (char c : text) {
            if (inStr) { if (esc) { esc = false; } else if (c == '\\') { esc = true; } else if (c == '"') { inStr = false; } continue; }
            if (c == '"') { inStr = true; }
            else if (c == '[' || c == '{') { if (++depth > 8) { return refuse("the macro file is nested more than 8 levels deep"); } }
            else if (c == ']' || c == '}') { --depth; }
        }
    }
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) { return refuse("the macro file is not a JSON object"); }
    const auto ver = j.find("version");
    if (ver == j.end() || !ver->is_number_integer() || ver->get<std::int64_t>() != 1) { return refuse("unsupported or missing \"version\" (this reader knows version 1)"); }
    const auto ml = j.find("macros");
    if (ml == j.end() || !ml->is_array()) { return refuse("\"macros\" must be a list"); }
    if (ml->size() > kMaxMacros) { return refuse("more than 200 macros"); }
    std::size_t mi = 0;
    for (const auto& o : *ml) {
        ++mi;
        if (!o.is_object()) { return refuse("macro " + std::to_string(mi) + ": must be an object"); }
        Macro m;
        const auto nm = o.find("name");
        if (nm == o.end() || !nm->is_string()) { return refuse("macro " + std::to_string(mi) + ": needs a text \"name\""); }
        m.name = nm->get<std::string>();
        const std::string at = "macro " + std::to_string(mi) + " ('" + m.name.substr(0, 64) + "'): ";
        if (const auto d = o.find("description"); d != o.end()) { if (!d->is_string()) { return refuse(at + "\"description\" must be text"); } m.description = d->get<std::string>(); }
        if (const auto rp = o.find("repeat"); rp != o.end()) {
            if (!rp->is_number_integer() || rp->get<std::int64_t>() < 1 || rp->get<std::int64_t>() > kMaxRepeat) { return refuse(at + "\"repeat\" must be a whole number from 1 to 1000"); }
            m.repeat = static_cast<int>(rp->get<std::int64_t>());
        }
        const auto sl = o.find("steps");
        if (sl == o.end() || !sl->is_array()) { return refuse(at + "needs a list of \"steps\""); }
        if (sl->size() > kMaxSteps) { return refuse(at + "more than 500 steps"); }
        std::size_t si = 0;
        for (const auto& so : *sl) {
            ++si;
            const std::string sat = at + "step " + std::to_string(si) + ": ";
            if (!so.is_object()) { return refuse(sat + "must be an object"); }
            Step s;
            const auto c = so.find("cmd");
            if (c == so.end() || !c->is_string()) { return refuse(sat + "needs a text \"cmd\""); }
            s.command = c->get<std::string>();
            if (const auto n = so.find("num"); n != so.end()) {
                if (!n->is_array() || n->size() > 4) { return refuse(sat + "\"num\" must be a list of up to 4 numbers"); }
                for (std::size_t k = 0; k < n->size(); ++k) {
                    if (!(*n)[k].is_number()) { return refuse(sat + "\"num\" must hold only numbers"); }
                    s.num[k] = (*n)[k].get<double>();
                }
            }
            if (const auto iv = so.find("ival"); iv != so.end()) {
                if (!iv->is_array() || iv->size() > 2) { return refuse(sat + "\"ival\" must be a list of up to 2 whole numbers"); }
                for (std::size_t k = 0; k < iv->size(); ++k) {
                    if (!(*iv)[k].is_number_integer()) { return refuse(sat + "\"ival\" must hold only whole numbers"); }
                    s.ival[k] = (*iv)[k].get<std::int64_t>();
                }
            }
            if (const auto t = so.find("text"); t != so.end()) { if (!t->is_string()) { return refuse(sat + "\"text\" must be text"); } s.text = t->get<std::string>(); }
            if (const auto d = so.find("delayMs"); d != so.end()) {
                if (!d->is_number_integer()) { return refuse(sat + "\"delayMs\" must be a whole number"); }
                s.delayMs = d->get<std::int64_t>();
            }
            m.steps.push_back(std::move(s));
        }
        const std::string bad = validateMacro(m, known, policy);
        if (!bad.empty()) { return refuse(at + bad); }
        r.macros.push_back(std::move(m));
    }
    const std::string bad = validateSet(r.macros, known, policy);
    if (!bad.empty()) { return refuse(bad); }
    return r;
}

inline std::string macrosToJson(const std::vector<Macro>& macros) {
    nlohmann::json root = nlohmann::json::object();
    root["version"] = 1;
    nlohmann::json arr = nlohmann::json::array();
    for (const Macro& m : macros) {
        nlohmann::json o = nlohmann::json::object();
        o["name"] = m.name;
        if (!m.description.empty()) { o["description"] = m.description; }
        o["repeat"] = m.repeat;
        nlohmann::json steps = nlohmann::json::array();
        for (const Step& s : m.steps) {
            nlohmann::json so = nlohmann::json::object();
            so["cmd"] = s.command;
            int nn = 4;
            while (nn > 0 && s.num[nn - 1] == 0.0) { --nn; }
            if (nn > 0) { nlohmann::json a = nlohmann::json::array(); for (int k = 0; k < nn; ++k) { a.push_back(s.num[k]); } so["num"] = a; }
            int ni = 2;
            while (ni > 0 && s.ival[ni - 1] == 0) { --ni; }
            if (ni > 0) { nlohmann::json a = nlohmann::json::array(); for (int k = 0; k < ni; ++k) { a.push_back(s.ival[k]); } so["ival"] = a; }
            if (!s.text.empty()) { so["text"] = s.text; }
            so["delayMs"] = s.delayMs;
            steps.push_back(so);
        }
        o["steps"] = steps;
        arr.push_back(o);
    }
    root["macros"] = arr;
    return root.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

inline LoadResult loadMacrosFile(const std::string& path, const KnownCommands& known, const MacroPolicy& policy = MacroPolicy{}) {
    LoadResult r;
    std::error_code ec;
    const std::filesystem::path p(path);
    if (!std::filesystem::is_regular_file(p, ec)) { r.error = "cannot open " + path; return r; }
    const std::uintmax_t size = std::filesystem::file_size(p, ec);
    if (ec || size > kMaxFileBytes) { r.error = ec ? "cannot read the size of " + path : "the macro file is larger than 1 MiB"; return r; }
    std::ifstream f(p, std::ios::binary);
    if (!f) { r.error = "cannot open " + path; return r; }
    std::string text(static_cast<std::size_t>(size), ' ');
    f.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(f.gcount()));
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) { text.erase(0, 3); }
    return parseMacros(text, known, policy);
}

inline bool saveMacrosFile(const std::string& path, const std::vector<Macro>& macros, const KnownCommands& known, std::string* err, const MacroPolicy& policy = MacroPolicy{}) {
    const std::string bad = validateSet(macros, known, policy);
    if (!bad.empty()) { if (err != nullptr) { *err = bad; } return false; }
    const std::string text = macrosToJson(macros);
    std::filesystem::path tmp(path);
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { if (err != nullptr) { *err = "cannot create " + tmp.string(); } return false; }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) { std::error_code ec; std::filesystem::remove(tmp, ec); if (err != nullptr) { *err = "disk error while writing " + tmp.string(); } return false; }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, std::filesystem::path(path), ec);
    if (ec) { std::filesystem::remove(tmp, ec); if (err != nullptr) { *err = "cannot replace " + path; } return false; }
    return true;
}

}  // namespace cascade::core::macros
