// The GUI's per-frame Pipeline getters must not queue behind a DSP block.
//
// WHAT THIS IS FOR. Field report "hang ntdll.dll @
// cascade::core::Pipeline::vfoOffsetHz" (0.96.2, an NESDR SMArt v5 on a machine
// whose audio callback was starving 110 times a minute): the GUI thread sat
// over five seconds in AppWindow::run -> maybeSaveConfig -> currentConfig ->
// Pipeline::vfoOffsetHz -> a std::mutex wait, and the hang watchdog filed a
// report against an application that was behaving exactly as designed.
//
// processAudioBlock() holds audioMutex_ across a WHOLE block, and the GUI reads
// these parameters every frame. On a machine that cannot keep up the blocks run
// back to back, so the render thread convoys behind the audio chain. The fix is
// that every getter the GUI polls reads an atomic mirror instead of taking the
// lock; this test stages the contention that cannot be staged any other way
// (Pipeline::holdLockForTest) and times each getter against it.
//
// TWO HALVES, AND THE SECOND ONE IS WHY THE FIRST IS SAFE. A getter that
// returns a constant would sail through the timing half - so every mirror is
// also driven through its setter and read back, because a mirror that is never
// published is a wrong readout rather than a slow one.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/pipeline.hpp"

#include "test_check.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

using cascade::core::Pipeline;

// How long the staged holder keeps the mutex. Two seconds is comfortably past
// the watchdog's own 5 s / 300-frame reasoning while staying a short test: if a
// getter still takes the lock, it waits essentially all of this.
constexpr int kHoldMs = 2000;

// What "did not wait for the lock" means, in milliseconds. A lock-free read is
// nanoseconds; this bar is loose enough to survive a scheduler hiccup on a busy
// build machine and still 20x below the 2000 ms a blocking getter would take,
// so it cannot pass by luck.
constexpr double kMaxGetterMs = 100.0;

