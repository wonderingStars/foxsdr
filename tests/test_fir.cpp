// Tests for dsp/fir.hpp: windowed-sinc design + streaming FirDecimator.
// SPDX-License-Identifier: MIT
#include "dsp/fir.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "test_check.hpp"

using cascade::dsp::FirDecimator;
using cascade::dsp::windowedSincLowpass;
using cascade::dsp::WindowType;

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// Direct evaluation of |H(f)| = |sum h[n] exp(-j 2 pi f n)| — the definition,
// independent of the implementation under test.
double magResponse(const std::vector<float>& h, double f) {
    double re = 0.0;
    double im = 0.0;
    for (std::size_t n = 0; n < h.size(); ++n) {
        const double phi = kTwoPi * f * static_cast<double>(n);
        re += static_cast<double>(h[n]) * std::cos(phi);
        im -= static_cast<double>(h[n]) * std::sin(phi);
    }
    return std::sqrt(re * re + im * im);
}

double magResponseDb(const std::vector<float>& h, double f) {
    return 20.0 * std::log10(magResponse(h, f));
}

// Naive offline reference: causal convolution y[k] = sum_j h[j] x[k-j] with
// x[n<0] = 0, then keep every M-th output starting at k = 0.
std::vector<std::complex<float>> offlineDecimate(
    const std::vector<float>& h, const std::vector<std::complex<float>>& x,
    std::size_t M) {
    std::vector<std::complex<float>> y;
    for (std::size_t k = 0; k < x.size(); k += M) {
        double re = 0.0;
        double im = 0.0;
        for (std::size_t j = 0; j < h.size() && j <= k; ++j) {
            const std::complex<float>& s = x[k - j];
            const double hj = static_cast<double>(h[j]);
            re += hj * static_cast<double>(s.real());
            im += hj * static_cast<double>(s.imag());
        }
        y.emplace_back(static_cast<float>(re), static_cast<float>(im));
    }
    return y;
}

// Fixed-seed LCG (Numerical Recipes constants) — deterministic test input.
struct Lcg {
    std::uint32_t state;
    explicit Lcg(std::uint32_t seed) : state(seed) {}
    float next() {
        state = state * 1664525u + 1013904223u;
        // Top 24 bits -> [-1, 1).
        return (static_cast<float>(state >> 8) / 8388608.0f) - 1.0f;
    }
};

std::vector<std::complex<float>> lcgSignal(std::size_t n, std::uint32_t seed) {
    Lcg rng(seed);
    std::vector<std::complex<float>> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float re = rng.next();
        const float im = rng.next();
        x[i] = {re, im};
    }
    return x;
}

// Runs the streaming decimator over `x` in blocks of `blockSize` and returns
// all outputs.
std::vector<std::complex<float>> runStreaming(const std::vector<float>& taps,
                                              std::size_t M,
                                              const std::vector<std::complex<float>>& x,
                                              std::size_t blockSize) {
    FirDecimator dec(taps, M);
    std::vector<std::complex<float>> y;
    std::vector<std::complex<float>> outBuf;
    for (std::size_t pos = 0; pos < x.size(); pos += blockSize) {
        const std::size_t n = std::min(blockSize, x.size() - pos);
        outBuf.resize(dec.outputCapacity(n));
        const std::size_t got = dec.process(x.data() + pos, n, outBuf.data());
        CHECK(got <= dec.outputCapacity(n));
        y.insert(y.end(), outBuf.begin(), outBuf.begin() + static_cast<std::ptrdiff_t>(got));
    }
    return y;
}

void checkSameSignal(const std::vector<std::complex<float>>& a,
                     const std::vector<std::complex<float>>& b, double tol) {
    CHECK(a.size() == b.size());
    if (a.size() != b.size()) { return; }
    double maxErr = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        maxErr = std::max(maxErr,
                          static_cast<double>(std::abs(a[i] - b[i])));
    }
    CHECK(maxErr <= tol);
    if (maxErr > tol) {
        std::printf("  max sample error %.3g exceeds tol %.3g\n", maxErr, tol);
    }
}

}  // namespace

