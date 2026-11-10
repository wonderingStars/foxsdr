// Feedback automatic gain control.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>

namespace cascade::dsp {

// Feedback AGC: every output sample's level is compared against a target and
// the gain is nudged by rate * (target - level) before the next sample.
//
// Feedback (measuring the OUTPUT) rather than feedforward (measuring the
// input) because the loop then corrects any gain error from whatever cause —
// including its own clamping — and the attack/decay asymmetry acts on the
// level the listener actually hears. The linear-in-error update makes the
// loop a first-order system with per-sample factor (1 - rate*|x|): for a
// constant-amplitude input with rate*|x| < 1 it converges exponentially to
// gain = target/|x| with no overshoot at all.
//
// Two rates, chosen by which side of the target the output is on:
//   - attackRate applies while the output is ABOVE target (gain falling);
//     conventionally fast, to catch loud onsets before they blast the ears.
//   - decayRate applies while the output is BELOW target (gain rising);
//     conventionally slow, so pauses do not pump the noise floor up audibly.
//
// The gain is clamped to [0, maxGain]. The upper clamp is what keeps silence
// from winding the gain toward infinity (on zero input the error is a constant
// +target, so the gain would ramp forever); the lower clamp keeps a violent
// transient from driving the gain negative and inverting the signal.
//
// MODES. Auto is the loop above, unchanged sample for sample: it is the
// default and every existing audio test and golden file was made with it. The
// other four are the user's choice:
//   Fast, Medium, Slow - a CONSTANT-SLEW loop: the gain moves by a fixed
//     number of dB per second (down at kTimedAttackDbPerS while the output is
//     above target, up at the mode's recovery slew while it is below), so the
//     time to recover from a level step is the same at every input level and,
//     because the slew is converted to a per-sample factor from the rate of the
//     samples process() is given (setSampleRateHz), the same at every channel
//     rate. Auto's error-proportional loop cannot offer that: its speed is
//     rate * |input|, i.e. it depends on how loud the signal is.
//   Off - the gain does not move. It is the value held at the moment of the
//     switch until setFixedGainDb() gives another.
class Agc {
public:
    enum class Mode { Auto, Fast, Medium, Slow, Off };

    // A step of 20 dB is "recovered" when the output is back within 3 dB of
    // target, i.e. after the gain has moved 17 dB. The three recovery slews are
    // that 17 dB over the recovery time the mode is named for.
    static constexpr float kRecoveredDb = 17.0f;
    static constexpr float kFastRecoverS = 0.1f;
    static constexpr float kMediumRecoverS = 0.5f;
    static constexpr float kSlowRecoverS = 2.0f;
    // Common to the three timed modes: 17 dB in 4.25 ms, inside the 10 ms the
    // design asks for.
    static constexpr float kTimedAttackDbPerS = 4000.0f;
    // The fixed-gain range, dB above unity: the range of the loop (0 to 100x).
    static constexpr float kFixedGainMinDb = 0.0f;
    static constexpr float kFixedGainMaxDb = 40.0f;
    // The multiplicative loops cannot leave zero by themselves, so they stop
    // at -60 dB (the input is demodulated audio, nominally within +-1).
    static constexpr float kTimedMinGain = 1.0e-3f;

    Agc(float targetLevel, float attackRate, float decayRate, float maxGain)
        : target_(targetLevel), attack_(attackRate), decay_(decayRate),
          maxGain_(maxGain) {
        computeTimedFactors();
    }

    float gain() const { return gain_; }

    // The gain in use, in dB (20 log10), floored at -120 so a zero gain is a
    // number a readout can show.
    float gainDb() const {
        return gain_ > 1.0e-6f ? 20.0f * std::log10(gain_) : -120.0f;
    }

    // Returns the gain to its initial neutral value - or, in Off, to the fixed
    // gain, which is the one gain Off ever applies.
    void reset() { gain_ = (mode_ == Mode::Off) ? fixedGain_ : 1.0f; }

    // The rate in Hz of the samples process() is given (for the pipeline: TWICE
    // the channel rate, it gates the interleaved stereo stream). The timed
    // modes' per-sample factors are derived from it. Call it whenever the rate
    // changes; it does not touch the gain.
    void setSampleRateHz(double hz) {
        if (hz > 0.0) { sampleRateHz_ = hz; }
        computeTimedFactors();
    }
    double sampleRateHz() const { return sampleRateHz_; }

    // Switching never resets the gain (no pump when the user changes speed),
    // except that entering Off freezes the gain held at that moment and reports
    // it as the fixed gain.
    void setMode(Mode m) {
        if (m == Mode::Off && mode_ != Mode::Off) {
            fixedGain_ = gain_;
        }
        mode_ = m;
        computeTimedFactors();
    }
    Mode mode() const { return mode_; }

