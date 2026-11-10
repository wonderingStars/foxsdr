// Tests for the AGC modes of dsp/agc.hpp (Auto, Fast, Medium, Slow, Off).
//
// The claims, each against an in-test reference and never against a constant
// read back from the implementation:
//   - Auto is the loop the pipeline has always had, SAMPLE FOR SAMPLE: the
//     reference is the original update rule re-written here, compared bit for
//     bit (not within a tolerance) on real and complex input.
//   - Fast, Medium and Slow recover from a 20 dB step in the time they are
//     named for, and that time is the SAME at two channel rates and at two
//     input levels (a constant-slew loop; the old loop's speed depends on the
//     level and on the rate).
//   - Off never moves the gain; the fixed gain is the gain applied.
//   - No mode lets a non-finite sample into the loop's state.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/agc.hpp"

#include "test_check.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using cascade::dsp::Agc;

constexpr float kTarget = 0.5f;
constexpr float kAttack = 0.005f / 2.0f;
constexpr float kDecay = 0.0005f / 2.0f;
constexpr float kMaxGain = 100.0f;

Agc makeAgc(double sampleRateHz) {
    Agc a(kTarget, kAttack, kDecay, kMaxGain);
    a.setSampleRateHz(sampleRateHz);
    return a;
}

// The original loop, verbatim in meaning, as the reference for Auto.
struct RefAgc {
    float gain = 1.0f;
    void step(float level) {
        const float rate = (level > kTarget) ? kAttack : kDecay;
        gain += rate * (kTarget - level);
        if (gain > kMaxGain) { gain = kMaxGain; }
        if (!(gain >= 0.0f)) { gain = 0.0f; }
    }
};

bool sameBits(float a, float b) {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    return x == y;
}

// A deterministic, level-varying real input: a chirp-ish tone under a slow
// envelope that steps a few times, plus a few non-finite samples.
std::vector<float> makeProgram(std::size_t n, bool withNonFinite) {
    std::vector<float> x(n);
    std::uint32_t r = 0x9e3779b9u;
    for (std::size_t i = 0; i < n; ++i) {
        r = r * 1664525u + 1013904223u;
        const double noise = (static_cast<double>(r >> 8) / 16777216.0 - 0.5) * 0.05;
        const double env = (i / 20000) % 4 == 0 ? 0.9 : ((i / 20000) % 4 == 1 ? 0.05 : 0.3);
        x[i] = static_cast<float>(
            env * std::sin(0.013 * static_cast<double>(i) * (1.0 + 1e-6 * static_cast<double>(i))) +
            noise);
    }
    if (withNonFinite) {
        x[1000] = std::numeric_limits<float>::quiet_NaN();
        x[2000] = std::numeric_limits<float>::infinity();
        x[3000] = -std::numeric_limits<float>::infinity();
    }
    return x;
}

// Time in seconds, from the step, for a constant-magnitude complex input to be
// within 3 dB of target at the output again. Starts from a settled loop.
// stepDb < 0: input falls (gain must rise); > 0: input rises (gain must fall).
double recoveryTimeS(Agc& agc, double fs, float baseAmp, float stepDb) {
    const std::complex<float> unit(1.0f, 0.0f);
    std::vector<std::complex<float>> in(4096);
    std::vector<std::complex<float>> out(4096);
    // Settle: hold baseAmp until the output sits on target (the loop needs the
    // time of a full 17 dB move at the slowest, 2 s; 3 s is generous).
    const std::size_t settle = static_cast<std::size_t>(3.0 * fs);
    for (std::size_t done = 0; done < settle; done += in.size()) {
        for (auto& v : in) { v = unit * baseAmp; }
        agc.process(in.data(), out.data(), in.size());
    }
    const float stepped = baseAmp * std::pow(10.0f, stepDb / 20.0f);
    const float lo = kTarget * std::pow(10.0f, -3.0f / 20.0f);
    const float hi = kTarget * std::pow(10.0f, 3.0f / 20.0f);
    std::size_t n = 0;
    const std::size_t limit = static_cast<std::size_t>(10.0 * fs);
    while (n < limit) {
        const std::complex<float> x = unit * stepped;
        std::complex<float> y;
        agc.process(&x, &y, 1);
        ++n;
        const float m = std::abs(y);
        if (m >= lo && m <= hi) { break; }
    }
    return static_cast<double>(n) / fs;
}

