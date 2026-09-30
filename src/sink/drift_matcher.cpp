// Holds the sound card's lead steady against a radio on a different clock -
// implementation. See drift_matcher.hpp for why.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "sink/drift_matcher.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace cascade::sink {

namespace {

// The loop, in the units the plant works in. A correction of c ppm moves the
// lead by c * 1e-3 ms per second, so:
//
//   kKpPpmPerMs  50  -> a lost lead decays back with a 20 s time constant
//                       (and 80 ms of loss asks for 4000 ppm, under the cap);
//   kKiPpmPerMsS  0.625 -> sqrt(Ki * 1e-3) = 0.025 rad/s against
//                       Kp * 1e-3 / 2 = 0.025: critically damped, so a step
//                       settles without ringing and a steady mismatch is
//                       learned in about a minute.
//
// The fill is smoothed first because the raw level is a sawtooth: the sound
// card takes whole device periods and the demodulator writes whole blocks,
// tens of milliseconds each. One second of smoothing is far faster than the
// loop and flattens that to a couple of milliseconds, which is a few tenths
// of a cent of pitch.
constexpr double kKpPpmPerMs = 50.0;
constexpr double kKiPpmPerMsS = 0.625;
constexpr double kFillSmoothingS = 1.0;

// 4-point, third-order Hermite (Catmull-Rom) between x1 and x2, t in [0, 1).
// Returns x1 exactly at t = 0, so a ratio of exactly 1 is a pure delay.
inline float hermite(float x0, float x1, float x2, float x3, float t) {
    const float c1 = 0.5f * (x2 - x0);
    const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * t + c2) * t + c1) * t + x1;
}

}  // namespace

void DriftMatcher::reset() {
    correctionPpm_ = 0.0;
    integralPpm_ = 0.0;
    fillAvgFrames_ = 0.0;
    fillAvgValid_ = false;
    channels_ = 0;
    hist_.fill(0.0f);
    pos_ = 1.0;
}

void DriftMatcher::observe(std::size_t fillFrames, bool playing, std::size_t blockFrames) {
    if (!playing) {
        // Priming, re-priming after a starvation, or no device at all. The
        // fill is climbing towards the prime threshold for reasons that have
        // nothing to do with the clocks, so it is not evidence. Keep the
        // learned mismatch (the clocks have not changed) and drop the
        // proportional push, and start the average afresh when playback does.
        fillAvgValid_ = false;
        correctionPpm_ = integralPpm_;
        return;
    }
    const double dt = static_cast<double>(blockFrames) / rateHz_;
    const double fill = static_cast<double>(fillFrames);
    if (!fillAvgValid_) {
        fillAvgFrames_ = fill;
        fillAvgValid_ = true;
    } else {
        const double a = dt / (kFillSmoothingS + dt);
        fillAvgFrames_ += a * (fill - fillAvgFrames_);
    }
    // Positive when the lead is short: play more frames than arrive.
    const double errMs = (kTargetFrames - fillAvgFrames_) * 1000.0 / rateHz_;
    integralPpm_ = std::clamp(integralPpm_ + kKiPpmPerMsS * errMs * dt,
                              -kMaxDriftPpm, kMaxDriftPpm);
    correctionPpm_ = std::clamp(kKpPpmPerMs * errMs + integralPpm_,
                                -kMaxCorrectionPpm, kMaxCorrectionPpm);
}

std::size_t DriftMatcher::process(const float* in, std::size_t frames,
                                  std::size_t channels, float* out,
                                  std::size_t outCapFrames) {
    if (channels != 1 && channels != 2) { return 0; }
    if (channels != channels_) {
        // A layout change (the sink fell back to mono, or came back to
        // stereo) - the history is in the wrong shape, so start clean.
        channels_ = channels;
        hist_.fill(0.0f);
        pos_ = 1.0;
    }
    if (frames == 0) { return 0; }

    // The stream is the three history frames followed by this block; frame k
    // of it is hist_ for k < 3 and in[k - 3] after.
    const std::size_t total = frames + 3;
    auto at = [&](std::size_t k, std::size_t c) -> float {
        return k < 3 ? hist_[k * channels + c] : in[(k - 3) * channels + c];
    };

    const double step = 1.0 / ratio();
    std::size_t produced = 0;
    // Output frame at pos_ needs frames floor(pos_) - 1 .. floor(pos_) + 2.
    while (produced < outCapFrames) {
        const auto i = static_cast<std::size_t>(pos_);
        if (i + 2 >= total) { break; }
        const float t = static_cast<float>(pos_ - static_cast<double>(i));
        for (std::size_t c = 0; c < channels; ++c) {
            out[produced * channels + c] =
                hermite(at(i - 1, c), at(i, c), at(i + 1, c), at(i + 2, c), t);
        }
        ++produced;
        pos_ += step;
    }

    // Keep the last three frames as the next block's history and move the
    // read position into their frame of reference. The loop can only stop
    // early on outCapFrames, which maxOut() sizing never hits; if a caller
    // under-sizes anyway, the unread part of this block is dropped rather
    // than letting pos_ run away.
    std::array<float, 6> keep{};
    for (std::size_t k = 0; k < 3; ++k) {
        for (std::size_t c = 0; c < channels; ++c) {
            keep[k * channels + c] = at(total - 3 + k, c);
        }
    }
    hist_ = keep;
    // Normally lands in [1, 1 + step): step is at most 1.005, so a value
    // just past 2 is a legitimate position and is kept as it is.
    pos_ -= static_cast<double>(frames);
    if (pos_ < 1.0) { pos_ = 1.0 + (pos_ - std::floor(pos_)); }
    return produced;
}

}  // namespace cascade::sink
