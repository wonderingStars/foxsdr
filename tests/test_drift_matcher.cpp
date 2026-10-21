// Tests for sink/drift_matcher.hpp.
//
// Three layers:
//   1. The resampler on its own: a ratio of exactly 1 is a pure two-frame
//      delay, any block split gives the same stream, and a tone through a
//      non-unity ratio comes out at the scaled frequency without distortion.
//   2. The controller on its own: it holds while the sink is not playing,
//      and its correction and learned drift stay inside their caps.
//   3. THE TESTER'S FAULT, closed loop: a radio clock and a sound-card clock
//      run against the REAL AudioOut ring (write + pullBlock, the exact code
//      the PortAudio callback runs), once with the matcher frozen at 1:1 -
//      which is FoxSDR before this fix - and once with it steering. Frozen,
//      a slow radio and a run of lossy hiccups both starve the ring and a
//      fast radio overflows it; steered, none of them do.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "sink/drift_matcher.hpp"

#include "sink/audio_out.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "test_check.hpp"

using cascade::sink::AudioOut;
using cascade::sink::DriftMatcher;

namespace {

constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

// Fixed-seed LCG - deterministic data in [-1, 1).
std::uint32_t g_lcg = 0x2468ACE1u;
float nextSample() {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return static_cast<float>(g_lcg >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

// Pushes `in` through `dm` at whatever ratio it currently holds, in blocks
// of `block` frames.
std::vector<float> runStream(const std::vector<float>& in, std::size_t channels,
                             std::size_t block, DriftMatcher& dm) {
    std::vector<float> out;
    const std::size_t frames = in.size() / channels;
    std::vector<float> buf(DriftMatcher::maxOut(block) * channels);
    for (std::size_t f = 0; f < frames; f += block) {
        const std::size_t n = (frames - f < block) ? frames - f : block;
        const std::size_t got = dm.process(in.data() + f * channels, n, channels,
                                           buf.data(), DriftMatcher::maxOut(n));
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(got * channels));
    }
    return out;
}

// Drives the controller until its correction is steady at the cap: a ring
// that is persistently empty asks for everything the matcher may give.
void forceMaxRatio(DriftMatcher& dm) {
    for (int i = 0; i < 20000; ++i) { dm.observe(0, true, 480); }
}

// The closed loop. A radio producing `blockFrames` at kRate*(1+radioPpm) and
// a sound card taking `periodFrames` at kRate*(1+cardPpm), interleaved in
// simulated time against a real AudioOut. `lossEveryS` > 0 throws away
// `lossMs` of the radio's output at that interval (a USB stall that dropped
// I/Q). `steer` false freezes the matcher at 1:1 - the old FoxSDR.
//
// `leadMs` is the sink's lead (0.99.73: AudioOut::setLeadFrames; 120 is the
// default). `followLead` true hands the matcher the sink's own target, which is
// what the application does (AudioOut::targetFrames, the lead plus 40 ms); false
// hands it the OLD fixed 160 ms whatever the lead - the matcher as it was before
// the lead was adjustable, which bleeds a deeper lead back down.
struct LoopResult {
    std::uint64_t underruns = 0;
    std::uint64_t droppedFrames = 0;  // producer frames the full ring refused
    double finalFillMs = 0.0;
    double driftPpm = 0.0;
};

LoopResult runLoop(double seconds, double radioPpm, double cardPpm, double lossEveryS,
                   double lossMs, bool steer, int leadMs = 120, bool followLead = true) {
    AudioOut ao;  // never opened: mono, and pullBlock is driven by hand
    ao.setLeadFrames(cascade::sink::audioLeadFrames(leadMs));
    DriftMatcher dm(kRate);
    constexpr std::size_t kBlock = 1024;   // a demod block
    constexpr std::size_t kPeriod = 480;   // a 10 ms device period
    const double producerDt = kBlock / (kRate * (1.0 + radioPpm * 1e-6));
    const double consumerDt = kPeriod / (kRate * (1.0 + cardPpm * 1e-6));
    double tProd = 0.0;
    double tCons = 0.0;
    double nextLoss = lossEveryS > 0.0 ? lossEveryS : 1e300;
    std::vector<float> block(kBlock, 0.25f);
    std::vector<float> matched(DriftMatcher::maxOut(kBlock));
    std::vector<float> dev(kPeriod);
    LoopResult r;
    double skipFrames = 0.0;
    while (tProd < seconds || tCons < seconds) {
        if (tProd <= tCons) {
            tProd += producerDt;
            if (tProd >= nextLoss) {
                skipFrames += lossMs * 1e-3 * kRate;
                nextLoss += lossEveryS;
            }
            if (skipFrames >= static_cast<double>(kBlock)) {
                skipFrames -= static_cast<double>(kBlock);  // this block never arrived
                continue;
            }
            if (steer) {
                dm.observe(ao.ringFrames(), ao.primed(), kBlock,
                           followLead ? static_cast<double>(ao.targetFrames())
                                      : DriftMatcher::kTargetFrames);
            }
            const std::size_t m =
                dm.process(block.data(), kBlock, 1, matched.data(), matched.size());
            const std::size_t took = ao.write(matched.data(), m);
            r.droppedFrames += m - took;
        } else {
            tCons += consumerDt;
            AudioOut::pullBlock(&ao, dev.data(), kPeriod);
        }
    }
    r.underruns = ao.underruns();
    r.finalFillMs = 1000.0 * static_cast<double>(ao.ringFrames()) / kRate;
    r.driftPpm = dm.driftPpm();
    return r;
}

}  // namespace

int main() {
    // --- 1a. Ratio 1 is a pure two-frame delay, mono and stereo ----------------
    for (std::size_t ch : {std::size_t{1}, std::size_t{2}}) {
        std::vector<float> in(4000 * ch);
        for (auto& s : in) { s = nextSample(); }
        DriftMatcher dm;
        CHECK(dm.ratio() == 1.0);
        const std::vector<float> out = runStream(in, ch, 256, dm);
        CHECK(out.size() == in.size());
        bool exact = out.size() == in.size();
        for (std::size_t c = 0; c < ch && exact; ++c) {
            if (out[c] != 0.0f || out[ch + c] != 0.0f) { exact = false; }
        }
        for (std::size_t i = 2 * ch; i < out.size() && exact; ++i) {
            if (out[i] != in[i - 2 * ch]) { exact = false; }
        }
        CHECK(exact);
    }

    // --- 1b. Block split does not change the stream at a non-unity ratio -------
    {
        std::vector<float> in(2 * 9000);
        for (auto& s : in) { s = nextSample(); }
        DriftMatcher a;
        DriftMatcher b;
        forceMaxRatio(a);
        forceMaxRatio(b);
        CHECK_NEAR(a.correctionPpm(), DriftMatcher::kMaxCorrectionPpm, 1e-9);
        const std::vector<float> one = runStream(in, 2, 9000, a);
        const std::vector<float> many = runStream(in, 2, 37, b);
        CHECK(one.size() == many.size());
        bool same = one.size() == many.size();
        for (std::size_t i = 0; i < one.size() && same; ++i) {
            if (std::fabs(one[i] - many[i]) > 1e-6f) { same = false; }
        }
        CHECK(same);
        // At +5000 ppm, 9000 frames in give ~45 extra frames out.
        CHECK_NEAR(static_cast<double>(one.size() / 2), 9000.0 * 1.005, 3.0);
    }

    // --- 1c. A tone keeps its shape and scales its frequency -------------------
    {
        // 1 kHz in; at ratio r the output is the same waveform sampled every
        // 1/r input frames, i.e. a (1000 / r) Hz tone at 48 kHz, delayed by
        // the two history frames.
        DriftMatcher dm;
        forceMaxRatio(dm);
        const double r = dm.ratio();
        std::vector<float> in(48000);
        for (std::size_t i = 0; i < in.size(); ++i) {
            in[i] = static_cast<float>(std::sin(2.0 * kPi * 1000.0 * i / kRate));
        }
        const std::vector<float> out = runStream(in, 1, 480, dm);
        double worst = 0.0;
        for (std::size_t j = 100; j + 100 < out.size(); ++j) {
            const double pos = static_cast<double>(j) / r - 2.0;  // input frame
            const double ideal = std::sin(2.0 * kPi * 1000.0 * pos / kRate);
            worst = std::fmax(worst, std::fabs(out[j] - ideal));
        }
        // Cubic Hermite at 1 kHz / 48 kHz is good to about -75 dB.
        CHECK(worst < 2e-4);
        std::printf("tone through +%.0f ppm: worst error %.2e\n", dm.correctionPpm(), worst);
    }

    // --- 2a. Not playing: hold the learned drift, drop the push ----------------
    {
        DriftMatcher dm;
        for (int i = 0; i < 2000; ++i) { dm.observe(0, true, 480); }
        const double learned = dm.driftPpm();
        CHECK(learned > 0.0);
        dm.observe(0, false, 480);
        CHECK(dm.correctionPpm() == learned);
        for (int i = 0; i < 100; ++i) { dm.observe(0, false, 480); }
        CHECK(dm.driftPpm() == learned);
        dm.reset();
        CHECK(dm.ratio() == 1.0);
        CHECK(dm.driftPpm() == 0.0);
    }

    // --- 2b. Caps: correction, and the integral separately ---------------------
    {
        DriftMatcher dm;
        for (int i = 0; i < 200000; ++i) { dm.observe(40000, true, 480); }  // far too full
        CHECK_NEAR(dm.correctionPpm(), -DriftMatcher::kMaxCorrectionPpm, 1e-9);
        CHECK_NEAR(dm.driftPpm(), -DriftMatcher::kMaxDriftPpm, 1e-9);
    }

    // --- 3. The tester's fault, closed loop --------------------------------------
    //
    // Each case runs frozen (the old behaviour) and steered. The frozen runs
    // are the proof the scenario is a real fault; the steered runs are the fix.
    {
        // A radio 300 ppm slow against the sound card, 20 minutes.
        const LoopResult oldR = runLoop(1200.0, -300.0, 0.0, 0.0, 0.0, false);
        const LoopResult newR = runLoop(1200.0, -300.0, 0.0, 0.0, 0.0, true);
        std::printf("slow radio: frozen %llu underruns, steered %llu (fill %.1f ms, drift %+.0f ppm)\n",
                    static_cast<unsigned long long>(oldR.underruns),
                    static_cast<unsigned long long>(newR.underruns), newR.finalFillMs,
                    newR.driftPpm);
        CHECK(oldR.underruns >= 2);
        CHECK(newR.underruns == 0);
        CHECK_NEAR(newR.finalFillMs, 160.0, 15.0);
        CHECK_NEAR(newR.driftPpm, 300.0, 30.0);
    }
    {
        // Clocks agree; every 45 s a hiccup loses 80 ms of I/Q (the bench
        // RTL's step), 10 minutes.
        const LoopResult oldR = runLoop(600.0, 0.0, 0.0, 45.0, 80.0, false);
        const LoopResult newR = runLoop(600.0, 0.0, 0.0, 45.0, 80.0, true);
        std::printf("80 ms losses: frozen %llu underruns, steered %llu (fill %.1f ms)\n",
                    static_cast<unsigned long long>(oldR.underruns),
                    static_cast<unsigned long long>(newR.underruns), newR.finalFillMs);
        CHECK(oldR.underruns >= 4);
        CHECK(newR.underruns == 0);
    }
    {
        // A radio 600 ppm FAST: a frozen sink fills its mono ring and then
        // throws audio away on every write. The ring was 682 ms until 0.99.73,
        // which filled in under 16 minutes; it is 2.73 s since the lead became
        // adjustable (131072 mono frames), so the same fault needs 100 simulated
        // minutes to show: 600 ppm adds 3.6 s, and the frozen ring is full after
        // about 73 of them.
        const LoopResult oldR = runLoop(6000.0, 600.0, 0.0, 0.0, 0.0, false);
        const LoopResult newR = runLoop(6000.0, 600.0, 0.0, 0.0, 0.0, true);
        std::printf("fast radio: frozen dropped %llu frames, steered %llu (fill %.1f ms, drift %+.0f ppm)\n",
                    static_cast<unsigned long long>(oldR.droppedFrames),
                    static_cast<unsigned long long>(newR.droppedFrames), newR.finalFillMs,
                    newR.driftPpm);
        CHECK(oldR.droppedFrames > 0);
        CHECK(newR.droppedFrames == 0);
        CHECK(newR.underruns == 0);
        CHECK_NEAR(newR.finalFillMs, 160.0, 15.0);
        CHECK_NEAR(newR.driftPpm, -600.0, 40.0);
    }

    // --- 4. The target follows the lead (0.99.73) ---------------------------------
    //
    // The sink's lead is adjustable (AudioOut::setLeadFrames) and a matcher still
    // steering to the old fixed 160 ms would bleed a deeper lead back down: the
    // buffer the application had just deepened would be gone within a minute or
    // two. The target is the sink's own (its lead plus 40 ms), handed over with
    // every observation.
    {
        // The controller: the same fill is "just right" against its own target and
        // "far too full" against the default one, and the default argument is the
        // old 160 ms.
        auto pushAfterHalfASecond = [](double fillFrames, double target) {
            DriftMatcher dm;
            for (int i = 0; i < 50; ++i) {
                dm.observe(static_cast<std::size_t>(fillFrames), true, 480, target);
            }
            return dm.correctionPpm();
        };
        CHECK(pushAfterHalfASecond(13440.0, 13440.0) == 0.0);                    // at its own target
        CHECK(pushAfterHalfASecond(13440.0, DriftMatcher::kTargetFrames) < -100.0);   // ...too full for 160 ms
        CHECK(pushAfterHalfASecond(7680.0, 13440.0) > 100.0);                    // 160 ms is short of 280 ms
        CHECK(pushAfterHalfASecond(7680.0, 7680.0) == 0.0);
        DriftMatcher defaults;
        for (int i = 0; i < 50; ++i) { defaults.observe(7680, true, 480); }       // no fourth argument
        CHECK(defaults.correctionPpm() == 0.0);
        CHECK(DriftMatcher::kTargetFrames == 7680.0);
        // The target of a sink at each rung is its lead plus 40 ms.
        for (const int ms : {120, 240, 480, 960}) {
            AudioOut ao;
            ao.setLeadFrames(cascade::sink::audioLeadFrames(ms));
            CHECK(ao.targetFrames() == cascade::sink::audioLeadFrames(ms) + 1920u);
        }
    }
    {
        // THE CLOSED LOOP at a 240 ms lead: a radio 300 ppm slow, 20 minutes. The
        // fill settles at 280 ms (the lead plus 40), not at the 160 of a matcher that
        // does not follow the lead - which is what the second run, the old target on
        // the same ring, is here to show.
        const LoopResult r = runLoop(1200.0, -300.0, 0.0, 0.0, 0.0, true, 240, true);
        const LoopResult bled = runLoop(1200.0, -300.0, 0.0, 0.0, 0.0, true, 240, false);
        std::printf("240 ms lead, slow radio: following the lead %.1f ms, %llu underruns; "
                    "the old fixed target %.1f ms\n",
                    r.finalFillMs, static_cast<unsigned long long>(r.underruns), bled.finalFillMs);
        CHECK(r.underruns == 0);
        CHECK_NEAR(r.finalFillMs, 280.0, 15.0);
        CHECK_NEAR(r.driftPpm, 300.0, 30.0);
        CHECK_NEAR(bled.finalFillMs, 160.0, 15.0);   // the old target bleeds 240 ms back down
    }
    {
        // The same at the other rungs: 480 ms settles at 520, and the deepest, 960 ms,
        // at 1000 with a fast radio (the ring takes it without a drop).
        const LoopResult mid = runLoop(1200.0, -300.0, 0.0, 0.0, 0.0, true, 480, true);
        CHECK(mid.underruns == 0);
        CHECK_NEAR(mid.finalFillMs, 520.0, 15.0);
        const LoopResult top = runLoop(1200.0, 600.0, 0.0, 0.0, 0.0, true, 960, true);
        std::printf("960 ms lead, fast radio: fill %.1f ms, dropped %llu frames, drift %+.0f ppm\n",
                    top.finalFillMs, static_cast<unsigned long long>(top.droppedFrames), top.driftPpm);
        CHECK(top.droppedFrames == 0);
        CHECK(top.underruns == 0);
        CHECK_NEAR(top.finalFillMs, 1000.0, 20.0);
        CHECK_NEAR(top.driftPpm, -600.0, 40.0);
    }
    {
        // A hiccup at a deeper lead is rebuilt to the DEEPER target: 80 ms lost every 45 s,
        // 10 minutes, 240 ms lead - no underrun, and the fill is back near 280 ms.
        // The last loss is 15 s before the end, so the fill may still be recovering
        // (an 80 ms loss decays with a 20 s time constant: 38 ms short at worst) -
        // hence a band, not a point: well above the 160 ms of the old target.
        const LoopResult r = runLoop(600.0, 0.0, 0.0, 45.0, 80.0, true, 240, true);
        CHECK(r.underruns == 0);
        CHECK(r.finalFillMs > 220.0 && r.finalFillMs < 320.0);
    }

    return testSummary("test_drift_matcher");
}
