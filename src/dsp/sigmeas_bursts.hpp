// sigmeas_bursts.hpp - BURST DETECTOR and BURST LIST (time, length, frequency, power, repeat period)
// on a selected channel. Row: "burst detector and burst list" (REMAINING-GAPS.md, Signal analysis
// and identification).
//
// METHOD
//  - Block power: the mean of |x|^2 over blocks of W samples (W = `blockSamples`, default
//    rate / 4000, at least 16: a quarter of a millisecond or 16 samples), in dB.
//  - Two levels: Otsu's threshold on the block powers in dB splits them into an "off" class and an
//    "on" class; the noise floor is the median of the off class, the burst level the median of the
//    on class. If the two differ by less than `minContrastDb` (default 6 dB) the span has no on/off
//    structure - a steady carrier, or noise only - and the block says so (`continuous`), because a
//    threshold laid through noise would list noise as bursts.
//  - Bursts are runs of "on" blocks. Gaps shorter than `minGapBlocks` (default 1 block) are bridged
//    (a fade inside one burst is not two bursts); runs shorter than `minBurstBlocks` (default 2) are
//    dropped (a noise spike is not a burst).
//  - Edges to a fraction of a block: the blocks on either side of a run are partly filled; the
//    share of such a block that is on is (p - floor) / (level - floor), clamped to 0..1, and moves
//    the edge by that share of W (the first and last on-blocks are partly filled too: the start is the
//    end of the first on-block minus the shares of it and of the block before). So the start, end and length come out finer than one block, for
//    bursts with a flat top. (The pinned accuracy is "within one block"; the tests show the finer
//    figure.)
//  - Power: mean block power over the burst, dBFS (full scale amplitude 1.0 = 0 dB), and the peak
//    block power. Frequency: the power-weighted mean frequency of the burst, arg(sum x[n] conj(x[n-1]))
//    rate / 2 pi (for an FSK burst, its centre), from the span's centre.
//  - Repeat period: the median start-to-start spacing (needs at least 3 bursts); `repeatJitter` is
//    the median absolute deviation of the spacings over their median (0 = perfectly regular).
//
// Nothing is allocated per sample: block powers and the list are sized before the loops.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_DSP_SIGMEAS_BURSTS_HPP
#define CASCADE_DSP_SIGMEAS_BURSTS_HPP

#include <cstddef>
#include <vector>

#include "dsp/sigmeas_common.hpp"

