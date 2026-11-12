// C-QUAM AM stereo decoder, with the 25 Hz pilot (stereo indicator) detector.
//
// SOURCE (open and primary): 47 CFR 73.128, "AM stereophonic broadcasting", 58 FR 66301, 20 December 1993, as served by the
// Legal Information Institute, https://www.law.cornell.edu/cfr/text/47/73.128, and by govinfo,
// https://www.govinfo.gov/content/pkg/CFR-2009-title47-vol4/xml/CFR-2009-title47-vol4-sec73-128.xml (both read 2026-11-11). Paragraph (c)
// says: the L+R sum is carried by amplitude (100 % AM at the peak of L+R); the L-R difference "shall be transmitted by varying the phase of
// the carrier", by an arctangent expression of L(t), R(t) and a modulation factor m; the carrier phase advances for a positive LEFT
// envelope change and retards for RIGHT; 100 % difference modulation is +-0.785 radians (pi / 4); a single channel at -0.75 with the other
// at zero must not exceed 1.25 radians of phase; the pilot is a 25 Hz tone, THD 3 % or less, +-0.1 Hz, phase-modulating the carrier
// +-0.05 radians peak (5 % L-R), injection 5 % +-1 %. The audio responses cite ANSI/EIA-549-1988 and NRSC-1 (not read; this block
// processes the demodulated channels, so their pre-emphasis is the audio chain's business).
// WHAT THE PAGES DO NOT SHOW: the equations themselves are images (ER20DE93.000 to .002) and could not be read. The form used here,
//     s(t) = (1 + L + R) cos( w t + arctan( (L - R) / (1 + L + R) ) )        (m = 1)
// was DERIVED from the words above and is consistent with every number the text gives: L - R = 1 with L + R = 0 gives arctan(1) = 0.7854
// (the 100 % figure) and L = -0.75, R = 0 gives arctan(-0.75 / 0.25) = -1.249 rad (the 1.25 limit). Where the pilot enters the
// arctangent (added to L - R, as here) is NOT stated in what was read: UNVERIFIED. The sign of the phase follows "left advances".
// So the description is primary, the encoding formula is a reconstruction; it is exact only to that extent.
//
// DECODER. Input: the channel complex baseband (carrier near 0 Hz, a few Hz of offset allowed; bigger offsets are the AFC's job).
// A second-order PLL (loop noise bandwidth 4 Hz after a 1 s wide-open start of 40 Hz, damping 0.707) derotates the carrier to y. With
// y = (1 + S) e^{j phi}:  S = |y| / Ac - 1 (Ac the carrier level, a 2 s one-pole average of |y|),  D = |y| tan(phi) / Ac = |y| Im(y) / Re(y) / Ac,
// L = (S + D) / 2,  R = (S - D) / 2. THE PLL ERROR IS THE RECOVERED D ITSELF (its mean is zero for zero-mean program, and a carrier-phase
// error delta appears as a mean of delta in it): locking on the mean phase of y instead would leave a program-dependent error (the mean
// of arctan((L-R)/(1+L+R)) is not zero for a single channel) and measurably worse separation. D passes a 25 Hz notch (Q 5)
// so the pilot does not reach the ears. PILOT DETECTOR: a Hann-windowed complex correlation of D at 25 Hz over 0.2 s (5 Hz resolution,
// so 25 Hz is bin 5); the pilot is present when its amplitude is between 0.5 and 2 times the nominal 0.05 (an interval, so a program
// tone AT 25 Hz at a normal level is refused) AND it stands 6 times above the mean of the same measure at 15 and 35 Hz (a Hann window has exact zeros 10 Hz from a tone, so the pilot does not leak into them; 20 and 30 Hz would see half of it)
// (so noise and bass do not count). Stereo comes on after 3 consecutive windows (0.6 s) and goes off after 5 misses (1 s). With stereo
// off the output is mono (L = R = S / 2), crossfaded over 50 ms.
//
// Every processing call is allocation-free; non-finite samples read as 0; any chunking gives the same output. THREADS: setters
// any thread, process() DSP thread. Not wired. Header-only. Latency: the notch only (a few ms).
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

