// Tests for dsp/squelch.hpp (PowerMeter + Squelch).
//
// References are in-test math only: meter readings are checked against the
// analytic power of synthesized tones and against a double-precision EMA
// recurrence computed here; hold times are counted in samples against the
// commanded ms-to-samples conversion — never against constants read back
// from the implementation. No wall clock, no randomness.
//
// SPDX-License-Identifier: MIT
#include "dsp/squelch.hpp"

#include "test_check.hpp"

#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// Complex exponential at normalized frequency f (cycles/sample), amplitude a.
// |x[n]|^2 == a^2 at every sample, so the analytic power is exactly a^2.
std::vector<std::complex<float>> makeTone(double f, double a, std::size_t n) {
    std::vector<std::complex<float>> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = kTwoPi * f * static_cast<double>(i);
        x[i] = {static_cast<float>(a * std::cos(ph)),
                static_cast<float>(a * std::sin(ph))};
    }
    return x;
}

// Feeds `count` samples of constant power `levelDb` into the squelch, with a
// throwaway audio buffer. A constant complex sample is a legitimate stimulus:
// the meter sees only |x|^2, which for amplitude 10^(dB/20) is 10^(dB/10).
void feedLevel(cascade::dsp::Squelch& sq, double levelDb, std::size_t count) {
    const float a = static_cast<float>(std::pow(10.0, levelDb / 20.0));
    const std::complex<float> s{a, 0.0f};
    float audio = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        audio = 0.0f;
        sq.process(&s, 1, &audio);
    }
}

// Opens the gate at th+5, then counts silence samples until it closes.
// The count is the meter's decay to the close threshold plus the hold, so it
// must land within 20% of the commanded hold (the decay is a few dozen
// samples, small against any practical hold).
std::size_t observedHoldSamples(double fs, float th, float holdMs) {
    cascade::dsp::Squelch sq(fs);
    sq.setThresholdDb(th);
    sq.setHoldMs(holdMs);
    feedLevel(sq, static_cast<double>(th) + 5.0, 2000);
    CHECK(sq.isOpen());

    const std::complex<float> zero{0.0f, 0.0f};
    float audio = 0.0f;
    std::size_t count = 0;
    const std::size_t bound = 60000;  // deadline: sample-counted, no wall clock
    while (sq.isOpen() && count < bound) {
        audio = 0.0f;
        sq.process(&zero, 1, &audio);
        ++count;
    }
    CHECK(count < bound);  // it must actually close
    return count;
}

}  // namespace

