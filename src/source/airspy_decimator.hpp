// airspy_decimator.hpp - the Airspy R2 / Mini's software DECIMATION: the
// complex stream the half-band converter produces, divided in rate by a power
// of two before it reaches the rest of FoxSDR.
//
// WHAT IT IS FOR, in the words of the reference Airspy application's own
// guide (SDRsharp - The Guide v2.1, Airspy R2/Mini panel): decimation "allows
// a lower bandwidth to be used to the benefit of bit resolution and therefore
// lower quantisation noise", with the values "none, 2, 4, 8, 16, 32 and 64",
// and the span shown is 80% of the decimated rate (an R2 at 10 MSPS: 8 MHz
// undecimated down to 125 kHz at 64). Every halving of the rate halves the
// noise bandwidth a sample carries - about 3 dB - and halves the work the
// whole receiver does after it, which on a slow machine is the difference
// between a 10 MS/s stream that plays and one that breaks up.
//
// HOW. log2(D) identical half-band stages, each a decimate-by-two. A half-band
// filter's even-offset taps are exactly zero, so a stage costs one multiply
// per NON-ZERO side tap PAIR per output. The design is the one FoxSDR uses for
// its channel filter - a Blackman-Harris windowed sinc, 92 dB floor, length
// from fred harris' rule - with its numbers chosen for the guide's 80%:
//   cutoff  0.25 of the stage input rate (the half-band point),
//   half-width 0.05, so the passband is flat to 0.20 of the input rate (0.40
//   of the output rate: the inner 80% of the output band) and everything that
//   folds into that inner 80% - input frequencies beyond 0.30 - is at the
//   window's floor.
// 83 taps, 21 non-zero pairs plus the centre; tests/test_airspy_source.cpp
// measures the passband and the alias floor off the taps themselves.
//
// Written here from the textbook structure, not from any Airspy source: the
// reference application's decimation is closed, and libairspy has none.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace cascade::source::airspy {

// The factors the reference application offers (the guide quoted above).
constexpr unsigned kDecimations[] = {1, 2, 4, 8, 16, 32, 64};
constexpr unsigned kMaxDecimation = 64;

constexpr bool decimationValid(unsigned d) {
    return d >= 1 && d <= kMaxDecimation && (d & (d - 1)) == 0;
}

// The share of the decimated rate that is alias-free and flat - what the
// reference application calls the displayed bandwidth.
constexpr double kDecimatedUsableFraction = 0.8;

// The half-band stage's taps. 83 = 4 * 20 + 3, so the centre sits at an odd
// index and the zero taps land on every even offset from it.
constexpr std::size_t kHalfBandStageTaps = 83;
constexpr std::size_t kHalfBandStagePairs = 21;  // offsets 1, 3, ..., 41

// The non-zero side taps h[c+1], h[c+3], ..., h[c+41] (h[c-k] is the same
// value), scaled so the whole filter sums to exactly one: the centre is 0.5
// and the pairs carry the other half. Computed once.
inline const std::array<float, kHalfBandStagePairs>& halfBandStagePairs() {
    static const std::array<float, kHalfBandStagePairs> taps = [] {
        constexpr double kPi = 3.14159265358979323846;
        constexpr double c = (kHalfBandStageTaps - 1) / 2.0;  // 41
        std::array<double, kHalfBandStagePairs> raw{};
        double sum = 0.0;
        for (std::size_t i = 0; i < kHalfBandStagePairs; ++i) {
            const double k = static_cast<double>(2 * i + 1);
            // sinc for a cutoff of a quarter of the rate: 0.5 * sin(pi k/2)/(pi k/2).
            const double sinc = std::sin(kPi * k / 2.0) / (kPi * k);
            // 4-term Blackman-Harris in its symmetric form, at position c + k.
            const double x = 2.0 * kPi * (c + k) / (kHalfBandStageTaps - 1);
            const double w = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) -
                             0.01168 * std::cos(3 * x);
            raw[i] = sinc * w;
            sum += 2.0 * raw[i];
        }
        std::array<float, kHalfBandStagePairs> out{};
        for (std::size_t i = 0; i < kHalfBandStagePairs; ++i) {
            out[i] = static_cast<float>(raw[i] * 0.5 / sum);
        }
        return out;
    }();
    return taps;
}

