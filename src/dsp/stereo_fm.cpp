// SPDX-License-Identifier: MIT
#include "dsp/stereo_fm.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace cascade::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr double kTwoPi = 6.283185307179586476925286766559;

// --- multiplex plan (from the broadcast-FM stereo standard) -----------------
// Audio band 0..15 kHz, pilot at 19 kHz, difference channel DSB-SC on a
// 38 kHz suppressed subcarrier, i.e. sidebands 23..53 kHz.
constexpr double kPilotHz = 19000.0;
constexpr double kAudioTopHz = 15000.0;
constexpr double kCompositeTopHz = 53000.0;

// Full stereo needs the whole composite present without aliasing.
constexpr double kMinStereoRateHz = 2.0 * kCompositeTopHz;

// --- 15 kHz audio low-pass ---------------------------------------------------
// One filter serves both the sum path and the difference path (packed into the
// real and imaginary parts of one complex stream). It has to pass 15 kHz and
// stop by 19 kHz, so the transition band is fixed by the standard at 4 kHz and
// the -6 dB cutoff sits at its midpoint, 17 kHz. Hamming window: ~-53 dB first
// stopband lobe, so the pilot leaking into the mono path lands ~73 dB below
// the composite peak (the pilot itself is only ~10% of it) — inaudible, at
// roughly half the taps a Blackman-Harris design would cost.
constexpr double kAudioCutoffHz = 17000.0;
constexpr double kAudioTransitionHz = 4000.0;

// --- pilot band-pass (as a complex low-pass after the -19 kHz mix) ----------
// After mixing the composite down by 19 kHz the nearest interferers are the
// top of (L+R) at -4 kHz and the bottom of the (L-R) sidebands at +4 kHz, so a
// 1.2 kHz cutoff with a 3.6 kHz transition (stopband from 3 kHz) isolates the
// pilot with the Hamming window's ~53 dB to spare. It is deliberately no
// narrower than it needs to be: every extra hertz of selectivity costs taps
// (the tap count scales as rate/transition) and group delay, and the PLL — not
// this filter — provides the final, arbitrarily narrow selectivity.
constexpr double kPilotCutoffHz = 1200.0;
constexpr double kPilotTransitionHz = 3600.0;

// --- PLL ---------------------------------------------------------------------
// Natural frequency and damping of the second-order loop. 20 Hz with critical-
// ish damping (1/sqrt(2)) gives a ~11 ms envelope time constant: fast enough to
// acquire in a few tens of milliseconds and to ride out interference, narrow
// enough that program material leaking through the pilot filter (worst case a
// few kHz away) is attenuated by another ~40 dB inside the loop.
constexpr double kLoopNaturalHz = 20.0;
constexpr double kLoopDamping = 0.70710678118654752440;
// Hold/capture limit on the integrator. The standard specifies the pilot to
// within a couple of hertz; +/-300 Hz is generous for the receiver's own
// tuning error while still stopping a pilot-less input from walking the loop
// off to somewhere it cannot recover from.
constexpr double kCaptureRangeHz = 300.0;

// --- level estimation, lock and gating --------------------------------------
constexpr double kLevelTauSec = 0.010;  // one-pole averaging for the estimators
// Pilot amplitude as a fraction of the composite level that counts as "a full
// strength pilot" for the UI indicator. The standard injects the pilot at
// ~8-10% of peak deviation; measuring the composite by its RMS-equivalent sine
// amplitude puts a compliant broadcast at or above this figure over the whole
// range from silence (composite = pilot only) to full modulation.
constexpr double kPilotRefFraction = 0.05;
constexpr double kLockOnLevel = 0.65;    // hysteresis: acquire ...
constexpr double kLockOffLevel = 0.45;   // ... and release, so a level hovering
                                         // at the threshold cannot chatter.
constexpr double kSilenceAmp = 1.0e-6;   // below this there is no signal at all
constexpr double kGateRampSec = 0.020;   // stereo blend ramp, see process()

