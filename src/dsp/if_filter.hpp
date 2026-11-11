// if_filter.hpp - the patch canvas's IF FILTER NODE: a complex (I/Q) band-pass whose two EDGES and up to eight NOTCHES are defined in RF Hz, and
// that keeps them on the RF frequency when the radio retunes. Not wired (a node kind in patch_graph.hpp and a runner call are forbidden files for
// this lane): the hand-off in docs/feature-records/dspmodels.md says what the node needs.
//
// WHAT IT IS.
//   - PASSBAND: edges lowRfHz .. highRfHz. At the radio's centre fc the baseband edges are lo - fc and hi - fc, so the band need not be symmetrical
//     (a 3 kHz SSB window sits on one side of DC). It is built the cheap exact way: rotate the I/Q by -fm (fm = the band's middle, relative to fc),
//     low-pass with a 4th-order Butterworth (two biquads, Q 0.5412 and 1.3066, the textbook 1 / (2 cos(pi/8)) and 1 / (2 cos(3 pi/8)), formulas
//     from dsp/audio_biquad.hpp = the W3C Audio EQ Cookbook) of cut-off = half the width, I and Q alike, and rotate back by +fm. -3.01 dB at each edge.
//   - NOTCH i: a centre RF frequency and a -3 dB width: rotate by -(fn - fc), apply H(z) = (1 - z^-1) / (1 - r z^-1) (a zero ON the unit circle at
//     DC, a pole just inside it, r = 1 - pi * width / fs), rotate back. Unity gain away from the notch, infinite depth at its centre.
//   - Neither uses a look-up table; nothing is copied from another program.
//
// KEEPING THE EDGES ON THE RF FREQUENCY. setCentre(newCentreHz) (the retune function) changes ONLY the rotation frequencies: the cut-off and the
// notch widths are in Hz, so no coefficient moves, and the oscillators keep their phase. In the rotated frame a signal at a fixed RF frequency sits
// at the same place before and after the retune (b - fm = f - mid), so the filter state is already right for it: nothing restarts, nothing clicks,
// and the tests hold the modulus of a continuous complex tone constant to within 1 % across the retune.
// What can change on a retune: an edge or a notch that is carried off the visible span (|offset| beyond 45 % of the sample rate).
//   - an edge past the span is CLAMPED to it: the width changes, so the new cut-off is CROSS-FADED in over 10 ms (the old and the new filter run side
//     by side; the new one starts from a copy of the old one's state); a second change during a fade waits for it to end;
//   - a band wholly outside the span passes nothing (its output gain fades to 0 over 10 ms, and back when it returns);
//   - a notch outside the span is switched off by a 10 ms fade (and on again by one).
// Switching the band or a notch on or off is the same 10 ms fade between the filtered and the unfiltered signal; with everything off and settled
// process() returns at once (zero cost, samples bit-exact).
//
// NOTCH WIDTH changes glide: r moves linearly over 10 ms. NO ALLOCATION (fixed arrays). Non-finite input samples read as 0. Any chunking of the input
// gives the same output (the oscillators renormalise on the absolute sample count, not per call). ONE THREAD: every call, setters included, is made
// from the DSP thread (the engine posts changes to it, as for the other blocks); there is no locking.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>

#include "dsp/audio_biquad.hpp"

namespace cascade::dsp {

class IfFilter {
public:
    using Cf = std::complex<float>;
    static constexpr int kMaxNotches = 8;
    static constexpr double kSpanFraction = 0.45;      // edges and notches live inside +-45 % of the sample rate
    static constexpr double kMinBandwidthHz = 10.0;
    static constexpr double kMinNotchWidthHz = 5.0;
    static constexpr double kFadeSeconds = 0.010;

    explicit IfFilter(double sampleRateHz = 48000.0) : fs_(sampleRateHz > 1000.0 ? sampleRateHz : 48000.0) {
        fadeStep_ = 1.0 / (kFadeSeconds * fs_);
        applyBand(true);
    }

    double sampleRateHz() const { return fs_; }
    double centreHz() const { return fc_; }

