// The GUI's Pipeline SETTERS must not queue behind the DSP thread.
//
// WHAT THIS IS FOR. Two field freezes on 0.99.81, both the hang watchdog catching the GUI
// thread parked in a std::mutex wait inside a Pipeline setter for over five seconds:
//
//   hang Pipeline::resetRds        a retune from the toolbar readout (applyRetuneNow), 1710 s
//                                  in, an RTL-SDR on NFM with ~40 decoder plugins loaded.
//   hang Pipeline::setDeemphasisUs a plugin preset (applyPluginPreset) that woke ~28 decoders,
//                                  an RTL-SDR at 2.4 MS/s on an old machine.
//
// processAudioBlock() holds audioMutex_ across a whole block, INCLUDING every I/Q decoder
// plugin's process() (PluginRunner::processIq runs them all under it). On a machine that does
// not keep up the DSP thread runs blocks back to back and the mutex is held nearly all of the
// time; std::mutex is not fair, so a GUI setter can lose the race to the DSP thread's re-lock
// again and again. tests/test_pipeline_getters.cpp already pins that the per-frame GETTERS do
// not take the lock; this pins the same for the SETTERS, which cannot read an atomic: they have
// to change the chain, so they post the change for the DSP thread to apply at its next block
// and return.
//
// THREE HALVES.
//   1. The staged hold (Pipeline::holdLockForTest) the getters test uses: every setter is
//      timed against a lock held for two seconds. A setter that still takes the lock waits
//      essentially all of it.
//   2. A REAL saturated DSP thread: a running pipeline whose I/Q decoder spends 150 ms in
//      every process() call, so the thread is inside audioMutex_ virtually all the time - the
//      field condition, produced by the same mechanism. The setters are timed in a loop.
//   3. What was posted must HAPPEN, in order: once the contention ends, the live chain carries
//      the last value of every setter (read back through the figures only the live objects can
//      supply - the clamped ones - so a command that was queued and never applied cannot pass).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/pipeline.hpp"

#include "core/plugin_host.hpp"
#include "core/plugin_runner.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

using cascade::core::LoadedPlugin;
using cascade::core::Pipeline;
using cascade::core::PluginRunner;
using cascade::dsp::DemodMode;

// How long the staged holder keeps the DSP mutex, ONE HOLD PER SETTER: a setter that still takes
// the lock waits essentially all of it, and with one shared hold the first offender would eat it
// and every later setter be timed after the lock had gone (the same trap
// tests/test_pipeline_getters.cpp documents).
constexpr int kHoldMs = 500;

// "Did not wait for the lock", in milliseconds. A posted setter is microseconds; this is loose
// enough to survive a scheduler hiccup on a busy build machine and 5x under the hold.
constexpr double kMaxSetterMs = 100.0;

// The saturated half: how long the fake decoder spends in each process() call, and the bar a
// setter has to beat. A setter that waited for the block would take 0..150 ms (mean 75), so a
// run of a few hundred calls cannot stay under 60 ms by luck.
constexpr int kBlockMs = 150;
constexpr double kMaxSaturatedSetterMs = 60.0;

