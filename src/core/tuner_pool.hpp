// tuner_pool.hpp - TUNER POOLING (engine serial lane A, row R19): a channel claims any free radio.
//
// THE IDEA. With several radios open, a listener who wants "this frequency" should not have to know which radio to point at it. A CLAIM names a frequency; the pool picks the radio that can give it for
// the least: first a free radio whose span on show already covers the frequency (it is tuned by its VFO alone, the hardware is not touched and whatever else it is feeding is not disturbed), then a free
// radio that can tune there by moving its centre. A radio that is claimed, locked, recording, scanning or not open is not free. Among equals the lowest radio number wins, so the same claim twice on
// the same pool gives the same answer.
//
// PURE: no engine, no thread; the engine fills the candidate list and acts on the answer (engine_pool.cpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace cascade::core::pool {

struct Candidate {
    int number = 0;              // the radio's number, 1-based
    bool free = false;           // open, not claimed, not locked, not recording, not scanning
    double centreHz = 0.0;       // its centre now
    double rateHz = 0.0;         // its sample rate now (0 = unknown)
    double rangeLoHz = 0.0;      // what its hardware can tune; both 0 = no limit known (the generator, a file is never offered by the engine)
    double rangeHiHz = 0.0;
    double channelHz = 0.0;      // the channel width it would listen with
};

enum class Verdict { ByVfo, ByCentre, None };

struct Pick {
    Verdict how = Verdict::None;
    int number = 0;
};

// The fraction of the sample rate the VFO may use (the same 0.45 as centre tuning).
inline constexpr double kUsableHalfSpan = 0.45;

inline bool coversByVfo(const Candidate& c, double hz) {
    return c.rateHz > 0.0 && std::fabs(hz - c.centreHz) + 0.5 * c.channelHz <= kUsableHalfSpan * c.rateHz;
}
inline bool canTune(const Candidate& c, double hz) {
    if (c.rangeLoHz == 0.0 && c.rangeHiHz == 0.0) { return true; }
    return hz >= c.rangeLoHz && hz <= c.rangeHiHz;
}

inline Pick choose(const std::vector<Candidate>& all, double hz) {
    if (!std::isfinite(hz) || !(hz > 0.0)) { return {}; }
    for (const Candidate& c : all) {
        if (c.free && coversByVfo(c, hz)) { return {Verdict::ByVfo, c.number}; }
    }
    for (const Candidate& c : all) {
        if (c.free && canTune(c, hz)) { return {Verdict::ByCentre, c.number}; }
    }
    return {};
}

}  // namespace cascade::core::pool
