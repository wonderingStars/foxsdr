// sigmeas_histogram.hpp - AMPLITUDE and PROBABILITY HISTOGRAMS (Baudline's two views) of audio or
// I/Q, and the power CCDF of a complex signal. Row: "amplitude and probability histograms"
// (DEPTH-GAPS.md Part B, Baudline).
//
//  - `amplitudeHistogram` bins audio samples (signed) into `bins` equal bins over [lo, hi] (the data
//    range when NaN). Reported per bin: the count; the PROBABILITY (count over the number of finite
//    samples - the "probability histogram", summing to 1 with the out-of-range mass); the density
//    (probability per unit of amplitude); the cumulative distribution at each bin's upper edge
//    (including the mass below the range) and its complement (the CCDF). Also mean, rms, extremes,
//    the peak and the crest factor (peak / rms, dB). Samples below lo or above hi are counted apart
//    and are in no bin; non-finite samples are counted apart and are in no statistic.
//  - `magnitudeHistogram` does the same for |x| of I/Q samples (lo defaults to 0).
//  - `powerCcdf` gives, for a complex signal, the probability that the instantaneous power exceeds
//    the mean power by x dB, x = 0, step, 2 step ... maxDb: the curve a transmitter's peak-to-average
//    behaviour is read from (complex Gaussian noise gives exp(-10^(x/10)); a constant-envelope signal
//    gives 1 up to 0 dB and 0 above). PAPR is the peak power over the mean power, dB.
//
// Memory only; two passes over the input and nothing allocated inside them.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_DSP_SIGMEAS_HISTOGRAM_HPP
#define CASCADE_DSP_SIGMEAS_HISTOGRAM_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "dsp/sigmeas_common.hpp"

namespace cascade::sigmeas {

struct HistParams {
    std::size_t bins = 64;
    double lo = kNaN;   // NaN = the data minimum
    double hi = kNaN;   // NaN = the data maximum
};

struct HistResult {
    bool valid = false;
    const char* reason = why::kEmpty;
    double lo = kNaN, hi = kNaN, binWidth = kNaN;
    std::vector<std::uint64_t> counts;
    std::vector<double> probability;   // count / finite samples
    std::vector<double> density;       // probability / binWidth
    std::vector<double> cdf;           // P(x <= upper edge of the bin)
    std::vector<double> ccdf;          // 1 - cdf
    std::uint64_t total = 0;           // finite samples
    std::uint64_t belowRange = 0, aboveRange = 0, nonFinite = 0;
    double mean = kNaN, rms = kNaN, minValue = kNaN, maxValue = kNaN, peakAbs = kNaN;
    double crestFactorDb = kNaN;
};

namespace detail {

template <class Get>
HistResult histogramImpl(std::size_t n, const HistParams& prm, double defaultLo, bool useDefaultLo, Get get) {
    HistResult r;
    if (n == 0) { r.reason = why::kEmpty; return r; }
    if (prm.bins == 0 || prm.bins > 65536 || (std::isfinite(prm.lo) && std::isfinite(prm.hi) && !(prm.hi > prm.lo))) {
        r.reason = why::kBadParam;
        return r;
    }
    // pass 1: statistics and range
    double mn = 0.0, mx = 0.0, sum = 0.0, sumSq = 0.0, peak = 0.0;
    std::uint64_t finite = 0, bad = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = get(i);
        if (!std::isfinite(v)) { ++bad; continue; }
        if (finite == 0) { mn = mx = v; }
        mn = std::min(mn, v);
        mx = std::max(mx, v);
        sum += v;
        sumSq += v * v;
        peak = std::max(peak, std::fabs(v));
        ++finite;
    }
    r.nonFinite = bad;
    if (finite == 0) { r.reason = why::kNonFinite; return r; }
    r.total = finite;
    r.mean = sum / static_cast<double>(finite);
    r.rms = std::sqrt(sumSq / static_cast<double>(finite));
    r.minValue = mn;
    r.maxValue = mx;
    r.peakAbs = peak;
    r.crestFactorDb = r.rms > 0.0 ? 20.0 * std::log10(peak / r.rms) : kNaN;
    double lo = std::isfinite(prm.lo) ? prm.lo : (useDefaultLo ? defaultLo : mn);
    double hi = std::isfinite(prm.hi) ? prm.hi : mx;
    if (!(hi > lo)) { hi = lo + 1.0; }   // a constant signal: one unit-wide range
    r.lo = lo;
    r.hi = hi;
    r.binWidth = (hi - lo) / static_cast<double>(prm.bins);
    r.counts.assign(prm.bins, 0);
    // pass 2: bin
    for (std::size_t i = 0; i < n; ++i) {
        const double v = get(i);
        if (!std::isfinite(v)) { continue; }
        if (v < lo) { ++r.belowRange; continue; }
        if (v > hi) { ++r.aboveRange; continue; }
        std::size_t b = static_cast<std::size_t>((v - lo) / r.binWidth);
        if (b >= prm.bins) { b = prm.bins - 1; }   // v == hi goes in the last bin
        ++r.counts[b];
    }
    const double tot = static_cast<double>(finite);
    r.probability.resize(prm.bins);
    r.density.resize(prm.bins);
    r.cdf.resize(prm.bins);
    r.ccdf.resize(prm.bins);
    double cum = static_cast<double>(r.belowRange) / tot;
    for (std::size_t b = 0; b < prm.bins; ++b) {
        r.probability[b] = static_cast<double>(r.counts[b]) / tot;
        r.density[b] = r.probability[b] / r.binWidth;
        cum += r.probability[b];
        r.cdf[b] = std::min(1.0, cum);
        r.ccdf[b] = std::max(0.0, 1.0 - cum);
    }
    r.valid = true;
    r.reason = why::kOk;
    return r;
}

}  // namespace detail

