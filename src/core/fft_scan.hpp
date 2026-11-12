// An FFT SCAN: look for signals across the whole captured span from ONE spectrum frame, without retuning the radio.
//
// A step scan retunes to every lattice point and waits a dwell at each; with 2 MHz of captured span and 12.5 kHz channels that is 160 retunes where
// the spectrum already shows every channel at once. This model takes one frame of power in dB (the same frame the spectrum draws) and the scan grid
// (startHz, stopHz, stepHz) and answers: which grid channels carry a signal, at what level and how far above the frame's noise floor. The channels
// that fall outside the captured span are COUNTED, not guessed, so the caller knows how many a retune still has to cover.
//
// FRAME LAYOUT. bins[0] is the lowest frequency: bin i covers centreHz - spanHz/2 + (i + 0.5) * spanHz / n (the display order, after the FFT shift).
// The caller converts if its frame is in FFT order.
//
// A CHANNEL is the grid point f = startHz + k * stepHz (k >= 0, f <= stopHz, computed from k so nothing drifts); its level is the MAXIMUM of the bins
// whose centre lies within +-stepHz/2 of f (+-widthHz/2 when the width is set), so a signal off the channel centre still counts. A channel with no bin
// in it (step finer than a bin and no bin centre inside) takes the nearest bin, so no in-span channel is ever skipped for lack of a bin.
//
// THE NOISE FLOOR comes from the frame itself and needs no history: the median of the in-span channel levels (a frame in which more than half the
// channels carry a signal has no honest floor; then the result's floorReliable is false and the Fixed level should be used). The hit rule is that of
// core/scan_trigger.hpp: Relative: level >= floor + marginDb; Fixed: level >= fixedDb. Neighbouring hit channels (a wide signal over several
// channels) are merged into ONE hit at the strongest channel when mergeAdjacent is set.
//
// Non-finite bins read as "no data" (kNoDataDb): never a hit, never the floor.
//
// No allocation beyond the result vectors; no state; thread safe (a pure function).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "core/scan_trigger.hpp"

namespace cascade::core::tuning {

struct FftScanGrid {
    double startHz = 0.0;
    double stopHz = 0.0;
    double stepHz = 12500.0;
    double widthHz = 0.0;       // 0 = the step
};

struct FftScanOptions {
    TriggerMode mode = TriggerMode::Relative;
    double fixedDb = -70.0;
    double marginDb = 8.0;
    bool mergeAdjacent = true;
    std::size_t maxChannels = 200000;
};

struct FftScanHit {
    double hz = 0.0;            // the channel's grid frequency
    double levelDb = 0.0;
    double aboveFloorDb = 0.0;  // 0 when the floor is not reliable
    std::size_t channel = 0;    // k
};

struct FftScanResult {
    bool ok = false;
    const char* error = "";
    std::vector<FftScanHit> hits;
    std::size_t channelsInSpan = 0;
    std::size_t channelsOutOfSpan = 0;   // grid channels this frame does not cover
    double floorDb = kNoDataDb;
    bool floorReliable = false;
};

inline FftScanResult fftScan(const double* binsDb, std::size_t n, double centreHz, double spanHz, FftScanGrid grid, const FftScanOptions& opt = {}) {
    FftScanResult r;
    if (binsDb == nullptr || n < 2) { r.error = "no spectrum frame"; return r; }
    if (!std::isfinite(centreHz) || !(spanHz > 0.0) || !std::isfinite(spanHz)) { r.error = "bad centre or span"; return r; }
    if (!std::isfinite(grid.startHz) || !std::isfinite(grid.stopHz) || !std::isfinite(grid.stepHz) || !(grid.stepHz > 0.0)) { r.error = "bad scan grid"; return r; }
    if (grid.stopHz < grid.startHz) { std::swap(grid.startHz, grid.stopHz); }
    if (!(grid.widthHz > 0.0) || !std::isfinite(grid.widthHz)) { grid.widthHz = grid.stepHz; }
    const double total = std::floor((grid.stopHz - grid.startHz) / grid.stepHz) + 1.0;
    if (!(total >= 1.0) || total > static_cast<double>(opt.maxChannels)) { r.error = "too many channels"; return r; }
    const std::size_t nCh = static_cast<std::size_t>(total);

    const double loHz = centreHz - spanHz / 2.0;
    const double binHz = spanHz / static_cast<double>(n);
    struct Ch { double hz; double level; bool in; };
    std::vector<Ch> ch(nCh);
    std::vector<double> inLevels;
    for (std::size_t k = 0; k < nCh; ++k) {
        const double f = grid.startHz + static_cast<double>(k) * grid.stepHz;
        ch[k] = {f, kNoDataDb, false};
        // the channel counts as covered when its centre is inside the captured span
        if (f < loHz || f >= loHz + spanHz) { ++r.channelsOutOfSpan; continue; }
        ++r.channelsInSpan;
        const double a = f - grid.widthHz / 2.0, b = f + grid.widthHz / 2.0;
        // bins whose centre is in [a, b]: centre(i) = loHz + (i + 0.5) * binHz
        long long i0 = static_cast<long long>(std::ceil((a - loHz) / binHz - 0.5));
        long long i1 = static_cast<long long>(std::floor((b - loHz) / binHz - 0.5));
        i0 = std::max<long long>(i0, 0);
        i1 = std::min<long long>(i1, static_cast<long long>(n) - 1);
        double best = kNoDataDb;
        bool any = false;
        if (i0 > i1) {
            long long nearest = static_cast<long long>(std::floor((f - loHz) / binHz));
            nearest = std::max<long long>(0, std::min<long long>(nearest, static_cast<long long>(n) - 1));
            i0 = i1 = nearest;
        }
        for (long long i = i0; i <= i1; ++i) {
            const double v = binsDb[static_cast<std::size_t>(i)];
            if (!std::isfinite(v) || v < kNoDataDb) { continue; }
            best = any ? std::max(best, v) : v;
            any = true;
        }
        ch[k].level = best;
        ch[k].in = any;
        if (any) { inLevels.push_back(best); }
    }
    if (!inLevels.empty()) {
        std::vector<double> sorted = inLevels;
        const std::size_t mid = sorted.size() / 2;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(mid), sorted.end());
        r.floorDb = sorted[mid];
    }
    const double trigger = opt.mode == TriggerMode::Fixed ? opt.fixedDb : (r.floorDb > kNoDataDb ? r.floorDb + opt.marginDb : INFINITY);
    std::size_t hitCount = 0;
    for (const Ch& c : ch) { if (c.in && c.level >= trigger) { ++hitCount; } }
    // more than half the channels over the trigger: the median is itself signal, not a floor
    r.floorReliable = !inLevels.empty() && hitCount * 2 <= inLevels.size();

    bool run = false;
    for (std::size_t k = 0; k < nCh; ++k) {
        const bool hit = ch[k].in && ch[k].level >= trigger;
        if (!hit) { run = false; continue; }
        FftScanHit h{ch[k].hz, ch[k].level, r.floorReliable ? ch[k].level - r.floorDb : 0.0, k};
        if (opt.mergeAdjacent && run && !r.hits.empty()) {
            if (h.levelDb > r.hits.back().levelDb) { r.hits.back() = h; }
        } else {
            r.hits.push_back(h);
        }
        run = true;
    }
    r.ok = true;
    return r;
}

}  // namespace cascade::core::tuning
