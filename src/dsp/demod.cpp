// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/demod.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace cascade::dsp {

// Mode names, in ENUM order. The one table; see demod.hpp for why it lives
// here rather than in each of the three places that need it.
namespace {
constexpr const char* kModeText[kDemodModeCount] = {"NFM", "WFM", "AM",  "DSB",
                                                    "USB", "LSB", "CW", "RAW"};
}  // namespace

const char* modeName(DemodMode m) {
    const auto i = static_cast<std::size_t>(m);
    // Never returns null, even for a value cast in from outside the enum:
    // callers put this straight into printf and JSON.
    return (i < kDemodModeCount) ? kModeText[i] : "NFM";
}

bool modeFromName(const std::string& name, DemodMode& out) {
    for (std::size_t i = 0; i < kDemodModeCount; ++i) {
        if (name == kModeText[i]) {
            out = static_cast<DemodMode>(i);
            return true;
        }
    }
    return false;
}

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
//   1. mix by -f0 (USB) or +f0 (LSB): the wanted sideband, which occupies
//      [lo,hi] (USB) or [-hi,-lo] (LSB) of the centered channel, now straddles
//      DC symmetrically as [-c,+c], c = (hi-lo)/2, f0 = (hi+lo)/2;
//   2. low-pass at c: a real-tap FIR has a passband symmetric about DC, so it
//      keeps exactly the wanted sideband and rejects the opposite one (an
//      opposite-sideband component at audio frequency x >= 0 lands at
//      -(f0+x), which is at least f0 = c + lo from DC);
//   3. mix back by the opposite BFO and take Re{}: every wanted component
//      returns to its original audio frequency, the rejected sideband is gone.
// The FIR group delay only adds a constant audio phase offset (inaudible);
// frequencies are restored exactly because the two BFOs cancel.
//
// WHY THE BFO IS NOT B/2 (0.99.82). The wanted and the opposite sideband meet
// AT THE CARRIER, and after the down-mix they meet at -f0 on the low-pass's
// skirt. With f0 = B/2 and the cutoff pushed out to 0.7 B (the design up to
// 0.99.81) the skirt sat 600 Hz INSIDE the opposite sideband: an opposite-
// sideband tone at 100 Hz came out at 0 dB, 500 Hz at -3 dB, 1 kHz at -32 dB,
// and only above ~1.25 kHz was it the 50 dB and more the tests were written
// for (they probed 1 kHz and 1.5 kHz only). The low half of a voice, where
// most of its energy is, was therefore heard from BOTH sidebands: a DSB
// receiver below 1 kHz. The skirt has to lie wholly on the WANTED side of the
// carrier, which means giving up the audio below the transition: the carrier-
// side -6 dB edge is half the transition width, as in any SSB filter (300 Hz
// by convention), and the opposite sideband is stopped from DC outward rather
// than from 1 kHz outward.
//
// B IS A SETTING (setSsbBandwidthHz), DEFAULT 3 kHz. Until 0.99.60 it was a
// constant, so the receiver's Bandwidth control reached the SSB modes only
// through the channel filter - which did not narrow anything (see dsp/vfo.hpp)
// - and a user who picked 6 kHz or 10 kHz in USB heard exactly the same 3 kHz
// of audio. The numbers below are the 3 kHz design; every other width scales
// them in proportion.
constexpr double kSsbAudioBandwidthHz = 3000.0;  // B: voice SSB channel width
// The audio top: the sideband's -6 dB point is this multiple of B (3.6 kHz at
// the default), the figure the demodulator has always documented.
constexpr double kSsbUpperEdgeFactor = 1.2;
// Full transition width budget in Hz; sets the tap count via the standard
// Hamming windowed-sinc estimate (transition ~ 3.3/N of the sample rate). Half
// of it is the carrier-side -6 dB edge (300 Hz) and the opposite sideband is
// down by the window's full stopband from DC outward. It scales DOWN with B
// below the 3 kHz default (a 1 kHz sideband cannot afford a 300 Hz hole) but
// never up: a wider sideband keeps the same 300 Hz edge, because the low audio
// of a wide-SSB listener is what they chose the width for - and because the
// pipeline's untouched default B (150 kHz, clamped to 0.3 of the channel rate)
// would otherwise put the hole at several kHz and eat a 700 Hz CW tone.
constexpr double kSsbTransitionHz = 600.0;
// The widths setSsbBandwidthHz accepts: 1 kHz is about the narrowest the tap
// cap (2047) can still build with its transition intact, and 0.3 x the channel
// rate is where the shifted band (B/2 + the 0.7 B cutoff) stops fitting.
constexpr double kSsbMinBandwidthHz = 1000.0;
constexpr double kSsbMaxRateFraction = 0.3;
// CW sidetone: a carrier on the VFO center beats at this audio pitch.
constexpr double kCwToneHz = 700.0;

// --- FM deemphasis -----------------------------------------------------------
// The one-pole network, its derivation and its 50 us default live in
// dsp/deemphasis.hpp (0.99.69: the airband monitor's FM strips run the same
// filter, so it is one class rather than two copies of the arithmetic).
//
// ONE network, shared by NFM. Nothing in the derivation depends on the channel
// being wide, and the receiver's De-emph control offers the same two constants
// for both FM modes — but process() used to apply the filter in its WFM case
// alone, which is what left that control inert on the narrowband mode a pager,
// APRS or weather-satellite listener is tuned to.

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
// the transition width in Hz is rate-independent. The BFO comes from the same
// computation (ssbShape) because the two are one design: the cutoff and the
// mix frequency together place the skirt.
struct SsbShape {
    std::size_t taps;   // odd FIR length
    double cutoffHz;    // c: low-pass -6 dB point in the shifted domain
    double bfoHz;       // f0: the two mixes' frequency
};

