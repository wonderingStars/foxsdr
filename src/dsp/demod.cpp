// SPDX-License-Identifier: MIT
#include "dsp/demod.hpp"

#include <cassert>
#include <cmath>

namespace cascade::dsp {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// --- SSB design constants ---------------------------------------------------
//
// Method chosen for USB/LSB (documented per the module contract): Weaver-style
// frequency-shifted analytic selection, picked over the two textbook
// alternatives because it needs only primitives that already exist here:
//   - phasing (Hilbert) method: needs a wideband 90-degree FIR pair we do not
//     have, whose low-frequency ripple limits opposite-sideband rejection;
//   - filter-then-shift at RF: needs an asymmetric COMPLEX-tap band-pass;
//     FirDecimator taps are real.
// Weaver instead uses two Nco mixes around a REAL-tap low-pass:
//   1. mix by -B/2 (USB) or +B/2 (LSB): the wanted sideband, which occupies
//      [0,+B] (USB) or [-B,0] (LSB) of the centered channel, now straddles DC
//      symmetrically as [-B/2,+B/2];
//   2. low-pass at ~B/2: a real-tap FIR has a passband symmetric about DC, so
//      it keeps exactly the wanted sideband and rejects the opposite one
//      (which now sits in [-3B/2,-B/2] resp. [+B/2,+3B/2]);
//   3. mix back by the opposite BFO and take Re{}: every wanted component
//      returns to its original audio frequency, the rejected sideband is gone.
// The FIR group delay only adds a constant audio phase offset (inaudible);
// frequencies are restored exactly because the two BFOs cancel.
constexpr double kSsbAudioBandwidthHz = 3000.0;  // B: voice SSB channel width
constexpr double kSsbBfoHz = kSsbAudioBandwidthHz / 2.0;  // Weaver BFO = B/2
// Low-pass -6 dB cutoff in the shifted domain. 2100 Hz rather than the ideal
// B/2 = 1500 Hz: the windowed-sinc transition is centered on the cutoff, and
// pushing the cutoff up keeps the passband flat out to the B/2 band edge while
// the stopband still starts before the nearest strong opposite-sideband
// content (a mirror-image tone at audio frequency f lands at f + B/2 away
// from DC, i.e. 3 kHz for the 1.5 kHz worst case the tests pin).
constexpr double kSsbCutoffHz = 2100.0;
// Full transition width budget in Hz; sets the tap count via the standard
// Hamming windowed-sinc estimate (transition ~ 3.3/N of the sample rate).
constexpr double kSsbTransitionHz = 1000.0;
// CW sidetone: a carrier on the VFO center beats at this audio pitch.
constexpr double kCwToneHz = 700.0;

// --- WFM deemphasis ----------------------------------------------------------
// Broadcast FM pre-emphasizes highs with a 75 us RC network (Americas; Europe
// uses 50 us — a future setting, not a different algorithm); the receiver
// undoes it with the matching one-pole low-pass H(s) = 1/(1 + s*tau).
// Discretized by pole matching (impulse invariance): the analog pole at
// s = -1/tau maps to z = p = exp(-T/tau) with T = 1/rate, and the numerator is
// scaled so DC gain is exactly 1:
//     H(z) = (1 - p) / (1 - p*z^-1)
// Pole matching over bilinear because it preserves both the time constant and
// the DC gain exactly with no frequency prewarping decision; the deemphasis
// corner (1/(2*pi*tau) ~ 2.1 kHz) sits far below Nyquist at any WFM channel
// rate, where the two mappings agree closely anyway.
// Default de-emphasis: 50 us. That is the standard everywhere except the
// Americas and South Korea, so it is the correct global default; the setter
// below makes it a user choice rather than a compile-time assumption.
constexpr double kDefaultDeemphTauSec = 50e-6;

// --- AM DC blocker -------------------------------------------------------
// The envelope of an AM carrier is (carrier amplitude) + modulation: the
// carrier term is pure DC and would otherwise thump the audio chain and eat
// AGC headroom. Standard first-order blocker
//     y[n] = x[n] - x[n-1] + R*y[n-1]
// has an exact zero at DC and gain ~1 above its corner. R = exp(-2*pi*fc/T
// ... /rate) places the -3 dB corner at fc = 30 Hz: comfortably below program
// audio (AM broadcast rolls off below ~100 Hz) yet fast enough that the
// blocker settles in ~5 ms of samples at 48 kHz instead of drooping bass.
constexpr double kAmDcCutoffHz = 30.0;

// Sideband-select low-pass for the Weaver chain. Hamming window: its first
// stopband lobe is ~-53 dB, 13 dB beyond the 40 dB opposite-sideband-rejection
// target, at roughly half the tap count the Blackman-Harris window would
// spend for margin nobody can hear on SSB. Tap count scales with the rate so
// the transition width in Hz is rate-independent.
std::vector<float> designSsbTaps(double rate) {
    std::size_t n =
        static_cast<std::size_t>(std::ceil(3.3 * rate / kSsbTransitionHz));
    if (n < 63) { n = 63; }        // floor: keep >40 dB feasible at low rates
    if (n > 2047) { n = 2047; }    // cap: bound CPU at very high channel rates
    if (n % 2 == 0) { ++n; }       // windowedSincLowpass requires odd length
    double cutoff = kSsbCutoffHz / rate;
    // Degenerate very-low-rate guard: keep the design inside the (0, 0.5)
    // precondition instead of asserting deep in the filter designer.
    if (cutoff > 0.45) { cutoff = 0.45; }
    return windowedSincLowpass(n, cutoff, WindowType::Hamming);
}

}  // namespace

