// Single-channel spectral noise reduction for real audio (STFT overlap-add).
//
// The suppression rule is plain spectral subtraction / Wiener gain: estimate a
// per-bin noise floor, keep the fraction of each bin that the estimate says is
// signal, clamp the result to a floor so nothing is ever fully muted. Both the
// rule (Wiener, 1949; Boll's spectral subtraction, 1979) and the STFT
// overlap-add machinery are foundational, long-published DSP with no live
// patent surface — which is the whole reason they were chosen for a product
// that ships commercially. No proprietary or unclear-provenance enhancement
// (psychoacoustic masking models, learned priors, vendor "AI" denoisers) is
// used anywhere in this file.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

#include "dsp/fft.hpp"

namespace cascade::dsp {

// Streaming spectral noise reduction.
//
// STRUCTURE. Weighted overlap-add at 50% overlap: every hop = fftSize/2 input
// samples, the most recent fftSize samples are windowed, transformed, scaled
// per bin, transformed back, windowed again and added into an output
// accumulator.
//
// WINDOW. Analysis and synthesis both use sqrt(Hann), so their PRODUCT is
// Hann — and periodic Hann at 50% overlap satisfies COLA exactly: the two
// overlapping copies sum to 1.0 at every sample, because shifting the window
// by N/2 negates its cosine term. That exact-unity sum is what makes the
// overlap-add reconstruct the input when the per-bin gains are unity, which is
// in turn what the bypass-accuracy test in tests/test_noise_reduction.cpp
// measures. Note the sqrt: applying a FULL Hann on both sides at 50% overlap
// would sum to 0.75 + 0.25*cos(4*pi*n/N) — a 33% ripple, not COLA. A synthesis
// window is used at all (rather than Hann analysis and no synthesis window,
// which is also COLA at 50%) because modifying the spectrum makes the inverse
// transform discontinuous at the frame edges, and the synthesis taper is what
// keeps those discontinuities from becoming an audible buzz at the hop rate.
//
// NOISE FLOOR. Per bin, an asymmetric decay tracker over the smoothed frame
// power: it falls quickly toward a lower level and rises back roughly twelve
// times more slowly, so it settles near the low end of that bin's power
// distribution. That is the classical minimum-statistics idea — noise is what
// is still there during the quiet moments — reduced to its unencumbered core,
// with the asymmetric pole standing in for a hard running minimum because a
// true minimum's downward bias depends on its horizon and would have to be
// compensated by a fudge factor to be usable at all.
//
// Minimum-flavoured tracking ALONE has a fatal failure mode for a receiver: a
// continuous
// carrier or heterodyne never gets quiet, so its own bin's minimum converges
// onto the tone and the suppressor deletes exactly the signal the user is
// listening to. So the per-bin estimate is additionally capped at
// kMaxTilt above the cross-bin median of the smoothed spectrum. Receiver
// noise is broadband; a bin standing far above the median of all bins is
// signal, not floor. The cap is loose enough (10 dB) that genuinely coloured
// noise is still tracked bin by bin.
//
// GAIN. g = 1 - overSubtraction * noise / power, clamped into
// [gainFloor, 1]. Both knobs are driven by strength: the oversubtraction runs
// 1 -> 3 (subtracting more than the estimate is what compensates the
// deliberate downward bias of the floor tracker), and the gain floor runs
// 1 -> 0.05.
// A floor rather than a hard zero is the standard musical-noise control: bins
// that flicker across the threshold move between g and the floor instead of
// between g and silence, so the residual is a quiet steady hiss instead of a
// shower of tonal blips. The gain is also averaged with the previous frame's
// gain for the same bin, which smooths that flicker in time.
//
// LATENCY is exactly fftSize samples: one hop for the overlap-add to finish
// reconstructing a sample (a sample is only complete once both frames covering
// it have been added), plus one hop of output priming so that every call can
// return as many samples as it consumed. process() therefore always writes and
// returns n, with out[j] corresponding to in[j - fftSize]; the first fftSize
// output samples of a fresh stream are the priming zeros.
//
// BYPASS. strength <= 0 or disabled takes a plain delay line of latency
// samples, so bypass is BIT-EXACT, not merely accurate to float precision.
// The delay line is fed on every sample whether or not it is being used, so
// flipping strength or the enable across the bypass boundary never produces a
// discontinuity in the bypass output. The STFT machinery runs whenever the
// module is enabled (even at strength 0, which keeps the noise estimate warm
// for an instant-on when the user turns the knob up); it is reset when the
// module transitions from disabled to enabled, which costs one hop of fade-in.
class NoiseReduction {
public:
    // fftSize must be a legal ComplexFFT size (see fft.hpp: 2^a * 3^b * 5^c
    // with a >= 5); the ComplexFFT constructor throws std::invalid_argument
    // otherwise. All legal sizes are multiples of 32, so the 50% hop is always
    // a whole number of samples.
    explicit NoiseReduction(double sampleRateHz, std::size_t fftSize = 512);

