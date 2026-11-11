// Generated signals and a small FFT shared by the rfmon-lane tests (RBW/detector, pseudo-Doppler). Nothing here is a recording.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace rfgen {

constexpr double kTwoPi = 6.283185307179586476925286766559;
using cd = std::complex<double>;

// Radix-2 FFT, in place, n a power of two (test-only; double precision).
inline void fft(std::vector<cd>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) { j ^= bit; }
        j ^= bit;
        if (i < j) { std::swap(a[i], a[j]); }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -kTwoPi / static_cast<double>(len);
        const cd wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            cd w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const cd u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Complex white Gaussian noise of total power p (E|x|^2 = p).
inline std::vector<cd> noise(std::size_t n, double p, std::uint32_t seed) {
    std::mt19937 g(seed);
    std::normal_distribution<double> d(0.0, std::sqrt(p / 2.0));
    std::vector<cd> v(n);
    for (auto& x : v) { x = cd(d(g), d(g)); }
    return v;
}

// Adds a complex tone of power p at `hz` (phase0 radians) to v.
inline void addTone(std::vector<cd>& v, double fs, double hz, double p, double phase0 = 0.0) {
    const double a = std::sqrt(p);
    for (std::size_t i = 0; i < v.size(); ++i) {
        const double ph = kTwoPi * hz * static_cast<double>(i) / fs + phase0;
        v[i] += cd(a * std::cos(ph), a * std::sin(ph));
    }
}

// One frame of per-bin W/Hz: |FFT(w x)|^2 / (fs sum w^2), bins re-ordered so bin 0 is the lowest frequency (DC at n/2).
// `w` is the window (n floats).
inline std::vector<float> psdFrame(const std::vector<cd>& x, std::size_t start, std::size_t n, const float* w, double fs) {
    std::vector<cd> a(n);
    double sw2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        a[i] = x[start + i] * static_cast<double>(w[i]);
        sw2 += static_cast<double>(w[i]) * w[i];
    }
    fft(a);
    std::vector<float> out(n);
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t src = (k + n / 2) % n;   // bin k of the shifted frame
        out[k] = static_cast<float>(std::norm(a[src]) / (fs * sw2));
    }
    return out;
}

}  // namespace rfgen
