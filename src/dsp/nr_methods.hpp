// Two further noise-reduction methods for 48 kHz mono audio behind one small interface:
//   * MinStatSubtractor - STFT spectral subtraction with a MINIMUM-STATISTICS noise estimate and a gain
//     floor against musical noise (good for broadband hiss on voice);
//   * LineEnhancer      - an NLMS adaptive line enhancer (good for CW and steady carriers in noise).
//
// STANDALONE BLOCK. Nothing here is wired into the pipeline, the engine, the settings or the screen
// (docs/feature-records/signal-blocks-2.md, "WIRING HAND-OFF", says where it goes). The EXISTING noise
// reduction (dsp/noise_reduction.hpp, class NoiseReduction: Wiener / spectral subtraction on an asymmetric
// decay tracker, latency 512 samples) is untouched and stays the default; these are alternatives with the
// same process() shape (n in, n out, in-place allowed).
//
// SOURCES (clean room: papers and textbooks; read-through status is in the record).
//  * Spectral subtraction: S. F. Boll, "Suppression of acoustic noise in speech using spectral
//    subtraction", IEEE Trans. ASSP 27(2), 1979. Over-subtraction and a spectral FLOOR against musical
//    noise: M. Berouti, R. Schwartz, J. Makhoul, "Enhancement of speech corrupted by acoustic noise",
//    Proc. ICASSP 1979. (Both cited from memory; UNVERIFIED against the papers.)
//  * Minimum statistics: R. Martin, "Noise power spectral density estimation based on optimal smoothing
//    and minimum statistics", IEEE Trans. Speech and Audio Processing 9(5), 2001. The idea used: smooth the
//    periodogram per bin and track its MINIMUM over a window of about a second; noise is what is left
//    when the speech pauses. The minimum is biased low, and Martin derives a bias factor from the
//    smoothing; here the factor kBias is instead MEASURED on white Gaussian noise (tests/test_nr_methods.cpp
//    prints and pins the ratio), and the smoothing is a fixed 0.85 rather than Martin's optimal
//    time-varying one. So this is "minimum statistics" in the sense of that paper, not a copy of its
//    estimator; UNVERIFIED for non-Gaussian or strongly coloured noise.
//  * Adaptive line enhancer: B. Widrow et al., "Adaptive noise cancelling: principles and applications",
//    Proc. IEEE 63(12), 1975; the ALE as a delayed-input linear predictor: J. R. Zeidler et al., "Adaptive
//    enhancement of multiple sinusoids in uncorrelated noise", IEEE Trans. ASSP 26(3), 1978 (UNVERIFIED
//    titles/volumes). NLMS: the standard normalised update w += mu e u / (eps + |u|^2) (e.g. Haykin,
//    "Adaptive Filter Theory").
//
// LATENCY (bounded and constant)
//   MinStatSubtractor: exactly fftSize = 512 samples (10.7 ms at 48 kHz): out[j] corresponds to in[j - 512],
//     the same as the existing NoiseReduction. LineEnhancer: 0 samples (a causal predictor).
// STRENGTH (0..1) for both: 0 is a bit-exact bypass (delayed by the latency for the subtractor).
//   Subtractor: over-subtraction 1 + 3 s, gain floor 10^-s (-20 dB at 1). Enhancer: dry/wet mix
//   out = (1 - s) x + s y, y the predictor output.
//
// THREADS AND ALLOCATION. Constructors allocate (set-up). process()/reset() run on the DSP thread
// only and never allocate, lock or wait; setStrength() is an atomic store from any thread, picked up at
// the next frame (subtractor) or the next call (enhancer). Non-finite input samples are read as 0.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "dsp/fft.hpp"

