// pasted_report.hpp - recognising a crash, freeze or sentinel report that somebody
// pasted into the REPORT A BUG / DISLIKE box (0.99.69).
//
// THE FIELD CASE. A tester on 0.99.66 saw `upload local-only` in the diagnostics
// bundle, had no idea what it meant, and pasted the whole sentinel report by hand
// into the bug form's text box. It arrived as eleven kilobytes of free text; the
// admin page showed it as prose, and nothing said it was a report "ended from
// outside" - a class the application writes and never sends. Recognising the text
// for what it is lets the site file it as a report with its class and the version
// that wrote it, and lets the person see, before they press SEND, that it was
// understood.
//
// WHAT THIS DOES NOT DO. It never reads a file and never adds anything the person
// did not already type or paste: the recognised text is a slice of the text they
// are already sending. It changes how that slice travels (a structured attachment
// beside the message, core/problem_report.hpp) and nothing about WHAT travels.
//
// HOW A REPORT IS RECOGNISED, and why it is strict. A report has a fixed shape
// (core/crash_upload.cpp parseReportText reads it, tests/test_crash_capture.cpp and
// tests/test_diag_hang.cpp hold the writers to it): header lines of `name: value`
// beginning with `kind: crash|hang|stall`, a `signature:` line, then a
// `--- context ---` block whose `version:` line names the build. Prose that merely
// says "sentinel", or quotes a `kind: crash` line, is not a report: ALL of the
// header's kind line, its signature, the context marker and the context's version
// must be there, in that order, or nothing is recognised. A report with Windows
// line endings is the same report.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PASTED_REPORT_HPP
#define CASCADE_CORE_PASTED_REPORT_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace cascade::core {

// The largest recognised text that is attached, in bytes: the same 65536 the
// diagnostics log is held to, which the site applies to an attachment by
// reference. A report is at most about 130 KiB on disk (core/diag_history.hpp reads
// no more of one); a longer paste is cut at a line, with one line saying so - the
// whole of it is still in the message text the person sent.
inline constexpr std::size_t kPastedReportMaxBytes = 65536;

struct PastedReport {
    // What it is, from a CLOSED list (pastedReportClassIds()): "crash", "hang",
    // "stall", or "sentinel:<class>" for a crash-kind report whose `reason:` line
    // begins with one of the five fixed sentinel sentences (core/sentinel.hpp).
    // `class` on the wire.
    std::string reportClass;
    // The `version:` line of the context block - the build that WROTE the report,
    // which is not necessarily the one the person is running now. Empty when it is
    // not made of version characters.
    std::string version;
    // The report, from its `kind:` line to the end of what was pasted, with LF line
    // endings, cut at kPastedReportMaxBytes.
    std::string text;
};

// The first report in `pasted`, or nothing. Pure: no file, no clock, no state.
std::optional<PastedReport> detectPastedReport(const std::string& pasted);

// Every class detectPastedReport can answer, and the site accepts: the wire
// vocabulary. tests/test_pasted_report.cpp holds it to the sentinel's own classes.
const std::vector<std::string>& pastedReportClassIds();

}  // namespace cascade::core

#endif  // CASCADE_CORE_PASTED_REPORT_HPP
