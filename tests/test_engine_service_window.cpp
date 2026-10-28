// test_engine_service_window.cpp - THE CONSTRUCTION FLIP: THE HOST BUILDS THE ENGINE, THE WINDOW IS ITS GUEST (shell retirement R7;
// docs/engine-stage4.md 6.25).
//
// THE CHANGE UNDER TEST. The window used to build the engine inside its own constructor (EngineShell::create), restore the settings into it there,
// attach to the plugin UI before the plugin scan, and own it to the end. Now main.cpp - and the test host, tests/test_host.hpp, which is main's
// order for a test - builds an EngineService, restores, and only then constructs the window, which is a GUEST of the engine: its shell is a VIEW
// that installs the window as the engine's host (Engine::setHost, before any control thread) and lets it go again (Engine::resetHost). This test
// holds the three things that change underneath that:
//
//   A  THE HOST OWNS THE ENGINE. The window's engine is the Host's own; the window is installed as the engine's host while it lives and the
//      engine has its own host back when it goes; its plugin UI is attached while it lives and not after.
//   B  THE INSTALL IS REFUSED LOUDLY, NOT SILENTLY. Engine::setHost answers false while the control thread runs (R6's audited guard). A window built
//      on an engine that is already running must not carry on as if it were the engine's host: the constructor throws, with the sentence, says so
//      in the log, and changes nothing about the engine - which then takes a window built after the thread is gone.
//   C  A WINDOW THAT GOES WHILE THE ENGINE STILL RUNS IS DETACHED, NOT DANGLING. The shell's destructor cannot take the host back (resetHost
//      refuses while the control thread runs); the adapter is kept, loudly, and answers nothing - the engine's later pause and resume reach no
//      dead window - until the engine does let go.
//   D  THE START-UP PATH EXERCISES THE REPLAY, and the cost of it to third-party code is exactly what the survey (2.5) said: the restore's scan built
//      the plugin UI for nobody (the engine's own instances: one create each), the window's attach destroys them and the replay builds the window's
//      (a second create each, one destroy) - inside the window's constructor. Before R7 the window attached BEFORE the scan and each GUI-class
//      instance was created once. Every create and destroy is on the thread that polls, the window's and the host's being one thread.
//   F  WHAT THE WINDOW KEEPS OF THE RESTORE. It no longer reads the settings file: its half is the table's (fact.config.interface); the last run's marker
//      (the crash-report offer), the saved I/Q file path (the path box) and the closed Transmit page come from the host's restore.
//   E  THE ENGINE'S CLOCK IS ONE CLOCK. The restore stamps the usage report's session start on the service's host; the window installs its own host
//      afterwards. Every EngineHost measures from the process's first, so the stamp and the reads that follow it are one clock (a host that restarted
//      the clock at its own construction would put the session's length and its mode seconds on two).
//
//   G  THE WINDOW GOES AFTER THE ENGINE HAS BEEN STOPPED (shell retirement R8: the product's order - main stops the service with the window alive and destroys
//      the window after): its session is closed on an engine with no pass left while a key is held; a harmless release, the key let go of once, by the stop.
//   H  THE LAST HALF OF THE SETTINGS REACHES THE FILE: the window's last-second change, handed over by the prelude's flush and applied by the service's final
//      pass, is in the file the stop writes - for the stepped pass and the free-running one; the control run without the flush keeps the loaded value.
//
// Break-it (docs/engine-stage4.md 6.25): the shell's view does not check setHost's answer: B RED (no throw); the destructor does not detach: C RED
// (the engine's pause reaches the dead hooks); Engine::attachPluginUi without its replay: D RED (the window has no instance); the clock per host: E RED.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "core/config.hpp"
#include "core/diag_log.hpp"
#include "core/plugin_api.hpp"
#include "engine/engine.hpp"
#include "engine/engine_host.hpp"
#include "engine/engine_shell.hpp"
#include "engine_api_fake_radio.hpp"
#include "source/tx_sink.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "pump3_support.hpp"
#include "test_check.hpp"

#define CHECK_EQ(a, b) CHECK((a) == (b))

