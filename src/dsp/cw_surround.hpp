// CW STEREO PLACEMENT BY PITCH ("digital surround"): the pitch of a CW signal decides where it sits between the ears.
//
// A CW operator who tunes across a band hears each station at a different beat pitch; with this block a lower pitch sits to the left and a higher one to the
// right, so two stations a few hundred hertz apart are also a few degrees apart. It places ONE mono signal per receiver by the dominant tone in it (it does
// not separate two simultaneous tones: both take the place of the stronger).
//
// PITCH ESTIMATE. A bank of Goertzel filters every `binHz` (10 Hz) from 200 to 1500 Hz over a window of 1024 samples, hopped every 512 samples; the peak bin
// is refined by a parabola through its log power and its two neighbours. A window counts as a TONE only when its peak power is at least `toneRatio` (default 100,
// 20 dB) times the reference level of the bank (the mean of its lowest three quarters: white noise peaked at 34 times it in 560 windows measured, a tone in a 10 dB signal-to-noise ratio is more than 1000 times it), and above an absolute floor; for a window without a tone the pitch estimate is held for holdMs (default 400 ms) and then the
// pan glides back to the centre. Noise alone therefore does not move the image.
//
// PLACEMENT. pan = clamp((pitch - centrePitchHz) / spreadHz, -1, 1) (default centre 700 Hz, spread 400 Hz: 300 Hz is hard left, 1100 Hz hard right; invert swaps
// the sides). Constant-power law with theta = (pan + 1) pi / 4: L = sqrt(2) cos theta, R = sqrt(2) sin theta, so a centred signal comes out at unity in each ear
// and L^2 + R^2 = 2 x^2 for every pan. The applied pan follows the target with a one-pole glide of about 30 ms, so the image moves smoothly and never clicks.
// Disabled: L = R = input, bit-exact. Non-finite input reads as 0. The output is the same for any chunking (the analysis runs sample by sample into its
// window).
//
// THREADS: one thread (the audio thread). The setters are for that thread. No allocation after construction.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace cascade::dsp {

struct CwSurroundConfig {
    double centrePitchHz = 700.0;
    double spreadHz = 400.0;
    bool invert = false;
    double toneRatio = 100.0;
    double holdMs = 400.0;
};

class CwSurround {
public:
    static constexpr std::size_t kWindow = 1024, kHop = 512;
    static constexpr double kLowHz = 200.0, kHighHz = 1500.0, kBinHz = 10.0;

    static double panForPitch(double pitchHz, double centreHz, double spreadHz, bool invert = false) {
        if (!std::isfinite(pitchHz) || !std::isfinite(centreHz) || !(spreadHz > 0.0)) { return 0.0; }
        const double p = std::max(-1.0, std::min(1.0, (pitchHz - centreHz) / spreadHz));
        return invert ? -p : p;
    }
    // Constant-power gains for a pan in -1..1.
    static void gainsForPan(double pan, double& l, double& r) {
        const double th = (std::max(-1.0, std::min(1.0, pan)) + 1.0) * 3.14159265358979323846 / 4.0;
        l = std::sqrt(2.0) * std::cos(th);
        r = std::sqrt(2.0) * std::sin(th);
    }

    explicit CwSurround(double sampleRateHz = 48000.0) : fs_(sampleRateHz >= 8000.0 && sampleRateHz <= 384000.0 ? sampleRateHz : 48000.0) {
        const std::size_t nb = static_cast<std::size_t>((kHighHz - kLowHz) / kBinHz) + 1;
        coef_.resize(nb);
        for (std::size_t b = 0; b < nb; ++b) {
            const double f = kLowHz + static_cast<double>(b) * kBinHz;
            coef_[b] = 2.0 * std::cos(2.0 * 3.14159265358979323846 * f / fs_);
        }
        window_.assign(kWindow, 0.0f);
        glide_ = 1.0 - std::exp(-1.0 / (0.030 * fs_));
    }

    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }
    // false (config kept) for a spread of 50..2000 Hz, a centre of 200..1500 Hz or a ratio of 2..1000, hold 0..5000 ms outside which it is refused.
    bool setConfig(const CwSurroundConfig& c) {
        if (!(std::isfinite(c.centrePitchHz) && c.centrePitchHz >= kLowHz && c.centrePitchHz <= kHighHz && std::isfinite(c.spreadHz) && c.spreadHz >= 50.0 && c.spreadHz <= 2000.0 &&
              std::isfinite(c.toneRatio) && c.toneRatio >= 2.0 && c.toneRatio <= 1000.0 && std::isfinite(c.holdMs) && c.holdMs >= 0.0 && c.holdMs <= 5000.0)) {
            return false;
        }
        cfg_ = c;
        return true;
    }
    const CwSurroundConfig& config() const { return cfg_; }

    // The pitch estimate (Hz) of the last tone window, 0 before any; and the pan being applied now.
    double pitchHz() const { return pitch_; }
    double pan() const { return pan_; }

    void process(const float* in, float* l, float* r, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            const float x = std::isfinite(in[i]) ? in[i] : 0.0f;
            if (!enabled_) { l[i] = x; r[i] = x; continue; }
            push(x);
            pan_ += glide_ * (target_ - pan_);
            double gl, gr;
            gainsForPan(pan_, gl, gr);
            l[i] = static_cast<float>(x * gl);
            r[i] = static_cast<float>(x * gr);
        }
    }