namespace cascade::dsp {

class NoiseReducer {
public:
    virtual ~NoiseReducer() = default;
    virtual void setStrength(float s01) = 0;
    virtual float strength() const = 0;
    // n samples in, n out (returns n); `in` and `out` may be the same buffer.
    virtual std::size_t process(const float* in, std::size_t n, float* out) = 0;
    // out[j] corresponds to in[j - latencySamples()]
    virtual std::size_t latencySamples() const = 0;
    virtual void reset() = 0;
    virtual const char* name() const = 0;
};

// ---------------------------------------------------------------------------------------------------------
class MinStatSubtractor final : public NoiseReducer {
public:
    static constexpr std::size_t kFft = 512;
    static constexpr std::size_t kHop = kFft / 2;
    static constexpr std::size_t kSubWindows = 6;        // U
    static constexpr std::size_t kFramesPerSub = 40;     // V: 40 frames * 5.33 ms = 213 ms; U*V = 1.28 s of memory
    static constexpr float kSlowAlpha = 0.85f;           // smoothing of the periodogram for the minimum
    static constexpr float kFastAlpha = 0.5f;            // smoothing used for the gain (a-posteriori power)
    static constexpr float kGainSmooth = 0.5f;           // weight of the previous gain (temporal smoothing)
    // Minimum of the slow-smoothed periodogram over the memory is biased low on Gaussian noise; this factor
    // is the MEASURED inverse of that bias (see the test: it prints est / true and pins it within 1 dB).
    static constexpr float kBias = 2.2f;
    // a bin's noise estimate may not exceed this multiple of the cross-bin median: a steady tone never pauses,
    // so its own minimum would otherwise be taken for noise (the same guard as the existing NoiseReduction).
    static constexpr float kMaxTilt = 10.0f;
    static constexpr float kPowerEps = 1e-20f;

    MinStatSubtractor() : fft_(kFft) {
        window_.resize(kFft);
        for (std::size_t i = 0; i < kFft; ++i) {
            const double hann = 0.5 - 0.5 * std::cos(6.283185307179586 * static_cast<double>(i) / static_cast<double>(kFft));
            window_[i] = static_cast<float>(std::sqrt(hann));
        }
        const std::size_t bins = kFft / 2 + 1;
        frame_.resize(kFft);
        spec_.resize(kFft);
        hist_.assign(kFft, 0.0f);
        ola_.assign(kFft, 0.0f);
        delay_.assign(kFft, 0.0f);
        fifo_.assign(kFft + kHop + 8, 0.0f);
        slow_.assign(bins, 0.0f);
        fast_.assign(bins, 0.0f);
        curMin_.assign(bins, 0.0f);
        subMin_.assign(kSubWindows * bins, 0.0f);
        noise_.assign(bins, 0.0f);
        gain_.assign(bins, 1.0f);
        scratch_.assign(bins, 0.0f);
        reset();
    }

    void setStrength(float s) override {
        strength_.store(std::isfinite(s) ? std::clamp(s, 0.0f, 1.0f) : 0.0f, std::memory_order_relaxed);
    }
    float strength() const override { return strength_.load(std::memory_order_relaxed); }
    std::size_t latencySamples() const override { return kFft; }
    const char* name() const override { return "Minimum-statistics spectral subtraction"; }

    void reset() override {
        std::fill(hist_.begin(), hist_.end(), 0.0f);
        std::fill(ola_.begin(), ola_.end(), 0.0f);
        std::fill(delay_.begin(), delay_.end(), 0.0f);
        std::fill(slow_.begin(), slow_.end(), 0.0f);
        std::fill(fast_.begin(), fast_.end(), 0.0f);
        std::fill(subMin_.begin(), subMin_.end(), 0.0f);
        std::fill(noise_.begin(), noise_.end(), 0.0f);
        std::fill(gain_.begin(), gain_.end(), 1.0f);
        frames_ = 0;
        subCount_ = 0;
        subHead_ = 0;
        histPos_ = delayPos_ = sinceFrame_ = 0;
        fifoHead_ = 0;
        fifoCount_ = kHop;               // priming: one hop of zeros, so every call can return as many samples as it took
        std::fill(fifo_.begin(), fifo_.end(), 0.0f);
    }

    std::size_t process(const float* in, std::size_t n, float* out) override {
        for (std::size_t i = 0; i < n; ++i) {
            float v = in[i];
            if (!std::isfinite(v)) { v = 0.0f; }
            // bypass delay line: always fed, so crossing strength 0 never jumps
            const float delayed = delay_[delayPos_];
            delay_[delayPos_] = v;
            delayPos_ = (delayPos_ + 1 == kFft) ? 0 : delayPos_ + 1;
            hist_[histPos_] = v;
            histPos_ = (histPos_ + 1 == kFft) ? 0 : histPos_ + 1;
            if (++sinceFrame_ == kHop) {
                sinceFrame_ = 0;
                processFrame();
            }
            const float wet = popOut();
            out[i] = (bypass_) ? delayed : wet;
        }
        return n;
    }

