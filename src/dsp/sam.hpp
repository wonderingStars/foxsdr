// Synchronous AM (SAM / ECSS) demodulator: a carrier-tracking phase-locked loop at the channel rate,
// a product detector on the recovered carrier, selectable sideband (both, upper only, lower only), a
// lock indicator, and a defined behaviour when the carrier fades (hold the oscillator, then fall back
// to the envelope detector and re-acquire; no burst of noise).
//
// STANDALONE BLOCK. Nothing here is wired into the pipeline, the engine, the settings or the screen
// (docs/feature-records/signal-blocks-2.md, "WIRING HAND-OFF", says where it goes). The existing AM
// mode (dsp/demod.cpp, DemodMode::AM) is an envelope detector and is untouched.
//
// SOURCES (clean room: textbook DSP only).
//  * Product (synchronous) detection of AM: any communications textbook, e.g. Haykin, "Communication
//    Systems", coherent detection of DSB with carrier. With the local carrier phase-locked to the
//    received carrier, Re{x e^{-j theta}} = A + a(t): linear in the message a(t) whatever the
//    carrier-to-sideband ratio, which the envelope |x| is not once the carrier is weaker than the
//    sidebands.
//  * The loop is a second-order digital PLL (proportional + integrator) whose gains are set from the
//    loop noise bandwidth B_n and the damping zeta by the normalised-bandwidth formulas
//        theta_n = (B_n/fs) / (zeta + 1/(4 zeta)),  Kp = 4 zeta theta_n / (1 + 2 zeta theta_n + theta_n^2),
//        Ki = 4 theta_n^2 / (1 + 2 zeta theta_n + theta_n^2)        (phase detector gain 1 rad/rad,
//    NCO gain 1 rad/sample), as in Rice, "Digital Communications: A Discrete-Time Approach" (2009),
//    chapter 7 (from memory; UNVERIFIED against the book, validated here by the measured lock-in
//    times in tests/test_sam.cpp).
//  * Frequency-locked-loop assist for the +-200 Hz pull-in: the cross-product frequency
//    discriminator, argument of y[n] conj(y[n-1]), on a low-pass-filtered carrier estimate; a
//    textbook FLL (Gardner, "Phaselock Techniques", 3rd ed., 2005, frequency-aided acquisition;
//    UNVERIFIED against the book).
//  * Sideband selection: the wanted sideband is kept by a complex band-pass made of a real
//    windowed-sinc low-pass (Blackman-Harris, dsp/window.hpp) shifted to the sideband's centre
//    frequency, which is the Weaver arrangement the SSB path of dsp/demod.cpp already uses, folded
//    into one set of complex taps (no per-sample oscillator). Only Re{} is needed, so the cost is
//    two real dot products of N taps.
//
// WHAT IT DOES
//  * Input: complex baseband at `rateHz` with the AM signal roughly at DC (carrier offset up to
//    +-kMaxOffsetHz), channel-filtered to the AM width (the carrier-presence test compares the
//    carrier with the total power in the channel). Output: real audio, exactly one sample per input
//    sample, DC removed by the same 2 Hz one-pole blocker as the envelope AM mode, level equal to
//    the envelope detector's for an unfaded carrier.
//  * States. Searching: PLL (wide loop) plus FLL run; the output is the envelope detector.
//    Locked: PLL (normal loop) runs; the output is the product detector. Held: the carrier has
//    vanished from the channel but the sidebands may remain; the oscillator free-runs at its last
//    frequency and the output stays the product detector (the selective-fade case, where an
//    envelope detector over-modulates and distorts). A hold lasts at most holdSeconds (default 2),
//    then the state returns to Searching and the output crossfades (10 ms) to the envelope
//    detector. When the carrier is back, Held also returns to Searching, which re-pulls the loop.
//  * Latency: (taps-1)/2 samples (latencySamples()), the same in every sideband mode and in the
//    envelope fallback, so a change of sideband or state is a crossfade of aligned signals.
//  * Near DC the sideband filter has its -6 dB edge (the carrier sits on it), so below about 300 Hz
//    of audio the opposite sideband is only partly rejected in Upper / Lower.
//
// THREADS AND ALLOCATION. The constructor allocates (taps, history): set-up, control thread, DSP
// thread parked. process() runs on the DSP thread only and does not allocate, lock or wait.
// setSideband / setLoopBandwidthHz / setHoldSeconds are atomics, callable from any thread, and take
// effect at the start of the next process() call. locked(), state(), carrierPresence(),
// carrierOffsetHz() are atomics readable from any thread.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dsp/fir.hpp"
#include "dsp/window.hpp"