double timeMs(const std::function<void()>& fn) {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

Pipeline::Config testConfig() {
    Pipeline::Config cfg;
    cfg.sampleRateHz = 2000000.0;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;  // no device on a build machine, and none needed
    return cfg;
}

// Every getter AppWindow polls per frame, by name, so a failure says which one
// went back to taking a lock rather than only that one of them did.
struct Getter {
    const char* name;
    std::function<void()> call;
};

std::vector<Getter> auditedGetters(Pipeline& p) {
    return {
        {"vfoOffsetHz", [&p] { (void)p.vfoOffsetHz(); }},
        {"channelRateHz", [&p] { (void)p.channelRateHz(); }},
        {"inputRateHz", [&p] { (void)p.inputRateHz(); }},
        {"demodMode", [&p] { (void)p.demodMode(); }},
        {"deemphasisUs", [&p] { (void)p.deemphasisUs(); }},
        {"stereoEnabled", [&p] { (void)p.stereoEnabled(); }},
        {"noiseReductionEnabled", [&p] { (void)p.noiseReductionEnabled(); }},
        {"noiseReductionStrength", [&p] { (void)p.noiseReductionStrength(); }},
        {"notchEnabled", [&p] { (void)p.notchEnabled(); }},
        {"notchFrequencyHz", [&p] { (void)p.notchFrequencyHz(); }},
        {"notchQ", [&p] { (void)p.notchQ(); }},
        {"autoNotchEnabled", [&p] { (void)p.autoNotchEnabled(); }},
        // Already lock-free before this change; audited anyway, because the
        // status cards read them on the same frame and a future edit that gave
        // one of them a mutex would reopen the same hole.
        {"signalPowerDb", [&p] { (void)p.signalPowerDb(); }},
        {"audioMuted", [&p] { (void)p.audioMuted(); }},
        {"pilotLocked", [&p] { (void)p.pilotLocked(); }},
        {"pilotLevel", [&p] { (void)p.pilotLevel(); }},
        {"stereoActive", [&p] { (void)p.stereoActive(); }},
        {"autoNotchEngaged", [&p] { (void)p.autoNotchEngaged(); }},
        {"autoNotchFrequencyHz", [&p] { (void)p.autoNotchFrequencyHz(); }},
        {"running", [&p] { (void)p.running(); }},
        {"faulted", [&p] { (void)p.faulted(); }},
    };
}

// Holds one of the pipeline's internal mutexes for kHoldMs and runs `body`
// while it is genuinely held - not after a sleep the test guessed at.
void underHeldLock(Pipeline& p, Pipeline::LockForTest which,
                   const std::function<void()>& body) {
    std::atomic<bool> acquired{false};
    std::thread holder([&p, which, &acquired] {
        p.holdLockForTest(which, kHoldMs, &acquired);
    });
    while (!acquired.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    body();
    holder.join();
}

void checkGettersDoNotBlock() {
    Pipeline p(testConfig());

    // THE DSP-SIDE MUTEX: the one processAudioBlock holds across a whole block.
    underHeldLock(p, Pipeline::LockForTest::Audio, [&p] {
        double worst = 0.0;
        const char* worstName = "(none)";
        for (const Getter& g : auditedGetters(p)) {
            const double ms = timeMs(g.call);
            if (ms > worst) {
                worst = ms;
                worstName = g.name;
            }
            if (ms > kMaxGetterMs) {
                std::printf("FAIL getter %s waited %.1f ms for the DSP mutex\n", g.name, ms);
            }
            CHECK(ms <= kMaxGetterMs);
        }
        std::printf("audioMutex held: slowest getter %s at %.3f ms\n", worstName, worst);
    });

    // THE CONTROL-PLANE MUTEX, which start()/stop()/setSource() hold for as
    // long as a vendor driver takes to give a source thread back. inputRateHz()
    // used to take it, thirteen times a frame.
    underHeldLock(p, Pipeline::LockForTest::Control, [&p] {
        const double ms = timeMs([&p] { (void)p.inputRateHz(); });
        std::printf("controlMutex held: inputRateHz at %.3f ms\n", ms);
        CHECK(ms <= kMaxGetterMs);
    });
}

// A mirror that is never published reads fast and reads WRONG, which is the
// failure this half exists to catch. Every setter that can move a mirrored
// value is driven, and the getter is read back.
void checkMirrorsFollowTheSetters() {
    Pipeline p(testConfig());

    // The construction defaults first: a mirror that was never seeded would
    // answer zero here while the chain answered something else.
    CHECK_NEAR(p.inputRateHz(), 2000000.0, 1e-9);
    CHECK(p.channelRateHz() > 0.0);
    CHECK(p.demodMode() == cascade::dsp::DemodMode::WFM);
    CHECK_NEAR(p.deemphasisUs(), 50.0, 1e-9);
    CHECK(p.stereoEnabled());
    CHECK(!p.noiseReductionEnabled());
    CHECK(!p.notchEnabled());
    CHECK(!p.autoNotchEnabled());
    CHECK(p.notchFrequencyHz() > 0.0);
    CHECK(p.notchQ() > 0.0);

    p.setVfoOffsetHz(123456.0);
    CHECK_NEAR(p.vfoOffsetHz(), 123456.0, 1e-6);
    p.setVfoOffsetHz(-7000.0);
    CHECK_NEAR(p.vfoOffsetHz(), -7000.0, 1e-6);

    p.setDemodMode(cascade::dsp::DemodMode::AM);
    CHECK(p.demodMode() == cascade::dsp::DemodMode::AM);

    p.setDeemphasisUs(75.0);
    CHECK_NEAR(p.deemphasisUs(), 75.0, 1e-9);

    p.setStereoEnabled(false);
    CHECK(!p.stereoEnabled());

    p.setNoiseReductionEnabled(true);
    CHECK(p.noiseReductionEnabled());
    p.setNoiseReductionStrength(0.5f);
    CHECK_NEAR(p.noiseReductionStrength(), 0.5, 1e-5);

    p.setNotchEnabled(true);
    CHECK(p.notchEnabled());
    p.setNotchFrequencyHz(1000.0);
    CHECK_NEAR(p.notchFrequencyHz(), 1000.0, 1.0);
    p.setNotchQ(12.0);
    CHECK_NEAR(p.notchQ(), 12.0, 1e-6);

    p.setAutoNotchEnabled(true);
    CHECK(p.autoNotchEnabled());

    // A RATE SWITCH REBUILDS THE CHAIN, so both derived mirrors move with it.
    // 1.92 MS/s decimates by 10 to a 192 kHz channel (integral, accepted).
    const double chanBefore = p.channelRateHz();
    CHECK(p.setInputRateHz(1920000.0));
    CHECK_NEAR(p.inputRateHz(), 1920000.0, 1e-9);
    CHECK_NEAR(p.channelRateHz(), 192000.0, 1e-6);
    CHECK(p.channelRateHz() != chanBefore);
    // ...and the tuning and mode survive it, exactly as they did when the
    // getters read the live objects.
    CHECK_NEAR(p.vfoOffsetHz(), -7000.0, 1e-6);
    CHECK(p.demodMode() == cascade::dsp::DemodMode::AM);

    // A REFUSED rate change must leave every mirror alone: 12345.6 Hz is not an
    // integral-decimation rate, so nothing may move.
    CHECK(!p.setInputRateHz(12345.6));
    CHECK_NEAR(p.inputRateHz(), 1920000.0, 1e-9);
    CHECK_NEAR(p.channelRateHz(), 192000.0, 1e-6);
}

}  // namespace

int main() {
    checkGettersDoNotBlock();
    checkMirrorsFollowTheSetters();
    return testSummary("test_pipeline_getters");
}