int main() {
    // --- PowerMeter: converges to the analytic power of a tone --------------
    {
        // Unit-amplitude tone: power 1.0 -> 0 dB.
        auto x = makeTone(0.05, 1.0, 4096);
        cascade::dsp::PowerMeter m;
        m.process(x.data(), x.size());
        CHECK_NEAR(m.powerDb(), 0.0, 0.2);

        // 0.1-amplitude tone: power 0.01 -> -20 dB.
        auto y = makeTone(0.03, 0.1, 4096);
        cascade::dsp::PowerMeter m2;
        m2.process(y.data(), y.size());
        CHECK_NEAR(m2.powerDb(), -20.0, 0.2);

        // reset() returns the meter to the floor.
        m.reset();
        CHECK_NEAR(m.powerDb(), -200.0, 1e-9);
    }

    // --- PowerMeter: EMA law and the alpha parameter ------------------------
    {
        // Mid-convergence reading must match the double-precision recurrence
        // ema += alpha*(p - ema) computed here, for a non-default alpha —
        // this pins both the smoothing law and that alpha is actually used.
        const float alpha = 0.2f;
        const double p = 0.25;  // amplitude 0.5 -> power 0.25
        const std::size_t N = 10;
        auto x = makeTone(0.07, 0.5, N);
        cascade::dsp::PowerMeter m(alpha);
        m.process(x.data(), N);

        double ref = 0.0;
        for (std::size_t i = 0; i < N; ++i) { ref += 0.2 * (p - ref); }
        CHECK_NEAR(m.powerDb(), 10.0 * std::log10(ref), 0.02);

        // Block splits are invisible: same samples in two calls, same reading.
        cascade::dsp::PowerMeter mSplit(alpha);
        mSplit.process(x.data(), 3);
        mSplit.process(x.data() + 3, N - 3);
        CHECK_NEAR(mSplit.powerDb(), m.powerDb(), 1e-6);
    }

    // --- PowerMeter: floor and n=0 safety ------------------------------------
    {
        cascade::dsp::PowerMeter m;
        CHECK_NEAR(m.powerDb(), -200.0, 1e-9);  // fresh meter reads the floor
        m.process(nullptr, 0);                  // n=0 must be safe
        CHECK_NEAR(m.powerDb(), -200.0, 1e-9);
    }

    // --- Squelch: hysteresis sequence ----------------------------------------
    {
        const double fs = 48000.0;
        const float th = -20.0f;
        cascade::dsp::Squelch sq(fs);
        sq.setThresholdDb(th);
        // Default hold is 100 ms = 4800 samples at 48 kHz: every "stays"
        // check below feeds far longer than that, so hold cannot mask a
        // wrong threshold decision.
        CHECK(!sq.isOpen());  // starts closed (receiver comes up muted)

        // Level at threshold+5 opens.
        feedLevel(sq, static_cast<double>(th) + 5.0, 2000);
        CHECK(sq.isOpen());

        // Level at threshold-1 is inside the hysteresis band (close point is
        // threshold-3): the gate must stay open indefinitely, not merely for
        // the hold time. 20000 samples >> 4800-sample hold.
        feedLevel(sq, static_cast<double>(th) - 1.0, 20000);
        CHECK(sq.isOpen());

        // Level at threshold-5 is below the close point: closes once the
        // hold expires.
        feedLevel(sq, static_cast<double>(th) - 5.0, 20000);
        CHECK(!sq.isOpen());

        // Hysteresis is state-dependent: the same threshold-1 level that
        // KEPT the gate open must not RE-open a closed gate (opening
        // requires exceeding the full threshold).
        feedLevel(sq, static_cast<double>(th) - 1.0, 20000);
        CHECK(!sq.isOpen());
    }

    // --- Squelch: hold time honored within 20%, sample-counted ---------------
    {
        const double fs = 48000.0;
        const float th = -20.0f;
        // 50 ms at 48 kHz -> 2400 samples; 10 ms -> 480 samples. Two hold
        // values prove the count tracks the commanded time, not one lucky
        // constant.
        const std::size_t c50 = observedHoldSamples(fs, th, 50.0f);
        CHECK_NEAR(static_cast<double>(c50), 2400.0, 2400.0 * 0.2);
        const std::size_t c10 = observedHoldSamples(fs, th, 10.0f);
        CHECK_NEAR(static_cast<double>(c10), 480.0, 480.0 * 0.2);
    }

    // --- Squelch: closed gate mutes to exact zero -----------------------------
    {
        cascade::dsp::Squelch sq(48000.0);
        sq.setThresholdDb(-20.0f);
        // Signal 10 dB under threshold: the gate never opens, so every audio
        // sample must come out exactly 0 (envelope starts and stays at 0).
        const float a = static_cast<float>(std::pow(10.0, -30.0 / 20.0));
        const std::complex<float> s{a, 0.0f};
        for (std::size_t i = 0; i < 5000; ++i) {
            float audio = 1.0f;
            sq.process(&s, 1, &audio);
            CHECK(audio == 0.0f);
        }
        CHECK(!sq.isOpen());
    }

    // --- Squelch: ~5 ms linear ramp, no clicks, exact zero after close -------
    {
        const double fs = 48000.0;
        // Ramp length from the spec (5 ms at fs), derived here, not read from
        // the implementation.
        const float ramp = static_cast<float>(fs * 0.005);  // 240 samples
        cascade::dsp::Squelch sq(fs);
        sq.setThresholdDb(-20.0f);
        sq.setHoldMs(10.0f);  // short hold keeps the record small

        // Constant full-scale audio isolates the envelope: any sample-to-
        // sample output delta is the gate's doing, not the signal's.
        std::vector<float> rec;
        const std::complex<float> strong{1.0f, 0.0f};  // 0 dB, 20 dB over th
        const std::complex<float> zero{0.0f, 0.0f};
        for (std::size_t i = 0; i < 2000; ++i) {
            float audio = 1.0f;
            sq.process(&strong, 1, &audio);
            rec.push_back(audio);
        }
        CHECK(sq.isOpen());
        // Envelope clamps at the endpoint: unity gain exactly once ramped up.
        CHECK(rec.back() == 1.0f);

        // Gate off: silence on the measurement input, audio still full-scale.
        // 3000 samples cover meter decay (~103) + hold (480) + ramp (240).
        for (std::size_t i = 0; i < 3000; ++i) {
            float audio = 1.0f;
            sq.process(&zero, 1, &audio);
            rec.push_back(audio);
        }
        CHECK(!sq.isOpen());
        // Full-scale tone gated off must reach EXACT zero after the ramp.
        CHECK(rec.back() == 0.0f);

        // Click bound over the whole record (both transitions): with audio
        // peak 1.0 the max delta must be within peak/ramp_samples * 2.
        float maxDelta = 0.0f;
        float prev = 0.0f;  // gate starts closed, so 0 is the true predecessor
        bool sawOpen = false;
        for (float v : rec) {
            const float d = std::fabs(v - prev);
            if (d > maxDelta) { maxDelta = d; }
            if (v == 1.0f) { sawOpen = true; }
            prev = v;
        }
        CHECK(sawOpen);  // the transition genuinely happened
        CHECK(maxDelta <= 1.0f / ramp * 2.0f);
        // And the ramp is real, not instantaneous-but-small: the biggest step
        // must be about one ramp increment, so at least half of one.
        CHECK(maxDelta >= 0.5f / ramp);
    }

    // --- Squelch: n=0 is safe and preserves state ----------------------------
    {
        cascade::dsp::Squelch sq(48000.0);
        sq.setThresholdDb(-20.0f);
        sq.process(nullptr, 0, nullptr);
        CHECK(!sq.isOpen());
        feedLevel(sq, -15.0, 2000);
        CHECK(sq.isOpen());
        sq.process(nullptr, 0, nullptr);  // n=0 while open: state untouched
        CHECK(sq.isOpen());
    }

    return testSummary("test_squelch");
}
