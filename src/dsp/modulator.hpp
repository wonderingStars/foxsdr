// modulator.hpp - a block of audio in, a block of complex baseband out. The
// demodulators in dsp/demod.hpp, run backwards.
//
// WHAT IT IS AND IS NOT. This is arithmetic and nothing else: no device, no
// thread, no clock, no state beyond the filter histories a streaming
// modulator has to carry between blocks. It does not know what rate the radio
// runs at, because it does not resample - it produces complex baseband at the
// AUDIO rate and something above it (core/transmitter.hpp) interpolates to
// whatever the board is clocked at. That split is what lets every claim below
// be checked with a direct transform in tests/test_modulator.cpp and no
// hardware at all, which matters here more than anywhere else in this
// product: there is no Pluto on this bench, and a modulator whose sidebands
// are on the wrong side is a transmitter interfering with a band nobody
// looked at.
//
// THE FIVE MODES, and what each one actually produces:
//
//   CW   A KEYED CARRIER, which at baseband is a real envelope with no phase
//        modulation at all - the tone is the LO. The envelope is raised-
//        cosine over kCwRampMs at each edge, and that shaping is the whole
//        substance of the mode: a carrier switched on in one sample is a step
//        function, and a step function's spectrum is everywhere. "Key clicks"
//        are that, heard several kilohertz away by people who are not being
//        transmitted to.
//
//   AM   carrier plus two sidebands: (1 + m*a) / 2, real. Halved so a full
//        modulation peak lands at 1.0 rather than clipping, and m is capped
//        below 1 so the envelope cannot go through zero - over-modulated AM
//        splatters for the same reason a hard-keyed carrier does.
//
//   NFM  constant envelope, phase accumulating at 2*pi*dev*a/fs. Two things
//        sit in front of that and both are required rather than decorative:
//        a PRE-EMPHASIS that lifts the top of the voice band, because FM's
//        own noise spectrum rises with frequency and every receiver de-
//        emphasises to match (FoxSDR's own does, in dsp/demod.cpp); and a
//        DEVIATION LIMIT, which is a hard clamp on the per-sample phase step
//        and is what keeps a shouted word inside the channel instead of in
//        the next one.
//
//   USB  the analytic signal, x + j*H{x}, where H is a windowed Hilbert
//   LSB  transformer; LSB is its conjugate, x - j*H{x}. The in-phase arm is
//        delayed by the transformer's own group delay, because the two arms
//        must be the same signal to within a quarter turn - an undelayed I
//        against a filtered Q is not a sideband, it is both of them.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_DSP_MODULATOR_HPP
#define CASCADE_DSP_MODULATOR_HPP

#include <complex>
#include <cstddef>
#include <vector>

namespace cascade::dsp {

// The order is the order the TRANSMIT page's selector shows them in, and the
// numbers are NOT a contract: config.json stores the id from txModeId().
enum class TxMode : int { CW = 0, AM, NFM, USB, LSB, Count };

inline constexpr int kTxModeCount = static_cast<int>(TxMode::Count);

// The word on the key, and the word in config.json - the same word, because
// there are five of them and they are already the names everybody uses.
const char* txModeName(TxMode m);

// A saved index, clamped to one that exists. A hand-edited file can say
// anything; whatever it says, the page opens on a mode this build has.
TxMode txModeFromIndex(int index);

// Name -> mode, for the config. False for anything the table does not carry.
bool txModeFromName(const char* name, TxMode& out);

// One sentence for the page, saying what the mode puts on the air.
const char* txModeCaption(TxMode m);

// --- the numbers ------------------------------------------------------------

// Peak deviation for NFM, in Hz. 2.5 kHz is the narrow-band figure the 12.5
// kHz channel raster is built on; a receiver expecting it will sound quiet if
// this is smaller and distorted if it is larger.
inline constexpr double kNfmDeviationHz = 2500.0;

// The pre-emphasis time constant. 750 us is the narrow-band FM value (a 212
// Hz corner), as against the 50 or 75 us of broadcast FM - which is what
// FoxSDR's own receiver de-emphasises with, and why this is not that number.
inline constexpr double kNfmPreemphasisTau = 750.0e-6;

// The AM modulation index. Below 1 deliberately: at m = 1 an audio peak takes
// the envelope exactly to zero and any overshoot at all inverts it, which
// sounds like distortion to a listener and looks like splatter to everybody
// else.
inline constexpr float kAmDepth = 0.9f;

// How long a CW envelope takes to rise and to fall. 5 ms is the usual
// compromise: shorter starts to click, longer starts to soften the character
// of the keying at speed.
inline constexpr double kCwRampMs = 5.0;

// Taps in the Hilbert transformer. Odd, because the delay has to be a whole
// number of samples for the in-phase arm to be delayable by a plain shift,
// and 129 at 48 kHz puts the opposite sideband about 40 dB down across the
// voice band - which is measured in the tests rather than claimed here.
inline constexpr int kHilbertTaps = 129;

// --- the modulator ----------------------------------------------------------

class Modulator {
public:
    Modulator();

