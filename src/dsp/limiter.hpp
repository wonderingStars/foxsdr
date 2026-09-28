// Output peak limiter: the last-resort guard between the audio chain and any
// consumer that quantizes to a fixed range (int16 in the recorder, the sound
// card's own clipping in the sink).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>

namespace cascade::dsp {

// Below kLimiterKnee this is the identity function — bit-exact, not merely
// "close" — so ordinary audio (the Agc's whole job is to sit near its 0.5
// target) is never touched and the limiter changes nothing about normal
// listening loudness. Only the sliver of amplitude ABOVE the knee is
// compressed, smoothly, onto (kLimiterKnee, kLimiterCeiling): the exponential
// approach never reaches, let alone exceeds, the ceiling for any finite
// input, so the output is unconditionally bounded strictly inside [-1, 1] —
// no per-signal assumption required, unlike the Agc, which only converges
// without overshoot for a CONSTANT-amplitude input (see agc.hpp). A signal
// with high crest factor — wideband noise let in by a widened channel filter
// is exactly this shape — can otherwise ride well above the Agc's target for
// many samples in a row while the feedback loop catches up, and every one of
// those samples used to be truncated flat at the int16 quantizer, which is
// what a listener hears as crackle rather than the softer sound of a single
// clipped peak.
//
// Continuous in VALUE and SLOPE at the knee (both sides evaluate to
// kLimiterKnee with unit slope there), so there is no audible kink or
// discontinuity in the transfer function at the one point a transient
// crosses it.
inline constexpr float kLimiterKnee = 0.97f;
inline constexpr float kLimiterCeiling = 0.99f;

inline float softLimit(float x) {
    const float sign = (x < 0.0f) ? -1.0f : 1.0f;
    const float a = std::fabs(x);
    if (!(a > kLimiterKnee)) {
        // Also the NaN path (every ordered comparison with NaN is false):
        // passed through unchanged, exactly as before this limiter existed —
        // the recorder's own NaN guard downstream is unaffected either way.
        return x;
    }
    constexpr float span = kLimiterCeiling - kLimiterKnee;
    const float over = a - kLimiterKnee;
    return sign * (kLimiterKnee + span * (1.0f - std::exp(-over / span)));
}

}  // namespace cascade::dsp