class CquamDecoder {
public:
    static constexpr double kPilotHz = 25.0;
    static constexpr double kPilotNominal = 0.05;  // radians of carrier phase = 5 % L-R

    explicit CquamDecoder(double channelRateHz = 48000.0) : fs_(channelRateHz > 8000.0 ? channelRateHz : 48000.0) {
        win_ = static_cast<std::size_t>(0.2 * fs_);
        hann_.resize(win_);
        for (std::size_t i = 0; i < win_; ++i) { hann_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * abq::kPi * static_cast<double>(i) / static_cast<double>(win_))); }
        // Oscillators for the five measured frequencies, as phase steps.
        for (int k = 0; k < 5; ++k) { step_[k] = 2.0 * abq::kPi * kFreq[k] / fs_; }
        notch_ = abq::notch(fs_, kPilotHz, 5.0);
        acAlpha_ = 1.0 - std::exp(-1.0 / (2.0 * fs_));
        fadeStep_ = 1.0 / (0.050 * fs_);
        setLoop(40.0);
    }

    void setEnabled(bool on) { enabled_.store(on); }
    void setLoopBandwidthHz(double hz) { if (std::isfinite(hz)) { bw_.store(std::min(std::max(hz, 1.0), 40.0)); } }
    // Force stereo or mono regardless of the pilot (a "stereo / mono" button on the screen); Auto follows the pilot.
    void setForce(int f) { force_.store(f < 0 ? -1 : (f > 0 ? 1 : 0)); }  // -1 mono, 0 auto, +1 stereo

    bool stereo() const { return stereo_.load(); }
    double pilotLevel() const { return pilotAmp_.load(); }      // measured amplitude at 25 Hz in D (nominal 0.05)
    double carrierOffsetHz() const { return offHz_.load(); }
    bool pllLocked() const { return locked_.load(); }

    // `left` and `right` each hold n samples. Level: a 100 %-modulated mono carrier (S = 1) gives |L| = |R| = 0.5 peak.
    void process(const std::complex<float>* x, std::size_t n, float* left, float* right) {
        if (!enabled_.load()) {
            for (std::size_t i = 0; i < n; ++i) { left[i] = right[i] = 0.0f; }
            return;
        }
        const double bwTarget = bw_.load();
        for (std::size_t i = 0; i < n; ++i) {
            float re = x[i].real(), im = x[i].imag();
            if (!std::isfinite(re) || !std::isfinite(im)) { re = im = 0.0f; }
            // Derotate.
            const std::complex<double> y = std::complex<double>(re, im) * std::complex<double>(std::cos(-th_), std::sin(-th_));
            const double mag = std::abs(y);
            // The carrier level: a running mean at first (so it is right from the first second), then the 2 s one-pole.
            ac_ += std::max(acAlpha_, 1.0 / static_cast<double>(count_ + 1)) * (mag - ac_);
            const double acUse = std::max(ac_, 1e-9);
            const double S = mag / acUse - 1.0;
            double D = 0.0;
            if (y.real() > 1e-3 * mag && mag > 0.0) { D = mag * y.imag() / y.real() / acUse; }
            D = std::min(std::max(D, -3.0), 3.0);
            // PLL: the recovered D is the phase error.
            if (++count_ == static_cast<std::size_t>(fs_)) { setLoop(bwTarget); }
            wInt_ += ki_ * D;
            th_ += wInt_ + kp_ * D;
            if (th_ > 3.14159265358979323846) { th_ -= 2.0 * 3.14159265358979323846; }
            if (th_ < -3.14159265358979323846) { th_ += 2.0 * 3.14159265358979323846; }
            // Pilot measurement on D before the notch.
            pilotStep(D);
            const double Dn = abq::run(notch_, nst_, D);
            const bool wantStereo = force_.load() == 0 ? stereoAuto_ : force_.load() > 0;
            mix_ = wantStereo ? std::min(1.0, mix_ + fadeStep_) : std::max(0.0, mix_ - fadeStep_);
            const double dd = Dn * mix_;
            left[i] = static_cast<float>(0.5 * (S + dd));
            right[i] = static_cast<float>(0.5 * (S - dd));
        }
        offHz_.store(wInt_ * fs_ / (2.0 * 3.14159265358979323846));
        locked_.store(std::fabs(lastErr_) < 0.05 && count_ > static_cast<std::size_t>(fs_));
        stereo_.store(force_.load() == 0 ? stereoAuto_ : force_.load() > 0);
    }

    void reset() {
        th_ = wInt_ = 0.0;
        count_ = 0;
        setLoop(40.0);
        ac_ = 0.0;
        nst_ = abq::State{};
        fillN_ = 0;
        for (auto& a : acc_) { a = 0; }
        stereoAuto_ = false;
        hits_ = misses_ = 0;
        mix_ = 0.0;
    }

