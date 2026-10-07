// exit_cause.hpp - HOW the previous session ended, when it ended without saying
// so (0.99.69). The one number the usage report always carried, `crashes`
// ("unclean exits since install"), counts every session that never wrote its
// clean-exit marker, and that is four different things: a real death, a kill from
// outside, an ending the operating system asked for, and an ending nothing can
// explain. The reliability page on the website could only show the sum, so it could
// not tell a version that crashes from a person who ends the task. This file is the
// rule that sorts an unclean exit into exactly one of four classes, and the four
// lifetime counters that sit beside `crashes`.
//
// THE FOUR CLASSES, and the evidence each one is read from. The evidence is what
// the diagnostics already write into the reports folder - nothing new is recorded
// about a person, and with Diagnostics off nothing is read at all:
//
//   died     A FAULT. The process's own crash handler wrote a report of its death
//            (`kind: crash` whose reason is not a survived fault), or the sentinel
//            (core/sentinel.hpp) did and filed it as `crash` (a crash exit code the
//            handler never saw), `frozen` (the window had stopped drawing, then was
//            ended: the application was at fault, whoever pressed End task), or
//            `startup` with a CRASH exit code. Failing those, a freeze report
//            (`kind: hang`) with nothing after it. The faulting module, where
//            Windows named one, is already in the report (`address:`), so Windows'
//            own "Application Error" record is read THROUGH the sentinel's report;
//            this file never asks the event log itself.
//   killed   ENDED FROM OUTSIDE, with the application healthy: the sentinel's
//            `outside` class (a TerminateProcess - taskkill, Task Manager, Stop-
//            Process, an installer - or STATUS_CONTROL_C_EXIT, which since 0.99.67
//            means the shutdown a closing console started did not finish in the time
//            the system allows), or `startup` with a code that is not a crash code
//            (an ending before the first frame by taskkill is a kill, not a death).
//            Failing those, a display-stall report (`kind: stall`) with nothing after
//            it: the display driver was waiting and the person ended the window.
//   ended    THE OPERATING SYSTEM CLOSED THE SESSION: the sentinel's `session`
//            class (WM_ENDSESSION, or the system said it was shutting down). It is a
//            class of its own and not a kind of kill because nothing was killed in
//            the sense a person or a program does it: the sentinel decides it ahead
//            of every other class BY THE SESSION'S STATE, not by an exit code, and
//            docs/DIAGNOSTICS.md records that no document or measurement says what
//            code a logoff leaves. Kept apart, a reliability reader can add it to
//            "killed" if that is the question; merged, it could not be taken out.
//   unknown  NO EVIDENCE. Diagnostics were off (no sentinel ran, no handler wrote
//            anything, and this file reads nothing then); the machine lost power; the
//            previous run was an older build that did not record when it began (so
//            its reports cannot be told from older ones); the sentinel was ended
//            first or wrote nothing; or a start-up that followed within a second of
//            the death beat the sentinel's report to the disk. Also what an ending is
//            before the evidence has been read (see ExitCounts).
//
// EVERY UNCLEAN EXIT LANDS IN EXACTLY ONE CLASS, and the sum of the four always
// equals the number of unclean exits counted from this version on: the counters
// move together or not at all (ExitCounts). They are NOT back-filled - an install's
// older `crashes` stay in `crashes` only, so `crashes - (died + killed + ended +
// unknown)` is how many unclean exits came before the classes existed.
//
// WHICH REPORT BELONGS TO THE PREVIOUS SESSION. Reports accumulate in one folder
// for weeks. The previous session's are those written between its start and this
// session's start - the start is persisted beside the counters
// (AppConfig::telemetrySessionStarted), and a run whose predecessor wrote none (an
// older build) is `unknown` rather than guessed at. The ENDING of a session is the
// newest `kind: crash` report of that window that is not the report of a fault the
// process survived (an absorbed vendor fault, an enumeration child's death: those
// are written by sessions that carried on, and say nothing of how this one ended).
// With none, the newest freeze report decides, because the sentinel files no
// report of its own for a session whose freeze the watchdog had already reported
// (sentinel.cpp, `freezeReportExists`); a freeze that recovered and was followed by
// any other ending has that ending's own sentinel report, which is looked at first.
//
// WHAT IT CANNOT KNOW, said here so nobody reads more into the numbers: a freeze
// report from a session that recovered, whose later ending left no report (the
// sentinel off or ended first), reads as `died`; two copies running at once share
// the folder and the config, so their endings can be sorted into each other's
// counts; and the classification is only as good as the sentinel's own rule, which
// docs/DIAGNOSTICS.md ("The sentinel") sets out and tests.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_EXIT_CAUSE_HPP
#define CASCADE_CORE_EXIT_CAUSE_HPP

