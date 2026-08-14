// Tests for dsp/quad_demod.hpp and dsp/agc.hpp.
//
// DSP claims are proven against in-test references: expected discriminator
// outputs come from the analytic phase increment of signals synthesized here,
// and the FM spectrum claim is checked with a direct DFT — never against
// constants read back from the implementation.
//
// SPDX-License-Identifier: MIT
#include "dsp/agc.hpp"
#include "dsp/quad_demod.hpp"

#include "test_check.hpp"

#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// Complex exponential at normalized frequency f (cycles/sample).
std::vector<std::complex<float>> makeTone(double f, std::size_t n) {
    std::vector<std::complex<float>> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = kTwoPi * f * static_cast<double>(i);
        x[i] = {static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph))};
    }
    return x;
}

// Direct-DFT magnitude at bin k — the independent spectral reference.
double dftMag(const std::vector<float>& x, std::size_t k) {
    double re = 0.0;
    double im = 0.0;
    const double N = static_cast<double>(x.size());
    for (std::size_t n = 0; n < x.size(); ++n) {
        const double ph = kTwoPi * static_cast<double>(k) * static_cast<double>(n) / N;
        re += static_cast<double>(x[n]) * std::cos(ph);
        im -= static_cast<double>(x[n]) * std::sin(ph);
    }
    return std::sqrt(re * re + im * im);
}

// Synthesizes single-tone FM (unit amplitude), demodulates it, and checks the
// demod output spectrum against the modulator via direct DFT: the dominant bin
// must be the modulator frequency and the recovered amplitude must equal the
// peak phase increment 2*pi*fdev within 5%.
void checkFmRoundTrip(double fm, double fdev, std::size_t N) {
    // Phase increment at sample n is 2*pi*fdev*sin(2*pi*fm*n), so the ideal
    // discriminator output IS the modulator, scaled by 2*pi*fdev (gain = 1).
    std::vector<std::complex<float>> x(N + 1);
    double phi = 0.0;
    x[0] = {1.0f, 0.0f};
    for (std::size_t n = 1; n <= N; ++n) {
        phi += kTwoPi * fdev * std::sin(kTwoPi * fm * static_cast<double>(n));
        x[n] = {static_cast<float>(std::cos(phi)), static_cast<float>(std::sin(phi))};
    }

    cascade::dsp::QuadDemod demod(1.0f);
    std::vector<float> y(N + 1);
    demod.process(x.data(), y.data(), x.size());

    // Drop the startup sample (absolute phase of x[0]); the remaining N
    // samples hold an integer number of modulator periods, so the DFT is
    // coherent and the tone lands in exactly one bin.
    std::vector<float> z(y.begin() + 1, y.end());

    const std::size_t kExpected = static_cast<std::size_t>(std::lround(fm * static_cast<double>(N)));
    std::size_t kDominant = 1;
    double magMax = 0.0;
    for (std::size_t k = 1; k <= N / 2; ++k) {
        const double m = dftMag(z, k);
        if (m > magMax) {
            magMax = m;
            kDominant = k;
        }
    }
    CHECK(kDominant == kExpected);

    // Real sinusoid of amplitude A, coherently sampled: |X[k]| = A*N/2.
    const double amp = 2.0 * magMax / static_cast<double>(N);
    const double expected = kTwoPi * fdev;
    CHECK_NEAR(amp, expected, 0.05 * expected);
}

}  // namespace