// Windowed-sinc low-pass sized so the transition width in HERTZ is
// rate-independent (Hamming: transition ~ 3.3/N of the sample rate).
std::vector<float> designLowpass(double rate, double cutoffHz,
                                 double transitionHz) {
    std::size_t n =
        static_cast<std::size_t>(std::ceil(3.3 * rate / transitionHz));
    if (n < 31) { n = 31; }      // floor: keep the stopband meaningful
    if (n > 2047) { n = 2047; }  // cap: bound CPU at very high composite rates
    if (n % 2 == 0) { ++n; }     // windowedSincLowpass requires odd length
    double cutoff = cutoffHz / rate;
    // Degenerate very-low-rate guard: stay inside the designer's (0, 0.5)
    // precondition instead of tripping its assert. Such a rate cannot carry a
    // composite anyway (stereoCapable_ is false there).
    if (cutoff > 0.45) { cutoff = 0.45; }
    return windowedSincLowpass(n, cutoff, WindowType::Hamming);
}

// Wrap to [-pi, pi) with conditional adds rather than fmod: every value fed in
// here is already within one turn of the range, and the +/-2*pi constant is
// applied at most twice, so this is exact where fmod would round.
inline double wrapPi(double a) noexcept {
    while (a >= kPi) { a -= kTwoPi; }
    while (a < -kPi) { a += kTwoPi; }
    return a;
}

}  // namespace

StereoFm::StereoFm(double compositeRateHz)
    : rate_(compositeRateHz),
      stereoCapable_(compositeRateHz >= kMinStereoRateHz),
      pilotLpf_(designLowpass(compositeRateHz, kPilotCutoffHz,
                              kPilotTransitionHz),
                1),
      audioLpf_(designLowpass(compositeRateHz, kAudioCutoffHz,
                              kAudioTransitionHz),
                1) {
    assert(rate_ > 0.0);
    static_assert(kAudioCutoffHz > kAudioTopHz, "audio LPF must pass 15 kHz");

    // The mixer runs at -19 kHz so the pilot lands at DC. Folding is harmless
    // here: below kMinStereoRateHz the pilot is aliased away and the whole
    // pilot chain is bypassed.
    pilotNco_.setFrequency(-kPilotHz / rate_);

    // A linear-phase FIR of odd length N delays by exactly (N-1)/2 samples.
    pilotGroupDelay_ =
        0.5 * static_cast<double>(pilotLpf_.taps().size() - 1);

    // Standard second-order loop constants from the natural frequency and
    // damping: with T = 1/rate, Kp = 2*zeta*wn*T and Ki = (wn*T)^2. Expressed
    // this way the loop dynamics are in seconds and therefore identical at any
    // composite rate.
    const double wnT = kTwoPi * kLoopNaturalHz / rate_;
    kp_ = 2.0 * kLoopDamping * wnT;
    ki_ = wnT * wnT;
    freqMax_ = kTwoPi * kCaptureRangeHz / rate_;

    // One-pole averaging coefficient for a kLevelTauSec time constant.
    levelAlpha_ = 1.0 - std::exp(-1.0 / (rate_ * kLevelTauSec));
    gateStep_ = 1.0 / (rate_ * kGateRampSec);

    setDeemphasisUs(50.0);
    reset();
}

void StereoFm::setDeemphasisUs(double us) {
    // Same contract as Demodulator::setDeemphasisUs: anything non-positive or
    // non-finite means "off", implemented as a pole of exactly 0, for which
    // y = (1-p)*x + p*y collapses to y = x — a true pass-through, not an
    // approximation of one.
    if (!(us > 0.0) || !std::isfinite(us)) {
        deemphTauSec_ = 0.0;
        deemphPole_ = 0.0;
    } else {
        deemphTauSec_ = us * 1.0e-6;
        // Pole matching (impulse invariance): the analog RC pole at s = -1/tau
        // maps to z = exp(-T/tau), and the numerator is scaled for exactly
        // unity DC gain. See demod.cpp for why this over the bilinear map.
        deemphPole_ = std::exp(-1.0 / (rate_ * deemphTauSec_));
    }
    // Clear both memories: a state charged at the old time constant would
    // thump on the switch.
    deemphLeft_ = 0.0;
    deemphRight_ = 0.0;
}