    // The gain Off applies, clamped to [kFixedGainMinDb, kFixedGainMaxDb]. In
    // any other mode it is remembered, but the switch to Off freezes the HELD
    // gain (see setMode), so a caller that wants a given fixed gain sets it
    // AFTER the mode.
    void setFixedGainDb(float db) {
        // Written as a negated compare so a NaN lands on the minimum.
        if (!(db >= kFixedGainMinDb)) { db = kFixedGainMinDb; }
        db = std::min(db, kFixedGainMaxDb);
        fixedGain_ = std::pow(10.0f, db * (1.0f / 20.0f));
        if (mode_ == Mode::Off) { gain_ = fixedGain_; }
    }
    // The fixed gain in force, dB. After a switch to Off it is the frozen
    // value, which may lie below kFixedGainMinDb.
    float fixedGainDb() const {
        return fixedGain_ > 1.0e-6f ? 20.0f * std::log10(fixedGain_) : -120.0f;
    }

    // Real-valued audio path. In-place operation (out == in) is allowed:
    // each output sample is written before the next input is read.
    void process(const float* in, float* out, std::size_t n) {
        if (mode_ != Mode::Auto) {
            processOther(in, out, n);
            return;
        }
        for (std::size_t i = 0; i < n; ++i) {
            const float y = in[i] * gain_;
            out[i] = y;
            update(std::fabs(y));
        }
    }

    // Complex baseband path (pre-demod leveling). Gain is a real scalar, so
    // phase is untouched. In-place operation is allowed.
    void process(const std::complex<float>* in, std::complex<float>* out,
                 std::size_t n) {
        if (mode_ != Mode::Auto) {
            processOther(in, out, n);
            return;
        }
        for (std::size_t i = 0; i < n; ++i) {
            const std::complex<float> y = in[i] * gain_;
            out[i] = y;
            update(std::abs(y));
        }
    }

private:
    // Off, Fast, Medium, Slow: one mode test per block, and the per-sample
    // body is a compare and a multiply.
    template <typename T>
    void processOther(const T* in, T* out, std::size_t n) {
        if (mode_ == Mode::Off) {
            const float g = gain_;
            for (std::size_t i = 0; i < n; ++i) { out[i] = in[i] * g; }
            return;
        }
        float g = gain_;
        const float target = target_;
        const float up = decayMul_;      // output below target: gain rises
        const float down = attackMul_;   // output above target: gain falls
        const float hi = maxGain_;
        for (std::size_t i = 0; i < n; ++i) {
            const T y = in[i] * g;
            out[i] = y;
            g *= (std::abs(y) > target) ? down : up;
            // g is finite and positive here whatever the input (a NaN level
            // fails the compare and takes the rising branch), so plain
            // min/max cannot let a NaN through.
            g = std::min(std::max(g, kTimedMinGain), hi);
        }
        gain_ = g;
    }

    void computeTimedFactors() {
        float recoverS = kMediumRecoverS;
        if (mode_ == Mode::Fast) { recoverS = kFastRecoverS; }
        if (mode_ == Mode::Slow) { recoverS = kSlowRecoverS; }
        const double fs = sampleRateHz_;
        const double upDbPerS = static_cast<double>(kRecoveredDb) / recoverS;
        decayMul_ = static_cast<float>(std::pow(10.0, upDbPerS / 20.0 / fs));
        attackMul_ = static_cast<float>(std::pow(10.0, -kTimedAttackDbPerS / 20.0 / fs));
    }

    void update(float level) {
        const float rate = (level > target_) ? attack_ : decay_;
        gain_ += rate * (target_ - level);
        if (gain_ > maxGain_) { gain_ = maxGain_; }
        // Written as a negated >= so that NaN — which fails every ordered
        // comparison, including `gain_ < 0` — lands on 0 as well. A single
        // non-finite input sample makes the level NaN and the gain NaN with
        // it, and an unguarded clamp pair lets that NaN survive for the rest
        // of the session: the loop then multiplies every later sample by NaN
        // and the sound card is fed silence-shaped garbage forever. Zero is
        // the right landing point (silence, not a blast), and the decay side
        // of the loop winds the gain straight back up on the next clean
        // sample. Costs nothing: it is the same one compare as before.
        if (!(gain_ >= 0.0f)) { gain_ = 0.0f; }
    }

    float target_;
    float attack_;
    float decay_;
    float maxGain_;
    float gain_ = 1.0f;
    Mode mode_ = Mode::Auto;
    double sampleRateHz_ = 400000.0;  // 2 x a 200 kHz channel, until told
    float decayMul_ = 1.0f;   // timed modes: per-sample factor while rising
    float attackMul_ = 1.0f;  // ... and while falling
    float fixedGain_ = 1.0f;  // linear; what Off applies
};

}  // namespace cascade::dsp
