// See crash_upload.hpp for the four rules that outrank this feature, what is
// sent, and what is never sent under any setting.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/crash_upload.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <windows.h>

#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace fs = std::filesystem;

namespace cascade::core {

namespace {

// Bounds on everything read off disk. A report is written by this application,
// but by the time it is read back it is a FILE on a machine we do not control,
// so every list it can grow is capped before it can become a request body.
constexpr std::size_t kMaxReportBytes = 1024 * 1024;
constexpr std::size_t kMaxThreads = 32;
constexpr std::size_t kMaxFramesPerThread = 64;
constexpr std::size_t kMaxLogLines = 256;
constexpr std::size_t kMaxPlugins = 32;
constexpr std::size_t kMaxModules = 256;
// A report nobody could send in a fortnight is not going to become sendable.
// Without this, a permanently 429ing server would keep one file on the retry
// list for ever - the failure rule 4 exists to prevent.
constexpr std::uint64_t kMaxReportAgeSeconds = 14ull * 24ull * 60ull * 60ull;

std::string trimSpace(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) { ++a; }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) { --b; }
    return s.substr(a, b - a);
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t nl = text.find('\n', at);
        if (nl == std::string::npos) {
            if (at < text.size()) { out.push_back(text.substr(at)); }
            break;
        }
        out.push_back(text.substr(at, nl - at));
        at = nl + 1;
    }
    for (std::string& s : out) {
        if (!s.empty() && s.back() == '\r') { s.pop_back(); }
    }
    return out;
}

std::uint64_t parseHex(const std::string& s) {
    return static_cast<std::uint64_t>(std::strtoull(s.c_str(), nullptr, 16));
}

// "cascade.exe+0x1A2B" -> ("cascade.exe", 0x1A2B); "0x00007FFA..." -> a raw
// address. False when the text is neither, which is how a line truncated by a
// handler that died mid-write is dropped rather than turned into a frame at
// offset zero inside a module named by half a word.
bool parseAddressText(const std::string& text, std::string& module, std::uint64_t& offset,
                      std::uint64_t& raw) {
    module.clear();
    offset = 0;
    raw = 0;
    const std::string t = trimSpace(text);
    if (t.empty()) { return false; }
    const std::size_t plus = t.find("+0x");
    if (plus != std::string::npos && plus > 0) {
        module = t.substr(0, plus);
        offset = parseHex(t.substr(plus + 3));
        return true;
    }
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
        raw = parseHex(t.substr(2));
        return true;
    }
    return false;
}

// "  name base=0x.. size=0x.. pdb=x.pdb build=ABCD". Only the name and the
// build id are wanted: the base and size describe a process that no longer
// exists.
void parseModuleLine(const std::string& line, std::string& name, std::string& buildId) {
    name.clear();
    buildId.clear();
    const std::string t = trimSpace(line);
    const std::size_t sp = t.find(' ');
    if (sp == std::string::npos || sp == 0) { return; }
    name = t.substr(0, sp);
    const std::size_t at = t.find(" build=");
    if (at == std::string::npos) { return; }
    std::string b = trimSpace(t.substr(at + 7));
    const std::size_t end = b.find(' ');
    if (end != std::string::npos) { b = b.substr(0, end); }
    // "(none)" is the writer's way of saying "this module carries no CodeView
    // record". It is not a build id and must never travel as one - a reader
    // would go looking for symbols filed under "(none)".
    if (b == "(none)") { b.clear(); }
    buildId = b;
}

// "--- thread 100 (gui, stalled) ---" and "--- stack (thread 24180) ---" both
// name their thread the same way.
std::uint64_t parseThreadId(const std::string& marker) {
    std::size_t at = marker.find("thread ");
    if (at == std::string::npos) { return 0; }
    at += 7;
    std::uint64_t v = 0;
    while (at < marker.size() && std::isdigit(static_cast<unsigned char>(marker[at])) != 0) {
        v = v * 10u + static_cast<std::uint64_t>(marker[at] - '0');
        ++at;
    }
    return v;
}

