// Fuzz target: a band plan file (resources/bandplans/*.json, or one a user adds).
//
// The waterfall's band overlay reads these, and so does the "what am I tuned to"
// readout; both query the plan with frequencies taken from the screen and from
// the tuner. The target loads the bytes as a plan, then asks the plan the
// questions the interface asks it (what is visible between two frequencies,
// what is at one), with the limits taken from the input so NaN, infinities and
// inverted ranges are all asked.
//
// Properties: visible() returns only entries that overlap the asked span, and
// at() returns an entry that contains the frequency.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/band_plan.hpp"
#include "fuzz_common.hpp"

using cascade::core::BandPlan;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::Bytes in(data, size);
    const double lo = in.rawDouble();
    const double hi = in.rawDouble();
    const std::size_t fileOffset = size < 16 ? size : 16;
    const std::string path = fuzz::writeScratch("plan.json", data + fileOffset, size - fileOffset);

    BandPlan plan;
    std::string error;
    if (!plan.loadFile(path, error)) {
        FUZZ_REQUIRE(!error.empty());
        return 0;
    }

    for (const auto* e : plan.visible(lo, hi)) {
        FUZZ_REQUIRE(e != nullptr);
        FUZZ_REQUIRE(e->startHz < hi && e->endHz > lo);  // strict on both edges, per band_plan.hpp
    }
    if (const auto* e = plan.at(lo)) { FUZZ_REQUIRE(e->startHz <= lo && lo <= e->endHz); }
    if (const auto* e = plan.at(hi)) { FUZZ_REQUIRE(e->startHz <= hi && hi <= e->endHz); }

    // The same text through the directory loader and the lister, which parse it
    // by another route (all-or-nothing merge; tolerant listing).
    BandPlan merged;
    (void)merged.loadDirectory(fuzz::scratchDir().string(), error);
    (void)BandPlan::available(fuzz::scratchDir().string());
    return 0;
}
