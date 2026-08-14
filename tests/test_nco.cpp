// Tests for dsp/nco.hpp / nco.cpp.
// SPDX-License-Identifier: MIT
#include "dsp/nco.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "test_check.hpp"

using cascade::dsp::Nco;

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// Direct-DFT reference (O(n^2), double precision) — the in-test ground truth
// the NCO output is proven against.
std::vector<std::complex<double>> dft(const std::vector<std::complex<float>>& x) {
    const std::size_t n = x.size();
    std::vector<std::complex<double>> spec(n);
    for (std::size_t k = 0; k < n; ++k) {
        std::complex<double> acc(0.0, 0.0);
        for (std::size_t m = 0; m < n; ++m) {
            const double a = -kTwoPi * static_cast<double>(k) *
                             static_cast<double>(m) / static_cast<double>(n);
            acc += std::complex<double>(x[m].real(), x[m].imag()) *
                   std::complex<double>(std::cos(a), std::sin(a));
        }
        spec[k] = acc;
    }
    return spec;
}

std::size_t peakBin(const std::vector<std::complex<double>>& spec) {
    std::size_t best = 0;
    double bestMag = -1.0;
    for (std::size_t k = 0; k < spec.size(); ++k) {
        const double m = std::abs(spec[k]);
        if (m > bestMag) {
            bestMag = m;
            best = k;
        }
    }
    return best;
}

}  // namespace

int main() {
    // --- exact-bin tone: DFT peak at the bin, near-zero energy elsewhere ---
    {
        const std::size_t n = 64;
        Nco nco;
        nco.setFrequency(5.0 / 64.0);
        std::vector<std::complex<float>> x(n);
        nco.generate(x.data(), n);
        auto spec = dft(x);
        CHECK(peakBin(spec) == 5u);
        CHECK_NEAR(std::abs(spec[5]), 64.0, 1e-3);
        for (std::size_t k = 0; k < n; ++k) {
            if (k != 5) { CHECK(std::abs(spec[k]) < 1e-3); }
        }
    }

    // --- negative frequency lands in the mirrored bin ----------------------
    {
        const std::size_t n = 64;
        Nco nco;
        nco.setFrequency(-7.0 / 64.0);
        std::vector<std::complex<float>> x(n);
        nco.generate(x.data(), n);
        auto spec = dft(x);
        CHECK(peakBin(spec) == 57u);  // -7/64 == bin 64-7
        CHECK_NEAR(std::abs(spec[57]), 64.0, 1e-3);
    }

    // --- out-of-range frequency folds (0.75 aliases to -0.25) --------------
    {
        const std::size_t n = 64;
        Nco nco;
        nco.setFrequency(0.75);
        CHECK_NEAR(nco.frequency(), -0.25, 1e-15);
        std::vector<std::complex<float>> x(n);
        nco.generate(x.data(), n);
        auto spec = dft(x);
        CHECK(peakBin(spec) == 48u);  // -0.25 == bin 64*3/4
    }

    // --- unit magnitude on every sample, non-dyadic frequency --------------
    {
        Nco nco;
        nco.setFrequency(0.123456789);
        std::vector<std::complex<float>> x(1000);
        nco.generate(x.data(), x.size());
        double worst = 0.0;
        for (const auto& s : x) {
            const double mag =
                std::abs(std::complex<double>(s.real(), s.imag()));
            worst = std::max(worst, std::fabs(mag - 1.0));
        }
        CHECK(worst <= 1e-6);
    }

    // --- continuity: two half-blocks match one full block exactly ----------
    {
        Nco full;
        Nco halves;
        full.setFrequency(0.0371);
        halves.setFrequency(0.0371);
        std::vector<std::complex<float>> a(256);
        std::vector<std::complex<float>> b(256);
        full.generate(a.data(), 256);
        halves.generate(b.data(), 128);
        halves.generate(b.data() + 128, 128);
        for (std::size_t i = 0; i < 256; ++i) {
            CHECK_NEAR(a[i].real(), b[i].real(), 0.0);
            CHECK_NEAR(a[i].imag(), b[i].imag(), 0.0);
        }
    }

    // --- mixing translates a tone: f1 mixed by -f0 peaks at f1-f0 ----------
    {
        const std::size_t n = 64;
        const double f1 = 10.0 / 64.0;
        const double f0 = 3.0 / 64.0;
        // Input tone built directly in-test, independent of the class under test.
        std::vector<std::complex<float>> x(n);
        for (std::size_t m = 0; m < n; ++m) {
            const double a = kTwoPi * f1 * static_cast<double>(m);
            x[m] = std::complex<float>(static_cast<float>(std::cos(a)),
                                       static_cast<float>(std::sin(a)));
        }
        Nco lo;
        lo.setFrequency(-f0);
        std::vector<std::complex<float>> y(n);
        lo.mix(x.data(), y.data(), n);
        auto spec = dft(y);
        CHECK(peakBin(spec) == 7u);  // (10 - 3)/64
        CHECK_NEAR(std::abs(spec[7]), 64.0, 1e-3);
        for (std::size_t k = 0; k < n; ++k) {
            if (k != 7) { CHECK(std::abs(spec[k]) < 1e-3); }
        }

        // In-place mixing (in == out) must give the identical result.
        Nco lo2;
        lo2.setFrequency(-f0);
        std::vector<std::complex<float>> z = x;
        lo2.mix(z.data(), z.data(), n);
        for (std::size_t k = 0; k < n; ++k) {
            CHECK_NEAR(z[k].real(), y[k].real(), 0.0);
            CHECK_NEAR(z[k].imag(), y[k].imag(), 0.0);
        }
    }

    // --- long run: no magnitude loss, no phase drift over 1e6 samples ------
    {
        // f is dyadic (12345/2^16), so the cycle-domain accumulator is exact
        // and the closed-form comparison below has no tolerance excuse: a
        // per-block phase reset or radian-domain drift fails immediately.
        const double f = 12345.0 / 65536.0;
        const std::size_t total = 1000000;
        Nco nco;
        nco.setFrequency(f);
        std::vector<std::complex<float>> block(4096);
        std::size_t done = 0;
        std::complex<float> last(0.0f, 0.0f);
        while (done < total) {
            const std::size_t n = std::min(block.size(), total - done);
            nco.generate(block.data(), n);
            done += n;
            last = block[n - 1];
        }
        // Magnitude intact after 1,000,000 samples.
        const double lastMag =
            std::abs(std::complex<double>(last.real(), last.imag()));
        CHECK_NEAR(lastMag, 1.0, 1e-6);
        // Final sample matches closed form e^{j*2*pi*f*(total-1)}.
        const double aLast =
            kTwoPi * std::fmod(f * static_cast<double>(total - 1), 1.0);
        CHECK_NEAR(last.real(), std::cos(aLast), 1e-6);
        CHECK_NEAR(last.imag(), std::sin(aLast), 1e-6);
        // Accumulated phase (of the next sample) matches closed form.
        double cyc = std::fmod(f * static_cast<double>(total), 1.0);
        if (cyc >= 0.5) {
            cyc -= 1.0;
        } else if (cyc < -0.5) {
            cyc += 1.0;
        }
        CHECK_NEAR(nco.phase(), kTwoPi * cyc, 1e-9);
    }

    // --- degenerate sizes must not crash ------------------------------------
    {
        Nco nco;
        nco.setFrequency(0.1);
        nco.generate(nullptr, 0);
        nco.mix(nullptr, nullptr, 0);
        CHECK_NEAR(nco.phase(), 0.0, 0.0);  // n==0 must not advance the phase
    }

    return testSummary("test_nco");
}
