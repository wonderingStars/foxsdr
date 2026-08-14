// Tests for dsp/fft.hpp — proven against an in-test direct O(n^2) DFT, never
// against constants derived from the implementation.
// SPDX-License-Identifier: MIT
#include "dsp/fft.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "test_check.hpp"

using cascade::dsp::ComplexFFT;

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

// Direct DFT in double precision: X[k] = sum_n x[n] * exp(sign*2*pi*i*k*n/N).
std::vector<std::complex<double>> directDft(const std::vector<std::complex<float>>& x,
                                            double sign) {
    const std::size_t n = x.size();
    std::vector<std::complex<double>> out(n);
    for (std::size_t k = 0; k < n; ++k) {
        std::complex<double> acc(0.0, 0.0);
        for (std::size_t m = 0; m < n; ++m) {
            const double ang =
                sign * kTwoPi * static_cast<double>(k) * static_cast<double>(m) /
                static_cast<double>(n);
            acc += std::complex<double>(x[m].real(), x[m].imag()) *
                   std::complex<double>(std::cos(ang), std::sin(ang));
        }
        out[k] = acc;
    }
    return out;
}

// Compare wrapper forward output against the direct DFT with a tolerance
// relative to the largest reference magnitude (per-bin relative tolerance
// would explode on near-null bins).
void checkForwardMatchesDft(std::size_t n, std::uint32_t seed) {
    auto x = randomBlock(n, seed);
    auto ref = directDft(x, -1.0);

    ComplexFFT fft(n);
    std::vector<std::complex<float>> got(n);
    fft.forward(x.data(), got.data());

    double maxMag = 0.0;
    for (std::size_t k = 0; k < n; ++k) {
        maxMag = std::max(maxMag, std::abs(ref[k]));
    }
    const double tol = 1e-4 * maxMag;
    for (std::size_t k = 0; k < n; ++k) {
        CHECK_NEAR(got[k].real(), ref[k].real(), tol);
        CHECK_NEAR(got[k].imag(), ref[k].imag(), tol);
    }
}

}  // namespace

