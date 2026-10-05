// Fuzz target: the bookmark list (bookmarks.json).
//
// Per-entry tolerant by design - a damaged entry is skipped and the rest load -
// and carried between machines and versions, so it meets every shape of wrong.
// After loading, the target drives what the interface does with the list: the
// range query that decides which bookmarks are drawn on the waterfall, the
// nearest-subset pick, and the group removal, over values straight from the
// file (NaN, infinities, huge or negative frequencies).
//
// Properties: the list stays sorted by frequency (every consumer binary-searches
// it), range() never returns an inverted or out-of-bounds span, nearestSubset()
// returns in-bounds, distinct indices, and a saved list loads back whole.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

#include "core/freq_manager.hpp"
#include "fuzz_common.hpp"

using cascade::core::FreqManager;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // The first eight bytes steer the queries; the rest is the file.
    fuzz::Bytes in(data, size);
    const double lo = in.rawDouble();
    const double hi = in.rawDouble();
    const std::size_t fileOffset = size < 16 ? size : 16;
    const std::string path =
        fuzz::writeScratch("bookmarks.json", data + fileOffset, size - fileOffset);

    FreqManager mgr;
    std::string error;
    if (!mgr.load(path, error)) { return 0; }

    const auto& list = mgr.list();
    for (std::size_t i = 1; i < list.size(); ++i) {
        FUZZ_REQUIRE(list[i - 1].freqHz <= list[i].freqHz);
    }

    const auto span = mgr.range(lo, hi);
    FUZZ_REQUIRE(span.first <= span.second);
    FUZZ_REQUIRE(span.second <= list.size());

    const std::vector<std::size_t> nearest = mgr.nearestSubset(lo, 7, 3);
    std::set<std::size_t> seen;
    for (const std::size_t idx : nearest) {
        FUZZ_REQUIRE(idx < list.size());
        FUZZ_REQUIRE(seen.insert(idx).second);
    }

    const std::string out = (fuzz::scratchDir() / "bookmarks_out.json").string();
    FUZZ_REQUIRE(mgr.save(out, error));
    FreqManager again;
    FUZZ_REQUIRE(again.load(out, error));
    FUZZ_REQUIRE(again.list().size() == list.size());

    if (!list.empty()) { (void)mgr.removeGroup(list.front().group); }
    return 0;
}
