// Small measurement helpers shared by the batch-4 block tests (tones, level by correlation, steps, dB).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace blk4 {

constexpr double kTwoPi = 6.283185307179586476925286766559;

inline std::vector<float> tone(std::size_t n, double fs, double hz, double amp, double phase = 0.0) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<float>(amp * std::sin(kTwoPi * hz * static_cast<double>(i) / fs + phase));
    }
    return v;
}

// Amplitude of the component at `hz` in v[from, to) by correlation (exact for whole numbers of cycles, otherwise within a
// fraction of a dB when the window holds many cycles).
inline double toneAmp(const std::vector<float>& v, std::size_t from, std::size_t to, double fs, double hz) {
    double re = 0, im = 0;
    for (std::size_t i = from; i < to; ++i) {
        const double w = kTwoPi * hz * static_cast<double>(i) / fs;
        re += v[i] * std::cos(w);
        im += v[i] * std::sin(w);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(to - from);
}

inline double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1e-30)); }

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

// Runs `proc(float*, n)` over a copy of `in` in blocks of `block`.
template <typename P>
std::vector<float> runBlocks(P&& proc, const std::vector<float>& in, std::size_t block) {
    std::vector<float> out = in;
    for (std::size_t p = 0; p < out.size(); p += block) { proc(out.data() + p, std::min(block, out.size() - p)); }
    return out;
}

}  // namespace blk4
