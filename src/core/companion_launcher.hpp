// COMPANION-PROGRAM LAUNCHER: start a program the user chose (a logger, a digital-mode decoder, a rotator controller) alongside the receiver.
//
// THIS IS A MODEL, OFF BY DEFAULT, AND IT NEVER STARTS A PROCESS ITSELF. It decides whether a launch is allowed and what exactly would be run; the act of
// starting is an injected `Spawner` (an interface this header declares), so a test uses a stand-in and the real one is a few lines the wiring lane writes
// (hand-off in docs/feature-records/tuning.md). It is an OWNER DECISION to wire it: it executes programs.
//
// THE RULES (each one is a refusal with a reason, tested):
//   1. The master switch (setEnabled) is OFF until the user turns it on. Off: every launch is refused.
//   2. A program is started ONLY from a FULL PATH the user typed: an absolute Windows path with a drive letter ("C:\\Tools\\logger.exe", backslash or slash)
//      or an absolute POSIX path; never relative, never a UNC share (\\\\server\\...), never a device or extended path (\\\\?\\, \\\\.\\), never a name looked up
//      on PATH, never a URL, never with ".." as a path part, never with a wildcard, a quote, or a control character; at most 259 bytes; the file name must end
//      in .exe (Windows form) - not .bat, .cmd, .com, .ps1 or any script, because those are run by a shell and this model has no shell.
//   3. NO SHELL: the spawner receives the executable path and ARGUMENT LIST separately. quoteWindowsCommandLine() builds the one command-line string
//      Windows needs from them under the documented CommandLineToArgvW rules (a backslash run before a quote is doubled and the quote escaped; a trailing
//      backslash run before the closing quote is doubled), so an argument cannot split into two or inject a third. An argument holds no NUL or control
//      character and is at most 1024 bytes; at most 16 arguments.
//   4. WHO MAY ASK: a launch has an Origin. UserAction (the user pressed a button or chose a menu entry) and AppStart (the app is starting) are the only
//      origins that can launch; Remote (web page, API client), Plugin and Network are ALWAYS refused, whatever the entry says. AppStart additionally needs the
//      entry's own startWithApp flag, which the user ticked.
//   5. The entry must exist in the user's list (at most 8) with the path validated again at launch, and the file must exist: the check is an injected
//      `fileExists`, so a test needs no disk.
//   6. At most one launch of the same entry per 2 seconds (a double click does not start two copies).
//
// The list is SAVED as versioned JSON (version 1); another version or an invalid entry refuses the whole file. The saved fields are the label, the path,
// the arguments and the startWithApp flag, all typed by the user; the master switch is saved too and defaults to off.
//
// THREADS: one thread (the control thread). No file access here beyond the injected check; no process is created here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/tuning_text.hpp"

namespace cascade::core::tuning {

enum class LaunchOrigin { UserAction = 0, AppStart = 1, Remote = 2, Plugin = 3, Network = 4 };

struct CompanionEntry {
    std::string label;
    std::string path;                 // a full path, see rule 2
    std::vector<std::string> args;
    bool startWithApp = false;        // AppStart may launch this entry only when set
};

// The refusal of a path, or "" when it is acceptable.
inline std::string pathProblem(const std::string& p) {
    if (p.empty()) { return "the path is empty"; }
    if (p.size() > 259) { return "the path is longer than 259 bytes"; }
    for (const unsigned char c : p) {
        if (c < 0x20 || c == 0x7f) { return "the path holds a control character"; }
        if (c == '"' || c == '*' || c == '?' || c == '<' || c == '>' || c == '|') { return "the path holds a quote or wildcard character"; }
    }
    if (p.rfind("\\\\", 0) == 0 || p.rfind("//", 0) == 0) { return "a share, device or extended path is not allowed"; }
    const bool windows = p.size() >= 3 && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' && (p[2] == '\\' || p[2] == '/');
    const bool posix = p[0] == '/';
    if (!windows && !posix) { return "the path is not a full path"; }
    if (windows && p.find(':', 2) != std::string::npos) { return "a stream or second colon in the path"; }
    // path parts
    std::size_t i = windows ? 3 : 1;
    std::string last;
    while (i <= p.size()) {
        std::size_t j = i;
        while (j < p.size() && p[j] != '\\' && p[j] != '/') { ++j; }
        const std::string part = p.substr(i, j - i);
        if (part == "..") { return "the path holds .."; }
        if (part == "." ) { return "the path holds a . part"; }
        last = part;
        i = j + 1;
        if (j >= p.size()) { break; }
    }
    if (last.empty()) { return "the path names a folder"; }
    if (windows) {
        if (last.size() < 5) { return "the file is not an .exe"; }
        std::string ext = last.substr(last.size() - 4);
        for (char& c : ext) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); } }
        if (ext != ".exe") { return "the file is not an .exe"; }
        if (last.find(' ') == 0 || last.back() == ' ' || last.back() == '.') { return "the file name ends in a space or dot"; }
    }
    return "";
}

