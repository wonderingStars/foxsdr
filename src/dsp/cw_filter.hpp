// CW audio peak filter, and a shape-factor design helper for the channel filter.
//
// 1. CwPeakFilter: an audio peaking filter for CW. Centre = the CW pitch (the sidetone / beat frequency the listener has
//    chosen; the wiring passes the same number it gives the demodulator), width 30 to 500 Hz, boost 0 to 24 dB. One section
//    of the W3C Audio EQ Cookbook peaking filter (audio_biquad.hpp) with Q = pitch / width. WIDTH MEANS: the distance between
//    the two frequencies where the boost has fallen to half its dB value (the cookbook's own bandwidth definition: for a
//    +12 dB boost, the 6 dB points). It boosts the wanted tone against everything else; it does not attenuate the rest (the
//    other side of the same job is the channel / IF filter). The pitch, width and boost glide (exponential, 10 ms time
//    constant, recomputed per 16 samples) so turning a knob or retuning the pitch cannot click. Off (disabled, or boost 0 dB
//    once settled) is a bit-exact no-op with no work. No allocation; non-finite input reads as 0; chunking does not change
//    the output. THREADS: setters from any thread (atomics); process() on the DSP thread only.
//
// 2. ChannelShape / designChannelFilter: taps for a linear-phase low-pass whose -6 dB bandwidth is wanted and whose SHAPE
//    FACTOR (width at -60 dB divided by width at -6 dB, the usual receiver-filter figure) is wanted too: sharp 1.3, medium
//    1.8, soft 3.16. Kaiser window (Kaiser 1974; Oppenheim and Schafer, Discrete-Time Signal Processing, 3rd ed., 7.5.3:
//    beta = 0.1102 (A - 8.7) for A > 50 and N - 1 = (A - 7.95) / (2.285 dw)), A = 65 dB so the sidelobes stay under -60 dB. The
//    closed-form length is only a first guess (it measures the pass-to-stop span, not the -6 to -60 span): the helper measures
//    the design's real -6 and -60 dB widths and rescales the transition and moves the sinc cutoff up to twelve times until the shape factor is within 2 %.
//    BELOW 2.6 the design is Kaiser as described. A Kaiser design cannot be made softer than about 2.2 (so short a
//    window that the -6 dB point leaves the cutoff: measured, the iteration settles at 2.23 whatever is asked), so FROM 2.6 UP the
//    helper builds a Gaussian filter instead: H(f) = exp(-a f^2) with a = ln 2 / fc^2 puts -6.02 dB at fc and -60 dB at
//    fc sqrt(ln(1e6) / ln 2) = 3.157 fc, a FIXED shape factor of 3.157 (the request is clamped to it; the taps are exp(-pi^2 k^2 /
//    (a fs^2)), cut where they fall under 1e-5 of the peak). Gaussian shape is the textbook minimum-ringing low-pass (no
//    overshoot in its step response). A tap cap (default 2047) is honoured and the result says so; the MEASURED shape factor is always returned. Control thread
//    only (it allocates the returned vector).
//
// Not wired. Header-only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "dsp/audio_biquad.hpp"

namespace cascade::dsp {

class CwPeakFilter {
public:
    static constexpr double kMinWidthHz = 30.0, kMaxWidthHz = 500.0, kMaxGainDb = 24.0;
    static constexpr double kMinPitchHz = 200.0, kMaxPitchHz = 2000.0;

    explicit CwPeakFilter(double sampleRateHz = 48000.0) : fs_(sampleRateHz > 4000.0 ? sampleRateHz : 48000.0) {
        alpha_ = 1.0 - std::exp(-16.0 / (0.010 * fs_));
        pitchCur_ = pitchTgt_ = 700.0;
        widthCur_ = widthTgt_ = 100.0;
    }

    void setEnabled(bool on) { enabled_.store(on); }
    void setPitchHz(double hz) {
        if (std::isfinite(hz)) { pitch_.store(std::min(std::max(hz, kMinPitchHz), std::min(kMaxPitchHz, 0.45 * fs_))); }
    }
    void setWidthHz(double hz) {
        if (std::isfinite(hz)) { width_.store(std::min(std::max(hz, kMinWidthHz), kMaxWidthHz)); }
    }
    void setGainDb(double db) {
        if (std::isfinite(db)) { gain_.store(std::min(std::max(db, 0.0), kMaxGainDb)); }
    }
    double pitchHz() const { return pitch_.load(); }
    double widthHz() const { return width_.load(); }
    double gainDb() const { return gain_.load(); }

    bool isBypassed() const { return idle_; }

