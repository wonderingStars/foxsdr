// issue_report.hpp - a report of something that went wrong for the person and that the
// program carried on from, filed so that it reaches foxsdr.com the way a crash does.
//
// WHY (owner's rule, 2026-10-28: "if a user has an issue we need to know about it,
// regardless"). The crash feed carried crashes, freezes and sentinel endings - the ways
// the program STOPS. A fault the program handles never stopped it, so it never reached
// the feed: two Intel UHD machines had the graphics driver refuse a second OpenGL window
// through October 2026, the map "vanished" session after session, every page was folded
// inside the main window as designed, and the website saw nothing. The survey of
// 2026-10-28 found twenty-five more places like it. The health-event count
// (core/health_events.hpp, `recovered.<word>`) says HOW MANY installs met a fault; it
// cannot carry the log that says WHY, and it goes with usage reporting, not with
// Diagnostics.
//
// WHAT. One report per RECOVERED-FROM WORD per session (the vocabulary is the closed list
// in health_events.hpp, so an issue is a fixed word and a fixed sentence, never free
// text), written into the same reports folder as a crash report, under the same consent
// (the Diagnostics switch: crash_handler.hpp activeCrashDir), and carried by the same
// uploader (core/crash_upload.cpp) to the same endpoint, where it is listed as a fault the
// application SURVIVED - never as a death, never in a fix's regression count, never as an
// alert of a crash. Its shape is the freeze report's minus the stacks: a header, the
// application context, the process uptime and the recent log. The log is the point: for
// the refused window it carries the GLFW line with the driver's Win32 error code.
//
// Once a session per word: the words that repeat by themselves (a ring that overflows
// overflows again) would otherwise file a report a minute, and the uploader's own limits
// (one of a signature a day, five a day) would then spend the day's budget on one word.
// The SIGNATURE is the word alone, so every install's report of one issue is one row on
// the feed, across versions, with the reports counted under it.
//
// Filed from health_events.cpp wherever a recovered-from word is counted, and from
// nowhere else: a new issue is a new word there, which the tests already hold to
// PRIVACY.md, the Worker and the survey of counting sites. Never from a fault handler:
// every caller is a healthy thread, and this allocates and takes the log ring's lock.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_ISSUE_REPORT_HPP
#define CASCADE_CORE_ISSUE_REPORT_HPP

#include <string>

#include "core/health_events.hpp"

namespace cascade::core::issue {

// The fixed sentence for a word: what happened, in the person's terms, and what the
// program did about it. One per word of health::Recovered; a word without one fails
// tests/test_issue_report.cpp.
const char* issueSentence(health::Recovered word);

// The report's `reason:` line: kIssueReasonPrefix (crash_handler.hpp), the word, " - ",
// the sentence. Fixed words and a fixed sentence - nothing from the machine.
std::string issueReason(health::Recovered word);

// The report's `signature:`: sixteen upper-case hex digits derived from the word alone,
// so one issue is one row on the feed whatever the version, machine or time.
std::string issueSignature(health::Recovered word);

// The whole report text as it would be filed now: the header (issueReportFieldNames in
// core/diag_report.hpp), the application context, the process block and the recent log.
std::string renderIssueReport(health::Recovered word);

// Files the report for `word` into activeCrashDir(), once per word per process. Returns
// the path written, or empty: capture is off (no directory, or Diagnostics off), this word
// was already filed this session, or the file could not be written. Never throws.
std::string fileIssueReport(health::Recovered word);

// Files the reports of the words met BEFORE capture was armed - at start-up, before the
// settings are read (a watcher that did not start, a display notification refused). Called
// where the Diagnostics switch is applied (main.cpp, and the Settings page); a no-op when
// nothing is pending or capture is still off.
void filePendingIssueReports();

// How many reports were written since the last call - and then none, until the next is
// written. The window's frame loop asks (AppWindow::crashUploadPoll) and sweeps the
// reports folder when the answer is not zero, so an issue report is SENT IN THE SESSION
// THAT WROTE IT (0.99.101) rather than at the next start: a person who met a fault and
// never opened FoxSDR again was never heard from. Any thread may file; one thread asks.
unsigned takeNewlyFiled();

// Forgets which words were filed or are pending, and what was newly filed, so a test can
// file the same word again.
void resetIssueReportsForTest();

}  // namespace cascade::core::issue

#endif  // CASCADE_CORE_ISSUE_REPORT_HPP
