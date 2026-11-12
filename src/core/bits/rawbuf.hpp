// BIT-VIEW LIBRARY, part 10: raw slices kept in a bounded ring, and the decoder chain that is re-run over
// them AFTER THE FACT with different settings.
//
// A "slice" is what the slicer saw for one burst: the level runs (or already-clocked chips) in
// microseconds, with a start time and a short label. Nothing decoded is kept - the point is that the
// user can change the line code, polarity, clock, whitening, sync word or framing and read the SAME
// burst again, without waiting for the signal to come back. The ring is bounded twice (slice count and
// total stored runs) and evicts the oldest; it is safe to call from several threads.
//
// No identifier or message content is stored beyond the runs themselves; the label is a short free
// word chosen by the caller ("burst 12") and the ring never writes anything to disk.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "core/bits/bits.hpp"
#include "core/bits/linecode.hpp"
#include "core/bits/ookslicer.hpp"
#include "core/bits/sync.hpp"
#include "core/bits/whiten.hpp"

namespace cascade::bits {

struct RawSlice {
    uint64_t id = 0;        // assigned by the ring, rising
    double startUs = 0;     // when the burst began, on the caller's clock
    std::string label;      // at most 32 characters
    std::vector<LevelRun> runs;  // the level runs (alternating levels); empty when `chips` is used
    Bits chips;                  // already clocked chips (one value per chip period)
    double chipUs = 0;           // chip period of `chips`, informational
};

struct RawRingLimits {
    size_t maxSlices = 256;
    size_t maxRuns = 1u << 20;    // total level runs (+ chips / 8) held
};

class RawRing {
public:
    explicit RawRing(RawRingLimits lim = {}) : lim_(lim) {}