    void process(float* x, std::size_t n) {
        pitchTgt_ = pitch_.load();
        widthTgt_ = width_.load();
        gainTgt_ = enabled_.load() ? gain_.load() : 0.0;
        if (idle_) {
            if (gainTgt_ == 0.0) { return; }
            idle_ = false;
            // Come up from the identity at the wanted pitch and width, so only the boost ramps.
            pitchCur_ = pitchTgt_;
            widthCur_ = widthTgt_;
            gainCur_ = 0.0;
            coef_ = abq::Coef{};
            st_ = abq::State{};
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (phase_ == 0) { step(); }
            phase_ = (phase_ + 1) & 15u;
            double v = static_cast<double>(x[i]);
            if (!std::isfinite(v)) { v = 0.0; }
            if (!idle_) { v = abq::run(coef_, st_, v); }
            x[i] = static_cast<float>(v);
        }
        if (!idle_ && gainCur_ == 0.0 && gainTgt_ == 0.0 && std::fabs(st_.z1) + std::fabs(st_.z2) < 1e-12) {
            idle_ = true;
            st_ = abq::State{};
        }
    }

    void reset() { st_ = abq::State{}; }

    // The boost, in dB, the filter has at `hz` with the settings in force now (what the screen draws).
    double responseDb(double hz) const { return abq::magnitudeDb(coef_, fs_, hz); }

private:
    void step() {
        if (idle_) { return; }
        auto glide = [this](double& cur, double tgt, double tol) {
            if (std::fabs(tgt - cur) <= tol) { cur = tgt; } else { cur += (tgt - cur) * alpha_; }
        };
        glide(pitchCur_, pitchTgt_, 0.01);
        glide(widthCur_, widthTgt_, 0.01);
        glide(gainCur_, gainTgt_, 0.001);
        coef_ = abq::peak(fs_, pitchCur_, pitchCur_ / widthCur_, gainCur_);
    }

    double fs_, alpha_;
    std::atomic<bool> enabled_{false};
    std::atomic<double> pitch_{700.0}, width_{100.0}, gain_{12.0};
    double pitchCur_, pitchTgt_, widthCur_, widthTgt_, gainCur_ = 0.0, gainTgt_ = 0.0;
    abq::Coef coef_;
    abq::State st_;
    bool idle_ = true;
    unsigned phase_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------------

enum class ChannelShape { Sharp, Medium, Soft };

inline double channelShapeFactor(ChannelShape s) {
    switch (s) {
        case ChannelShape::Sharp: return 1.3;
        case ChannelShape::Medium: return 1.8;
        case ChannelShape::Soft: return 3.157;
    }
    return 1.8;
}

// Response of real symmetric taps at `hz` in dB (relative to DC gain 1: the design normalises the sum to 1).
inline double tapsResponseDb(const std::vector<float>& h, double fs, double hz) {
    const double w = 2.0 * abq::kPi * hz / fs;
    const std::size_t mid = h.size() / 2;
    double s = 0.0;
    for (std::size_t k = 0; k < h.size(); ++k) {
        s += static_cast<double>(h[k]) * std::cos(w * (static_cast<double>(k) - static_cast<double>(mid)));
    }
    return 20.0 * std::log10(std::max(std::fabs(s), 1e-30));
}

struct ChannelDesign {
    std::vector<float> taps;
    double bw6Hz = 0;           // measured full width at -6 dB (two sides)
    double bw60Hz = 0;          // measured full width at -60 dB
    double shapeFactor = 0;     // bw60 / bw6, measured
    bool capped = false;        // the tap cap stopped the design reaching the wanted shape factor
    std::size_t delaySamples = 0;
};

// The frequency above `lo` where the response first falls to `levelDb`, bisected between steps of `step` Hz.
inline double firstCrossingHz(const std::vector<float>& h, double fs, double lo, double step, double levelDb) {
    double f = lo;
    const double top = 0.5 * fs;
    while (f < top && tapsResponseDb(h, fs, f) > levelDb) { f += step; }
    if (f >= top) { return top; }
    double a = std::max(lo, f - step), b = f;
    for (int i = 0; i < 40; ++i) {
        const double m = 0.5 * (a + b);
        if (tapsResponseDb(h, fs, m) > levelDb) { a = m; } else { b = m; }
    }
    return 0.5 * (a + b);
}

inline double besselI0Kaiser(double x) {
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 80; ++k) {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < 1e-17 * sum) { break; }
    }
    return sum;
}

