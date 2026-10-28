// test_plugin_ui_threaded.cpp - THE PLUGIN UI IS THE WINDOW'S, AND THE ENGINE'S PASS ON ITS OWN THREAD NEVER REACHES IT (the pump
// move, step P3; docs/engine-stage4.md, "PUMP MOVE, STEP P3").
//
// THE CHANGE UNDER TEST. PluginUi - the track sources, panels and instruments the window polls every frame, and the references
// into its vectors that twenty-odd consumers hold for the whole frame - used to be rebuilt and cleared by the engine's pass, and
// its tracks walked by it every pass. That was safe only while the frame was the pass. Now the engine owns the API core
// (grants, stop set, request queues, the request_tune records) and POSTS a directive; the window applies the newest at the head
// of its plugin poll and acknowledges it; the unload handshake waits on that acknowledgement; the legacy request_tune is a queued
// request; and the browser's target list is a copy the window hands over. These tests run the pass on the engine's control thread
// (the pump move's end state) with the TEST'S thread as the window's frame, on a REAL AppWindow, a REAL Engine and an
// instrumented REAL plugin module (tests/fixture_pump3_plugin.cpp).
//
//   A  REBUILDS DURING ACTIVE POLLING. The frame polls and reads the tracks flat out while a stop/start churn rebuilds the plugin
//      UI on the pass: no instance is polled or destroyed through a dead handle, none is destroyed beside a poll, every create
//      and destroy is on the polling thread, and the tracks the frame reads are always whole.
//   B  EPOCHS SKIPPED UNDER LOAD, NEWEST WINS. With the frame parked, four rebuilds are posted (each wait gives up and says so);
//      when the frame returns it applies the NEWEST ONLY: one rebuild, three directives skipped, the warning said four times.
//   C  THE STALE REFERENCE THE DESIGN PREVENTS. The frame holds a reference into the host's track vector (inside a hold, with the
//      pass wanting to destroy the instance that filled it). Nothing changes under the reference for as long as it is held; the
//      directive is applied at the NEXT head of poll, and then the instance is gone. (Break-it: rebuild from the pass, as the old
//      code did, and C fails deterministically - the instance is destroyed under the held reference.)
//   D  request_tune, QUEUED. OK means ACCEPTED; the receiver is not moved by the call (the pass is parked, so nothing can have
//      moved it) and lands within a pass of unparking; the newest of many wins; a revoked grant, a stopped plugin and a grant
//      revoked between the call and the pass all stop the tune; a refused call is answered DENIED at once and recorded; a
//      Doppler corrector at poll rate, and a plugin's own thread, are safe beside the pass.
//   E  EVERY RELEASE IS ON THE WINDOW'S THREAD. A rescan from the pass destroys the basemap's and the track-info client's
//      instances (the basemap's tiles are GL textures) and the track instances on the thread that polls them, never the pass's;
//      the module is unmapped with none alive (the module says so itself, from its static destructor).
//   F  THE CORE'S QUEUE, alone: one slot per module, newest wins, and a request that is no longer allowed is not applied.
//   G  A WINDOW THAT ARRIVES AFTER THE FIRST SCAN (shell retirement R4, the late attach): the engine's own instances are let go, the plugin UI's
//      current state is replayed into the window (tracks, instruments, basemap, track-info client, the stop set), nothing is held twice, every
//      create and destroy is on the window's thread - and the window's own release (PluginUiPort::release) leaves nothing for the engine's.
//   G5 A WINDOW'S EXIT disconnects its port, once, as its own last act.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/plugin_api.hpp"
#include "engine/engine.hpp"
#include "engine_api_fake_radio.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "pump3_support.hpp"
#include "test_check.hpp"

#define CHECK_EQ(a, b) CHECK((a) == (b))

using cascade::gui::AppWindow;
using Access = cascade::gui::AppWindowTestAccess;
using namespace pump3;

