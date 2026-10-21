// Fuzz target: the crash-report feed the maintainer's reader tool consumes
// (foxsdr-reports), and the symbol tool's text output it parses.
//
// Not a surface a user meets - the reader is a developer tool - but it takes
// JSON from a network endpoint or a hand-saved export, and shapes it into the
// grouped, symbolised view a maintainer reads when deciding what to fix first.
// A reader that faults on one bad report hides the rest of the day's reports.
//
// Properties: a feed that does not parse yields no reports and an error;
// grouping an accepted feed accounts for every report (group counts sum to the
// number of reports) and renders to text and to JSON without a fault.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/report_reader.hpp"
#include "fuzz_common.hpp"

using namespace cascade::core;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::Bytes in(data, size);
    // Half the inputs go to the other parser in this file: addr2line's text.
    const bool addr2line = (in.u8() & 1) != 0;
    const std::string text = in.rest();

    if (addr2line) {
        SymbolResult r;
        (void)SymbolArchive::parseAddr2lineOutput(text, r);
        (void)isElfBuildId(text);
        return 0;
    }

    std::string error;
    int skipped = 0;
    const std::vector<ReaderReport> reports = parseReportFeed(text, error, &skipped);
    if (reports.empty()) { return 0; }
    FUZZ_REQUIRE(skipped >= 0);

    // An archive that does not exist: every module is "missing", which is the
    // state a maintainer's first run is in, and it keeps this target off the
    // disk and off the symbol tools.
    static const SymbolArchive archive("fuzz_no_such_symbol_archive");
    const std::vector<ReportGroup> groups = groupReports(reports, archive);
    int counted = 0;
    for (const ReportGroup& g : groups) { counted += g.count; }
    FUZZ_REQUIRE(counted == static_cast<int>(reports.size()));

    (void)renderGroupsText(groups, static_cast<int>(reports.size()), skipped, archive);
    (void)renderGroupsJson(groups, static_cast<int>(reports.size()), skipped);
    return 0;
}
