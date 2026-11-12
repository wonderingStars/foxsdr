// sigmeas_modulation.hpp - FM DEVIATION METER and AM MODULATION-DEPTH METER on a selected,
// already-isolated channel (the caller crops the time span and shifts/filters the channel so that
// it is the only signal in the span; the carrier may be anywhere in the baseband).
// Row: "FM deviation meter and AM modulation-depth meter" (REMAINING-GAPS.md).
//
// DEFINITIONS
//  - FM peak frequency deviation: the largest excursion of the instantaneous frequency from the
//    carrier (the usual deviation-meter definition; no ITU number is cited because none was
//    opened). The instantaneous frequency is f[n] = arg(x[n] conj(x[n-1])) rate / 2 pi, low-passed to the audio bandwidth;
//    the carrier is its mean.
//  - AM modulation depth: m = (A_max - A_min) / (A_max + A_min) of the envelope |x| (the
//    usual "modulation factor"; not checked against a standard text); the positive and negative
//    peak depths (A_max/A_c - 1, 1 - A_min/A_c, A_c = the mean envelope) are reported too, and a
//    negative peak depth of 1 or more flags over-modulation.
//
// TWO ESTIMATES, because program material and a test tone want different ones.
//  - PEAK: the 0.1 / 99.9 percentiles of the filtered track (a single click does not set the
//    reading; a percentile of a sine is within 1e-5 of its amplitude). It reads NOISE as extra
//    excursion: at 35 dB SNR an AM depth of 0.10 reads 0.117 and a 12.5 kHz deviation reads
//    about 1-2 % high; at 60 dB SNR both are within 0.5 point / 1 %. Narrow `audioBandwidthHz`
//    to the audio actually modulated to cut that.
//  - TONE: the amplitude of the strongest spectral line of the track, from a least-squares sine
//    fit at the interpolated line frequency. It is the noise-immune reading and is used (`isTone`)
//    when that one line carries at least 90 % of the track's variance.
//  The headline `peakDeviationHz` / `depth` is the TONE value when the track is a tone and the
//  PEAK value otherwise; both are in the result.
//
// ACCURACY, pinned in tests/test_sigmeas_modulation.cpp on generated signals with noise (35 dB
// SNR), a carrier offset and a 200 ppm clock error: FM deviation within 1 % at 12.5 kHz and
// 25 kHz peak deviation (and at 2.5 kHz), AM depth within 0.5 point.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_DSP_SIGMEAS_MODULATION_HPP
#define CASCADE_DSP_SIGMEAS_MODULATION_HPP

#include <cstddef>
#include <vector>

#include "dsp/sigmeas_common.hpp"

namespace cascade::sigmeas {

struct ModParams {
    // Low-pass applied to the demodulated track. 0 = a fifth of the sample rate. A value at or
    // above 0.45 of the rate switches the filter off.
    double audioBandwidthHz = 0.0;
    double peakPercentile = 0.001;  // 0.001 = the 0.1 / 99.9 percentiles
};

struct FmResult {
    bool valid = false;
    const char* reason = why::kEmpty;
    double carrierOffsetHz = kNaN;   // mean instantaneous frequency
    double peakDeviationHz = kNaN;   // headline: tone amplitude if isTone, else the percentile peak
    double peakEstimateHz = kNaN;    // (p99.9 - p0.1) / 2
    double positivePeakHz = kNaN;
    double negativePeakHz = kNaN;
    double rmsDeviationHz = kNaN;
    double toneHz = kNaN;            // strongest modulating line
    double toneDeviationHz = kNaN;   // its amplitude
    double toneShare = 0.0;          // share of the track's variance in that line
    bool isTone = false;
};

struct AmResult {
    bool valid = false;
    const char* reason = why::kEmpty;
    double carrierLevel = kNaN;      // mean envelope
    double depth = kNaN;             // headline: tone depth if isTone, else peak depth (0..1+)
    double peakDepth = kNaN;         // (Amax - Amin) / (Amax + Amin)
    double positiveDepth = kNaN;     // Amax / carrier - 1
    double negativeDepth = kNaN;     // 1 - Amin / carrier
    double toneDepth = kNaN;
    double toneHz = kNaN;
    double toneShare = 0.0;
    bool isTone = false;
    bool overModulated = false;      // negative peaks reach the carrier's zero
};

namespace detail {

// Windowed-sinc low-pass (Hamming), odd length, unity DC gain; cutoff in cycles/sample.
inline std::vector<double> lowpassTaps(double cutoffCyc, std::size_t taps) {
    std::vector<double> h(taps);
    const double mid = static_cast<double>(taps - 1) / 2.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < taps; ++i) {
        const double t = static_cast<double>(i) - mid;
        const double s = t == 0.0 ? 2.0 * cutoffCyc : std::sin(kTwoPi * cutoffCyc * t) / (kPi * t);
        const double w = 0.54 - 0.46 * std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(taps - 1));
        h[i] = s * w;
        sum += h[i];
    }
    for (double& v : h) { v /= sum; }
    return h;
}

