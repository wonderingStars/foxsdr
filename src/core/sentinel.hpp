// sentinel.hpp - a small watcher that notices when the application ends
// abnormally WITHOUT having been able to report it, and writes the one report
// that can be known (0.99.64). The design record is docs/DIAGNOSTICS.md, "The
// sentinel"; this header is the contract.
//
// WHAT IT IS. The SAME EXECUTABLE, started by the application itself with
// `--sentinel`, which does nothing but wait for the application to end. It is
// not a second binary (the installer ships one), it opens no window, no console,
// no GL and no radio, and it reads and writes nothing of the user's
// configuration. While it waits it uses no CPU: it blocks on a handle.
//
// WHAT IT CAN SEE, and the rule that bounds it. It reads no memory of the
// application and takes no dump: no debugger, no ReadProcessMemory, no minidump.
// It has (1) a handle that lets it WAIT for the process and read its exit code
// (SYNCHRONIZE and PROCESS_QUERY_LIMITED_INFORMATION - nothing more), (2) the
// breadcrumb block the application writes into deliberately
// (core/breadcrumb.hpp) and (3) files the application already writes: the log
// and the report folder. On Linux a watcher that is not the parent cannot learn
// an exit status, so it gets (1) as a pipe whose write end the application holds:
// end-of-file means the application has gone, and nothing more.
//
// WHAT IT DECIDES. When the application ends it answers one question - is there
// anything the application could not itself report - and writes ONE report in
// the existing format and folder, or nothing:
//
//   a whole report of this death by the in-process handler  -> nothing
//   exit code 0 after the shutdown reached its last stage   -> nothing (clean)
//   the session was closing                                 -> Session  (kept here)
//   it ended before the first frame                         -> Startup  (sent)
//   a crash exit code the handler never saw                 -> Crash    (sent)
//   it had stopped drawing for longer than the watchdog's
//   own threshold, then ended - and nobody was holding the
//   window (a drag, a menu, a prompt)                       -> Frozen   (sent)
//   anything else: ended while it was drawing               -> Outside  (kept here)
//
// "SENT" means the next launch's uploader may send it, under the same switch and
// the same limits (one signature a day, five a day) as every other report;
// "KEPT HERE" means the uploader never does, and the report is still in the
// folder and in the bundle's "reports on this machine" list. A healthy
// application that the user ends from Task Manager must not use up the daily
// five, which is the whole reason there are two kinds.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SENTINEL_HPP
#define CASCADE_CORE_SENTINEL_HPP

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "core/breadcrumb.hpp"