inline std::string argProblem(const std::string& a) {
    if (a.size() > 1024) { return "an argument is longer than 1024 bytes"; }
    for (const unsigned char c : a) { if (c < 0x20 || c == 0x7f) { return "an argument holds a control character"; } }
    return "";
}

inline std::string entryProblem(const CompanionEntry& e) {
    if (e.label.empty() || !validUserText(e.label, 64)) { return "the label is not valid"; }
    std::string p = pathProblem(e.path);
    if (!p.empty()) { return p; }
    if (e.args.size() > 16) { return "more than 16 arguments"; }
    for (const std::string& a : e.args) { p = argProblem(a); if (!p.empty()) { return p; } }
    return "";
}

// The command line Windows needs for CreateProcess from an executable path and its arguments, quoted so that CommandLineToArgvW gives them back unchanged.
inline std::string quoteWindowsArg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) { return a; }
    std::string o = "\"";
    std::size_t backslashes = 0;
    for (const char c : a) {
        if (c == '\\') { ++backslashes; continue; }
        if (c == '"') { o.append(backslashes * 2 + 1, '\\'); o.push_back('"'); }
        else { o.append(backslashes, '\\'); o.push_back(c); }
        backslashes = 0;
    }
    o.append(backslashes * 2, '\\');
    o.push_back('"');
    return o;
}

inline std::string quoteWindowsCommandLine(const std::string& exe, const std::vector<std::string>& args) {
    std::string o = quoteWindowsArg(exe);
    for (const std::string& a : args) { o += ' '; o += quoteWindowsArg(a); }
    return o;
}

// What starts a process. The real one is the wiring lane's; a test passes a stand-in.
class Spawner {
public:
    virtual ~Spawner() = default;
    // `exePath` is the validated full path (also the first argument of commandLine); `commandLine` is quoteWindowsCommandLine(). No shell is involved.
    virtual bool spawn(const std::string& exePath, const std::string& commandLine, std::string& err) = 0;
};

struct LaunchResult {
    bool launched = false;
    std::string reason;   // why not (empty when launched)
};

class CompanionLauncher {
public:
    static constexpr std::size_t kMaxEntries = 8;