// A plugin's display name reduced to something comparable with a module file
// name, so "ADS-B 1.1.0" can find "adsb.dll" in the module table. Best effort
// ON PURPOSE: when it does not match, the plugin's build id is sent empty
// rather than guessed. Every FRAME carries its own build id regardless, and
// that is the one a reader actually resolves against.
std::string comparableName(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

std::string moduleStem(const std::string& fileName) {
    const std::size_t dot = fileName.find_last_of('.');
    return (dot == std::string::npos) ? fileName : fileName.substr(0, dot);
}

std::string epochStamp(std::uint64_t epoch) {
    const std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[40] = {};
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02dZ", tmv.tm_year + 1900,
                  tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

}  // namespace

// ---------------------------------------------------------------------------
// Parsing a report back off disk
// ---------------------------------------------------------------------------
bool parseReportText(const std::string& text, ParsedReport& out) {
    out = ParsedReport();
    if (text.empty()) { return false; }

    const std::vector<std::string> lines = splitLines(text);
    enum class Section { Header, Context, Modules, Stack, Log, Ignore };
    Section section = Section::Header;
    std::string addressText;
    bool sawKind = false;

    for (const std::string& raw : lines) {
        if (raw.rfind("--- ", 0) == 0) {
            if (raw.rfind("--- context ---", 0) == 0) {
                section = Section::Context;
            } else if (raw.rfind("--- modules ---", 0) == 0) {
                section = Section::Modules;
            } else if (raw.rfind("--- log", 0) == 0) {
                section = Section::Log;
            } else if (raw.find("thread") != std::string::npos) {
                if (out.threads.size() < kMaxThreads) {
                    ReportThread t;
                    t.id = parseThreadId(raw);
                    out.threads.push_back(t);
                    section = Section::Stack;
                } else {
                    // At the cap. Ignore the section rather than appending its
                    // frames to the previous thread, which would fabricate a
                    // stack that never existed.
                    section = Section::Ignore;
                }
            } else {
                section = Section::Ignore;
            }
            continue;
        }

        if (section == Section::Header || section == Section::Context) {
            const std::size_t colon = raw.find(':');
            if (colon == std::string::npos) { continue; }
            const std::string k = trimSpace(raw.substr(0, colon));
            const std::string v = trimSpace(raw.substr(colon + 1));
            if (section == Section::Header) {
                if (k == "kind") {
                    out.kind = v;
                    sawKind = true;
                } else if (k == "signature") {
                    out.signature = v;
                } else if (k == "address") {
                    addressText = v;
                } else if (k == "reason") {
                    // Carried since 0.66.0: the reason is what tells the
                    // dashboard an ABSORBED vendor fault (the guard filed
                    // this report and the process continued) from a process
                    // death at the same address. Dropping it made the two
                    // identical rows, and a survived teardown fault reopened
                    // a fixed signature because nobody could tell.
                    out.reason = v;
                } else if (k == "code") {
                    // The exception code, e.g. 0xC0000005, verbatim.
                    out.code = v;
                }
                // thread, stalled-ms, threshold-ms and threads are
                // deliberately not carried: the rest is either in the stack
                // or meaningless outside the dead process.
                continue;
            }
            if (k == "version") {
                out.version = v;
            } else if (k == "commit") {
                out.commit = v;
            } else if (k == "os") {
                out.os = v;
            } else if (k == "arch") {
                out.arch = v;
            } else if (k == "mode") {
                out.mode = v;
            } else if (k == "source") {
                out.source = v;
            } else if (k == "sample-rate") {
                out.sampleRateHz = std::strtod(v.c_str(), nullptr);
            } else if (k == "device-open") {
                out.deviceOpen = (v == "yes");
            } else if (k == "sdr-model") {
                out.sdrModel = (v == "(none)") ? std::string() : v;
            } else if (k == "plugin") {
                if (v != "(none)" && !v.empty() && out.plugins.size() < kMaxPlugins) {
                    out.plugins.push_back(v);
                }
            }
            continue;
        }

        if (section == Section::Modules) {
            std::string n, b;
            parseModuleLine(raw, n, b);
            if (!n.empty() && out.modules.size() < kMaxModules) {
                out.modules.emplace_back(n, b);
            }
            continue;
        }

        if (section == Section::Stack) {
            if (out.threads.empty()) { continue; }
            ReportThread& t = out.threads.back();
            if (t.frames.size() >= kMaxFramesPerThread) { continue; }
            ReportFrame f;
            if (!parseAddressText(raw, f.module, f.offset, f.rawAddress)) { continue; }
            t.frames.push_back(f);
            continue;
        }

        if (section == Section::Log) {
            if (out.log.size() < kMaxLogLines) { out.log.push_back(raw); }
            continue;
        }
    }

    // The writers end every file with a newline, which splitLines does not turn
    // into a trailing entry - but a report truncated mid-line can still leave
    // an empty tail, and an empty log line is not a log line.
    while (!out.log.empty() && trimSpace(out.log.back()).empty()) { out.log.pop_back(); }

    if (!sawKind) { return false; }
    // An unknown kind is refused rather than forwarded: the receiving end
    // accepts two, and inventing a third here would produce a 400 on every
    // send for ever.
    if (out.kind != "crash" && out.kind != "hang") { return false; }

    auto lookup = [&out](const std::string& name) -> std::string {
        for (const auto& kv : out.modules) {
            if (kv.first == name) { return kv.second; }
        }
        return std::string();
    };
    for (ReportThread& t : out.threads) {
        for (ReportFrame& f : t.frames) {
            if (!f.module.empty()) { f.buildId = lookup(f.module); }
        }
    }

    // A crash report names the faulting address in its header. A freeze report
    // does not: its signature was built from the TOP FRAME OF THE FIRST THREAD
    // captured, which is the stalled one, so that is the frame that has to name
    // the module here or the two would group differently.
    if (!addressText.empty()) {
        std::string m;
        std::uint64_t off = 0;
        std::uint64_t rawAddr = 0;
        if (parseAddressText(addressText, m, off, rawAddr)) {
            out.module = m;
            out.offset = off;
        }
    }
    if (out.module.empty() && !out.threads.empty() && !out.threads.front().frames.empty()) {
        const ReportFrame& f = out.threads.front().frames.front();
        out.module = f.module;
        out.offset = f.offset;
    }
    out.buildId = lookup(out.module);
    return true;
}

// ---------------------------------------------------------------------------
// The wire payload
// ---------------------------------------------------------------------------
namespace {

nlohmann::json buildPayload(const ParsedReport& r, const std::string& installId,
                            std::size_t maxLog, std::size_t maxThreads,
                            std::size_t maxFrames) {
    nlohmann::json j;
    // THE IDENTIFYING HALF IS NEVER TRIMMED. Everything below is what makes a
    // report groupable and symbolisable; a payload that dropped one of these
    // to fit would be a row in a table nobody can act on.
    j["schema"] = 1;
    j["kind"] = r.kind;
    j["version"] = r.version;
    j["commit"] = r.commit;
    j["buildId"] = r.buildId;
    j["module"] = r.module;
    j["offset"] = r.offset;
    j["signature"] = r.signature;
    j["os"] = r.os;
    j["arch"] = r.arch;
    // Verbatim from the report file; empty for a hang report, whose writer
    // has no such lines. Always present rather than omitted-when-empty — the
    // inventory below is exact both ways, and an optional key would let a
    // future writer stop sending them without any test noticing.
    j["reason"] = r.reason;
    j["code"] = r.code;
    // Empty when usage reporting is off, because then it does not exist. A
    // crash report must not be the thing that mints an identifier the user
    // switched off. See crash_upload.hpp.
    j["installId"] = installId;

    nlohmann::json plugins = nlohmann::json::array();
    for (const std::string& p : r.plugins) {
        const std::size_t sp = p.find_last_of(' ');
        const std::string name = (sp == std::string::npos) ? p : p.substr(0, sp);
        const std::string version = (sp == std::string::npos) ? std::string() : p.substr(sp + 1);
        std::string buildId;
        const std::string want = comparableName(name);
        for (const auto& kv : r.modules) {
            if (!want.empty() && comparableName(moduleStem(kv.first)) == want) {
                buildId = kv.second;
                break;
            }
        }
        nlohmann::json e;
        e["name"] = name;
        e["version"] = version;
        e["buildId"] = buildId;
        plugins.push_back(std::move(e));
    }
    j["plugins"] = std::move(plugins);

    nlohmann::json ctx = nlohmann::json::object();
    // Deliberately NOT a tuned frequency, a bookmark or a centre frequency:
    // what somebody listens to is the most sensitive thing this application
    // knows. tests/test_crash_upload.cpp asserts their absence from the bytes
    // that actually leave the machine.
    ctx["mode"] = r.mode;
    ctx["source"] = r.source;
    ctx["sampleRate"] = r.sampleRateHz;
    ctx["deviceOpen"] = r.deviceOpen;
    ctx["sdrModel"] = r.sdrModel;
    j["context"] = std::move(ctx);

    nlohmann::json log = nlohmann::json::array();
    if (maxLog > 0) {
        // The LAST maxLog lines: the end of the ring is the part next to the
        // fault. Keeping the first n would carry the least useful half.
        const std::size_t start = (r.log.size() > maxLog) ? r.log.size() - maxLog : 0;
        for (std::size_t i = start; i < r.log.size(); ++i) { log.push_back(r.log[i]); }
    }
    j["log"] = std::move(log);

    nlohmann::json threads = nlohmann::json::array();
    for (std::size_t ti = 0; ti < r.threads.size() && ti < maxThreads; ++ti) {
        const ReportThread& t = r.threads[ti];
        nlohmann::json frames = nlohmann::json::array();
        for (std::size_t fi = 0; fi < t.frames.size() && fi < maxFrames; ++fi) {
            const ReportFrame& f = t.frames[fi];
            nlohmann::json e;
            e["module"] = f.module;
            e["buildId"] = f.buildId;
            // An unattributed frame keeps its raw address in the offset field
            // with an empty module: "we could not attribute this" is
            // information, and dropping the frame would silently shorten a
            // stack.
            e["offset"] = f.module.empty() ? f.rawAddress : f.offset;
            frames.push_back(std::move(e));
        }
        nlohmann::json e;
        e["id"] = t.id;
        e["frames"] = std::move(frames);
        threads.push_back(std::move(e));
    }
    j["threads"] = std::move(threads);
    return j;
}

std::string dumpPayload(const nlohmann::json& j) {
    // replace, not throw: a plugin name is third-party text and must not be
    // able to make a crash report unserialisable - which would mean the one
    // fault most worth reporting is the one that cannot be.
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace

std::string uploadJson(const ParsedReport& r, const std::string& installId) {
    // The trimming ladder. Each rung drops the least identifying thing left,
    // so a report from a pathological session still arrives naming its bug.
    struct Level {
        std::size_t log;
        std::size_t threads;
        std::size_t frames;
    };
    static const Level kLevels[] = {
        {kMaxLogLines, kMaxThreads, kMaxFramesPerThread},
        {64, 16, 48},
        {16, 8, 32},
        {0, 4, 24},
        {0, 1, 16},
        {0, 0, 0},
    };
    std::string last;
    for (const Level& lv : kLevels) {
        last = dumpPayload(buildPayload(r, installId, lv.log, lv.threads, lv.frames));
        if (last.size() <= kMaxUploadBytes) { return last; }
    }
    // Still too large with nothing left to drop. Returned anyway: the sweep
    // refuses it locally and marks the report `too-large`, rather than making
    // the server say 413 to learn what the client already knew.
    return last;
}

const std::vector<std::string>& uploadFieldNames() {
    // THE INVENTORY. PRIVACY.md documents these field by field, and
    // tests/test_crash_upload.cpp compares this list with what uploadJson()
    // emits IN BOTH DIRECTIONS.
    static const std::vector<std::string> names = {
        "schema",  "kind",    "version", "commit",  "buildId", "module",
        "offset",  "signature", "os",    "arch",    "reason",  "code",
        "installId", "plugins", "context", "log",   "threads"};
    return names;
}

const std::vector<std::string>& uploadContextFieldNames() {
    static const std::vector<std::string> names = {"mode", "source", "sampleRate", "deviceOpen",
                                                   "sdrModel"};
    return names;
}

// ---------------------------------------------------------------------------
// Client-side rate limiting and deduplication
// ---------------------------------------------------------------------------
namespace {

bool splitRecent(const std::string& entry, std::string& sig, std::uint64_t& when) {
    const std::size_t sp = entry.find(' ');
    if (sp == std::string::npos || sp == 0) { return false; }
    sig = entry.substr(0, sp);
    const std::string t = entry.substr(sp + 1);
    if (t.empty() || t.size() > 20) { return false; }
    for (char c : t) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) { return false; }
    }
    if (sig.size() > 64) { return false; }
    for (char c : sig) {
        if (std::isalnum(static_cast<unsigned char>(c)) == 0) { return false; }
    }
    when = static_cast<std::uint64_t>(std::strtoull(t.c_str(), nullptr, 10));
    return true;
}

}  // namespace

UploadDecision decideUpload(const UploadPolicyState& state, bool enabled,
                            const std::string& signature, std::uint64_t nowEpoch) {
    // Off means off, and so does "nothing to group this by": a report with no
    // signature could never be counted with its repeats, so sending it would
    // add a row and no knowledge.
    if (!enabled || signature.empty()) { return UploadDecision::Disabled; }
    if (state.blockedUntil > nowEpoch) { return UploadDecision::Backoff; }

    for (const std::string& entry : state.recent) {
        std::string sig;
        std::uint64_t when = 0;
        if (!splitRecent(entry, sig, when)) { continue; }
        if (sig != signature) { continue; }
        // "Still crashing a day later" is new information; "crashed again four
        // seconds later" is the same information twice.
        if (nowEpoch >= when && nowEpoch - when <= kDedupSeconds) {
            return UploadDecision::Duplicate;
        }
    }

    const bool windowLive =
        (state.windowStart != 0) && (nowEpoch >= state.windowStart) &&
        (nowEpoch - state.windowStart < kWindowSeconds);
    if (windowLive && state.windowCount >= static_cast<std::uint32_t>(kMaxPerWindow)) {
        return UploadDecision::RateLimited;
    }
    return UploadDecision::Send;
}

void noteSent(UploadPolicyState& state, const std::string& signature, std::uint64_t nowEpoch) {
    const bool windowLive =
        (state.windowStart != 0) && (nowEpoch >= state.windowStart) &&
        (nowEpoch - state.windowStart < kWindowSeconds);
    if (!windowLive) {
        state.windowStart = nowEpoch;
        state.windowCount = 0;
    }
    ++state.windowCount;

    // One entry per signature: a repeat refreshes the timestamp rather than
    // filling the memory with copies of the fault that is already known.
    for (auto it = state.recent.begin(); it != state.recent.end();) {
        std::string sig;
        std::uint64_t when = 0;
        if (splitRecent(*it, sig, when) && sig == signature) {
            it = state.recent.erase(it);
        } else {
            ++it;
        }
    }
    state.recent.push_back(signature + " " + std::to_string(nowEpoch));
    while (state.recent.size() > kMaxRecentSignatures) { state.recent.erase(state.recent.begin()); }
}

void noteRateLimited(UploadPolicyState& state, std::uint64_t nowEpoch,
                     std::uint64_t retryAfterSeconds) {
    std::uint64_t wait = retryAfterSeconds;
    if (wait == 0) { wait = kDefaultBackoffSeconds; }
    // A hostile or broken header can neither disable the limit nor mute the
    // client for a year.
    if (wait < kDefaultBackoffSeconds / 60) { wait = kDefaultBackoffSeconds / 60; }
    if (wait > kMaxBackoffSeconds) { wait = kMaxBackoffSeconds; }
    const std::uint64_t until = nowEpoch + wait;
    if (until > state.blockedUntil) { state.blockedUntil = until; }
}

std::vector<std::string> encodePolicyRecent(const UploadPolicyState& state) {
    return state.recent;
}

UploadPolicyState decodePolicyState(const std::vector<std::string>& recent,
                                    std::uint64_t windowStart, std::uint32_t windowCount,
                                    std::uint64_t blockedUntil) {
    UploadPolicyState out;
    // VALIDATED, NOT TRUSTED: this is user-editable text on disk, and the one
    // thing it controls is how much this machine is allowed to send.
    for (const std::string& e : recent) {
        std::string sig;
        std::uint64_t when = 0;
        if (!splitRecent(e, sig, when)) { continue; }
        out.recent.push_back(sig + " " + std::to_string(when));
        if (out.recent.size() >= kMaxRecentSignatures) { break; }
    }
    out.windowStart = windowStart;
    out.windowCount = (windowCount > 1000u) ? 1000u : windowCount;
    out.blockedUntil = blockedUntil;
    return out;
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------
void UploadCancel::cancel() {
    cancelled_.store(true, std::memory_order_release);
    void* h = request_.exchange(nullptr, std::memory_order_acq_rel);
#if defined(_WIN32)
    // CLOSING THE REQUEST HANDLE IS THE CANCELLATION. A blocked
    // WinHttpReceiveResponse on that handle returns immediately with
    // ERROR_WINHTTP_OPERATION_CANCELLED instead of sitting out its timeout,
    // which is what keeps a hanging server from delaying shutdown. The
    // exchange above means exactly one of the two threads ever closes it.
    if (h != nullptr) { ::WinHttpCloseHandle(static_cast<HINTERNET>(h)); }
#else
    (void)h;
#endif
}

bool UploadCancel::publish(void* handle) {
    if (cancelled_.load(std::memory_order_acquire)) { return false; }
    request_.store(handle, std::memory_order_release);
    // Re-checked, because cancel() may have run between the test above and the
    // store - in which case it found nullptr and closed nothing, and this is
    // the only place left that can notice.
    if (cancelled_.load(std::memory_order_acquire)) {
        void* h = request_.exchange(nullptr, std::memory_order_acq_rel);
#if defined(_WIN32)
        if (h != nullptr) { ::WinHttpCloseHandle(static_cast<HINTERNET>(h)); }
#else
        (void)h;
#endif
        return false;
    }
    return true;
}

void* UploadCancel::take() { return request_.exchange(nullptr, std::memory_order_acq_rel); }

std::string crashUploadEndpoint() {
    // The project's own domain rather than the worker behind it: this string
    // is compiled into every shipped binary, so it has to be one that can be
    // repointed later without orphaning copies already installed.
#if defined(_WIN32)
    char buf[512] = {0};
    const DWORD n = ::GetEnvironmentVariableA("FOXSDR_CRASH_URL", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) { return std::string(buf, n); }

    // A REDIRECTED DIAGNOSTICS TREE HAS NOWHERE TO SEND, and that is a safety
    // interlock rather than a convenience. FOXSDR_DIAG_DIR already means "this
    // is a test tree, not a user's machine": tests/test_diag_hang.cpp wedges the
    // real application on purpose and tests/test_diagnostics.cpp launches it
    // repeatedly, so without this every `ctest` run would post fabricated
    // faults to the live endpoint from a machine that is not crashing. A test
    // that means to exercise the transport says so by setting FOXSDR_CRASH_URL,
    // which is what tests/test_crash_upload.cpp does.
    //
    // Probed with a deliberately tiny buffer: a variable that is SET but longer
    // than the buffer still returns the size it would need, which is non-zero,
    // so this answers "is it set" without caring what it says.
    char diag[8] = {0};
    if (::GetEnvironmentVariableA("FOXSDR_DIAG_DIR", diag, sizeof(diag)) > 0) {
        return std::string();
    }
#endif
    return std::string("https://foxsdr.com/api/crash");
}

#if defined(_WIN32)
namespace {

bool isLoopbackHost(const std::wstring& host) {
    return host == L"127.0.0.1" || host == L"localhost" || host == L"::1";
}

std::uint64_t queryRetryAfter(HINTERNET req) {
    wchar_t buf[64] = {};
    DWORD len = sizeof(buf);
    if (::WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, L"Retry-After", buf, &len,
                              WINHTTP_NO_HEADER_INDEX) == 0) {
        return 0;
    }
    // Only the delta-seconds form is honoured. An HTTP-date would need a
    // parser and a clock comparison for a header this endpoint controls, and
    // an unparsed value falls back to the default backoff, which is the safe
    // direction.
    return static_cast<std::uint64_t>(::_wcstoui64(buf, nullptr, 10));
}

}  // namespace
#endif

UploadResult postCrashReport(const std::string& url, const std::string& json,
                             const std::shared_ptr<UploadCancel>& cancel) {
    UploadResult res;
#if defined(_WIN32)
    if (url.empty() || json.empty() || !cancel) { return res; }

    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0};
    wchar_t path[1024] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 1023;
    const std::wstring wurl(url.begin(), url.end());
    if (::WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) == 0) { return res; }

    const bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    // Plain http is refused off the loopback. A shipped binary can therefore
    // never be talked into putting a report - which carries an install id - in
    // clear on somebody's network; a test can still use a socket.
    if (!secure && !isLoopbackHost(host)) { return res; }

    HINTERNET ses = ::WinHttpOpen(L"FoxSDR-crash/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses == nullptr) { return res; }
    // Short on purpose. These bound the worst case even when cancellation is
    // never asked for - a background thread nobody is waiting on is still a
    // thread holding a socket open on a user's machine.
    ::WinHttpSetTimeouts(ses, 3000, 3000, 5000, 5000);

    HINTERNET con = ::WinHttpConnect(ses, host, uc.nPort, 0);
    if (con != nullptr) {
        HINTERNET req =
            ::WinHttpOpenRequest(con, L"POST", path, nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
        if (req != nullptr) {
            bool closed = false;
            if (!cancel->publish(req)) {
                res.cancelled = true;
                // publish() closed it on our behalf when the cancel landed in
                // the window; either way the handle is no longer ours.
                closed = true;
            } else {
                res.attempted = true;
                const wchar_t* kType = L"Content-Type: application/json\r\n";
                BOOL ok = ::WinHttpSendRequest(req, kType, static_cast<DWORD>(-1),
                                               const_cast<char*>(json.data()),
                                               static_cast<DWORD>(json.size()),
                                               static_cast<DWORD>(json.size()), 0);
                if (ok != 0) { ok = ::WinHttpReceiveResponse(req, nullptr); }
                if (ok != 0) {
                    DWORD status = 0;
                    DWORD len = sizeof(status);
                    if (::WinHttpQueryHeaders(
                            req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                            WINHTTP_NO_HEADER_INDEX) != 0) {
                        res.status = static_cast<int>(status);
                        res.accepted = (status >= 200 && status < 300);
                        if (status == 429) {
                            res.rateLimited = true;
                            res.retryAfterSeconds = queryRetryAfter(req);
                        }
                    }
                    // The body is never read. There is nothing the server could
                    // say that this client should obey - no config, no
                    // commands, no identifiers - and not reading it is the
                    // simplest way to guarantee that stays true.
                } else if (::GetLastError() == ERROR_WINHTTP_OPERATION_CANCELLED) {
                    res.cancelled = true;
                }
                void* mine = cancel->take();
                if (mine != nullptr) {
                    ::WinHttpCloseHandle(static_cast<HINTERNET>(mine));
                }
                closed = true;  // either we closed it, or cancel() did
            }
            if (!closed) { ::WinHttpCloseHandle(req); }
        }
        ::WinHttpCloseHandle(con);
    }
    ::WinHttpCloseHandle(ses);
#else
    (void)url;
    (void)json;
    (void)cancel;
#endif
    return res;
}

// ---------------------------------------------------------------------------
// The sweep
// ---------------------------------------------------------------------------
std::string uploadSidecarPath(const std::string& reportPath) {
    if (reportPath.empty()) { return std::string(); }
    return reportPath + ".upload";
}

namespace {

struct Sidecar {
    std::string status;
    int attempts = 0;
};

Sidecar readSidecar(const std::string& path) {
    Sidecar s;
    std::ifstream in(path, std::ios::binary);
    if (!in) { return s; }
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) { continue; }
        const std::string k = trimSpace(line.substr(0, colon));
        const std::string v = trimSpace(line.substr(colon + 1));
        if (k == "status") {
            s.status = v;
        } else if (k == "attempts") {
            s.attempts = std::atoi(v.c_str());
        }
    }
    return s;
}