namespace cascade::dsp {

class SyncAm {
public:
    enum class Sideband : int { Both = 0, Upper = 1, Lower = 2 };
    enum class State : int { Searching = 0, Locked = 1, Held = 2 };

    // Largest carrier offset the acquisition is designed for (clamps the oscillator frequency).
    static constexpr double kMaxOffsetHz = 400.0;
    static constexpr double kDefaultLoopBandwidthHz = 20.0;   // loop noise bandwidth, Locked
    static constexpr double kDefaultHoldSeconds = 2.0;
    static constexpr double kDamping = 0.7071;
    // Carrier presence = |carrier estimate| / channel rms. 1 for an unmodulated carrier, about 0.8 for
    // 100 % modulation, ~0.04 for noise alone at 48 kHz. Lock needs > kPresenceLock, loss is < kPresenceLoss.
    static constexpr double kPresenceLock = 0.35;
    static constexpr double kPresenceLoss = 0.20;
    static constexpr double kPhaseCosLock = 0.95;    // cos of the mean phase error, to declare lock
    static constexpr double kPhaseCosLost = 0.50;    // below this for kPhaseLostSeconds the lock is given up
    static constexpr double kLockSeconds = 0.030;
    static constexpr double kLossSeconds = 0.015;
    static constexpr double kPhaseLostSeconds = 0.200;
    static constexpr double kCrossfadeSeconds = 0.010;

    explicit SyncAm(double rateHz, double audioWidthHz = 4500.0) : rate_(rateHz > 1000.0 ? rateHz : 48000.0) {
        const double w = std::clamp(audioWidthHz, 500.0, 0.45 * rate_);
        // Blackman-Harris: ~8 bins null-to-null, so N = 8 fs / transition; a 600 Hz transition at 48 kHz
        // gives 641 taps; capped so a high channel rate does not cost more than 1025 taps.
        std::size_t n = static_cast<std::size_t>(std::ceil(8.0 * rate_ / kTransitionHz));
        n = std::clamp<std::size_t>(n, 63, 1025);
        if (n % 2 == 0) { ++n; }
        taps_ = n;
        delay_ = (n - 1) / 2;
        const std::vector<float> lp = windowedSincLowpass(n, (w / 2.0) / rate_, WindowType::BlackmanHarris);
        hr_.resize(n);
        hi_.resize(n);
        const double ws = kTwoPi * (w / 2.0) / rate_;   // shift of the low-pass to the sideband centre
        for (std::size_t j = 0; j < n; ++j) {
            // history is read oldest first, so the taps are stored reversed: j pairs with tap k = n-1-j;
            // the modulation phase is referred to the centre tap so the filter has zero phase there
            const double k = static_cast<double>(n - 1 - j) - static_cast<double>(delay_);
            hr_[j] = static_cast<float>(lp[n - 1 - j] * std::cos(ws * k));
            hi_[j] = static_cast<float>(lp[n - 1 - j] * std::sin(ws * k));
        }
        mrBuf_.assign(2 * n, 0.0f);
        miBuf_.assign(2 * n, 0.0f);
        step_ = static_cast<float>(1.0 / (kCrossfadeSeconds * rate_));
        aCar_ = onePole(30.0);
        aAcq_ = onePole(250.0);
        aRms_ = onePole(20.0);
        aDc_ = std::exp(-kTwoPi * 2.0 / rate_);
        lockN_ = static_cast<int>(kLockSeconds * rate_);
        lossN_ = static_cast<int>(kLossSeconds * rate_);
        phaseLostN_ = static_cast<int>(kPhaseLostSeconds * rate_);
        fllGain_ = 1.0 / (0.03 * rate_);
        maxOmega_ = kTwoPi * kMaxOffsetHz / rate_;
        holdN_ = static_cast<std::int64_t>(kDefaultHoldSeconds * rate_);
        snapSideband(0);
        recomputeGains();
    }

