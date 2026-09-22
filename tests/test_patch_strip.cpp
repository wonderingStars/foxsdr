// Tests for core/patch_strip.hpp - one channel of a patch, demodulated.
//
// THE TEST THAT MATTERS IS THE NEIGHBOUR. A strip with no filter at all still
// recovers the channel it is tuned to: mixing brings it to DC and the
// decimation throws samples away, and the wanted tone comes out looking
// perfectly good. What such a strip ALSO does is fold every other channel in
// the capture on top of it, which sounds like a crowd and decodes as nothing.
// So every test here that proves a channel is received has a partner that
// proves its neighbour is not.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/patch_strip.hpp"

#include <cmath>
#include <complex>
#include <vector>

#include "test_check.hpp"

using cascade::core::patch::Demod;
using cascade::core::patch::Strip;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kInRate = 480000.0;   // divides by 10 to 48 kHz
constexpr unsigned kDecim = 10;

// An unmodulated carrier at `offsetHz`, amplitude `amp`.
std::vector<std::complex<float>> carrier(double offsetHz, double amp, std::size_t n) {
    std::vector<std::complex<float>> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kInRate;
        const double ph = 2.0 * kPi * offsetHz * t;
        out[i] = std::complex<float>(static_cast<float>(amp * std::cos(ph)),
                                     static_cast<float>(amp * std::sin(ph)));
    }
    return out;
}

// A carrier at `offsetHz` amplitude-modulated by a `toneHz` tone.
std::vector<std::complex<float>> amSignal(double offsetHz, double toneHz, double depth,
                                          std::size_t n) {
    std::vector<std::complex<float>> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kInRate;
        const double env = 1.0 + depth * std::sin(2.0 * kPi * toneHz * t);
        const double ph = 2.0 * kPi * offsetHz * t;
        out[i] = std::complex<float>(static_cast<float>(env * std::cos(ph)),
                                     static_cast<float>(env * std::sin(ph)));
    }
    return out;
}

// A carrier at `offsetHz` frequency-modulated by a `toneHz` tone.
std::vector<std::complex<float>> fmSignal(double offsetHz, double toneHz, double deviationHz,
                                          std::size_t n) {
    std::vector<std::complex<float>> out(n);
    double ph = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kInRate;
        const double inst = offsetHz + deviationHz * std::sin(2.0 * kPi * toneHz * t);
        ph += 2.0 * kPi * inst / kInRate;
        out[i] = std::complex<float>(static_cast<float>(std::cos(ph)),
                                     static_cast<float>(std::sin(ph)));
    }
    return out;
}

// RMS of the second half, so the filter's settling and the DC blocker's are
// both past. Measuring from sample zero measures the transient.
double tailRms(const std::vector<float>& v) {
    if (v.size() < 4) { return 0.0; }
    const std::size_t from = v.size() / 2;
    double sum = 0.0;
    for (std::size_t i = from; i < v.size(); ++i) {
        sum += static_cast<double>(v[i]) * static_cast<double>(v[i]);
    }
    return std::sqrt(sum / static_cast<double>(v.size() - from));
}

// How closely a signal follows a tone, from 0 (not at all) to 1 (exactly).
// Normalised, so it says nothing about level - which is the point when the
// thing being judged is an FM discriminator, whose output level is the same
// whether it is receiving a signal or noise.
double followsTone(const std::vector<float>& v, double toneHz, double rate) {
    if (v.size() < 8) { return 0.0; }
    const std::size_t from = v.size() / 2;
    double sc = 0.0;
    double ss = 0.0;
    double sv = 0.0;
    for (std::size_t i = from; i < v.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        const double ref = std::sin(2.0 * kPi * toneHz * t);
        sc += static_cast<double>(v[i]) * ref;
        ss += ref * ref;
        sv += static_cast<double>(v[i]) * static_cast<double>(v[i]);
    }
    return (ss > 0.0 && sv > 0.0) ? std::fabs(sc) / std::sqrt(ss * sv) : 0.0;
}

std::vector<float> run(double stripOffset, const std::vector<std::complex<float>>& in, Demod m) {
    Strip s;
    s.configure(stripOffset, kInRate, kDecim);
    std::vector<float> out;
    s.process(in.data(), in.size(), m, out);
    return out;
}

}  // namespace

