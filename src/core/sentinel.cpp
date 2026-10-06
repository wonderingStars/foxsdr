// See sentinel.hpp for what the sentinel is, what it can see and the table it
// decides by. This file is everything about it that does not depend on the
// operating system: the vocabulary, the decision, the report, and the last act
// the watcher performs when the application has gone. The process plumbing is in
// sentinel_host_win.cpp and sentinel_host_posix.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/sentinel.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

#include "core/crash_handler.hpp"
#include "core/diag_history.hpp"
#include "core/diag_log.hpp"
#include "core/diag_report.hpp"
#include "core/frame_timing.hpp"
#include "core/hang_watchdog.hpp"
#include "core/telemetry.hpp"
#include "core/version.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace cascade::core {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Classes and their words
// ---------------------------------------------------------------------------
const char* sentinelClassId(SentinelClass c) {
    switch (c) {
        case SentinelClass::Crash: return "crash";
        case SentinelClass::Frozen: return "frozen";
        case SentinelClass::Startup: return "startup";
        case SentinelClass::Outside: return "outside";
        case SentinelClass::Session: return "session";
        case SentinelClass::None: break;
    }
    return "none";
}

const char* sentinelClassReason(SentinelClass c) {
    switch (c) {
        case SentinelClass::Crash: return kSentinelReasonCrash;
        case SentinelClass::Frozen: return kSentinelReasonFrozen;
        case SentinelClass::Startup: return kSentinelReasonStartup;
        case SentinelClass::Outside: return kSentinelReasonOutside;
        case SentinelClass::Session: return kSentinelReasonSession;
        case SentinelClass::None: break;
    }
    return "";
}

bool sentinelClassUploads(SentinelClass c) {
    return c == SentinelClass::Crash || c == SentinelClass::Frozen || c == SentinelClass::Startup;
}

namespace {
bool startsWith(const std::string& s, const char* prefix) {
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}
}  // namespace

bool sentinelReasonIsSentinel(const std::string& reason) {
    return startsWith(reason, kSentinelReasonPrefix);
}

bool sentinelReasonIsLocalOnly(const std::string& reason) {
    return startsWith(reason, kSentinelReasonOutside) || startsWith(reason, kSentinelReasonSession);
}

// ---------------------------------------------------------------------------
// The vocabulary
// ---------------------------------------------------------------------------
PhaseLabel sentinelPhaseLabel(const breadcrumb::Snapshot& b) {
    using breadcrumb::Phase;
    if (!b.valid) { return {"unknown", "unknown"}; }
    // What else was going on, while the application was up and not yet closing.
    if (b.phase >= Phase::BuildingApp && b.phase <= Phase::Running) {
        if ((b.activity & breadcrumb::kReloadingPlugins) != 0) {
            return b.phase < Phase::Running ? PhaseLabel{"loading-plugins", "loading plugins"}
                                            : PhaseLabel{"reloading-plugins", "reloading plugins"};
        }
        if ((b.activity & breadcrumb::kOpeningRadio) != 0) {
            return {"opening-radio", "opening a radio"};
        }
    }
    // Last of the three, because it is the least about the program: somebody was
    // holding the window, or reading a prompt.
    if (sentinelUserPaced(b)) { return {"user-held", "held by the user"}; }
    switch (b.phase) {
        case Phase::Starting: return {"starting", "starting"};
        case Phase::BuildingApp: return {"building-app", "building the application"};
        case Phase::CreatingWindow: return {"creating-window", "creating the window"};
        case Phase::AwaitingFirstFrame: return {"first-frame", "waiting for the first frame"};
        case Phase::Running: return {"running", "running"};
        case Phase::ShutdownBegun: return {"shutdown-saving", "shutting down: saving"};
        case Phase::ShutdownStoppingReceiver:
            return {"shutdown-receiver", "shutting down: stopping the receiver"};
        case Phase::ShutdownUnloadingPlugins:
            return {"shutdown-plugins", "shutting down: unloading plugins"};
        case Phase::ShutdownWritingMarker:
            return {"shutdown-marker", "shutting down: writing the exit marker"};
        case Phase::ShutdownClosingWindow:
            return {"shutdown-window", "shutting down: closing the window"};
        case Phase::Finished: return {"finished", "after shutdown"};
        case Phase::Unset: break;
    }
    return {"unknown", "unknown"};
}

