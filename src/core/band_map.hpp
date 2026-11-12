// band_map.hpp - a band-map store with a spot stack (hamops lane): spots with age-out, a bounded size, a "worked" flag and a "needed" (multiplier)
// flag, listed by frequency, with next-up / next-down navigation. Pure code; the window that draws it is another lane's, and the spot INPUT (the
// dx-spots plugin or a cluster feed) is a hand-off: whoever has a spot calls addSpot().
//
// N1MM's own "multiplier / band map window with a spot stack" was not opened; this model provides the parts the gap names (spots, ages, worked,
// needed, a stack in frequency order, stepping) and does not claim N1MM's exact semantics.
//
// Time is passed in (unix seconds) so the store is deterministic. The spot calls are other people's callsigns kept only in memory; nothing is
// printed or written.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "qso_log.hpp"

namespace cascade::core::ham {

struct Spot {
    std::string call;
    double freqHz = 0.0;
    std::string mode;
    std::int64_t seenUnix = 0;   // last time a spot for this call near this frequency arrived
    std::int64_t firstUnix = 0;
    int count = 1;               // how many spots were merged into this one
    bool worked = false;
    bool needed = false;         // set by the caller (e.g. a new multiplier)
};

struct BandMapConfig {
    std::size_t capacity = 200;           // the oldest-seen spot goes when a new one would pass this
    std::int64_t maxAgeSec = 1800;        // 30 minutes
    double mergeToleranceHz = 500.0;      // the same call within this of a stored spot updates it
};

enum class AddStatus { Added, Merged, Refused };

class BandMap {
public:
    explicit BandMap(BandMapConfig c = BandMapConfig()) : cfg_(c) {
        if (cfg_.capacity == 0) { cfg_.capacity = 1; }
        if (cfg_.capacity > 100000) { cfg_.capacity = 100000; }
        if (cfg_.maxAgeSec < 1) { cfg_.maxAgeSec = 1; }
        if (cfg_.mergeToleranceHz < 0) { cfg_.mergeToleranceHz = 0; }
    }

    const BandMapConfig& config() const { return cfg_; }
    std::size_t size() const { return spots_.size(); }
    std::uint64_t evicted() const { return evicted_; }
    std::uint64_t refused() const { return refused_; }
    void clear() { spots_.clear(); }

    AddStatus addSpot(const std::string& call, double freqHz, const std::string& mode, std::int64_t nowUnix) {
        std::string c;
        if (!normaliseCall(call, c) || !(freqHz > 0.0) || freqHz > 1.0e12 || nowUnix < 0) { ++refused_; return AddStatus::Refused; }
        for (Spot& s : spots_) {
            if (s.call == c && std::abs(s.freqHz - freqHz) <= cfg_.mergeToleranceHz) {
                s.freqHz = freqHz;   // the newest report is the best one
                s.seenUnix = std::max(s.seenUnix, nowUnix);
                if (!mode.empty()) { s.mode = mode.substr(0, 12); }
                if (s.count < 1000000) { ++s.count; }
                return AddStatus::Merged;
            }
        }
        if (spots_.size() >= cfg_.capacity) {
            auto oldest = std::min_element(spots_.begin(), spots_.end(), [](const Spot& a, const Spot& b) { return a.seenUnix < b.seenUnix; });
            spots_.erase(oldest);
            ++evicted_;
        }
        Spot s;
        s.call = c;
        s.freqHz = freqHz;
        s.mode = mode.substr(0, 12);
        s.seenUnix = s.firstUnix = nowUnix;
        spots_.push_back(s);
        return AddStatus::Added;
    }

    // Removes spots not refreshed for maxAgeSec; returns how many went. A spot stamped in the future is kept.
    std::size_t ageOut(std::int64_t nowUnix) {
        const std::size_t before = spots_.size();
        spots_.erase(std::remove_if(spots_.begin(), spots_.end(), [&](const Spot& s) { return nowUnix - s.seenUnix > cfg_.maxAgeSec; }), spots_.end());
        return before - spots_.size();
    }

    // Flags every spot of this call; returns how many were flagged.
    std::size_t markWorked(const std::string& call, bool worked = true) {
        std::string c;
        if (!normaliseCall(call, c)) { return 0; }
        std::size_t n = 0;
        for (Spot& s : spots_) { if (s.call == c) { s.worked = worked; ++n; } }
        return n;
    }
    std::size_t setNeeded(const std::string& call, bool needed) {
        std::string c;
        if (!normaliseCall(call, c)) { return 0; }
        std::size_t n = 0;
        for (Spot& s : spots_) { if (s.call == c) { s.needed = needed; ++n; } }
        return n;
    }

    // The stack: spots with loHz <= f <= hiHz in ascending frequency (ties: older first).
    std::vector<Spot> stack(double loHz, double hiHz, bool hideWorked = false) const {
        std::vector<Spot> v;
        for (const Spot& s : spots_) {
            if (s.freqHz >= loHz && s.freqHz <= hiHz && !(hideWorked && s.worked)) { v.push_back(s); }
        }
        std::stable_sort(v.begin(), v.end(), [](const Spot& a, const Spot& b) { return a.freqHz != b.freqHz ? a.freqHz < b.freqHz : a.seenUnix < b.seenUnix; });
        return v;
    }
    std::vector<Spot> all() const { return stack(0.0, 1.0e12); }

    // The nearest spot strictly above / below `freqHz` (outside a 1 Hz dead zone), optionally skipping worked ones and/or keeping only needed ones.
    std::optional<Spot> nextUp(double freqHz, bool skipWorked = true, bool neededOnly = false) const { return step(freqHz, true, skipWorked, neededOnly); }
    std::optional<Spot> nextDown(double freqHz, bool skipWorked = true, bool neededOnly = false) const { return step(freqHz, false, skipWorked, neededOnly); }

private:
    std::optional<Spot> step(double f, bool up, bool skipWorked, bool neededOnly) const {
        const Spot* best = nullptr;
        for (const Spot& s : spots_) {
            if (skipWorked && s.worked) { continue; }
            if (neededOnly && !s.needed) { continue; }
            if (up ? !(s.freqHz > f + 1.0) : !(s.freqHz < f - 1.0)) { continue; }
            if (best == nullptr || (up ? s.freqHz < best->freqHz : s.freqHz > best->freqHz)) { best = &s; }
        }
        if (best == nullptr) { return std::nullopt; }
        return *best;
    }

    BandMapConfig cfg_;
    std::vector<Spot> spots_;
    std::uint64_t evicted_ = 0, refused_ = 0;
};

}  // namespace cascade::core::ham