SsbShape ssbShape(double rate, double bandwidthHz) {
    // The carrier-side transition scales with B/3000 below the default and is
    // held at kSsbTransitionHz above it (see there).
    const double scale = std::min(1.0, bandwidthHz / kSsbAudioBandwidthHz);
    std::size_t n =
        static_cast<std::size_t>(std::ceil(3.3 * rate / (kSsbTransitionHz * scale)));
    if (n < 63) { n = 63; }        // floor: keep >40 dB feasible at low rates
    if (n > 2047) { n = 2047; }    // cap: bound CPU at very high channel rates
    if (n % 2 == 0) { ++n; }       // windowedSincLowpass requires odd length
    // The transition the filter REALLY has once the tap count is rounded and
    // capped: its half is the carrier-side edge, so a capped (narrow B, high
    // rate) filter widens the audio hole rather than leaking the other side.
    const double transition = 3.3 * rate / static_cast<double>(n);
    const double lo = 0.5 * transition;
    const double hi = kSsbUpperEdgeFactor * bandwidthHz;
    return {n, 0.5 * (hi - lo), 0.5 * (hi + lo)};
}

std::vector<float> designSsbTaps(double rate, double bandwidthHz) {
    const SsbShape s = ssbShape(rate, bandwidthHz);
    double cutoff = s.cutoffHz / rate;
    // Degenerate very-low-rate guard: keep the design inside the (0, 0.5)
    // precondition instead of asserting deep in the filter designer.
    if (cutoff > 0.45) { cutoff = 0.45; }
    return windowedSincLowpass(s.taps, cutoff, WindowType::Hamming);
}

}  // namespace

Demodulator::Demodulator(double channelRateHz)
    : rate_(channelRateHz),
      deemph_(channelRateHz),   // 50 us, the global default
      ssbBfoHz_(ssbShape(channelRateHz, kSsbAudioBandwidthHz).bfoHz),
      ssbFilter_(designSsbTaps(channelRateHz, kSsbAudioBandwidthHz), 1) {
    assert(rate_ > 0.0);
    dcPole_ = std::exp(-kTwoPi * kAmDcCutoffHz / rate_);
    setMode(DemodMode::NFM);
}

void Demodulator::setDeemphasisUs(double us) {
    // Nonsense (and 0) means "no de-emphasis", and the filter clears its memory
    // either way: see dsp/deemphasis.hpp.
    deemph_.setTimeUs(us);
}

void Demodulator::setSsbBandwidthHz(double bandwidthHz) {
    // A non-finite request lands on the default, so a bad value can never
    // poison the filter design.
    double bw = std::isfinite(bandwidthHz) ? bandwidthHz : kSsbAudioBandwidthHz;
    bw = std::min(std::max(bw, kSsbMinBandwidthHz), kSsbMaxRateFraction * rate_);
    if (bw == ssbBandwidthHz_) { return; }  // idempotent: no gratuitous reset
    ssbBandwidthHz_ = bw;
    ssbBfoHz_ = ssbShape(rate_, bw).bfoHz;
    ssbFilter_ = FirDecimator(designSsbTaps(rate_, bw), 1);
    // The BFOs follow B. Only the sideband modes run the Weaver chain, so only
    // they are restarted (a documented clean start, like a mode change); AM, FM
    // and the rest keep their state untouched and pick the new width up at the
    // next setMode into a sideband mode.
    if (mode_ == DemodMode::USB || mode_ == DemodMode::LSB || mode_ == DemodMode::CW) {
        setMode(mode_);
    }
}

void Demodulator::setMode(DemodMode m) {
    mode_ = m;
    const double bfo = ssbBfoHz_ / rate_;  // Weaver BFO = f0, see ssbShape
    switch (m) {
        case DemodMode::USB:
            // Wanted sideband [lo,hi]: shift DOWN by f0 to center it on DC.
            shiftDown_.setFrequency(-bfo);
            shiftUp_.setFrequency(+bfo);
            break;
        case DemodMode::LSB:
            // Mirror image: wanted sideband [-hi,-lo] shifts UP by f0.
            shiftDown_.setFrequency(+bfo);
            shiftUp_.setFrequency(-bfo);
            break;
        case DemodMode::CW:
            // USB with the input pre-shifted up by the sidetone pitch so a
            // carrier at DC beats at kCwToneHz. Both shifts are folded into
            // the first mix (+tone then -f0 commute into one Nco); the
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
    deemph_.reset();
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
        case DemodMode::WFM: {
            quad_.process(in, out, n);
            // In-place one-pole deemphasis over the discriminator output.
            //
            // BOTH FM modes run it, from the one time constant. NFM used to
            // fall out of the switch here with no filter at all, so the
            // De-emph control — which the receiver enables for NFM and WFM
            // alike, and persists — did nothing whatsoever in NFM. Owners that
            // de-emphasise downstream instead set 0 us (the pipeline does, for
            // WFM, where StereoFm applies the network after the stereo matrix):
            // a pole of exactly 0 makes g == 1 and the loop an exact
            // pass-through, so no signal is ever de-emphasised twice.
            deemph_.process(out, n);
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
