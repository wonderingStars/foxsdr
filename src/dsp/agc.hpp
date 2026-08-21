// Feedback automatic gain control.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

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
class Agc {
public:
    Agc(float targetLevel, float attackRate, float decayRate, float maxGain)
        : target_(targetLevel), attack_(attackRate), decay_(decayRate),
          maxGain_(maxGain) {}

    float gain() const { return gain_; }

    // Returns the gain to its initial neutral value.
    void reset() { gain_ = 1.0f; }

    // Real-valued audio path. In-place operation (out == in) is allowed:
    // each output sample is written before the next input is read.
    void process(const float* in, float* out, std::size_t n) {
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
        for (std::size_t i = 0; i < n; ++i) {
            const std::complex<float> y = in[i] * gain_;
            out[i] = y;
            update(std::abs(y));
        }
    }

private:
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
};

}  // namespace cascade::dsp