void StereoFm::reset() {
    pilotNco_.reset();
    pilotLpf_.reset();
    audioLpf_.reset();
    psi_ = 0.0;
    freq_ = 0.0;
    compSq_ = 0.0;
    cohI_ = 0.0;
    cohQ_ = 0.0;
    level_ = 0.0f;
    locked_ = false;
    // The gate starts CLOSED, so a stream begins in mono and fades into stereo
    // once the pilot is actually locked. Starting open would mean decoding the
    // difference channel with an unlocked carrier for the first few ms — the
    // one condition under which stereo sounds worse than mono.
    gate_ = 0.0;
    deemphLeft_ = 0.0;
    deemphRight_ = 0.0;
}

std::size_t StereoFm::process(const float* mpx, std::size_t n, float* left,
                              float* right) {
    if (n == 0) { return 0; }

    audioIn_.resize(n);
    audioOut_.resize(audioLpf_.outputCapacity(n));

    if (stereoCapable_) {
        osc_.resize(n);
        mix_.resize(n);
        pilotBb_.resize(pilotLpf_.outputCapacity(n));

        // Stage 1: pilot to complex baseband. osc_[k] = e^{-j*2pi*19k*k/fs};
        // the real composite times it puts the pilot at DC (and its negative-
        // frequency image at -38 kHz, which the low-pass removes outright).
        pilotNco_.generate(osc_.data(), n);
        for (std::size_t i = 0; i < n; ++i) {
            mix_[i] = osc_[i] * mpx[i];
        }
        [[maybe_unused]] const std::size_t np =
            pilotLpf_.process(mix_.data(), n, pilotBb_.data());
        // Decimation 1: every input yields exactly one output.
        assert(np == n);
    }

    // Stage 2: per-sample PLL, level/lock tracking, gating, and construction
    // of the two-channel input to the shared 15 kHz low-pass. This is one loop
    // rather than several because every quantity here is causal in the sample
    // index — splitting it into per-block passes would make the result depend
    // on where the caller chose to cut the stream.
    for (std::size_t i = 0; i < n; ++i) {
        const double x = static_cast<double>(mpx[i]);

        // Composite level as the amplitude of an equivalent sine, sqrt(2*mean
        // square). Used only as the reference the pilot is measured against.
        compSq_ += levelAlpha_ * (x * x - compSq_);
        const double compAmp = std::sqrt(2.0 * compSq_);
        const double refAmp = kPilotRefFraction * compAmp;

        double carrier38 = 0.0;
        if (stereoCapable_) {
            const double zr = static_cast<double>(pilotBb_[i].real());
            const double zi = static_cast<double>(pilotBb_[i].imag());

            // De-rotate the pilot baseband by the loop's current phase. The
            // in-phase/quadrature pair that falls out serves twice: its
            // quadrature part is the phase detector, and its slow average is
            // the coherent pilot amplitude behind pilotLevel().
            const double c = std::cos(psi_);
            const double s = std::sin(psi_);
            const double di = zr * c + zi * s;
            const double dq = zi * c - zr * s;

            cohI_ += levelAlpha_ * (di - cohI_);
            cohQ_ += levelAlpha_ * (dq - cohQ_);
            const double coh = std::sqrt(cohI_ * cohI_ + cohQ_ * cohQ_);
            level_ = (compAmp > kSilenceAmp)
                         ? static_cast<float>(std::min(1.0, coh / refAmp))
                         : 0.0f;

            if (locked_) {
                if (level_ < kLockOffLevel) { locked_ = false; }
            } else if (level_ > kLockOnLevel) {
                locked_ = true;
            }

            // Phase detector: the true phase error, from atan2 of the
            // de-rotated pilot. atan2 rather than the cheaper cross-product
            // approximation because it stays proportional to the error all the
            // way out to +/-180 degrees, which is what makes acquisition from
            // an arbitrary initial phase take a few time constants instead of
            // stalling near the anti-phase point.
            const double err = wrapPi(std::atan2(dq, di));
            // Weight by how much pilot there actually is, saturating at a
            // standard-strength one. With no pilot the weight is ~0 and the
            // loop HOLDS its state instead of integrating noise into the
            // frequency estimate and railing against the capture limit.
            const double zmag = std::sqrt(zr * zr + zi * zi);
            const double w =
                (refAmp > 0.0) ? std::min(1.0, zmag / refAmp) : 0.0;
            const double e = w * err;

            // Regenerated carrier, from the SAME phase the loop tracks.
            //   Phi = psi + freq*D + (2pi*19k*k/fs)
            // The freq*D term compensates the pilot filter's group delay D:
            // the loop measures the pilot as it was D samples ago, and the
            // loop's own frequency estimate extrapolates that phase forward to
            // the sample being demodulated right now. Without it a pilot
            // offset by df hertz would leave a fixed 2*pi*df*D/fs phase error
            // in the doubled carrier — small, but it is free to remove.
            //
            // e^{j*2*Phi} = e^{j*2*(psi + freq*D)} * conj(osc_[i])^2, since
            // conj(osc_[i]) = e^{+j*2pi*19k*k/fs}. Deriving the deterministic
            // half from the very oscillator that did the down-conversion is
            // what makes the doubling exact: there is no second oscillator to
            // drift, and squaring a phasor doubles its phase by construction.
            const double psiC = psi_ + freq_ * pilotGroupDelay_;
            const double c2 = std::cos(2.0 * psiC);
            const double s2 = std::sin(2.0 * psiC);
            const double ur = static_cast<double>(osc_[i].real());
            const double ui = -static_cast<double>(osc_[i].imag());
            const double u2r = ur * ur - ui * ui;
            const double u2i = 2.0 * ur * ui;
            carrier38 = c2 * u2r - s2 * u2i;  // = cos(2*Phi)

            // Loop update, AFTER the carrier has been taken, so the phase used
            // for sample i depends only on samples up to i-1.
            psi_ = wrapPi(psi_ + freq_ + kp_ * e);
            freq_ = std::clamp(freq_ + ki_ * e, -freqMax_, freqMax_);
        }

        // Stereo gate. A hard mono/stereo switch steps L and R by +/-(L-R)/2 —
        // an audible click, and it happens exactly when reception is marginal
        // and lock is flickering. Ramping the difference channel over
        // kGateRampSec instead keeps the SUM path (and therefore the mono
        // content of both channels) perfectly continuous while the stereo
        // image opens and closes; nothing in the output is ever discontinuous.
        // The gate is applied BEFORE the low-pass, so the filter smooths the
        // ramp further and no separate delay-matching is needed.
        const double target = (locked_ && !forceMono_) ? 1.0 : 0.0;
        if (gate_ < target) {
            gate_ = std::min(target, gate_ + gateStep_);
        } else if (gate_ > target) {
            gate_ = std::max(target, gate_ - gateStep_);
        }

        // Real part: the composite itself -> (L+R) after the low-pass.
        // Imaginary part: the product detector -> (L-R) after the low-pass.
        // The factor 2 undoes the one-half from cos*cos, so a difference
        // channel modulated at unit amplitude comes back at unit amplitude.
        audioIn_[i] = {static_cast<float>(x),
                       static_cast<float>(2.0 * carrier38 * x * gate_)};
    }

    [[maybe_unused]] const std::size_t na =
        audioLpf_.process(audioIn_.data(), n, audioOut_.data());
    assert(na == n);

    // Stage 3: matrix, then de-emphasis per channel.
    const double p = deemphPole_;
    const double g = 1.0 - p;
    double sl = deemphLeft_;
    double sr = deemphRight_;
    for (std::size_t i = 0; i < n; ++i) {
        const double sum = static_cast<double>(audioOut_[i].real());
        const double dif = static_cast<double>(audioOut_[i].imag());
        // With the gate shut, dif is exactly 0.0 and these two expressions are
        // bit-identical: forced mono really is L == R, not L ~= R.
        const double l = 0.5 * (sum + dif);
        const double r = 0.5 * (sum - dif);
        sl = g * l + p * sl;
        sr = g * r + p * sr;
        left[i] = static_cast<float>(sl);
        right[i] = static_cast<float>(sr);
    }
    deemphLeft_ = sl;
    deemphRight_ = sr;

    return n;
}

}  // namespace cascade::dsp