void testAutoIsTheOldLoopSampleForSample() {
    const std::vector<float> x = makeProgram(120000, true);
    Agc agc(kTarget, kAttack, kDecay, kMaxGain);
    // Sample-rate configuration must not touch Auto at all.
    agc.setSampleRateHz(300000.0);
    RefAgc ref;
    std::vector<float> out(x.size());
    agc.process(x.data(), out.data(), x.size());
    std::size_t diffs = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float y = x[i] * ref.gain;
        ref.step(std::fabs(y));
        if (!sameBits(y, out[i]) && !(std::isnan(y) && std::isnan(out[i]))) { ++diffs; }
    }
    CHECK(diffs == 0);
    CHECK(sameBits(agc.gain(), ref.gain));
    CHECK(agc.mode() == Agc::Mode::Auto);

    // Complex path, in blocks of an odd size.
    Agc agcC(kTarget, kAttack, kDecay, kMaxGain);
    RefAgc refC;
    std::size_t cdiffs = 0;
    std::vector<std::complex<float>> cin(777);
    std::vector<std::complex<float>> cout_(777);
    for (std::size_t base = 0; base + cin.size() <= 60000; base += cin.size()) {
        for (std::size_t i = 0; i < cin.size(); ++i) {
            cin[i] = {x[base + i + 4000], x[base + i + 4001] * 0.5f};
        }
        agcC.process(cin.data(), cout_.data(), cin.size());
        for (std::size_t i = 0; i < cin.size(); ++i) {
            const std::complex<float> y = cin[i] * refC.gain;
            refC.step(std::abs(y));
            if (!sameBits(y.real(), cout_[i].real()) || !sameBits(y.imag(), cout_[i].imag())) {
                ++cdiffs;
            }
        }
    }
    CHECK(cdiffs == 0);
    CHECK(sameBits(agcC.gain(), refC.gain));

    // reset() in Auto is the neutral gain, as ever.
    agcC.reset();
    CHECK(agcC.gain() == 1.0f);
}

void testTimedModesRecoveryAtTwoRates() {
    struct Row { Agc::Mode mode; double wantS; const char* name; };
    const Row rows[] = {
        {Agc::Mode::Fast, 0.1, "Fast"},
        {Agc::Mode::Medium, 0.5, "Medium"},
        {Agc::Mode::Slow, 2.0, "Slow"},
    };
    // Twice the channel rate: 150 kHz and 250 kHz channels, the ends of the
    // range the pipeline's decimation search produces.
    const double rates[] = {300000.0, 500000.0};
    for (const Row& row : rows) {
        double times[2] = {0.0, 0.0};
        for (int ri = 0; ri < 2; ++ri) {
            Agc agc = makeAgc(rates[ri]);
            agc.setMode(row.mode);
            const double t = recoveryTimeS(agc, rates[ri], 0.5f, -20.0f);
            times[ri] = t;
            std::printf("  %s fs=%.0f 20 dB drop recovered to within 3 dB in %.4f s (want %.3f)\n",
                        row.name, rates[ri], t, row.wantS);
            CHECK(t > 0.85 * row.wantS);
            CHECK(t < 1.15 * row.wantS);
        }
        // Rate independence, tighter than either bound on its own.
        CHECK(std::fabs(times[0] - times[1]) < 0.03 * row.wantS);

        // A 20 dB RISE is pulled back inside 10 ms at both rates.
        for (int ri = 0; ri < 2; ++ri) {
            Agc agc = makeAgc(rates[ri]);
            agc.setMode(row.mode);
            const double t = recoveryTimeS(agc, rates[ri], 0.5f, +20.0f);
            std::printf("  %s fs=%.0f 20 dB rise pulled back in %.5f s\n", row.name,
                        rates[ri], t);
            CHECK(t < 0.010);
            CHECK(t > 0.001);  // and it is a real move, not a measurement slip
        }
    }
}

void testTimedRecoveryIsLevelIndependent() {
    // The same 20 dB step from a quiet and from a loud base level: the old loop
    // took a different time for each; the constant-slew loop must not.
    const double fs = 400000.0;
    for (Agc::Mode mode : {Agc::Mode::Fast, Agc::Mode::Medium}) {
        Agc a = makeAgc(fs);
        a.setMode(mode);
        Agc b = makeAgc(fs);
        b.setMode(mode);
        const double tq = recoveryTimeS(a, fs, 0.05f, -20.0f);   // gain settles near +20 dB
        const double tl = recoveryTimeS(b, fs, 0.4f, -20.0f);    // gain settles near +2 dB
        CHECK(std::fabs(tq - tl) < 0.05 * tq);
    }
}

