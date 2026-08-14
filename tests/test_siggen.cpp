// Tests for source/siggen.hpp / siggen.cpp.
// SPDX-License-Identifier: MIT
#include "source/siggen.hpp"

#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "test_check.hpp"

using cascade::source::SigGen;

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// Direct-DFT reference (O(n^2), double precision) — the in-test ground truth
// the generator output is proven against, per the project testing protocol.
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

// Tone level in dB full scale from an exact-bin DFT peak: a tone of linear
// amplitude a contributes |X[k]| = a*N at its bin.
double binLevelDb(const std::vector<std::complex<double>>& spec, std::size_t k) {
    return 20.0 *
           std::log10(std::abs(spec[k]) / static_cast<double>(spec.size()));
}

double meanPower(const std::vector<std::complex<float>>& x) {
    double acc = 0.0;
    for (const auto& s : x) {
        acc += static_cast<double>(s.real()) * static_cast<double>(s.real()) +
               static_cast<double>(s.imag()) * static_cast<double>(s.imag());
    }
    return acc / static_cast<double>(x.size());
}

}  // namespace

int main() {
    const double fs = 1.0e6;
    const std::size_t n = 1024;
    const double binHz = fs / static_cast<double>(n);  // exact-bin spacing

    // --- single tone at an exact bin: DFT peak there, level within 0.2 dB ---
    {
        SigGen gen(fs);
        gen.setTone(0, 100.0 * binHz, -6.0f);
        std::vector<std::complex<float>> x(n);
        gen.generate(x.data(), n);
        auto spec = dft(x);
        CHECK(peakBin(spec) == 100u);
        CHECK_NEAR(binLevelDb(spec, 100), -6.0, 0.2);
        // Exact-bin tone: every other bin holds only float-rounding leakage.
        const double amp = std::pow(10.0, -6.0 / 20.0);
        for (std::size_t k = 0; k < n; ++k) {
            if (k != 100) {
                CHECK(std::abs(spec[k]) < amp * static_cast<double>(n) * 1e-3);
            }
        }
    }

    // --- 0 dB means amplitude 1.0 exactly ----------------------------------
    {
        SigGen gen(fs);
        gen.setTone(1, 25.0 * binHz, 0.0f);
        std::vector<std::complex<float>> x(n);
        gen.generate(x.data(), n);
        auto spec = dft(x);
        CHECK(peakBin(spec) == 25u);
        CHECK_NEAR(binLevelDb(spec, 25), 0.0, 0.2);
    }

    // --- two tones: two peaks, absolute and relative levels correct --------
    {
        SigGen gen(fs);
        gen.setTone(0, 50.0 * binHz, -3.0f);
        gen.setTone(1, 200.0 * binHz, -23.0f);
        std::vector<std::complex<float>> x(n);
        gen.generate(x.data(), n);
        auto spec = dft(x);
        CHECK(peakBin(spec) == 50u);  // the louder of the two wins overall
        CHECK_NEAR(binLevelDb(spec, 50), -3.0, 0.2);
        CHECK_NEAR(binLevelDb(spec, 200), -23.0, 0.2);
        // Relative level is the difference of the two configured amplitudes.
        CHECK_NEAR(binLevelDb(spec, 50) - binLevelDb(spec, 200), 20.0, 0.3);
        // Everything that is not one of the two tone bins is only leakage,
        // far below even the weaker tone.
        const double weakAmp = std::pow(10.0, -23.0 / 20.0);
        for (std::size_t k = 0; k < n; ++k) {
            if (k != 50 && k != 200) {
                CHECK(std::abs(spec[k]) <
                      weakAmp * static_cast<double>(n) * 1e-2);
            }
        }
    }

    // --- negative freqHz lands in the negative-frequency DFT half ----------
    {
        SigGen gen(fs);
        gen.setTone(0, -100.0 * binHz, 0.0f);
        std::vector<std::complex<float>> x(n);
        gen.generate(x.data(), n);
        auto spec = dft(x);
        const std::size_t peak = peakBin(spec);
        CHECK(peak == n - 100u);  // -f maps to bin N-k in an unshifted DFT
        CHECK(peak > n / 2u);     // i.e. the negative-frequency half
        CHECK_NEAR(binLevelDb(spec, n - 100u), 0.0, 0.2);
    }

    // --- phase continuity: split generates match one full generate ---------
    {
        // Noise is ON here so the check also proves the LCG stream carries
        // across calls, and the tone frequencies are non-bin values so phase
        // wraps mid-block; equality must be bit-exact (tolerance 0.0).
        SigGen full(fs);
        SigGen split(fs);
        for (SigGen* g : {&full, &split}) {
            g->setTone(0, 12345.678, -6.0f);
            g->setTone(3, -98765.4, -12.0f);
            g->setNoiseFloorDb(-40.0f);
        }
        const std::size_t total = 512;
        std::vector<std::complex<float>> a(total);
        std::vector<std::complex<float>> b(total);
        full.generate(a.data(), total);
        // Deliberately uneven split (not a multiple of any internal chunk
        // size) to catch phase or RNG restarts at call boundaries.
        split.generate(b.data(), 100);
        split.generate(b.data() + 100, 412);
        for (std::size_t i = 0; i < total; ++i) {
            CHECK_NEAR(a[i].real(), b[i].real(), 0.0);
            CHECK_NEAR(a[i].imag(), b[i].imag(), 0.0);
        }
    }

    // --- clearTone silences exactly that slot ------------------------------
    {
        SigGen gen(fs);
        gen.setTone(2, 30.0 * binHz, 0.0f);
        std::vector<std::complex<float>> x(n);
        gen.generate(x.data(), n);
        auto spec = dft(x);
        CHECK(peakBin(spec) == 30u);  // slot was audibly present first
        gen.clearTone(2);
        gen.generate(x.data(), n);
        // No tones, no noise: the output must be exactly silent.
        CHECK(meanPower(x) == 0.0);
    }

    // --- clearTone leaves other slots running ------------------------------
    {
        SigGen gen(fs);
        gen.setTone(0, 40.0 * binHz, 0.0f);
        gen.setTone(1, 90.0 * binHz, -6.0f);
        gen.clearTone(0);
        std::vector<std::complex<float>> x(n);
        gen.generate(x.data(), n);
        auto spec = dft(x);
        CHECK(peakBin(spec) == 90u);
        CHECK_NEAR(binLevelDb(spec, 90), -6.0, 0.2);
        CHECK(std::abs(spec[40]) < 1e-2);  // the cleared slot is gone
    }

    // --- noise disabled: residual below -280 dB equivalent -----------------
    {
        // Default state is disabled; also exercise the explicit <= -300 path
        // after noise had been on, to prove disabling really zeroes it.
        SigGen gen(fs);
        std::vector<std::complex<float>> x(n);
        gen.generate(x.data(), n);
        auto spec = dft(x);
        for (std::size_t k = 0; k < n; ++k) {
            const double p = std::abs(spec[k]) / static_cast<double>(n);
            CHECK(p * p < 1e-28);  // power below -280 dB
        }
        gen.setNoiseFloorDb(-40.0f);
        gen.generate(x.data(), n);
        CHECK(meanPower(x) > 0.0);  // it was audibly on...
        gen.setNoiseFloorDb(-300.0f);
        gen.generate(x.data(), n);
        CHECK(meanPower(x) == 0.0);  // ...and -300 turned it hard off
    }

    // --- noise at -60 dB: measured mean power within 1.5 dB ----------------
    {
        SigGen gen(fs);
        gen.setNoiseFloorDb(-60.0f);
        // 65536 samples put the sample-mean estimator's own sigma near
        // 0.02 dB, so a 1.5 dB tolerance only fails on a real scaling bug.
        std::vector<std::complex<float>> x(65536);
        gen.generate(x.data(), x.size());
        const double db = 10.0 * std::log10(meanPower(x));
        CHECK_NEAR(db, -60.0, 1.5);
        // Zero-mean sanity: a DC bias would raise the measured power and
        // indicates the uniform-sum offset is wrong.
        double meanRe = 0.0;
        double meanIm = 0.0;
        for (const auto& s : x) {
            meanRe += static_cast<double>(s.real());
            meanIm += static_cast<double>(s.imag());
        }
        meanRe /= static_cast<double>(x.size());
        meanIm /= static_cast<double>(x.size());
        // sigma_mean = sigma/sqrt(N) ~ 2.8e-6; allow ~5 sigma.
        CHECK(std::fabs(meanRe) < 1.5e-5);
        CHECK(std::fabs(meanIm) < 1.5e-5);
    }

    // --- degenerate calls must not crash or corrupt state ------------------
    {
        SigGen gen(fs);
        gen.setTone(-1, 1000.0, 0.0f);   // out-of-range slots are ignored
        gen.setTone(4, 1000.0, 0.0f);
        gen.clearTone(-1);
        gen.clearTone(4);
        gen.generate(nullptr, 0);
        std::vector<std::complex<float>> x(8);
        gen.generate(x.data(), x.size());
        CHECK(meanPower(x) == 0.0);  // the ignored setTone calls added nothing
    }

    return testSummary("test_siggen");
}