inline HistResult amplitudeHistogram(const float* x, std::size_t n, const HistParams& prm = {}) {
    if (x == nullptr) { n = 0; }
    return detail::histogramImpl(n, prm, 0.0, false, [x](std::size_t i) { return static_cast<double>(x[i]); });
}

inline HistResult magnitudeHistogram(const Cf* x, std::size_t n, const HistParams& prm = {}) {
    if (x == nullptr) { n = 0; }
    return detail::histogramImpl(n, prm, 0.0, true, [x](std::size_t i) {
        const double re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) { return kNaN; }
        return std::sqrt(re * re + im * im);
    });
}

struct CcdfResult {
    bool valid = false;
    const char* reason = why::kEmpty;
    double stepDb = 0.0;
    std::vector<double> dbAboveMean;    // x axis
    std::vector<double> probability;    // P(power > mean * 10^(x/10))
    double meanPowerDbfs = kNaN;
    double peakPowerDbfs = kNaN;
    double paprDb = kNaN;
    std::uint64_t total = 0;
};

inline CcdfResult powerCcdf(const Cf* x, std::size_t n, double maxDb = 20.0, double stepDb = 0.5) {
    CcdfResult r;
    if (x == nullptr || n == 0) { r.reason = why::kEmpty; return r; }
    if (!(stepDb > 0.0) || !(maxDb >= stepDb) || !std::isfinite(maxDb) || !std::isfinite(stepDb) ||
        maxDb / stepDb > 100000.0) {
        r.reason = why::kBadParam;
        return r;
    }
    if (!allFinite(x, n)) { r.reason = why::kNonFinite; return r; }
    double sum = 0.0, peak = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double p = std::norm(std::complex<double>(x[i]));
        sum += p;
        peak = std::max(peak, p);
    }
    const double mean = sum / static_cast<double>(n);
    if (!(mean > 0.0)) { r.reason = why::kNoSignal; return r; }
    const std::size_t K = static_cast<std::size_t>(std::floor(maxDb / stepDb)) + 1;
    std::vector<std::uint64_t> hist(K + 1, 0);   // bucket k: x_k <= dB < x_{k+1}; K: above the last
    for (std::size_t i = 0; i < n; ++i) {
        const double p = std::norm(std::complex<double>(x[i]));
        if (p <= mean * 1e-12) { continue; }   // far under the mean: below every x >= 0, no bucket
        const double db = 10.0 * std::log10(p / mean);
        if (db < -1e-6) { continue; }   // under the mean: counted in no bucket
        const double dbc = std::max(db, 0.0);   // rounding on a constant envelope
        std::size_t k = static_cast<std::size_t>(dbc / stepDb);
        if (k > K) { k = K; }
        ++hist[k];
    }
    r.stepDb = stepDb;
    r.total = n;
    r.dbAboveMean.resize(K);
    r.probability.resize(K);
    std::uint64_t above = 0;   // samples with dB >= x_k
    for (std::size_t k = K + 1; k-- > 0;) {
        above += hist[k];
        if (k < K) {
            r.dbAboveMean[k] = static_cast<double>(k) * stepDb;
            r.probability[k] = static_cast<double>(above) / static_cast<double>(n);
        }
    }
    r.meanPowerDbfs = toDb(mean);
    r.peakPowerDbfs = toDb(peak);
    r.paprDb = r.peakPowerDbfs - r.meanPowerDbfs;
    r.valid = true;
    r.reason = why::kOk;
    return r;
}

}  // namespace cascade::sigmeas

#endif  // CASCADE_DSP_SIGMEAS_HISTOGRAM_HPP