    // 0 = bypass (bit-exact passthrough), 1 = maximum suppression. Values
    // outside [0, 1] are clamped.
    void setStrength(float s01);
    float strength() const { return strength_; }

    void setEnabled(bool on);
    bool isEnabled() const { return enabled_; }

    // Processes n samples. Always writes n samples to out and returns n; in
    // and out may be the same buffer. The return value exists so callers do
    // not have to assume the 1:1 rate.
    std::size_t process(const float* in, std::size_t n, float* out);

    // out[j] corresponds to in[j - latencySamples()]. Constant for the life of
    // the object, and identical on the bypass and processing paths.
    std::size_t latencySamples() const { return n_; }

    std::size_t fftSize() const { return n_; }
    std::size_t hopSize() const { return hop_; }
    double sampleRateHz() const { return sampleRate_; }

    // Returns the object to its just-constructed state: delay line, overlap
    // accumulator, noise estimate and gain memory all cleared. Strength and
    // the enable flag are settings, not state, and are kept.
    void reset();

private:
    void resetStft();
    void processFrame();
    void pushOut(float v);
    float popOut();

    // -26 dB: deep enough that the residual noise is genuinely quiet, shallow
    // enough that the noise floor stays audible as a floor rather than
    // pumping in and out of digital silence.
    static constexpr float kMinGain = 0.05f;
    // Per-frame smoothing of the bin power before minimum tracking. Fast
    // enough to follow a real level change, slow enough that the minimum is
    // taken over a smoothed quantity instead of a single noisy periodogram.
    static constexpr float kSmoothAlpha = 0.20f;
    // Asymmetric pole of the noise tracker. Falling is ~12x faster than
    // rising (about 0.1 s down, 1.3 s up at 512/48k frames), which is what
    // makes the estimate sit near the floor of the bin's power distribution
    // instead of its mean, while still following a genuine change in band
    // noise within about a second.
    static constexpr float kNoiseFall = 0.05f;
    static constexpr float kNoiseRise = 0.004f;
    // See the class comment: how far above the cross-bin median a per-bin
    // noise estimate may go before it is assumed to be signal. This is the
    // constant that sets how much of a wanted carrier survives maximum
    // suppression — the loss at a tone bin is oversubtraction * kMaxTilt *
    // median / tonePower — so it is kept as tight as coloured receiver noise
    // allows.
    static constexpr float kMaxTilt = 10.0f;  // 10 dB
    static constexpr float kMedianAlpha = 0.10f;
    // Keeps the gain expression finite on an all-zero frame; far below any
    // real audio power, so it never biases a live signal.
    static constexpr float kPowerEps = 1e-20f;

    double sampleRate_;
    std::size_t n_;
    std::size_t hop_;
    ComplexFFT fft_;

    std::vector<float> window_;   // sqrt(Hann), analysis AND synthesis
    std::vector<float> hist_;     // ring of the last n_ input samples
    std::vector<float> ola_;      // overlap accumulator, ola_[0] = oldest
    std::vector<float> delay_;    // bypass delay line, n_ samples
    std::vector<float> fifo_;     // ready output samples (ring)
    std::vector<std::complex<float>> frame_;
    std::vector<std::complex<float>> spec_;
    std::vector<float> smooth_;   // smoothed bin power
    std::vector<float> noise_;    // tracked noise floor
    std::vector<float> gain_;     // previous frame's gain (temporal smoothing)
    std::vector<float> scratch_;  // median workspace

    std::size_t histPos_ = 0;
    std::size_t sinceFrame_ = 0;
    std::size_t delayPos_ = 0;
    std::size_t fifoHead_ = 0;
    std::size_t fifoCount_ = 0;
    float medianEma_ = 0.0f;
    bool haveStats_ = false;

    float strength_ = 0.0f;
    bool enabled_ = true;
};

}  // namespace cascade::dsp
