// Stereo placement of ONE receiver's mono audio into a left / right pair: pan with a constant-power law, hard routing to the left
// ear, the right ear or both, and two ways to make a stereo image of a mono SSB or CW signal (pseudo-stereo and binaural). Each
// receiver owns one StereoPlacer; summing receivers is the mixer's job.
//
// PAN LAW. theta = (pan + 1) pi / 4, L = cos(theta), R = sin(theta): L^2 + R^2 = 1 for every pan (constant power; the sine / cosine
// law found in any audio-engineering text), pan -1 = left only (L 1, R 0), +1 = right only, centre = -3.01 dB in each ear. A
// makeup gain in dB can be set (+3.01 gives a centred receiver the level it has when simply copied to both ears). ROUTING: Left and
// Right put the signal in that ear alone at unity; Both applies the pan law. Gains move linearly over 10 ms (no click).
//
// PSEUDO-STEREO (for SSB / CW audio, which has no stereo information): complementary comb filters, Schroeder, "An artificial
// stereophonic effect obtained from using a single signal", J. Audio Eng. Soc. 6(2), 1958 (the method and its name from memory,
// UNVERIFIED against the paper; the maths here is derived and measured): d = x delayed by `delayMs` (default 8 ms), amount g in 0..1,
//   L = (x + g d) / sqrt(1 + g^2),   R = (x - g d) / sqrt(1 + g^2).
// L + R = 2 x / sqrt(1 + g^2): a MONO SUM keeps the signal with no comb (mono compatible). The normalised zero-lag correlation of
// L and R for a noise-like input is (1 - g^2) / (1 + g^2) (1 at g = 0, 0 at g = 1: fully decorrelated). Each channel has a comb
// ripple of 20 log((1 + g) / (1 - g)) dB peak to peak, with notches 1 / delay (125 Hz) apart, and the notches of L are the peaks of R.
//
// BINAURAL: the source is placed at an azimuth (-90 left .. +90 right degrees) by an interaural TIME difference and level
// difference. ITD: the Woodworth spherical-head formula ITD = (a / c)(theta + sin theta) with head radius a = 0.0875 m and speed of sound
// c = 343 m/s (R. S. Woodworth, Experimental Psychology, 1938; Woodworth and Schlosberg, 1954; constants as usually quoted, from
// memory: UNVERIFIED against the book): 656 us at 90 degrees. The far ear is delayed (fractional delay by linear interpolation,
// error under 0.2 dB below 3 kHz at 48 kHz). ILD: broadband, the near ear +ildDb/2 x sin(theta), the far ear -ildDb/2 x sin(theta),
// ildDb default 6 (a stand-in for the frequency-dependent head shadow, which is not modelled). Azimuth changes glide.
//
// Mode changes crossfade over 10 ms. Disabled = L = R = input, bit-exact. No allocation after construction; non-finite input reads
// as 0; any chunking gives the same output. THREADS: setters any thread (atomics), process() DSP thread. Not wired. Header-only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

namespace cascade::dsp {

enum class StereoMode : int { Pan = 0, Pseudo = 1, Binaural = 2 };
enum class StereoRoute : int { Both = 0, Left = 1, Right = 2 };

class StereoPlacer {
public:
    static constexpr double kHeadRadiusM = 0.0875, kSpeedOfSoundMps = 343.0;

    // ITD in seconds for an azimuth in degrees (positive right): the far ear lags by this much.
    static double woodworthItdS(double azimuthDeg) {
        const double th = std::fabs(azimuthDeg) * 3.14159265358979323846 / 180.0;
        return (kHeadRadiusM / kSpeedOfSoundMps) * (th + std::sin(th));
    }

    explicit StereoPlacer(double sampleRateHz = 48000.0)
        : fs_(sampleRateHz > 8000.0 ? sampleRateHz : 48000.0), ring_(static_cast<std::size_t>(0.060 * fs_ + 8.0), 0.0f) {
        ramp_ = 1.0 / (0.010 * fs_);
        itdGlide_ = 1.0 / (0.020 * fs_);
    }

    void setEnabled(bool on) { enabled_.store(on); }
    void setMode(StereoMode m) { mode_.store(static_cast<int>(m)); }
    void setRoute(StereoRoute r) { route_.store(static_cast<int>(r)); }
    void setPan(double p) { put(pan_, p, -1.0, 1.0); }
    void setMakeupDb(double db) { put(makeup_, db, -12.0, 12.0); }
    void setPseudoAmount(double g) { put(amount_, g, 0.0, 1.0); }
    void setPseudoDelayMs(double ms) { put(delayMs_, ms, 1.0, 20.0); }
    void setAzimuthDeg(double deg) { put(azimuth_, deg, -90.0, 90.0); }
    void setIldDb(double db) { put(ild_, db, 0.0, 12.0); }

    bool isActive() const { return enabled_.load(); }