// A status nothing will change. Everything else is retried on a later start,
// up to kMaxAttempts.
bool terminalStatus(const std::string& s) {
    return s == "sent" || s == "duplicate" || s == "abandoned" || s == "refused" ||
           s == "too-large" || s == "expired";
}

void writeSidecar(const std::string& reportPath, const std::string& status, int attempts,
                  const std::string& signature, std::uint64_t nowEpoch,
                  const std::string& note) {
    const std::string path = uploadSidecarPath(reportPath);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { return; }
    // Written for a PERSON, because the person who reads it is the one asking
    // "did my crash report go anywhere". Machine-readable enough for a test,
    // plain enough for a support conversation.
    out << "status: " << status << "\n";
    out << "attempts: " << attempts << "\n";
    out << "at: " << epochStamp(nowEpoch) << "\n";
    out << "signature: " << (signature.empty() ? std::string("(none)") : signature) << "\n";
    out << "note: " << note << "\n";
}

std::string readCapped(const fs::path& p, std::size_t cap) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { return std::string(); }
    std::string out;
    out.resize(cap);
    in.read(&out[0], static_cast<std::streamsize>(cap));
    out.resize(static_cast<std::size_t>(in.gcount()));
    return out;
}

std::uint64_t fileEpoch(const fs::path& p) {
    std::error_code ec;
    const auto ft = fs::last_write_time(p, ec);
    if (ec) { return 0; }
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t t = std::chrono::system_clock::to_time_t(sys);
    return (t < 0) ? 0 : static_cast<std::uint64_t>(t);
}