namespace cascade::sigmeas {

struct BurstParams {
    std::size_t blockSamples = 0;   // 0 = automatic (rate / 4000, at least 16)
    double minContrastDb = 6.0;     // burst level over the floor, to call it on/off
    std::size_t minGapBlocks = 1;   // gaps up to this many blocks are bridged
    std::size_t minBurstBlocks = 2; // shorter runs are dropped
    std::size_t maxBursts = 1000;   // listed (totalBursts counts all)
};

struct Burst {
    std::size_t startSample = 0;
    std::size_t lengthSamples = 0;
    double startS = 0.0;
    double lengthS = 0.0;
    double powerDbfs = -300.0;      // mean power over the burst
    double peakDbfs = -300.0;       // strongest block
    double freqHz = 0.0;            // mean frequency from the span centre
};

struct BurstReport {
    bool valid = false;
    const char* reason = why::kEmpty;
    bool continuous = false;        // one steady level: no on/off structure
    std::size_t blockSamples = 0;
    double noiseDbfs = kNaN;        // block-power floor
    double levelDbfs = kNaN;        // typical burst block power
    double thresholdDbfs = kNaN;
    std::size_t totalBursts = 0;
    double dutyCycle = kNaN;        // on-time over the span
    double repeatPeriodS = kNaN;    // median start-to-start; NaN with fewer than 3 bursts
    double repeatJitter = kNaN;     // MAD / median of the spacings
    std::vector<Burst> bursts;
};

inline BurstReport detectBursts(const Cf* x, std::size_t n, double rate, const BurstParams& prm = {}) {
    BurstReport r;
    if (const char* bad = checkIq(x, n, rate, 64)) { r.reason = bad; return r; }
    if (!std::isfinite(prm.minContrastDb) || !(prm.minContrastDb > 0.0) || prm.minBurstBlocks == 0 ||
        prm.maxBursts == 0) {
        r.reason = why::kBadParam;
        return r;
    }
    const std::size_t W = prm.blockSamples > 0
                              ? prm.blockSamples
                              : std::max<std::size_t>(16, static_cast<std::size_t>(rate / 4000.0));
    const std::size_t nb = n / W;
    if (nb < 8) { r.reason = why::kTooShort; return r; }
    r.blockSamples = W;

    std::vector<double> pw(nb), db(nb);
    for (std::size_t b = 0; b < nb; ++b) {
        double s = 0.0;
        for (std::size_t i = b * W; i < (b + 1) * W; ++i) { s += std::norm(std::complex<double>(x[i])); }
        pw[b] = s / static_cast<double>(W);
        db[b] = toDb(pw[b]);
    }
    // Digital silence (exact zeros) is -300 dB and would stretch the histogram the threshold is
    // taken from: block levels are held to 50 dB under the median block.
    {
        std::vector<double> tmp = db;
        const double lowest = percentileInPlace(tmp, 0.5) - 50.0;
        for (double& d : db) { d = std::max(d, lowest); }
    }
    double sep = 0.0;
    const double thrDb = otsuThreshold(db.data(), db.size(), &sep);
    std::vector<double> up, dn;
    for (double d : db) { (d > thrDb ? up : dn).push_back(d); }
    if (up.empty() || dn.empty()) {
        std::vector<double> all = db;
        r.noiseDbfs = percentileInPlace(all, 0.5);
        r.continuous = true;
        r.reason = why::kNoStructure;
        return r;
    }
    const double levelDb = percentileInPlace(up, 0.5);
    const double floorDb = percentileInPlace(dn, 0.5);
    if (!(levelDb - floorDb >= prm.minContrastDb)) {
        std::vector<double> all = db;
        r.noiseDbfs = percentileInPlace(all, 0.5);
        r.continuous = true;
        r.reason = why::kNoStructure;
        return r;
    }
    r.noiseDbfs = floorDb;
    r.levelDbfs = levelDb;
    r.thresholdDbfs = thrDb;
    const double floorP = fromDb(floorDb), levelP = fromDb(levelDb);

    // runs of on-blocks
    std::vector<char> on(nb);
    for (std::size_t b = 0; b < nb; ++b) { on[b] = db[b] > thrDb ? 1 : 0; }
    // bridge short gaps
    for (std::size_t b = 0; b < nb;) {
        if (!on[b]) {
            std::size_t e = b;
            while (e < nb && !on[e]) { ++e; }
            if (b > 0 && e < nb && e - b <= prm.minGapBlocks) {
                for (std::size_t k = b; k < e; ++k) { on[k] = 1; }
            }
            b = e;
        } else {
            ++b;
        }
    }
    struct Run { std::size_t a, b; };  // blocks [a, b)
    std::vector<Run> runs;
    for (std::size_t b = 0; b < nb;) {
        if (on[b]) {
            std::size_t e = b;
            while (e < nb && on[e]) { ++e; }
            if (e - b >= prm.minBurstBlocks) { runs.push_back({b, e}); }
            b = e;
        } else {
            ++b;
        }
    }
    r.totalBursts = runs.size();
    auto share = [&](std::size_t blk) {
        if (blk >= nb) { return 0.0; }
        return std::min(1.0, std::max(0.0, (pw[blk] - floorP) / (levelP - floorP)));
    };
    std::vector<double> starts;
    double onSamples = 0.0;
    r.bursts.reserve(std::min(runs.size(), prm.maxBursts));
    for (const Run& run : runs) {
        const double wd = static_cast<double>(W);
        // edge shares: a burst that starts inside block a-1 or a is "full" from the end of block a on, so
        //   start = end of block a - (share(a-1) + share(a)) * W; the end is mirrored on blocks b-1, b.
        const double sA1 = run.a > 0 ? share(run.a - 1) : 0.0;
        const double startPos = static_cast<double>(run.a + 1) * wd - (sA1 + share(run.a)) * wd;
        const double endPos = static_cast<double>(run.b - 1) * wd + (share(run.b - 1) + share(run.b)) * wd;
        starts.push_back(startPos);
        onSamples += endPos - startPos;
        if (r.bursts.size() >= prm.maxBursts) { continue; }
        Burst bu;
        bu.startSample = static_cast<std::size_t>(std::max(0.0, std::round(startPos)));
        bu.lengthSamples = static_cast<std::size_t>(std::max(1.0, std::round(endPos - startPos)));
        bu.startS = startPos / rate;
        bu.lengthS = (endPos - startPos) / rate;
        double sumP = 0.0, peak = 0.0;
        for (std::size_t b = run.a; b < run.b; ++b) {
            sumP += pw[b];
            peak = std::max(peak, pw[b]);
        }
        bu.powerDbfs = toDb(sumP / static_cast<double>(run.b - run.a));
        bu.peakDbfs = toDb(peak);
        // power-weighted mean frequency over the run's samples
        std::complex<double> acc = 0.0;
        const std::size_t i0 = std::max<std::size_t>(1, run.a * W), i1 = run.b * W;
        for (std::size_t i = i0; i < i1; ++i) {
            acc += std::complex<double>(x[i]) * std::conj(std::complex<double>(x[i - 1]));
        }
        bu.freqHz = std::arg(acc) * rate / kTwoPi;
        r.bursts.push_back(bu);
    }
    r.dutyCycle = onSamples / static_cast<double>(nb * W);
    if (starts.size() >= 3) {
        std::vector<double> sp;
        for (std::size_t i = 1; i < starts.size(); ++i) { sp.push_back((starts[i] - starts[i - 1]) / rate); }
        std::vector<double> t = sp;
        const double med = percentileInPlace(t, 0.5);
        std::vector<double> dev;
        for (double v : sp) { dev.push_back(std::fabs(v - med)); }
        const double mad = percentileInPlace(dev, 0.5);
        r.repeatPeriodS = med;
        r.repeatJitter = med > 0.0 ? mad / med : kNaN;
    }
    r.valid = true;
    r.reason = why::kOk;
    return r;
}

}  // namespace cascade::sigmeas

#endif  // CASCADE_DSP_SIGMEAS_BURSTS_HPP
