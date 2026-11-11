// sigmeas_gen.hpp - signal generators for the sigmeas tests: every signal can carry noise, a
// frequency offset and a sample-rate error, as the lane brief requires. Test support only.
//
// SAMPLE-RATE ERROR. A generator that takes `trueRate` builds the signal on the clock of a
// converter that really runs at trueRate while the analyser is told the NOMINAL rate: every
// frequency the analyser then reports is the true one times trueRate/nominal... the tests state
// the expected value with that factor.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <random>
#include <vector>

#include "dsp/sigmeas_common.hpp"

namespace sg {

using cascade::sigmeas::Cf;
constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr double kTwoPi = 2.0 * kPi;

struct Rng {
    std::mt19937_64 eng;
    explicit Rng(std::uint64_t seed) : eng(seed) {}
    double gauss() { return std::normal_distribution<double>(0.0, 1.0)(eng); }
    double uni() { return std::uniform_real_distribution<double>(0.0, 1.0)(eng); }
    int bit() { return (eng() >> 17) & 1 ? 1 : 0; }
};

inline double meanPower(const std::vector<Cf>& x) {
    double s = 0.0;
    for (const Cf& c : x) { s += std::norm(c); }
    return x.empty() ? 0.0 : s / static_cast<double>(x.size());
}

// Adds complex white Gaussian noise so that signal power / noise power = snrDb (over the whole
// sampled band). The signal power is the mean power of `x` as it stands.
inline void addNoise(std::vector<Cf>& x, double snrDb, Rng& rng) {
    const double ps = meanPower(x);
    const double pn = ps / std::pow(10.0, snrDb / 10.0);
    const double s = std::sqrt(pn / 2.0);
    for (Cf& c : x) {
        c += Cf(static_cast<float>(s * rng.gauss()), static_cast<float>(s * rng.gauss()));
    }
}

// A multisine whose power spectrum is EXACTLY shape(f) at the grid frequencies k*rate/N (random
// phases): no randomness in its PSD, so a bandwidth measured on it has a known answer.
// `centreHz` shifts it. N must be a power of two. Normalised to unit mean power.
template <class Shape>
std::vector<Cf> multisine(std::size_t N, double rate, Shape shape, double centreHz, Rng& rng) {
    cascade::sigmeas::Fft fft(N);
    std::vector<std::complex<double>> a(N, {0.0, 0.0});
    const double df = rate / static_cast<double>(N);
    for (std::size_t k = 0; k < N; ++k) {
        const double f = (k < N / 2 ? static_cast<double>(k) : static_cast<double>(k) - static_cast<double>(N)) * df;
        const double p = shape(f);
        if (p > 0.0) {
            const double ph = kTwoPi * rng.uni();
            a[k] = std::polar(std::sqrt(p), ph);
        }
    }
    // inverse via conjugation: x = conj(FFT(conj(a))) (unscaled)
    for (auto& v : a) { v = std::conj(v); }
    fft.forward(a.data());
    std::vector<Cf> out(N);
    double pw = 0.0;
    for (std::size_t i = 0; i < N; ++i) {
        const std::complex<double> v = std::conj(a[i]);
        pw += std::norm(v);
    }
    pw /= static_cast<double>(N);
    const double g = pw > 0.0 ? 1.0 / std::sqrt(pw) : 1.0;
    double phase = 0.0;
    const double dph = kTwoPi * centreHz / rate;
    for (std::size_t i = 0; i < N; ++i) {
        const std::complex<double> v = std::conj(a[i]) * g * std::polar(1.0, phase);
        out[i] = Cf(static_cast<float>(v.real()), static_cast<float>(v.imag()));
        phase += dph;
    }
    return out;
}

// Repeats a block until it is n long.
inline std::vector<Cf> repeatTo(const std::vector<Cf>& blk, std::size_t n) {
    std::vector<Cf> out(n);
    for (std::size_t i = 0; i < n; ++i) { out[i] = blk[i % blk.size()]; }
    return out;
}

// Constant-envelope FM: carrier offset `fo`, tone `fm`, peak deviation `dev`; plus AM depth
// `am` at tone `amHz` if set. t runs on the true clock.
inline std::vector<Cf> fmAm(std::size_t n, double trueRate, double fo, double fm, double dev,
                            double am = 0.0, double amHz = 0.0, double amp = 0.5) {
    std::vector<Cf> x(n);
    double ph = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / trueRate;
        const double f = fo + dev * std::sin(kTwoPi * fm * t);
        ph += kTwoPi * f / trueRate;
        const double env = amp * (1.0 + am * std::sin(kTwoPi * amHz * t));
        x[i] = Cf(static_cast<float>(env * std::cos(ph)), static_cast<float>(env * std::sin(ph)));
    }
    return x;
}