    void setSideband(Sideband s) { sideband_.store(static_cast<int>(s), std::memory_order_relaxed); }
    Sideband sideband() const { return static_cast<Sideband>(sideband_.load(std::memory_order_relaxed)); }
    void setLoopBandwidthHz(double hz) {
        if (std::isfinite(hz)) { loopBw_.store(std::clamp(hz, 5.0, 100.0), std::memory_order_relaxed); }
    }
    double loopBandwidthHz() const { return loopBw_.load(std::memory_order_relaxed); }
    void setHoldSeconds(double s) {
        if (std::isfinite(s)) { holdSeconds_.store(std::clamp(s, 0.0, 10.0), std::memory_order_relaxed); }
    }
    double holdSeconds() const { return holdSeconds_.load(std::memory_order_relaxed); }

    State state() const { return static_cast<State>(state_.load(std::memory_order_relaxed)); }
    // The lock indicator: true while the loop is in phase lock on the carrier (state Locked).
    bool locked() const { return state() == State::Locked; }
    // Carrier amplitude over the channel rms, 0..~1 (see kPresenceLock).
    float carrierPresence() const { return presenceOut_.load(std::memory_order_relaxed); }
    // The oscillator's frequency relative to the channel centre: the estimated carrier offset, Hz.
    double carrierOffsetHz() const { return offsetHz_.load(std::memory_order_relaxed); }
    std::size_t latencySamples() const { return delay_; }
    std::size_t taps() const { return taps_; }
    double rateHz() const { return rate_; }

    // Clears the loop and every history; the settings are kept.
    void reset() {
        std::fill(mrBuf_.begin(), mrBuf_.end(), 0.0f);
        std::fill(miBuf_.begin(), miBuf_.end(), 0.0f);
        wp_ = 0;
        theta_ = 0.0;
        omega_ = 0.0;
        cRe_ = cIm_ = 0.0;
        a1r_ = a1i_ = a2r_ = a2i_ = pr_ = pi_ = 0.0;
        rms2_ = 0.0;
        dcPrevIn_ = dcPrevOut_ = 0.0;
        goodCount_ = lossCount_ = badPhaseCount_ = 0;
        holdCount_ = 0;
        mixCoh_ = 0.0f;
        stateLocal_ = State::Searching;
        state_.store(static_cast<int>(State::Searching), std::memory_order_relaxed);
        presenceOut_.store(0.0f, std::memory_order_relaxed);
        offsetHz_.store(0.0, std::memory_order_relaxed);
        snapSideband(sideband_.load(std::memory_order_relaxed));
    }

