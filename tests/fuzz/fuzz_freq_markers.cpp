// Fuzz target: the waterfall markers file (markers.json).
//
// A user-visible file the user may edit by hand or carry between machines.
// Beyond parsing it, the target renders what was loaded - the clipboard text
// and the time stamp each marker shows - because a loaded value is only as safe
// as the code that later formats it: a time stamp from a file is an arbitrary
// 64-bit number by the time it reaches the C library's calendar functions.
//
// Properties: a loaded list never holds more than kMaxMarkers, numbers are
// unique, and a saved list loads back to the same markers.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

#include "core/freq_markers.hpp"
#include "fuzz_common.hpp"

using cascade::core::FreqMarkers;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string path = fuzz::writeScratch("markers.json", data, size);

    FreqMarkers markers;
    std::string error;
    if (!markers.load(path, error)) { return 0; }

    FUZZ_REQUIRE(markers.size() <= FreqMarkers::kMaxMarkers);
    std::set<int> numbers;
    for (const auto& m : markers.list()) {
        FUZZ_REQUIRE(numbers.insert(m.number).second);
        (void)cascade::core::formatMarkerTime(m.notedUnix, false);
        (void)cascade::core::formatMarkerTime(m.notedUnix, true);
        (void)cascade::core::markerMhzText(m.freqHz);
        (void)markers.nearest(m.freqHz, 1000.0);
    }
    (void)markers.clipboardText(false);
    (void)markers.clipboardText(true);
    (void)markers.nearest(0.0, 1.0e300);

    // The next marker the user drops must get a number nobody holds. "next" is
    // read from the file and every loaded number counts towards it, so an
    // extreme number in the file is an extreme starting point for the counter.
    if (markers.size() < FreqMarkers::kMaxMarkers) {
        double fresh = 1.0;
        for (const auto& m : markers.list()) { fresh = std::max(fresh, m.freqHz * 2.0 + 1.0); }
        if (std::isfinite(fresh)) {
            const std::size_t before = markers.size();
            const int made = markers.add(fresh, 0);
            FUZZ_REQUIRE(made > 0);
            FUZZ_REQUIRE(markers.size() == before + 1);
            std::size_t holders = 0;
            for (const auto& m : markers.list()) { holders += (m.number == made) ? 1 : 0; }
            FUZZ_REQUIRE(holders == 1);
        }
    }

    const std::string out = (fuzz::scratchDir() / "markers_out.json").string();
    FUZZ_REQUIRE(markers.save(out, error));
    FreqMarkers again;
    FUZZ_REQUIRE(again.load(out, error));
    FUZZ_REQUIRE(again.size() == markers.size());
    return 0;
}
