// test_sentinel_beat_source.cpp - WHO FEEDS THE SENTINEL'S HEARTBEAT (deep review of engine/stage4, batch A; docs/engine-stage4.md 6.31).
//
// core/breadcrumb.hpp promises the watcher outside the process one thing about the heartbeat: it is the HOST'S FRAME LOOP, drawn once a frame by
// the thread that draws. A "silent" window to the watcher is exactly a stalled frame loop to the in-process watchdog. Since the shell retirement
// (6.25, R7/R8) the engine's control thread runs a hang watchdog of its own - a HangWatchdog like the window's - and `HangWatchdog::heartbeat()`
// called `breadcrumb::beat()` for EVERY instance, so once the window was up the control thread (62 beats a second, whatever the window was doing)
// drove the sentinel's frame count, its silence clock, the AwaitingFirstFrame -> Running transition and the "somebody is holding the window" bit.
// Three review findings had one root:
//
//   - the GUI thread wedges (a driver wait, a modal SendMessage) and the control thread beats on: the watcher's silence clock reads zero, the
//     Frozen branch can never fire, and the ending is filed `Outside` instead of `Frozen`;
//   - a window being dragged raises kUserPaced and the control thread's next beat clears it again, within 16 ms: the modal-loop excuse is defeated;
//   - the control thread's second beat after AwaitingFirstFrame ends the phase before the window has drawn a frame: a death during the first frame
//     (shader compile, atlas upload) is classed as "after the first frame".
//
// THE ONE ROOT FIX: a HangWatchdog feeds the sentinel only when it is told it is the process's frame loop (`setSentinelHeartbeat`, on by default - the
// window's watchdog is one); the engine's control thread is told so only when no window follows (a headless service has no frame loop, and the pass is
// the loop whose liveness the sentinel should judge: ControlThreadOptions::sentinelHeartbeat, set by EngineService::start from `frontEndFollows`).
//
//   A  A WATCHDOG THAT IS NOT THE FRAME LOOP MOVES NOTHING in the breadcrumb - not the count, not the clock, not the phase, not the activity bits - however
//      often it beats; the frame loop's own watchdog moves all of them, exactly as it did.
//   B  THE ENGINE'S CONTROL THREAD, REAL, with a breadcrumb attached and the phase at AwaitingFirstFrame (the window just shown): started with the WINDOW HOST's
//      options (the default) it leaves the breadcrumb alone for as long as it likes; started as a HEADLESS host's (sentinelHeartbeat) it is the heartbeat.
//   C  THE SERVICE WIRES IT: an EngineService that a window follows starts its control thread without the flag; one that no window follows, with it.
//   D  THE COUNT IS ONE ATOMIC ADD: two threads beating a million times between them lose none (the block's comment promised "no update is ever lost").
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "core/breadcrumb.hpp"
#include "core/hang_watchdog.hpp"
#include "engine/engine.hpp"
#include "engine/engine_service.hpp"
#include "engine_api_test_env.hpp"
#include "test_check.hpp"

namespace breadcrumb = cascade::core::breadcrumb;
using breadcrumb::Phase;
using cascade::core::HangWatchdog;
using cascade::engine::Engine;
using cascade::engine::EngineService;
using cascade::engine::ServiceOptions;
using Clock = std::chrono::steady_clock;

