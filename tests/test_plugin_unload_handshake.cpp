// test_plugin_unload_handshake.cpp - THE ENGINE UNMAPS A PLUGIN MODULE ONLY AFTER THE WINDOW HAS LET GO OF EVERYTHING IT BORROWED
// FROM IT (the pump move, step P3; owner decision 2: bounded 250 ms, the control thread's watchdog paused, no unload on a timeout,
// retried; the wait also ends when the control thread is told to stop).
//
// THE HANDSHAKE. Before a rescan, a removal or the shutdown unmaps the modules, the engine's pass posts a RELEASE directive - "let
// go of everything borrowed from the modules, epoch N" - and waits for the window's frame to apply it: the window destroys the
// plugin UI's instances, lets go of the basemap's and the track-info client's, and acknowledges. Only then does the pass unmap.
// These tests run it on a real window (the test's own thread is the window's frame), a real engine with its pass on its own thread,
// and the instrumented module of tests/fixture_pump3_plugin.cpp.
//
//   H1  THE WINDOW IS INSIDE THE PLUGIN WHEN THE RELEASE IS POSTED. The frame is held inside the module's poll_tracks; the pass
//       waits; nothing is destroyed and nothing unmapped for as long as the frame is inside; it comes out, applies the release at its
//       next head of poll, and the module is unmapped after, with nothing alive and no destroy beside a poll.
//   H2  THE WINDOW IS NOT FRAMING. The wait ends after its bound (about 250 ms), the module is NOT unmapped (its instance is alive),
//       the error line is written once, the pass goes on running at its own period, nothing is re-waited; when the frame returns the
//       unload is retried and completes, and the error line is not written again.
//   H3  SHUTDOWN WITH NO WINDOW FRAMING. The control thread is stopped while its pass waits for a window that is not framing: the
//       stop returns at once (the wait's predicate is the loop's run flag), the interrupted unload does not unmap, and the window's
//       own teardown - the GUI thread, inline - releases the plugin UI and unmaps with nothing alive, as it always did.
//   H4  THE CONTROL THREAD'S WATCHDOG IS PAUSED ACROSS THE WAIT (and the pause is released).
//   H5  NO WINDOW AT ALL (the headless engine): nothing to wait for. A rescan from the pass applies the release itself, at once,
//       and unmaps with nothing alive.
//   H6  PluginUi::apply: a directive is the whole of what the plugin UI is built from; a release leaves it holding nothing and does
//       not touch the grants or the stop set of the core (those are the engine's).
//   H7  THE CONTRACT IS WRITTEN DOWN: the ABI header and the plugin author's guide say what a plugin may rely on (request_tune
//       is a queued request, the GUI capabilities' one thread, no unmap while alive).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
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
using Clock = std::chrono::steady_clock;

namespace {

std::string g_fixturePath;
std::string g_unmapLog;

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// ---- what the module looked like at the unmap, taken by the unloading thread just before it ---------------------------------------
struct AtUnmap {
    std::atomic<int> runs{0};
    std::atomic<long> live{-1}, basemapLive{-1}, infoLive{-1}, destroyDuringPoll{-1}, stale{-1};
    std::atomic<bool> gateWasOpen{false};
    std::atomic<std::int64_t> atNs{0};
};
AtUnmap g_unmap;
std::atomic<std::int64_t> g_gateOpenedNs{0};

std::int64_t nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }

void atUnmapHook() {
    const Fx fx = fxNow(g_fixturePath);
    if (!fx.mapped) { return; }
    g_unmap.live.store(fx.live());
    g_unmap.basemapLive.store(fx.basemapLive());
    g_unmap.infoLive.store(fx.infoLive());
    g_unmap.destroyDuringPoll.store(fx.destroyDuringPoll());
    g_unmap.stale.store(fx.staleCalls());
    g_unmap.atNs.store(nowNs());
    g_unmap.runs.fetch_add(1);
}
void resetUnmap() {
    g_unmap.runs.store(0);
    g_unmap.live.store(-1);
    g_unmap.basemapLive.store(-1);
    g_unmap.infoLive.store(-1);
    g_unmap.destroyDuringPoll.store(-1);
    g_unmap.stale.store(-1);
    g_unmap.atNs.store(0);
    g_gateOpenedNs.store(0);
}

