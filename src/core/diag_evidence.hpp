// diag_evidence.hpp - THE TWO LINES THAT LAND A HUMAN REPORT BESIDE THE MACHINE EVIDENCE (owner-approved 2026-10-27, stub campaign B14).
//
// A person who presses SEND on the REPORT A BUG page writes in their own words, and the words are all a human triager has to go on; the machine already holds the
// evidence - the crash and freeze reports this machine has written, each filed under a signature, and the things the program recovered from without telling anyone.
// These two header lines of the diagnostics bundle (the text "Copy diagnostics" copies and the bug page attaches) put the ids and the words in front of the person who
// reads the report, so the human report and the machine report can be laid side by side:
//
//   recent-faults: 0123456789ABCDEF stall, FEDCBA9876543210 hang       signature id and kind of the newest reports on this machine, newest first
//   recovered: audio 1, ringdrop 3                                      what the program recovered from THIS session: a fixed word and a count each
//
// THE PRIVACY LINE IS THE REPORTS' OWN: ids, kinds and counts, never content, never a path, never a time of day. A signature is the grouping hash the reports already
// carry (sixteen hex digits - anything else is dropped), a kind is one of crash / hang / stall, a recovered word is one of the closed list in core/health_events.hpp.
// The lines are in the bundle and NOT in the report file a crash or a freeze writes and NOT one of the fields the automatic crash report sends (PRIVACY.md).
//
// WHAT THEY SAY WHEN THEY CANNOT SAY: the reports are read from the reports folder by a worker (core/diag_history.hpp), so the line says "not read" when diagnostics
// are off, "still being read" until the worker has answered, and "could not be read" when the folder cannot be. The recovered words are counted by the health ledger,
// which counts only while usage reporting is on (and the Diagnostics switch is): with it off the line says "not counted" and never "none" - an absence of counting is
// not an absence of recoveries.
//
// NOT HERE: the breadcrumb trail (the last steps the program took before a fault) lives on master's field-early-warning branch; it joins this bundle after the merge.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_DIAG_EVIDENCE_HPP
#define CASCADE_CORE_DIAG_EVIDENCE_HPP

#include <string>
#include <vector>

#include "core/diag_history.hpp"
#include "core/health_events.hpp"

namespace cascade::core {

// The bundle's names for the two lines, in the order they are written: the inventory tests/test_diagnostics.cpp compares with what the bundle emits and PRIVACY.md
// documents (core::bundleFieldNames() is the builder's, core::frameBundleFieldNames() the slow-frame line's).
const std::vector<std::string>& evidenceBundleFieldNames();

// The value of `recent-faults`: "<signature> <kind>" for each report of `h.reports.newest` (newest first, as listed), joined by ", ", then ", +N more" when the folder
// holds more than were listed. A report whose file named no usable signature is "unsigned <kind>". "none" for a folder with no report. The other states are the
// parenthesised sentences described above.
std::string recentFaultsText(const DiagHistory& h);

// The value of `recovered`: this session's recoveries, "<word> <count>" in the written order of the vocabulary, joined by ", " - `all` less `prior` (the ledger holds
// earlier sessions' unsettled counts beside this session's). "none" when armed and nothing was recovered from. `armed` false (usage reporting is off: the ledger counts
// nothing) is "(not counted - usage reporting is off)"; `allowed` false (the Diagnostics switch is off) is "(not counted - diagnostics are off)".
std::string recoveredText(const health::Counts& all, const health::Counts& prior, bool armed, bool allowed);

// `bundle` with the two lines added at the end of its header block, before the log, in this order - after the lines already there (the same place
// core::withSlowFramesField puts its own). A bundle with no log marker has them appended.
std::string withEvidenceFields(std::string bundle, const std::string& recentFaults, const std::string& recovered);

}  // namespace cascade::core

#endif  // CASCADE_CORE_DIAG_EVIDENCE_HPP