    // Test probe: the current noise power estimate in bin k (periodogram units of the 512-point transform).
    float noisePower(std::size_t k) const { return k < noise_.size() ? noise_[k] : 0.0f; }
    std::size_t bins() const { return noise_.size(); }

private:
    void processFrame() {
        const float s = strength_.load(std::memory_order_relaxed);
        bypass_ = s <= 0.0f;
        const std::size_t bins = kFft / 2 + 1;
        // newest kFft samples, oldest first, windowed
        std::size_t p = histPos_;
        for (std::size_t i = 0; i < kFft; ++i) {
            frame_[i] = std::complex<float>(hist_[p] * window_[i], 0.0f);
            p = (p + 1 == kFft) ? 0 : p + 1;
        }
        fft_.forward(frame_.data(), spec_.data());

        // ---- smoothed periodograms and the minimum-statistics floor --------------------------------
        const bool first = (frames_ == 0);
        for (std::size_t k = 0; k < bins; ++k) {
            const float P = std::norm(spec_[k]);
            if (first) {
                slow_[k] = P;
                fast_[k] = P;
                curMin_[k] = P;
                for (std::size_t u = 0; u < kSubWindows; ++u) { subMin_[u * bins + k] = P; }
            } else {
                slow_[k] = kSlowAlpha * slow_[k] + (1.0f - kSlowAlpha) * P;
                fast_[k] = kFastAlpha * fast_[k] + (1.0f - kFastAlpha) * P;
                curMin_[k] = std::min(curMin_[k], slow_[k]);
            }
        }
        ++frames_;
        if (++subCount_ >= kFramesPerSub) {
            subCount_ = 0;
            for (std::size_t k = 0; k < bins; ++k) {
                subMin_[subHead_ * bins + k] = curMin_[k];
                curMin_[k] = slow_[k];
            }
            subHead_ = (subHead_ + 1 == kSubWindows) ? 0 : subHead_ + 1;
        }
        for (std::size_t k = 0; k < bins; ++k) {
            float m = curMin_[k];
            for (std::size_t u = 0; u < kSubWindows; ++u) { m = std::min(m, subMin_[u * bins + k]); }
            noise_[k] = m * kBias;
        }
        // cap against the cross-bin median
        std::copy(noise_.begin(), noise_.end(), scratch_.begin());
        const std::size_t mid = bins / 2;
        std::nth_element(scratch_.begin(), scratch_.begin() + static_cast<std::ptrdiff_t>(mid), scratch_.end());
        const float cap = kMaxTilt * scratch_[mid];
        for (std::size_t k = 0; k < bins; ++k) { noise_[k] = std::min(noise_[k], cap); }

        // ---- gain: over-subtraction with a floor ----------------------------------------------------
        const float over = 1.0f + 3.0f * s;
        const float floorG = std::pow(10.0f, -s);
        for (std::size_t k = 0; k < bins; ++k) {
            const float P = std::max(fast_[k], kPowerEps);
            float g2 = 1.0f - over * noise_[k] / P;
            g2 = std::max(g2, floorG * floorG);
            float g = std::sqrt(std::min(g2, 1.0f));
            g = kGainSmooth * gain_[k] + (1.0f - kGainSmooth) * g;
            gain_[k] = g;
            spec_[k] *= g;
        }
        // mirror onto the conjugate half so the inverse is real
        for (std::size_t k = 1; k < kFft / 2; ++k) { spec_[kFft - k] = std::conj(spec_[k]); }
        spec_[0] = std::complex<float>(spec_[0].real(), 0.0f);
        spec_[kFft / 2] = std::complex<float>(spec_[kFft / 2].real(), 0.0f);
        fft_.inverse(spec_.data(), frame_.data());

        // overlap-add: accumulate, then the oldest kHop samples are complete
        for (std::size_t i = 0; i < kFft; ++i) { ola_[i] += frame_[i].real() * window_[i]; }
        for (std::size_t i = 0; i < kHop; ++i) { pushOut(ola_[i]); }
        std::copy(ola_.begin() + static_cast<std::ptrdiff_t>(kHop), ola_.end(), ola_.begin());
        std::fill(ola_.begin() + static_cast<std::ptrdiff_t>(kHop), ola_.end(), 0.0f);
    }

    void pushOut(float v) {
        const std::size_t cap = fifo_.size();
        fifo_[(fifoHead_ + fifoCount_) % cap] = v;
        ++fifoCount_;
    }
    float popOut() {
        if (fifoCount_ == 0) { return 0.0f; }
        const float v = fifo_[fifoHead_];
        fifoHead_ = (fifoHead_ + 1) % fifo_.size();
        --fifoCount_;
        return v;
    }