namespace cascade::core {

// ---------------------------------------------------------------------------
// The classes, and the fixed wording of each reason line.
// ---------------------------------------------------------------------------
enum class SentinelClass { None, Crash, Frozen, Startup, Outside, Session };

// The START of every sentinel report's `reason:` line. FIXED: each class has one
// sentence, so the site can classify by prefix and nothing else varies. What
// follows the sentence is a closed vocabulary (` - <what the exit code means>;
// phase <phase>; in <part of the frame>; silent <N> s`), never text from the
// machine. At most 200
// characters in all, which is what the site keeps of a reason.
inline constexpr const char* kSentinelReasonCrash =
    "sentinel: crash exit code, no report from the process";
inline constexpr const char* kSentinelReasonFrozen =
    "sentinel: window had stopped drawing when it ended";
inline constexpr const char* kSentinelReasonStartup = "sentinel: ended before the first frame";
inline constexpr const char* kSentinelReasonOutside =
    "sentinel: ended from outside, window was drawing";
inline constexpr const char* kSentinelReasonSession = "sentinel: ended as the session closed";

// What every sentinel reason begins with, and the two classes that are NEVER
// uploaded. Shared with the uploader (crash_upload.cpp) so that the writer and
// the reader of the distinction cannot drift apart.
inline constexpr const char* kSentinelReasonPrefix = "sentinel: ";
bool sentinelReasonIsLocalOnly(const std::string& reason);
bool sentinelReasonIsSentinel(const std::string& reason);

const char* sentinelClassId(SentinelClass c);        // "crash", "frozen", "startup", ...
const char* sentinelClassReason(SentinelClass c);    // the fixed sentence above
bool sentinelClassUploads(SentinelClass c);          // Crash, Frozen, Startup

// ---------------------------------------------------------------------------
// The vocabulary: phases and exit codes, in words.
// ---------------------------------------------------------------------------
struct PhaseLabel {
    const char* id;     // "running", "opening-radio" - used in the signature
    const char* words;  // "running", "opening a radio" - used in the reason
};
// The phase as the report names it: the breadcrumb's phase, refined by what else
// was going on (a plugin load, a radio open) while the application was up.
PhaseLabel sentinelPhaseLabel(const breadcrumb::Snapshot& b);

// The part of the frame the window's thread was in (a name from
// core/frame_timing.hpp's closed list: "rail", "plugin-panels", ...), or nullptr
// when the page has none that means anything: the frame loop was not turning, no
// frame had begun, or the number is not one this build knows.
const char* sentinelFrameScopeName(const breadcrumb::Snapshot& b);

// Was a PERSON holding the frame loop - a window being dragged or resized, an
// open menu, a prompt of the operating system's being read? Then a silent
// heartbeat is not a freeze. True only while the frame loop is turning.
bool sentinelUserPaced(const breadcrumb::Snapshot& b);

// A crash EXIT CODE: a Windows exception or NTSTATUS error (top nibble 8, C or E)
// other than STATUS_CONTROL_C_EXIT, which is a request to end and not a fault.
// Anything else - 1 from taskkill, -1 from Stop-Process, 0 - is not.
bool isCrashLikeExitCode(unsigned long code);

// What an exit code means, in the fixed words the reason carries. "unknown exit
// code" for any value not in the table; the table is checked against the Windows
// SDK's ntstatus.h and, for the two codes a forced kill leaves, against a
// measurement (docs/DIAGNOSTICS.md, "The sentinel").
std::string sentinelExitWords(bool exitKnown, unsigned long code);

// ---------------------------------------------------------------------------
// The decision, a pure function of what was observed.
// ---------------------------------------------------------------------------
struct SentinelFacts {
    bool exitKnown = false;          // false on Linux: only "it has gone" is known
    unsigned long exitCode = 0;
    breadcrumb::Snapshot crumb;      // .valid false when the block could not be read
    std::uint64_t nowMs = 0;         // breadcrumb::nowMs() as the watcher woke
    bool osSessionEnding = false;    // the operating system says the session is closing
    bool crashReportExists = false;  // a whole report of this death by the in-process handler
    bool freezeReportExists = false; // a freeze report by this process, after its last heartbeat
};

struct SentinelVerdict {
    SentinelClass cls = SentinelClass::None;
    std::string reason;         // the whole `reason:` line value; empty for None
    unsigned long code = 0;     // the exit code (0 when not known)
    bool codeKnown = false;
    std::string signatureTag;   // hashed with the code: "sentinel:<class>:<phase>[:<scope>]"
    std::int64_t silentMs = -1; // how long the heartbeat had been silent; -1 unknown
    PhaseLabel phase{"unknown", "unknown"};
    bool write() const { return cls != SentinelClass::None; }
    bool upload() const { return sentinelClassUploads(cls); }
};

// How long the heartbeat may be silent before the ending is "frozen": the SAME
// thresholds the freeze watchdog judges by - its start-up budget for the first
// frames, its frame threshold afterwards, its shutdown budget once the frame loop
// has ended - read from its own constants, so the two cannot disagree.
std::uint64_t sentinelFreezeThresholdMs(const breadcrumb::Snapshot& b);

SentinelVerdict decideSentinel(const SentinelFacts& f);

// ---------------------------------------------------------------------------
// The report
// ---------------------------------------------------------------------------
struct SentinelReportInfo {
    std::string version, commit, os, arch;
    std::int64_t uptimeSec = -1;            // -1: not known, the line is left out
    std::vector<std::string> logLines;      // the end of the session's log, oldest first
    std::size_t logTotalLines = 0;          // how many lines the session had in the files
};

// The text of the report: the existing format (kind, reason, code, signature,
// context, process, log), no stack and no module list because there is nothing
// of either to show. Every header line and every context line it can carry is
// listed below.
std::string renderSentinelReport(const SentinelVerdict& v, const SentinelReportInfo& info);

// THE INVENTORY, in the spirit of crashReportFieldNames(): PRIVACY.md documents
// these field by field and tests/test_sentinel.cpp compares them with a report a
// real sentinel wrote, in both directions. The header lines before the first
// "--- " marker, the lines of the context block, and the lines of the process
// block.
const std::vector<std::string>& sentinelHeaderFieldNames();
const std::vector<std::string>& sentinelContextFieldNames();
const std::vector<std::string>& sentinelProcessFieldNames();

// "crash-<stamp>-<application pid>-999999.txt". Named with the APPLICATION's
// process id so that crashReportWrittenByProcess finds it (one report per
// death, and asking twice is answered the same), and with a sequence number no
// in-process report ever reaches, so it can never overwrite one.
inline constexpr unsigned long kSentinelReportSeq = 999999;
std::string sentinelReportFileName(unsigned long appPid, std::chrono::system_clock::time_point at);

// Is there a freeze report (hang-*.txt, kind hang or stall) by `appPid` written
// at or after `since`? Healthy path only. On Linux a freeze report is named with
// pid 0, so any freeze report since `since` counts.
bool freezeReportWrittenSince(const std::string& crashDir, unsigned long appPid,
                              std::chrono::system_clock::time_point since);

// ---------------------------------------------------------------------------
// The watcher's last act, platform-neutral.
// ---------------------------------------------------------------------------
struct SentinelEnd {
    unsigned long appPid = 0;
    bool exitKnown = false;
    unsigned long exitCode = 0;
    bool osSessionEnding = false;
    bool appStartedKnown = false;
    std::chrono::system_clock::time_point appStarted{};  // when the application began
    std::int64_t uptimeSec = -1;
    breadcrumb::Snapshot crumb;
    std::string crashDir;
    std::string logDir;
};

struct SentinelOutcome {
    SentinelVerdict verdict;
    std::string reportPath;  // empty when nothing was written
};

// Looks for an in-process report of this death, decides, and writes the one
// report or none. Pure of any process-wide effect, so a test can call it
// in-process; the exit deadline below is armed by runSentinelMain, not here.
SentinelOutcome finishSentinelWatch(const SentinelEnd& end);

// The sentinel never outlives the application by more than this, so the
// executable can be replaced by an installer: runSentinelMain arms it the moment
// the application has ended, and whatever is still going on after that - a stuck
// disk - is cut off. The cost of that is a report that is lost, never an
// installer that cannot replace cascade.exe.
inline constexpr std::chrono::milliseconds kSentinelExitDeadline{1000};
void armSentinelExitDeadline();

// ---------------------------------------------------------------------------
// The application's side: starting it, switching it, noticing it die.
// All of these are for the GUI thread.
// ---------------------------------------------------------------------------
struct SentinelOptions {
    std::string crashDir;  // where reports are, as the application armed them
    std::string logDir;    // where the log is
    // The executable to start. Empty: this one, which is what the application
    // does. A test names another (cascade.exe, from a test program).
    std::string exePath;
};

// Remembers where reports go and that this process MAY run a sentinel. Starts
// nothing: main() calls this only for an interactive session (or a bounded one a
// test asked about), and the diagnostics switch decides whether one runs.
void sentinelConfigure(const SentinelOptions& opts);

// THE DIAGNOSTICS SWITCH. On: start the sentinel if none is running (and log, once,
// if it cannot be started). Off: end it and set the breadcrumb's flag, so that
// nothing is written whatever happens next. Returns whether one is running
// afterwards. A no-op returning false before sentinelConfigure.
bool sentinelSetEnabled(bool on);

// Called now and then from the frame loop (a non-blocking handle test): says ONCE
// in the log if the watcher has ended while the application is still running.
void sentinelPoll();

bool sentinelRunning();
// The sentinel's own process id, 0 when none. For tests.
unsigned long sentinelProcessId();
// How long starting it took the application, in milliseconds, for the log and a
// test; -1 before any start.
double sentinelLastStartMs();

// `cascade --sentinel ...`: the watcher's own entry point. Returns the process
// exit code. Touches no configuration, installs no crash handler.
int runSentinelMain(int argc, char** argv);

}  // namespace cascade::core

#endif  // CASCADE_CORE_SENTINEL_HPP