#include <cstdint>
#include <string>

#include "core/diag_history.hpp"

namespace cascade::core {

enum class ExitClass { Died, Killed, Ended, Unknown };

// "died", "killed", "ended", "unknown": the words of the log line and of the
// counters' names (`exits_died` ...).
const char* exitClassId(ExitClass c);

// The four lifetime counters. NEVER RESET, like `crashes`, and kept in the same
// store (AppConfig).
//
// THE INVARIANT HOLDS AT EVERY INSTANT, not only once the evidence is in: an
// unclean exit found at start-up is counted as `unknown` AT ONCE - the same moment
// `crashes` goes up, so a session that is itself ended a second later has left both
// numbers moved together - and moved to its real class by refine() when the
// evidence has been read (off the thread that draws the window, a moment later).
// refine() takes one out of `unknown` and puts it in the class, so the total never
// changes and a class can never be counted without an unclean exit behind it.
struct ExitCounts {
    std::uint64_t died = 0;
    std::uint64_t killed = 0;
    std::uint64_t ended = 0;
    std::uint64_t unknown = 0;

    std::uint64_t total() const { return died + killed + ended + unknown; }
    std::uint64_t of(ExitClass c) const;

    // An unclean exit has just been found, cause not yet read: one more `unknown`.
    void noteUnclean() { ++unknown; }

    // The evidence for that exit has been read and says `cls`: moves one from
    // `unknown` to `cls`. False, and nothing moves, for `Unknown` (it is already
    // there) or when `unknown` is zero (a hand-edited file, or a second refine).
    bool refine(ExitClass cls);
};

// What was decided, and in words the log line carries. `detail` is built from
// closed vocabularies only (the sentinel's exit-code words, fixed sentences): never
// text from a report beyond what those tables return.
struct ExitVerdict {
    ExitClass cls = ExitClass::Unknown;
    std::string detail;
};

// Does this report's `reason:` line describe a fault the writing process
// SURVIVED - an absorbed vendor fault, the parent's report of an enumeration
// child's death, a contained child's own report? Such a report is evidence about
// that session's life, never about its ending. The website's
// reliabilityReasonSurvived (reliability.go) is the same rule over the same
// wording; the two are separate because they are separate repositories.
bool reportReasonIsSurvivedFault(const std::string& reason);

// THE RULE, as a pure function of the report listing (core/diag_history.hpp) and
// two times: `prevStartEpoch`, when the previous session began (0 = not recorded),
// and `thisStartEpoch`, when this one did. A report counts when it was written at or
// after `prevStartEpoch` and not after `thisStartEpoch` (plus a few seconds' slack
// for a clock that moved: this session cannot have written one that early, and the
// slack costs nothing).
ExitVerdict classifyPreviousExit(const ReportListing& reports, std::int64_t prevStartEpoch,
                                 std::int64_t thisStartEpoch);

// The verdict for a run that has nothing to read: diagnostics off, or no
// reports folder to read from. `why` is the sentence for the log line.
ExitVerdict unknownExit(const std::string& why);

// "previous session ended: killed (ended from outside while drawing: ...)": the
// ONE line written to the diagnostic log when the cause is known.
std::string previousExitLogLine(const ExitVerdict& v);

}  // namespace cascade::core

#endif  // CASCADE_CORE_EXIT_CAUSE_HPP