double timeMs(const std::function<void()>& fn) {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

Pipeline::Config testConfig() {
    Pipeline::Config cfg;
    cfg.sampleRateHz = 1000000.0;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;  // no device on a build machine, and none needed
    return cfg;
}

template <typename Fn>
bool waitFor(Fn ready, int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return ready();
}

// One setter call, by name, so a failure says WHICH setter went back to taking the lock.
// `round` alternates the value so no call is a no-op the setter could return early on.
struct Setter {
    const char* name;
    std::function<void(Pipeline&, int round)> call;
};

std::vector<Setter> auditedSetters() {
    return {
        {"setDeemphasisUs", [](Pipeline& p, int r) { p.setDeemphasisUs(r % 2 ? 75.0 : 50.0); }},
        {"resetRds", [](Pipeline& p, int) { p.resetRds(); }},
        {"setDemodMode",
         [](Pipeline& p, int r) { p.setDemodMode(r % 2 ? DemodMode::NFM : DemodMode::AM); }},
        {"setVfoOffsetHz", [](Pipeline& p, int r) { p.setVfoOffsetHz(1000.0 * (r % 7)); }},
        {"setVfoBandwidthHz",
         [](Pipeline& p, int r) { p.setVfoBandwidthHz(r % 2 ? 12500.0 : 25000.0); }},
        {"setSquelchDb", [](Pipeline& p, int r) { p.setSquelchDb(r % 2 ? -40.0f : -50.0f); }},
        {"setStereoEnabled", [](Pipeline& p, int r) { p.setStereoEnabled(r % 2 == 0); }},
        {"setNoiseReductionEnabled",
         [](Pipeline& p, int r) { p.setNoiseReductionEnabled(r % 2 == 0); }},
        {"setNoiseReductionStrength",
         [](Pipeline& p, int r) { p.setNoiseReductionStrength(r % 2 ? 0.3f : 0.6f); }},
        {"setNotchEnabled", [](Pipeline& p, int r) { p.setNotchEnabled(r % 2 == 0); }},
        {"setNotchFrequencyHz",
         [](Pipeline& p, int r) { p.setNotchFrequencyHz(r % 2 ? 1000.0 : 1500.0); }},
        {"setNotchQ", [](Pipeline& p, int r) { p.setNotchQ(r % 2 ? 8.0 : 12.0); }},
        {"setAutoNotchEnabled", [](Pipeline& p, int r) { p.setAutoNotchEnabled(r % 2 == 0); }},
        {"publishAudioChannels", [](Pipeline& p, int r) { p.publishAudioChannels(r % 2 == 0); }},
    };
}

// --- 1. the staged hold ---------------------------------------------------------------------

// Holds the DSP-side mutex for kHoldMs and runs `body` while it is genuinely held.
void underHeldLock(Pipeline& p, const std::function<void()>& body) {
    std::atomic<bool> acquired{false};
    std::thread holder([&p, &acquired] {
        p.holdLockForTest(Pipeline::LockForTest::Audio, kHoldMs, &acquired);
    });
    while (!acquired.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    body();
    holder.join();
}

void checkSettersDoNotBlockOnAHeldLock() {
    Pipeline p(testConfig());
    double worst = 0.0;
    const char* worstName = "(none)";
    int round = 1;
    for (const Setter& s : auditedSetters()) {
        underHeldLock(p, [&] {
            const double ms = timeMs([&] { s.call(p, round); });
            if (ms > worst) {
                worst = ms;
                worstName = s.name;
            }
            if (ms > kMaxSetterMs) {
                std::printf("FAIL setter %s waited %.1f ms for the DSP mutex\n", s.name, ms);
            }
            CHECK(ms <= kMaxSetterMs);
        });
        ++round;
    }
    std::printf("audioMutex held: slowest setter %s at %.3f ms\n", worstName, worst);
}

// WHAT WAS POSTED MUST HAPPEN, IN ORDER, with no DSP thread at all: the next setter after the
// lock goes (the fast path, where the lock is free) applies everything that queued behind it.
// The figures read back are the ones the live objects CLAMP and that are therefore published
// only when a command has run, never stored at post time - a command that was queued and
// dropped cannot pass.
void checkPostedChangesAreAppliedInOrder() {
    Pipeline p(testConfig());
    const double q0 = p.notchQ();
    const double hz0 = p.notchFrequencyHz();
    const float strength0 = p.noiseReductionStrength();

    underHeldLock(p, [&] {
        p.setNotchQ(5.0);
        p.setNotchQ(9.0);                    // the later one must win
        p.setNotchFrequencyHz(1.0e9);        // beyond Nyquist: the notch clamps it
        p.setNoiseReductionStrength(2.0f);   // beyond 1: the block clamps it
        // Nothing has run: the figures only the live objects can supply are untouched.
        CHECK(p.commandsPendingForTest());
        CHECK_NEAR(p.notchQ(), q0, 1e-9);
        CHECK_NEAR(p.notchFrequencyHz(), hz0, 1e-9);
        CHECK_NEAR(p.noiseReductionStrength(), strength0, 1e-6);
        // ...while the verbatim ones read back at once, so "set, then read" holds.
        p.setVfoOffsetHz(-12500.0);
        p.setDemodMode(DemodMode::AM);
        p.setDeemphasisUs(75.0);
        CHECK_NEAR(p.vfoOffsetHz(), -12500.0, 1e-9);
        CHECK(p.demodMode() == DemodMode::AM);
        CHECK_NEAR(p.deemphasisUs(), 75.0, 1e-9);
    });
    CHECK(p.commandsPendingForTest());  // nobody has drained it yet: no DSP thread exists

    p.resetRds();  // the lock is free now, so this call applies the whole queue
    CHECK(!p.commandsPendingForTest());
    CHECK_NEAR(p.notchQ(), 9.0, 1e-6);
    CHECK(p.notchFrequencyHz() > 0.0);
    CHECK(p.notchFrequencyHz() < 1.0e9);
    CHECK(p.notchFrequencyHz() != hz0);
    CHECK_NEAR(p.noiseReductionStrength(), 1.0, 1e-6);
    CHECK_NEAR(p.vfoOffsetHz(), -12500.0, 1e-9);
    CHECK(p.demodMode() == DemodMode::AM);
    CHECK_NEAR(p.deemphasisUs(), 75.0, 1e-9);
}

// --- 2. a real, saturated DSP thread --------------------------------------------------------

// A decoder that is slow on every block, as thirty of them are on a slow machine. It is a stand-in
// for the load, not for any one plugin; it decodes nothing.
void* createSlow(double, double) {
    static int token = 0;
    return &token;
}
void slowProcess(void*, const float*, std::size_t) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kBlockMs));
}
std::int32_t slowPollText(void*, char*, std::size_t) { return 0; }
void slowDestroy(void*) {}

