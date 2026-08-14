// SpectrumEstimator implementation. See spectrum.hpp for the contract.
//
// SPDX-License-Identifier: MIT
#include "dsp/spectrum.hpp"

#include <cmath>

namespace cascade::dsp {

namespace {
// Floor for the linear power before the log: 1e-20 corresponds to -200 dB,
// far below anything a float pipeline can represent meaningfully, and it
// keeps log10 away from -infinity for all-zero input blocks.
constexpr float kPowerFloor = 1e-20f;
constexpr float kMinAlpha = 1e-4f;
}  // namespace

SpectrumEstimator::SpectrumEstimator(std::size_t fftSize, WindowType window)
    : fft_(fftSize),  // validates the size; throws std::invalid_argument
      window_(makeWindow(window, fftSize)),
      windowed_(fftSize),
      spectrum_(fftSize),
      avgPower_(fftSize, 0.0f) {}

void SpectrumEstimator::setAlpha(float alpha) noexcept {
    // Written so a NaN argument fails the first comparison and lands on the
    // minimum instead of being stored (see header).
    if (alpha >= 1.0f) {
        alpha_ = 1.0f;
    } else if (alpha > kMinAlpha) {
        alpha_ = alpha;
    } else {
        alpha_ = kMinAlpha;
    }
}

void SpectrumEstimator::process(const std::complex<float>* in, float* outDb) {
    const std::size_t n = fft_.size();
    for (std::size_t i = 0; i < n; ++i) {
        windowed_[i] = in[i] * window_[i];
    }
    fft_.forward(windowed_.data(), spectrum_.data());

    // 1/N^2 makes a unit-amplitude exact-bin tone read power 1.0 (0 dBFS)
    // under a rectangular window: its DFT magnitude is N, so |X|^2/N^2 = 1.
    const float invN2 = 1.0f / (static_cast<float>(n) * static_cast<float>(n));
    const std::size_t half = n / 2;  // every legal size is even (multiple of 32)
    for (std::size_t i = 0; i < n; ++i) {
        // fftshift: output slot i takes natural FFT bin (i + N/2) mod N, so
        // DC lands at slot N/2 and slot 0 holds -sampleRate/2.
        std::size_t k = i + half;
        if (k >= n) { k -= n; }
        const float p = std::norm(spectrum_[k]) * invN2;
        avgPower_[i] = primed_ ? alpha_ * p + (1.0f - alpha_) * avgPower_[i] : p;
        outDb[i] = 10.0f * std::log10(avgPower_[i] > kPowerFloor ? avgPower_[i]
                                                                 : kPowerFloor);
    }
    primed_ = true;
}

}  // namespace cascade::dsp
