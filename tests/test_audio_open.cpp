// An audio device that will not open must not take the application with it.
//
// THE FIELD REPORT THIS EXISTS FOR. "hang ntdll.dll @ InitializeWaveHandles"
// (0.96.4, Windows 11 26200, an RTL-SDR Blog V4, 350 s uptime). The user picked
// an output device in the Sinks panel and the GUI thread went
//
//   AppWindow::drawSinksSection -> Pipeline::openAudioDevice
//     -> sink::AudioOut::open -> Pa_OpenStream -> InitializeWaveHandles -> ntdll
//
// and stayed there. Pa_OpenStream is waveOutOpen on the WMME host API and it
// has no timeout; the watchdog filed a hang at five seconds and the
// application's own log records the frame loop coming back 57 seconds later.
//
// WHAT IS TESTED, and at which level. The blocking call itself is the operating
// system's and no test can stage it - so gui::AudioOpen takes the opener as a
// parameter, and an opener that sleeps IS the field's wedged driver as far as
// the frame loop can tell. The first block below drives a REAL HangWatchdog
// exactly as the frame loop does: heartbeat, request a device that takes 2.5
// seconds, keep beating. Against the synchronous call this replaced
// (`pipeline_.openAudioDevice(dev.index)` straight from the combo, which is
// what start() + a bare future_.get() amounts to) the loop stops beating, the
// watchdog writes a report and reportsWritten() is 1 - the user's fault,
// reproduced. That check is the red one; the rest of this file holds the
// properties the fix must not lose while achieving it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/audio_open.hpp"

#include "core/hang_watchdog.hpp"
#include "core/pipeline.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::HangWatchdog;
using cascade::gui::AudioOpen;

namespace {

fs::path scratchDir(const std::string& tag) {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = 0;
#endif
    return base / (std::string("cascade-audio-open-") + tag + "-" + std::to_string(pid));
}

double nowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// An opener that behaves like a driver: it takes `blockMs` to answer, records
// what it was asked for, and refuses to be running twice at once - which is the
// property that makes a second concurrent waveOutOpen on one AudioOut
// detectable rather than merely unlikely.
struct FakeDriver {
    std::atomic<int> blockMs{0};
    std::atomic<bool> answer{true};
    std::atomic<int> calls{0};
    std::atomic<int> concurrent{0};
    std::atomic<int> maxConcurrent{0};
    std::atomic<int> lastDevice{-99};

    AudioOpen::Opener opener() {
        return [this](int deviceIndex) {
            const int live = concurrent.fetch_add(1) + 1;
            int seen = maxConcurrent.load();
            while (live > seen && !maxConcurrent.compare_exchange_weak(seen, live)) {}
            lastDevice.store(deviceIndex);
            calls.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(blockMs.load()));
            concurrent.fetch_sub(1);
            return answer.load();
        };
    }
};