private:
    static constexpr double kFreq[5] = {25.0, 15.0, 20.0, 30.0, 35.0};  // 20 and 30 are measured for the display only

    void setLoop(double bwHz) {
        const double zeta = 0.70710678;
        const double wn = 2.0 * (2.0 * abq::kPi * bwHz) / (zeta + 1.0 / (4.0 * zeta));
        kp_ = 2.0 * zeta * wn / fs_;
        ki_ = wn * wn / (fs_ * fs_);
    }

    void pilotStep(double D) {
        lastErr_ = D;
        const double w = static_cast<double>(hann_[fillN_]);
        for (int k = 0; k < 5; ++k) {
            acc_[k] += w * D * std::complex<double>(std::cos(ph_[k]), -std::sin(ph_[k]));
            ph_[k] += step_[k];
        }
        if (++fillN_ == win_) {
            fillN_ = 0;
            double sumHann = 0;
            for (float h : hann_) { sumHann += static_cast<double>(h); }
            double amp[5];
            for (int k = 0; k < 5; ++k) {
                amp[k] = 2.0 * std::abs(acc_[k]) / sumHann;  // amplitude of a tone at that frequency
                acc_[k] = 0;
                ph_[k] = 0;
            }
            pilotAmp_.store(amp[0]);
            const double nb = 0.5 * (amp[1] + amp[4]);  // 15 and 35 Hz: a Hann window has exact zeros two bins (10 Hz) from a tone, so they see no pilot leakage
            const bool hit = amp[0] >= 0.5 * kPilotNominal && amp[0] <= 2.0 * kPilotNominal && amp[0] >= 6.0 * nb;
            if (hit) {
                misses_ = 0;
                if (++hits_ >= 3) { stereoAuto_ = true; }
            } else {
                hits_ = 0;
                if (++misses_ >= 5) { stereoAuto_ = false; }
            }
        }
    }

    double fs_;
    std::size_t win_ = 0;
    std::vector<float> hann_;
    double step_[5] = {};
    double ph_[5] = {};
    std::complex<double> acc_[5] = {};
    std::size_t fillN_ = 0;
    abq::Coef notch_;
    abq::State nst_;
    double acAlpha_, fadeStep_;
    double th_ = 0.0, wInt_ = 0.0, kp_ = 0.0, ki_ = 0.0, ac_ = 0.0, mix_ = 0.0, lastErr_ = 0.0;
    std::size_t count_ = 0;
    bool stereoAuto_ = false;
    int hits_ = 0, misses_ = 0;
    std::atomic<bool> enabled_{true}, stereo_{false}, locked_{false};
    std::atomic<int> force_{0};
    std::atomic<double> bw_{4.0}, pilotAmp_{0.0}, offHz_{0.0};
};

}  // namespace cascade::dsp