using cascade::engine::Engine;
using Access = cascade::gui::AppWindowTestAccess;
using namespace pump3;

namespace {

// What a window answers: counted, so a test can tell a call that reached it from one that did not.
class CountingHooks final : public cascade::engine::ShellHooks {
public:
    double wallTimeS() const override {
        ++wall;
        return 0.0;
    }
    void pauseWatchdog() override { ++pauses; }
    void resumeWatchdog() override { ++resumes; }
    mutable std::atomic<int> wall{0};
    std::atomic<int> pauses{0};
    std::atomic<int> resumes{0};
};

// ======================================================================================================================
// A. THE HOST OWNS THE ENGINE
// ======================================================================================================================
void testHostOwnsTheEngine() {
    std::printf("  A: the Host's engine is the window's; the window is its host and its plugin UI while it lives, and neither after\n");
    th::Host h("");
    Engine& e = h.engine();
    CHECK(Access::hostIsOwn(e));                // restored: the engine asks its own host
    CHECK(!e.pluginUiAttached());               // ... and has scanned for nobody
    CHECK(!e.controlThreadRunning());
    {
        th::BareWindow w(h);
        CHECK(&th::engine(w) == &e);            // the window's engine IS the Host's
        CHECK(!Access::hostIsOwn(e));           // the window is the engine's host now (the shell's adapter)
        CHECK(e.pluginUiAttached());            // and its plugin UI's front end
    }
    CHECK(Access::hostIsOwn(e));                // the view gave the engine its own host back
    CHECK(!e.pluginUiAttached());               // the port disconnected
}

// ======================================================================================================================
// B. THE INSTALL IS REFUSED LOUDLY
// ======================================================================================================================
void testRefusedWhileTheControlThreadRuns() {
    std::printf("  B: a window built on an engine whose control thread runs is refused - a thrown sentence, a log line, nothing changed\n");
    th::Host h("");
    Engine& e = h.engine();
    Access::startControlOn(e);
    CHECK(e.controlThreadRunning());
    const int said0 = diagLines("could not become the engine's host");
    bool threw = false;
    std::string what;
    try {
        th::BareWindow w(h);
    } catch (const std::runtime_error& ex) {
        threw = true;
        what = ex.what();
    }
    CHECK(threw);                                                                    // loud: not a window that quietly carries on
    CHECK(what.find("control thread is already running") != std::string::npos);     // ... with the reason
    CHECK(what.find("Engine::setHost") != std::string::npos);
    CHECK_EQ(diagLines("could not become the engine's host") - said0, 1);            // ... and in the log
    CHECK(Access::hostIsOwn(e));                                                     // nothing changed: the engine still asks its own host
    CHECK(!e.pluginUiAttached());
    CHECK(e.controlThreadRunning());                                                 // ... and is still running
    e.stopControlThread();
    {
        th::BareWindow w(h);                                                         // the same engine takes a window once its thread is gone
        CHECK(!Access::hostIsOwn(e));
    }
}

// ======================================================================================================================
// C. A WINDOW THAT GOES WHILE THE ENGINE STILL RUNS IS DETACHED, NOT DANGLING
// ======================================================================================================================
void testViewDetachesWhenTheEngineWillNotLetGo() {
    std::printf("  C: a shell destroyed while the control thread runs keeps its adapter, says so, and answers nothing\n");
    th::Host h("");
    Engine& e = h.engine();
    CountingHooks hooks;
    std::unique_ptr<cascade::engine::EngineShell> view = cascade::engine::EngineShell::view(e, hooks);
    CHECK(!Access::hostIsOwn(e));
    // The window answers the watchdog's two questions while installed - and NOT the clock: the engine's clock is the engine's.
    Access::hostPause(e);
    Access::hostResume(e);
    CHECK_EQ(hooks.pauses.load(), 1);
    CHECK_EQ(hooks.resumes.load(), 1);
    (void)Access::hostWall(e);
    CHECK_EQ(hooks.wall.load(), 0);
    Access::startControlOn(e);
    const int said0 = diagLines("the window went while the control thread was still running");
    view.reset();                                                                    // the engine refuses to give the host up
    CHECK_EQ(diagLines("the window went while the control thread was still running") - said0, 1);
    CHECK(!Access::hostIsOwn(e));                                                    // it still holds the (detached) adapter ...
    Access::hostPause(e);
    Access::hostResume(e);
    CHECK_EQ(hooks.pauses.load(), 1);                                                // ... which reaches no window
    CHECK_EQ(hooks.resumes.load(), 1);
    e.stopControlThread();
    CHECK(e.resetHost());                                                            // and the engine takes its own back when it can
    CHECK(Access::hostIsOwn(e));
}

// ======================================================================================================================
// D. THE START-UP PATH EXERCISES THE REPLAY - and what it costs third-party code
// ======================================================================================================================
struct Counts {
    long live = 0;
    long creates = 0;
    long destroys = 0;
    std::size_t tracks = 0;
    bool basemap = false;
    bool trackInfo = false;
    bool mapped = false;
};

// THE NEW ORDER (main.cpp's): the Host restores - the plugin scan runs on this thread, before any window - the plugins are pinned (a fitted plugin runs
// only while used; the product's user keeps one running), and the window attaches after.
Counts newOrder(const std::string& fixturePath, long* builtForNobody) {
    th::Host h("");
    Engine& e = h.engine();
    Access::pinPlugins(e);
    const Fx fx = fxNow(fixturePath);
    CHECK(fx.mapped);
    *builtForNobody = fx.live();                   // what the engine built for nobody before the window: nothing (the directive is kept for the attach)
    CHECK(!e.pluginUiAttached());
    th::BareWindow w(h);                           // the late attach: the replay builds the window's instances in its constructor
    CHECK(e.pluginUiAttached());
    Access::poll(w);                               // (a frame's poll changes nothing)
    CHECK_EQ(fx.wrongThread(), 0);                 // every create and destroy was on the thread that polls
    CHECK_EQ(fx.staleCalls(), 0);
    CHECK_EQ(th::pluginUiPort(w).offThreadCalls(), std::uint64_t{0});
    Counts c;
    c.mapped = fx.mapped;
    c.live = fx.live();
    c.creates = fx.creates();
    c.destroys = fx.destroys();
    c.tracks = Access::ui(w).trackCount();
    c.basemap = Access::basemap(w).active();
    c.trackInfo = Access::trackInfo(w).active();
    return c;
}

// THE OLD ORDER, built on purpose: the window attached BEFORE the scan (what AppWindow's constructor did until R7: openUiSession, attachPluginUi, then
// Engine::initialise). The Host is made without its restore, the window attaches to the bare engine, and the restore's scan then builds straight into the
// window's plugin UI.
Counts oldOrder(const std::string& fixturePath) {
    th::Host h("", false, /*restoreNow=*/false);
    Engine& e = h.engine();
    th::BareWindow w(h);
    CHECK(e.pluginUiAttached());
    (void)h.restore();
    Access::pinPlugins(e);
    Access::poll(w);
    const Fx fx = fxNow(fixturePath);
    CHECK(fx.mapped);
    CHECK_EQ(fx.wrongThread(), 0);
    Counts c;
    c.mapped = fx.mapped;
    c.live = fx.live();
    c.creates = fx.creates();
    c.destroys = fx.destroys();
    c.tracks = Access::ui(w).trackCount();
    c.basemap = Access::basemap(w).active();
    c.trackInfo = Access::trackInfo(w).active();
    return c;
}

void testStartupPathReplays(const std::string& fixturePath) {
    std::printf("  D: restore, then window (the replay) - a plugin counts the same creates and destroys as when the window attached before the scan\n");
    CHECK(fxNow(fixturePath).mapped == false);
    long builtForNobody = -1;
    const Counts fresh = newOrder(fixturePath, &builtForNobody);
    CHECK(fxNow(fixturePath).mapped == false);     // (the engine and the window are gone: the module is unmapped, its counters begin again)
    const Counts old = oldOrder(fixturePath);
    CHECK(fxNow(fixturePath).mapped == false);
    std::printf("      new order: live %ld creates %ld destroys %ld, tracks %zu; old order: live %ld creates %ld destroys %ld, tracks %zu; built for nobody: %ld\n",
                fresh.live, fresh.creates, fresh.destroys, fresh.tracks, old.live, old.creates, old.destroys, old.tracks, builtForNobody);
    CHECK_EQ(builtForNobody, 0);                   // nothing was created for nobody
    CHECK(fresh.live >= 1);                        // the window has its instances ...
    CHECK_EQ(fresh.creates, fresh.live);           // ... each created once ...
    CHECK_EQ(fresh.destroys, 0);                   // ... and nothing destroyed on the way (before the hold this was one create and one destroy more)
    CHECK_EQ(fresh.tracks, 3u);                    // the replay is the first scan's tracks (without it: 0 - the break-it)
    CHECK(fresh.basemap && fresh.trackInfo);
    CHECK_EQ(fresh.live, old.live);                // THE SAME TOTALS AS THE OLD ORDER
    CHECK_EQ(fresh.creates, old.creates);
    CHECK_EQ(fresh.destroys, old.destroys);
    CHECK_EQ(fresh.tracks, old.tracks);
    CHECK(old.basemap && old.trackInfo);
}

// ======================================================================================================================
// E. THE ENGINE'S CLOCK IS ONE CLOCK
// ======================================================================================================================
void testOneClock() {
    std::printf("  E: every host measures from the process's first - a later host (the window's) continues the restore's clock\n");
    cascade::engine::EngineHost first;
    const double t0 = first.wallTimeS();
    std::this_thread::sleep_for(std::chrono::milliseconds(160));
    cascade::engine::EngineHost later;             // a host built later: with a clock of its own it would start again near zero
    CHECK(later.wallTimeS() >= t0 + 0.15);
    // And across the engine's own host swap: the stamp the restore takes and the read after the window installed.
    th::Host h("");
    Engine& e = h.engine();
    const double before = Access::hostWall(e);
    {
        th::BareWindow w(h);
        const double after = Access::hostWall(e);
        CHECK(after >= before);
        CHECK(after - before < 5.0);               // (a window's own clock, glfw's, would be near zero here, or the process's age away)
    }
}

// ======================================================================================================================
// F. WHAT THE WINDOW KEEPS OF THE RESTORE: its half from the table, and the four things only the file said
// ======================================================================================================================
// The window no longer reads the settings file. Its half of the settings is the table's (fact.config.interface, already through the start-up state);
// the last run's marker, the saved source's I/Q file path and the page that is open come from the host's restore (WindowHost). Held on a real file, through
// the test host - main's order - and the window's own members.
void testRestoredIntoTheWindow() {
    std::printf("  F: the window's half comes from the table; the last run's marker and the saved I/Q path from the host's restore\n");
    namespace fs = std::filesystem;
    const fs::path dir = g_scratch / "restored";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const auto write = [&](const char* name, const std::function<void(cascade::core::AppConfig&)>& edit) {
        cascade::core::AppConfig c;
        c.volume = 0.0f;
        edit(c);
        const fs::path p = dir / name;
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << cascade::core::ConfigStore::serialize(c);
        return p.string();
    };
    // An UNCLEAN last run (the marker false) with Diagnostics on: the crash-report offer opens; the saved source is a file, its path in the box; the
    // interface's half (a rail bank, a counter size) is the table's; the Transmit page, saved open, is closed (start-up state).
    {
        const std::string path = write("unclean.json", [](cascade::core::AppConfig& c) {
            c.telemetryCleanExit = false;
            c.diagnosticsEnabled = true;
            c.sourceKind = "file";
            c.iqFilePath = "C:/nowhere/capture.iq";
            c.railBank = 2;
            c.counterScale = 2;
            c.transmitOpen = true;
        });
        th::TestWindow w(path);
        CHECK(w.host().restored().loaded);
        CHECK(w.host().restored().lastRunUnclean);
        CHECK(Access::lastRunUnclean(w));                 // (the carried marker: without it nothing is ever offered after a crash)
        CHECK(Access::diagOfferOpen(w));
        CHECK_EQ(Access::iqPath(w), std::string("C:/nowhere/capture.iq"));   // (the carried path: without it the box starts empty)
        CHECK_EQ(Access::railBank(w), 2);                 // the half from the table
        CHECK_EQ(Access::counterScale(w), 2);
        CHECK(!Access::transmitPageOpen(w));              // the start-up state: no page opens itself
    }
    // A CLEAN last run: no offer. A file whose saved source is not a file: the box stays empty.
    {
        const std::string path = write("clean.json", [](cascade::core::AppConfig& c) {
            c.telemetryCleanExit = true;
            c.diagnosticsEnabled = true;
            c.railBank = 1;
        });
        th::TestWindow w(path);
        CHECK(!w.host().restored().lastRunUnclean);
        CHECK(!Access::lastRunUnclean(w));
        CHECK(!Access::diagOfferOpen(w));
        CHECK_EQ(Access::iqPath(w), std::string());
        CHECK_EQ(Access::railBank(w), 1);
    }
}

// ======================================================================================================================
// G. THE WINDOW GOES AFTER THE ENGINE HAS BEEN STOPPED - the product's order (shell retirement R8)
// ======================================================================================================================
// main.cpp stops the service with the window still alive and destroys the window after: the window's session is closed on an engine whose control
// thread is joined, with a key held at that moment (the survey's 2.4). A close on a stopped engine must be a harmless release - no crash, no second
// unkey, no pass to hang for, and the window's shell gives the engine its own host back (resetHost succeeds because the thread is gone) without the
// "went while the control thread was still running" line of C.
class KeySink final : public cascade::source::IqSink {
public:
    bool start() override {
        running_ = true;
        ++starts;
        return true;
    }
    void stop() override {
        if (running_.exchange(false)) { ++stops; }
    }
    void finish() override { stop(); }
    bool running() const override { return running_; }
    double sampleRateHz() const override { return 2.5e6; }
    bool setSampleRateHz(double) override { return true; }
    double centerFrequencyHz() const override { return 146.52e6; }
    bool setCenterFrequencyHz(double) override { return true; }
    bool frequencyRangeHz(double& lo, double& hi) const override {
        lo = 70.0e6;
        hi = 6.0e9;
        return true;
    }
    bool sampleRateRangeHz(double& lo, double& hi) const override {
        lo = 2.083e6;
        hi = 61.44e6;
        return true;
    }
    bool gainRangeDb(double& lo, double& hi) const override {
        lo = -89.75;
        hi = 0.0;
        return true;
    }
    double gainDb() const override { return 0.0; }
    bool setGainDb(double) override { return true; }
    std::size_t write(const std::complex<float>*, std::size_t n) override { return running_ ? n : 0; }
    bool faulted() const override { return false; }
    const char* name() const override { return "key sink"; }
    const char* lastError() const override { return ""; }
    std::atomic<int> starts{0};
    std::atomic<int> stops{0};

private:
    std::atomic<bool> running_{false};
};

void testWindowGoesAfterTheStop() {
    std::printf("  G: the service stopped with a key held, THEN the window destroyed - a harmless release, the engine's own host back\n");
    th::Host h("");
    Engine& e = h.engine();
    auto sinkOwner = std::make_unique<KeySink>();
    KeySink* sink = sinkOwner.get();
    const int wentRunning0 = diagLines("the window went while the control thread was still running");
    {
        th::BareWindow w(h);
        Access::installSink(w, std::move(sinkOwner));
        std::string why;
        CHECK(h.service().start(why));              // main's start: after the window's constructor
        CHECK(e.controlThreadRunning());
        const auto t0 = std::chrono::steady_clock::now();
        while (!sink->running() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3)) {
            Access::frame(w, true);                 // the window's frames, each re-asserting the PTT on its own session
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(sink->running());                     // THE KEY IS HELD
        h.service().stop();                         // main's stop, with the window alive
        CHECK(!e.controlThreadRunning());
        CHECK(!sink->running());                    // the stop let go of it (the transmitter first of all)
        CHECK_EQ(sink->stops.load(), 1);
        CHECK(e.pluginUiAttached());                // (the window's port is still connected: the prelude was not run by this test)
    }                                               // ... and the window goes: its session is closed on an engine with no pass left
    CHECK_EQ(sink->stops.load(), 1);                // the key was not let go of a second time, by anyone
    CHECK(Access::hostIsOwn(e));                    // the engine's own host is back ...
    CHECK_EQ(diagLines("the window went while the control thread was still running") - wentRunning0, 0);   // ... without the detach of C
    CHECK(!e.pluginUiAttached());                   // and the port is gone
}

// ======================================================================================================================
// H. THE LAST HALF OF THE SETTINGS REACHES THE FILE (shell retirement R8)
// ======================================================================================================================
// The window's change in its last second is in no snapshot the engine holds until the window hands it over; the prelude's flush does, and the service's
// final pass applies it before the final-state save. Held end to end on a real file in the product's order (start, frames, flush, stop), for the STEPPED
// pass (a bounded run: nothing would run the pass that takes the half, so the service makes one) and the FREE-RUNNING one (the product: the submit wakes
// it). The control run flushes nothing and the file keeps the value it was loaded with - so the value in the other file is the flush's doing.
int savedRailBank(const std::string& path, bool* cleanExit) {
    cascade::core::AppConfig c;
    std::string err;
    if (!cascade::core::ConfigStore::load(path, c, err)) { return -1; }
    if (cleanExit != nullptr) { *cleanExit = c.telemetryCleanExit; }
    return c.railBank;
}

void lastHalfRun(const char* name, bool free, bool flush, int* savedBank, bool* clean) {
    namespace fs = std::filesystem;
    const fs::path dir = g_scratch / "lasthalf";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string path = (dir / name).string();
    fs::remove(path, ec);
    {
        cascade::core::AppConfig c;
        c.volume = 0.0f;
        c.railBank = 1;                                      // what the file says; the window starts there
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << cascade::core::ConfigStore::serialize(c);
    }
    pump3::setEnv("FOXSDR_PASS", free ? "free" : "");
    {
        th::Host h(path);                                    // frames = 0: a bounded run's options - stepped unless FOXSDR_PASS=free
        th::BareWindow w(h, path);
        CHECK_EQ(Access::railBank(w), 1);
        std::string why;
        CHECK(h.service().start(why));
        for (int i = 0; i < 5; ++i) {
            Access::frame(w);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Access::setRailBank(w, 3);                           // the last second's change: no look has sent it
        if (flush) { CHECK(Access::flushHalf(w)); }          // the prelude's hand-over
        h.service().stop();
    }
    pump3::setEnv("FOXSDR_PASS", "");
    *savedBank = savedRailBank(path, clean);
}

void testLastHalfReachesTheFile() {
    std::printf("  H: the window's last-second change is in the file the service's stop writes (stepped and free-running); without the hand-over it is not\n");
    for (const bool free : {false, true}) {
        int bank = -2;
        bool clean = false;
        lastHalfRun(free ? "free.json" : "stepped.json", free, /*flush=*/true, &bank, &clean);
        std::printf("      %s pass, flushed: railBank in the file %d (expect 3), clean-exit marker %d\n", free ? "free-running" : "stepped", bank, clean ? 1 : 0);
        CHECK_EQ(bank, 3);
        CHECK(clean);
        lastHalfRun(free ? "free-control.json" : "stepped-control.json", free, /*flush=*/false, &bank, &clean);
        std::printf("      %s pass, NOT flushed: railBank in the file %d (expect 1, the loaded value)\n", free ? "free-running" : "stepped", bank);
        CHECK_EQ(bank, 1);
        CHECK(clean);
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_service_window\n");
    isolate("service_window");
    Access::install();
    const std::string fixturePath = installFixture();
    testHostOwnsTheEngine();
    testRefusedWhileTheControlThreadRuns();
    testViewDetachesWhenTheEngineWillNotLetGo();
    testOneClock();
    testRestoredIntoTheWindow();
    testWindowGoesAfterTheStop();
    testLastHalfReachesTheFile();
    testStartupPathReplays(fixturePath);
    return testSummary("test_engine_service_window");
}
