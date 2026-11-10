// The AGC choice at pipeline level: setters, getters and mirrors, and what the
// modes do to the audio of a running chain.
//
// WHAT IS PROVEN HERE, AND WHAT IS NOT. dsp/agc.hpp's own suite
// (test_agc_modes) proves the loop. This file proves the PIPELINE hands the
// choice to the DSP thread, reads it back from mirrors (no lock), puts the gain
// where the audio is, and publishes the live gain: the built-in signal
// generator (a 1 kHz USB tone) is stepped 20 dB down and the audio level and
// the gain readout are measured through the real chain in each mode.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/pipeline.hpp"
#include "dsp/agc.hpp"
#include "dsp/demod.hpp"
#include "source/siggen.hpp"

#include "test_check.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

using cascade::core::Pipeline;
using cascade::dsp::Agc;

Pipeline::Config testConfig() {
    Pipeline::Config cfg;
    cfg.sampleRateHz = 2000000.0;   // channel rate 200 kHz
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;
    return cfg;
}

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

// Lets `ms` of AUDIO go by (not wall clock: a loaded machine slows both).
void letAudioRun(Pipeline& p, double seconds) {
    const std::uint64_t need = static_cast<std::uint64_t>(seconds * Pipeline::kAudioRateHz);
    const std::uint64_t mark = p.audioSamplesProduced();
    waitFor([&] { return p.audioSamplesProduced() - mark >= need; }, 60000);
}

double audioRms(const Pipeline& p) {
    std::vector<float> buf(4096);
    const std::size_t n = p.audioTap(buf.data(), buf.size());
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) { acc += static_cast<double>(buf[i]) * buf[i]; }
    return n ? std::sqrt(acc / static_cast<double>(n)) : 0.0;
}

void testSettersAndMirrors() {
    Pipeline p(testConfig());
    CHECK(p.agcMode() == Agc::Mode::Auto);
    CHECK(p.agcGainDb() == 0.0f);

    for (Agc::Mode m : {Agc::Mode::Fast, Agc::Mode::Medium, Agc::Mode::Slow, Agc::Mode::Off,
                        Agc::Mode::Auto}) {
        p.setAgcMode(m);
        CHECK(p.agcMode() == m);  // verbatim mirror: read straight after the setter
    }

    // The fixed gain is clamped by the block and reported as in use.
    p.setAgcMode(Agc::Mode::Off);
    p.setAgcFixedGain(12.0f);
    CHECK_NEAR(p.agcFixedGainDb(), 12.0, 1e-3);
    p.setAgcFixedGain(90.0f);
    CHECK_NEAR(p.agcFixedGainDb(), 40.0, 1e-3);
    p.setAgcFixedGain(-5.0f);
    CHECK_NEAR(p.agcFixedGainDb(), 0.0, 1e-3);
    p.setAgcFixedGain(std::nanf(""));
    CHECK_NEAR(p.agcFixedGainDb(), 0.0, 1e-3);
}

void testGettersDoNotWaitForAudioLock() {
    Pipeline p(testConfig());
    std::atomic<bool> held{false};
    std::thread holder([&] { p.holdLockForTest(Pipeline::LockForTest::Audio, 800, &held); });
    waitFor([&] { return held.load(); }, 5000);
    const auto t0 = std::chrono::steady_clock::now();
    p.setAgcMode(Agc::Mode::Slow);            // posts, does not wait
    const Agc::Mode m = p.agcMode();          // mirror
    const float f = p.agcFixedGainDb();
    const float g = p.agcGainDb();
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    (void)f;
    (void)g;
    CHECK(m == Agc::Mode::Slow);
    CHECK(ms < 200.0);
    holder.join();
    // The command ran once the lock was free: the live object has the mode.
    p.setAgcFixedGain(3.0f);
    CHECK(p.agcMode() == Agc::Mode::Slow);
}

struct StepResult {
    double levelBefore = 0.0;
    double levelAfter = 0.0;
    float gainBeforeDb = 0.0f;
    float gainAfterDb = 0.0f;
};