    ComplexFFT fft_;
    std::vector<float> window_, hist_, ola_, delay_, fifo_;
    std::vector<std::complex<float>> frame_, spec_;
    std::vector<float> slow_, fast_, curMin_, subMin_, noise_, gain_, scratch_;
    std::atomic<float> strength_{0.0f};
    bool bypass_ = true;
    std::size_t histPos_ = 0, delayPos_ = 0, sinceFrame_ = 0, fifoHead_ = 0, fifoCount_ = 0;
    std::size_t frames_ = 0, subCount_ = 0, subHead_ = 0;
};

// ---------------------------------------------------------------------------------------------------------
class LineEnhancer final : public NoiseReducer {
public:
    // length: taps of the predictor (frequency resolution about rate / length); delay: samples between the
    // predicted sample and the newest input to the predictor (it must be long enough that the NOISE is
    // uncorrelated across it: 1 for white noise, a few ms for audio with coloured noise); mu: NLMS step.
    explicit LineEnhancer(std::size_t length = 128, std::size_t delay = 8, float mu = 0.01f)
        : len_(std::clamp<std::size_t>(length, 8, 4096)), dly_(std::clamp<std::size_t>(delay, 1, 4096)),
          mu_(std::clamp(mu, 1e-4f, 1.0f)) {
        m_ = len_ + dly_;
        buf_.assign(2 * m_, 0.0f);
        w_.assign(len_, 0.0f);
    }

    void setStrength(float s) override {
        strength_.store(std::isfinite(s) ? std::clamp(s, 0.0f, 1.0f) : 0.0f, std::memory_order_relaxed);
    }
    float strength() const override { return strength_.load(std::memory_order_relaxed); }
    std::size_t latencySamples() const override { return 0; }
    const char* name() const override { return "Adaptive line enhancer (NLMS)"; }
    std::size_t length() const { return len_; }
    std::size_t delay() const { return dly_; }

    void reset() override {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        std::fill(w_.begin(), w_.end(), 0.0f);
        wp_ = 0;
        energy_ = 0.0;
        sinceRecalc_ = 0;
    }

    std::size_t process(const float* in, std::size_t n, float* out) override {
        const float s = strength_.load(std::memory_order_relaxed);
        const float eps = 1e-8f * static_cast<float>(len_);
        for (std::size_t i = 0; i < n; ++i) {
            float x = in[i];
            if (!std::isfinite(x)) { x = 0.0f; }
            // The history window holds the m_ = dly_ + len_ previous samples, oldest first, starting at buf_[wp_]
            // (the buffer is doubled so the window is contiguous). The predictor input is the oldest len_ of them
            // that are at least dly_ old: win[1 .. len_] with win = &buf_[wp_] (win[len_] = x[n - dly_]).
            const float* win = &buf_[wp_];
            float y = 0.0f;
            for (std::size_t k = 0; k < len_; ++k) { y += w_[k] * win[k + 1]; }
            const float e = x - y;
            const float g = mu_ * e / (static_cast<float>(energy_) + eps);
            for (std::size_t k = 0; k < len_; ++k) { w_[k] += g * win[k + 1]; }
            out[i] = (1.0f - s) * x + s * y;
            // slide: x replaces the oldest history sample
            buf_[wp_] = x;
            buf_[wp_ + m_] = x;
            wp_ = (wp_ + 1 == m_) ? 0 : wp_ + 1;
            const float* nw = &buf_[wp_];
            energy_ += static_cast<double>(nw[len_]) * nw[len_] - static_cast<double>(nw[0]) * nw[0];
            if (energy_ < 0.0) { energy_ = 0.0; }
            if (++sinceRecalc_ >= 4096) {   // exact recompute now and then: the running sum drifts in float
                sinceRecalc_ = 0;
                double e2 = 0.0;
                for (std::size_t k = 0; k < len_; ++k) { e2 += static_cast<double>(nw[k + 1]) * nw[k + 1]; }
                energy_ = e2;
            }
        }
        return n;
    }

private:
    std::size_t len_, dly_, m_ = 0, wp_ = 0, sinceRecalc_ = 0;
    float mu_;
    double energy_ = 0.0;
    std::vector<float> buf_, w_;
    std::atomic<float> strength_{0.0f};
};

}  // namespace cascade::dsp
