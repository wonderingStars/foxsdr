// Power-spectrum estimator: window -> FFT -> |X|^2 -> fftshift -> EMA -> dB.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

#include "dsp/fft.hpp"
#include "dsp/window.hpp"

namespace cascade::dsp {

class SpectrumEstimator {
public:
    // fftSize must satisfy ComplexFFT::isValidSize (throws otherwise).
    SpectrumEstimator(std::size_t fftSize, WindowType window);

    std::size_t fftSize() const noexcept { return fft_.size(); }

    // Weight of the newest block in the exponential moving average:
    //   avg = alpha * power + (1 - alpha) * avg
    // alpha = 1 means no averaging. Clamped to [1e-4, 1] because alpha <= 0
    // would freeze the display forever and a NaN would poison the average
    // permanently; the clamp also swallows NaN (comparisons with NaN are
    // false, so it falls through to the minimum).
    void setAlpha(float alpha) noexcept;
    float alpha() const noexcept { return alpha_; }

    // Forget the running average; the next block re-primes it. The first
    // block after construction or reset() is taken as-is rather than being
    // averaged against zero, so a low alpha does not cause a slow fade-in
    // from -infinity dB.
    void reset() noexcept { primed_ = false; }

    // Consumes fftSize() samples from `in` and writes fftSize() dB values to
    // `outDb`, fftshifted: outDb[fftSize()/2] is DC, lower indices are
    // negative frequencies, index 0 is -sampleRate/2.
    //
    // Scaling (deliberate choice, documented so display code can rely on it):
    //   outDb[i] = 10*log10(avg(|X[k]|^2 / N^2)), floored at -200 dB.
    // With this normalization a unit-amplitude complex tone at an exact bin
    // reads 0 dBFS under a rectangular window. Window attenuation is NOT
    // compensated: a Hann window shows the same tone 20*log10(coherentGain)
    // ~= -6 dB lower. That is intentional — the estimator reports what the
    // windowed transform actually measured, and a display that wants
    // calibrated tone levels adds -20*log10(coherentGain(window)) itself.
    // Averaging runs in the LINEAR power domain (before the dB conversion)
    // because averaging dB values would estimate the mean of the log — a
    // biased-low estimate of mean power for fluctuating signals.
    void process(const std::complex<float>* in, float* outDb);

private:
    ComplexFFT fft_;
    std::vector<float> window_;
    std::vector<std::complex<float>> windowed_;  // scratch: input * window
    std::vector<std::complex<float>> spectrum_;  // scratch: FFT output
    std::vector<float> avgPower_;                // EMA state, fftshifted order
    float alpha_ = 1.0f;
    bool primed_ = false;
};

}  // namespace cascade::dsp
