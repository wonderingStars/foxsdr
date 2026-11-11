// Generated signals shared by the signal-block tests (CTCSS, DCS): white and
// discriminator-shaped noise, tones, and a speech-like audio synthesiser. Nothing here
// is a recording; the block records say so.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace sbgen {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// Gaussian white noise, standard deviation `sigma`.
inline std::vector<float> whiteNoise(std::size_t n, double sigma, std::uint32_t seed) {
    std::mt19937 g(seed);
    std::normal_distribution<double> d(0.0, sigma);
    std::vector<float> v(n);
    for (auto& x : v) { x = static_cast<float>(d(g)); }
    return v;
}

// What an FM discriminator makes of noise: white noise differentiated, so the spectrum rises
// with frequency (6 dB per octave). Scaled so the standard deviation is `sigma`.
inline std::vector<float> discriminatorNoise(std::size_t n, double sigma, std::uint32_t seed) {
    std::vector<float> w = whiteNoise(n + 1, 1.0, seed);
    std::vector<float> v(n);
    double s2 = 0;
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = w[i + 1] - w[i];
        s2 += static_cast<double>(v[i]) * v[i];
    }
    const double sd = std::sqrt(s2 / static_cast<double>(std::max<std::size_t>(n, 1)));
    for (auto& x : v) { x = static_cast<float>(x / sd * sigma); }
    return v;
}

// Adds `amp * sin(2 pi hz t + phase)` to `v` between samples [from, to) (time measured from sample 0).
inline void addTone(std::vector<float>& v, double fs, double hz, double amp, std::size_t from,
                    std::size_t to, double phase = 0.0) {
    to = std::min(to, v.size());
    for (std::size_t i = from; i < to; ++i) {
        v[i] += static_cast<float>(amp * std::sin(kTwoPi * hz * static_cast<double>(i) / fs + phase));
    }
}

// Speech-like audio, peak about `peak`: syllables of 80-250 ms of voiced harmonics (fundamental
// gliding between 85 and 240 Hz, three formant bumps, 1/h roll-off up to 3.4 kHz) with
// syllabic envelopes, separated by 40-200 ms pauses, and fricative noise bursts. NOT speech and
// not claimed to be: the point is that it has a moving fundamental in and around the CTCSS band
// and a lot of energy in the voice band, which is what a tone decoder has to ignore.
inline std::vector<float> speechLike(std::size_t n, double fs, double peak, std::uint32_t seed) {
    std::mt19937 g(seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::normal_distribution<double> nd(0.0, 1.0);
    std::vector<float> v(n, 0.0f);
    std::size_t pos = 0;
    double phase = 0.0;
    while (pos < n) {
        const bool voiced = u(g) < 0.75;
        const std::size_t len = static_cast<std::size_t>((0.08 + 0.17 * u(g)) * fs);
        const double f0a = 85.0 + 155.0 * u(g), f0b = 85.0 + 155.0 * u(g);
        const double form[3] = {400.0 + 400.0 * u(g), 1200.0 + 800.0 * u(g), 2300.0 + 600.0 * u(g)};
        for (std::size_t i = 0; i < len && pos + i < n; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(len);
            const double env = std::sin(3.14159265358979 * t);
            double s = 0.0;
            if (voiced) {
                const double f0 = f0a + (f0b - f0a) * t;
                phase += kTwoPi * f0 / fs;
                for (int h = 1; h * f0 < 3400.0; ++h) {
                    const double fh = h * f0;
                    double gain = 1.0 / h;
                    for (double fm : form) { gain += 0.8 * std::exp(-std::pow((fh - fm) / 250.0, 2.0)) / std::sqrt(h); }
                    s += gain * std::sin(h * phase);
                }
                s *= 0.15;
            } else {
                s = 0.25 * nd(g);
            }
            v[pos + i] = static_cast<float>(s * env);
        }
        pos += len + static_cast<std::size_t>((0.04 + 0.16 * u(g)) * fs);
    }
    float mx = 1e-9f;
    for (float x : v) { mx = std::max(mx, std::fabs(x)); }
    for (auto& x : v) { x = static_cast<float>(x / mx * peak); }
    return v;
}

inline double rms(const std::vector<float>& v, std::size_t from, std::size_t to) {
    to = std::min(to, v.size());
    double s = 0;
    for (std::size_t i = from; i < to; ++i) { s += static_cast<double>(v[i]) * v[i]; }
    return std::sqrt(s / static_cast<double>(std::max<std::size_t>(to - from, 1)));
}

}  // namespace sbgen
