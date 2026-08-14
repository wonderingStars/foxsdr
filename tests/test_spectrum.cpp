// Tests for dsp/spectrum.hpp — the reference spectrum is a windowed direct
// DFT computed in-test in double precision, with the same documented scaling
// (|X|^2 / N^2, fftshifted), never values copied from the implementation.
// SPDX-License-Identifier: MIT
#include "dsp/spectrum.hpp"

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "dsp/window.hpp"
#include "test_check.hpp"

using cascade::dsp::SpectrumEstimator;
using cascade::dsp::WindowType;

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// Fixed-seed LCG (Numerical Recipes constants) — deterministic test input.
struct Lcg {
    std::uint32_t s;
    explicit Lcg(std::uint32_t seed) : s(seed) {}
    // Uniform in [-1, 1).
    float next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(s) * (2.0f / 4294967296.0f) - 1.0f;
    }
};

std::vector<std::complex<float>> randomBlock(std::size_t n, std::uint32_t seed) {
    Lcg lcg(seed);
    std::vector<std::complex<float>> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float re = lcg.next();
        const float im = lcg.next();
        x[i] = {re, im};
    }
    return x;
}

// Complex tone exp(i*2*pi*cycles*n/N); negative `cycles` gives a negative
// frequency.
std::vector<std::complex<float>> tone(std::size_t n, double cycles) {
    std::vector<std::complex<float>> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double ang = kTwoPi * cycles * static_cast<double>(i) /
                           static_cast<double>(n);
        x[i] = {static_cast<float>(std::cos(ang)), static_cast<float>(std::sin(ang))};
    }
    return x;
}

// Reference: fftshifted linear power spectrum |X[k]|^2 / N^2 of the windowed
// block, via direct DFT in double precision.
std::vector<double> refShiftedPower(const std::vector<std::complex<float>>& x,
                                    WindowType wt) {
    const std::size_t n = x.size();
    auto w = cascade::dsp::makeWindow(wt, n);
    std::vector<double> p(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t k = (i + n / 2) % n;  // fftshift: DC to slot n/2
        std::complex<double> acc(0.0, 0.0);
        for (std::size_t m = 0; m < n; ++m) {
            const double ang = -kTwoPi * static_cast<double>(k) *
                               static_cast<double>(m) / static_cast<double>(n);
            const std::complex<double> xm(
                static_cast<double>(x[m].real()) * static_cast<double>(w[m]),
                static_cast<double>(x[m].imag()) * static_cast<double>(w[m]));
            acc += xm * std::complex<double>(std::cos(ang), std::sin(ang));
        }
        p[i] = std::norm(acc) / (static_cast<double>(n) * static_cast<double>(n));
    }
    return p;
}

std::size_t argmax(const std::vector<float>& v) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < v.size(); ++i) {
        if (v[i] > v[best]) { best = i; }
    }
    return best;
}

}  // namespace