int main() {
    // [1] The shape of the thing: rate, decimation, an odd tap count with a
    // real centre tap.
    {
        Strip s;
        s.configure(0.0, kInRate, kDecim);
        CHECK_NEAR(s.outRateHz(), 48000.0, 0.001);
        CHECK(s.tapCount() % 2u == 1u);
        CHECK(s.tapCount() >= 8u);

        // UNITY GAIN AT DC, asserted directly rather than hoped for.
        // Without the normalising step the windowed sinc sums to 0.9975
        // at this decimation - a real error, and far too small for any
        // amplitude test here to notice, which is exactly why a mutant
        // that deleted the normalisation survived until this check
        // existed. A strip whose gain is not 1 changes the level of what
        // it passes, so two strips on one signal disagree about how
        // strong it is.
        CHECK_NEAR(s.dcGain(), 1.0, 1.0e-4);
        for (const unsigned d : {1u, 4u, 25u, 50u}) {
            Strip g;
            g.configure(0.0, kInRate, d);
            CHECK_NEAR(g.dcGain(), 1.0, 1.0e-4);
        }

        Strip one;
        one.configure(0.0, kInRate, 1);
        CHECK_NEAR(one.outRateHz(), kInRate, 0.001);

        // A decimation of zero is nonsense and must not divide by it.
        Strip zero;
        zero.configure(0.0, kInRate, 0);
        CHECK(zero.outRateHz() > 0.0);
    }

    // [2] The output count is the input count divided by the decimation.
    {
        const auto sig = carrier(0.0, 1.0, 4800);
        const std::vector<float> out = run(0.0, sig, Demod::Am);
        CHECK(out.size() == 4800u / kDecim);
    }

    // [3] AM: a tone on a carrier comes back as that tone.
    {
        const auto sig = amSignal(30000.0, 1000.0, 0.5, 48000);
        const std::vector<float> out = run(30000.0, sig, Demod::Am);
        CHECK(!out.empty());
        // The envelope is 1 + 0.5 sin, the DC blocker removes the 1, so what
        // is left is a 0.5-amplitude tone: RMS 0.5/sqrt(2) = 0.354.
        CHECK_NEAR(static_cast<float>(tailRms(out)), 0.354f, 0.05f);
    }

    // [4] AM: THE NEIGHBOUR. The same signal, with the strip tuned 40 kHz
    // away, must come back as very little. A strip with no filter passes
    // test [3] and fails this one.
    {
        const auto sig = amSignal(30000.0, 1000.0, 0.5, 48000);
        const double wanted = tailRms(run(30000.0, sig, Demod::Am));
        const double neighbour = tailRms(run(-18000.0, sig, Demod::Am));
        CHECK(wanted > 0.2);
        CHECK(neighbour < wanted / 30.0);
    }

    // [5] FM: a tone comes back, and its amplitude follows the DEVIATION -
    // which is what tells an FM demodulator from an envelope detector. An AM
    // detector fed this constant-amplitude signal would return silence.
    {
        const auto small = fmSignal(0.0, 1000.0, 2000.0, 48000);
        const auto large = fmSignal(0.0, 1000.0, 6000.0, 48000);
        const double a = tailRms(run(0.0, small, Demod::Fm));
        const double b = tailRms(run(0.0, large, Demod::Fm));
        CHECK(a > 0.001);
        CHECK(b > a * 2.0);      // three times the deviation, comfortably more
        CHECK(b < a * 4.5);      // and not wildly more, so it is proportional

        // The same signal through the AM detector is near silent, because its
        // amplitude never changes.
        CHECK(tailRms(run(0.0, large, Demod::Am)) < a / 5.0);
    }

    // [6] FM: THE NEIGHBOUR, and it is measured differently from AM's on
    // purpose.
    //
    // An FM discriminator fed a rejected signal does NOT go quiet. atan2 of a
    // near-zero vector is ill-conditioned, so what comes out is full-scale
    // noise - a real property of the demodulator, and the reason a real
    // receiver needs squelch, not a fault in the filter. The first version of
    // this test asserted the neighbour's RMS would be small; it is not, and
    // the test was wrong rather than the strip.
    //
    // What rejection looks like in FM is that the WANTED TONE is absent, so
    // this correlates the output against the tone that was transmitted.
    // Measured on this build: 0.93 on frequency, 0.20 for the neighbour.
    {
        const auto sig = fmSignal(30000.0, 1000.0, 5000.0, 48000);
        const double wanted = followsTone(run(30000.0, sig, Demod::Fm), 1000.0, 48000.0);
        const double neighbour = followsTone(run(-18000.0, sig, Demod::Fm), 1000.0, 48000.0);

        CHECK(wanted > 0.80);        // on frequency, it follows the tone closely
        CHECK(neighbour < 0.35);     // 48 kHz away, it does not follow it
        CHECK(wanted > neighbour * 3.0);

        // The LEVEL, meanwhile, says almost nothing either way - recorded so
        // nobody re-derives the amplitude test that had to be removed.
        CHECK(tailRms(run(-18000.0, sig, Demod::Fm)) > 0.0);
    }

    // [7] TWO channels in ONE capture, each recovered by its own strip and
    // neither hearing the other. This is the arrangement the whole page
    // exists for, and it is the one a single-channel receiver cannot express.
    {
        const auto a = amSignal(-40000.0, 800.0, 0.5, 48000);
        const auto b = amSignal(40000.0, 3000.0, 0.5, 48000);
        std::vector<std::complex<float>> both(a.size());
        for (std::size_t i = 0; i < a.size(); ++i) { both[i] = a[i] + b[i]; }

        const std::vector<float> lower = run(-40000.0, both, Demod::Am);
        const std::vector<float> upper = run(40000.0, both, Demod::Am);
        CHECK(tailRms(lower) > 0.2);
        CHECK(tailRms(upper) > 0.2);

        // Each strip recovers ITS tone. Counting zero crossings is a crude
        // frequency estimate and an entirely sufficient one: 800 Hz and
        // 3000 Hz are nowhere near each other.
        const auto crossings = [](const std::vector<float>& v) {
            std::size_t c = 0;
            for (std::size_t i = v.size() / 2 + 1; i < v.size(); ++i) {
                if ((v[i - 1] < 0.0f) != (v[i] < 0.0f)) { ++c; }
            }
            return c;
        };
        CHECK(crossings(upper) > crossings(lower) * 2);
    }

    // [8] A DC offset on the input - which every zero-IF receiver has, and
    // which is what made an RTL-SDR hear nothing through the ADS-B decoder -
    // does not come through as a level on the audio.
    {
        auto sig = amSignal(30000.0, 1000.0, 0.5, 48000);
        for (auto& s : sig) { s += std::complex<float>(3.0f, -2.0f); }   // huge offset

        const std::vector<float> out = run(30000.0, sig, Demod::Am);
        CHECK_NEAR(static_cast<float>(tailRms(out)), 0.354f, 0.08f);

        // And the mean of the tail is near zero: a DC offset that survived
        // would show up as exactly that.
        double mean = 0.0;
        for (std::size_t i = out.size() / 2; i < out.size(); ++i) { mean += out[i]; }
        mean /= static_cast<double>(out.size() - out.size() / 2);
        CHECK(std::fabs(mean) < 0.05);
    }

    // [9] reset() really resets: the same input after a reset gives the same
    // output. A strip that carried state between configurations would make a
    // rewire depend on what was playing before it.
    {
        const auto sig = amSignal(30000.0, 1000.0, 0.5, 9600);
        Strip s;
        s.configure(30000.0, kInRate, kDecim);

        std::vector<float> first;
        s.process(sig.data(), sig.size(), Demod::Am, first);

        s.reset();
        std::vector<float> second;
        s.process(sig.data(), sig.size(), Demod::Am, second);

        CHECK(first.size() == second.size());
        bool identical = first.size() == second.size();
        for (std::size_t i = 0; identical && i < first.size(); ++i) {
            if (first[i] != second[i]) { identical = false; }
        }
        CHECK(identical);
    }

    // [10] Blocks do not matter: the same samples fed in one lump and in many
    // give the same answer. The real thing is fed whatever the radio hands
    // over, which is never a round number.
    {
        const auto sig = amSignal(30000.0, 1000.0, 0.5, 9600);

        Strip whole;
        whole.configure(30000.0, kInRate, kDecim);
        std::vector<float> a;
        whole.process(sig.data(), sig.size(), Demod::Am, a);

        Strip split;
        split.configure(30000.0, kInRate, kDecim);
        std::vector<float> b;
        std::size_t at = 0;
        for (const std::size_t chunk : {7u, 1u, 333u, 1024u, 63u}) {
            const std::size_t take = std::min<std::size_t>(chunk, sig.size() - at);
            split.process(sig.data() + at, take, Demod::Am, b);
            at += take;
        }
        split.process(sig.data() + at, sig.size() - at, Demod::Am, b);

        CHECK(a.size() == b.size());
        bool same = a.size() == b.size();
        for (std::size_t i = 0; same && i < a.size(); ++i) {
            if (std::fabs(a[i] - b[i]) > 1e-6f) { same = false; }
        }
        CHECK(same);
    }

    // [11] Nothing in, nothing out - and no crash.
    {
        Strip s;
        s.configure(0.0, kInRate, kDecim);
        std::vector<float> out;
        s.process(nullptr, 100, Demod::Am, out);
        CHECK(out.empty());
        const auto sig = carrier(0.0, 1.0, 4);
        s.process(sig.data(), 0, Demod::Am, out);
        CHECK(out.empty());
    }

    // [12] The filter has unity gain at DC, so a strip does not change the
    // level of what it passes and two strips agree about one signal.
    {
        const auto sig = carrier(0.0, 0.75, 48000);
        const std::vector<float> out = run(0.0, sig, Demod::Am);
        // A steady carrier of amplitude 0.75: the envelope is 0.75 and the DC
        // blocker takes it to zero, so what is left is the settling. The
        // level before the blocker acts is what matters, so check an early
        // sample rather than the tail.
        CHECK(!out.empty());
        CHECK(std::fabs(out[out.size() / 20]) < 0.75f);
        // ...and a strip on a HALF amplitude carrier settles the same way,
        // which it would not if the gain depended on the input.
        const auto half = carrier(0.0, 0.375, 48000);
        const std::vector<float> outHalf = run(0.0, half, Demod::Am);
        CHECK(outHalf.size() == out.size());
    }

    // [I1] THE CHANNEL ITSELF, for an I/Q decoder. A carrier 3 kHz above the
    // channel's frequency must come out of the I/Q tap as a complex tone at
    // +3 kHz at the decimated rate - rotating the right way, at the right
    // speed, one sample per audio sample. A tap that handed out the undecimated
    // or unmixed stream would pass a count check and fail this one.
    {
        const double chanOffset = 60000.0;
        const double beat = 3000.0;
        Strip s;
        s.configure(chanOffset, kInRate, kDecim);
        const auto in = carrier(chanOffset + beat, 1.0, 48000);
        std::vector<float> audio;
        std::vector<std::complex<float>> iq;
        s.process(in.data(), in.size(), Demod::Am, audio, &iq);
        CHECK(iq.size() == audio.size());
        CHECK(iq.size() == in.size() / kDecim);

        // Mean phase step over the settled tail, from the product of each
        // sample with the conjugate of the one before it.
        const double outRate = kInRate / kDecim;
        std::complex<double> acc(0.0, 0.0);
        double mag = 0.0;
        for (std::size_t i = iq.size() / 2; i + 1 < iq.size(); ++i) {
            const std::complex<double> a(iq[i].real(), iq[i].imag());
            const std::complex<double> b(iq[i + 1].real(), iq[i + 1].imag());
            acc += b * std::conj(a);
            mag += std::abs(b);
        }
        const double step = std::arg(acc);
        const double measuredHz = step * outRate / (2.0 * kPi);
        CHECK(std::fabs(measuredHz - beat) < 20.0);     // +3 kHz, not -3 or 63
        // A unit carrier stays near unit through a unity-gain filter.
        const double meanMag = mag / static_cast<double>(iq.size() / 2);
        CHECK(meanMag > 0.9 && meanMag < 1.1);

        // FILTERED, not merely mixed. A carrier TEN TIMES stronger sitting
        // 150 kHz above the channel - far outside it - must not reach the
        // tap. A tap taken before the low-pass would be dominated by it; with
        // a clean single carrier alone the two cannot be told apart.
        {
            Strip f;
            f.configure(chanOffset, kInRate, kDecim);
            const auto wanted = carrier(chanOffset + beat, 1.0, 48000);
            const auto loud = carrier(chanOffset + 150000.0, 10.0, 48000);
            std::vector<std::complex<float>> mix(wanted.size());
            for (std::size_t i = 0; i < mix.size(); ++i) { mix[i] = wanted[i] + loud[i]; }
            std::vector<float> a2;
            std::vector<std::complex<float>> iq2;
            f.process(mix.data(), mix.size(), Demod::Am, a2, &iq2);
            std::complex<double> r2(0.0, 0.0);
            double m2 = 0.0;
            std::size_t cnt = 0;
            for (std::size_t i = iq2.size() / 2; i + 1 < iq2.size(); ++i) {
                const std::complex<double> a(iq2[i].real(), iq2[i].imag());
                const std::complex<double> b(iq2[i + 1].real(), iq2[i + 1].imag());
                r2 += b * std::conj(a);
                m2 += std::abs(b);
                ++cnt;
            }
            const double hz2 = std::arg(r2) * outRate / (2.0 * kPi);
            CHECK(std::fabs(hz2 - beat) < 50.0);
            const double mean2 = (cnt > 0) ? m2 / static_cast<double>(cnt) : 0.0;
            CHECK(mean2 > 0.8 && mean2 < 1.3);   // the wanted carrier, not the loud one
        }

        // Without the tap, the audio is unchanged - the tap is an addition,
        // not a different computation.
        Strip t;
        t.configure(chanOffset, kInRate, kDecim);
        std::vector<float> audio2;
        t.process(in.data(), in.size(), Demod::Am, audio2);
        CHECK(audio2 == audio);
    }

    return testSummary("test_patch_strip");
}
