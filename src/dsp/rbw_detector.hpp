// rbw_detector.hpp - RBW / VBW readout and a selectable DETECTOR for a spectrum trace (peak, negative peak, RMS, voltage average, log average,
// sample, quasi-peak), computed from a stream of FFT power frames.
//
// STAND-ALONE (lane rfmon): nothing in the engine uses it yet. It needs no FFT of its own: it takes frames of per-bin power
// density, the way spectrum_block (branch feat/signal-blocks-3, SpecScale::Psd) produces them, so it can sit behind either spectrum path.
//
// THE FRAME CONTRACT. A frame is n floats, bin 0 the LOWEST frequency (the caller has already put DC in the middle), each the power
// spectral density of that bin in W/Hz (or any consistent unit per Hz): |X|^2 / (fs * sum w^2) for a window w of length n. With that
// calibration, white noise of density D reads D in every bin whatever the window, and a tone of power P on a bin centre reads
// P / ENBW_Hz in that bin, so "density times a bandwidth" is a power for noise and for tones alike. rbwBinPsd() does the conversion.
//
// RBW. The window's equivalent noise bandwidth is ENBW = fs * sum(w^2) / (sum w)^2 (in bins: n * sum(w^2) / (sum w)^2; the definition used
// by Heinzel, Ruediger, Schilling 2002 and Harris 1978; 1.0 for a rectangular window and 1.5 for Hann are the values printed by Audio
// Precision's note, both pinned in the test). For the cosine-sum windows the sums are closed forms of the coefficients, so rbwEnbwBins() is
// exact and needs no n. The readout is this ENBW; a wider RBW is made by summing M adjacent bins of power (a rectangular video of M bins):
// the average noise reading is then exactly D * M * binHz, and a tone reads its true power once the M bins cover its main lobe, which
// rbwToneLossDb() computes from the window itself (worst case over the tone's position between bins), so a readout that would under-read a
// tone says so. A request finer than the frame's ENBW is refused (clamped to it) and flagged: only a longer FFT gives a finer RBW,
// rbwFftSizeFor() says which.
//
// DETECTORS (per display point, over every bin that maps to the point and every frame pushed since the trace was last taken):
//   Peak      largest power         NegPeak   smallest power        Sample   the bin under the point's centre in the LAST frame
//   Rms       mean of POWER (the root-mean-square of the voltage, 10 log10 of the mean power)
//   AvgVolt   (mean of sqrt(power))^2, the voltage ("linear") average
//   AvgLog    mean of the dB values (a log-averaging analyser)
// For white complex Gaussian noise the per-bin power is exponential with mean mu, so the closed forms the test pins are:
// AvgVolt = mu * pi/4 (-1.05 dB below Rms), AvgLog = mu * exp(-gamma) (-2.51 dB below Rms), E[max of K] = mu * H_K. For a steady tone every detector
// reads the same.
//
// VBW is a video low-pass on the successive TRACES: one pole in dB (log video) or in power, alpha = 1 - exp(-2 pi VBW dt) with dt the interval
// between traces; for iid input the variance falls by alpha/(2-alpha), which the test pins.
//
// QUASI-PEAK. Time constants (charge / discharge / meter) per CISPR band, as tabulated by emccalc.com/articles/detector (opened 2026-11-11):
//   Band A 9-150 kHz: 45 ms / 500 ms / 160 ms     Band B 150 kHz-30 MHz: 1 ms / 160 ms / 160 ms     Band C/D 30 MHz-1 GHz: 1 ms / 550 ms / 100 ms
// (the charge / discharge numbers also on rfessentials.com/resources/rf-glossary/cispr-detector, opened the same day; the meter number
// is from the first page only). The model is an envelope follower that charges with the charge time constant and discharges with the
// discharge time constant, read through a one-pole meter of the meter time constant. A constant carrier reads its own level (the detector is
// calibrated that way, and the pages say CW reads the same on QP, peak and average); a pulse train reads lower the lower its repetition rate.
// WHAT IS NOT CLAIMED: conformance to the CISPR 16-1-1 pulse-weighting calibration curves and the meter's critical damping (a second-order
// meter) - those are in the paid standard, so this is the published-time-constant model, not a certified receiver. The detector needs a
// frame period no longer than half the charge constant (qpFramePeriodOk()).
//
// THREADS: none of these types lock; one thread owns an object. No allocation after construction except configure().
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace cascade::dsp {

enum class RbwWindow { Rectangular, Hann, Hamming, BlackmanHarris, FlatTop };
enum class RbwDetector { Peak, NegPeak, Rms, AvgVolt, AvgLog, Sample };
enum class VbwDomain { Log, Power };

inline constexpr double kRbwTwoPi = 6.283185307179586476925286766559;
inline constexpr float kRbwFloorPower = 1e-30f;   // what a zero, negative or non-finite bin counts as
inline constexpr float kRbwFloorDb = -300.0f;     // 10 log10 of the floor
inline constexpr std::size_t kRbwMaxPoints = 16384;
inline constexpr std::size_t kRbwMaxBins = 1u << 20;

inline const char* rbwWindowName(RbwWindow w) {
    switch (w) {
        case RbwWindow::Rectangular: return "rectangular";
        case RbwWindow::Hann: return "hann";
        case RbwWindow::Hamming: return "hamming";
        case RbwWindow::BlackmanHarris: return "blackman-harris";
        case RbwWindow::FlatTop: return "flat-top";
    }
    return "?";
}
inline const char* rbwDetectorName(RbwDetector d) {
    switch (d) {
        case RbwDetector::Peak: return "peak";
        case RbwDetector::NegPeak: return "negative peak";
        case RbwDetector::Rms: return "rms";
        case RbwDetector::AvgVolt: return "average (voltage)";
        case RbwDetector::AvgLog: return "average (log)";
        case RbwDetector::Sample: return "sample";
    }
    return "?";
}

// Cosine-sum coefficients a_k of w[n] = sum_k (-1)^k a_k cos(2 pi k n / N) (periodic). Same as the spectrum block's tables.
inline int rbwCoefficients(RbwWindow w, double* a) {
    switch (w) {
        case RbwWindow::Rectangular: a[0] = 1.0; return 1;
        case RbwWindow::Hann: a[0] = 0.5; a[1] = 0.5; return 2;
        case RbwWindow::Hamming: a[0] = 0.54; a[1] = 0.46; return 2;
        case RbwWindow::BlackmanHarris: a[0] = 0.35875; a[1] = 0.48829; a[2] = 0.14128; a[3] = 0.01168; return 4;
        case RbwWindow::FlatTop:
            a[0] = 0.21557895; a[1] = 0.41663158; a[2] = 0.277263158; a[3] = 0.083578947; a[4] = 0.006947368; return 5;
    }
    a[0] = 1.0;
    return 1;
}

// Periodic window of n points into out.
inline void rbwFillWindow(RbwWindow w, std::size_t n, float* out) {
    double a[5];
    const int terms = rbwCoefficients(w, a);
    for (std::size_t i = 0; i < n; ++i) {
        double v = 0.0;
        const double x = kRbwTwoPi * static_cast<double>(i) / static_cast<double>(n);
        for (int k = 0; k < terms; ++k) { v += ((k & 1) != 0 ? -1.0 : 1.0) * a[k] * std::cos(k * x); }
        out[i] = static_cast<float>(v);
    }
}

// Equivalent noise bandwidth in BINS, exact: sum w = N a0, sum w^2 = N (a0^2 + sum_{k>=1} a_k^2 / 2).
inline double rbwEnbwBins(RbwWindow w) {
    double a[5];
    const int terms = rbwCoefficients(w, a);
    double s2 = a[0] * a[0];
    for (int k = 1; k < terms; ++k) { s2 += 0.5 * a[k] * a[k]; }
    return s2 / (a[0] * a[0]);
}

// ENBW and bin width in Hz for an FFT of n points at sample rate fs. Zero when the arguments are unusable.
inline double rbwBinHz(double fs, std::size_t n) { return (n > 0 && fs > 0.0 && std::isfinite(fs)) ? fs / static_cast<double>(n) : 0.0; }
inline double rbwEnbwHz(RbwWindow w, double fs, std::size_t n) { return rbwEnbwBins(w) * rbwBinHz(fs, n); }

// The smallest power-of-two FFT length in [minN, maxN] whose ENBW does not exceed `wantedHz`. When even maxN is too coarse, maxN is
// returned with reachable = false; when minN is already finer than wanted, minN is returned (combine bins to widen it: rbwPlan()).
struct RbwFftChoice {
    std::size_t n = 0;
    double rbwHz = 0.0;
    bool reachable = false;   // false: the wanted RBW is finer than maxN can give (n = maxN) or the arguments were unusable
};
inline RbwFftChoice rbwFftSizeFor(RbwWindow w, double fs, double wantedHz, std::size_t minN = 64, std::size_t maxN = 1u << 20) {
    RbwFftChoice c;
    if (!(fs > 0.0) || !std::isfinite(fs) || !(wantedHz > 0.0) || !std::isfinite(wantedHz) || minN < 2 || maxN < minN) { return c; }
    std::size_t n = 1;
    while (n < minN) { n <<= 1; }
    while (n < maxN && rbwEnbwHz(w, fs, n) > wantedHz) { n <<= 1; }
    c.n = std::min(n, maxN);
    c.rbwHz = rbwEnbwHz(w, fs, c.n);
    c.reachable = c.rbwHz <= wantedHz;
    return c;
}

// |W(nu)|^2 / |W(0)|^2 of the window at nu bins (fractional), for a canonical 128-point periodic window (the shape of these windows does not
// depend on n beyond the leakage of the sampling, which is negligible at 128 for the lobes that matter).
inline double rbwWindowResponse(RbwWindow w, double nuBins) {
    constexpr std::size_t kN = 128;
    static thread_local RbwWindow cached = RbwWindow::Rectangular;
    static thread_local bool have = false;
    static thread_local float win[kN];
    static thread_local double sum = 0.0;
    if (!have || cached != w) {
        rbwFillWindow(w, kN, win);
        sum = 0.0;
        for (std::size_t i = 0; i < kN; ++i) { sum += win[i]; }
        cached = w;
        have = true;
    }
    double re = 0.0, im = 0.0;
    for (std::size_t i = 0; i < kN; ++i) {
        const double ph = -kRbwTwoPi * nuBins * static_cast<double>(i) / static_cast<double>(kN);
        re += win[i] * std::cos(ph);
        im += win[i] * std::sin(ph);
    }
    return (re * re + im * im) / (sum * sum);
}

// How many dB BELOW its true power a tone reads in the combined trace of M adjacent bins (M = 1: the native single-bin reading), worst case
// over the tone's position between two bin centres. 0 when the readout covers the whole lobe; positive when it is too narrow for this
// window. m < 1 is treated as 1.
inline double rbwToneLossDb(RbwWindow w, std::size_t m) {
    if (m < 1) { m = 1; }
    const double enbw = rbwEnbwBins(w);
    const int lo = -static_cast<int>(m / 2);
    const int hi = static_cast<int>((m - 1) / 2);
    double worst = 1e300;
    for (int s = -10; s <= 10; ++s) {
        const double delta = 0.05 * s;                    // tone offset from the centre bin, in bins
        double sum = 0.0;
        for (int j = lo; j <= hi; ++j) { sum += rbwWindowResponse(w, static_cast<double>(j) - delta); }
        worst = std::min(worst, sum);
    }
    // One bin is read as psd * ENBW (a tone reads |W(delta)|^2: the familiar scalloping loss); M > 1 bins are summed (a tone reads
    // sum |W|^2 / ENBW). Both are 1 when the lobe is wholly inside the readout.
    const double ratio = std::min(1.0, m == 1 ? worst : worst / enbw);
    return ratio > 0.0 ? -10.0 * std::log10(ratio) : 200.0;
}

// What the user asked for and what is shown.
struct RbwPlan {
    std::size_t combine = 1;     // M: adjacent bins summed (1 = none: the native ENBW is the RBW)
    double rbwHz = 0.0;          // the readout: M * binHz when M > 1, else the window's ENBW
    double toneLossDb = 0.0;     // worst-case under-read of a tone at this readout
    bool clamped = false;        // the request was finer than the native ENBW (shown: the native one) or unusable
    bool valid = false;
};
inline RbwPlan rbwPlan(RbwWindow w, double fs, std::size_t n, double wantedHz) {
    RbwPlan p;
    const double bin = rbwBinHz(fs, n);
    if (bin <= 0.0) { return p; }
    p.valid = true;
    const double enbw = rbwEnbwHz(w, fs, n);
    if (!(wantedHz > 0.0) || !std::isfinite(wantedHz) || wantedHz <= enbw) {
        p.rbwHz = enbw;
        p.clamped = (wantedHz != 0.0) && !(wantedHz == enbw);
        p.toneLossDb = rbwToneLossDb(w, 1);
        return p;
    }
    const double mReal = wantedHz / bin;
    p.combine = static_cast<std::size_t>(std::max(1.0, std::min(std::round(mReal), static_cast<double>(n))));
    if (p.combine <= 1) {
        p.combine = 1;
        p.rbwHz = enbw;
        p.toneLossDb = rbwToneLossDb(w, 1);
        return p;
    }
    p.rbwHz = static_cast<double>(p.combine) * bin;
    p.toneLossDb = rbwToneLossDb(w, p.combine);
    return p;
}

// Raw |X|^2 of an FFT of n points through the window -> W/Hz. sumSq = sum of w^2 over the frame.
inline float rbwBinPsd(float rawPower, double fs, double sumSq) {
    if (!(fs > 0.0) || !(sumSq > 0.0)) { return 0.0f; }
    return static_cast<float>(static_cast<double>(rawPower) / (fs * sumSq));
}

inline double rbwDb(double p) { return 10.0 * std::log10(std::max(p, static_cast<double>(kRbwFloorPower))); }

struct RbwTraceConfig {
    RbwWindow window = RbwWindow::BlackmanHarris;
    double sampleRateHz = 0.0;       // fs of the FFT input
    std::size_t bins = 0;            // frame length n
    double wantedRbwHz = 0.0;        // 0: the native ENBW
    std::size_t points = 0;          // display points; 0 = one per bin
    RbwDetector detector = RbwDetector::Rms;
    double vbwHz = 0.0;              // 0: no video filter
    VbwDomain vbwDomain = VbwDomain::Log;
};

class RbwTrace {
public:
    // Control thread. Returns false (and keeps the object unusable) when the configuration cannot work.
    bool configure(const RbwTraceConfig& c) {
        valid_ = false;
        cfg_ = c;
        if (c.bins < 2 || c.bins > kRbwMaxBins || !(c.sampleRateHz > 0.0) || !std::isfinite(c.sampleRateHz)) { return false; }
        if (c.points > kRbwMaxPoints) { return false; }
        points_ = c.points == 0 ? std::min(c.bins, kRbwMaxPoints) : c.points;
        plan_ = rbwPlan(c.window, c.sampleRateHz, c.bins, c.wantedRbwHz);
        if (!plan_.valid) { return false; }
        binHz_ = rbwBinHz(c.sampleRateHz, c.bins);
        enbwHz_ = rbwEnbwHz(c.window, c.sampleRateHz, c.bins);
        rbw_.assign(c.bins, 0.0f);
        cum_.assign(c.bins + 1, 0.0);
        acc_.assign(points_, Acc{});
        video_.assign(points_, 0.0f);
        haveVideo_ = false;
        frames_ = 0;
        valid_ = true;
        return true;
    }

    bool valid() const { return valid_; }
    const RbwPlan& plan() const { return plan_; }
    std::size_t points() const { return points_; }
    std::size_t framesInTrace() const { return frames_; }

    // One frame of n W/Hz values. Frames of any other length are ignored (returns false).
    bool pushFrame(const float* psd, std::size_t n) {
        if (!valid_ || psd == nullptr || n != cfg_.bins) { return false; }
        // Sliding sum over the combined bins (prefix sums in double so a long span stays exact).
        const std::size_t m = plan_.combine;
        cum_[0] = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double v = (std::isfinite(psd[i]) && psd[i] > 0.0f) ? static_cast<double>(psd[i]) : 0.0;
            cum_[i + 1] = cum_[i] + v;
        }
        if (m <= 1) {
            for (std::size_t i = 0; i < n; ++i) { rbw_[i] = static_cast<float>((cum_[i + 1] - cum_[i]) * enbwHz_); }
        } else {
            const long lo = -static_cast<long>(m / 2);
            const long hi = static_cast<long>((m - 1) / 2);
            for (std::size_t i = 0; i < n; ++i) {
                const long a = std::max<long>(0, static_cast<long>(i) + lo);
                const long b = std::min<long>(static_cast<long>(n) - 1, static_cast<long>(i) + hi);
                // Near the edges fewer than m bins exist: scale up so a flat spectrum stays flat.
                const double sum = cum_[static_cast<std::size_t>(b) + 1] - cum_[static_cast<std::size_t>(a)];
                const double have = static_cast<double>(b - a + 1);
                rbw_[i] = static_cast<float>(sum * (static_cast<double>(m) / have) * binHz_);
            }
        }
        for (std::size_t p = 0; p < points_; ++p) {
            std::size_t a = p * n / points_;
            std::size_t b = std::max(a + 1, (p + 1) * n / points_);
            if (b > n) { b = n; }
            Acc& ac = acc_[p];
            for (std::size_t i = a; i < b; ++i) {
                const double v = std::max(static_cast<double>(rbw_[i]), static_cast<double>(kRbwFloorPower));
                ac.max = std::max(ac.max, v);
                ac.min = std::min(ac.min, v);
                ac.sum += v;
                ac.sumRoot += std::sqrt(v);
                ac.sumDb += rbwDb(v);
                ++ac.count;
            }
            const std::size_t centre = std::min(n - 1, (2 * p + 1) * n / (2 * points_));
            ac.sample = std::max(static_cast<double>(rbw_[centre]), static_cast<double>(kRbwFloorPower));
        }
        ++frames_;
        return true;
    }

    // Finishes the trace (dB per display point, in the unit the frames were in times Hz: for W/Hz input, dBW), starts the next one and runs
    // the video filter with `dtS` seconds since the previous trace. Returns false when no frame was pushed (out is left alone).
    bool takeTrace(float* outDb, std::size_t capacity, double dtS) {
        if (!valid_ || frames_ == 0 || outDb == nullptr || capacity < points_) { return false; }
        const double alpha = vbwAlpha(cfg_.vbwHz, dtS);
        for (std::size_t p = 0; p < points_; ++p) {
            const Acc& a = acc_[p];
            double val = 0.0;   // power
            double db = 0.0;
            switch (cfg_.detector) {
                case RbwDetector::Peak: val = a.max; db = rbwDb(val); break;
                case RbwDetector::NegPeak: val = a.min; db = rbwDb(val); break;
                case RbwDetector::Rms: val = a.sum / static_cast<double>(a.count); db = rbwDb(val); break;
                case RbwDetector::AvgVolt: {
                    const double r = a.sumRoot / static_cast<double>(a.count);
                    val = r * r;
                    db = rbwDb(val);
                    break;
                }
                case RbwDetector::AvgLog: db = a.sumDb / static_cast<double>(a.count); val = std::pow(10.0, db / 10.0); break;
                case RbwDetector::Sample: val = a.sample; db = rbwDb(val); break;
            }
            float shown;
            if (alpha >= 1.0 || !haveVideo_) {
                shown = static_cast<float>(cfg_.vbwDomain == VbwDomain::Log ? db : val);
            } else if (cfg_.vbwDomain == VbwDomain::Log) {
                shown = static_cast<float>(video_[p] + alpha * (db - video_[p]));
            } else {
                shown = static_cast<float>(video_[p] + alpha * (val - video_[p]));
            }
            video_[p] = shown;
            outDb[p] = cfg_.vbwDomain == VbwDomain::Log ? shown : static_cast<float>(rbwDb(shown));
        }
        haveVideo_ = true;
        for (auto& a : acc_) { a = Acc{}; }
        frames_ = 0;
        return true;
    }

    // Forgets the video filter's memory (the next trace passes through).
    void resetVideo() { haveVideo_ = false; }

    // alpha = 1 - exp(-2 pi vbw dt); 1 (no filtering) when the VBW is off, unusable, or wider than the trace rate can show.
    static double vbwAlpha(double vbwHz, double dtS) {
        if (!(vbwHz > 0.0) || !std::isfinite(vbwHz) || !(dtS > 0.0) || !std::isfinite(dtS)) { return 1.0; }
        return 1.0 - std::exp(-kRbwTwoPi * vbwHz * dtS);
    }

private:
    struct Acc {
        double max = 0.0;
        double min = std::numeric_limits<double>::max();
        double sum = 0.0;
        double sumRoot = 0.0;
        double sumDb = 0.0;
        double sample = 0.0;
        std::size_t count = 0;
    };
    RbwTraceConfig cfg_{};
    RbwPlan plan_{};
    bool valid_ = false;
    bool haveVideo_ = false;
    std::size_t points_ = 0;
    std::size_t frames_ = 0;
    double binHz_ = 0.0;
    double enbwHz_ = 0.0;
    std::vector<float> rbw_;
    std::vector<double> cum_;
    std::vector<Acc> acc_;
    std::vector<float> video_;
};

