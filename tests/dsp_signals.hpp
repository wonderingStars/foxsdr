// dsp_signals.hpp - deterministic test signals for the demod performance pass (tests/test_dsp_equivalence.cpp and tools/bench_demod_chain): the
// same samples on every machine and compiler, generated block by block so ten seconds at 2 MS/s are never held in memory.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>

namespace dspsig {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// A small deterministic generator (xorshift64*), the same on every machine and compiler.
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    std::uint64_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 2685821657736338717ull;
    }
    // uniform in [-1, 1)
    float uni() { return static_cast<float>(static_cast<std::int64_t>(next() >> 11) - (1ll << 52)) / static_cast<float>(1ll << 52); }
    // a rough Gaussian (sum of four uniforms), unit-ish variance
    float gauss() { return 0.8660254f * (uni() + uni() + uni() + uni()); }
};

// THE SIGNALS. Both are generated block by block from a deterministic state, so ten seconds at 2 MS/s are never held in memory.
//   kGenerator  the program's own demonstration signal: tones at +300 kHz (-30 dB) and -500 kHz (-45 dB) over a -90 dB noise floor.
//   kStereoFm   a broadcast-FM multiplex (a 1 kHz and a 3 kHz tone in L and R of different levels, the 19 kHz pilot at 9%, the 38 kHz difference,
//               a 57 kHz subcarrier carrying a toggled tone) modulating a carrier at +300 kHz with 50 kHz deviation, with the pilot switched on and
//               off every `pilotToggleMs` (0 = always on): the lock, the gate opening and closing, the transitions P4 must keep exact.
class Signal {
public:
    enum Kind { kGenerator, kStereoFm };
    Signal(Kind kind, double rateHz, int pilotToggleMs = 0) : kind_(kind), rate_(rateHz), toggleSamples_(pilotToggleMs > 0 ? static_cast<std::uint64_t>(rateHz * pilotToggleMs / 1000.0) : 0), rng_(0x1234567ull) {}

    void fill(std::complex<float>* out, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i, ++pos_) {
            if (kind_ == kGenerator) {
                const double t = static_cast<double>(pos_) / rate_;
                const double a0 = kTwoPi * 300000.0 * t;
                const double a1 = kTwoPi * -500000.0 * t;
                const float noise = 0.00001581f;   // -90 dB total complex power: 10^(-90/20) / sqrt(2) per component
                out[i] = std::complex<float>(static_cast<float>(0.0316227766 * std::cos(a0) + 0.0056234132 * std::cos(a1)) + noise * rng_.gauss(),
                                             static_cast<float>(0.0316227766 * std::sin(a0) + 0.0056234132 * std::sin(a1)) + noise * rng_.gauss());
            } else {
                const double t = static_cast<double>(pos_) / rate_;
                const bool pilotOn = toggleSamples_ == 0 || ((pos_ / toggleSamples_) % 2) == 0;
                const double l = 0.5 * std::sin(kTwoPi * 1000.0 * t);
                const double r = 0.3 * std::sin(kTwoPi * 3000.0 * t + 0.4);
                const double sum = 0.5 * (l + r);
                const double dif = 0.5 * (l - r);
                const double pilot = pilotOn ? 0.09 * std::sin(kTwoPi * 19000.0 * t + 0.7) : 0.0;
                const double dsb = pilotOn ? dif * std::sin(2.0 * (kTwoPi * 19000.0 * t + 0.7)) : 0.0;
                const bool bit = ((pos_ / static_cast<std::uint64_t>(rate_ / 1187.5)) & 1u) != 0u;
                const double rds = 0.04 * (bit ? 1.0 : -1.0) * std::sin(kTwoPi * 57000.0 * t + 1.1);
                const double mpx = 0.9 * (sum + dsb) + pilot + rds;
                fmPhase_ += kTwoPi * 50000.0 * mpx / rate_;
                if (fmPhase_ > kTwoPi) { fmPhase_ -= kTwoPi; } else if (fmPhase_ < -kTwoPi) { fmPhase_ += kTwoPi; }
                const double a = kTwoPi * 300000.0 * t + fmPhase_;
                out[i] = std::complex<float>(static_cast<float>(0.5 * std::cos(a)) + 0.00002f * rng_.gauss(), static_cast<float>(0.5 * std::sin(a)) + 0.00002f * rng_.gauss());
            }
        }
    }

private:
    Kind kind_;
    double rate_;
    std::uint64_t toggleSamples_;
    std::uint64_t pos_ = 0;
    double fmPhase_ = 0.0;
    Rng rng_;
};

}  // namespace dspsig