    // The radio's centre frequency in RF Hz (the retune function). Cheap: no coefficient is recomputed unless an edge reaches the span's end.
    void retune(double centreRfHz) {
        if (!std::isfinite(centreRfHz)) { return; }
        fc_ = centreRfHz;
        applyBand(false);
        for (int i = 0; i < kMaxNotches; ++i) { applyNotch(i, false); }
    }
    void setCentre(double centreRfHz) { retune(centreRfHz); }

    // The pass-band edges in RF Hz (swapped if given the wrong way round; a width under kMinBandwidthHz is widened about its middle).
    void setBand(double lowRfHz, double highRfHz, bool enabled = true) {
        if (!std::isfinite(lowRfHz) || !std::isfinite(highRfHz)) { return; }
        loRf_ = std::min(lowRfHz, highRfHz);
        hiRf_ = std::max(lowRfHz, highRfHz);
        if (hiRf_ - loRf_ < kMinBandwidthHz) {
            const double mid = 0.5 * (loRf_ + hiRf_);
            loRf_ = mid - 0.5 * kMinBandwidthHz;
            hiRf_ = mid + 0.5 * kMinBandwidthHz;
        }
        bandOn_ = enabled;
        applyBand(false);
    }
    void setBandEnabled(bool on) {
        bandOn_ = on;
        applyBand(false);
    }
    // Notch i (0 .. kMaxNotches-1): centre RF Hz, -3 dB width Hz (kMinNotchWidthHz .. a quarter of the sample rate), on or off.
    void setNotch(int i, double rfHz, double widthHz, bool enabled) {
        if (i < 0 || i >= kMaxNotches || !std::isfinite(rfHz) || !std::isfinite(widthHz)) { return; }
        Notch& n = notch_[static_cast<std::size_t>(i)];
        n.rf = rfHz;
        n.widthHz = std::min(std::max(widthHz, kMinNotchWidthHz), 0.25 * fs_);
        n.on = enabled;
        n.rTarget = 1.0 - 3.14159265358979323846 * n.widthHz / fs_;
        n.rTarget = std::min(std::max(n.rTarget, 0.5), 0.9999999);
        if (!n.seen) {
            n.r = n.rTarget;
            n.seen = true;
        }
        n.rStep = std::fabs(n.rTarget - n.r) / (kFadeSeconds * fs_);  // glides over 10 ms
        applyNotch(i, false);
    }

    // The baseband edges now in force (after the span clamp), Hz relative to the centre; both 0 when the band is wholly outside.
    void edgesBaseband(double* lowHz, double* highHz) const {
        *lowHz = bandOutside_ ? 0.0 : bandLo_;
        *highHz = bandOutside_ ? 0.0 : bandHi_;
    }
    bool bandOutsideSpan() const { return bandOutside_; }
    bool notchActive(int i) const { return i >= 0 && i < kMaxNotches && notch_[static_cast<std::size_t>(i)].wantWet > 0.5; }
    bool isBypassed() const { return !anyActive(); }

    // The response (dB) to a signal at RF frequency `rfHz`, from the coefficients in force: the band and every enabled in-span notch. For the node's
    // drawing and for the tests.
    double responseDb(double rfHz) const {
        const double off = rfHz - fc_;
        double db = 0.0;
        if (bandOn_) {
            if (bandOutside_) { return -300.0; }
            const double f = off - bandMid_;
            db += abq::magnitudeDb(bandCoef_[0], fs_, std::fabs(f)) + abq::magnitudeDb(bandCoef_[1], fs_, std::fabs(f));
            // the biquads are symmetrical in +-f, as a low-pass of real coefficients is
        }
        for (int i = 0; i < kMaxNotches; ++i) {
            const Notch& n = notch_[static_cast<std::size_t>(i)];
            if (!n.on || n.outside) { continue; }
            const double w = 2.0 * 3.14159265358979323846 * (off - (n.rf - fc_)) / fs_;
            const std::complex<double> z1 = std::polar(1.0, -w);
            db += 20.0 * std::log10(std::max(std::abs((1.0 - z1) / (1.0 - n.rTarget * z1)), 1e-30));
        }
        return db;
    }

