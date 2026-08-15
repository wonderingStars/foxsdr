// Analysis windows for spectral estimation.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace cascade::dsp {

enum class WindowType {
    Rectangular,
    Hann,
    Hamming,
    BlackmanHarris,  // 4-term, -92 dB sidelobes; the sane default for a waterfall
};

// Fills `out` with an `n`-point periodic window.
//
// Periodic (divisor n) rather than symmetric (divisor n-1): the periodic form is
// the correct one for spectral analysis via FFT, because it makes the window
// seamless when the transform treats the block as one period of an infinite
// sequence. The symmetric form is for FIR filter design.
inline void fillWindow(WindowType type, std::size_t n, float* out) {
    if (n == 0) { return; }
    if (n == 1) { out[0] = 1.0f; return; }

    const double N = static_cast<double>(n);
    constexpr double kTwoPi = 6.283185307179586476925286766559;

    for (std::size_t i = 0; i < n; ++i) {
        const double x = kTwoPi * static_cast<double>(i) / N;
        double w = 1.0;
        switch (type) {
            case WindowType::Rectangular:
                w = 1.0;
                break;
            case WindowType::Hann:
                w = 0.5 - 0.5 * std::cos(x);
                break;
            case WindowType::Hamming:
                w = 0.54 - 0.46 * std::cos(x);
                break;
            case WindowType::BlackmanHarris:
                w = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2.0 * x) -
                    0.01168 * std::cos(3.0 * x);
                break;
        }
        out[i] = static_cast<float>(w);
    }
}

inline std::vector<float> makeWindow(WindowType type, std::size_t n) {
    std::vector<float> w(n);
    fillWindow(type, n, w.data());
    return w;
}

// Coherent gain: the DC gain of the window, sum(w)/n. Dividing a magnitude
// spectrum by this restores the true amplitude of a coherent tone, which the
// window would otherwise attenuate.
inline float coherentGain(const float* w, std::size_t n) {
    if (n == 0) { return 1.0f; }
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) { sum += w[i]; }
    return static_cast<float>(sum / static_cast<double>(n));
}

}  // namespace cascade::dsp