    void process(const float* in, std::size_t n, float* left, float* right) {
        if (!enabled_.load()) {
            for (std::size_t i = 0; i < n; ++i) {
                const float v = std::isfinite(in[i]) ? in[i] : 0.0f;
                left[i] = v;
                right[i] = v;
            }
            return;
        }
        const int want = mode_.load();
        const int route = route_.load();
        // Pan targets.
        const double pan = pan_.load();
        const double th = (pan + 1.0) * 3.14159265358979323846 / 4.0;
        const double mk = std::pow(10.0, makeup_.load() / 20.0);
        const double tL = (route == static_cast<int>(StereoRoute::Both) ? std::cos(th) : (route == static_cast<int>(StereoRoute::Left) ? 1.0 : 0.0)) * (route == static_cast<int>(StereoRoute::Both) ? mk : 1.0);
        const double tR = (route == static_cast<int>(StereoRoute::Both) ? std::sin(th) : (route == static_cast<int>(StereoRoute::Right) ? 1.0 : 0.0)) * (route == static_cast<int>(StereoRoute::Both) ? mk : 1.0);
        const double tAmount = amount_.load();
        const double az = azimuth_.load();
        const double ild = ild_.load();
        const double tItd = woodworthItdS(az) * fs_;  // samples
        const double tIld = ild / 2.0 * std::sin(std::fabs(az) * 3.14159265358979323846 / 180.0);
        const std::size_t dp = static_cast<std::size_t>(delayMs_.load() * 1e-3 * fs_ + 0.5);
        const std::size_t cap = ring_.size();
        for (std::size_t i = 0; i < n; ++i) {
            const double x = std::isfinite(in[i]) ? static_cast<double>(in[i]) : 0.0;
            ring_[wp_] = static_cast<float>(x);
            auto tap = [&](double d) {  // x delayed by d samples (linear interpolation between the two neighbours)
                double pos = static_cast<double>(wp_) - d;
                while (pos < 0.0) { pos += static_cast<double>(cap); }
                const double fl = std::floor(pos);
                const std::size_t i0 = static_cast<std::size_t>(fl) % cap;
                const double fr = pos - fl;
                return static_cast<double>(ring_[i0]) * (1.0 - fr) + static_cast<double>(ring_[(i0 + 1) % cap]) * fr;
            };
            // Smooth every parameter.
            gl_ = slew(gl_, tL);
            gr_ = slew(gr_, tR);
            amt_ = slew(amt_, tAmount);
            ildCur_ = slewdb(ildCur_, tIld);
            azSign_ = az >= 0.0 ? 1.0 : -1.0;
            if (std::fabs(tItd - itd_) <= itdGlide_ * 8.0) { itd_ = tItd; } else { itd_ += (tItd > itd_ ? 1.0 : -1.0) * itdGlide_ * 8.0; }
            // The three candidate outputs (only the ones needed are computed).
            double l[3] = {0, 0, 0}, r[3] = {0, 0, 0};
            auto compute = [&](int m) {
                if (m == 0) {
                    l[0] = x * gl_;
                    r[0] = x * gr_;
                } else if (m == 1) {
                    const double d = tap(static_cast<double>(dp));
                    const double k = 1.0 / std::sqrt(1.0 + amt_ * amt_);
                    l[1] = (x + amt_ * d) * k;
                    r[1] = (x - amt_ * d) * k;
                } else {
                    // Far ear delayed by the ITD; near ear +ild/2 dB, far ear -ild/2 dB.
                    const double dl = tap(itd_);
                    const double nearG = std::pow(10.0, ildCur_ / 20.0), farG = std::pow(10.0, -ildCur_ / 20.0);
                    if (azSign_ >= 0.0) {  // source on the right: the left ear is far
                        l[2] = dl * farG;
                        r[2] = x * nearG;
                    } else {
                        l[2] = x * nearG;
                        r[2] = dl * farG;
                    }
                }
            };
            if (modeCur_ != want && !fading_) {
                fading_ = true;
                modeNext_ = want;
                mix_ = 0.0;
            }
            double ol, orr;
            if (fading_) {
                compute(modeCur_);
                compute(modeNext_);
                ol = l[modeCur_] + (l[modeNext_] - l[modeCur_]) * mix_;
                orr = r[modeCur_] + (r[modeNext_] - r[modeCur_]) * mix_;
                mix_ += ramp_;
                if (mix_ >= 1.0) {
                    fading_ = false;
                    modeCur_ = modeNext_;
                }
            } else {
                compute(modeCur_);
                ol = l[modeCur_];
                orr = r[modeCur_];
            }
            left[i] = static_cast<float>(ol);
            right[i] = static_cast<float>(orr);
            wp_ = (wp_ + 1) % cap;
        }
    }

    void reset() {
        std::fill(ring_.begin(), ring_.end(), 0.0f);
        wp_ = 0;
    }

private:
    static void put(std::atomic<double>& a, double v, double lo, double hi) {
        if (std::isfinite(v)) { a.store(std::min(std::max(v, lo), hi)); }
    }
    double slew(double cur, double target) const {
        if (std::fabs(target - cur) <= ramp_) { return target; }
        return cur + (target > cur ? ramp_ : -ramp_);
    }
    double slewdb(double cur, double target) const {
        const double step = 12.0 * ramp_;  // 12 dB over 10 ms at most per... (full-scale 12 dB in the ramp time)
        if (std::fabs(target - cur) <= step) { return target; }
        return cur + (target > cur ? step : -step);
    }

    double fs_, ramp_, itdGlide_;
    std::vector<float> ring_;
    std::size_t wp_ = 0;
    std::atomic<bool> enabled_{true};
    std::atomic<int> mode_{0}, route_{0};
    std::atomic<double> pan_{0.0}, makeup_{0.0}, amount_{0.5}, delayMs_{8.0}, azimuth_{0.0}, ild_{6.0};
    double gl_ = 0.70710678118654752, gr_ = 0.70710678118654752, amt_ = 0.5, ildCur_ = 0.0, itd_ = 0.0, azSign_ = 1.0, mix_ = 0.0;
    int modeCur_ = 0, modeNext_ = 0;
    bool fading_ = false;
};

}  // namespace cascade::dsp