Demodulator::Demodulator(double channelRateHz)
    : rate_(channelRateHz), ssbFilter_(designSsbTaps(channelRateHz), 1) {
    assert(rate_ > 0.0);
    deemphTauSec_ = kDefaultDeemphTauSec;
    deemphPole_ = std::exp(-1.0 / (rate_ * deemphTauSec_));
    dcPole_ = std::exp(-kTwoPi * kAmDcCutoffHz / rate_);
    setMode(DemodMode::NFM);
}

void Demodulator::setDeemphasisUs(double us) {
    // Reject nonsense rather than poisoning the filter with a NaN pole; 0 (and
    // anything non-finite/negative treated as 0) means "no de-emphasis", which
    // the process loop implements as a pole of exactly 0 — a pass-through,
    // since y = (1-p)*x + p*y collapses to y = x at p = 0.
    if (!(us > 0.0) || !std::isfinite(us)) {
        deemphTauSec_ = 0.0;
        deemphPole_ = 0.0;
        deemphState_ = 0.0;
        return;
    }
    deemphTauSec_ = us * 1.0e-6;
    deemphPole_ = std::exp(-1.0 / (rate_ * deemphTauSec_));
    // Clear the filter memory: carrying a state charged at the old time
    // constant produces an audible thump on the switch.
    deemphState_ = 0.0;
}

void Demodulator::setMode(DemodMode m) {
    mode_ = m;
    const double bfo = kSsbBfoHz / rate_;
    switch (m) {
        case DemodMode::USB:
            // Wanted sideband [0,+B]: shift DOWN by B/2 to center it on DC.
            shiftDown_.setFrequency(-bfo);
            shiftUp_.setFrequency(+bfo);
            break;
        case DemodMode::LSB:
            // Mirror image: wanted sideband [-B,0] shifts UP by B/2.
            shiftDown_.setFrequency(+bfo);
            shiftUp_.setFrequency(-bfo);
            break;
        case DemodMode::CW:
            // USB with the input pre-shifted up by the sidetone pitch so a
            // carrier at DC beats at kCwToneHz. Both shifts are folded into
            // the first mix (+tone then -B/2 commute into one Nco); the
            // up-mix is unchanged, so audio = 700 + (carrier offset).
            shiftDown_.setFrequency(kCwToneHz / rate_ - bfo);
            shiftUp_.setFrequency(+bfo);
            break;
        default:
            // Non-SSB modes never run the Weaver chain; parking the BFOs at 0
            // just keeps the object's state fully determined by (mode, rate).
            shiftDown_.setFrequency(0.0);
            shiftUp_.setFrequency(0.0);
            break;
    }
    reset();
}

void Demodulator::reset() {
    quad_.reset();
    deemphState_ = 0.0;
    dcPrevIn_ = 0.0;
    dcPrevOut_ = 0.0;
    shiftDown_.reset();
    shiftUp_.reset();
    ssbFilter_.reset();
}

std::size_t Demodulator::process(const std::complex<float>* in, std::size_t n,
                                 float* out) {
    if (n == 0) { return 0; }
    switch (mode_) {
        case DemodMode::NFM:
            quad_.process(in, out, n);
            break;

        case DemodMode::WFM: {
            quad_.process(in, out, n);
            // In-place one-pole deemphasis over the discriminator output.
            const double p = deemphPole_;
            const double g = 1.0 - p;
            double s = deemphState_;
            for (std::size_t i = 0; i < n; ++i) {
                s = g * static_cast<double>(out[i]) + p * s;
                out[i] = static_cast<float>(s);
            }
            deemphState_ = s;
            break;
        }

        case DemodMode::AM: {
            // Envelope detector: |x| ignores carrier phase/frequency error
            // entirely (unlike Re{x}), which is the point of envelope AM.
            const double r = dcPole_;
            double prevIn = dcPrevIn_;
            double prevOut = dcPrevOut_;
            for (std::size_t i = 0; i < n; ++i) {
                const double e = static_cast<double>(std::abs(in[i]));
                const double y = e - prevIn + r * prevOut;
                prevIn = e;
                prevOut = y;
                out[i] = static_cast<float>(y);
            }
            dcPrevIn_ = prevIn;
            dcPrevOut_ = prevOut;
            break;
        }

        case DemodMode::DSB:  // product detector with the carrier at DC: the
                              // multiply is by 1, leaving Re{x} — same code as
        case DemodMode::RAW:  // the diagnostic passthrough, kept as one case.
            for (std::size_t i = 0; i < n; ++i) { out[i] = in[i].real(); }
            break;

        case DemodMode::USB:
        case DemodMode::LSB:
        case DemodMode::CW:
            processSsb(in, n, out);
            break;
    }
    return n;
}

void Demodulator::processSsb(const std::complex<float>* in, std::size_t n,
                             float* out) {
    work_.resize(n);
    filtered_.resize(ssbFilter_.outputCapacity(n));
    shiftDown_.mix(in, work_.data(), n);
    // Decimation 1 => the grid phase is always 0 and every input yields one
    // output, so m == n and the 1:1 process() contract holds.
    const std::size_t m = ssbFilter_.process(work_.data(), n, filtered_.data());
    assert(m == n);
    shiftUp_.mix(filtered_.data(), filtered_.data(), m);
    for (std::size_t i = 0; i < m; ++i) { out[i] = filtered_[i].real(); }
}

}  // namespace cascade::dsp