// ----- Quasi-peak -----------------------------------------------------------------------------------------------------------------------

enum class CisprBand { A, B, CD };

struct QpTimes {
    double chargeS = 0.0;
    double dischargeS = 0.0;
    double meterS = 0.0;
};
inline QpTimes qpTimesFor(CisprBand b) {
    switch (b) {
        case CisprBand::A: return {0.045, 0.500, 0.160};
        case CisprBand::B: return {0.001, 0.160, 0.160};
        case CisprBand::CD: return {0.001, 0.550, 0.100};
    }
    return {};
}
inline const char* cisprBandName(CisprBand b) {
    switch (b) {
        case CisprBand::A: return "A (9-150 kHz)";
        case CisprBand::B: return "B (150 kHz-30 MHz)";
        case CisprBand::CD: return "C/D (30 MHz-1 GHz)";
    }
    return "?";
}
// The detector samples the envelope once per frame: the frame period must be no more than half the charge time constant.
inline bool qpFramePeriodOk(CisprBand b, double framePeriodS) {
    return framePeriodS > 0.0 && std::isfinite(framePeriodS) && framePeriodS <= 0.5 * qpTimesFor(b).chargeS;
}

class QuasiPeakDetector {
public:
    // `points` independent channels (one per display point), one amplitude per channel per step. Returns false on an unusable setup.
    bool configure(CisprBand band, double framePeriodS, std::size_t points) {
        valid_ = false;
        if (!qpFramePeriodOk(band, framePeriodS) || points == 0 || points > kRbwMaxPoints) { return false; }
        const QpTimes t = qpTimesFor(band);
        aCharge_ = 1.0 - std::exp(-framePeriodS / t.chargeS);
        aDischarge_ = 1.0 - std::exp(-framePeriodS / t.dischargeS);
        aMeter_ = 1.0 - std::exp(-framePeriodS / t.meterS);
        det_.assign(points, 0.0);
        meter_.assign(points, 0.0);
        valid_ = true;
        return true;
    }
    bool valid() const { return valid_; }
    std::size_t points() const { return det_.size(); }
    void reset() { std::fill(det_.begin(), det_.end(), 0.0); std::fill(meter_.begin(), meter_.end(), 0.0); }

    // One envelope amplitude per point (linear, not dB); negative or non-finite counts as zero. Writes the meter reading per point.
    bool step(const float* amplitude, std::size_t n, float* meterOut) {
        if (!valid_ || amplitude == nullptr || meterOut == nullptr || n != det_.size()) { return false; }
        for (std::size_t i = 0; i < n; ++i) {
            const double x = (std::isfinite(amplitude[i]) && amplitude[i] > 0.0f) ? static_cast<double>(amplitude[i]) : 0.0;
            double& y = det_[i];
            y += (x - y) * (x > y ? aCharge_ : aDischarge_);
            meter_[i] += (y - meter_[i]) * aMeter_;
            meterOut[i] = static_cast<float>(meter_[i]);
        }
        return true;
    }
    // The detector capacitor (before the meter), for the closed-form test.
    double detectorValue(std::size_t i) const { return i < det_.size() ? det_[i] : 0.0; }

private:
    bool valid_ = false;
    double aCharge_ = 0.0, aDischarge_ = 0.0, aMeter_ = 0.0;
    std::vector<double> det_;
    std::vector<double> meter_;
};

}  // namespace cascade::dsp