int main() {
    const std::size_t n = 64;

    // fftshift orientation: DC lands at bin N/2, a +5-cycle tone at N/2 + 5,
    // a -5-cycle tone at N/2 - 5.
    {
        SpectrumEstimator est(n, WindowType::Rectangular);
        std::vector<float> db(n);

        std::vector<std::complex<float>> dc(n, {0.7f, 0.0f});
        est.process(dc.data(), db.data());
        CHECK(argmax(db) == n / 2);
        // Documented scaling: amplitude-0.7 DC reads 20*log10(0.7) dBFS.
        CHECK_NEAR(db[n / 2], 20.0 * std::log10(0.7), 1e-3);

        est.reset();
        auto pos = tone(n, 5.0);
        est.process(pos.data(), db.data());
        CHECK(argmax(db) == n / 2 + 5);
        // Unit tone at an exact bin, rectangular window: 0 dBFS by the
        // documented scaling.
        CHECK_NEAR(db[n / 2 + 5], 0.0, 1e-3);

        est.reset();
        auto neg = tone(n, -5.0);
        est.process(neg.data(), db.data());
        CHECK(argmax(db) == n / 2 - 5);
    }

    // Single-shot output matches the in-test windowed DFT reference, bin by
    // bin, for a non-trivial window.
    {
        SpectrumEstimator est(n, WindowType::Hann);
        auto x = randomBlock(n, 0xD00D01u);
        std::vector<float> db(n);
        est.process(x.data(), db.data());
        auto ref = refShiftedPower(x, WindowType::Hann);
        for (std::size_t i = 0; i < n; ++i) {
            const double refDb = 10.0 * std::log10(ref[i] > 1e-20 ? ref[i] : 1e-20);
            CHECK_NEAR(db[i], refDb, 0.05);
        }
    }

    // alpha = 1: processing A then B equals a fresh estimator seeing only B —
    // no history may leak through.
    {
        SpectrumEstimator est(n, WindowType::Hann);
        est.setAlpha(1.0f);
        auto a = randomBlock(n, 0x11111111u);
        auto b = randomBlock(n, 0x22222222u);
        std::vector<float> dbAB(n);
        est.process(a.data(), dbAB.data());
        est.process(b.data(), dbAB.data());

        SpectrumEstimator fresh(n, WindowType::Hann);
        std::vector<float> dbB(n);
        fresh.process(b.data(), dbB.data());
        for (std::size_t i = 0; i < n; ++i) {
            CHECK_NEAR(dbAB[i], dbB[i], 1e-9);
        }
    }

    // alpha < 1: the estimator follows the EMA recurrence exactly. Reference
    // runs the same recurrence in double over in-test DFT powers for a
    // deterministic schedule of six distinct blocks.
    {
        const float alpha = 0.25f;
        SpectrumEstimator est(n, WindowType::Rectangular);
        est.setAlpha(alpha);
        std::vector<float> db(n);
        std::vector<double> refAvg(n, 0.0);
        for (int blk = 0; blk < 6; ++blk) {
            auto x = randomBlock(n, 0x5EED0000u + static_cast<std::uint32_t>(blk));
            est.process(x.data(), db.data());
            auto p = refShiftedPower(x, WindowType::Rectangular);
            for (std::size_t i = 0; i < n; ++i) {
                // First block primes the average (documented in spectrum.hpp).
                refAvg[i] = (blk == 0)
                                ? p[i]
                                : static_cast<double>(alpha) * p[i] +
                                      (1.0 - static_cast<double>(alpha)) * refAvg[i];
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            const double refDb =
                10.0 * std::log10(refAvg[i] > 1e-20 ? refAvg[i] : 1e-20);
            CHECK_NEAR(db[i], refDb, 0.05);
        }
    }

    // alpha < 1 convergence: prime with block A, then feed block B repeatedly;
    // the average must converge to B's single-shot spectrum (the mean of the
    // steady schedule), and be closer after convergence than it was early on.
    {
        SpectrumEstimator est(n, WindowType::Rectangular);
        est.setAlpha(0.25f);
        auto a = randomBlock(n, 0xAAAAAAAAu);
        auto b = randomBlock(n, 0xBBBBBBBBu);
        std::vector<float> db(n);
        est.process(a.data(), db.data());

        SpectrumEstimator single(n, WindowType::Rectangular);
        std::vector<float> target(n);
        single.process(b.data(), target.data());

        est.process(b.data(), db.data());
        const double earlyErr = std::fabs(static_cast<double>(db[0]) -
                                          static_cast<double>(target[0]));
        for (int i = 0; i < 100; ++i) { est.process(b.data(), db.data()); }
        for (std::size_t i = 0; i < n; ++i) {
            CHECK_NEAR(db[i], target[i], 0.01);
        }
        const double lateErr = std::fabs(static_cast<double>(db[0]) -
                                         static_cast<double>(target[0]));
        CHECK(lateErr <= earlyErr);
    }

    // Window comparison: the same exact-bin unit tone peaks 20*log10(CG)
    // lower under Hann than under rectangular, because the documented scaling
    // does not compensate coherent gain. Expected value comes from
    // window.hpp's coherentGain, not from the estimator.
    {
        auto x = tone(n, 8.0);
        std::vector<float> dbRect(n);
        std::vector<float> dbHann(n);
        SpectrumEstimator rect(n, WindowType::Rectangular);
        SpectrumEstimator hann(n, WindowType::Hann);
        rect.process(x.data(), dbRect.data());
        hann.process(x.data(), dbHann.data());

        const std::size_t peakBin = n / 2 + 8;
        CHECK(argmax(dbRect) == peakBin);
        CHECK(argmax(dbHann) == peakBin);

        auto w = cascade::dsp::makeWindow(WindowType::Hann, n);
        const double cg =
            static_cast<double>(cascade::dsp::coherentGain(w.data(), w.size()));
        const double expectedDiff = -20.0 * std::log10(cg);  // ~ +6.02 dB
        CHECK_NEAR(dbRect[peakBin] - dbHann[peakBin], expectedDiff, 0.01);
    }

    // reset() forgets history: A, reset, B equals single-shot B even with
    // averaging enabled.
    {
        SpectrumEstimator est(n, WindowType::BlackmanHarris);
        est.setAlpha(0.1f);
        auto a = randomBlock(n, 0x0BADF00Du);
        auto b = randomBlock(n, 0xCAFED00Du);
        std::vector<float> db(n);
        est.process(a.data(), db.data());
        est.reset();
        est.process(b.data(), db.data());

        SpectrumEstimator fresh(n, WindowType::BlackmanHarris);
        std::vector<float> dbB(n);
        fresh.process(b.data(), dbB.data());
        for (std::size_t i = 0; i < n; ++i) {
            CHECK_NEAR(db[i], dbB[i], 1e-9);
        }
    }

    return testSummary("test_spectrum");
}
