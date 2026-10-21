// os_crash_record.hpp - what WINDOWS recorded about the application's own death
// (0.99.66), read by the sentinel so that its report can say WHERE.
//
// WHY. A fast-fail (a /GS stack-cookie failure, a hardened-library check, a
// third-party DLL such as a graphics or radio driver failing fast) ends the
// process with 0xC0000409 and runs nothing inside it: no handler, no stack. The
// sentinel (core/sentinel.hpp) then knows the class, the exit code, the phase and
// the part of the frame, and nothing about WHERE - the first sentinel report from
// a user arrived exactly so: "crash exit code, no report from the process -
// fast-fail; phase running; in render". Windows Error Reporting, though, records
// every application crash in the Application event log (provider "Application
// Error", event id 1000), naming the faulting module, the exception code and the
// fault offset, and the process id and creation time of the application.
//
// WINDOWS ONLY. There is no Linux equivalent asked for or built: on any other
// platform findOsCrashRecord() answers "not known" without doing anything, and the
// portable half of this file (the validation and the parser) exists on every
// platform only so that its tests do.
//
// WHAT WAS MEASURED, on Windows 11 22631, unelevated, 2026-10-05 (docs/DIAGNOSTICS.md,
// "Where in the process it ended: Windows' own crash record"):
//   - the event is written for a fast-fail (__fastfail), for a /GS failure and for
//     an access violation, in the main executable and in a DLL (the faulting module
//     is the DLL's file name in the second case);
//   - its time stamp is 214-475 ms BEFORE the process object is signalled (WerFault.exe
//     suspends the process, writes the event, queues its report and only then ends the
//     process) - but it is not always READABLE yet when a watcher wakes: asked the
//     instant the process object was signalled, 16 of 38 real deaths of a stand-in
//     showed it at once, the other 22 showed it 40-373 ms later, and other probes saw
//     it up to 752 ms later in 8 runs of about 180. So the reader asks again, every
//     kOsCrashRetryInterval, until it shows or the caller's budget is gone;
//   - it is readable by an ordinary, unelevated process;
//   - its data are NAMED (ModuleName, ExceptionCode, FaultingOffset, ProcessId,
//     ProcessCreationTime, AppPath, ...), and ProcessCreationTime is the process's
//     creation FILETIME exactly as GetProcessTimes reports it;
//   - none is written for a TerminateProcess, for a normal exit, or - the one that
//     matters for the tests - for a process that set SEM_NOGPFAULTERRORBOX.
//
// WHAT IS KEPT. Three values and nothing else: the faulting module's FILE NAME
// (never a path), the offset inside it, and (as a match condition, never stored
// apart from the exit code the report already carries) the exception code. The
// event also carries the application's full path, which can hold the user's
// account name, the module's path, versions, a report id: none of it is kept,
// none of it is sent. The parser below looks up five NAMED fields and leaves the
// rest of the text unread.
//
// WHAT IS MATCHED. The event is attached to a death only when its ProcessId AND
// its ProcessCreationTime are both this process's (a process id is reused, a
// creation time to 100 ns is not) AND its exception code is the code the process
// exited with. An event about another program, another instance or an earlier run
// is never attached. The event does offer the application's path (AppPath), which
// would allow "same executable plus a tight time window" as a fallback; that is
// deliberately NOT used, because it would mean reading the path.
//
// FAILING QUIETLY. No event, no access, a disabled or full log, a malformed event,
// a query that does not finish in time: every one of them is "not known", and the
// report is written exactly as it was before this existed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_OS_CRASH_RECORD_HPP
#define CASCADE_CORE_OS_CRASH_RECORD_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace cascade::core {

// WHICH DEATH. Everything the sentinel's process handle can say about the
// application, in the units Windows uses.
struct OsCrashQuery {
    unsigned long pid = 0;
    // The application's creation time as a FILETIME (100 ns ticks since 1601), the
    // value GetProcessTimes gives. 0: not known, and then nothing is attached.
    std::uint64_t startFileTime = 0;
    // When it ended, same unit; 0 when not known (the window then ends now).
    std::uint64_t endFileTime = 0;
    // The code the process exited with: the event's exception code must be this.
    unsigned long exitCode = 0;
};

// WHERE, as Windows recorded it. `offset` is the distance into `module`.
struct OsCrashLocation {
    std::string module;      // a plain file name, validated by osCrashModuleNameOk
    std::uint64_t offset = 0;
};

// ---------------------------------------------------------------------------
// The portable half: validation, matching, the query text.
// ---------------------------------------------------------------------------