const char* sentinelFrameScopeName(const breadcrumb::Snapshot& b) {
    // Only while the frame loop is turning: before the first frame there is no
    // scope, and after the loop has ended the last one written is history.
    if (!b.valid || b.phase != breadcrumb::Phase::Running) { return nullptr; }
    if (b.frameScope == 0 || b.frameScope > static_cast<std::uint32_t>(kFrameScopeCount)) {
        return nullptr;
    }
    return kFrameScopeNames[b.frameScope - 1];
}

bool sentinelUserPaced(const breadcrumb::Snapshot& b) {
    if (!b.valid || b.phase != breadcrumb::Phase::Running) { return false; }
    // A modal window loop (the window procedure says so), or the shell-open
    // bracket, which the frame timer knows as the scope user-wait.
    if ((b.activity & breadcrumb::kUserPaced) != 0) { return true; }
    return b.frameScope == static_cast<std::uint32_t>(FrameScope::UserWait) + 1u;
}

bool isCrashLikeExitCode(unsigned long code) {
    // STATUS_CONTROL_C_EXIT (0xC000013A, "The application terminated as a result
    // of a CTRL+C") is a request to end, not a fault.
    if (code == 0xC000013Aul) { return false; }
    const unsigned long nibble = (code >> 28) & 0xFul;
    return nibble == 0x8ul || nibble == 0xCul || nibble == 0xEul;
}

std::string sentinelExitWords(bool exitKnown, unsigned long code) {
    if (!exitKnown) { return "exit status not available on this platform"; }
    // Each row is a value from the Windows SDK's ntstatus.h with its message, or a
    // value a forced kill was MEASURED to leave: `taskkill /F` gives 1, and
    // Stop-Process and .NET's Process.Kill() give -1 (docs/DIAGNOSTICS.md, "The
    // sentinel"). 0xC0000409 is what __fastfail raises (Microsoft's __fastfail
    // page) and so what abort() and a failed stack-cookie check end in.
    switch (code) {
        case 0xC0000005ul: return "access violation";
        case 0xC0000409ul: return "fast-fail (abort or failed integrity check)";
        case 0xC0000374ul: return "heap corruption";
        case 0xC00000FDul: return "stack overflow";
        case 0xC000013Aul: return "Ctrl+C or a console close (STATUS_CONTROL_C_EXIT)";
        case 0x00000001ul: return "ended by another process (exit code 1, as taskkill /F does)";
        case 0xFFFFFFFFul:
            return "ended by another process (exit code -1, as Stop-Process and Process.Kill do)";
        case 0x00000000ul: return "exit code 0 before the shutdown had finished";
        default: break;
    }
    return "unknown exit code";
}

// ---------------------------------------------------------------------------
// The decision
// ---------------------------------------------------------------------------
std::uint64_t sentinelFreezeThresholdMs(const breadcrumb::Snapshot& b) {
    if (b.phase >= breadcrumb::Phase::ShutdownBegun) { return HangWatchdog::kShutdownThresholdMs; }
    if (b.frames < HangWatchdog::kStartupFrames) { return HangWatchdog::kStartupThresholdMs; }
    return HangWatchdog::kDefaultThresholdMs;
}