// A window that polls like a frame, on a thread of its own, until told to stop; its polls can be paused between frames.
struct Frames {
    AppWindow& a;
    std::atomic<bool> run{true};
    std::atomic<bool> paused{false};
    std::atomic<long> frames{0};
    std::atomic<std::size_t> tracks{0};   // what the frame thread saw after its latest poll (the plugin UI is its alone)
    std::thread t;
    explicit Frames(AppWindow& win) : a(win) {
        t = std::thread([this] {
            while (run.load()) {
                if (!paused.load()) {
                    Access::poll(a);
                    tracks.store(Access::ui(a).trackCount());
                    frames.fetch_add(1);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    ~Frames() { stop(); }
    void stop() {
        run.store(false);
        if (t.joinable()) { t.join(); }
    }
};

void rescan(AppWindow& a) { Access::submit(a, cmd::makeText(FOXAPI_OP_PLUGIN_RESCAN, std::string())); }

// ======================================================================================================================
// H1. THE WINDOW IS INSIDE THE PLUGIN WHEN THE RELEASE IS POSTED
// ======================================================================================================================
void testWindowInsidePlugin() {
    std::printf("  H1: the window is inside the plugin's poll when the release is posted - nothing is unmapped until it is out\n");
    th::TestWindow app;
    Access::pinPlugins(app);
    Access::poll(app);
    const Fx fx = fxNow(g_fixturePath);
    CHECK(fx.mapped);
    CHECK_EQ(fx.live(), 1);
    fx.reset();
    resetUnmap();
    Access::setUnmapHook(&atUnmapHook);
    Access::startControl(app);

    // The frame thread enters the module and is HELD there.
    fx.setGate(1);
    Frames frames(app);
    CHECK(waitFor([&] { return fx.inPoll() == 1; }, 4000));
    const std::uint64_t posted0 = Access::link(app).postedEpoch();
    rescan(app);
    CHECK(waitFor([&] { return Access::link(app).postedEpoch() > posted0; }, 4000));   // the release is posted...
    std::this_thread::sleep_for(std::chrono::milliseconds(110));                        // ...and the frame is still inside
    CHECK_EQ(fx.inPoll(), 1);
    CHECK_EQ(g_unmap.runs.load(), 0);                  // nothing unmapped
    CHECK_EQ(fx.destroys(), 0);                        // nothing destroyed
    CHECK_EQ(fx.live(), 1);
    CHECK(fxNow(g_fixturePath).mapped);
    CHECK(Access::link(app).hasPending());

    // The frame leaves the module (inside the 250 ms bound) and comes back round to its next head of poll.
    g_gateOpenedNs.store(nowNs());
    fx.setGate(0);
    CHECK(waitFor([&] { return g_unmap.runs.load() == 1; }, 6000));
    CHECK(g_unmap.atNs.load() > g_gateOpenedNs.load());      // the unmap came after the frame came out
    CHECK_EQ(g_unmap.live.load(), 0);                        // with nothing alive,
    CHECK_EQ(g_unmap.basemapLive.load(), 0);                 // not the basemap,
    CHECK_EQ(g_unmap.infoLive.load(), 0);                    // not the track-info client,
    CHECK_EQ(g_unmap.destroyDuringPoll.load(), 0);           // and nothing destroyed while the frame was in a poll
    CHECK_EQ(g_unmap.stale.load(), 0);
    // The rescan then completes and the plugin UI is rebuilt over the reloaded module.
    CHECK(waitFor([&] {
        const Fx f = fxNow(g_fixturePath);
        return f.mapped && frames.tracks.load() == 3u && !Access::unloadPending(app);
    }, 8000));
    frames.stop();
    Access::stopControl(app);
    Access::setUnmapHook(nullptr);
    CHECK_EQ(g_unmap.runs.load(), 1);
}

// ======================================================================================================================
// H2. THE WINDOW IS NOT FRAMING
// ======================================================================================================================
void testNotFraming() {
    std::printf("  H2: the window is not framing - the wait is bounded, nothing is unmapped, the error is said once, the unload is retried\n");
    th::TestWindow app;
    Access::pinPlugins(app);
    Access::poll(app);
    const Fx fx = fxNow(g_fixturePath);
    CHECK_EQ(fx.live(), 1);
    resetUnmap();
    Access::setUnmapHook(&atUnmapHook);
    const int said0 = diagLines("did not release the plugin UI");
    Access::startControl(app);

    const std::uint64_t posted0 = Access::link(app).postedEpoch();
    const auto t0 = Clock::now();
    rescan(app);
    CHECK(waitFor([&] { return Access::link(app).postedEpoch() > posted0; }, 4000));
    const auto posted = Clock::now();
    CHECK(waitFor([&] { return Access::unloadPending(app); }, 4000));       // the pass gave up and deferred the unload
    const double waited = msSince(posted);
    std::printf("      gave up after about %.0f ms (bound %d)\n", waited, static_cast<int>(Engine::kPluginUiHandshake.count()));
    CHECK(waited >= 200.0);                                                   // it did wait for the bound...
    CHECK(waited < 1500.0);                                                   // ...and not for much longer
    (void)t0;
    // NOT UNLOADED: the module is mapped, its instance alive, the hook never reached.
    CHECK(fxNow(g_fixturePath).mapped);
    CHECK_EQ(fx.live(), 1);
    CHECK_EQ(fx.destroys(), 0);
    CHECK_EQ(g_unmap.runs.load(), 0);
    // THE ERROR LINE, once.
    CHECK_EQ(diagLines("did not release the plugin UI") - said0, 1);
    // THE PASS DOES NOT GO ON WAITING: it runs at its own period meanwhile, and does not re-ask while the window is silent.
    const std::uint64_t passes0 = Access::passes(app);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const std::uint64_t passes1 = Access::passes(app);
    std::printf("      %llu passes in 400 ms with the window silent\n", static_cast<unsigned long long>(passes1 - passes0));
    CHECK(passes1 - passes0 >= 8u);   // (a pass that re-waited the bound each time would manage one or two)
    CHECK_EQ(diagLines("did not release the plugin UI") - said0, 1);
    CHECK_EQ(g_unmap.runs.load(), 0);
    CHECK(Access::unloadPending(app));

    // THE WINDOW FRAMES AGAIN: the retry goes through.
    Frames frames(app);
    CHECK(waitFor([&] { return g_unmap.runs.load() == 1; }, 8000));
    CHECK_EQ(g_unmap.live.load(), 0);
    CHECK(waitFor([&] {
        const Fx f = fxNow(g_fixturePath);
        return f.mapped && frames.tracks.load() == 3u && !Access::unloadPending(app);
    }, 8000));
    CHECK_EQ(diagLines("did not release the plugin UI") - said0, 1);        // not said again
    frames.stop();
    Access::stopControl(app);
    Access::setUnmapHook(nullptr);
    CHECK_EQ(g_unmap.runs.load(), 1);
}

// ======================================================================================================================
// H3. SHUTDOWN WITH NO WINDOW FRAMING
// ======================================================================================================================
void testShutdown() {
    std::printf("  H3: shutdown with no window framing - the stop does not wait out the bound, the teardown releases itself\n");
    auto app = std::make_unique<th::TestWindow>();
    Access::pinPlugins(*app);
    Access::poll(*app);
    const Fx fx = fxNow(g_fixturePath);
    CHECK_EQ(fx.live(), 1);
    resetUnmap();
    Access::setUnmapHook(&atUnmapHook);
    Access::startControl(*app);

    const std::uint64_t posted0 = Access::link(*app).postedEpoch();
    rescan(*app);
    CHECK(waitFor([&] { return Access::link(*app).postedEpoch() > posted0; }, 4000));   // the pass is now waiting for the window
    const auto t0 = Clock::now();
    Access::stopControl(*app);                         // the control thread is told to stop, mid-wait
    const double stopMs = msSince(t0);
    std::printf("      the control thread stopped in %.1f ms with its pass inside the wait (bound %d ms, join bound %d ms)\n", stopMs,
                static_cast<int>(Engine::kPluginUiHandshake.count()), static_cast<int>(Engine::kControlJoinWait.count()));
    CHECK(stopMs < 200.0);   // (a stop that waited out the bound would read 250+; this one is the 5 ms polling slice)
    CHECK(!Access::controlRunning(*app));
    // The interrupted unload did not unmap anything.
    CHECK_EQ(g_unmap.runs.load(), 0);
    CHECK(fxNow(g_fixturePath).mapped);
    CHECK_EQ(fx.live(), 1);
    // The window's own teardown: the GUI thread, no control thread - the release is applied inline and the module unmapped, empty.
    app.reset();
    CHECK(!fxNow(g_fixturePath).mapped);
    CHECK_EQ(g_unmap.runs.load(), 1);
    CHECK_EQ(g_unmap.live.load(), 0);
    CHECK_EQ(g_unmap.basemapLive.load(), 0);
    CHECK_EQ(g_unmap.infoLive.load(), 0);
    Access::setUnmapHook(nullptr);
}

// ======================================================================================================================
// H4. THE WATCHDOG IS PAUSED ACROSS THE WAIT
// ======================================================================================================================
void testWatchdogPause() {
    std::printf("  H4: the control thread's watchdog is paused across the wait, and the pause is released\n");
    th::TestWindow app;
    Access::pinPlugins(app);
    Access::poll(app);
    std::error_code mk;
    std::filesystem::create_directories(g_scratch / "wd", mk);
    Access::startControl(app, std::chrono::milliseconds(2), (g_scratch / "wd").string(), 100u);
    const unsigned pauses0 = Access::watchdogPauses(app);
    const unsigned reports0 = Access::watchdogReports(app);
    const std::uint64_t posted0 = Access::link(app).postedEpoch();
    rescan(app);
    CHECK(waitFor([&] { return Access::link(app).postedEpoch() > posted0; }, 4000));
    CHECK(waitFor([&] { return Access::unloadPending(app); }, 4000));        // the wait is over (it ran out)
    const unsigned pauses1 = Access::watchdogPauses(app);
    std::printf("      pauses taken for one rescan that waited: %u\n", pauses1 - pauses0);
    // Two: rescanPlugins' own (it always held one) and the handshake's - the mutant that drops the latter reads 1.
    CHECK_EQ(pauses1 - pauses0, 2u);
    CHECK_EQ(Access::watchdogReports(app), reports0);  // ...so a 250 ms silence against a 100 ms threshold is not a hang
    // Released: the passes carry on beating, and nothing was reported afterwards either.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_EQ(Access::watchdogReports(app), reports0);
    Frames frames(app);
    CHECK(waitFor([&] { return !Access::unloadPending(app) && frames.tracks.load() == 3u; }, 8000));
    frames.stop();
    Access::stopControl(app);
}

// ======================================================================================================================
// H5. NO WINDOW AT ALL
// ======================================================================================================================
void testHeadless() {
    std::printf("  H5: no window - the engine releases and unmaps its own plugin UI at once, nothing to wait for\n");
    Engine e;
    e.initialise();
    const Fx fx = fxNow(g_fixturePath);
    CHECK(fx.mapped);
    CHECK_EQ(fx.live(), 1);                  // the engine built it itself, over its own plugin UI
    resetUnmap();
    Access::setUnmapHook(&atUnmapHook);
    e.startControlThread(cascade::engine::ControlThreadOptions{std::chrono::milliseconds(2), std::string(), 5000u});
    const auto t0 = Clock::now();
    e.submitCommand(cmd::makeText(FOXAPI_OP_PLUGIN_RESCAN, std::string()).c);
    CHECK(waitFor([&] { return g_unmap.runs.load() == 1; }, 6000));
    const double ms = msSince(t0);
    std::printf("      released and unmapped in %.0f ms, with no window (bound %d)\n", ms, static_cast<int>(Engine::kPluginUiHandshake.count()));
    CHECK(ms < 200.0);                       // it did not wait the bound
    CHECK_EQ(g_unmap.live.load(), 0);
    CHECK(waitFor([&] { return fxNow(g_fixturePath).mapped && fxNow(g_fixturePath).live() == 1; }, 8000));
    e.stopControlThread();
    Access::setUnmapHook(nullptr);
    CHECK_EQ(fxNow(g_fixturePath).wrongThread(), 0);
}

// ======================================================================================================================
// H8. WHAT THE HANDSHAKE COSTS WHEN THE WINDOW IS FRAMING
// ======================================================================================================================
void testLatency() {
    std::printf("  H8: what the handshake costs when the window is framing (twelve rescans, the frame thread polling every millisecond)\n");
    th::TestWindow app;
    Access::pinPlugins(app);
    Access::poll(app);
    const Fx fx0 = fxNow(g_fixturePath);
    CHECK_EQ(fx0.live(), 1);
    Access::startControl(app);
    Frames frames(app);
    std::vector<double> ack, total;
    for (int round = 0; round < 12; ++round) {
        const std::uint64_t posted0 = Access::link(app).postedEpoch();
        const auto t0 = Clock::now();
        rescan(app);
        // The first post after the press is the release.
        std::uint64_t release = 0;
        while (Clock::now() - t0 < std::chrono::milliseconds(4000)) {
            release = Access::link(app).postedEpoch();
            if (release > posted0) { break; }
            std::this_thread::yield();
        }
        const auto tPost = Clock::now();
        CHECK(release > posted0);
        while (Clock::now() - tPost < std::chrono::milliseconds(4000) && Access::link(app).appliedEpoch() < release) {
            std::this_thread::yield();
        }
        ack.push_back(msSince(tPost));
        // ...and the rest of the rescan: unmap, scan, load, rebuild directive applied.
        CHECK(waitFor([&] {
            return Access::link(app).appliedEpoch() == Access::link(app).postedEpoch() && frames.tracks.load() == 3u &&
                   !Access::unloadPending(app) && Access::link(app).postedEpoch() > release;
        }, 6000));
        total.push_back(msSince(t0));
    }
    frames.stop();
    Access::stopControl(app);
    std::sort(ack.begin(), ack.end());
    std::sort(total.begin(), total.end());
    std::printf("      release posted -> acknowledged: median %.1f ms, max %.1f ms; whole rescan press -> rebuilt: median %.1f ms, max %.1f ms\n",
                ack[ack.size() / 2], ack.back(), total[total.size() / 2], total.back());
    CHECK(ack[ack.size() / 2] < 100.0);   // a frame, not the bound
    CHECK(ack.back() < 2.0 * static_cast<double>(Engine::kPluginUiHandshake.count()));   // (a loaded machine may deschedule the frame thread once)
}

// ======================================================================================================================
// H6. PluginUi::apply
// ======================================================================================================================
void testApply() {
    std::printf("  H6: PluginUi::apply - a directive is the whole of what the plugin UI is built from; a release leaves it empty\n");
    th::TestWindow app;
    Access::pinPlugins(app);
    const Fx fx = fxNow(g_fixturePath);
    cascade::core::PluginApiCore& core = Access::core(app);
    cascade::core::PluginUi& ui = Access::ui(app);
    const std::string key = fixtureKey();
    CHECK_EQ(ui.trackCount(), 0u);                         // (nothing polled yet)
    Access::poll(app);
    CHECK_EQ(ui.trackCount(), 3u);
    core.setTuneGranted(key, true);
    core.setStopped({"someone-else.dll"});

    cascade::core::PluginDirective release;
    release.release = true;
    release.stopped = {"x.dll"};
    ui.apply(release);
    CHECK_EQ(fx.live(), 0);
    CHECK_EQ(ui.trackCount(), 0u);
    CHECK(ui.trackPluginNames().empty());
    CHECK(ui.tracks().empty());
    CHECK(ui.isStopped("x.dll"));                          // the window's local stop set follows the directive...
    CHECK(core.tuneGranted(key));                          // ...but the engine's grants and stop set are not applying's business
    CHECK(core.isStopped("someone-else.dll"));
    CHECK(!core.isStopped("x.dll"));
    // A rebuild directive over the same plugins builds again; the services it carries are the ones plugins call.
    cascade::core::PluginDirective d;
    d.plugins = Access::loaded(app);
    d.services.centreHz = [] { return 123.0e6; };
    ui.apply(d);
    CHECK_EQ(fx.live(), 1);
    CHECK_EQ(ui.trackCount(), 0u);                         // (built; not polled)
    CHECK_NEAR(fx.centre(), 123.0e6, 0.5);                 // the plugin's centre_hz() answers from the services applied
    ui.poll();
    CHECK_EQ(ui.trackCount(), 3u);
    ui.apply(release);
    CHECK_EQ(fx.live(), 0);
    CHECK_EQ(fx.staleCalls(), 0);
}

// ======================================================================================================================
// H7. THE CONTRACT IS WRITTEN DOWN
// ======================================================================================================================
std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
void testContractText() {
    std::printf("  H7: the ABI header and the plugin author's guide state the contract\n");
    const std::filesystem::path root(CASCADE_SOURCE_DIR);
    const std::string abi = slurp(root / "src" / "core" / "plugin_abi.h");
    const std::string guide = slurp(root / "docs" / "PLUGIN-API.md");
    CHECK(!abi.empty());
    CHECK(!guide.empty());
    for (const char* phrase : {"request_tune IS A REQUEST THE HOST ACCEPTS", "CASCADE_TUNE_OK (0) now means ACCEPTED",
                               "within about\n * 16 ms", "REPLACES it", "THE GUI CAPABILITIES", "ONE host thread",
                               "A host\n * never unmaps a module while one of these instances"}) {
        std::string p = phrase;
        // The header is CRLF on this checkout: compare with its line ends normalised.
        std::string text = abi;
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        const bool found = text.find(p) != std::string::npos;
        if (!found) { std::printf("      MISSING from plugin_abi.h: %s\n", phrase); }
        CHECK(found);
    }
    std::string g = guide;
    g.erase(std::remove(g.begin(), g.end(), '\r'), g.end());
    for (const char* phrase : {"`request_tune` is a\nrequest the host accepts", "Which thread your GUI capabilities run on",
                               "unmaps only after that thread says it has"}) {
        const bool found = g.find(phrase) != std::string::npos;
        if (!found) { std::printf("      MISSING from PLUGIN-API.md: %s\n", phrase); }
        CHECK(found);
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_plugin_unload_handshake\n");
    isolate("plugin_unload");
    Access::install();
    g_fixturePath = installFixture();
    g_unmapLog = (g_scratch / "unmap.log").string();
    setEnv("FIXTURE_P3_UNMAP_LOG", g_unmapLog);

    testWindowInsidePlugin();
    testNotFraming();
    testShutdown();
    testWatchdogPause();
    testHeadless();
    testLatency();
    testApply();
    testContractText();

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
        CHECK(unmaps >= 20);
        CHECK_EQ(dirty, 0);
    }
    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_plugin_unload_handshake");
}
