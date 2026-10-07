// Tests for dsp/deemphasis.hpp - the one-pole FM de-emphasis that dsp::Demodulator
// and core::patch::Strip (the airband monitor's NFM channels) both run.
//
// The reference is the closed-form magnitude of the pole-matched network,
//     |H(f)| = (1 - p) / |1 - p e^{-jw}|,   p = exp(-1 / (rate * tau)),
// worked out here from the 50 us and 75 us SPEC constants, never read back from
// the class. test_demod.cpp pins the same network through the Demodulator; this
// file pins what the demodulator never exercises: a change of rate, the
// "off" inputs, and that a block split changes nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/deemphasis.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <vector>

#include "test_check.hpp"

using cascade::dsp::Deemphasis;

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
// What the correlation in measuredGain can be off by: its window is not a whole
// number of cycles at every rate and tone (300 Hz at 47628 Hz is 25.2), and the
// tone's mirror image leaks in by about 1 / (window * w). The effects under test
// are two orders of magnitude larger (3 kHz is 0.73, not 1.0).
constexpr double kGainTol = 5e-3;

double refGain(double tauUs, double fHz, double rateHz) {
    const double p = std::exp(-1.0 / (rateHz * tauUs * 1e-6));
    const double w = kTwoPi * fHz / rateHz;
    return (1.0 - p) / std::abs(1.0 - p * std::polar(1.0, -w));
}

// Steady-state amplitude of a unit sine of fHz through `d`: the correlation of
// the last 4000 samples with the tone itself (2|sum x e^-jw n| / N, which reads
// a sine of amplitude a as a), after 200 for the filter to settle (its time
// constant is a few samples). Not the peak sample: at 16 samples a cycle the
// largest of them can sit a few percent under the crest.
double measuredGain(Deemphasis& d, double fHz, double rateHz) {
    std::vector<float> x(4200);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(std::sin(kTwoPi * fHz * static_cast<double>(i) / rateHz));
    }
    d.process(x.data(), x.size());
    std::complex<double> acc(0.0, 0.0);
    for (std::size_t i = 200; i < x.size(); ++i) {
        acc += static_cast<double>(x[i]) * std::polar(1.0, -kTwoPi * fHz * static_cast<double>(i) / rateHz);
    }
    return 2.0 * std::abs(acc) / static_cast<double>(x.size() - 200);
}

}  // namespace

int main() {
    // --- the default is the 50 us network, and it is the network -------------
    for (const double rate : {48000.0, 47628.0, 96000.0}) {
        Deemphasis d(rate);
        CHECK_NEAR(d.timeUs(), 50.0, 1e-9);
        CHECK(d.enabled());
        for (const double f : {300.0, 1000.0, 3000.0, 6000.0}) {
            Deemphasis fresh(rate);
            CHECK_NEAR(measuredGain(fresh, f, rate), refGain(50.0, f, rate), kGainTol);
        }
    }
    {
        Deemphasis d(48000.0);
        d.setTimeUs(75.0);
        CHECK_NEAR(d.timeUs(), 75.0, 1e-9);
        CHECK_NEAR(measuredGain(d, 3000.0, 48000.0), refGain(75.0, 3000.0, 48000.0), kGainTol);
        // 75 is not 50: the two standards are told apart, by about 1.5 dB at 3 kHz.
        CHECK(refGain(75.0, 3000.0, 48000.0) < refGain(50.0, 3000.0, 48000.0) - 0.08);
    }

    // --- OFF: anything that is not a positive finite number ------------------
    for (const double bad : {0.0, -50.0, std::nan(""), std::numeric_limits<double>::infinity()}) {
        Deemphasis d(48000.0);
        d.setTimeUs(bad);
        CHECK(d.timeUs() == 0.0);
        CHECK(!d.enabled());
        std::vector<float> x = {0.5f, -0.25f, 1.0f, 0.0f, 0.125f};
        const std::vector<float> before = x;
        d.process(x.data(), x.size());
        CHECK(x == before);   // a pass-through to the bit, never a NaN
        CHECK(d.step(0.75f) == 0.75f);
    }

    // --- A RATE THAT IS NOT A RATE places no pole, and the time constant survives
    {
        Deemphasis d(48000.0);
        d.setRate(0.0);
        CHECK(!d.enabled());
        CHECK_NEAR(d.timeUs(), 50.0, 1e-9);
        d.setRate(-48000.0);   // a pole outside the unit circle would blow up
        CHECK(!d.enabled());
        std::vector<float> x(100, 1.0f);
        d.process(x.data(), x.size());
        CHECK(x.back() == 1.0f);
        d.setRate(48000.0);
        CHECK(d.enabled());
        CHECK_NEAR(measuredGain(d, 3000.0, 48000.0), refGain(50.0, 3000.0, 48000.0), kGainTol);
    }

    // --- a change of rate keeps the time constant and moves the pole ---------
    {
        Deemphasis d(48000.0);
        d.setRate(96000.0);
        CHECK_NEAR(d.timeUs(), 50.0, 1e-9);
        // The corner is a frequency, not a number of samples: at the doubled rate
        // 3 kHz is attenuated by the same amount to within the pole-matching error.
        CHECK_NEAR(measuredGain(d, 3000.0, 96000.0), refGain(50.0, 3000.0, 96000.0), kGainTol);
        CHECK_NEAR(refGain(50.0, 3000.0, 96000.0), refGain(50.0, 3000.0, 48000.0), 0.01);
    }

    // --- reset and a change of constant clear the memory ---------------------
    {
        Deemphasis d(48000.0);
        for (int i = 0; i < 100; ++i) { (void)d.step(1.0f); }
        d.reset();
        const double p = std::exp(-1.0 / (48000.0 * 50e-6));
        CHECK_NEAR(d.step(1.0f), 1.0 - p, 1e-6);   // first output of a fresh filter
        for (int i = 0; i < 100; ++i) { (void)d.step(1.0f); }
        d.setTimeUs(75.0);
        const double p75 = std::exp(-1.0 / (48000.0 * 75e-6));
        CHECK_NEAR(d.step(1.0f), 1.0 - p75, 1e-6);
    }

    // --- any block split, and step(), give the output of one call ------------
    {
        std::vector<float> in(1000);
        for (std::size_t i = 0; i < in.size(); ++i) { in[i] = static_cast<float>(std::sin(0.01 * i * i)); }
        Deemphasis whole(48000.0), blocks(48000.0), single(48000.0);
        std::vector<float> a = in, b = in, c = in;
        whole.process(a.data(), a.size());
        std::size_t at = 0;
        for (const std::size_t n : {1u, 7u, 100u, 333u, 559u}) {
            blocks.process(b.data() + at, n);
            at += n;
        }
        CHECK(at == b.size());
        for (float& v : c) { v = single.step(v); }
        CHECK(a == b);
        CHECK(a == c);
    }

    return testSummary("test_deemphasis");
}