bool isReportName(const std::string& name) {
    if (name.size() < 5) { return false; }
    if (name.compare(name.size() - 4, 4, ".txt") != 0) { return false; }
    // crash-*.txt and hang-*.txt ONLY. A .dmp is process memory and is never
    // opened here, let alone sent; diagnostics.txt is the bundle the user
    // copied for themselves and is not ours to transmit.
    return name.rfind("crash-", 0) == 0 || name.rfind("hang-", 0) == 0;
}

}  // namespace

SweepOutcome sweepCrashDir(const SweepParams& params,
                           const std::shared_ptr<UploadCancel>& cancel) {
    SweepOutcome out;
    out.state = params.state;
    // OFF MEANS OFF. Not a socket, not a sidecar, not a directory listing: a
    // machine whose owner switched this off is not a machine this feature
    // writes to.
    if (!params.enabled || params.crashDir.empty() || params.url.empty() || !cancel) {
        return out;
    }

    std::error_code ec;
    if (!fs::is_directory(fs::path(params.crashDir), ec)) { return out; }

    std::vector<fs::path> candidates;
    for (const fs::directory_entry& e : fs::directory_iterator(params.crashDir, ec)) {
        if (ec) { break; }
        if (!e.is_regular_file(ec)) { continue; }
        const std::string name = e.path().filename().string();
        if (!isReportName(name)) { continue; }
        const Sidecar sc = readSidecar(uploadSidecarPath(e.path().string()));
        if (terminalStatus(sc.status)) { continue; }
        candidates.push_back(e.path());
    }
    // Newest first: the report that describes what just happened is worth more
    // than one from a fortnight ago, and the per-start cap means the older ones
    // may never get a turn.
    std::sort(candidates.begin(), candidates.end(),
              [](const fs::path& a, const fs::path& b) {
                  return a.filename().string() > b.filename().string();
              });
    if (params.maxReports > 0 &&
        candidates.size() > static_cast<std::size_t>(params.maxReports)) {
        candidates.resize(static_cast<std::size_t>(params.maxReports));
    }

    for (const fs::path& p : candidates) {
        if (cancel->cancelled()) { break; }
        const std::string path = p.string();
        const Sidecar sc = readSidecar(uploadSidecarPath(path));
        ++out.considered;

        const std::string text = readCapped(p, kMaxReportBytes);
        ParsedReport r;
        if (!parseReportText(text, r)) {
            ++out.refused;
            writeSidecar(path, "refused", sc.attempts, std::string(), params.nowEpoch,
                         "not a readable report; it was not sent and will not be retried");
            out.notes.push_back(p.filename().string() + ": refused (unreadable)");
            continue;
        }

        // Old enough that nothing is going to make it sendable. Marked and left
        // alone rather than kept on a list for ever.
        const std::uint64_t written = fileEpoch(p);
        if (written != 0 && params.nowEpoch > written &&
            params.nowEpoch - written > kMaxReportAgeSeconds) {
            ++out.abandoned;
            writeSidecar(path, "expired", sc.attempts, r.signature, params.nowEpoch,
                         "older than 14 days; it stays on this machine and is not retried");
            out.notes.push_back(p.filename().string() + ": expired");
            continue;
        }

        const UploadDecision d = decideUpload(out.state, true, r.signature, params.nowEpoch);
        if (d == UploadDecision::Duplicate) {
            ++out.duplicate;
            writeSidecar(path, "duplicate", sc.attempts, r.signature, params.nowEpoch,
                         "this exact fault was already reported; it stays on this machine");
            out.notes.push_back(p.filename().string() + ": duplicate of a report already sent");
            continue;
        }
        if (d == UploadDecision::RateLimited || d == UploadDecision::Backoff) {
            ++out.limited;
            // NOT an attempt: nothing was sent, so nothing was spent. The
            // report keeps its attempt budget for a start when sending is
            // allowed again.
            writeSidecar(path, d == UploadDecision::Backoff ? "backoff" : "rate-limited",
                         sc.attempts, r.signature, params.nowEpoch,
                         d == UploadDecision::Backoff
                             ? "the server asked us to wait; it will be retried later"
                             : "this machine has sent its limit for today; it will be retried");
            out.notes.push_back(p.filename().string() + ": held back");
            break;  // nothing later in the directory can pass either
        }
        if (d == UploadDecision::Disabled) {
            ++out.refused;
            writeSidecar(path, "refused", sc.attempts, r.signature, params.nowEpoch,
                         "the report carries no grouping signature and was not sent");
            continue;
        }

        const std::string body = uploadJson(r, params.installId);
        if (body.size() > kMaxUploadBytes) {
            ++out.refused;
            writeSidecar(path, "too-large", sc.attempts, r.signature, params.nowEpoch,
                         "too large to send; it stays on this machine");
            out.notes.push_back(p.filename().string() + ": too large to send");
            continue;
        }

        const UploadResult ur = postCrashReport(params.url, body, cancel);
        if (ur.cancelled || cancel->cancelled()) {
            // Shutdown. NOT counted as an attempt and NOT recorded: closing the
            // application must never cost a report one of its three tries.
            out.notes.push_back(p.filename().string() + ": abandoned for shutdown");
            break;
        }
        if (ur.accepted) {
            ++out.sent;
            noteSent(out.state, r.signature, params.nowEpoch);
            writeSidecar(path, "sent", sc.attempts + 1, r.signature, params.nowEpoch,
                         "sent to " + params.url);
            out.notes.push_back(p.filename().string() + ": sent");
            continue;
        }
        if (ur.rateLimited) {
            noteRateLimited(out.state, params.nowEpoch, ur.retryAfterSeconds);
            ++out.limited;
            writeSidecar(path, "backoff", sc.attempts, r.signature, params.nowEpoch,
                         "the server asked us to wait; it will be retried later");
            out.notes.push_back(p.filename().string() + ": rate limited by the server");
            break;  // honour it for the whole sweep, not just this file
        }
        if (ur.status == 400 || ur.status == 413) {
            ++out.refused;
            writeSidecar(path, ur.status == 413 ? "too-large" : "refused", sc.attempts + 1,
                         r.signature, params.nowEpoch,
                         "the server would not accept it; it will not be retried");
            out.notes.push_back(p.filename().string() + ": refused by the server");
            continue;
        }

        const int attempts = sc.attempts + 1;
        if (attempts >= kMaxAttempts) {
            ++out.abandoned;
            writeSidecar(path, "abandoned", attempts, r.signature, params.nowEpoch,
                         "could not be sent after " + std::to_string(attempts) +
                             " attempts; it stays on this machine and is not retried");
            out.notes.push_back(p.filename().string() + ": abandoned after " +
                                std::to_string(attempts) + " attempts");
        } else {
            ++out.failed;
            writeSidecar(path, "failed", attempts, r.signature, params.nowEpoch,
                         "could not be sent; it will be retried on a later start");
            out.notes.push_back(p.filename().string() + ": failed, will retry");
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// The owner of the sweep thread
// ---------------------------------------------------------------------------
CrashUploader::~CrashUploader() { stop(); }

void CrashUploader::start(const SweepParams& params) {
    if (thread_.joinable() || !params.enabled) { return; }
    cancel_ = std::make_shared<UploadCancel>();
    SweepParams copy = params;
    if (copy.nowEpoch == 0) {
        copy.nowEpoch = static_cast<std::uint64_t>(std::time(nullptr));
    }
    auto cancel = cancel_;
    thread_ = std::thread([this, copy, cancel]() {
        try {
            outcome_ = sweepCrashDir(copy, cancel);
        } catch (...) {
            // Silent by design. A diagnostics feature that interrupted
            // somebody's listening to complain it could not file a report
            // would be worse than having no diagnostics feature.
        }
    });
}

void CrashUploader::stop() {
    if (cancel_) { cancel_->cancel(); }
    if (thread_.joinable()) { thread_.join(); }
    cancel_.reset();
}

bool CrashUploader::busy() const { return thread_.joinable(); }

}  // namespace cascade::core