SentinelVerdict decideSentinel(const SentinelFacts& f) {
    using breadcrumb::Phase;
    SentinelVerdict v;
    const breadcrumb::Snapshot& b = f.crumb;
    v.phase = sentinelPhaseLabel(b);
    v.codeKnown = f.exitKnown;
    v.code = f.exitKnown ? f.exitCode : 0ul;
    if (b.valid) {
        // Silent since the LATER of the last heartbeat and the last change of
        // phase: a teardown has no heartbeats, and what it has instead is the
        // moment it entered its step.
        const std::uint64_t since = std::max({b.beatMs, b.phaseMs, b.startedMs});
        v.silentMs = f.nowMs >= since ? static_cast<std::int64_t>(f.nowMs - since) : 0;
    }

    // OFF MEANS OFF, whatever else is true.
    if (b.valid && (b.flags & breadcrumb::kFlagReportsOff) != 0) { return v; }
    // ONE REPORT PER DEATH: the in-process handler got there first.
    if (f.crashReportExists) { return v; }

    // A CLEAN EXIT: the shutdown reached its last stage (the clean-exit marker has
    // been written, which is the same fact the unclean-exit counter reads) and,
    // where the platform says, the exit code is 0. A death after the marker still
    // counts as clean, as it always has (docs/DIAGNOSTICS.md, "Where the
    // clean-exit marker is written").
    const bool shutdownDone = b.valid && b.phase >= Phase::ShutdownClosingWindow;
    if (f.exitKnown ? (f.exitCode == 0 && (shutdownDone || !b.valid)) : shutdownDone) { return v; }

    SentinelClass c = SentinelClass::Outside;
    if ((b.valid && (b.flags & breadcrumb::kFlagSessionEnding) != 0) || f.osSessionEnding) {
        c = SentinelClass::Session;
    } else if (b.valid && b.phase < Phase::Running) {
        c = SentinelClass::Startup;
    } else if (f.exitKnown && (isCrashLikeExitCode(f.exitCode) || f.exitCode == 0)) {
        c = SentinelClass::Crash;
    } else if (b.valid && v.silentMs >= 0 &&
               static_cast<std::uint64_t>(v.silentMs) > sentinelFreezeThresholdMs(b) &&
               // SILENT BECAUSE SOMEBODY WAS HOLDING IT IS NOT FROZEN. A window
               // being dragged, an open menu, a prompt being read: the frame loop
               // is stopped for as long as the person likes and the freeze
               // watchdog excuses it for exactly that reason (its rule 2 and its
               // user-paced pause). Ended from outside in that state, the
               // application was healthy, and the report stays on the machine.
               !sentinelUserPaced(b)) {
        c = SentinelClass::Frozen;
    }
    // A freeze the watchdog already filed, and the process then ended without
    // recovering: that freeze has its report, with every thread's stack.
    if (f.freezeReportExists && (c == SentinelClass::Startup || c == SentinelClass::Frozen ||
                                 c == SentinelClass::Outside)) {
        return v;
    }

    v.cls = c;
    std::string reason = sentinelClassReason(c);
    reason += " - " + sentinelExitWords(f.exitKnown, f.exitCode);
    reason += std::string("; phase ") + v.phase.words;
    // WHICH PART OF THE FRAME, from the frame timer's closed list of names
    // (core/frame_timing.hpp): the nearest thing to a stack this report can have.
    const char* scope = sentinelFrameScopeName(b);
    if (scope != nullptr) { reason += std::string("; in ") + scope; }
    if (v.silentMs >= 0) { reason += "; silent " + std::to_string(v.silentMs / 1000) + " s"; }
    if (reason.size() > 200) { reason.resize(200); }  // what the site keeps of a reason
    v.reason = std::move(reason);
    v.signatureTag = std::string("sentinel:") + sentinelClassId(c) + ":" + v.phase.id;
    // Two freezes in different parts of the frame are different bugs, and so are
    // two crashes: the scope is part of what groups them.
    if (scope != nullptr) { v.signatureTag += std::string(":") + scope; }
    return v;
}

// ---------------------------------------------------------------------------
// The report
// ---------------------------------------------------------------------------
bool sentinelWantsOsCrashRecord(const SentinelVerdict& v) {
    return (v.cls == SentinelClass::Crash || v.cls == SentinelClass::Startup) && v.codeKnown &&
           isCrashLikeExitCode(v.code);
}

std::string sentinelSignature(const SentinelVerdict& v, const OsCrashLocation* location) {
    char sig[17] = {};
    const unsigned long code = v.codeKnown ? v.code : 0ul;
    if (location != nullptr && !location->module.empty()) {
        // THE LOCATION REFINES THE TAG, it does not replace it: the same hash input as ever
        // (`<tag>`), joined to the module with an '@' so that no module name can make one
        // tag read as another, and the offset where the hash has always taken it. A report
        // without a location hashes exactly as it did in 0.99.64 and 0.99.65.
        const std::string hashed = v.signatureTag + "@" + location->module;
        crashSignatureRaw(code, hashed.c_str(), static_cast<std::uintptr_t>(location->offset), sig);
    } else {
        crashSignatureRaw(code, v.signatureTag.c_str(), 0, sig);
    }
    return std::string(sig);
}