    // Demodulates n channel samples into n audio samples (1:1). `in` and `out` must not overlap.
    // Non-finite input samples are read as zero.
    std::size_t process(const std::complex<float>* in, std::size_t n, float* out) {
        refreshSettings();
        const int target = sideband_.load(std::memory_order_relaxed);
        const std::size_t N = taps_;
        for (std::size_t i = 0; i < n; ++i) {
            double xr = in[i].real();
            double xi = in[i].imag();
            if (!std::isfinite(xr) || !std::isfinite(xi)) { xr = 0.0; xi = 0.0; }

            // ---- mix to baseband with the recovered carrier ------------------------------------
            const double c = std::cos(theta_);
            const double s = std::sin(theta_);
            const double mr = xr * c + xi * s;
            const double mi = xi * c - xr * s;

            // ---- carrier estimate, channel power, lock metrics ---------------------------------
            cRe_ += aCar_ * (mr - cRe_);
            cIm_ += aCar_ * (mi - cIm_);
            rms2_ += aRms_ * ((mr * mr + mi * mi) - rms2_);
            const double rms = std::sqrt(rms2_) + 1e-30;
            const double cMag = std::sqrt(cRe_ * cRe_ + cIm_ * cIm_);
            const double presence = cMag / rms;
            const double phaseCos = cMag > 1e-30 ? cRe_ / cMag : 0.0;

            // ---- state machine ------------------------------------------------------------------
            switch (stateLocal_) {
                case State::Searching:
                    if (presence > kPresenceLock && phaseCos > kPhaseCosLock) {
                        if (++goodCount_ >= lockN_) { enter(State::Locked); }
                    } else {
                        goodCount_ = 0;
                    }
                    break;
                case State::Locked:
                    if (presence < kPresenceLoss) {
                        if (++lossCount_ >= lossN_) { enter(State::Held); }
                    } else {
                        lossCount_ = 0;
                    }
                    if (stateLocal_ == State::Locked) {
                        if (phaseCos < kPhaseCosLost) {
                            if (++badPhaseCount_ >= phaseLostN_) { enter(State::Searching); }
                        } else {
                            badPhaseCount_ = 0;
                        }
                    }
                    break;
                case State::Held:
                    if (++holdCount_ > holdN_) {
                        enter(State::Searching);
                    } else if (presence > kPresenceLock) {
                        if (++goodCount_ >= lockN_) { enter(State::Searching); }
                    } else {
                        goodCount_ = 0;
                    }
                    break;
            }

            // ---- loop update --------------------------------------------------------------------
            if (stateLocal_ != State::Held) {
                const double den = std::max(cMag, 0.1 * rms);
                const double err = std::clamp(mi / den, -1.0, 1.0);
                const bool wide = (stateLocal_ == State::Searching);
                const double kp = wide ? kpAcq_ : kp_;
                const double ki = wide ? kiAcq_ : ki_;
                omega_ += ki * err;
                if (wide) {
                    // frequency discriminator on the (more heavily) low-passed carrier estimate
                    a1r_ += aAcq_ * (mr - a1r_);
                    a1i_ += aAcq_ * (mi - a1i_);
                    a2r_ += aAcq_ * (a1r_ - a2r_);
                    a2i_ += aAcq_ * (a1i_ - a2i_);
                    const double cross = pr_ * a2i_ - pi_ * a2r_;   // Im{ conj(prev) cur }
                    const double dot = pr_ * a2r_ + pi_ * a2i_;     // Re{ conj(prev) cur }
                    const double mag = std::sqrt(a2r_ * a2r_ + a2i_ * a2i_);
                    if (mag > 0.2 * rms) { omega_ += fllGain_ * std::atan2(cross, dot) / kFllNorm; }
                    pr_ = a2r_;
                    pi_ = a2i_;
                }
                omega_ = std::clamp(omega_, -maxOmega_, maxOmega_);
                theta_ += omega_ + kp * err;
            } else {
                theta_ += omega_;
            }
            if (theta_ > kTwoPi || theta_ < -kTwoPi) { theta_ = std::fmod(theta_, kTwoPi); }

            // ---- history (doubled so a window is contiguous) ------------------------------------
            mrBuf_[wp_] = static_cast<float>(mr);
            mrBuf_[wp_ + N] = static_cast<float>(mr);
            miBuf_[wp_] = static_cast<float>(mi);
            miBuf_[wp_ + N] = static_cast<float>(mi);
            const std::size_t dly = wp_ + N - delay_;     // the sample `delay_` ago (matched delay)
            const float mrD = mrBuf_[dly];
            const float miD = miBuf_[dly];
            const float env = std::sqrt(mrD * mrD + miD * miD);

            // ---- sideband weights (one-hot targets, ramped) -------------------------------------
            wB_ = ramp(wB_, target == 0 ? 1.0f : 0.0f);
            wU_ = ramp(wU_, target == 1 ? 1.0f : 0.0f);
            wL_ = ramp(wL_, target == 2 ? 1.0f : 0.0f);
            float coh = wB_ * mrD;
            if (wU_ > 0.0f || wL_ > 0.0f) {
                const float* hr = hr_.data();
                const float* hi = hi_.data();
                const float* br = &mrBuf_[wp_ + 1];
                const float* bi = &miBuf_[wp_ + 1];
                float sa = 0.0f, sb = 0.0f;
                for (std::size_t j = 0; j < N; ++j) {
                    sa += hr[j] * br[j];
                    sb += hi[j] * bi[j];
                }
                coh += 2.0f * (wU_ * (sa - sb) + wL_ * (sa + sb));
            }
            wp_ = (wp_ + 1 == N) ? 0 : wp_ + 1;

            // ---- coherent / envelope crossfade, DC block ----------------------------------------
            mixCoh_ = ramp(mixCoh_, stateLocal_ == State::Searching ? 0.0f : 1.0f);
            const double y = static_cast<double>(mixCoh_) * coh + (1.0 - static_cast<double>(mixCoh_)) * env;
            const double o = y - dcPrevIn_ + aDc_ * dcPrevOut_;
            dcPrevIn_ = y;
            dcPrevOut_ = o;
            out[i] = static_cast<float>(o);
        }
        presenceOut_.store(static_cast<float>(std::sqrt(cRe_ * cRe_ + cIm_ * cIm_) / (std::sqrt(rms2_) + 1e-30)),
                           std::memory_order_relaxed);
        offsetHz_.store(omega_ * rate_ / kTwoPi, std::memory_order_relaxed);
        return n;
    }

private:
    static constexpr double kTwoPi = 6.283185307179586476925286766559;
    static constexpr double kTransitionHz = 600.0;
    static constexpr double kFllNorm = 1.0;