    // In place. x may hold any number of samples.
    void process(Cf* x, std::size_t n) {
        if (x == nullptr || n == 0) { return; }
        if (!anyActive()) {  // everything off and settled: samples untouched (the filter state is cleared so a later start is clean)
            clearState();
            idle_ = true;
            return;
        }
        idle_ = false;
        for (std::size_t s = 0; s < n; ++s) {
            double vr = static_cast<double>(x[s].real()), vi = static_cast<double>(x[s].imag());
            if (!std::isfinite(vr)) { vr = 0.0; }
            if (!std::isfinite(vi)) { vi = 0.0; }
            std::complex<double> v(vr, vi);
            for (int k = 0; k < kMaxNotches; ++k) {
                Notch& nt = notch_[static_cast<std::size_t>(k)];
                if (nt.wet == 0.0 && nt.wantWet == 0.0) { continue; }
                v = runNotch(nt, v);
            }
            if (band_.wet != 0.0 || band_.wantWet != 0.0) { v = runBand(v); }
            ++count_;
            x[s] = Cf(static_cast<float>(v.real()), static_cast<float>(v.imag()));
        }
    }

private:
    struct Osc {
        std::complex<double> p{1.0, 0.0};
        std::complex<double> w{1.0, 0.0};
        double hz = 0.0;
        void setHz(double f, double fs) {
            hz = f;
            w = std::polar(1.0, 2.0 * 3.14159265358979323846 * f / fs);
        }
        void step(std::uint64_t count) {
            p *= w;
            if ((count & 63u) == 0u) { p /= std::abs(p); }
        }
    };
    struct Lp {  // two biquad sections on I and on Q
        std::array<abq::Coef, 2> c;
        std::array<abq::State, 2> si{}, sq{};
        std::complex<double> run(std::complex<double> v) {
            double a = v.real(), b = v.imag();
            for (int k = 0; k < 2; ++k) {
                a = abq::run(c[static_cast<std::size_t>(k)], si[static_cast<std::size_t>(k)], a);
                b = abq::run(c[static_cast<std::size_t>(k)], sq[static_cast<std::size_t>(k)], b);
            }
            return {a, b};
        }
    };
    struct Band {
        double wet = 0.0, wantWet = 0.0;  // 0 = unfiltered .. 1 = filtered
        double gain = 1.0, wantGain = 1.0;  // 0 outside the span
        Osc osc;
        Lp a, b;
        bool fading = false;
        double fade = 0.0;
        bool pending = false;
        std::array<abq::Coef, 2> pendingCoef;
    } band_;
    struct Notch {
        double rf = 0.0, widthHz = 100.0;
        bool on = false, seen = false, outside = false;
        double wet = 0.0, wantWet = 0.0;
        double r = 0.99, rTarget = 0.99, rStep = 0.0;
        Osc osc;
        std::complex<double> xPrev{0, 0}, yPrev{0, 0};
    };

    bool anyActive() const {
        if (band_.wet != 0.0 || band_.wantWet != 0.0 || band_.fading) { return true; }
        for (const Notch& n : notch_) {
            if (n.wet != 0.0 || n.wantWet != 0.0) { return true; }
        }
        return false;
    }
    void clearState() {
        band_.a.si = {};
        band_.a.sq = {};
        band_.b = band_.a;
        for (Notch& n : notch_) { n.xPrev = n.yPrev = {0, 0}; }
    }

    static std::array<abq::Coef, 2> design(double fs, double cutoffHz) {
        return {abq::lowpass(fs, cutoffHz, 0.5411961001461970), abq::lowpass(fs, cutoffHz, 1.3065629648763766)};
    }
    static double approach(double cur, double tgt, double step) {
        if (cur < tgt) { return std::min(tgt, cur + step); }
        if (cur > tgt) { return std::max(tgt, cur - step); }
        return cur;
    }