void testOffHoldsTheGain() {
    const double fs = 400000.0;
    Agc agc = makeAgc(fs);
    // Let Auto find a gain well away from unity on a quiet input.
    std::vector<float> quiet(200000, 0.0f);
    for (std::size_t i = 0; i < quiet.size(); ++i) {
        quiet[i] = 0.05f * std::sin(0.2f * static_cast<float>(i));
    }
    std::vector<float> out(quiet.size());
    agc.process(quiet.data(), out.data(), quiet.size());
    const float held = agc.gain();
    CHECK(held > 2.0f);

    agc.setMode(Agc::Mode::Off);
    CHECK(agc.gain() == held);                         // frozen at the value held
    CHECK_NEAR(agc.fixedGainDb(), 20.0 * std::log10(held), 1e-3);

    // A 20 dB step up and down: not one sample of movement.
    std::vector<float> loud(50000);
    for (std::size_t i = 0; i < loud.size(); ++i) { loud[i] = 5.0f * std::sin(0.3f * static_cast<float>(i)); }
    std::vector<float> o2(loud.size());
    agc.process(loud.data(), o2.data(), loud.size());
    CHECK(agc.gain() == held);
    agc.process(quiet.data(), out.data(), 50000);
    CHECK(agc.gain() == held);
    std::size_t wrong = 0;
    for (std::size_t i = 0; i < 50000; ++i) {
        if (!sameBits(out[i], quiet[i] * held)) { ++wrong; }
    }
    CHECK(wrong == 0);

    // Fixed gain is the gain applied.
    agc.setFixedGainDb(12.0f);
    CHECK_NEAR(agc.gain(), std::pow(10.0, 12.0 / 20.0), 1e-5);
    CHECK_NEAR(agc.fixedGainDb(), 12.0, 1e-3);
    agc.process(loud.data(), o2.data(), loud.size());
    CHECK_NEAR(o2[10], loud[10] * std::pow(10.0, 12.0 / 20.0), 1e-3);
    CHECK_NEAR(agc.gainDb(), 12.0, 1e-3);

    // Range: -20 to +40 dB (below unity is real: entering Off freezes a held gain that can be
    // under 0 dB), NaN lands on 0.
    agc.setFixedGainDb(-7.0f);
    CHECK_NEAR(agc.fixedGainDb(), -7.0, 1e-3);
    CHECK_NEAR(agc.gain(), std::pow(10.0, -7.0 / 20.0), 1e-5);
    agc.setFixedGainDb(-55.0f);
    CHECK_NEAR(agc.fixedGainDb(), -20.0, 1e-3);
    agc.setFixedGainDb(90.0f);
    CHECK_NEAR(agc.fixedGainDb(), 40.0, 1e-3);
    agc.setFixedGainDb(std::numeric_limits<float>::quiet_NaN());
    CHECK_NEAR(agc.fixedGainDb(), 0.0, 1e-4);
    agc.setFixedGainDb(20.0f);
    CHECK_NEAR(agc.gain(), 10.0, 1e-4);

    // A reset in Off keeps the fixed gain (a retune or mode change must not
    // turn a fixed 20 dB into unity), and Off complex path likewise.
    agc.reset();
    CHECK_NEAR(agc.gain(), 10.0, 1e-4);
    std::complex<float> c(0.1f, -0.2f);
    std::complex<float> y;
    agc.process(&c, &y, 1);
    CHECK_NEAR(y.real(), 1.0, 1e-4);
    CHECK_NEAR(y.imag(), -2.0, 1e-4);

    // Leaving Off resumes from the gain it held, without a jump.
    agc.setMode(Agc::Mode::Slow);
    CHECK_NEAR(agc.gain(), 10.0, 1e-4);
    agc.setMode(Agc::Mode::Auto);
    CHECK_NEAR(agc.gain(), 10.0, 1e-4);
    agc.reset();
    CHECK(agc.gain() == 1.0f);
}

// Entering Off freezes the gain the loop HELD, which on a strong signal is under unity: the fixed-gain
// range is -20 to +40 dB, a held gain inside it is kept exactly, and one below -20 dB is frozen at -20.
void testOffFreezesAHeldGainBelowUnity() {
    const double fs = 400000.0;
    {
        Agc agc = makeAgc(fs);
        std::vector<float> loud(200000);
        for (std::size_t i = 0; i < loud.size(); ++i) { loud[i] = 1.5f * std::sin(0.2f * static_cast<float>(i)); }
        std::vector<float> out(loud.size());
        agc.process(loud.data(), out.data(), loud.size());
        const float held = agc.gain();
        CHECK(held < 1.0f && held > 0.2f);  // between -14 and 0 dB
        agc.setMode(Agc::Mode::Off);
        CHECK(agc.gain() == held);
        CHECK_NEAR(agc.fixedGainDb(), 20.0 * std::log10(held), 1e-3);
        CHECK(agc.fixedGainDb() < 0.0f);
    }
    {
        Agc agc = makeAgc(fs);
        std::vector<float> huge(300000);
        for (std::size_t i = 0; i < huge.size(); ++i) { huge[i] = 80.0f * std::sin(0.2f * static_cast<float>(i)); }
        std::vector<float> out(huge.size());
        agc.process(huge.data(), out.data(), huge.size());
        CHECK(agc.gainDb() < -20.0f);  // the loop went below the floor
        agc.setMode(Agc::Mode::Off);
        CHECK_NEAR(agc.fixedGainDb(), -20.0, 1e-3);  // clamped to -20
        CHECK_NEAR(agc.gainDb(), -20.0, 1e-3);
    }
}