std::string renderSentinelReport(const SentinelVerdict& v, const SentinelReportInfo& info) {
    // A location is attached only if what it names is a plain file name: whatever filled
    // it in, nothing else can reach the report.
    const bool located = info.located && osCrashModuleNameOk(info.location.module);
    const std::string sig = sentinelSignature(v, located ? &info.location : nullptr);
    char code[16] = {};
    if (v.codeKnown) { std::snprintf(code, sizeof(code), "0x%08lX", v.code); }

    std::string out;
    out.reserve(2048 + info.logLines.size() * 96);
    out += "kind: crash\n";
    out += "reason: " + v.reason + "\n";
    out += std::string("code: ") + (v.codeKnown ? code : "unknown") + "\n";
    if (located) {
        // WHERE WINDOWS SAYS IT ENDED (0.99.66): the line the in-process reports carry,
        // which the uploader reads into the payload's `module` and `offset`, and one line
        // saying it is not a stack frame. Hex digits are unpadded and upper case, as the
        // in-process writer renders them.
        char off[24] = {};
        std::snprintf(off, sizeof(off), "%llX", static_cast<unsigned long long>(info.location.offset));
        out += "address: " + info.location.module + "+0x" + off + "\n";
        out += std::string("address-source: ") + kSentinelAddressSource + "\n";
    }
    out += "signature: " + sig + "\n";
    out += "--- context ---\n";
    out += "version: " + info.version + "\n";
    out += "commit: " + info.commit + "\n";
    out += "os: " + info.os + "\n";
    out += "arch: " + info.arch + "\n";
    out += "receiver: not known to the sentinel\n";
    if (located && info.hasModule) {
        // THE APPLICATION'S OWN EXECUTABLE, in the crash writer's own line format
        // (crash_handler.cpp, writeModules - one parser reads both), so that the build id
        // is there for the offset above. The file name is the validated one from the
        // event, never a path; the PDB's is a plain file name or `(none)`, and the build
        // id is hex digits or `(none)`, whatever filled the info in.
        auto hexOnly = [](const std::string& s) {
            if (s.empty() || s.size() > 47) { return false; }
            for (const char c : s) {
                const bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
                if (!hex) { return false; }
            }
            return true;
        };
        char nums[64] = {};
        std::snprintf(nums, sizeof(nums), " base=0x%016llX size=0x%llX",
                      static_cast<unsigned long long>(info.moduleBase),
                      static_cast<unsigned long long>(info.moduleSize));
        out += "--- modules ---\n";
        out += "  " + info.location.module + nums + " pdb=" +
               (osCrashModuleNameOk(info.modulePdb) ? info.modulePdb : std::string("(none)")) +
               " build=" + (hexOnly(info.moduleBuildId) ? info.moduleBuildId : std::string("(none)")) + "\n";
    }
    out += "--- process ---\n";
    if (info.uptimeSec >= 0) { out += "uptime-sec: " + std::to_string(info.uptimeSec) + "\n"; }
    out += "fault-thread-own: unknown\n";
    out += "--- log (last " + std::to_string(info.logLines.size()) + " of " +
           std::to_string(info.logTotalLines) + " lines) ---\n";
    for (const std::string& line : info.logLines) {
        out += line;
        out += "\n";
    }
    return out;
}

const std::vector<std::string>& sentinelHeaderFieldNames() {
    static const std::vector<std::string> names = {"kind", "reason", "code", "signature"};
    return names;
}

const std::vector<std::string>& sentinelLocationFieldNames() {
    static const std::vector<std::string> names = {"address", "address-source"};
    return names;
}

const std::vector<std::string>& sentinelModuleBlockFieldNames() {
    static const std::vector<std::string> names = {"modules", "base", "size", "pdb", "build"};
    return names;
}

const std::vector<std::string>& sentinelContextFieldNames() {
    static const std::vector<std::string> names = {"version", "commit", "os", "arch", "receiver"};
    return names;
}

const std::vector<std::string>& sentinelProcessFieldNames() {
    static const std::vector<std::string> names = {"uptime-sec", "fault-thread-own"};
    return names;
}

std::string sentinelReportFileName(unsigned long appPid,
                                   std::chrono::system_clock::time_point at) {
    std::string stamp;
#if defined(_WIN32)
    const std::time_t t = std::chrono::system_clock::to_time_t(at);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d", tmv.tm_year + 1900,
                  tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    stamp = buf;
#else
    // The Linux handler's own naming: epoch seconds, no calendar breakdown.
    stamp = std::to_string(static_cast<long long>(std::chrono::system_clock::to_time_t(at)));
#endif
    return "crash-" + stamp + "-" + std::to_string(appPid) + "-" +
           std::to_string(kSentinelReportSeq) + ".txt";
}