    void applyBand(bool first) {
        const double lim = kSpanFraction * fs_;
        double lo = loRf_ - fc_, hi = hiRf_ - fc_;
        bool outside = false;
        if (hi <= -lim || lo >= lim) {
            outside = true;
        } else {
            lo = std::max(lo, -lim);
            hi = std::min(hi, lim);
        }
        bandOutside_ = outside;
        if (!outside) {
            bandLo_ = lo;
            bandHi_ = hi;
            bandMid_ = 0.5 * (lo + hi);
            const double hw = std::max(0.5 * (hi - lo), 0.5 * kMinBandwidthHz);
            const std::array<abq::Coef, 2> want = design(fs_, hw);
            if (first) {
                bandCoef_ = want;
                band_.a.c = want;
                band_.b.c = want;
            } else if (!sameCoef(want, bandCoef_)) {
                if (!band_.fading && band_.wet == 0.0 && band_.wantWet == 0.0) {  // not running: just take them
                    band_.a.c = want;
                    band_.b.c = want;
                } else if (band_.fading) {  // one fade at a time: this waits for the running one
                    band_.pending = true;
                    band_.pendingCoef = want;
                } else {
                    startFade(want);
                }
                bandCoef_ = want;
            }
            band_.osc.setHz(-bandMid_, fs_);  // rotate by -fm (the phase p is kept: continuous across the retune)
        }
        if (bandOn_ && band_.wet == 0.0 && band_.wantWet == 0.0) {  // starting from off: the filter starts clean
            band_.a.si = {};
            band_.a.sq = {};
            band_.b = band_.a;
            band_.b.c = band_.a.c;
        }
        band_.wantWet = bandOn_ ? 1.0 : 0.0;
        band_.wantGain = (bandOn_ && outside) ? 0.0 : 1.0;
    }
    static bool sameCoef(const std::array<abq::Coef, 2>& a, const std::array<abq::Coef, 2>& b) {
        return a[0].b0 == b[0].b0 && a[0].a1 == b[0].a1 && a[1].b0 == b[1].b0 && a[1].a1 == b[1].a1;
    }
    void startFade(const std::array<abq::Coef, 2>& want) {
        band_.b = band_.a;  // the new filter starts from the old one's state
        band_.b.c = want;
        band_.fading = true;
        band_.fade = 0.0;
    }

    void applyNotch(int i, bool) {
        Notch& n = notch_[static_cast<std::size_t>(i)];
        const double off = n.rf - fc_;
        n.outside = std::fabs(off) >= kSpanFraction * fs_;
        n.wantWet = (n.on && !n.outside) ? 1.0 : 0.0;
        if (!n.outside) { n.osc.setHz(-off, fs_); }
    }

    std::complex<double> runNotch(Notch& n, std::complex<double> v) {
        n.wet = approach(n.wet, n.wantWet, fadeStep_);
        n.r = approach(n.r, n.rTarget, n.rStep);
        n.osc.step(count_);
        const std::complex<double> rot = v * n.osc.p;                 // rotate by -(fn - fc)
        const std::complex<double> y = rot - n.xPrev + n.r * n.yPrev;  // (1 - z^-1) / (1 - r z^-1)
        n.xPrev = rot;
        n.yPrev = y;
        const std::complex<double> back = y * std::conj(n.osc.p);
        return v + n.wet * (back - v);
    }

    std::complex<double> runBand(std::complex<double> v) {
        band_.wet = approach(band_.wet, band_.wantWet, fadeStep_);
        band_.gain = approach(band_.gain, band_.wantGain, fadeStep_);
        band_.osc.step(count_);
        const std::complex<double> rot = v * band_.osc.p;
        std::complex<double> y;
        if (band_.fading) {
            const std::complex<double> ya = band_.a.run(rot), yb = band_.b.run(rot);
            y = (1.0 - band_.fade) * ya + band_.fade * yb;
            band_.fade += fadeStep_;
            if (band_.fade >= 1.0) {
                band_.a = band_.b;
                band_.fading = false;
                if (band_.pending) {
                    band_.pending = false;
                    startFade(band_.pendingCoef);
                }
            }
        } else {
            y = band_.a.run(rot);
        }
        y *= std::conj(band_.osc.p);  // rotate back
        y *= band_.gain;
        return v + band_.wet * (y - v);
    }

    double fs_;
    double fadeStep_ = 0.0;
    double fc_ = 0.0;
    double loRf_ = -1500.0, hiRf_ = 1500.0;
    bool bandOn_ = false;
    bool bandOutside_ = false;
    double bandLo_ = 0.0, bandHi_ = 0.0, bandMid_ = 0.0;
    std::array<abq::Coef, 2> bandCoef_;
    std::array<Notch, kMaxNotches> notch_{};
    std::uint64_t count_ = 0;
    bool idle_ = true;
};

}  // namespace cascade::dsp