void testNonFiniteInEveryMode() {
    const std::vector<float> x = makeProgram(30000, true);
    for (Agc::Mode m : {Agc::Mode::Auto, Agc::Mode::Fast, Agc::Mode::Medium, Agc::Mode::Slow,
                        Agc::Mode::Off}) {
        Agc agc = makeAgc(400000.0);
        agc.setMode(m);
        std::vector<float> out(x.size());
        agc.process(x.data(), out.data(), x.size());
        CHECK(std::isfinite(agc.gain()));
        CHECK(agc.gain() >= 0.0f);
        CHECK(agc.gain() <= 100.0f);
        // And the loop is still tracking afterwards: a clean quiet tone is
        // output finite, in a mode that moves the gain it is lifted.
        std::vector<float> clean(20000);
        for (std::size_t i = 0; i < clean.size(); ++i) { clean[i] = 0.01f * std::sin(0.1f * static_cast<float>(i)); }
        std::vector<float> o2(clean.size());
        agc.process(clean.data(), o2.data(), clean.size());
        std::size_t bad = 0;
        for (float v : o2) { if (!std::isfinite(v)) { ++bad; } }
        CHECK(bad == 0);
    }
}

void testRealAndComplexAgree() {
    // A real signal through the real path and the same samples as complex with
    // zero imaginary part leave the loop at the same gain, in a timed mode.
    const std::vector<float> x = makeProgram(60000, false);
    Agc a = makeAgc(400000.0);
    a.setMode(Agc::Mode::Fast);
    Agc b = makeAgc(400000.0);
    b.setMode(Agc::Mode::Fast);
    std::vector<float> o(x.size());
    a.process(x.data(), o.data(), x.size());
    std::vector<std::complex<float>> cx(x.size());
    std::vector<std::complex<float>> co(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) { cx[i] = {x[i], 0.0f}; }
    b.process(cx.data(), co.data(), cx.size());
    CHECK(sameBits(a.gain(), b.gain()));
}

void testSteadyToneRipple() {
    // A real 1 kHz-ish tone (the level detector sees |x|, which crosses the
    // target every cycle): each timed mode must settle with the output peak
    // close to the target and a small gain ripple. Measured and printed.
    const double fs = 400000.0;
    for (Agc::Mode m : {Agc::Mode::Fast, Agc::Mode::Medium, Agc::Mode::Slow}) {
        Agc agc = makeAgc(fs);
        agc.setMode(m);
        std::vector<float> in(static_cast<std::size_t>(fs));  // 1 s blocks
        std::vector<float> out(in.size());
        for (int sec = 0; sec < 5; ++sec) {
            for (std::size_t i = 0; i < in.size(); ++i) {
                in[i] = 0.1f * static_cast<float>(
                                   std::sin(6.283185307179586 * 1000.0 * static_cast<double>(i) / fs));
            }
            agc.process(in.data(), out.data(), in.size());
        }
        float peak = 0.0f;
        float gmin = 1e9f;
        float gmax = 0.0f;
        for (std::size_t i = 0; i < 4000; ++i) {  // ten cycles
            float o;
            const float x = 0.1f * static_cast<float>(
                                       std::sin(6.283185307179586 * 1000.0 * static_cast<double>(i) / fs));
            agc.process(&x, &o, 1);
            peak = std::max(peak, std::fabs(o));
            gmin = std::min(gmin, agc.gain());
            gmax = std::max(gmax, agc.gain());
        }
        const double rippleDb = 20.0 * std::log10(static_cast<double>(gmax) / gmin);
        std::printf("  mode %d tone: output peak %.3f, gain ripple %.3f dB\n",
                    static_cast<int>(m), peak, rippleDb);
        CHECK(peak > 0.45f);
        CHECK(peak < 0.62f);
        CHECK(rippleDb < 0.6);
    }
}

}  // namespace

int main() {
    testAutoIsTheOldLoopSampleForSample();
    testTimedModesRecoveryAtTwoRates();
    testTimedRecoveryIsLevelIndependent();
    testOffHoldsTheGain();
    testOffFreezesAHeldGainBelowUnity();
    testNonFiniteInEveryMode();
    testRealAndComplexAgree();
    testSteadyToneRipple();
    return testSummary("test_agc_modes");
}