namespace {

std::string g_fixturePath;
std::string g_unmapLog;

// A fresh window with the fixture loaded, the GUI side of the plugin system built (inline, by the constructor's scan).
struct Env {
    std::unique_ptr<th::TestWindow> app;
    Env() {
        app = std::make_unique<th::TestWindow>();
        Access::pinPlugins(*app);
        Access::poll(*app);  // the frame's first poll
    }
    ~Env() {
        if (app != nullptr && Access::controlRunning(*app)) { Access::stopControl(*app); }
    }
    AppWindow& a() { return *app; }
    Fx fx() const { return fxNow(g_fixturePath); }
};

// Polls like a frame until `pred` holds or `ms` pass.
bool framesUntil(AppWindow& a, const std::function<bool()>& pred, int ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        Access::poll(a);
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Access::poll(a);
    return pred();
}

void send(AppWindow& a, std::uint32_t op, const std::string& key) { Access::submit(a, cmd::makeText(op, key)); }
void grant(AppWindow& a, bool on) {
    cmd::QueuedCommand q = cmd::makeText(FOXAPI_OP_PLUGIN_GRANT, fixtureKey(), 1, on ? 1 : 0);
    Access::submit(a, q);
}

// Parks the control thread and waits until it is really between passes (the pass counter stops).
void parkControl(AppWindow& a) {
    Access::park(a, true);
    std::uint64_t last = Access::passes(a);
    int stable = 0;
    for (int i = 0; i < 400 && stable < 8; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const std::uint64_t now = Access::passes(a);
        stable = (now == last) ? stable + 1 : 0;
        last = now;
    }
    CHECK(stable >= 8);
}
void unparkControl(AppWindow& a) { Access::park(a, false); }

// ======================================================================================================================
// A. REBUILDS DURING ACTIVE POLLING
// ======================================================================================================================
void testRebuildDuringPolling() {
    std::printf("  A: rebuilds during active polling (the pass rebuilds on a stop/start churn, the frame reads flat out)\n");
    Env env;
    AppWindow& a = env.a();
    const std::string key = fixtureKey();
    CHECK_EQ(Access::ui(a).trackCount(), 3u);
    const Fx fx0 = env.fx();
    CHECK(fx0.mapped);
    fx0.reset();
    Access::startControl(a);

    int whole = 0;
    int empty = 0;
    int torn = 0;
    bool wantStopped = false;
    // TIME-BOUNDED, flat out (a sleep per frame would be 15 ms on this OS, and the point is pressure): two seconds of frames, a
    // stop or a start every 25 ms.
    const auto began = std::chrono::steady_clock::now();
    auto lastToggle = began;
    while (std::chrono::steady_clock::now() - began < std::chrono::milliseconds(2000)) {
        Access::poll(a);
        // A reference into the host's vector, held across the body of the frame: its contents must be whole while held.
        const std::vector<cascade::core::HostTrack>& t = Access::tracks(a);
        const std::size_t n = t.size();
        bool ok = true;
        for (std::size_t k = 0; k < n; ++k) {
            const std::string id = t[k].t.id;
            if (id.rfind("P3A", 0) != 0 || !(t[k].t.latDeg >= 51.0 && t[k].t.latDeg < 54.0)) { ok = false; }
        }
        if (n != 0u && n != 3u) { ok = false; }
        if (n == 3u && ok) {
            ++whole;
        } else if (n == 0u && ok) {
            ++empty;
        } else {
            ++torn;
        }
        if (std::chrono::steady_clock::now() - lastToggle >= std::chrono::milliseconds(25)) {
            lastToggle = std::chrono::steady_clock::now();
            wantStopped = !wantStopped;
            send(a, wantStopped ? FOXAPI_OP_DECODER_STOP : FOXAPI_OP_DECODER_START, key);
        }
        std::this_thread::yield();
    }
    if (wantStopped) { send(a, FOXAPI_OP_DECODER_START, key); }
    CHECK(framesUntil(a, [&] { return Access::ui(a).trackCount() == 3u; }, 5000));
    // The pass has nothing more to post.
    CHECK(framesUntil(a, [&] { return Access::link(a).appliedEpoch() == Access::link(a).postedEpoch(); }, 5000));
    Access::stopControl(a);

    const Fx fx = env.fx();
    std::printf("      %d whole frames, %d empty (stopped), %d torn; %ld creates, %ld destroys, %ld polls; directives %llu applied, %llu skipped\n",
                whole, empty, torn, fx.creates(), fx.destroys(), fx.polls(),
                static_cast<unsigned long long>(Access::link(a).appliedCount()),
                static_cast<unsigned long long>(Access::link(a).skipped()));
    CHECK_EQ(torn, 0);
    CHECK(whole > 20);
    CHECK(empty > 5);                         // the churn really stopped it
    CHECK(fx.creates() >= 8);                 // ...and really rebuilt it
    CHECK_EQ(fx.live(), 1);                   // one running instance, none leaked
    CHECK_EQ(fx.creates(), fx.destroys());    // (the counters were zeroed with the first instance alive: its destroy is counted)
    CHECK_EQ(fx.staleCalls(), 0);             // never a poll or a destroy through a dead handle
    CHECK_EQ(fx.destroyDuringPoll(), 0);      // never a destroy beside a poll
    CHECK_EQ(fx.wrongThread(), 0);            // every create and destroy on the thread that polls
}

// ======================================================================================================================
// B. EPOCHS SKIPPED UNDER LOAD
// ======================================================================================================================
void testNewestWins() {
    std::printf("  B: epochs skipped under load - four rebuilds posted with the frame parked, the frame applies the newest only\n");
    Env env;
    AppWindow& a = env.a();
    const std::string key = fixtureKey();
    const Fx fx0 = env.fx();
    fx0.reset();
    const std::uint64_t posted0 = Access::link(a).postedEpoch();
    const std::uint64_t skipped0 = Access::link(a).skipped();
    const std::uint64_t applied0 = Access::link(a).appliedCount();
    const int warned0 = diagLines("had not applied the plugin directive");
    Access::startControl(a);

    // STOP, START, STOP, START: the same pass applies all four. The first one's wait runs out (the frame is parked); the other three
    // are posted at once, WITHOUT a wait of their own - the window has not framed since (the silence rule) - so the pass is held
    // up for one bound, not four.
    const auto began = std::chrono::steady_clock::now();
    send(a, FOXAPI_OP_DECODER_STOP, key);
    send(a, FOXAPI_OP_DECODER_START, key);
    send(a, FOXAPI_OP_DECODER_STOP, key);
    send(a, FOXAPI_OP_DECODER_START, key);
    CHECK(waitFor([&] { return Access::link(a).postedEpoch() >= posted0 + 4u; }, 8000));
    const double fourPostsMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    std::printf("      four rebuilds posted in %.0f ms (one bound is %d)\n", fourPostsMs,
                static_cast<int>(Engine::kPluginUiHandshake.count()));
    CHECK(fourPostsMs < 3.0 * static_cast<double>(Engine::kPluginUiHandshake.count()));   // four waits would be a second
    CHECK(Access::link(a).hasPending());
    CHECK(Access::link(a).appliedEpoch() < Access::link(a).postedEpoch());   // none of the four applied
    CHECK_EQ(fx0.creates(), 0);                                              // ...so nothing was rebuilt
    CHECK_EQ(fx0.destroys(), 0);
    CHECK_EQ(fx0.live(), 1);
    CHECK_EQ(Access::link(a).skipped(), skipped0 + 3u);

    // The frame comes back: ONE application, of the newest (START), and the stale three are acknowledged with it.
    Access::poll(a);
    CHECK_EQ(Access::link(a).appliedEpoch(), Access::link(a).postedEpoch());
    CHECK_EQ(Access::link(a).appliedCount(), applied0 + 1u);
    CHECK(waitFor([&] { return Access::link(a).postedEpoch() >= posted0 + 4u; }, 100));
    // STUB CAMPAIGN B2 (0.99.73): a stop or a start is ONE plugin's instances, not a wholesale rebuild, and the directive that carries it is
    // the STATE the interface is to hold (core/plugin_directive.hpp, lifeOnly), not an event. The frame applies the newest ONCE (above) -
    // and the newest says "running", which is what the instance already is: STOP, START, STOP, START is a net nothing, so it is not
    // destroyed and made again. (Before 0.99.73 each one rebuilt every instance; the churn the interface sees is the net one.)
    CHECK_EQ(fx0.creates(), 0);
    CHECK_EQ(fx0.destroys(), 0);
    CHECK_EQ(fx0.live(), 1);
    CHECK_EQ(Access::ui(a).trackCount(), 3u);
    // ...and the pass said so each time it could not confirm a rebuild: four lines (not an error: nothing is being unmapped).
    std::printf("      the pass could not confirm a rebuild, and said so %d time(s)\n",
                diagLines("had not applied the plugin directive") - warned0);
    CHECK_EQ(diagLines("had not applied the plugin directive") - warned0, 4);
    // A NET CHANGE is applied as one: STOP, START, STOP with the frame parked is one application, and it destroys the instance once;
    // a START after it brings it back, created once.
    {
        const std::uint64_t applied1 = Access::link(a).appliedCount();
        const std::uint64_t posted1 = Access::link(a).postedEpoch();
        send(a, FOXAPI_OP_DECODER_STOP, key);
        send(a, FOXAPI_OP_DECODER_START, key);
        send(a, FOXAPI_OP_DECODER_STOP, key);
        CHECK(waitFor([&] { return Access::link(a).postedEpoch() >= posted1 + 3u; }, 8000));
        CHECK_EQ(Access::link(a).appliedCount(), applied1);   // the frame has not come back
        Access::poll(a);
        CHECK_EQ(Access::link(a).appliedCount(), applied1 + 1u);
        CHECK_EQ(fx0.destroys(), 1);
        CHECK_EQ(fx0.live(), 0);
        CHECK_EQ(Access::ui(a).trackCount(), 0u);
        send(a, FOXAPI_OP_DECODER_START, key);
        CHECK(framesUntil(a, [&] { return Access::ui(a).trackCount() == 3u; }, 5000));
        CHECK_EQ(fx0.creates(), 1);
        CHECK_EQ(fx0.live(), 1);
    }
    Access::stopControl(a);
    CHECK_EQ(fx0.staleCalls(), 0);
}

// ======================================================================================================================
// C. THE STALE REFERENCE THE DESIGN PREVENTS
// ======================================================================================================================
void testStaleReference() {
    std::printf("  C: a reference held across the pass's rebuild stays whole until the next head of poll\n");
    Env env;
    AppWindow& a = env.a();
    const std::string key = fixtureKey();
    const Fx fx0 = env.fx();
    fx0.reset();
    Access::startControl(a);
    Access::poll(a);

    // THE FRAME TAKES ITS REFERENCES...
    const std::vector<cascade::core::HostTrack>& tracks = Access::tracks(a);
    CHECK_EQ(tracks.size(), 3u);
    const cascade::core::HostTrack* data0 = tracks.data();
    const std::vector<cascade::core::HostTrack> copy = tracks;
    // ...and, while it holds them, the pass is told to stop the plugin (which destroys the instance behind them).
    const std::uint64_t posted0 = Access::link(a).postedEpoch();
    send(a, FOXAPI_OP_DECODER_STOP, key);
    CHECK(waitFor([&] { return Access::link(a).postedEpoch() > posted0; }, 4000));   // the pass has posted its directive...
    // ...and now WAITS for the frame. For as long as the frame is mid-frame, NOTHING changes under the reference.
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK(Access::link(a).hasPending());
    CHECK_EQ(fx0.destroys(), 0);                // the instance behind the reference is alive
    CHECK_EQ(fx0.live(), 1);
    CHECK_EQ(fx0.creates(), 0);
    CHECK_EQ(tracks.size(), 3u);                // the vector is as it was
    CHECK(tracks.data() == data0);
    for (std::size_t i = 0; i < copy.size(); ++i) {
        CHECK(std::string(tracks[i].t.id) == copy[i].t.id);
        CHECK(tracks[i].t.latDeg == copy[i].t.latDeg);
    }
    // The frame ends; the next head of poll applies the directive, and only then is the instance gone.
    Access::poll(a);
    CHECK_EQ(fx0.live(), 0);
    CHECK_EQ(fx0.destroys(), 1);
    CHECK_EQ(Access::ui(a).trackCount(), 0u);
    CHECK(framesUntil(a, [&] { return Access::link(a).appliedEpoch() == Access::link(a).postedEpoch(); }, 2000));
    Access::stopControl(a);
    CHECK_EQ(fx0.staleCalls(), 0);
    CHECK_EQ(fx0.destroyDuringPoll(), 0);
    CHECK_EQ(fx0.wrongThread(), 0);
}

// ======================================================================================================================
// D. request_tune, QUEUED
// ======================================================================================================================
void testRequestTune() {
    std::printf("  D: request_tune is a request - OK means accepted, the pass tunes, the newest wins, a withdrawn grant stops it\n");
    Env env;
    AppWindow& a = env.a();
    const std::string key = fixtureKey();
    const Fx fx = env.fx();
    CHECK(fx.requestTune != nullptr);
    Access::startControl(a);

    // --- refused: no grant. Answered at once, in the call, and recorded for the user to see ---
    CHECK_EQ(fx.requestTune(101.0e6), CASCADE_TUNE_DENIED);
    CHECK(waitFor([&] { return Access::core(a).lastDeniedPlugin() == key; }, 1000));
    {
        const std::vector<std::string> askers = Access::core(a).tuneRequesters();
        CHECK(askers.size() == 1u && askers[0] == key);
    }
    // (the pass publishes its first state a moment after it starts)
    CHECK(waitFor([&] { return Access::centre(a) > 1.0e3; }, 4000));
    const double centre0 = Access::centre(a);

    // --- granted: OK = accepted; with the pass parked, the receiver cannot have moved ---
    grant(a, true);
    CHECK(waitFor([&] { return Access::core(a).tuneGranted(key); }, 4000));
    parkControl(a);
    const double target = 103.25e6;
    CHECK(std::fabs(target - centre0) > 1.0);
    const double source0 = Access::sourceCentre(a);
    CHECK_EQ(fx.requestTune(target), CASCADE_TUNE_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK_NEAR(Access::centre(a), centre0, 0.5);                 // the call did not move the published centre...
    CHECK_NEAR(Access::sourceCentre(a), source0, 0.5);           // ...nor the receiver itself (the pass is parked: nothing else could)
    unparkControl(a);
    CHECK(waitFor([&] { return std::fabs(Access::centre(a) - target) < 1.0; }, 4000));   // ...the pass did, within passes
    std::printf("      accepted at the call, tuned by the pass: %.3f MHz\n", Access::centre(a) / 1.0e6);

    // --- the newest of many wins (a plugin's own thread; one slot per module) ---
    parkControl(a);
    std::atomic<int> notOk{0};
    std::thread worker([&] {
        for (int i = 1; i <= 60; ++i) {
            if (fx.requestTune(104.0e6 + static_cast<double>(i) * 1000.0) != CASCADE_TUNE_OK) { ++notOk; }
        }
    });
    worker.join();
    CHECK_EQ(notOk.load(), 0);
    unparkControl(a);
    CHECK(waitFor([&] { return std::fabs(Access::centre(a) - (104.0e6 + 60.0e3)) < 1.0; }, 4000));

    // --- a grant withdrawn between the call and the pass: the retune is not applied ---
    const double beforeRevoke = Access::centre(a);
    parkControl(a);
    CHECK_EQ(fx.requestTune(106.5e6), CASCADE_TUNE_OK);
    Access::core(a).setTuneGranted(key, false);                  // (the pass is parked: this thread owns the engine)
    unparkControl(a);
    for (int i = 0; i < 25; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(4)); }
    CHECK_NEAR(Access::centre(a), beforeRevoke, 0.5);
    CHECK_EQ(fx.requestTune(106.5e6), CASCADE_TUNE_DENIED);      // ...and the next call is refused outright
    CHECK_NEAR(Access::centre(a), beforeRevoke, 0.5);

    // --- out of range and not a number: refused in the call, nothing queued ---
    Access::core(a).setTuneGranted(key, true);
    CHECK_EQ(fx.requestTune(0.0), CASCADE_TUNE_OUT_OF_RANGE);
    CHECK_EQ(fx.requestTune(std::nan("")), CASCADE_TUNE_OUT_OF_RANGE);
    CHECK_EQ(fx.requestTune(2.0e12), CASCADE_TUNE_OUT_OF_RANGE);

    // --- a stopped plugin: refused (its bridge outlives its instances), and nothing moves ---
    send(a, FOXAPI_OP_DECODER_STOP, key);
    CHECK(framesUntil(a, [&] { return Access::ui(a).trackCount() == 0u; }, 4000));
    CHECK(waitFor([&] { return Access::core(a).isStopped(key); }, 2000));
    CHECK_EQ(fx.requestTune(107.0e6), CASCADE_TUNE_DENIED);
    CHECK_NEAR(Access::centre(a), beforeRevoke, 0.5);
    send(a, FOXAPI_OP_DECODER_START, key);
    CHECK(framesUntil(a, [&] { return Access::ui(a).trackCount() == 3u; }, 4000));
    Access::stopControl(a);
}

void testDopplerChurn() {
    std::printf("  D2: a Doppler corrector at poll rate, from the frame's own poll, beside the pass and a rebuild churn\n");
    Env env;
    AppWindow& a = env.a();
    const std::string key = fixtureKey();
    const Fx fx = env.fx();
    fx.reset();
    Access::startControl(a);
    grant(a, true);
    CHECK(waitFor([&] { return Access::core(a).tuneGranted(key); }, 4000));
    const double base = 110.0e6;
    fx.setChurn(1, base, 1000.0);
    int notOk = 0;
    {
        const auto began = std::chrono::steady_clock::now();
        auto lastToggle = began;
        bool stopped = false;
        while (std::chrono::steady_clock::now() - began < std::chrono::milliseconds(1500)) {
            Access::poll(a);
            // OK - or DENIED, which is the right answer in the moments after a stop was applied by the pass and before the window
            // has destroyed the instance (the core's stop set is the engine's and takes effect at once). Nothing else may appear.
            const int rc = fx.lastTuneRc();
            // (12345 is the fixture's "no request made yet" - the first polls after the churn is switched on can precede the instance the pass is
            // still building; the full-suite run of step P5 caught this test counting it as an answer, once in three runs.)
            constexpr int kNoRequestYet = 12345;
            if (rc != kNoRequestYet && rc != CASCADE_TUNE_OK && rc != CASCADE_TUNE_DENIED) {
                if (notOk++ == 0) { std::printf("      (first answer other than OK or DENIED: %d after %ld calls)\n", rc, fx.tuneCalls()); }
            }
            if (std::chrono::steady_clock::now() - lastToggle >= std::chrono::milliseconds(120)) {
                lastToggle = std::chrono::steady_clock::now();
                stopped = !stopped;
                send(a, stopped ? FOXAPI_OP_DECODER_STOP : FOXAPI_OP_DECODER_START, key);
            }
            std::this_thread::yield();
        }
    }
    send(a, FOXAPI_OP_DECODER_START, key);
    CHECK(framesUntil(a, [&] { return Access::ui(a).trackCount() == 3u; }, 5000));
    // The corrector stops; the last request it made (every one was accepted) is the one the receiver ends on.
    fx.setChurn(0, base, 1000.0);
    const long calls = fx.tuneCalls();
    CHECK(waitFor([&] { return std::fabs(Access::centre(a) - (base + static_cast<double>(calls) * 1000.0)) < 1.0; }, 4000));
    std::printf("      %ld corrections requested, receiver ends on %.3f MHz\n", calls, Access::centre(a) / 1.0e6);
    CHECK(calls > 100);
    CHECK_EQ(notOk, 0);
    Access::stopControl(a);
    CHECK_EQ(fx.staleCalls(), 0);
    CHECK_EQ(fx.destroyDuringPoll(), 0);
    CHECK_EQ(fx.wrongThread(), 0);
}

// ======================================================================================================================
// E. EVERY RELEASE IS ON THE WINDOW'S THREAD
// ======================================================================================================================
// Written by the pass's thread at the unmap, read by the test's after `g_atUnmapRan` is seen set (which the hook sets LAST).
std::atomic<bool> g_atUnmapRan{false};
struct AtUnmap {
    int runs = 0;
    long live = -1, basemapLive = -1, infoLive = -1;
    unsigned long long basemapDestroyedOn = 0, infoDestroyedOn = 0, ranOn = 0;
    long destroys = -1, creates = -1;
};
AtUnmap g_atUnmap;
void atUnmapHook() {
    const Fx fx = fxNow(g_fixturePath);
    if (!fx.mapped) { return; }
    ++g_atUnmap.runs;
    g_atUnmap.live = fx.live();
    g_atUnmap.basemapLive = fx.basemapLive();
    g_atUnmap.infoLive = fx.infoLive();
    g_atUnmap.basemapDestroyedOn = fx.basemapDestroyThread();
    g_atUnmap.infoDestroyedOn = fx.infoDestroyThread();
    g_atUnmap.ranOn = fx.thread();
    g_atUnmap.destroys = fx.destroys();
    g_atUnmap.creates = fx.creates();
    g_atUnmapRan.store(true);
}

void testReleaseOnWindowThread() {
    std::printf("  E: a rescan from the pass releases the plugin UI, the basemap and the track-info client on the window's thread\n");
    Env env;
    AppWindow& a = env.a();
    const Fx fx0 = env.fx();
    // The constructor's scan applied the directive inline: both clients are attached, on this thread.
    CHECK(Access::basemap(a).active());
    CHECK(Access::trackInfo(a).active());
    CHECK_EQ(fx0.basemapLive(), 1);
    CHECK_EQ(fx0.infoLive(), 1);
    const unsigned long long guiThread = fx0.thread();
    CHECK_EQ(fx0.basemapCreateThread(), guiThread);
    CHECK_EQ(fx0.infoCreateThread(), guiThread);

    g_atUnmap = AtUnmap{};
    g_atUnmapRan.store(false);
    Access::setUnmapHook(&atUnmapHook);
    Access::startControl(a);
    send(a, FOXAPI_OP_PLUGIN_RESCAN, std::string());
    // The frame polls; the pass releases (the frame applies the release), unmaps, scans, posts the rebuild (the frame applies it).
    CHECK(framesUntil(a, [&] { return g_atUnmapRan.load(); }, 6000));
    CHECK(framesUntil(a, [&] {
        const Fx f = fxNow(g_fixturePath);
        return f.mapped && Access::basemap(a).active() && Access::trackInfo(a).active() && Access::ui(a).trackCount() == 3u;
    }, 8000));
    Access::stopControl(a);
    Access::setUnmapHook(nullptr);

    std::printf("      at the unmap: %ld handle(s) alive, basemap %ld, track info %ld\n", g_atUnmap.live, g_atUnmap.basemapLive,
                g_atUnmap.infoLive);
    CHECK_EQ(g_atUnmap.runs, 1);
    CHECK_EQ(g_atUnmap.live, 0);                       // nothing alive when the module went
    CHECK_EQ(g_atUnmap.basemapLive, 0);
    CHECK_EQ(g_atUnmap.infoLive, 0);
    CHECK(g_atUnmap.ranOn != guiThread);               // the unmapping thread is the pass's...
    CHECK_EQ(g_atUnmap.basemapDestroyedOn, guiThread); // ...and every release was the window's
    CHECK_EQ(g_atUnmap.infoDestroyedOn, guiThread);
    CHECK_EQ(g_atUnmap.destroys, g_atUnmap.creates);
    const Fx fx1 = env.fx();
    CHECK(fx1.mapped);
    CHECK_EQ(fx1.wrongThread(), 0);
    CHECK_EQ(fx1.staleCalls(), 0);
    CHECK_EQ(fx1.basemapCreateThread(), guiThread);    // the reloaded module's clients were attached by the window too
    CHECK_EQ(fx1.infoCreateThread(), guiThread);
}

// ======================================================================================================================
// F. THE CORE'S QUEUE
// ======================================================================================================================
void testCoreQueue() {
    std::printf("  F: the core's legacy-tune queue - one slot per module, newest wins, checked again when applied\n");
    using cascade::core::PluginApiCore;
    PluginApiCore core;
    core.client("a.dll", "A");
    core.client("b.dll", "B");
    core.setLiveSet({"a.dll", "b.dll"});
    core.setTuneGranted("a.dll", true);
    core.setTuneGranted("b.dll", true);

    CHECK_EQ(core.queueCentreRetune("a.dll", 100.0e6), CASCADE_TUNE_OK);
    CHECK_EQ(core.queueCentreRetune("a.dll", 101.0e6), CASCADE_TUNE_OK);   // replaces
    CHECK_EQ(core.queueCentreRetune("b.dll", 102.0e6), CASCADE_TUNE_OK);
    CHECK_EQ(core.queueCentreRetune("nobody.dll", 100.0e6), CASCADE_TUNE_FAILED);
    CHECK_EQ(core.queueCentreRetune("a.dll", 0.0), CASCADE_TUNE_OUT_OF_RANGE);
    CHECK_EQ(core.queueCentreRetune("a.dll", std::nan("")), CASCADE_TUNE_OUT_OF_RANGE);
    std::vector<PluginApiCore::CentreRetune> got;
    core.takeCentreRetunes(got);
    CHECK_EQ(got.size(), 2u);
    CHECK(got[0].hz == 101.0e6 && got[1].hz == 102.0e6);
    CHECK(core.centreRetuneStillAllowed(got[0]));
    CHECK(core.centreRetuneStillAllowed(got[1]));
    got.clear();
    core.takeCentreRetunes(got);
    CHECK(got.empty());

    // withdrawn before it is applied
    CHECK_EQ(core.queueCentreRetune("a.dll", 105.0e6), CASCADE_TUNE_OK);
    CHECK_EQ(core.queueCentreRetune("b.dll", 106.0e6), CASCADE_TUNE_OK);
    core.takeCentreRetunes(got);
    core.setTuneGranted("a.dll", false);
    core.setStopped({"b.dll"});
    CHECK(!core.centreRetuneStillAllowed(got[0]));      // the grant went
    CHECK(!core.centreRetuneStillAllowed(got[1]));      // the plugin was stopped
    // a module that stops or detaches loses what it had waiting
    core.setTuneGranted("a.dll", true);
    core.setStopped({});
    CHECK_EQ(core.queueCentreRetune("a.dll", 107.0e6), CASCADE_TUNE_OK);
    core.setLiveSet({"b.dll"});
    got.clear();
    core.takeCentreRetunes(got);
    CHECK(got.empty());
    // a full queue answers FAILED, never blocks: sixteen modules is the whole of it
    PluginApiCore many;
    std::vector<std::string> keys;
    for (std::size_t i = 0; i < PluginApiCore::kCentreRetuneQueue + 1; ++i) {
        const std::string k = "m" + std::to_string(i) + ".dll";
        many.client(k, k);
        keys.push_back(k);
    }
    many.setLiveSet(keys);
    int ok = 0, failed = 0;
    for (const std::string& k : keys) { (many.queueCentreRetune(k, 100.0e6) == CASCADE_TUNE_OK ? ok : failed)++; }
    CHECK_EQ(ok, static_cast<int>(PluginApiCore::kCentreRetuneQueue));
    CHECK_EQ(failed, 1);
    // detached core
    core.setAttached(false);
    CHECK_EQ(core.queueCentreRetune("b.dll", 100.0e6), CASCADE_TUNE_FAILED);
}

// ======================================================================================================================
// G. A WINDOW THAT ARRIVES AFTER THE FIRST SCAN (shell retirement R4, the late attach; docs/engine-stage4.md 6.25, survey 2.5)
// ======================================================================================================================
// The window used to install its plugin UI's applier BEFORE the engine's first scan, so the scan's directive built the window's instruments. The
// engine host's order (R7) is the other way: the engine restores, scans and builds - for nobody - and the window comes after. The engine keeps the plugin
// UI's current state; the attach lets go of the instances the engine's own plugin UI built and posts that state to the new front end, which applies it.
// A release is the window's own too (PluginUiPort::release): the shutdown's, before the engine's.
void testLateAttach() {
    std::printf("  G: a window that arrives after the first scan - the engine's own instances are let go, the current state is replayed\n");
    using Host = cascade::gui::TestHostAccess;
    Env env;
    AppWindow& a = env.a();
    const std::string key = fixtureKey();
    const Fx fx0 = env.fx();
    CHECK(fx0.mapped);
    Access::poll(a);
    CHECK_EQ(Access::ui(a).trackCount(), 3u);
    const long held = fx0.live();                         // what a window holds of the fixture: its track source, panel and instrument instances
    const std::size_t instruments0 = Access::ui(a).instruments().size();
    CHECK(held >= 1);
    CHECK(Access::basemap(a).active());
    CHECK(Access::trackInfo(a).active());
    CHECK_EQ(th::pluginUiPort(a).offThreadCalls(), std::uint64_t{0});

    // G1 THE WINDOW'S OWN RELEASE (what run() does before the engine's shutdownRelease): every instance destroyed, the basemap and the track-info
    // client let go, on this thread - and applying the engine's release afterwards finds nothing to do.
    th::pluginUiPort(a).release();
    CHECK_EQ(fx0.live(), 0);
    CHECK_EQ(fx0.basemapLive(), 0);
    CHECK_EQ(fx0.infoLive(), 0);
    CHECK(!Access::basemap(a).active());
    CHECK(!Access::trackInfo(a).active());
    CHECK_EQ(Access::ui(a).trackCount(), 0u);
    th::pluginUiPort(a).release();                        // idempotent
    CHECK_EQ(fx0.live(), 0);

    // G2 NOBODY ATTACHED: the port goes (the engine drives its own plugin UI again), a rescan scans and builds the instances for no window.
    Host::dropPort(a);
    CHECK(!th::engine(a).pluginUiAttached());
    Access::rescan(a);
    const Fx fx1 = env.fx();
    CHECK(fx1.mapped);
    CHECK_EQ(fx1.live(), held);                           // the engine's own plugin UI holds them now
    Access::headless(a).poll();
    CHECK_EQ(Access::headless(a).trackCount(), 3u);
    const long destroysBefore = fx1.destroys();

    // G3 THE WINDOW ARRIVES (the same call its constructor makes). The engine's instances are let go as the window's are made - not held twice - and the
    // state it kept is the window's: the first scan's tracks, instruments, basemap and track-info client.
    Host::attachPort(a);
    CHECK(th::engine(a).pluginUiAttached());
    CHECK_EQ(Access::headless(a).trackCount(), 0u);
    CHECK_EQ(fx1.live(), held);                           // exactly one set
    CHECK(fx1.destroys() > destroysBefore);               // the engine's were destroyed ...
    Access::poll(a);
    CHECK_EQ(Access::ui(a).trackCount(), 3u);             // ... and REPLAYED into the window (without the replay this is 0)
    CHECK_EQ(Access::ui(a).instruments().size(), instruments0);
    CHECK(Access::basemap(a).active());
    CHECK(Access::trackInfo(a).active());
    CHECK_EQ(fx1.basemapLive(), 1);
    CHECK_EQ(fx1.infoLive(), 1);
    CHECK_EQ(fx1.wrongThread(), 0);                       // every create and destroy on the thread that polls
    CHECK_EQ(fx1.staleCalls(), 0);
    CHECK_EQ(th::pluginUiPort(a).offThreadCalls(), std::uint64_t{0});

    // G4 THE REPLAY IS THE CURRENT STATE, NOT THE LAST REBUILD: a plugin stopped while nobody was attached stays stopped for the window that
    // arrives, and a start afterwards (a life-only directive, which rebuilds nothing) brings it up in the window.
    th::pluginUiPort(a).release();
    Host::dropPort(a);
    send(a, FOXAPI_OP_DECODER_STOP, key);
    Access::pass(a);                                      // the stop: a life-only directive, applied to the engine's own plugin UI (nobody attached)
    CHECK_EQ(fx1.live(), 0);
    Host::attachPort(a);
    Access::poll(a);
    CHECK_EQ(Access::ui(a).trackCount(), 0u);             // stopped: the window has no instance of it
    CHECK_EQ(fx1.live(), 0);
    send(a, FOXAPI_OP_DECODER_START, key);
    Access::pass(a);                                      // the start: applied to the WINDOW's plugin UI, inline (no control thread runs)
    Access::poll(a);
    CHECK_EQ(Access::ui(a).trackCount(), 3u);             // started in the window - which needs the stop set the replay carried
    CHECK_EQ(fx1.live(), held);
    CHECK_EQ(fx1.wrongThread(), 0);
    CHECK_EQ(th::pluginUiPort(a).offThreadCalls(), std::uint64_t{0});

    // G6 A REPLAY NEVER CARRIES A POINTER INTO AN UNMAPPED MODULE. The plugins are released and unmapped with no rebuild after (a removal, the first half of
    // a rescan): the engine's kept state is empty from the release on, so a window that arrives now is replayed nothing - not the plugin records of
    // the image that is gone.
    th::pluginUiPort(a).release();
    Host::dropPort(a);
    CHECK(Access::unloadPlugins(a));
    CHECK(!env.fx().mapped);
    Host::attachPort(a);
    Access::poll(a);
    CHECK_EQ(Access::ui(a).trackCount(), 0u);
    CHECK_EQ(Access::ui(a).instruments().size(), std::size_t{0});
    CHECK(!Access::basemap(a).active());
    CHECK(!Access::trackInfo(a).active());
}

// G5. THE EXIT PATH: a window that goes - with or without having run - disconnects its port as its last act. The port says so (one line); it also says
// when it is destroyed while still connected (its owner should have disconnected it), and a window never causes that line.
void testWindowExitDisconnects() {
    std::printf("  G5: a window's destruction disconnects its port (the engine drives its own plugin UI again), once, and the port never has to\n");
    const int said0 = diagLines("plugin ui: the port is disconnected");
    const int late0 = diagLines("the port was destroyed while still connected");
    {
        Env env;
        CHECK(Access::controlRunning(env.a()) == false);
        Access::startControl(env.a());
        CHECK(waitFor([&] { return Access::passes(env.a()) > 3u; }, 3000));
    }   // ~Env stops the control thread; ~AppWindow tears the engine down (the plugin UI released through the applier, inline) and disconnects
    CHECK_EQ(diagLines("plugin ui: the port is disconnected") - said0, 1);
    CHECK_EQ(diagLines("the port was destroyed while still connected") - late0, 0);
    {
        Env env;   // a window that never ran a frame
    }
    CHECK_EQ(diagLines("plugin ui: the port is disconnected") - said0, 2);
    CHECK_EQ(diagLines("the port was destroyed while still connected") - late0, 0);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_plugin_ui_threaded\n");
    isolate("plugin_ui_threaded");
    Access::install();
    g_fixturePath = installFixture();
    g_unmapLog = (g_scratch / "unmap.log").string();
    setEnv("FIXTURE_P3_UNMAP_LOG", g_unmapLog);

    const auto timed = [](const char* name, void (*fn)()) {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        std::printf("      (%s: %.2f s)\n", name,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    };
    // PUMP3_CASES=C,E runs only those (the mutation runs use it to read one case's verdict on its own).
    const char* only = std::getenv("PUMP3_CASES");
    const auto want = [only](const char* name) {
        return only == nullptr || (std::string(",") + only + ",").find(std::string(",") + name + ",") != std::string::npos;
    };
    if (want("F")) { timed("F", &testCoreQueue); }
    if (want("G")) { timed("G", &testLateAttach); }
    if (want("G5")) { timed("G5", &testWindowExitDisconnects); }
    if (want("A")) { timed("A", &testRebuildDuringPolling); }
    if (want("B")) { timed("B", &testNewestWins); }
    if (want("C")) { timed("C", &testStaleReference); }
    if (want("D")) { timed("D", &testRequestTune); }
    if (want("D2")) { timed("D2", &testDopplerChurn); }
    if (want("E")) { timed("E", &testReleaseOnWindowThread); }

    // THE MODULE'S OWN WITNESS: every time its image was unmapped, nothing of it was alive. (The windows are gone by now, and
    // the last unmaps happened with them.)
    {
        std::ifstream in(g_unmapLog);
        std::string line;
        int unmaps = 0;
        int dirty = 0;
        while (std::getline(in, line)) {
            ++unmaps;
            if (line.find("unmapped live=0 ") != 0u) { ++dirty; std::printf("      %s\n", line.c_str()); }
        }
        std::printf("  the module saw itself unmapped %d time(s), %d with a handle still alive\n", unmaps, dirty);
        CHECK(only != nullptr || unmaps >= 6);
        CHECK_EQ(dirty, 0);
    }
    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_plugin_ui_threaded");
}