    // Adds a slice (the id is assigned); returns the id, or 0 when a single slice alone exceeds the
    // total limit (it is refused, never half-stored).
    uint64_t push(RawSlice s) {
        if (s.label.size() > 32) s.label.resize(32);
        const size_t cost = costOf(s);
        std::lock_guard<std::mutex> g(mu_);
        if (cost > lim_.maxRuns) { ++refused_; return 0; }
        s.id = ++nextId_;
        used_ += cost;
        items_.push_back(std::move(s));
        while (items_.size() > lim_.maxSlices || used_ > lim_.maxRuns) {
            used_ -= costOf(items_.front());
            items_.pop_front();
            ++evicted_;
        }
        return nextId_;
    }
    bool get(uint64_t id, RawSlice* out) const {
        std::lock_guard<std::mutex> g(mu_);
        for (const auto& s : items_)
            if (s.id == id) { if (out) *out = s; return true; }
        return false;
    }
    std::vector<RawSlice> snapshot() const {
        std::lock_guard<std::mutex> g(mu_);
        return std::vector<RawSlice>(items_.begin(), items_.end());
    }
    size_t size() const { std::lock_guard<std::mutex> g(mu_); return items_.size(); }
    size_t used() const { std::lock_guard<std::mutex> g(mu_); return used_; }
    size_t evicted() const { std::lock_guard<std::mutex> g(mu_); return evicted_; }
    size_t refused() const { std::lock_guard<std::mutex> g(mu_); return refused_; }
    void clear() { std::lock_guard<std::mutex> g(mu_); items_.clear(); used_ = 0; }

private:
    static size_t costOf(const RawSlice& s) { return s.runs.size() + s.chips.size() / 8 + 1; }
    RawRingLimits lim_;
    mutable std::mutex mu_;
    std::deque<RawSlice> items_;
    size_t used_ = 0, evicted_ = 0, refused_ = 0;
    uint64_t nextId_ = 0;
};

// ---- the decoder chain ---------------------------------------------------------------------------------

// A slice longer than this many chips is cut (a resource limit: a burst of an hour is not a burst).
inline constexpr size_t kMaxChips = 1u << 18;

enum class FramingKind : uint8_t { None, Fixed, Length, Flag };

struct ChainSettings {
    // runs -> chips
    bool balance = true;        // compensate the slicer's mark/space bias first
    double chipUs = 0;          // chip period in microseconds; 0 = estimate it from the runs
    // chips -> bits
    std::vector<LineCode> stages = {LineCode::NrzL};
    int phase = -1;             // chip phase for the first two-chip stage: -1 = the one with fewest violations
    bool invert = false;
    WhitenSetting whiten;
    // bits -> frames
    Bits sync;                  // empty: the whole bit stream is one frame
    size_t syncErrors = 0;
    bool syncBothPolarities = false;
    FramingKind framing = FramingKind::None;
    size_t fixedBits = 0;
    LengthFraming length;
    Bits flag = fromString("01111110");
    bool unstuff = true;
    size_t maxFrames = 1024;       // resource limits on the result
    size_t maxFrameBits = 4096;    // a frame framed by sync alone ends at the next sync hit or after this many bits
};

struct ChainResult {
    bool ok = false;
    std::string problem;           // why not ok (plain language)
    double chipUs = 0;             // the chip period used
    size_t violations = 0;
    Bits bits;                     // after line decoding, inversion and whitening
    std::vector<PatternHit> syncHits;
    std::vector<Frame> frames;
};

inline ChainResult redecode(const RawSlice& s, const ChainSettings& cs) {
    ChainResult r;
    if (cs.stages.empty() || cs.stages.size() > 3) { r.problem = "between one and three decoding stages are needed"; return r; }
    Bits cur;
    if (!s.runs.empty()) {
        std::vector<LevelRun> runs = cs.balance ? balanceRuns(s.runs) : s.runs;
        double T = cs.chipUs;
        if (!(T > 0)) T = estimateChipPeriod(runs);
        if (!(T > 0)) { r.problem = "the chip period could not be estimated (too few runs)"; return r; }
        r.chipUs = T;
        cur = runsToChips(runs, T, kMaxChips);
    } else if (!s.chips.empty()) {
        cur = s.chips;
        r.chipUs = s.chipUs;
    } else {
        r.problem = "the slice is empty";
        return r;
    }
    for (size_t i = 0; i < cs.stages.size(); ++i) {
        const LineCode lc = cs.stages[i];
        Decoded best;
        bool have = false;
        for (size_t ph = 0; ph < chipsPerBit(lc); ++ph) {
            if (i == 0 && cs.phase >= 0 && static_cast<size_t>(cs.phase) != ph && chipsPerBit(lc) > 1) continue;
            Decoded d = decodeLineCode(cur, lc, ph, 0);
            if (!have || d.violations < best.violations) { best = std::move(d); have = true; }
        }
        if (!have) { r.problem = "the chip phase asked for is outside this code"; return r; }
        r.violations += best.violations;
        cur = std::move(best.bits);
    }
    if (cs.invert) cur = inverted(cur);
    cur = applyWhitening(cur, cs.whiten);
    r.bits = cur;
    if (!cs.sync.empty()) {
        r.syncHits = findPattern(r.bits, cs.sync, cs.syncErrors, cs.syncBothPolarities);
        switch (cs.framing) {
            case FramingKind::Fixed: r.frames = frameFixed(r.bits, r.syncHits, cs.sync.size(), cs.fixedBits); break;
            case FramingKind::Length: r.frames = frameByLength(r.bits, r.syncHits, cs.sync.size(), cs.length); break;
            default: {
                for (size_t i = 0; i < r.syncHits.size() && r.frames.size() < cs.maxFrames; ++i) {
                    const auto& h = r.syncHits[i];
                    Frame f;
                    f.start = h.pos + cs.sync.size();
                    f.syncDistance = h.distance;
                    size_t end = i + 1 < r.syncHits.size() ? r.syncHits[i + 1].pos : r.bits.size();
                    if (end < f.start) end = f.start;
                    f.bits = slice(r.bits, f.start, std::min(end - f.start, cs.maxFrameBits));
                    r.frames.push_back(std::move(f));
                }
            }
        }
    } else if (cs.framing == FramingKind::Flag) {
        r.frames = frameByFlag(r.bits, cs.flag, cs.unstuff);
    } else {
        Frame f;
        f.bits = r.bits;
        r.frames.push_back(std::move(f));
    }
    if (r.frames.size() > cs.maxFrames) r.frames.resize(cs.maxFrames);
    r.ok = true;
    return r;
}

// Re-run every slice in the ring with one setting; the frames of all of them, in slice order.
struct RedecodeAll {
    std::vector<uint64_t> sliceIds;       // one per frame, the slice it came from
    std::vector<Frame> frames;
    size_t slicesOk = 0, slicesFailed = 0;
};
inline RedecodeAll redecodeAll(const RawRing& ring, const ChainSettings& cs) {
    RedecodeAll out;
    for (const auto& s : ring.snapshot()) {
        const ChainResult r = redecode(s, cs);
        if (!r.ok) { ++out.slicesFailed; continue; }
        ++out.slicesOk;
        for (const auto& f : r.frames) { out.sliceIds.push_back(s.id); out.frames.push_back(f); }
    }
    return out;
}

// Try every combination of polarity x a set of first-stage codes on one slice and rank by the number of sync hits then
// the violation rate: "which settings read this burst?" The caller's other settings are kept.
struct SweepRow {
    LineCode code = LineCode::NrzL;
    bool invert = false;
    size_t syncHits = 0;
    size_t violations = 0;
    size_t bits = 0;
};
inline std::vector<SweepRow> sweepSettings(const RawSlice& s, const ChainSettings& base, const std::vector<LineCode>& codes) {
    std::vector<SweepRow> rows;
    for (LineCode lc : codes)
        for (int inv = 0; inv < 2; ++inv) {
            ChainSettings cs = base;
            cs.stages = {lc};
            cs.invert = inv != 0;
            const ChainResult r = redecode(s, cs);
            if (!r.ok) continue;
            rows.push_back({lc, inv != 0, r.syncHits.size(), r.violations, r.bits.size()});
        }
    std::stable_sort(rows.begin(), rows.end(), [](const SweepRow& a, const SweepRow& b) {
        if (a.syncHits != b.syncHits) return a.syncHits > b.syncHits;
        const double ra = a.bits ? static_cast<double>(a.violations) / static_cast<double>(a.bits) : 1.0;
        const double rb = b.bits ? static_cast<double>(b.violations) / static_cast<double>(b.bits) : 1.0;
        return ra < rb;
    });
    return rows;
}

}  // namespace cascade::bits