namespace {

void sleepMs(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// A zeroed block, attached as the process's breadcrumb, in the phase the window leaves BuildingApp/CreatingWindow in.
struct Crumb {
    alignas(64) breadcrumb::Block block{};
    Crumb() {
        breadcrumb::attach(&block);
        breadcrumb::setPhase(Phase::AwaitingFirstFrame);
    }
    ~Crumb() { breadcrumb::attach(nullptr); }
    breadcrumb::Snapshot read() const { return breadcrumb::read(&block); }
};

// ---- A ------------------------------------------------------------------------------------------------------------------------------------
void testWatchdogFlag() {
    std::printf("  A: a watchdog that is not the frame loop moves nothing in the breadcrumb\n");
    Crumb crumb;
    HangWatchdog window;   // the default: the frame loop's
    HangWatchdog control;
    control.setSentinelHeartbeat(false);
    breadcrumb::setActivity(breadcrumb::kUserPaced, true);   // somebody is dragging the window
    for (int i = 0; i < 40; ++i) { control.heartbeat(); }
    breadcrumb::Snapshot s = crumb.read();
    CHECK(s.valid);
    CHECK(s.frames == 0u);                                   // no frame was drawn
    CHECK(s.beatMs == 0u);                                   // the silence clock has not been touched
    CHECK(s.phase == Phase::AwaitingFirstFrame);             // and the first frame is still to come
    CHECK((s.activity & breadcrumb::kUserPaced) != 0u);      // and the person is still holding the window
    // the frame loop's own watchdog moves all of it, as ever
    window.heartbeat();
    s = crumb.read();
    CHECK(s.frames == 1u && s.beatMs != 0u);
    CHECK(s.phase == Phase::AwaitingFirstFrame);             // (the SECOND heartbeat ends it)
    CHECK((s.activity & breadcrumb::kUserPaced) == 0u);      // a frame was drawn to the end: nobody is holding the loop
    window.heartbeat();
    CHECK(crumb.read().phase == Phase::Running);
    // and a watchdog switched back on is the heartbeat again
    control.setSentinelHeartbeat(true);
    const std::uint64_t before = crumb.read().frames;
    control.heartbeat();
    CHECK(crumb.read().frames == before + 1u);
}

// ---- B ------------------------------------------------------------------------------------------------------------------------------------
std::uint64_t framesAfterRunning(bool sentinelHeartbeat, Phase& phaseOut, std::uint64_t& beatOut, unsigned ms) {
    Crumb crumb;
    Engine e;
    e.initialise();
    cascade::engine::ControlThreadOptions opts;
    opts.period = std::chrono::milliseconds(10);
    opts.watchdogThresholdMs = 5000;
    opts.sentinelHeartbeat = sentinelHeartbeat;
    e.startControlThread(opts);
    sleepMs(ms);
    e.stopControlThread();
    const breadcrumb::Snapshot s = crumb.read();
    phaseOut = s.phase;
    beatOut = s.beatMs;
    return s.frames;
}

void testEngineControlThread() {
    std::printf("  B: the engine's control thread: the window host's options leave the breadcrumb alone, a headless host's are the heartbeat\n");
    Phase phase = Phase::Unset;
    std::uint64_t beatMs = 0;
    const std::uint64_t windowFrames = framesAfterRunning(false, phase, beatMs, 500);
    std::printf("    window host: %llu beats, phase %u, beatMs %llu\n", static_cast<unsigned long long>(windowFrames), static_cast<unsigned>(phase),
                static_cast<unsigned long long>(beatMs));
    CHECK(windowFrames == 0u);
    CHECK(beatMs == 0u);
    CHECK(phase == Phase::AwaitingFirstFrame);   // a death now is still "before the first frame"
    const std::uint64_t headlessFrames = framesAfterRunning(true, phase, beatMs, 500);
    std::printf("    headless host: %llu beats, phase %u, beatMs %llu\n", static_cast<unsigned long long>(headlessFrames), static_cast<unsigned>(phase),
                static_cast<unsigned long long>(beatMs));
    CHECK(headlessFrames >= 10u);
    CHECK(beatMs != 0u);
    CHECK(phase == Phase::Running);
}

// ---- C ------------------------------------------------------------------------------------------------------------------------------------
std::uint64_t framesOfService(bool frontEndFollows, unsigned ms) {
    Crumb crumb;
    ServiceOptions o;           // hermetic: no settings file, the signal generator
    o.frontEndFollows = frontEndFollows;
    o.receive = false;
    {
        EngineService svc(o);
        svc.restore();
        std::string why;
        CHECK(svc.start(why));
        sleepMs(ms);
        svc.stop();
    }
    return crumb.read().frames;
}

void testServiceWiring() {
    std::printf("  C: the service starts the control thread as the heartbeat only when no window follows\n");
    CHECK(framesOfService(true, 400) == 0u);
    CHECK(framesOfService(false, 400) >= 10u);
}

// ---- D ------------------------------------------------------------------------------------------------------------------------------------
void testCountLosesNothing() {
    std::printf("  D: two threads beating lose no count\n");
    Crumb crumb;
    constexpr int kEach = 500000;
    std::atomic<bool> go{false};
    auto beater = [&] {
        while (!go.load()) {}
        for (int i = 0; i < kEach; ++i) { breadcrumb::beat(); }
    };
    std::thread a(beater);
    std::thread b(beater);
    go.store(true);
    a.join();
    b.join();
    const std::uint64_t frames = crumb.read().frames;
    std::printf("    %llu of %d\n", static_cast<unsigned long long>(frames), 2 * kEach);
    CHECK(frames == static_cast<std::uint64_t>(2 * kEach));
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_sentinel_beat_source\n");
    engine_api_test::hermetic("sentinel_beat");
    testWatchdogFlag();
    testEngineControlThread();
    testServiceWiring();
    testCountLosesNothing();
    return testSummary("test_sentinel_beat_source");
}
