// See exit_cause.hpp for the rule and for what it is read from. This file is the
// rule itself: a pure function of the report listing, with no clock, no disk and
// no process of its own, so a test can put every row of it in front of it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/exit_cause.hpp"

#include <cstdlib>
#include <cstring>

#include "core/crash_handler.hpp"
#include "core/sentinel.hpp"

namespace cascade::core {

namespace {

// This session cannot have written a report in its first seconds (the freeze
// watchdog's own shortest threshold is five), so anything stamped later than its
// start by more than this is its own, or a later one's. The slack is for a file
// system's clock and for a sentinel that finished the previous session's report
// a moment after the next launch began; it is the same on both ends because both
// are the file's time against a time the application read from the same clock.
constexpr std::int64_t kStartSlackSec = 5;

bool startsWith(const std::string& s, const char* prefix) {
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

// The exit code a report carries (`code: 0xC0000005`), when it carries one that fits
// the 32 bits Windows exit codes have. The reader (core/diag_history.cpp) has
// already cut the field down to "0x" and hex digits.
bool reportExitCode(const std::string& token, unsigned long& out) {
    if (token.size() < 3 || token[0] != '0' || (token[1] != 'x' && token[1] != 'X')) { return false; }
    char* end = nullptr;
    const unsigned long long v = std::strtoull(token.c_str() + 2, &end, 16);
    if (end == token.c_str() + 2 || *end != '\0' || v > 0xFFFFFFFFull) { return false; }
    out = static_cast<unsigned long>(v);
    return true;
}

// ": <what the code means>" in the sentinel's own words, or nothing when the
// report names no code (Linux: only "it has gone" is known).
std::string codeWords(const ReportSummary& r) {
    unsigned long code = 0;
    if (!reportExitCode(r.code, code)) { return std::string(); }
    return ": " + sentinelExitWords(true, code);
}

// The class of one report that ENDS a session (a crash-kind report that is not a
// survived fault), as the table in exit_cause.hpp says.
ExitVerdict fromEndingReport(const ReportSummary& r) {
    ExitVerdict v;
    if (!sentinelReasonIsSentinel(r.reason)) {
        // THE PROCESS'S OWN HANDLER wrote it, and wrote it because the process
        // was dying: a fault the handler saw. (An absorbed fault or a child's
        // death never gets here: reportReasonIsSurvivedFault filtered it.)
        v.cls = ExitClass::Died;
        v.detail = "a crash report written by the process itself";
        return v;
    }
    unsigned long code = 0;
    const bool haveCode = reportExitCode(r.code, code);
    if (startsWith(r.reason, kSentinelReasonCrash)) {
        v.cls = ExitClass::Died;
        v.detail = "crash exit code" + codeWords(r);
    } else if (startsWith(r.reason, kSentinelReasonFrozen)) {
        // The window had stopped drawing for longer than the watchdog allows and
        // nobody was holding it, and then it was ended. Whoever ended it, the
        // application was at fault: a freeze is a failure of the program.
        v.cls = ExitClass::Died;
        v.detail = "the window had stopped drawing when it ended";
    } else if (startsWith(r.reason, kSentinelReasonStartup)) {
        // "Before the first frame" says WHEN, not by what: the exit code does.
        if (haveCode && isCrashLikeExitCode(code)) {
            v.cls = ExitClass::Died;
            v.detail = "ended before the first frame" + codeWords(r);
        } else if (haveCode) {
            v.cls = ExitClass::Killed;
            v.detail = "ended before the first frame" + codeWords(r);
        } else {
            v.cls = ExitClass::Unknown;
            v.detail = "ended before the first frame, and the exit status is not available here";
        }
    } else if (startsWith(r.reason, kSentinelReasonOutside)) {
        v.cls = ExitClass::Killed;
        v.detail = "ended from outside while the window was drawing" + codeWords(r);
    } else if (startsWith(r.reason, kSentinelReasonSession)) {
        v.cls = ExitClass::Ended;
        v.detail = "the operating system was closing the session (logoff or shutdown)";
    } else {
        // A sentinel sentence this build does not know (a later build's): the
        // honest answer is that the report was not understood.
        v.cls = ExitClass::Unknown;
        v.detail = "a sentinel report this version does not recognise";
    }
    return v;
}

bool inWindow(const ReportSummary& r, std::int64_t prevStartEpoch, std::int64_t thisStartEpoch) {
    return r.writtenEpoch > 0 && r.writtenEpoch >= prevStartEpoch &&
           r.writtenEpoch <= thisStartEpoch + kStartSlackSec;
}

}  // namespace

const char* exitClassId(ExitClass c) {
    switch (c) {
        case ExitClass::Died: return "died";
        case ExitClass::Killed: return "killed";
        case ExitClass::Ended: return "ended";
        case ExitClass::Unknown: break;
    }
    return "unknown";
}

std::uint64_t ExitCounts::of(ExitClass c) const {
    switch (c) {
        case ExitClass::Died: return died;
        case ExitClass::Killed: return killed;
        case ExitClass::Ended: return ended;
        case ExitClass::Unknown: break;
    }
    return unknown;
}

bool ExitCounts::refine(ExitClass cls) {
    if (cls == ExitClass::Unknown || unknown == 0) { return false; }
    --unknown;
    switch (cls) {
        case ExitClass::Died: ++died; break;
        case ExitClass::Killed: ++killed; break;
        case ExitClass::Ended: ++ended; break;
        case ExitClass::Unknown: break;
    }
    return true;
}

bool reportReasonIsSurvivedFault(const std::string& reason) {
    if (startsWith(reason, kAbsorbedFaultReasonPrefix) ||
        startsWith(reason, kChildDeathReasonPrefix) ||
        startsWith(reason, "child process fault (contained)")) {
        return true;
    }
    // THE CHILD'S OWN REPORT (0.99.62): the fault's own words, then a clause that
    // says what the process was and that the parent survived it
    // (enumerateChildReasonSuffix, source/soapy_enum_proc.cpp). Found by the clause,
    // never by a prefix, because the words before it are the platform's.
    static const char kMarker[] = " - enumeration child, ";
    static const char kContained[] = " (contained)";
    const std::size_t at = reason.find(kMarker);
    return at != std::string::npos && at > 0 &&
           reason.find(kContained, at + sizeof(kMarker) - 1) != std::string::npos;
}

ExitVerdict unknownExit(const std::string& why) {
    ExitVerdict v;
    v.cls = ExitClass::Unknown;
    v.detail = why;
    return v;
}

ExitVerdict classifyPreviousExit(const ReportListing& reports, std::int64_t prevStartEpoch,
                                 std::int64_t thisStartEpoch) {
    if (prevStartEpoch <= 0) {
        // No start recorded: the previous run was a build that did not write one,
        // and with no start there is no telling its reports from an older run's.
        return unknownExit("the previous session's start was not recorded (an older version)");
    }
    if (!reports.readable) {
        return unknownExit("the reports folder could not be read");
    }

    // THE ENDING: the newest crash-kind report of the window that is not a survived
    // fault. Newest first is the listing's own order. A report that says nothing
    // readable (kind unknown) is skipped: it names no ending.
    const ReportSummary* freeze = nullptr;
    for (const ReportSummary& r : reports.newest) {
        if (!inWindow(r, prevStartEpoch, thisStartEpoch)) { continue; }
        if (r.kind == "crash") {
            if (reportReasonIsSurvivedFault(r.reason)) { continue; }
            return fromEndingReport(r);
        }
        if ((r.kind == "hang" || r.kind == "stall") && freeze == nullptr) { freeze = &r; }
    }
    if (freeze != nullptr) {
        // THE WATCHDOG'S REPORT AND NOTHING AFTER IT. The sentinel files no ending of
        // its own for a session whose freeze was reported and never recovered
        // (sentinel.cpp: freezeReportExists), so this is the only record there is:
        // an unrecovered hang is the program's fault, a display stall is the display
        // driver's and the person ended the window.
        ExitVerdict v;
        if (freeze->kind == "hang") {
            v.cls = ExitClass::Died;
            v.detail = "a freeze report and no recovery";
        } else {
            v.cls = ExitClass::Killed;
            v.detail = "ended while the display driver was stalled";
        }
        return v;
    }
    return unknownExit("neither the crash handler nor the sentinel left a report of it");
}

std::string previousExitLogLine(const ExitVerdict& v) {
    std::string out = std::string("previous session ended: ") + exitClassId(v.cls);
    if (!v.detail.empty()) { out += " (" + v.detail + ")"; }
    return out;
}

}  // namespace cascade::core