void checkSettersDoNotBlockOnASaturatedDspThread() {
    CascadeIqDecoderApi iq{};
    iq.structSize = static_cast<std::uint32_t>(sizeof(CascadeIqDecoderApi));
    iq.requiredRateHz = 0.0;
    iq.preferredRateHz = 0.0;
    iq.create = &createSlow;
    iq.process = &slowProcess;
    iq.retune = nullptr;
    iq.poll_text = &slowPollText;
    iq.destroy = &slowDestroy;
    LoadedPlugin lp;
    lp.loaded = true;
    lp.name = "Slow decoder";
    lp.path = "C:/plugins/slow.dll";
    lp.version = "1.0.0";
    lp.capabilities = CASCADE_CAP_IQ_DECODER;
    lp.iqDecoder = &iq;

    const Pipeline::Config cfg = testConfig();
    Pipeline p(cfg);
    PluginRunner runner;
    runner.rebuild({lp}, Pipeline::kAudioRateHz, cfg.sampleRateHz, 100.0e6);
    p.setPluginRunner(&runner);
    p.start();

    // The DSP thread is genuinely inside the slow decoder, not merely started: its block counter
    // moves, and moves slowly.
    CHECK(waitFor([&] { return p.audioSamplesProduced() > 0; }, 30000));

    double worst = 0.0;
    const char* worstName = "(none)";
    int calls = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    int round = 0;
    while (std::chrono::steady_clock::now() < until) {
        for (const Setter& s : auditedSetters()) {
            const double ms = timeMs([&] { s.call(p, round); });
            ++calls;
            if (ms > worst) {
                worst = ms;
                worstName = s.name;
            }
            if (ms > kMaxSaturatedSetterMs) {
                std::printf("FAIL setter %s waited %.1f ms behind a %d ms DSP block\n", s.name, ms,
                            kBlockMs);
            }
            CHECK(ms <= kMaxSaturatedSetterMs);
        }
        ++round;
        // A GUI frame's worth of gap, so the loop is a user's hand and not a spin.
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    std::printf("saturated DSP (%d ms/block): %d setter calls, slowest %s at %.3f ms\n", kBlockMs,
                calls, worstName, worst);

    p.stop();
    p.setPluginRunner(nullptr);
}

}  // namespace

int main() {
    checkSettersDoNotBlockOnAHeldLock();
    checkPostedChangesAreAppliedInOrder();
    checkSettersDoNotBlockOnASaturatedDspThread();
    return testSummary("test_pipeline_setters");
}