int main() {
    // --- windowedSincLowpass: unity DC gain and symmetry -------------------
    struct DesignCase {
        std::size_t taps;
        double fc;
        WindowType win;
    };
    const DesignCase designs[] = {
        {101, 0.10, WindowType::BlackmanHarris},
        {63, 0.15, WindowType::Hamming},
        {31, 0.10, WindowType::Hann},
        {11, 0.20, WindowType::Rectangular},
    };
    for (const auto& d : designs) {
        const auto h = windowedSincLowpass(d.taps, d.fc, d.win);
        CHECK(h.size() == d.taps);
        double sum = 0.0;
        for (const float t : h) { sum += static_cast<double>(t); }
        CHECK_NEAR(sum, 1.0, 1e-6);
        // Linear phase requires an exactly symmetric impulse response.
        for (std::size_t i = 0; i < h.size() / 2; ++i) {
            CHECK_NEAR(h[i], h[h.size() - 1 - i], 1e-6);
        }
        // |H(0)| is the tap sum by definition; check via the DFT path too so
        // the two references agree with each other.
        CHECK_NEAR(magResponse(h, 0.0), 1.0, 1e-6);
    }

    // --- Frequency response, evaluated directly as H(f) --------------------
    // Thresholds below were verified numerically against an independent
    // double-precision model before being asserted (measured values in
    // comments); each assert keeps a comfortable margin.
    {
        const auto h = windowedSincLowpass(101, 0.10, WindowType::BlackmanHarris);
        // Passband: measured deviation <= 6e-6 at these frequencies.
        CHECK_NEAR(magResponse(h, 0.01), 1.0, 1e-4);
        CHECK_NEAR(magResponse(h, 0.02), 1.0, 1e-4);
        CHECK_NEAR(magResponse(h, 0.05), 1.0, 1e-4);
        // Cutoff is the -6 dB point of a windowed-sinc design.
        CHECK_NEAR(magResponseDb(h, 0.10), -6.0, 0.3);
        // Stopband: measured -112.5 dB at 0.18, -116.6 at 0.20, -127.9 at 0.25.
        CHECK(magResponseDb(h, 0.18) < -90.0);
        CHECK(magResponseDb(h, 0.20) < -90.0);
        CHECK(magResponseDb(h, 0.25) < -90.0);
    }
    {
        const auto h = windowedSincLowpass(31, 0.10, WindowType::Hann);
        // Measured: +0.0063 at 0.02, -52.1 dB at 0.18, -78.9 dB at 0.25.
        CHECK_NEAR(magResponse(h, 0.02), 1.0, 0.02);
        CHECK(magResponseDb(h, 0.18) < -45.0);
        CHECK(magResponseDb(h, 0.25) < -60.0);
    }
    {
        const auto h = windowedSincLowpass(63, 0.15, WindowType::Hamming);
        // Measured: -7e-4 at 0.05, +1e-4 at 0.10, -60.1 dB at 0.25.
        CHECK_NEAR(magResponse(h, 0.05), 1.0, 5e-3);
        CHECK_NEAR(magResponse(h, 0.10), 1.0, 5e-3);
        CHECK(magResponseDb(h, 0.25) < -55.0);
    }

    // --- FirDecimator vs naive offline reference ---------------------------
    const auto taps = windowedSincLowpass(31, 0.10, WindowType::Hann);
    const std::size_t M = 4;
    const auto x = lcgSignal(512, 0xC0FFEEu);
    const auto ref = offlineDecimate(taps, x, M);
    CHECK(ref.size() == (x.size() + M - 1) / M);

    // Block sizes: smaller than M (1, 3), equal to M, not divisible by M
    // (5, 7, 37), a power of two, and the whole signal at once.
    const std::size_t blockSizes[] = {1, 3, 4, 5, 7, 16, 37, 512};
    for (const std::size_t bs : blockSizes) {
        const auto y = runStreaming(taps, M, x, bs);
        checkSameSignal(y, ref, 1e-5);
    }

    // M = 1 degenerates to a plain streaming FIR: outputs = full convolution.
    {
        const auto ref1 = offlineDecimate(taps, x, 1);
        const auto y1 = runStreaming(taps, 1, x, 37);
        checkSameSignal(y1, ref1, 1e-5);
    }

    // --- Two half-length calls == one full call ----------------------------
    {
        const auto xh = lcgSignal(257, 0xBEEF01u);
        FirDecimator whole(taps, M);
        std::vector<std::complex<float>> yWhole(whole.outputCapacity(xh.size()));
        yWhole.resize(whole.process(xh.data(), xh.size(), yWhole.data()));

        FirDecimator halves(taps, M);
        // 257 samples split 128/129: the second call starts mid-grid (128 % 4
        // == 0 puts the boundary exactly on an output sample, the hard case).
        const std::size_t h1 = xh.size() / 2;
        std::vector<std::complex<float>> yHalves;
        std::vector<std::complex<float>> buf(halves.outputCapacity(xh.size()));
        std::size_t got = halves.process(xh.data(), h1, buf.data());
        yHalves.insert(yHalves.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(got));
        got = halves.process(xh.data() + h1, xh.size() - h1, buf.data());
        yHalves.insert(yHalves.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(got));

        checkSameSignal(yHalves, yWhole, 1e-6);
        checkSameSignal(yHalves, offlineDecimate(taps, xh, M), 1e-5);
    }

    // --- reset() restores freshly-constructed behavior ---------------------
    {
        FirDecimator dec(taps, M);
        std::vector<std::complex<float>> buf(dec.outputCapacity(x.size()));
        (void)dec.process(x.data(), x.size(), buf.data());
        dec.reset();
        std::vector<std::complex<float>> y(dec.outputCapacity(x.size()));
        y.resize(dec.process(x.data(), x.size(), y.data()));
        checkSameSignal(y, ref, 1e-5);
    }

    // --- reset() clears the decimation phase, not just the history ---------
    // The block above feeds a prefix whose length is a multiple of M, which
    // leaves the phase counter at 0 by coincidence; a reset() that only zeroes
    // history would still pass it. Here the prefix length 510 % 4 == 2 parks
    // the phase mid-cycle, so post-reset output only matches a fresh decimator
    // if reset() really rewinds the output grid to sample 0.
    {
        FirDecimator dec(taps, M);
        const std::size_t prefixLen = 510;
        static_assert(510 % 4 != 0, "prefix must park the phase mid-cycle");
        std::vector<std::complex<float>> buf(dec.outputCapacity(x.size()));
        (void)dec.process(x.data(), prefixLen, buf.data());
        dec.reset();
        std::vector<std::complex<float>> y(dec.outputCapacity(x.size()));
        y.resize(dec.process(x.data(), x.size(), y.data()));
        // Fresh behavior: first input sample lands on the output grid, so the
        // very first output is h[0]*x[0] (zero history), exactly ref[0].
        CHECK(!y.empty());
        if (!y.empty()) {
            CHECK_NEAR(y[0].real(), ref[0].real(), 1e-5);
            CHECK_NEAR(y[0].imag(), ref[0].imag(), 1e-5);
        }
        checkSameSignal(y, ref, 1e-5);
    }

    return testSummary("test_fir");
}