// Low-passes `v` in place and drops the filter's edge samples. Returns the number of samples
// removed from each end. cutoffHz <= 0 or >= 0.45 rate: unchanged.
inline std::size_t audioLowpass(std::vector<double>& v, double rate, double cutoffHz) {
    if (!(cutoffHz > 0.0) || cutoffHz >= 0.45 * rate) { return 0; }
    const double cyc = cutoffHz / rate;
    std::size_t taps = static_cast<std::size_t>(std::ceil(3.3 / (0.5 * cyc))) | 1u;  // ~ 3.3/transition
    taps = std::min<std::size_t>(std::max<std::size_t>(taps, 15), 511);
    if (v.size() <= 4 * taps) { return 0; }
    const std::vector<double> h = lowpassTaps(cyc, taps);
    const std::size_t half = taps / 2;
    const std::size_t outN = v.size() - 2 * half;
    std::vector<double> out(outN);
    for (std::size_t i = 0; i < outN; ++i) {
        double acc = 0.0;
        for (std::size_t j = 0; j < taps; ++j) { acc += h[j] * v[i + j]; }
        out[i] = acc;
    }
    v.swap(out);
    return half;
}

struct ToneFit {
    double freqHz = kNaN;
    double amp = 0.0;
    double share = 0.0;  // amp^2/2 over the variance
};

// Strongest spectral line of v (mean removed) and its amplitude from a projection at the
// refined frequency.
inline ToneFit fitTone(const std::vector<double>& v, double rate) {
    ToneFit t;
    const std::size_t n = v.size();
    if (n < 64) { return t; }
    const double mean = meanOf(v);
    double var = 0.0;
    for (double d : v) { var += (d - mean) * (d - mean); }
    var /= static_cast<double>(n);
    if (!(var > 0.0)) { return t; }
    std::size_t N = floorPow2(n);
    const std::vector<double> w = makeWindow(Win::Hann, N);
    std::vector<std::complex<double>> buf(N);
    for (std::size_t i = 0; i < N; ++i) { buf[i] = {(v[i] - mean) * w[i], 0.0}; }
    Fft(N).forward(buf.data());
    std::size_t best = 2;
    double bp = 0.0;
    for (std::size_t k = 2; k < N / 2; ++k) {
        const double p = std::norm(buf[k]);
        if (p > bp) { bp = p; best = k; }
    }
    // parabolic interpolation on the log magnitude
    double delta = 0.0;
    if (best > 2 && best + 1 < N / 2) {
        const double a = std::log(std::max(std::norm(buf[best - 1]), 1e-300));
        const double b = std::log(std::max(std::norm(buf[best]), 1e-300));
        const double c = std::log(std::max(std::norm(buf[best + 1]), 1e-300));
        const double den = a - 2 * b + c;
        if (den != 0.0) { delta = 0.5 * (a - c) / den; }
    }
    double fc = (static_cast<double>(best) + delta) / static_cast<double>(N);  // cycles/sample
    auto proj = [&](double f) {
        // |sum (v-mean) exp(-j 2 pi f n)| via a rotating phasor
        const double dph = -kTwoPi * f;
        std::complex<double> rot = std::polar(1.0, dph), z = 1.0, acc = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            acc += (v[i] - mean) * z;
            z *= rot;
            if ((i & 1023u) == 1023u) { z /= std::abs(z); }
        }
        return std::abs(acc);
    };
    // golden-section refinement within +-1 bin
    double lo = fc - 1.0 / static_cast<double>(N), hi = fc + 1.0 / static_cast<double>(N);
    const double gr = 0.6180339887498949;
    double x1 = hi - gr * (hi - lo), x2 = lo + gr * (hi - lo);
    double f1 = proj(x1), f2 = proj(x2);
    for (int it = 0; it < 24; ++it) {
        if (f1 > f2) { hi = x2; x2 = x1; f2 = f1; x1 = hi - gr * (hi - lo); f1 = proj(x1); }
        else { lo = x1; x1 = x2; f1 = f2; x2 = lo + gr * (hi - lo); f2 = proj(x2); }
    }
    fc = 0.5 * (lo + hi);
    // amplitude by least squares of A cos + B sin at fc (removes the image-frequency bias)
    double cc = 0, ss = 0, cs = 0, xc = 0, xs = 0;
    {
        const double dph = kTwoPi * fc;
        std::complex<double> rot = std::polar(1.0, dph), z = 1.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double c = z.real(), s = z.imag(), y = v[i] - mean;
            cc += c * c; ss += s * s; cs += c * s; xc += y * c; xs += y * s;
            z *= rot;
            if ((i & 1023u) == 1023u) { z /= std::abs(z); }
        }
    }
    const double det = cc * ss - cs * cs;
    if (!(det > 0.0)) { return t; }
    const double A = (xc * ss - xs * cs) / det;
    const double B = (xs * cc - xc * cs) / det;
    t.freqHz = fc * rate;
    t.amp = std::sqrt(A * A + B * B);
    t.share = std::min(1.0, 0.5 * t.amp * t.amp / var);
    return t;
}

}  // namespace detail