int main() {
    // --- QuadDemod: pure tone -> constant 2*pi*f*gain in steady state -------
    {
        const double f = 0.05;
        const float gain = 2.5f;
        const double expected = kTwoPi * f * static_cast<double>(gain);
        auto x = makeTone(f, 256);

        cascade::dsp::QuadDemod demod(gain);
        std::vector<float> y(x.size());
        demod.process(x.data(), y.data(), x.size());

        // Sample 0 is the absolute phase of x[0] = 1+0j, i.e. zero.
        CHECK_NEAR(y[0], 0.0, 1e-6);
        for (std::size_t i = 1; i < y.size(); ++i) { CHECK_NEAR(y[i], expected, 1e-4); }

        // Negative frequency demodulates to the negated constant.
        auto xn = makeTone(-0.07, 64);
        cascade::dsp::QuadDemod demodNeg(gain);
        std::vector<float> yn(xn.size());
        demodNeg.process(xn.data(), yn.data(), xn.size());
        for (std::size_t i = 1; i < yn.size(); ++i) {
            CHECK_NEAR(yn[i], -kTwoPi * 0.07 * static_cast<double>(gain), 1e-4);
        }

        // --- Continuity across a block split ----------------------------
        // Split at 100: 100*f = 5.0 cycles, so a demod that forgot the carried
        // sample would emit atan2 of the ABSOLUTE phase (0 here, not 2*pi*f)
        // at the seam. The split index is chosen so absolute and differential
        // phase disagree there; the sample-for-sample comparison against the
        // unsplit run then pins the whole stream, not just the seam.
        demod.reset();
        std::vector<float> ySplit(x.size());
        const std::size_t split = 100;
        demod.process(x.data(), ySplit.data(), split);
        demod.process(x.data() + split, ySplit.data() + split, x.size() - split);
        CHECK_NEAR(ySplit[split], expected, 1e-4);
        for (std::size_t i = 0; i < y.size(); ++i) { CHECK_NEAR(ySplit[i], y[i], 1e-9); }
    }

    // --- QuadDemod: synthesized FM round trip via direct DFT ----------------
    // Two deviations to show amplitude TRACKS deviation rather than merely
    // matching one lucky constant.
    checkFmRoundTrip(16.0 / 1024.0, 0.05, 1024);
    checkFmRoundTrip(16.0 / 1024.0, 0.08, 1024);

    // --- AGC: small constant input pulled up to target, no overshoot --------
    {
        cascade::dsp::Agc agc(1.0f, 0.1f, 0.01f, 100.0f);
        const std::size_t N = 20000;
        std::vector<float> in(N, 0.05f);
        std::vector<float> out(N);
        agc.process(in.data(), out.data(), N);

        float outMax = 0.0f;
        for (float v : out) { outMax = (v > outMax) ? v : outMax; }
        // First-order loop approaching from below: never exceeds target.
        CHECK(outMax <= 1.0f + 1e-3f);
        // Loop time constant is 1/(decay*|x|) = 2000 samples; by 12000 (6 tau)
        // the output must be within 2% of target, and fully settled by the end.
        CHECK_NEAR(out[12000], 1.0, 0.02);
        CHECK_NEAR(out[N - 1], 1.0, 0.005);
        // Steady-state gain is target/|x|, reached without touching the clamp.
        CHECK_NEAR(agc.gain(), 20.0, 0.1);
    }

    // --- AGC: loud constant input pulled down via the attack rate -----------
    {
        cascade::dsp::Agc agc(1.0f, 0.1f, 0.01f, 100.0f);
        const std::size_t N = 200;
        std::vector<float> in(N, 8.0f);
        std::vector<float> out(N);
        agc.process(in.data(), out.data(), N);
        // Per-sample factor |1 - attack*|x|| = 0.2: settled long before N.
        CHECK_NEAR(out[N - 1], 1.0, 0.02);
        CHECK_NEAR(agc.gain(), 1.0 / 8.0, 0.01);
        // Gain must never swing negative and invert the signal.
        for (float v : out) { CHECK(v >= 0.0f); }
    }

    // --- AGC: complex path levels magnitude and preserves phase -------------
    {
        cascade::dsp::Agc agc(1.0f, 0.1f, 0.01f, 100.0f);
        const std::size_t N = 20000;
        auto tone = makeTone(0.01, N);
        std::vector<std::complex<float>> in(N);
        for (std::size_t i = 0; i < N; ++i) { in[i] = 0.05f * tone[i]; }
        std::vector<std::complex<float>> out(N);
        agc.process(in.data(), out.data(), N);

        CHECK_NEAR(std::abs(out[N - 1]), 1.0, 0.005);
        // Real positive gain: out * conj(in) must be purely real positive.
        for (std::size_t i = N - 5; i < N; ++i) {
            const std::complex<float> r = out[i] * std::conj(in[i]);
            CHECK_NEAR(r.imag(), 0.0, 1e-6);
            CHECK(r.real() > 0.0f);
        }
    }

    // --- AGC: all-zero input stays finite and the gain hits the clamp -------
    {
        const float maxGain = 50.0f;
        cascade::dsp::Agc agc(1.0f, 0.1f, 0.01f, maxGain);
        // Gain ramps by decay*target = 0.01/sample from 1.0: the clamp is
        // reached by sample 4900; run past it to prove it stays pinned.
        const std::size_t N = 8000;
        std::vector<float> in(N, 0.0f);
        std::vector<float> out(N);
        agc.process(in.data(), out.data(), N);

        for (float v : out) { CHECK(v == 0.0f); }
        CHECK(std::isfinite(agc.gain()));
        CHECK(agc.gain() <= maxGain);
        CHECK_NEAR(agc.gain(), maxGain, 1e-4);
    }

    return testSummary("test_demod_agc");
}