    // The audio rate the blocks arrive at, and the rate the complex output is
    // produced at - they are the same, because this stage does not resample.
    void setSampleRateHz(double hz);
    double sampleRateHz() const { return sampleRateHz_; }

    void setMode(TxMode m);
    TxMode mode() const { return mode_; }

    // THE KEY, and it is not the same thing as "is the transmitter running".
    // In CW it raises and lowers the envelope through its raised-cosine ramp;
    // in every other mode it is the gate that fades the modulation in and out
    // so that keying up in the middle of a word does not put a step on the
    // air either. A caller that keys down must keep calling process() until
    // idle() is true, or the ramp it asked for never gets played.
    void setKeyed(bool on);
    bool keyed() const { return keyed_; }

    // True when the envelope has reached zero after a key-up: the transmitter
    // may stop feeding the radio. Always false while keyed.
    bool idle() const;

    // NFM's pre-emphasis. On in the product; the tests turn it off so a
    // Bessel spectrum can be compared against the closed form rather than
    // against the closed form filtered by something.
    void setPreemphasis(bool on);
    bool preemphasis() const { return preemphasis_; }

    void setDeviationHz(double hz);
    double deviationHz() const { return deviationHz_; }

    // The largest deviation the last process() call actually produced, in Hz.
    // How the limiter is proven to limit, and what a panel would letter under
    // a deviation meter.
    double lastPeakDeviationHz() const { return peakDeviationHz_; }

    // Forgets every history: the pre-emphasis state, the Hilbert delay line,
    // the phase accumulator and the envelope. The envelope goes to ZERO and
    // the key goes UP, not to whatever they were - a modulator that was reset
    // mid-transmission must not resume mid-word.
    void reset();

    // n samples of mono audio in [-1, 1] -> n complex baseband samples. `out`
    // may not alias `audio`. Everything produced is inside the unit circle:
    // see the per-mode note in the file header for how each one is scaled to
    // get there, and packSample() in source/iiod_client.hpp for what happens
    // to anything that is not.
    void process(const float* audio, std::size_t n, std::complex<float>* out);

private:
    void rebuildHilbert();
    void rebuildPreemphasis();
    // The envelope for one sample, advancing the ramp. 0 when fully unkeyed,
    // 1 when fully keyed.
    float nextEnvelope();

    double sampleRateHz_ = 48000.0;
    TxMode mode_ = TxMode::CW;
    bool keyed_ = false;
    bool preemphasis_ = true;
    double deviationHz_ = kNfmDeviationHz;
    double peakDeviationHz_ = 0.0;

    // The envelope ramp, in [0, 1], and how much of it one sample crosses.
    double envelope_ = 0.0;
    double envelopeStep_ = 1.0;

    // NFM's phase accumulator, in cycles - the same unit dsp/nco.hpp keeps
    // its phase in, and for the same reason: wrapping by the exactly
    // representable 1.0 adds no rounding error of its own, so a long
    // transmission does not accumulate phase drift.
    double phaseCycles_ = 0.0;

    // Pre-emphasis: y[n] = g * (x[n] - a * x[n-1]), with g chosen so the
    // filter's gain at 1 kHz is exactly 1. Normalising at 1 kHz rather than
    // at DC is what keeps the overall loudness of a voice the same when the
    // pre-emphasis is switched in - normalising at DC would make the whole
    // band 30 dB quieter and let the limiter do nothing.
    double preA_ = 0.0;
    double preG_ = 1.0;
    double preLast_ = 0.0;

    std::vector<float> hilbert_;
    // The delay line for both arms, oldest first. Long enough to hold the
    // whole transformer, so the in-phase arm's delayed sample and the
    // quadrature arm's convolution come out of the same buffer.
    std::vector<float> delay_;
};

// --- the test tone ----------------------------------------------------------
//
// THE OTHER THING A TRANSMITTER NEEDS, and it is not a luxury. The first
// question about any transmit path is "is anything coming out at all", and
// the answer must not depend on a microphone being plugged in, being
// unmuted, and being talked into. A steady tone is a signal whose spectrum is
// one line, so it is also the only input that makes a spectrum analyser
// readable.
class ToneGenerator {
public:
    void setSampleRateHz(double hz);
    void setFrequencyHz(double hz);
    void setLevel(float level01);

    double frequencyHz() const { return freqHz_; }
    float level() const { return level_; }

    void reset() { phaseCycles_ = 0.0; }

    // n samples of a sine at frequencyHz(), scaled by level().
    void generate(float* out, std::size_t n);

private:
    double sampleRateHz_ = 48000.0;
    double freqHz_ = 1000.0;
    double phaseCycles_ = 0.0;
    float level_ = 0.5f;
};

// The default test tone. 1 kHz because it is in the middle of every voice
// passband there is, and half scale because a test signal at full scale tells
// you nothing about whether the path clips.
inline constexpr double kToneDefaultHz = 1000.0;
inline constexpr float kToneDefaultLevel = 0.5f;

}  // namespace cascade::dsp

#endif  // CASCADE_DSP_MODULATOR_HPP