    double onePole(double hz) const { return 1.0 - std::exp(-kTwoPi * hz / rate_); }
    float ramp(float v, float t) const {
        if (v < t) { return std::min(t, v + step_); }
        if (v > t) { return std::max(t, v - step_); }
        return v;
    }
    void snapSideband(int sb) {
        wB_ = sb == 0 ? 1.0f : 0.0f;
        wU_ = sb == 1 ? 1.0f : 0.0f;
        wL_ = sb == 2 ? 1.0f : 0.0f;
    }
    void enter(State s) {
        stateLocal_ = s;
        goodCount_ = lossCount_ = badPhaseCount_ = 0;
        holdCount_ = 0;
        if (s == State::Searching) {
            pr_ = a2r_;   // the frequency discriminator restarts from the current estimate
            pi_ = a2i_;
        }
        state_.store(static_cast<int>(s), std::memory_order_relaxed);
    }
    void refreshSettings() {
        const double bw = loopBw_.load(std::memory_order_relaxed);
        if (bw != gainsFor_) { recomputeGains(); }
        holdN_ = static_cast<std::int64_t>(holdSeconds_.load(std::memory_order_relaxed) * rate_);
    }
    void recomputeGains() {
        const double bw = loopBw_.load(std::memory_order_relaxed);
        gainsFor_ = bw;
        const auto gains = [&](double bn, double& kp, double& ki) {
            const double tn = (bn / rate_) / (kDamping + 1.0 / (4.0 * kDamping));
            const double d = 1.0 + 2.0 * kDamping * tn + tn * tn;
            kp = 4.0 * kDamping * tn / d;
            ki = 4.0 * tn * tn / d;
        };
        gains(bw, kp_, ki_);
        gains(std::max(3.0 * bw, 60.0), kpAcq_, kiAcq_);
    }

    double rate_;
    std::size_t taps_ = 0;
    std::size_t delay_ = 0;
    std::vector<float> hr_, hi_;          // reversed complex taps (cos / sin parts), phase centred
    std::vector<float> mrBuf_, miBuf_;    // doubled history of the mixed signal
    std::size_t wp_ = 0;

    std::atomic<int> sideband_{0};
    std::atomic<double> loopBw_{kDefaultLoopBandwidthHz};
    std::atomic<double> holdSeconds_{kDefaultHoldSeconds};
    std::atomic<int> state_{0};
    std::atomic<float> presenceOut_{0.0f};
    std::atomic<double> offsetHz_{0.0};

    State stateLocal_ = State::Searching;
    double gainsFor_ = 0.0;
    double kp_ = 0, ki_ = 0, kpAcq_ = 0, kiAcq_ = 0;
    double theta_ = 0.0, omega_ = 0.0, maxOmega_ = 0.0, fllGain_ = 0.0;
    double aCar_ = 0, aAcq_ = 0, aRms_ = 0, aDc_ = 0;
    double cRe_ = 0, cIm_ = 0, rms2_ = 0;
    double a1r_ = 0, a1i_ = 0, a2r_ = 0, a2i_ = 0, pr_ = 0, pi_ = 0;
    double dcPrevIn_ = 0, dcPrevOut_ = 0;
    int lockN_ = 0, lossN_ = 0, phaseLostN_ = 0;
    int goodCount_ = 0, lossCount_ = 0, badPhaseCount_ = 0;
    std::int64_t holdN_ = 0, holdCount_ = 0;
    float step_ = 0.0f;
    float wB_ = 1.0f, wU_ = 0.0f, wL_ = 0.0f, mixCoh_ = 0.0f;
};

}  // namespace cascade::dsp