int main() {
    // Size legality per pffft.h: 2^a * 3^b * 5^c, a >= 5. 32 is the smallest.
    CHECK(ComplexFFT::isValidSize(32));
    CHECK(ComplexFFT::isValidSize(96));    // 2^5 * 3
    CHECK(ComplexFFT::isValidSize(160));   // 2^5 * 5
    CHECK(ComplexFFT::isValidSize(480));   // 2^5 * 3 * 5
    CHECK(ComplexFFT::isValidSize(4096));
    CHECK(!ComplexFFT::isValidSize(0));
    CHECK(!ComplexFFT::isValidSize(16));   // a = 4: below pffft's minimum
    CHECK(!ComplexFFT::isValidSize(48));   // 2^4 * 3: a = 4 again
    CHECK(!ComplexFFT::isValidSize(33));   // factor 11
    CHECK(!ComplexFFT::isValidSize(224));  // 2^5 * 7: factor 7
    bool threw = false;
    try {
        ComplexFFT bad(16);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    // Wrapper matches the direct DFT at the smallest legal size, and at a
    // size exercising the radix-3 and radix-5 paths.
    checkForwardMatchesDft(32, 0xC0FFEEu);
    checkForwardMatchesDft(480, 0xBEEF01u);

    // A unit tone at an exact bin concentrates all energy there: |X[k0]| = N,
    // every other bin only holds float rounding leakage.
    {
        const std::size_t n = 32;
        const std::size_t k0 = 5;
        std::vector<std::complex<float>> tone(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double ang = kTwoPi * static_cast<double>(k0) *
                               static_cast<double>(i) / static_cast<double>(n);
            tone[i] = {static_cast<float>(std::cos(ang)),
                       static_cast<float>(std::sin(ang))};
        }
        ComplexFFT fft(n);
        std::vector<std::complex<float>> X(n);
        fft.forward(tone.data(), X.data());
        std::size_t peak = 0;
        double peakMag = 0.0;
        for (std::size_t k = 0; k < n; ++k) {
            const double mag = std::abs(std::complex<double>(X[k].real(), X[k].imag()));
            if (mag > peakMag) {
                peakMag = mag;
                peak = k;
            }
        }
        CHECK(peak == k0);
        CHECK_NEAR(peakMag, static_cast<double>(n), 1e-3 * static_cast<double>(n));
        for (std::size_t k = 0; k < n; ++k) {
            if (k == k0) { continue; }
            const double mag = std::abs(std::complex<double>(X[k].real(), X[k].imag()));
            CHECK(mag < 1e-3 * static_cast<double>(n));
        }
    }

    // Parseval: sum |x[n]|^2 == (1/N) * sum |X[k]|^2 for the unscaled forward
    // transform.
    {
        const std::size_t n = 96;
        auto x = randomBlock(n, 0xA5A5A5u);
        ComplexFFT fft(n);
        std::vector<std::complex<float>> X(n);
        fft.forward(x.data(), X.data());
        double timeEnergy = 0.0;
        double freqEnergy = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            timeEnergy += std::norm(std::complex<double>(x[i].real(), x[i].imag()));
            freqEnergy += std::norm(std::complex<double>(X[i].real(), X[i].imag()));
        }
        freqEnergy /= static_cast<double>(n);
        CHECK_NEAR(freqEnergy, timeEnergy, 1e-4 * timeEnergy);
    }

    // Round trip: inverse(forward(x)) == x, because inverse() folds in the
    // 1/N that pffft leaves out (documented in fft.hpp).
    {
        const std::size_t n = 160;
        auto x = randomBlock(n, 0x1234567u);
        ComplexFFT fft(n);
        std::vector<std::complex<float>> X(n);
        std::vector<std::complex<float>> back(n);
        fft.forward(x.data(), X.data());
        fft.inverse(X.data(), back.data());
        for (std::size_t i = 0; i < n; ++i) {
            CHECK_NEAR(back[i].real(), x[i].real(), 1e-4);
            CHECK_NEAR(back[i].imag(), x[i].imag(), 1e-4);
        }
    }

    // In-place operation (in == out) is part of the contract.
    {
        const std::size_t n = 32;
        auto x = randomBlock(n, 0xFACEu);
        ComplexFFT fft(n);
        std::vector<std::complex<float>> outOfPlace(n);
        fft.forward(x.data(), outOfPlace.data());
        std::vector<std::complex<float>> inPlace = x;
        fft.forward(inPlace.data(), inPlace.data());
        for (std::size_t i = 0; i < n; ++i) {
            CHECK_NEAR(inPlace[i].real(), outOfPlace[i].real(), 1e-12);
            CHECK_NEAR(inPlace[i].imag(), outOfPlace[i].imag(), 1e-12);
        }
    }

    // Move semantics. The moved-to object must own the pffft resources
    // outright: the moved-from object is destroyed FIRST, freshly freed
    // heap blocks are deliberately re-allocated and scribbled on, and only
    // then is the moved-to object used for a transform verified against the
    // direct DFT. A move that leaves the source's pointers dangling either
    // computes through clobbered memory (FAIL lines) or double-frees when
    // the moved-to object destructs (crash) — both are red.
    {
        const std::size_t n = 32;
        auto x = randomBlock(n, 0x5EEDFEEDu);
        auto ref = directDft(x, -1.0);
        double maxMag = 0.0;
        for (std::size_t k = 0; k < n; ++k) {
            maxMag = std::max(maxMag, std::abs(ref[k]));
        }
        const double tol = 1e-4 * maxMag;

        // Move construction.
        {
            auto src = std::make_unique<ComplexFFT>(n);
            ComplexFFT moved(std::move(*src));
            CHECK(moved.size() == n);
            CHECK(src->size() == 0);  // moved-from is the empty state
            src.reset();              // moved-from destructs before any use
            std::vector<std::vector<float>> clobber;
            for (int i = 0; i < 8; ++i) {
                clobber.emplace_back(2 * n, 12345.0f);  // reuse freed blocks
            }
            std::vector<std::complex<float>> got(n);
            moved.forward(x.data(), got.data());
            for (std::size_t k = 0; k < n; ++k) {
                CHECK_NEAR(got[k].real(), ref[k].real(), tol);
                CHECK_NEAR(got[k].imag(), ref[k].imag(), tol);
            }
        }  // moved destructs: a shallow move double-frees here

        // Move assignment onto an object that already owns resources of a
        // different size: old resources released, new ones adopted, and the
        // moved-from object again destructs before the moved-to is used.
        {
            ComplexFFT moved(96);
            {
                ComplexFFT src(n);
                moved = std::move(src);
                CHECK(src.size() == 0);
            }  // moved-from destructs before any use
            CHECK(moved.size() == n);
            std::vector<std::vector<float>> clobber;
            for (int i = 0; i < 8; ++i) {
                clobber.emplace_back(2 * n, -777.0f);  // reuse freed blocks
            }
            std::vector<std::complex<float>> got(n);
            moved.forward(x.data(), got.data());
            for (std::size_t k = 0; k < n; ++k) {
                CHECK_NEAR(got[k].real(), ref[k].real(), tol);
                CHECK_NEAR(got[k].imag(), ref[k].imag(), tol);
            }
        }  // moved destructs: a shallow move-assign double-frees here
    }

    return testSummary("test_fft");
}