inline FmResult measureFmDeviation(const Cf* x, std::size_t n, double rate, const ModParams& prm = {}) {
    FmResult r;
    if (const char* bad = checkIq(x, n, rate, 512)) { r.reason = bad; return r; }
    if (!(prm.peakPercentile >= 0.0 && prm.peakPercentile < 0.25) ||
        !(prm.audioBandwidthHz >= 0.0) || !std::isfinite(prm.audioBandwidthHz)) {
        r.reason = why::kBadParam;
        return r;
    }
    // a carrier must be there: median magnitude over zero
    std::vector<double> mag(n);
    for (std::size_t i = 0; i < n; ++i) { mag[i] = std::abs(std::complex<double>(x[i])); }
    {
        std::vector<double> tmp = mag;
        const double med = percentileInPlace(tmp, 0.5);
        if (!(med > 1e-9)) { r.reason = why::kNoSignal; return r; }
        // samples in a deep fade carry noise phase: hold the last good value
        std::vector<double> f(n);
        instantaneousFrequency(x, n, rate, f.data());
        const double gate = 0.1 * med;
        for (std::size_t i = 1; i < n; ++i) {
            if (mag[i] < gate || mag[i - 1] < gate) { f[i] = f[i - 1]; }
        }
        const double bw = prm.audioBandwidthHz > 0.0 ? prm.audioBandwidthHz : 0.2 * rate;
        detail::audioLowpass(f, rate, bw);
        if (f.size() < 64) { r.reason = why::kTooShort; return r; }

        const double mean = meanOf(f);
        r.carrierOffsetHz = mean;
        double var = 0.0;
        for (double d : f) { var += (d - mean) * (d - mean); }
        r.rmsDeviationHz = std::sqrt(var / static_cast<double>(f.size()));
        std::vector<double> t1 = f, t2 = f;
        const double hi = percentileInPlace(t1, 1.0 - prm.peakPercentile);
        const double lo = percentileInPlace(t2, prm.peakPercentile);
        r.positivePeakHz = hi - mean;
        r.negativePeakHz = mean - lo;
        r.peakEstimateHz = 0.5 * (hi - lo);
        const detail::ToneFit tf = detail::fitTone(f, rate);
        r.toneHz = tf.freqHz;
        r.toneDeviationHz = tf.amp;
        r.toneShare = tf.share;
        r.isTone = tf.share >= 0.9;
        r.peakDeviationHz = r.isTone ? tf.amp : r.peakEstimateHz;
    }
    r.valid = true;
    r.reason = why::kOk;
    return r;
}

inline AmResult measureAmDepth(const Cf* x, std::size_t n, double rate, const ModParams& prm = {}) {
    AmResult r;
    if (const char* bad = checkIq(x, n, rate, 512)) { r.reason = bad; return r; }
    if (!(prm.peakPercentile >= 0.0 && prm.peakPercentile < 0.25) ||
        !(prm.audioBandwidthHz >= 0.0) || !std::isfinite(prm.audioBandwidthHz)) {
        r.reason = why::kBadParam;
        return r;
    }
    std::vector<double> e(n);
    for (std::size_t i = 0; i < n; ++i) { e[i] = std::abs(std::complex<double>(x[i])); }
    const double bw = prm.audioBandwidthHz > 0.0 ? prm.audioBandwidthHz : 0.2 * rate;
    detail::audioLowpass(e, rate, bw);
    if (e.size() < 64) { r.reason = why::kTooShort; return r; }
    const double carrier = meanOf(e);
    if (!(carrier > 1e-9)) { r.reason = why::kNoSignal; return r; }
    r.carrierLevel = carrier;
    std::vector<double> t1 = e, t2 = e;
    const double hi = percentileInPlace(t1, 1.0 - prm.peakPercentile);
    const double lo = percentileInPlace(t2, prm.peakPercentile);
    r.peakDepth = (hi + lo) > 0.0 ? (hi - lo) / (hi + lo) : 0.0;
    r.positiveDepth = hi / carrier - 1.0;
    r.negativeDepth = 1.0 - lo / carrier;
    r.overModulated = r.negativeDepth >= 0.99;
    const detail::ToneFit tf = detail::fitTone(e, rate);
    r.toneHz = tf.freqHz;
    r.toneDepth = tf.amp / carrier;
    r.toneShare = tf.share;
    r.isTone = tf.share >= 0.9;
    r.depth = r.isTone ? r.toneDepth : r.peakDepth;
    r.valid = true;
    r.reason = why::kOk;
    return r;
}

}  // namespace cascade::sigmeas

#endif  // CASCADE_DSP_SIGMEAS_MODULATION_HPP
