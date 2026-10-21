// Fuzz target: a frequency list a user imports - SDR# XML, or CSV in any of the
// layouts spreadsheets produce.
//
// The importers are hand-written scanners (no XML library, a CSV splitter with
// its own quoting), reading a file the user was handed by somebody else: a
// forum attachment, a club's channel list. After import the items go through
// FreqManager::addMany, which is what the Import dialog does, so that is part
// of the target.
//
// Properties: nothing that reaches the bookmark list has a non-finite or
// negative frequency; the export of an imported list parses back.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/freq_import.hpp"
#include "core/freq_manager.hpp"
#include "fuzz_common.hpp"

using namespace cascade::core;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string text = fuzz::asString(data, size);

    const ImportResult r = importFrequencyList(text);
    // The two parsers directly as well, whichever one the sniffing picked.
    (void)importSdrSharpXml(text);
    (void)importCsv(text);

    FreqManager mgr;
    mgr.addMany(r.items);
    for (const Bookmark& b : mgr.list()) {
        FUZZ_REQUIRE(std::isfinite(b.freqHz) && b.freqHz >= 0.0);
    }

    if (!mgr.list().empty()) {
        const std::string xml = exportSdrSharpXml(mgr.list());
        const ImportResult back = importSdrSharpXml(xml);
        FUZZ_REQUIRE(back.entries == mgr.list().size());
    }
    return 0;
}