// --- 1. THE FIELD HANG ------------------------------------------------------
//
// A frame loop, a real watchdog, and a device that takes 2.5 s to answer
// against a 800 ms threshold. The loop must keep beating and the watchdog must
// write nothing.
void checkBlockingOpenDoesNotStallTheFrameLoop() {
    const fs::path dir = scratchDir("hang");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    FakeDriver driver;
    driver.blockMs.store(2500);

    HangWatchdog w;
    w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    w.start(dir.string(), 800);
    CHECK(w.running());

    AudioOpen gate;
    gate.bind(driver.opener(), [&w] { w.pause(); }, [&w] { w.resume(); });

    // Healthy frames first, so a report afterwards cannot be blamed on the
    // loop never having started.
    for (int i = 0; i < 100; ++i) {
        w.heartbeat();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(w.reportsWritten() == 0u);

    // THE CLICK. The frame that asks for the device waits the bound and then
    // goes back to rendering; it must NOT come back holding the answer.
    const double t0 = nowMs();
    const AudioOpen::Outcome outcome = gate.request(3);
    const double requestMs = nowMs() - t0;
    CHECK(outcome == AudioOpen::Outcome::Busy);
    CHECK(gate.inFlight());
    // Bounded: the bound plus a scheduler tick's slack, nowhere near the 2500
    // the driver is taking.
    CHECK(requestMs < 1800.0);

    // Frames keep turning while the driver thinks, and the longest gap between
    // two of them stays a frame rather than a stall.
    double worstGap = 0.0;
    double last = nowMs();
    int frames = 0;
    bool collected = false;
    const double until = nowMs() + 2600.0;
    while (nowMs() < until) {
        w.heartbeat();
        if (gate.poll()) { collected = true; }
        const double t = nowMs();
        if (t - last > worstGap) { worstGap = t - last; }
        last = t;
        ++frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // THE CHECK THE FIELD REPORT IS. A synchronous open of the same device
    // writes one hang report here; this must write none.
    CHECK(w.reportsWritten() == 0u);
    CHECK(frames > 100);
    CHECK(worstGap < 800.0);

    // And the answer did arrive, on a later frame, without anyone blocking for
    // it - a fix that merely dropped the open would pass everything above.
    CHECK(collected);
    CHECK(!gate.inFlight());
    CHECK(gate.completed() == 1u);
    CHECK(gate.result().deviceIndex == 3);
    CHECK(gate.result().ok);
    CHECK(driver.calls.load() == 1);

    w.stop();
    if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
}

// --- 2. THE COMMON CASE IS UNCHANGED ---------------------------------------
//
// A device that opens in milliseconds still opens inside the frame that asked,
// because a switch that took a frame to appear would be a new bug in place of
// the old one.
void checkFastOpenFinishesInTheRequestingFrame() {
    FakeDriver driver;
    driver.blockMs.store(0);

    AudioOpen gate;
    gate.bind(driver.opener(), nullptr, nullptr);

    CHECK(gate.request(-1) == AudioOpen::Outcome::Finished);
    CHECK(!gate.inFlight());
    CHECK(gate.completed() == 1u);
    CHECK(gate.result().ok);
    CHECK(gate.result().deviceIndex == -1);
    CHECK(driver.lastDevice.load() == -1);
    // Nothing left for the poll to find: a result collected twice would count
    // a recovery twice and re-enumerate for nothing.
    CHECK(!gate.poll());
    CHECK(gate.completed() == 1u);

    // A refusal is a result, not a retry.
    driver.answer.store(false);
    CHECK(gate.request(2) == AudioOpen::Outcome::Finished);
    CHECK(!gate.result().ok);
    CHECK(gate.result().deviceIndex == 2);
}

// --- 3. THE WATCHDOG BRACKET ------------------------------------------------
//
// The bounded wait is deliberate blocking work, so hang_watchdog.hpp's rule 2b
// applies to it. What rots silently is the ORDER (a pause taken after the wait
// has already blocked fixes nothing) and the BALANCE (HangWatchdog counts its
// pauses, so one that is never released disarms the watchdog for the rest of
// the session).
void checkBoundedWaitIsBracketed() {
    FakeDriver driver;
    driver.blockMs.store(400);

    std::atomic<int> paused{0};
    std::atomic<int> resumed{0};
    std::atomic<int> pausedWhenDriverRan{-1};

    AudioOpen gate;
    gate.bind(
        [&driver, &paused, &pausedWhenDriverRan](int dev) {
            // Sampled on the worker while the requesting thread is inside its
            // bounded wait: the pause must already be in force.
            pausedWhenDriverRan.store(paused.load());
            return driver.opener()(dev);
        },
        [&paused] { ++paused; }, [&resumed] { ++resumed; });

    CHECK(gate.request(1) == AudioOpen::Outcome::Finished);
    CHECK(paused.load() == 1);
    CHECK(resumed.load() == 1);
    CHECK(pausedWhenDriverRan.load() == 1);

    // The wait that EXPIRES must release it too - that is the path a wedged
    // device takes, and it is the one where a leaked pause would matter most.
    driver.blockMs.store(2200);
    AudioOpen slow;
    std::atomic<int> paused2{0};
    std::atomic<int> resumed2{0};
    slow.bind(driver.opener(), [&paused2] { ++paused2; }, [&resumed2] { ++resumed2; });
    CHECK(slow.request(1) == AudioOpen::Outcome::Busy);
    CHECK(paused2.load() == 1);
    CHECK(resumed2.load() == 1);
    while (slow.inFlight()) {
        slow.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // The collect on a later frame takes no pause of its own: it blocks on
    // nothing.
    CHECK(paused2.load() == 1);
    CHECK(resumed2.load() == 1);
}

// --- 4. NEVER TWO OPENS AT ONCE --------------------------------------------
//
// Two waveOutOpen calls racing on one AudioOut is worse than the hang: open()
// closes the previous stream, drains the ring and installs a new one, and two
// threads doing that at once is corruption rather than a stall. A user clicking
// three devices while the first is stuck must end on the third, and must not
// have opened the second at all.
void checkRequestsAreQueuedNotRaced() {
    FakeDriver driver;
    driver.blockMs.store(300);

    AudioOpen gate;
    gate.bind(driver.opener(), nullptr, nullptr);
    // A bound of 1500 ms would swallow a 300 ms open, so make the first one
    // outlast it and then shorten the driver for the queued one.
    driver.blockMs.store(2200);
    // Tags 1 and 2 stand for the two askers AppWindow has: the audio watchdog
    // reopening a dead stream, and the user picking a device. A tag kept in
    // the CALLER instead of travelling with the request would be overwritten
    // by the click queued behind the reopen, and the two answers would come
    // back wearing each other's - a user's switch counted as a recovery.
    CHECK(gate.request(1, 1) == AudioOpen::Outcome::Busy);
    driver.blockMs.store(0);
    CHECK(gate.request(2, 2) == AudioOpen::Outcome::Queued);
    CHECK(gate.request(5, 2) == AudioOpen::Outcome::Queued);
    CHECK(gate.queuedDevice() == 5);
    CHECK(driver.calls.load() == 1);  // neither queued request has run

    // Frame loop until both the first and the queued open have completed.
    const double until = nowMs() + 6000.0;
    int collects = 0;
    std::vector<int> tags;
    std::vector<int> devicesSeen;
    while (nowMs() < until && collects < 2) {
        if (gate.poll()) {
            ++collects;
            tags.push_back(gate.result().tag);
            devicesSeen.push_back(gate.result().deviceIndex);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(collects == 2);
    // Each answer carries the tag of the request it belongs to, in order.
    CHECK(tags == std::vector<int>({1, 2}));
    CHECK(devicesSeen == std::vector<int>({1, 5}));
    CHECK(gate.completed() == 2u);
    // The LAST click won; the middle one was superseded and never opened.
    CHECK(gate.result().deviceIndex == 5);
    CHECK(driver.calls.load() == 2);
    CHECK(driver.lastDevice.load() == 5);
    CHECK(gate.queuedDevice() == -2);
    // And at no point were two openers inside the driver together.
    CHECK(driver.maxConcurrent.load() == 1);
}

// --- 5. QUIT DOES NOT WAIT FOR A WEDGED DRIVER ------------------------------
//
// std::async's future blocks in its own destructor until the worker returns, so
// a quit during a stuck open would park ~AppWindow inside waveOutOpen - the
// same hang, after the window has gone. reap() spends a short grace and then
// abandons the worker.
void checkQuitAbandonsAWedgedOpen() {
    FakeDriver driver;
    driver.blockMs.store(2500);

    // The keepalive the real opener carries (Pipeline's shared_ptr to its
    // AudioOut). Abandoning the worker is only safe because of this, so the
    // test holds the same shape: the object must still be alive while the
    // abandoned worker runs, and released once it finishes.
    auto sink = std::make_shared<int>(7);
    std::weak_ptr<int> watch = sink;

    double reapMs = 0.0;
    {
        AudioOpen gate;
        gate.bind([&driver, sink](int dev) { return driver.opener()(dev); }, nullptr,
                  nullptr);
        sink.reset();
        CHECK(gate.request(0) == AudioOpen::Outcome::Busy);
        CHECK(!watch.expired());

        const double t0 = nowMs();
        gate.reap();
        reapMs = nowMs() - t0;
        CHECK(!gate.inFlight());
        // Reaping twice is a no-op, not a second detached thread.
        gate.reap();
    }
    // Bounded by kQuitGrace, not by the 2500 ms driver.
    CHECK(reapMs < 1200.0);
    // The worker was still running, and the sink it holds was NOT destroyed
    // under it.
    CHECK(!watch.expired());

    // ...and once it finishes, the last reference goes with it.
    const double until = nowMs() + 5000.0;
    while (nowMs() < until && !watch.expired()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(watch.expired());
}

// --- 6. THE REAL SINK, THROUGH THE REAL OPENER ------------------------------
//
// EVERYTHING ABOVE IS A MOCK, and a mock-only verification of an I/O feature is
// a hypothesis. The one thing it cannot say is whether PortAudio actually opens
// a device when Pa_OpenStream is called from a WORKER thread instead of the
// GUI thread - and a fix that quietly stopped opening any device at all would
// pass every check in this file. So this drives Pipeline's own opener, the same
// callable AppWindow binds into the gate, at the system default output.
//
// Asserted loudly rather than skipped when a device exists, exactly as
// tests/test_audio_out.cpp does for the synchronous path; a machine with no
// output device at all (headless CI) is the only case that is let through, and
// it is distinguished by asking the sink, not by ignoring the result.
void checkRealOpenerOnAWorkerThread() {
    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = 1000000.0;
    cfg.fftSize = 1024;
    // The constructor's own synchronous open is not what is under test here -
    // it runs before any frame loop exists - so start closed and let the gate
    // do the opening.
    cfg.audioEnabled = false;
    cascade::core::Pipeline pipeline(cfg);

    const bool haveDevice = !pipeline.audio().listOutputDevices().empty();

    AudioOpen gate;
    gate.bind(pipeline.audioOpener(), nullptr, nullptr);
    const AudioOpen::Outcome outcome = gate.request(-1);

    // A real default device opens in milliseconds, so the bounded wait should
    // have carried it; if the machine is slow enough to have missed the bound,
    // poll it out rather than failing on the timing.
    if (outcome != AudioOpen::Outcome::Finished) {
        const double until = nowMs() + 10000.0;
        while (nowMs() < until && gate.inFlight()) {
            gate.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    CHECK(!gate.inFlight());
    CHECK(gate.completed() == 1u);

    if (!haveDevice) {
        std::printf("test_audio_open: no output device on this machine - "
                    "the real-opener block asserted only that it answered\n");
        return;
    }
    CHECK(gate.result().ok);
    // The GUI-thread half, and the proof the stream is genuinely playing: this
    // is what AppWindow::applyAudioOpenResult does on the frame it collects.
    pipeline.publishAudioChannels(gate.result().ok);
    CHECK(pipeline.audio().everOpened());
    CHECK(pipeline.audio().streamAlive());
    CHECK(pipeline.audio().channels() == 1 || pipeline.audio().channels() == 2);
    if (!gate.result().ok) {
        std::printf("FAIL the real opener refused the system default device\n");
    }
}

// --- 7. AN UNBOUND GATE REFUSES RATHER THAN CRASHES -------------------------
void checkUnboundGateRefuses() {
    AudioOpen gate;
    CHECK(gate.request(0) == AudioOpen::Outcome::Finished);
    CHECK(!gate.result().ok);
}

}  // namespace

int main() {
    checkBlockingOpenDoesNotStallTheFrameLoop();
    checkFastOpenFinishesInTheRequestingFrame();
    checkBoundedWaitIsBracketed();
    checkRequestsAreQueuedNotRaced();
    checkQuitAbandonsAWedgedOpen();
    checkRealOpenerOnAWorkerThread();
    checkUnboundGateRefuses();
    return testSummary("test_audio_open");
}
