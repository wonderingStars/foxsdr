// Implementation of the STFT spectral noise reducer declared in
// noise_reduction.hpp. Every design decision and its reason lives in the
// header comment; this file only says why the CODE is shaped the way it is.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/noise_reduction.hpp"

#include <algorithm>
#include <cmath>

#include "dsp/window.hpp"

namespace cascade::dsp {

NoiseReduction::NoiseReduction(double sampleRateHz, std::size_t fftSize)
    : sampleRate_(sampleRateHz > 0.0 ? sampleRateHz : 1.0),
      n_(fftSize),
      hop_(fftSize / 2),
      fft_(fftSize),  // throws std::invalid_argument on an illegal size
      window_(makeWindow(WindowType::Hann, fftSize)),
      hist_(fftSize, 0.0f),
      ola_(fftSize, 0.0f),
      delay_(fftSize, 0.0f),
      fifo_(fftSize, 0.0f),
      frame_(fftSize),
      spec_(fftSize),
      smooth_(fftSize / 2 + 1, 0.0f),
      noise_(fftSize / 2 + 1, 0.0f),
      gain_(fftSize / 2 + 1, 1.0f),
      scratch_(fftSize / 2 + 1, 0.0f) {
    // sqrt(Hann) in place: the product of the analysis and synthesis windows
    // must be Hann for 50% COLA (header comment), and both windows are the
    // same array.
    for (std::size_t i = 0; i < n_; ++i) { window_[i] = std::sqrt(window_[i]); }
    reset();
}

void NoiseReduction::setStrength(float s01) {
    // NaN lands on 0 (bypass): a broken control value must not be able to
    // scramble the audio.
    if (!(s01 > 0.0f)) {
        strength_ = 0.0f;
    } else if (s01 > 1.0f) {
        strength_ = 1.0f;
    } else {
        strength_ = s01;
    }
}

void NoiseReduction::setEnabled(bool on) {
    // Coming back from disabled, the STFT has been starved of input for an
    // unknown time; its history and noise estimate are stale, so start clean.
    if (on && !enabled_) { resetStft(); }
    enabled_ = on;
}

void NoiseReduction::reset() {
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    delayPos_ = 0;
    resetStft();
}

void NoiseReduction::resetStft() {
    std::fill(hist_.begin(), hist_.end(), 0.0f);
    std::fill(ola_.begin(), ola_.end(), 0.0f);
    std::fill(fifo_.begin(), fifo_.end(), 0.0f);
    std::fill(smooth_.begin(), smooth_.end(), 0.0f);
    std::fill(noise_.begin(), noise_.end(), 0.0f);
    std::fill(gain_.begin(), gain_.end(), 1.0f);
    histPos_ = 0;
    sinceFrame_ = 0;
    // Prime the output queue with one hop of zeros. Without it a call could
    // ask for more samples than the overlap-add has finished; with it the
    // queue holds hop_ - (samples consumed since the last frame) entries,
    // which is never negative. This priming is the second half of the
    // documented fftSize latency.
    fifoHead_ = 0;
    fifoCount_ = hop_;
    medianEma_ = 0.0f;
    haveStats_ = false;
}

void NoiseReduction::pushOut(float v) {
    fifo_[(fifoHead_ + fifoCount_) % fifo_.size()] = v;
    ++fifoCount_;
}

float NoiseReduction::popOut() {
    const float v = fifo_[fifoHead_];
    fifoHead_ = (fifoHead_ + 1) % fifo_.size();
    --fifoCount_;
    return v;
}

std::size_t NoiseReduction::process(const float* in, std::size_t n, float* out) {
    for (std::size_t i = 0; i < n; ++i) {
        // Read before any write so in == out is legal.
        const float x = in[i];

        // The bypass delay line runs unconditionally: it is what makes a
        // strength or enable change take effect without a discontinuity.
        const float delayed = delay_[delayPos_];
        delay_[delayPos_] = x;
        if (++delayPos_ == n_) { delayPos_ = 0; }

        float y = delayed;
        if (enabled_) {
            hist_[histPos_] = x;
            if (++histPos_ == n_) { histPos_ = 0; }
            if (++sinceFrame_ == hop_) {
                sinceFrame_ = 0;
                processFrame();
            }
            // Always drain, even when the bypass output is the one being
            // returned, so the queue occupancy stays tied to the sample count.
            const float processed = popOut();
            if (strength_ > 0.0f) { y = processed; }
        }
        out[i] = y;
    }
    return n;
}

void NoiseReduction::processFrame() {
    // hist_ is a ring whose oldest sample is at histPos_ (the write cursor has
    // just wrapped past it), so this both linearises and windows the frame.
    for (std::size_t i = 0; i < n_; ++i) {
        std::size_t idx = histPos_ + i;
        if (idx >= n_) { idx -= n_; }
        frame_[i] = std::complex<float>(hist_[idx] * window_[i], 0.0f);
    }
    fft_.forward(frame_.data(), spec_.data());

    const std::size_t half = n_ / 2;

    // Pass 1: smoothed power per bin, and the cross-bin median that bounds the
    // per-bin noise estimate.
    for (std::size_t k = 0; k <= half; ++k) {
        const float p = std::norm(spec_[k]);
        smooth_[k] = haveStats_ ? smooth_[k] + kSmoothAlpha * (p - smooth_[k]) : p;
    }
    scratch_.assign(smooth_.begin(), smooth_.end());
    const std::size_t mid = scratch_.size() / 2;
    std::nth_element(scratch_.begin(), scratch_.begin() + static_cast<std::ptrdiff_t>(mid),
                     scratch_.end());
    const float median = scratch_[mid];
    medianEma_ = haveStats_ ? medianEma_ + kMedianAlpha * (median - medianEma_) : median;

    // Pass 2: track the floor, form the gain, apply it to the half spectrum
    // and mirror it onto the conjugate half so the inverse transform is real.
    const float overSub = 1.0f + 2.0f * strength_;
    const float floorGain = 1.0f - strength_ * (1.0f - kMinGain);
    const float cap = medianEma_ * kMaxTilt;
    for (std::size_t k = 0; k <= half; ++k) {
        const float sm = smooth_[k];
        float nf = noise_[k];
        if (!haveStats_) {
            nf = sm;  // no history yet; anything else is a guess
        } else {
            // Asymmetric decay tracker: falls fast, rises slowly, so it
            // settles near the LOW end of the bin's power distribution — the
            // floor is what is still there in the quiet moments. A hard
            // running minimum was tried first and is unusable here: its
            // effective horizon makes it settle several times BELOW the mean
            // noise power, and the resulting under-subtraction cost most of
            // the suppression. The asymmetric pole has a bounded bias
            // (roughly the kNoiseFall/kNoiseRise-th percentile of the
            // smoothed power) that the oversubtraction factor covers.
            const float rate = (sm < nf) ? kNoiseFall : kNoiseRise;
            nf += rate * (sm - nf);
        }
        if (nf > cap) { nf = cap; }
        noise_[k] = nf;

        // The gain is formed from the SMOOTHED power, not the raw periodogram
        // of this frame. A single frame's bin power is exponentially
        // distributed about the true level, so a raw-periodogram gain lets
        // every upward fluctuation of the noise through at near-unity gain —
        // that residual is precisely what musical noise is. Smoothing first
        // cuts the variance by 1/kSmoothAlpha and buys well over 10 dB of
        // extra suppression. The price is that the gain lags a genuine level
        // change by the smoother's time constant (~5 frames, ~27 ms at
        // 512/48 kHz), which softens hard onsets slightly.
        float g = 1.0f - overSub * nf / (sm + kPowerEps);
        if (g < floorGain) { g = floorGain; }
        if (g > 1.0f) { g = 1.0f; }
        // Average with the previous frame's gain for this bin: halves the
        // frame-to-frame gain flicker that is heard as musical noise. At
        // strength 0 both terms are exactly 1, so the average is exactly 1 and
        // the transform pair is left to reconstruct the input untouched.
        g = 0.5f * (gain_[k] + g);
        gain_[k] = g;

        spec_[k] *= g;
        if (k > 0 && k < half) { spec_[n_ - k] *= g; }
    }
    haveStats_ = true;

    fft_.inverse(spec_.data(), frame_.data());
    for (std::size_t i = 0; i < n_; ++i) {
        ola_[i] += frame_[i].real() * window_[i];
    }

    // The first hop of the accumulator can receive no further contribution
    // (the next frame starts one hop later), so it is finished output.
    for (std::size_t i = 0; i < hop_; ++i) { pushOut(ola_[i]); }
    for (std::size_t i = 0; i + hop_ < n_; ++i) { ola_[i] = ola_[i + hop_]; }
    for (std::size_t i = n_ - hop_; i < n_; ++i) { ola_[i] = 0.0f; }
}

}  // namespace cascade::dsp
