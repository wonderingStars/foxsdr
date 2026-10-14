// rail_reads.hpp - the pure parts of what the receiver rail and the display read
// from the engine's table (window port, group 5, docs/engine-stage4.md 6.5; API
// 0.3 section 8.4). No ImGui, no engine: arithmetic over rows and numbers, so a
// test can hold each rule against the model it restates.
//
//   - the mode the Radio section lights, from FoxReceiverState::demodMode
//   - the band plan as the window keeps it (BAND_PLAN's rows) and the two rules
//     the model (core/band_plan.hpp) answers with: the NARROWEST band under a
//     frequency, and the bands a view shows. tests/test_rail_reads.cpp holds
//     both against core::BandPlan::at and ::visible on the shipped plans.
//   - when the Language section may say that the Display section has since
//     chosen another band plan than the country picked.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cascade::gui {

// THE RADIO SECTION'S MODE KEYS are in kModeNames' order (NFM WFM AM DSB USB CW
// LSB RAW) and FOXAPI_DEMOD_* is that order, 1-based (abiDemodForModeIndex). The
// zero state before the window's first read has demodMode 0: NFM, the engine's
// own initial answer (initialPublishedState), never an index of -1.
inline int railModeIndex(std::uint32_t demodMode) {
    if (demodMode < 1u) { return 0; }
    if (demodMode > 8u) { return 7; }
    return static_cast<int>(demodMode) - 1;
}

// One row of BAND_PLAN as the window keeps it: the band's edges, its name and the
// service the palette colours it by.
struct BandRow {
    double startHz = 0.0;
    double endHz = 0.0;
    std::string name;
    std::string service;
};

// The most specific (NARROWEST) row containing hz, or nullptr in a gap: both
// edges inclusive, the first of two of equal width in row order, nothing for a
// non-finite frequency - core::BandPlan::at's rule, over the rows in the model's
// order (start ascending, the widest first on a shared start).
inline const BandRow* bandRowAt(const std::vector<BandRow>& rows, double hz) {
    if (!std::isfinite(hz)) { return nullptr; }
    const BandRow* best = nullptr;
    double bestWidth = 0.0;
    for (const BandRow& e : rows) {
        if (hz < e.startHz || hz > e.endHz) { continue; }
        const double width = e.endHz - e.startHz;
        if (best == nullptr || width < bestWidth) {
            best = &e;
            bestWidth = width;
        }
    }
    return best;
}

// Whether a row overlaps the window [lowHz, highHz]: STRICT at both edges (a band
// that only touches the edge covers no pixel), and nothing at all for an empty,
// inverted or non-finite window - core::BandPlan::visible's rule, one row at a
// time.
inline bool bandRowVisible(const BandRow& e, double lowHz, double highHz) {
    if (!(highHz > lowHz)) { return false; }  // also rejects NaN endpoints
    return e.startHz < highHz && e.endHz > lowHz;
}

// How many rows a window shows (a pre-scan, so the labels' room can be reserved
// once and a frame with nothing visible draws nothing).
inline std::size_t bandRowsVisible(const std::vector<BandRow>& rows, double lowHz, double highHz) {
    std::size_t n = 0;
    for (const BandRow& e : rows) { n += bandRowVisible(e, lowHz, highHz) ? 1u : 0u; }
    return n;
}

// THE LANGUAGE SECTION'S NOTE, "Display has since been set to a different band
// plan". A country picked in the Language section asks the engine for its band
// plan (SET_BAND_PLAN); the engine applies it in its next pass, so for the frame
// or two before the state shows it the plan in force is still the old one, and
// the note would be a falsehood about a plan that has just been asked for. The
// window therefore remembers what it asked for, the plan that was in force when
// it asked and the frame it asked on, and holds the note back while the state
// still shows the plan it was in force - for a bounded time, so a request the
// engine refused still ends in the note.
inline constexpr int kBandPlanLandFrames = 120;

struct BandPlanAsk {
    std::string asked;   // the plan the country wanted, "" none asked yet
    std::string before;  // the plan in force when it asked
    int frame = 0;       // ImGui's frame count when it asked
};

// `wanted` is the plan the chosen country maps to, `active` the plan in force
// (the BAND_PLANS entry that is ACTIVE; "" none), `pickedNow` that the country
// was picked on this very frame.
inline bool bandPlanNoteDue(const BandPlanAsk& ask, const std::string& wanted, const std::string& active,
                            int frameNow, bool pickedNow) {
    if (active == wanted || pickedNow) { return false; }
    const bool landing = !ask.asked.empty() && ask.asked == wanted && active == ask.before &&
                         frameNow - ask.frame < kBandPlanLandFrames;
    return !landing;
}

}  // namespace cascade::gui
