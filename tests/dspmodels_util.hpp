// Small helpers shared by the dspmodels lane's tests: a seeded generator, tones, dB, an exponentially distributed bin power (one FFT bin of white
// Gaussian noise), and a real FFT-based spectrum frame of generated I/Q.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "dsp/fft.hpp"
#include "dsp/window.hpp"

namespace dspm {

constexpr double kTwoPi = 6.283185307179586476925286766559;

inline double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1e-30)); }

inline std::vector<float> tone(std::size_t n, double fs, double hz, double amp, double phase = 0.0) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) { v[i] = static_cast<float>(amp * std::sin(kTwoPi * hz * static_cast<double>(i) / fs + phase)); }
    return v;
}

inline double toneAmp(const std::vector<float>& v, std::size_t from, std::size_t to, double fs, double hz) {
    double re = 0, im = 0;
    for (std::size_t i = from; i < to; ++i) {
        const double w = kTwoPi * hz * static_cast<double>(i) / fs;
        re += v[i] * std::cos(w);
        im += v[i] * std::sin(w);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(to - from);
}

inline double maxStep(const std::vector<float>& v, std::size_t from, std::size_t to) {
    double m = 0;
    for (std::size_t i = from + 1; i < to && i < v.size(); ++i) { m = std::max(m, std::fabs(static_cast<double>(v[i]) - v[i - 1])); }
    return m;
}
inline double maxAbs(const std::vector<float>& v, std::size_t from, std::size_t to) {
    double m = 0;
    for (std::size_t i = from; i < to && i < v.size(); ++i) { m = std::max(m, std::fabs(static_cast<double>(v[i]))); }
    return m;
}

// One frame of `n` bins of white-noise power with mean power 10^(meanDb/10): each bin's power is exponentially distributed.
inline std::vector<float> noiseFrameDb(std::mt19937_64& rng, std::size_t n, double meanDb) {
    std::exponential_distribution<double> e(1.0);
    std::vector<float> f(n);
    for (std::size_t i = 0; i < n; ++i) { f[i] = static_cast<float>(meanDb + 10.0 * std::log10(std::max(e(rng), 1e-12))); }
    return f;
}

inline double meanOf(const std::vector<float>& v, std::size_t from, std::size_t to) {
    double s = 0;
    for (std::size_t i = from; i < to; ++i) { s += v[i]; }
    return s / static_cast<double>(to - from);
}
inline double meanPowerDb(const std::vector<float>& v, std::size_t from, std::size_t to) {
    double s = 0;
    for (std::size_t i = from; i < to; ++i) { s += std::pow(10.0, v[i] / 10.0); }
    return 10.0 * std::log10(s / static_cast<double>(to - from));
}

// A Blackman-Harris spectrum frame (dB, fftshifted: bin n/2 is DC) of complex Gaussian noise of per-component sigma `sigma` plus tones
// (frequency as a fraction of the sample rate, amplitude, with `ppm` of sample-rate error applied to every frequency).
struct ToneSpec {
    double frac;
    double amp;
};
inline std::vector<float> fftFrameDb(std::mt19937_64& rng, std::size_t n, double sigma, const std::vector<ToneSpec>& tones, double ppm = 0.0) {
    cascade::dsp::ComplexFFT fft(n);
    std::vector<float> w = cascade::dsp::makeWindow(cascade::dsp::WindowType::BlackmanHarris, n);
    std::normal_distribution<double> g(0.0, sigma);
    std::uniform_real_distribution<double> ph(0.0, kTwoPi);
    std::vector<double> phase(tones.size());
    for (auto& p : phase) { p = ph(rng); }
    std::vector<std::complex<float>> x(n), X(n);
    for (std::size_t i = 0; i < n; ++i) {
        std::complex<double> s(g(rng), g(rng));
        for (std::size_t k = 0; k < tones.size(); ++k) {
            const double f = tones[k].frac * (1.0 + ppm * 1e-6);
            s += std::polar(tones[k].amp, kTwoPi * f * static_cast<double>(i) + phase[k]);
        }
        x[i] = std::complex<float>(static_cast<float>(s.real() * w[i]), static_cast<float>(s.imag() * w[i]));
    }
    fft.forward(x.data(), X.data());
    std::vector<float> db(n);
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t shifted = (k + n / 2) % n;
        db[k] = static_cast<float>(10.0 * std::log10(std::max(static_cast<double>(std::norm(X[shifted])) / (static_cast<double>(n) * n), 1e-30)));
    }
    return db;
}

}  // namespace dspm
