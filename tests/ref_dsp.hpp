// ref_dsp.hpp - the REFERENCE implementations the equivalence tests and the benchmark compare the optimised kernels against: the inner loops as they
// were at cebfddb5 (before the demod performance pass, docs/DIAGNOSTICS.md and D:/foxsdr-build/prep/PERF-demod-2026-11-23.md), copied verbatim into
// test code. They are never changed to match the product: if a product kernel and its reference disagree the product is wrong or the test is told
// why in a comment next to the tolerance.
//
//   RefFirDecimator   src/dsp/fir.cpp FirDecimator::process (scalar, double accumulators, tap 0 first)
//   RefNco            src/dsp/nco.cpp Nco::generate / Nco::mix (the library cos and sin of the double phase, once per sample)
//
// Plus the comparison helpers: a byte-exact compare (memcmp) and a sample-by-sample comparison reporting the maximum absolute difference and the
// signal-to-difference ratio in dB.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstring>
#include <limits>
#include <vector>

#include "dsp/fft.hpp"
#include "dsp/window.hpp"

namespace refdsp {

class RefFirDecimator {
public:
    RefFirDecimator(std::vector<float> taps, std::size_t decimation)
        : taps_(std::move(taps)), decim_(decimation), hist_(taps_.empty() ? 0 : taps_.size() - 1) {}

    std::size_t outputCapacity(std::size_t inCount) const noexcept { return inCount / decim_ + 1; }