// The longest module name kept: the same 63 characters the application's own module
// table (core/diag_report.hpp, DiagModule::name) keeps.
inline constexpr std::size_t kOsCrashModuleNameMax = 63;

// Is `s` something that can be kept as a module's file name? A plain file name, 1 to
// 63 characters, from the conservative set [A-Za-z0-9._+-], beginning with a letter,
// a digit or an underscore; never a path (no separator, no drive colon), never a
// control character, never one of the two words Windows writes when it could not
// name the module ("unknown", and "StackHash_" followed by anything).
bool osCrashModuleNameOk(const std::string& s);

// The fault offset as hex digits only (1 to 16, no prefix), below 2^32: an offset
// inside a module is bounded by the module's image size, a 32-bit field of the PE
// header, so a larger value is a raw address (Windows writes one when no module
// contains the fault) and is dropped.
bool osCrashOffsetOk(const std::string& hexDigits, std::uint64_t& offset);

// One rendered event (the XML EvtRender gives) against one death: true, with `out`
// filled, only when ALL of these hold - provider "Application Error", event id 1000,
// ProcessId == q.pid, ProcessCreationTime == q.startFileTime (both nonzero),
// ExceptionCode == q.exitCode, a valid module name and a valid offset, and no field
// named twice. Reads exactly five NAMED fields and nothing else of the event.
bool parseApplicationErrorEvent(const std::string& eventXml, const OsCrashQuery& q,
                                OsCrashLocation& out);

// The XPath of the one query this makes: this provider, this event id, and the last
// `windowMs` milliseconds. Nothing about the application is in it.
std::string osCrashEventXPath(std::uint64_t windowMs);

// How far back to ask, in milliseconds: from the application's start, but never
// more than kOsCrashWindowMaxMs before its end (a crash dialog left open for a
// minute is covered; a log that is days deep is not scanned), plus a second of slack.
inline constexpr std::uint64_t kOsCrashWindowMaxMs = 30ull * 60ull * 1000ull;
std::uint64_t osCrashWindowMs(const OsCrashQuery& q, std::uint64_t nowFileTime);

// ---------------------------------------------------------------------------
// The seam and the lookup.
// ---------------------------------------------------------------------------

// WHERE THE EVENTS COME FROM. Called with the XPath above, newest first, it hands
// each rendered event (XML, UTF-8) to `each` and stops when `each` returns true, when
// there are no more, or when `cancel` is raised. The Windows one reads the Application
// event log through the Event Log API; a test passes a fixture, or one that never
// returns. It runs on a worker thread, which is how the lookup is bounded.
using OsEventSource = std::function<void(
    const std::string& xpath, const std::function<bool(const std::string& eventXml)>& each,
    const std::atomic<bool>& cancel)>;

// The Application event log, through EvtQuery / EvtNext / EvtRender (wevtapi), as
// the current user with no privilege. Empty on any other platform.
OsEventSource windowsApplicationLogSource();

// The most events one attempt examines: the matching one is among the newest, and a
// log full of other programs' crashes must not be read to the end for it.
inline constexpr int kOsCrashMaxEventsExamined = 64;

// How long the lookup waits between attempts when the event is not there yet (see
// findOsCrashRecord): a query is about a millisecond, so this is a poll, not a load.
inline constexpr std::chrono::milliseconds kOsCrashRetryInterval{25};

struct OsCrashLookup {
    bool found = false;
    OsCrashLocation location;
    bool timedOut = false;  // the bound was reached before the answer was
    int attempts = 0;       // how many times the source was asked (for the log and the tests)
};

// Looks for the one event that is about `q`. Returns within `budget` (plus a few
// milliseconds) whatever the source does: the source runs on its own thread and a
// source that has not finished is left behind. An empty `source` means the real one.
// Every failure is "not found".
//
// RETRIES. With a `retryInterval` above zero the source is asked again, that long
// after it last finished, until the event is found or the budget is gone. That is the
// whole reason it exists: the event's own time stamp is 250-475 ms BEFORE the process
// object is signalled, but the log does not always SHOW it yet when the watcher wakes -
// measured, three runs of the same stand-in, the first and the third query found
// nothing and the second found it at once (docs/DIAGNOSTICS.md). Zero (the default)
// asks once.
OsCrashLookup findOsCrashRecord(const OsCrashQuery& q, std::chrono::milliseconds budget,
                                const OsEventSource& source = OsEventSource(),
                                std::chrono::milliseconds retryInterval = std::chrono::milliseconds(0));

}  // namespace cascade::core

#endif  // CASCADE_CORE_OS_CRASH_RECORD_HPP