    explicit CompanionLauncher(std::function<bool(const std::string&)> fileExists) : fileExists_(std::move(fileExists)) {}

    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    bool add(const CompanionEntry& e, std::string* err = nullptr) {
        if (entries_.size() >= kMaxEntries) { return fail(err, "more than 8 entries"); }
        const std::string p = entryProblem(e);
        if (!p.empty()) { return fail(err, p.c_str()); }
        entries_.push_back(e);
        lastMs_.push_back(-1e18);
        return true;
    }
    bool remove(std::size_t i) {
        if (i >= entries_.size()) { return false; }
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
        lastMs_.erase(lastMs_.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    const std::vector<CompanionEntry>& entries() const { return entries_; }

    // One launch request. `nowMs` is any monotonic-ish clock in milliseconds.
    LaunchResult launch(std::size_t index, LaunchOrigin origin, double nowMs, Spawner& spawner) {
        LaunchResult r;
        auto no = [&](const char* why) { r.reason = why; return r; };
        if (!enabled_) { return no("the launcher is off"); }
        if (origin != LaunchOrigin::UserAction && origin != LaunchOrigin::AppStart) { return no("only the user or the app start may launch a program"); }
        if (index >= entries_.size()) { return no("no such entry"); }
        const CompanionEntry& e = entries_[index];
        if (origin == LaunchOrigin::AppStart && !e.startWithApp) { return no("the entry is not set to start with the app"); }
        const std::string p = entryProblem(e);
        if (!p.empty()) { return no(p.c_str()); }
        if (!fileExists_ || !fileExists_(e.path)) { return no("the file does not exist"); }
        if (nowMs >= lastMs_[index] && nowMs - lastMs_[index] < 2000.0) { return no("started a moment ago"); }
        std::string err;
        if (!spawner.spawn(e.path, quoteWindowsCommandLine(e.path, e.args), err)) { return no(err.empty() ? "the program did not start" : err.c_str()); }
        lastMs_[index] = nowMs;
        r.launched = true;
        return r;
    }

    // The app is starting: launches every entry set to start with the app, each through the same rules. Returns how many started.
    int launchAtAppStart(double nowMs, Spawner& spawner) {
        int n = 0;
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].startWithApp && launch(i, LaunchOrigin::AppStart, nowMs, spawner).launched) { ++n; }
        }
        return n;
    }

    nlohmann::json toJson() const {
        nlohmann::json arr = nlohmann::json::array();
        for (const CompanionEntry& e : entries_) { arr.push_back({{"label", e.label}, {"path", e.path}, {"args", e.args}, {"startWithApp", e.startWithApp}}); }
        return {{"version", 1}, {"enabled", enabled_}, {"entries", arr}};
    }
    bool fromJson(const nlohmann::json& j, std::string* err = nullptr) {
        if (!j.is_object()) { return fail(err, "not an object"); }
        if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<long long>() != 1) { return fail(err, "unknown version"); }
        if (!j.contains("enabled") || !j["enabled"].is_boolean() || !j.contains("entries") || !j["entries"].is_array()) { return fail(err, "a field is missing"); }
        if (j["entries"].size() > kMaxEntries) { return fail(err, "more than 8 entries"); }
        std::vector<CompanionEntry> next;
        for (const auto& e : j["entries"]) {
            if (!e.is_object() || !e.contains("label") || !e["label"].is_string() || !e.contains("path") || !e["path"].is_string() || !e.contains("args") ||
                !e["args"].is_array() || !e.contains("startWithApp") || !e["startWithApp"].is_boolean()) {
                return fail(err, "an entry is damaged");
            }
            CompanionEntry c;
            c.label = e["label"].get<std::string>();
            c.path = e["path"].get<std::string>();
            c.startWithApp = e["startWithApp"].get<bool>();
            for (const auto& a : e["args"]) {
                if (!a.is_string()) { return fail(err, "an argument is not text"); }
                c.args.push_back(a.get<std::string>());
            }
            const std::string p = entryProblem(c);
            if (!p.empty()) { return fail(err, p.c_str()); }
            next.push_back(std::move(c));
        }
        entries_ = std::move(next);
        lastMs_.assign(entries_.size(), -1e18);
        enabled_ = j["enabled"].get<bool>();
        return true;
    }

private:
    static bool fail(std::string* err, const char* m) { if (err != nullptr) { *err = m; } return false; }
    std::function<bool(const std::string&)> fileExists_;
    std::vector<CompanionEntry> entries_;
    std::vector<double> lastMs_;
    bool enabled_ = false;   // OFF until the user turns it on
};

}  // namespace cascade::core::tuning