    std::size_t process(const std::complex<float>* in, std::size_t inCount, std::complex<float>* out) {
        const std::size_t histLen = hist_.size();
        const std::size_t numTaps = taps_.size();
        work_.resize(histLen + inCount);
        std::copy(hist_.begin(), hist_.end(), work_.begin());
        if (inCount != 0) { std::copy(in, in + inCount, work_.begin() + static_cast<std::ptrdiff_t>(histLen)); }
        std::size_t produced = 0;
        for (std::size_t i = 0; i < inCount; ++i) {
            if (phase_ == 0) {
                const std::size_t newest = histLen + i;
                double re = 0.0;
                double im = 0.0;
                for (std::size_t j = 0; j < numTaps; ++j) {
                    const std::complex<float>& s = work_[newest - j];
                    const double hj = static_cast<double>(taps_[j]);
                    re += hj * static_cast<double>(s.real());
                    im += hj * static_cast<double>(s.imag());
                }
                out[produced++] = {static_cast<float>(re), static_cast<float>(im)};
            }
            ++phase_;
            if (phase_ == decim_) { phase_ = 0; }
        }
        if (histLen != 0) { std::copy(work_.end() - static_cast<std::ptrdiff_t>(histLen), work_.end(), hist_.begin()); }
        return produced;
    }

private:
    std::vector<float> taps_;
    std::size_t decim_;
    std::size_t phase_ = 0;
    std::vector<std::complex<float>> hist_;
    std::vector<std::complex<float>> work_;
};

class RefNco {
public:
    static constexpr double kTwoPi = 6.283185307179586476925286766559;
    void setFrequency(double normalized) { freq_ = normalized - std::floor(normalized + 0.5); }
    double frequency() const noexcept { return freq_; }
    double phase() const noexcept { return kTwoPi * phaseCycles_; }
    void reset() noexcept { phaseCycles_ = 0.0; }
    void generate(std::complex<float>* out, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            const double a = kTwoPi * phaseCycles_;
            out[i] = std::complex<float>(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
            step();
        }
    }
    void mix(const std::complex<float>* in, std::complex<float>* out, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            const double a = kTwoPi * phaseCycles_;
            const std::complex<float> osc(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
            out[i] = in[i] * osc;
            step();
        }
    }

private:
    void step() noexcept {
        phaseCycles_ += freq_;
        if (phaseCycles_ >= 0.5) {
            phaseCycles_ -= 1.0;
        } else if (phaseCycles_ < -0.5) {
            phaseCycles_ += 1.0;
        }
    }
    double freq_ = 0.0;
    double phaseCycles_ = 0.0;
};

// The legacy spectrum estimator as it was at cebfddb5: window, FFT, the per-block exponential average in the linear power domain, and the logarithm
// taken HERE, once per bin per block (src/dsp/spectrum.cpp SpectrumEstimator::process).
class RefSpectrumEstimator {
public:
    RefSpectrumEstimator(std::size_t fftSize, cascade::dsp::WindowType window)
        : fft_(fftSize), window_(cascade::dsp::makeWindow(window, fftSize)), windowed_(fftSize), spectrum_(fftSize), avgPower_(fftSize, 0.0f) {}
    void setAlpha(float alpha) { alpha_ = alpha >= 1.0f ? 1.0f : (alpha > 1e-4f ? alpha : 1e-4f); }
    void process(const std::complex<float>* in, float* outDb) {
        constexpr float kPowerFloor = 1e-20f;
        const std::size_t n = fft_.size();
        for (std::size_t i = 0; i < n; ++i) { windowed_[i] = in[i] * window_[i]; }
        fft_.forward(windowed_.data(), spectrum_.data());
        const float invN2 = 1.0f / (static_cast<float>(n) * static_cast<float>(n));
        const std::size_t half = n / 2;
        for (std::size_t i = 0; i < n; ++i) {
            std::size_t k = i + half;
            if (k >= n) { k -= n; }
            const float p = std::norm(spectrum_[k]) * invN2;
            avgPower_[i] = primed_ ? alpha_ * p + (1.0f - alpha_) * avgPower_[i] : p;
            outDb[i] = 10.0f * std::log10(avgPower_[i] > kPowerFloor ? avgPower_[i] : kPowerFloor);
        }
        primed_ = true;
    }

private:
    cascade::dsp::ComplexFFT fft_;
    std::vector<float> window_;
    std::vector<std::complex<float>> windowed_, spectrum_;
    std::vector<float> avgPower_;
    float alpha_ = 1.0f;
    bool primed_ = false;
};

// ---- comparison helpers ---------------------------------------------------------------------------------------------------------------------

struct Diff {
    std::size_t count = 0;            // samples compared (components for a complex run)
    std::size_t differing = 0;        // components whose bits differ
    double maxAbs = 0.0;              // the largest |ref - new| of any component
    double signalPower = 0.0;         // sum of ref^2
    double diffPower = 0.0;           // sum of (ref - new)^2
    bool sameBits = true;             // memcmp equal
    // 10 log10(sum ref^2 / sum (ref - new)^2); +infinity when the two are identical
    double sdrDb() const {
        if (diffPower <= 0.0) { return std::numeric_limits<double>::infinity(); }
        if (signalPower <= 0.0) { return -std::numeric_limits<double>::infinity(); }
        return 10.0 * std::log10(signalPower / diffPower);
    }
};

inline Diff compareFloats(const float* ref, const float* got, std::size_t n) {
    Diff d;
    d.count = n;
    d.sameBits = n == 0 || std::memcmp(ref, got, n * sizeof(float)) == 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double r = static_cast<double>(ref[i]);
        const double e = r - static_cast<double>(got[i]);
        if (std::memcmp(&ref[i], &got[i], sizeof(float)) != 0) { ++d.differing; }
        d.maxAbs = std::max(d.maxAbs, std::fabs(e));
        d.signalPower += r * r;
        d.diffPower += e * e;
    }
    return d;
}
inline Diff compareFloats(const std::vector<float>& a, const std::vector<float>& b) {
    Diff d = compareFloats(a.data(), b.data(), std::min(a.size(), b.size()));
    if (a.size() != b.size()) { d.sameBits = false; }
    return d;
}
inline Diff compareComplex(const std::complex<float>* ref, const std::complex<float>* got, std::size_t n) {
    return compareFloats(reinterpret_cast<const float*>(ref), reinterpret_cast<const float*>(got), 2 * n);
}
inline Diff compareComplex(const std::vector<std::complex<float>>& a, const std::vector<std::complex<float>>& b) {
    Diff d = compareComplex(a.data(), b.data(), std::min(a.size(), b.size()));
    if (a.size() != b.size()) { d.sameBits = false; }
    return d;
}

}  // namespace refdsp