// Runs a pipeline in `mode` on a 1 kHz tone, steps it 20 dB down, and measures
// the audio level and gain readout before and `afterS` audio-seconds after.
StepResult runStep(Agc::Mode mode, double afterS) {
    Pipeline p(testConfig());
    p.setDemodMode(cascade::dsp::DemodMode::USB);
    p.setVfoOffsetHz(0.0);
    p.setVfoBandwidthHz(3000.0);
    p.setSquelchDb(-120.0f);
    p.setAgcMode(mode);
    if (mode == Agc::Mode::Off) { p.setAgcFixedGain(0.0f); }
    p.sigGen().setTone(0, 1000.0, -26.0f);
    p.start();
    letAudioRun(p, 2.5);   // settle (Slow needs the longest)
    StepResult r;
    r.levelBefore = audioRms(p);
    r.gainBeforeDb = p.agcGainDb();
    p.sigGen().setTone(0, 1000.0, -46.0f);
    letAudioRun(p, afterS);
    r.levelAfter = audioRms(p);
    r.gainAfterDb = p.agcGainDb();
    p.stop();
    return r;
}

void testModesOnTheRunningChain() {
    // Off: the level follows the input (-20 dB = x0.1) and the gain never moves.
    {
        const StepResult r = runStep(Agc::Mode::Off, 0.6);
        std::printf("  Off:  level %.5f -> %.5f (ratio %.3f), gain %.2f -> %.2f dB\n",
                    r.levelBefore, r.levelAfter, r.levelAfter / r.levelBefore,
                    r.gainBeforeDb, r.gainAfterDb);
        CHECK(r.levelBefore > 0.0);
        CHECK(r.levelAfter / r.levelBefore > 0.085);
        CHECK(r.levelAfter / r.levelBefore < 0.12);
        CHECK(r.gainBeforeDb == r.gainAfterDb);
        CHECK_NEAR(r.gainAfterDb, 0.0, 1e-3);
    }
    // Fast: back within 3 dB of the old level well inside 1 s, the gain up ~20 dB.
    {
        const StepResult r = runStep(Agc::Mode::Fast, 1.0);
        std::printf("  Fast: level %.5f -> %.5f (ratio %.3f), gain %.2f -> %.2f dB\n",
                    r.levelBefore, r.levelAfter, r.levelAfter / r.levelBefore,
                    r.gainBeforeDb, r.gainAfterDb);
        CHECK(r.levelAfter / r.levelBefore > 0.7);
        CHECK(r.levelAfter / r.levelBefore < 1.42);
        CHECK(r.gainAfterDb - r.gainBeforeDb > 15.0f);
        CHECK(r.gainAfterDb - r.gainBeforeDb < 25.0f);
    }
    // Slow: one second after the step it has not recovered (17 dB takes 2 s).
    {
        const StepResult r = runStep(Agc::Mode::Slow, 1.0);
        std::printf("  Slow: level %.5f -> %.5f (ratio %.3f), gain %.2f -> %.2f dB\n",
                    r.levelBefore, r.levelAfter, r.levelAfter / r.levelBefore,
                    r.gainBeforeDb, r.gainAfterDb);
        CHECK(r.levelAfter / r.levelBefore < 0.6);
        CHECK(r.levelAfter / r.levelBefore > 0.1);
        CHECK(r.gainAfterDb > r.gainBeforeDb);
    }
}

void testFixedGainIsTheGainApplied() {
    // Off with 0 dB against Off with 12 dB: the audio is 12 dB louder.
    double rms0 = 0.0;
    double rms12 = 0.0;
    {
        Pipeline p(testConfig());
        p.setDemodMode(cascade::dsp::DemodMode::USB);
        p.setVfoBandwidthHz(3000.0);
        p.setSquelchDb(-120.0f);
        p.setAgcMode(Agc::Mode::Off);
        p.setAgcFixedGain(0.0f);
        p.sigGen().setTone(0, 1000.0, -40.0f);
        p.start();
        letAudioRun(p, 1.0);
        rms0 = audioRms(p);
        p.setAgcFixedGain(12.0f);
        letAudioRun(p, 0.5);
        rms12 = audioRms(p);
        CHECK_NEAR(p.agcGainDb(), 12.0, 1e-3);
        p.stop();
    }
    const double ratioDb = 20.0 * std::log10(rms12 / rms0);
    std::printf("  fixed gain 0 -> 12 dB: audio moved %.2f dB\n", ratioDb);
    CHECK_NEAR(ratioDb, 12.0, 0.4);
}

}  // namespace

int main() {
    testSettersAndMirrors();
    testGettersDoNotWaitForAudioLock();
    testModesOnTheRunningChain();
    testFixedGainIsTheGainApplied();
    return testSummary("test_pipeline_agc");
}
