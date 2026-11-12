// BIT-VIEW LIBRARY, part 8: the OOK slicer - a magnitude envelope to level runs in microseconds.
//
// This is the front half of "decode a device the program has never heard of": the I/Q (or a
// magnitude trace) becomes a list of (level, duration) runs, which the line-code decoders and the
// user-described decoder (bitspec.hpp) read. It is deliberately small and adaptive:
//
//   * the envelope is smoothed with a short moving average (`smoothUs`),
//   * the noise floor is a low percentile and the peak a high percentile of the smoothed envelope,
//     the threshold sits `thresholdFraction` of the way between them with hysteresis of
//     `hysteresis` of that span, and
//   * a signal that never rises `minSpanRatio` above the floor is reported as "no signal" (empty).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/bits/linecode.hpp"

namespace cascade::bits {

struct SliceParams {
    double sampleRate = 250000.0;
    double smoothUs = 16.0;           // moving-average length
    double thresholdFraction = 0.5;   // of the way from the floor to the peak
    double hysteresis = 0.15;         // of (peak - floor)
    double floorPercentile = 0.20;
    double peakPercentile = 0.995;
    double minSpanRatio = 3.0;        // peak must exceed floor by this factor, else no signal
    size_t maxRuns = 1u << 20;        // resource limit
};

struct SliceResult {
    std::vector<LevelRun> runs;  // durations in microseconds; the first run starts at sample 0
    double floorLevel = 0, peakLevel = 0, threshold = 0;
    bool signal = false;
};

inline SliceResult sliceEnvelopeAt(const std::vector<float>& mag, const SliceParams& p) {
    SliceResult r;
    if (mag.size() < 8 || !(p.sampleRate > 0)) return r;
    const size_t win = std::max<size_t>(1, static_cast<size_t>(std::lround(p.smoothUs * 1e-6 * p.sampleRate)));
    std::vector<float> sm(mag.size());
    double acc = 0;
    for (size_t i = 0; i < mag.size(); ++i) {
        acc += mag[i];
        if (i >= win) acc -= mag[i - win];
        sm[i] = static_cast<float>(acc / static_cast<double>(std::min(i + 1, win)));
    }
    std::vector<float> sorted(sm);
    const size_t nF = static_cast<size_t>(p.floorPercentile * static_cast<double>(sorted.size() - 1));
    const size_t nP = static_cast<size_t>(p.peakPercentile * static_cast<double>(sorted.size() - 1));
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(nF), sorted.end());
    r.floorLevel = sorted[nF];
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(nP), sorted.end());
    r.peakLevel = sorted[nP];
    if (!(r.peakLevel > p.minSpanRatio * std::max(r.floorLevel, 1e-9))) return r;
    const double span = r.peakLevel - r.floorLevel;
    r.threshold = r.floorLevel + p.thresholdFraction * span;
    const double hi = r.threshold + p.hysteresis * span / 2, lo = r.threshold - p.hysteresis * span / 2;
    r.signal = true;
    const double usPerSample = 1e6 / p.sampleRate;
    uint8_t level = sm[0] > r.threshold ? 1 : 0;
    size_t start = 0;
    for (size_t i = 1; i < sm.size(); ++i) {
        const uint8_t next = level ? (sm[i] < lo ? 0 : 1) : (sm[i] > hi ? 1 : 0);
        if (next != level) {
            r.runs.push_back({level, static_cast<double>(i - start) * usPerSample});
            if (r.runs.size() >= p.maxRuns) return r;
            level = next;
            start = i;
        }
    }
    r.runs.push_back({level, static_cast<double>(sm.size() - start) * usPerSample});
    return r;
}

inline SliceResult sliceEnvelope(const std::vector<float>& mag, const SliceParams& p = {}) { return sliceEnvelopeAt(mag, p); }

// Edge-bias compensation. A transmitter whose carrier rises and falls unevenly, or an envelope that is not
// symmetric about the threshold, makes every mark shorter by b and every space longer by b (or the reverse):
// on a recorded Oregon sensor the shortest marks measured 380 us and the shortest spaces 600 us for one and
// the same chip. b is estimated from the shortest eighth of the marks and of the spaces (runs over 5 ms are
// idle and ignored) as half their difference, and applied to every run. Returns the compensated runs and, in
// `biasOut`, the shift applied to marks (negative: marks were too short and get longer).
inline std::vector<LevelRun> balanceRuns(std::vector<LevelRun> runs, double* biasOut = nullptr, double idleUs = 5000.0) {
    std::vector<double> marks, spaces;
    for (const auto& x : runs)
        if (x.duration <= idleUs) (x.level ? marks : spaces).push_back(x.duration);
    if (biasOut) *biasOut = 0.0;
    if (marks.size() < 4 || spaces.size() < 4) return runs;
    auto lowMedian = [](std::vector<double>& v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 8];
    };
    const double b = (lowMedian(spaces) - lowMedian(marks)) / 2.0;  // marks gain b, spaces lose b
    for (auto& x : runs) {
        if (x.duration > idleUs) continue;
        x.duration = std::max(1.0, x.duration + (x.level ? b : -b));
    }
    if (biasOut) *biasOut = b;
    return runs;
}

// Convenience: interleaved float I/Q to magnitude.
inline std::vector<float> iqMagnitude(const std::vector<float>& iq) {
    std::vector<float> m(iq.size() / 2);
    for (size_t i = 0; i < m.size(); ++i) m[i] = std::hypot(iq[2 * i], iq[2 * i + 1]);
    return m;
}

// Absorb glitches: runs shorter than `minUs` are merged into their neighbours (the level of the
// longer neighbour wins), repeatedly, so that a one-sample dropout inside a mark disappears.
inline std::vector<LevelRun> despeckle(std::vector<LevelRun> runs, double minUs) {
    bool changed = true;
    while (changed && runs.size() > 2) {
        changed = false;
        for (size_t i = 0; i < runs.size(); ++i) {
            if (runs[i].duration >= minUs) continue;
            if (i == 0 || i + 1 == runs.size()) continue;  // keep the ends
            // merge run i and its next neighbour into the previous one (same level as i-1 and i+1)
            runs[i - 1].duration += runs[i].duration + runs[i + 1].duration;
            runs.erase(runs.begin() + static_cast<std::ptrdiff_t>(i), runs.begin() + static_cast<std::ptrdiff_t>(i + 2));
            changed = true;
            break;
        }
    }
    return runs;
}

}  // namespace cascade::bits