// ---------------------------------------------------------------------------
// Looking for a freeze report
// ---------------------------------------------------------------------------
namespace {

// The pid field of "hang-<pid>-<n>.txt" (both writers: hang_watchdog.cpp).
bool hangNamePid(const std::string& name, unsigned long long& pid) {
    static const std::string kHead = "hang-";
    static const std::string kTail = ".txt";
    if (name.size() <= kHead.size() + kTail.size() || name.compare(0, kHead.size(), kHead) != 0 ||
        name.compare(name.size() - kTail.size(), kTail.size(), kTail) != 0) {
        return false;
    }
    const std::string stem = name.substr(kHead.size(), name.size() - kHead.size() - kTail.size());
    const std::size_t dash = stem.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= stem.size()) { return false; }
    for (std::size_t i = 0; i < stem.size(); ++i) {
        if (i == dash) { continue; }
        if (stem[i] < '0' || stem[i] > '9') { return false; }
    }
    pid = std::strtoull(stem.substr(0, dash).c_str(), nullptr, 10);
    return true;
}

}  // namespace

bool freezeReportWrittenSince(const std::string& crashDir, unsigned long appPid,
                              std::chrono::system_clock::time_point since) {
    if (crashDir.empty()) { return false; }
    std::error_code ec;
    const auto fileNow = fs::file_time_type::clock::now();
    const auto sysNow = std::chrono::system_clock::now();
#if defined(_WIN32)
    const unsigned long long wantPid = appPid;
#else
    const unsigned long long wantPid = 0;  // hang_watchdog.cpp names a Linux report with pid 0
    (void)appPid;
#endif
    fs::directory_iterator it(fs::path(crashDir), ec);
    const fs::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        unsigned long long namePid = 0;
        if (!hangNamePid(it->path().filename().string(), namePid) || namePid != wantPid) { continue; }
        std::error_code tec;
        const auto written = it->last_write_time(tec);
        if (tec) { continue; }
        const auto writtenSys =
            sysNow + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                         written - fileNow);
        if (writtenSys < since) { continue; }
        std::ifstream in(it->path(), std::ios::binary);
        if (!in) { continue; }
        char buf[512];
        in.read(buf, sizeof(buf));
        const std::string head(buf, static_cast<std::size_t>(in.gcount()));
        // `kind: hang` or `kind: stall` is the first line either writer emits.
        if (head.rfind("kind: hang", 0) == 0 || head.rfind("kind: stall", 0) == 0) { return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
// The watcher's last act
// ---------------------------------------------------------------------------
void armSentinelExitDeadline() {
    static std::atomic<bool> armed{false};
    if (armed.exchange(true)) { return; }
    try {
        std::thread([] {
            std::this_thread::sleep_for(kSentinelExitDeadline);
#if defined(_WIN32)
            ::TerminateProcess(::GetCurrentProcess(), 6);
#else
            ::_exit(6);
#endif
        }).detach();
    } catch (...) {
        // No thread: the work below is small and bounded, and the process still ends.
    }
}

namespace {

// Two file names that differ at most in case (Windows file names do not).
bool sameFileNameNoCase(const char* a, const std::string& b) {
    const std::size_t n = std::strlen(a);
    if (n != b.size()) { return false; }
    for (std::size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// A file written whole: opened for truncation, written, flushed, and checked.
bool writeWholeFile(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) { return false; }
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.flush();
    return static_cast<bool>(file);
}

}  // namespace

SentinelOutcome finishSentinelWatch(const SentinelEnd& e) {
    SentinelOutcome out;

    const std::uint64_t nowMs = breadcrumb::nowMs();
    const auto sysNow = std::chrono::system_clock::now();

    SentinelFacts f;
    f.exitKnown = e.exitKnown;
    f.exitCode = e.exitCode;
    f.crumb = e.crumb;
    f.nowMs = nowMs;
    f.osSessionEnding = e.osSessionEnding;

    // When the application began, for the questions "was that report written
    // since" - a process id is reused and the folder keeps reports for weeks.
    auto began = e.appStartedKnown ? e.appStarted : sysNow - std::chrono::hours(1);
    if (!e.appStartedKnown && e.crumb.valid && nowMs >= e.crumb.startedMs) {
        began = sysNow - std::chrono::milliseconds(nowMs - e.crumb.startedMs);
    }
    f.crashReportExists = crashReportWrittenByProcess(e.crashDir, e.appPid, began);

    // A freeze report counts only if it was written after the last sign of life:
    // a report of a freeze the application recovered from is not this one.
    auto silentSince = began;
    if (e.crumb.valid) {
        const std::uint64_t since = std::max({e.crumb.beatMs, e.crumb.phaseMs, e.crumb.startedMs});
        if (nowMs >= since) {
            silentSince = sysNow - std::chrono::milliseconds(nowMs - since);
        }
    }
    // Two seconds of slack for a file system with coarse timestamps (FAT keeps two).
    f.freezeReportExists =
        freezeReportWrittenSince(e.crashDir, e.appPid, silentSince - std::chrono::seconds(2));

    out.verdict = decideSentinel(f);
    if (!out.verdict.write()) { return out; }

    SentinelReportInfo info;
    info.version = versionString();
    info.commit = gitCommit();
    info.os = osDescription();
    info.arch = archDescription();
    info.uptimeSec = e.uptimeSec;
    if (info.uptimeSec < 0 && e.crumb.valid && nowMs >= e.crumb.startedMs) {
        info.uptimeSec = static_cast<std::int64_t>((nowMs - e.crumb.startedMs) / 1000);
    }
    const SessionLogTail tail = readNewestSessionLogTail(e.logDir, DiagLog::kRingLines);
    info.logLines = tail.lines;
    info.logTotalLines = tail.sessionLines;

    // WRITTEN INTO THE FOLDER THE APPLICATION ARMED, and never created here: a
    // folder that is not there means the application's own capture is off, and
    // the sentinel writes nowhere the application would not.
    std::error_code ec;
    if (e.crashDir.empty() || !fs::is_directory(fs::path(e.crashDir), ec)) { return out; }

    // THE REPORT IS WRITTEN FIRST, exactly as it was before 0.99.66, and only then is
    // Windows' crash record waited for (below): a read that stalls, or a disk that is slow
    // after it, can cost the location but never the report.
    const fs::path path = fs::path(e.crashDir) / sentinelReportFileName(e.appPid, sysNow);
    if (!writeWholeFile(path, renderSentinelReport(out.verdict, info))) { return out; }
    out.reportPath = path.string();

    // WHERE WINDOWS SAYS IT ENDED (0.99.66), only for the endings that carry a crash code
    // and are written for sending, and only when the application's start time is known
    // (the event is matched by process id AND creation time). Windows Error Reporting stamps
    // its event before the process object is signalled, but the log does not always show it
    // yet when this watcher wakes (0 to about 750 ms later, measured), so the lookup asks
    // again until it does or kSentinelOsRecordBudget is gone. If it is found, the report is
    // REPLACED by one that carries it - written beside the report and renamed over it, so
    // that the deadline cannot leave half of either. Elsewhere, nothing is found.
    if (sentinelWantsOsCrashRecord(out.verdict) && e.appStartFileTime != 0) {
        OsCrashQuery q;
        q.pid = e.appPid;
        q.startFileTime = e.appStartFileTime;
        q.endFileTime = e.appEndFileTime;
        q.exitCode = e.exitCode;
        const std::chrono::milliseconds budget =
            e.osRecordBudget.count() >= 0 ? e.osRecordBudget : kSentinelOsRecordBudget;
        const OsCrashLookup found = findOsCrashRecord(q, budget, e.osEventSource, kOsCrashRetryInterval);
        if (found.found) {
            info.located = true;
            info.location = found.location;
            // A location in OUR OWN executable gets its module-table line, because the
            // sentinel is a copy of the same file and so its build id is the application's.
            // A fault in anything else (a vendor DLL, the C runtime) gets none: its build
            // id is not ours to name and nothing here could resolve it. A report with no
            // location has no module block either, so it stays what 0.99.65 wrote.
            DiagModule self;
            if (describeMainModule(self) && sameFileNameNoCase(self.name, info.location.module)) {
                info.hasModule = true;
                info.moduleBase = static_cast<std::uint64_t>(self.base);
                info.moduleSize = static_cast<std::uint64_t>(self.size);
                info.modulePdb = self.pdb;
                info.moduleBuildId = self.buildId;
            }
            fs::path beside = path;
            beside += ".tmp";  // not "*.txt": the uploader and the folder list never see it
            if (writeWholeFile(beside, renderSentinelReport(out.verdict, info))) {
                std::error_code rec;
                fs::rename(beside, path, rec);  // replaces the first report in one step
                if (rec) {
                    std::error_code dec;
                    fs::remove(beside, dec);
                } else {
                    out.located = true;
                }
            } else {
                std::error_code dec;
                fs::remove(beside, dec);
            }
        }
    }
    return out;
}

}  // namespace cascade::core