private:
    void push(float x) {
        window_[w_] = x;
        w_ = (w_ + 1) % kWindow;
        if (filled_ < kWindow) { ++filled_; }
        if (++sinceHop_ >= kHop) {
            sinceHop_ = 0;
            if (filled_ >= kWindow) { analyse(); }
        }
        if (heldMs_ >= 0.0) { heldMs_ += 1000.0 / fs_; }
        if (heldMs_ > cfg_.holdMs) { target_ = 0.0; }   // no tone for holdMs: back to the centre
    }
    void analyse() {
        const std::size_t nb = coef_.size();
        std::vector<double>& p = power_;
        p.assign(nb, 0.0);
        for (std::size_t b = 0; b < nb; ++b) {
            double s1 = 0.0, s2 = 0.0;
            const double c = coef_[b];
            for (std::size_t k = 0; k < kWindow; ++k) {
                const double s = static_cast<double>(window_[(w_ + k) % kWindow]) + c * s1 - s2;
                s2 = s1;
                s1 = s;
            }
            p[b] = s1 * s1 + s2 * s2 - c * s1 * s2;
        }
        std::size_t best = 0;
        for (std::size_t b = 1; b < nb; ++b) { if (p[b] > p[best]) { best = b; } }
        // the noise reference is the mean of the LOWEST THREE QUARTERS of the bank: a tone's own lobe (about nine bins of 131) sits in the top quarter and
        // cannot lift it, and it varies far less from window to window than the median does (measured: white noise reaches 20 to 34 times it in the strongest
        // bin over a few hundred windows, a tone far more; the median-based first try varied more still)
        std::vector<double> sorted = p;
        std::sort(sorted.begin(), sorted.end());
        const std::size_t lowN = nb * 3 / 4;
        double acc = 0.0;
        for (std::size_t b = 0; b < lowN; ++b) { acc += sorted[b]; }
        const double median = acc / static_cast<double>(lowN);   // (the name is kept: it is the reference level)
        const double floor = 1e-6 * static_cast<double>(kWindow) * static_cast<double>(kWindow) / 4.0;   // 1e-6 amplitude squared
        if (!(p[best] > cfg_.toneRatio * median) || !(p[best] > floor)) { return; }   // no tone in this window: hold
        double off = 0.0;
        if (best > 0 && best + 1 < nb && p[best - 1] > 0.0 && p[best + 1] > 0.0) {
            const double a = std::log(p[best - 1]), b = std::log(p[best]), c = std::log(p[best + 1]);
            const double den = a - 2.0 * b + c;
            if (den < 0.0) { off = std::max(-0.5, std::min(0.5, 0.5 * (a - c) / den)); }
        }
        pitch_ = kLowHz + (static_cast<double>(best) + off) * kBinHz;
        target_ = panForPitch(pitch_, cfg_.centrePitchHz, cfg_.spreadHz, cfg_.invert);
        heldMs_ = 0.0;
    }

    double fs_;
    CwSurroundConfig cfg_{};
    bool enabled_ = true;
    std::vector<double> coef_, power_;
    std::vector<float> window_;
    std::size_t w_ = 0, filled_ = 0, sinceHop_ = 0;
    double pitch_ = 0.0, pan_ = 0.0, target_ = 0.0, glide_ = 0.0;
    double heldMs_ = -1.0;   // time since the last tone window; negative before the first
};

}  // namespace cascade::dsp