// Binary FSK: random bits at `baud` (true clock), tones fo -/+ shift/2 (bit 1 = mark = higher),
// continuous phase, symbol-edge jitter (fraction of a symbol, uniform +-), optional bit list.
inline std::vector<Cf> fsk(std::size_t n, double trueRate, double fo, double shift, double baud,
                           double jitter, Rng& rng, std::vector<int>* bitsOut = nullptr,
                           const std::vector<int>* fixedBits = nullptr, double amp = 0.5) {
    std::vector<Cf> x(n);
    const double T = trueRate / baud;  // samples per symbol
    std::vector<int> bits;
    std::vector<double> edge;  // end time (sample units) of each symbol
    double t = 0.0;
    std::size_t k = 0;
    while (t < static_cast<double>(n) + T) {
        const int b = fixedBits ? (*fixedBits)[k % fixedBits->size()] : rng.bit();
        bits.push_back(b);
        t += T;
        // jitter each EDGE independently of the nominal grid
        edge.push_back(static_cast<double>(k + 1) * T + jitter * T * (2.0 * rng.uni() - 1.0));
        ++k;
    }
    if (bitsOut) { *bitsOut = bits; }
    double ph = 0.0;
    std::size_t sym = 0;
    for (std::size_t i = 0; i < n; ++i) {
        while (sym + 1 < edge.size() && static_cast<double>(i) >= edge[sym]) { ++sym; }
        const double f = fo + (bits[sym] ? 0.5 : -0.5) * shift;
        ph += kTwoPi * f / trueRate;
        x[i] = Cf(static_cast<float>(amp * std::cos(ph)), static_cast<float>(amp * std::sin(ph)));
    }
    return x;
}

// On-off keying of a carrier at fo: bit 1 = carrier on. Edges are `rise` samples long (raised
// cosine). Jitter as in fsk().
inline std::vector<Cf> ook(std::size_t n, double trueRate, double fo, double baud, double jitter,
                           Rng& rng, double rise = 2.0, std::vector<int>* bitsOut = nullptr,
                           const std::vector<int>* fixedBits = nullptr, double amp = 0.5) {
    std::vector<Cf> x(n);
    const double T = trueRate / baud;
    std::vector<int> bits;
    std::vector<double> edge;
    std::size_t k = 0;
    while (static_cast<double>(k) * T < static_cast<double>(n) + T) {
        const int b = fixedBits ? (*fixedBits)[k % fixedBits->size()] : rng.bit();
        bits.push_back(b);
        edge.push_back(static_cast<double>(k + 1) * T + jitter * T * (2.0 * rng.uni() - 1.0));
        ++k;
    }
    if (bitsOut) { *bitsOut = bits; }
    double ph = 0.0;
    std::size_t sym = 0;
    for (std::size_t i = 0; i < n; ++i) {
        while (sym + 1 < edge.size() && static_cast<double>(i) >= edge[sym]) { ++sym; }
        // level is a smooth step towards the symbol's level
        const double startOfSym = sym == 0 ? 0.0 : edge[sym - 1];
        const double since = static_cast<double>(i) - startOfSym;
        const int prev = sym == 0 ? bits[0] : bits[sym - 1];
        double lvl = static_cast<double>(bits[sym]);
        if (prev != bits[sym] && since < rise) {
            const double u = since / rise;
            const double s = 0.5 - 0.5 * std::cos(kPi * u);
            lvl = static_cast<double>(prev) + (static_cast<double>(bits[sym]) - prev) * s;
        }
        ph += kTwoPi * fo / trueRate;
        x[i] = Cf(static_cast<float>(amp * lvl * std::cos(ph)), static_cast<float>(amp * lvl * std::sin(ph)));
    }
    return x;
}

}  // namespace sg