// The whole 83-tap kernel, for the tests' frequency-response measurement.
inline std::vector<float> halfBandStageKernel() {
    std::vector<float> h(kHalfBandStageTaps, 0.0f);
    const std::size_t c = (kHalfBandStageTaps - 1) / 2;
    h[c] = 0.5f;
    const auto& p = halfBandStagePairs();
    for (std::size_t i = 0; i < kHalfBandStagePairs; ++i) {
        h[c + 2 * i + 1] = p[i];
        h[c - 2 * i - 1] = p[i];
    }
    return h;
}

// A streaming decimate-by-D. Output sample m is the filtered input at index
// m*D (each stage emits on its own grid's phase 0), so any split of the input
// into blocks gives the same outputs. process() may be called with out == in.
class PowerOfTwoDecimator {
public:
    // D must satisfy decimationValid; anything else is taken as 1.
    void configure(unsigned d) {
        if (!decimationValid(d)) { d = 1; }
        factor_ = d;
        stages_.clear();
        for (unsigned f = d; f > 1; f /= 2) { stages_.emplace_back(); }
    }

    unsigned factor() const { return factor_; }

    void reset() {
        for (Stage& s : stages_) { s = Stage{}; }
    }

    std::size_t process(const std::complex<float>* in, std::size_t n, std::complex<float>* out) {
        if (stages_.empty()) {
            if (out != in) {
                for (std::size_t i = 0; i < n; ++i) { out[i] = in[i]; }
            }
            return n;
        }
        const std::complex<float>* src = in;
        std::size_t count = n;
        for (Stage& s : stages_) {
            count = s.process(src, count, out);
            src = out;
        }
        return count;
    }

private:
    struct Stage {
        // The last kHalfBandStageTaps - 1 inputs, oldest first.
        std::vector<std::complex<float>> hist =
            std::vector<std::complex<float>>(kHalfBandStageTaps - 1);
        std::vector<std::complex<float>> work;
        unsigned phase = 0;  // 0 = the next input is on the output grid

        // Writes at most n/2 + 1 outputs; out may alias in (the input is
        // copied into `work` before anything is written).
        std::size_t process(const std::complex<float>* in, std::size_t n,
                            std::complex<float>* out) {
            const std::size_t histLen = hist.size();
            work.resize(histLen + n);
            std::copy(hist.begin(), hist.end(), work.begin());
            std::copy(in, in + n, work.begin() + static_cast<std::ptrdiff_t>(histLen));
            const auto& pairs = halfBandStagePairs();
            constexpr std::size_t c = (kHalfBandStageTaps - 1) / 2;
            std::size_t produced = 0;
            for (std::size_t i = 0; i < n; ++i) {
                if (phase == 0) {
                    // Newest sample work[histLen + i]; the centre tap sits
                    // c samples before it.
                    const std::complex<float>* mid = work.data() + histLen + i - c;
                    float re = 0.5f * mid[0].real();
                    float im = 0.5f * mid[0].imag();
                    for (std::size_t p = 0; p < kHalfBandStagePairs; ++p) {
                        const std::size_t k = 2 * p + 1;
                        re += pairs[p] * (mid[k].real() + mid[-static_cast<std::ptrdiff_t>(k)].real());
                        im += pairs[p] * (mid[k].imag() + mid[-static_cast<std::ptrdiff_t>(k)].imag());
                    }
                    out[produced++] = {re, im};
                }
                phase ^= 1u;
            }
            std::copy(work.end() - static_cast<std::ptrdiff_t>(histLen), work.end(), hist.begin());
            return produced;
        }
    };

    unsigned factor_ = 1;
    std::vector<Stage> stages_;
};

}  // namespace cascade::source::airspy