// bw6Hz: wanted -6 dB bandwidth (full width, so the cutoff is bw6 / 2). Bad input (non-finite, bw6 <= 0 or >= 0.9 fs / 2
// ... ) gives an empty design. maxTaps is forced odd.
inline ChannelDesign designChannelFilter(double fs, double bw6Hz, double shapeFactor, std::size_t maxTaps = 2047) {
    ChannelDesign d;
    if (!(fs > 0) || !std::isfinite(bw6Hz) || !std::isfinite(shapeFactor) || bw6Hz <= 0.0) { return d; }
    const double fc = 0.5 * bw6Hz;
    shapeFactor = std::min(std::max(shapeFactor, 1.05), 20.0);
    if (shapeFactor > 2.0 && shapeFactor < 2.6) { shapeFactor = 2.0; }  // not offered: Kaiser stops at 2.0, Gaussian starts at 2.6
    if (fc * std::min(shapeFactor, 3.157) >= 0.48 * fs) { shapeFactor = std::min(shapeFactor, 0.48 * fs / fc); }
    if (shapeFactor <= 1.02) { return d; }
    if (maxTaps < 15) { maxTaps = 15; }
    if (maxTaps % 2 == 0) { --maxTaps; }
    if (shapeFactor >= 2.6) {
        // Gaussian: fixed shape factor 3.157. Half length 0.9 fs / fc covers exp(-11.5).
        std::size_t half = static_cast<std::size_t>(std::ceil(0.9 * fs / fc));
        d.capped = 2 * half + 1 > maxTaps;
        if (d.capped) { half = (maxTaps - 1) / 2; }
        if (half < 4) { half = 4; }
        const std::size_t n = 2 * half + 1;
        std::vector<double> tmp(n);
        double sum = 0.0;
        for (std::size_t k = 0; k < n; ++k) {
            const double m = static_cast<double>(k) - static_cast<double>(half);
            tmp[k] = std::exp(-abq::kPi * abq::kPi * m * m * fc * fc / (0.6931471805599453 * fs * fs));
            sum += tmp[k];
        }
        d.taps.resize(n);
        for (std::size_t k = 0; k < n; ++k) { d.taps[k] = static_cast<float>(tmp[k] / sum); }
        const double f6 = firstCrossingHz(d.taps, fs, 0.2 * fc, fc / 50.0, -6.0);
        const double f60 = firstCrossingHz(d.taps, fs, f6, fc / 100.0, -60.0);
        d.bw6Hz = 2.0 * f6;
        d.bw60Hz = 2.0 * f60;
        d.shapeFactor = d.bw60Hz / d.bw6Hz;
        d.delaySamples = half;
        return d;
    }
    const double A = 65.0;
    const double beta = 0.1102 * (A - 8.7);
    const double i0b = besselI0Kaiser(beta);
    double transition = (shapeFactor - 1.0) * fc;  // wanted -6 to -60 dB span, Hz
    double fcDesign = fc;                           // the sinc cutoff: moved until the MEASURED -6 dB point is where it is wanted
    for (int iter = 0; iter < 12; ++iter) {
        // First guess: treat the span as the Kaiser transition width.
        double nd = (A - 7.95) / (2.285 * 2.0 * abq::kPi * transition / fs) + 1.0;
        std::size_t n = static_cast<std::size_t>(std::ceil(nd));
        if (n < 9) { n = 9; }
        if (n % 2 == 0) { ++n; }
        d.capped = n > maxTaps;
        if (d.capped) { n = maxTaps; }
        const std::size_t mid = n / 2;
        std::vector<float> h(n);
        const double wc = 2.0 * abq::kPi * fcDesign / fs;
        double sum = 0.0;
        std::vector<double> tmp(n);
        for (std::size_t k = 0; k < n; ++k) {
            const double m = static_cast<double>(k) - static_cast<double>(mid);
            const double r = mid == 0 ? 0.0 : m / static_cast<double>(mid);
            const double w = besselI0Kaiser(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
            tmp[k] = (m == 0.0 ? wc / abq::kPi : std::sin(wc * m) / (abq::kPi * m)) * w;
            sum += tmp[k];
        }
        for (std::size_t k = 0; k < n; ++k) { h[k] = static_cast<float>(tmp[k] / sum); }
        const double f6 = firstCrossingHz(h, fs, 0.2 * fc, fc / 50.0, -6.0);
        const double f60 = firstCrossingHz(h, fs, f6, fc / 100.0, -60.0);
        d.taps = std::move(h);
        d.bw6Hz = 2.0 * f6;
        d.bw60Hz = 2.0 * f60;
        d.shapeFactor = d.bw60Hz / d.bw6Hz;
        d.delaySamples = mid;
        if (d.capped || (std::fabs(d.shapeFactor / shapeFactor - 1.0) < 0.02 && std::fabs(f6 / fc - 1.0) < 0.01)) { break; }
        // Rescale the transition by the ratio of the wanted -6 to -60 span to the measured one, and move the cutoff so the
        // measured -6 dB point lands on the wanted one (a very soft filter has so short a window that the sinc cutoff and the
        // -6 dB point part company).
        const double got = f60 - f6, want = (shapeFactor - 1.0) * fc;
        if (!(got > 0) || !(f6 > 0)) { break; }
        transition *= want / got;
        fcDesign *= fc / f6;
    }
    return d;
}

inline ChannelDesign designChannelFilter(double fs, double bw6Hz, ChannelShape s, std::size_t maxTaps = 2047) {
    return designChannelFilter(fs, bw6Hz, channelShapeFactor(s), maxTaps);
}

}  // namespace cascade::dsp
