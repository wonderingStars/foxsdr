// Tests for dsp/quad_demod.hpp and dsp/agc.hpp.
//
// DSP claims are proven against in-test references: expected discriminator
// outputs come from the analytic phase increment of signals synthesized here,
// and the FM spectrum claim is checked with a direct DFT — never against
// constants read back from the implementation.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/agc.hpp"
#include "dsp/quad_demod.hpp"

#include "test_check.hpp"

#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
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

    // --- QuadDemod: amplitude-insensitive (polar discriminator) -------------
    // atan2 of x[n]*conj(x[n-1]) sees only phase: a common real positive
    // magnitude scales both atan2 arguments equally. A tone scaled to 0.3 and
    // the same tone under a varying (always-positive) AM envelope must both
    // demodulate to the same constant 2*pi*f*gain.
    {
        const double f = 0.05;
        const float gain = 2.5f;
        const double expected = kTwoPi * f * static_cast<double>(gain);
        const std::size_t N = 256;
        auto tone = makeTone(f, N);

        std::vector<std::complex<float>> scaled(N);
        std::vector<std::complex<float>> am(N);
        for (std::size_t i = 0; i < N; ++i) {
            scaled[i] = 0.3f * tone[i];
            // Envelope swings 0.1..0.9 — strongly amplitude-modulated but
            // never zero, so the phase of every sample stays well defined.
            const double env = 0.5 + 0.4 * std::sin(kTwoPi * 0.003 * static_cast<double>(i));
            am[i] = static_cast<float>(env) * tone[i];
        }

        cascade::dsp::QuadDemod demodScaled(gain);
        cascade::dsp::QuadDemod demodAm(gain);
        std::vector<float> yScaled(N);
        std::vector<float> yAm(N);
        demodScaled.process(scaled.data(), yScaled.data(), N);
        demodAm.process(am.data(), yAm.data(), N);

        // Sample 0 is the absolute phase of a positive-real sample (zero)
        // in both runs; every later sample must be the analytic constant,
        // and the two runs must agree with each other.
        for (std::size_t i = 1; i < N; ++i) {
            CHECK_NEAR(yScaled[i], expected, 1e-4);
            CHECK_NEAR(yAm[i], expected, 1e-4);
            CHECK_NEAR(yScaled[i], yAm[i], 1e-4);
        }
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
        // Decay SPEED, not just the settled endpoint. Below target the loop
        // must use the SLOW rate: per-sample factor (1 - decay*|x|) = 0.9995,
        // so analytically out[i] = target + (|x|*g0 - target) * 0.9995^i and
        // out[500] = 1 - 0.95*0.9995^500 ~= 0.260 — still far below target.
        // The fast attack rate applied on this side (factor 1 - 0.1*0.05 =
        // 0.995) would already be at 1 - 0.95*0.995^500 ~= 0.922, so a
        // mid-early sample well below target pins WHICH rate decays.
        const double decayFactor = 1.0 - 0.01 * 0.05;
        const double expected500 = 1.0 + (0.05 - 1.0) * std::pow(decayFactor, 500.0);
        CHECK_NEAR(out[500], expected500, 0.02);
        CHECK(out[500] < 0.5f);
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
        // Attack SPEED, not just the settled endpoint. Above target the loop
        // must use the FAST rate: per-sample factor (1 - attack*|x|) = 0.2,
        // so analytically out[i] = target + (|x|*g0 - target) * 0.2^i and
        // out[3] = 1 + 7*0.2^3 = 1.056 — within 6% of target after only
        // three samples. The slow decay rate applied on this side (factor
        // 1 - 0.01*8 = 0.92) would leave out[3] at 1 + 7*0.92^3 ~= 6.45,
        // so an early sample near target pins WHICH rate attacks.
        const double attackFactor = 1.0 - 0.1 * 8.0;
        const double expected3 = 1.0 + (8.0 - 1.0) * std::pow(attackFactor, 3.0);
        CHECK_NEAR(out[3], expected3, 1e-3);
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

    // --- AGC: a non-finite input burst must not latch the gain -------------
    // One NaN sample makes the level NaN, and every clamp comparison in the
    // update is false for NaN, so an unguarded loop keeps gain_ = NaN for the
    // rest of the session and feeds NaN to the sound card. The gain must be
    // finite the moment the burst ends and must re-converge on clean input to
    // the same place a loop that never saw the burst reaches — the reference
    // here is a second Agc fed only the clean tone.
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();

        cascade::dsp::Agc poisoned(1.0f, 0.1f, 0.01f, 100.0f);
        std::vector<float> burst(256);
        for (std::size_t i = 0; i < burst.size(); ++i) {
            burst[i] = (i % 3 == 0) ? nan : ((i % 3 == 1) ? inf : -inf);
        }
        std::vector<float> burstOut(burst.size());
        poisoned.process(burst.data(), burstOut.data(), burst.size());
        // The decisive assertion: the burst is over, the state must not be.
        CHECK(std::isfinite(poisoned.gain()));

        const std::size_t N = 60000;
        std::vector<float> clean(N);
        for (std::size_t i = 0; i < N; ++i) {
            clean[i] = static_cast<float>(
                0.05 * std::cos(kTwoPi * 0.01 * static_cast<double>(i)));
        }
        std::vector<float> out(N);
        poisoned.process(clean.data(), out.data(), N);

        cascade::dsp::Agc reference(1.0f, 0.1f, 0.01f, 100.0f);
        std::vector<float> refOut(N);
        reference.process(clean.data(), refOut.data(), N);

        CHECK(std::isfinite(reference.gain()));
        CHECK(reference.gain() > 1.0f);  // the reference itself really moved
        CHECK(std::isfinite(poisoned.gain()));
        CHECK(std::fabs(poisoned.gain() - reference.gain()) <=
              0.02f * reference.gain());

        std::size_t nonFinite = 0;
        for (std::size_t i = N / 2; i < N; ++i) {
            if (!std::isfinite(out[i])) { ++nonFinite; }
        }
        CHECK(nonFinite == 0);
    }

    return testSummary("test_demod_agc");
}
