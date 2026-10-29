// test_bounded_hooks.cpp - THE BOUNDED RUN'S TWO HOOKS: the host owns the stepping and the measurement, the window is handed two std::functions
// (shell retirement R9, 2026-10-28; docs/engine-stage4.md 6.25; core/bounded_hooks.hpp, engine/host_measure.hpp).
//
// THE CHANGE UNDER TEST. The window used to step the engine's pass, take the measurement run's one step a frame and hand the engine a diagnostic's
// switch and frame number through the engine shell (setFrontEndWaitBeat, stepPass, seamMeasureTick, seamMarkPassStats, seamPassStatsJson,
// seamInstallSource, FrontEndFacts::pluginTestReport / frame). The host owns the engine, so it owns these: main() - or the test host - installs
// BoundedHooks{stepPass, onFrame} in a `--frames` run and never in a product run, and the window wraps them in its own WatchdogPause.
//
//   A  THE GATE. A host for a bounded run (frames >= 0) hands its window both hooks; a host for a real session (frames < 0) hands it none.
//   B  A PRODUCT WINDOW STEPS NOTHING. With no hook the window's step does not touch the engine's pass (passes begun stay where they were), pauses
//      no watchdog, and its end-of-frame hook says "carry on" and tells the engine nothing.
//   C  A BOUNDED WINDOW STEPS THE PASS, part by part, under its own pause. Three steps are one whole pass of a STEPPED control thread (begun and ended
//      each move by one), and each takes one pause of the window's watchdog (and the heartbeat that follows it, so the frame's own gap is still measured:
//      the worst gap of a window that only steps is not zero by the skip a release makes).
//   C2 THE PAUSE IS HELD ACROSS THE WAIT: taken before a long part runs, still held while it runs (was test_pass_control D3, the engine's wait beat).
//   D  THE FRAME THE CATALOGUE REPORT NAMES is the window's, told at the end of each frame, and the report is armed by the host from
//      CASCADE_PLUGIN_TEST - and by nothing else (a hook built with the variable unset arms nothing).
//   E  THE MEASUREMENT RUN is the host's: FOXSDR_MEASURE=run, a short window, driven by the end-of-frame hook over a free-running pass, installs its
//      generator through the pass, marks and reads the pass's figures, writes its result and then says "close" (false) once - the hook is spent.
//
// Break-it (docs/engine-stage4.md 6.25): makeBoundedHooks leaves stepPass empty: C RED (the pass does not move); the test host installs the hooks for a
// real session: A RED; the harness does not tell the engine the frame: D RED; the harness does not mark / read the pass's figures: E RED (no "pass").
//
// HERMETIC: this process's per-user folders and temp are a scratch tree, the telemetry endpoint is a closed local port, the settings are none.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/bounded_hooks.hpp"
#include "core/engine_measure.hpp"
#include "engine/engine.hpp"
#include "engine/host_measure.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "test_check.hpp"

using cascade::engine::Engine;
using cascade::core::PassPart;

namespace cascade::gui {
struct AppWindowTestAccess {
    static void step(AppWindow& a, PassPart part) { a.stepPass(part); }
    static bool endOfFrame(AppWindow& a, int next) { return a.endOfFrameHook(next); }
    static const cascade::core::BoundedClient& clientOf(AppWindow& a) { return a.boundedClient_; }
    static cascade::core::BoundedClient client(AppWindow& a) { return a.boundedClient_; }
    static bool endOfFrameWith(AppWindow& a, int next, const cascade::core::BoundedClient& c) { return a.bounded_.onFrame(next, c); }
    static bool hasStepHook(AppWindow& a) { return static_cast<bool>(a.bounded_.stepPass); }
    static bool hasFrameHook(AppWindow& a) { return static_cast<bool>(a.bounded_.onFrame); }
    static unsigned pauses(AppWindow& a) { return a.watchdog_.pausesTaken(); }
    static double worstGapMs(AppWindow& a) { return a.watchdog_.worstGapMs(); }
    static void beat(AppWindow& a) { a.watchdog_.heartbeat(); }
    // The window's hooks replaced by a set built over its engine with a measurement (what main does for a measurement run).
    static void installHooks(AppWindow& a, cascade::core::BoundedHooks h) { a.bounded_ = std::move(h); }
    static void stall(Engine& e, int ms) { e.controlStallForTestMs_.store(ms); }
    static std::uint64_t begun(Engine& e) { return e.passesBegun(); }
    static std::uint64_t ended(Engine& e) { return e.passesEnded(); }
    static bool reportArmed(Engine& e) { return e.pluginTestReport_.load(); }
    static int reportFrame(Engine& e) { return e.pluginTestFrame_.load(); }
};
}  // namespace cascade::gui

using A = cascade::gui::AppWindowTestAccess;

namespace {

void setEnv(const char* n, const std::string& v) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(n, v.c_str());
    _putenv_s(n, v.c_str());
#else
    ::setenv(n, v.c_str(), 1);
#endif
}

// A. THE GATE
void testTheGate() {
    std::printf("  A: a bounded run's host hands its window both hooks; a real session's hands it none\n");
    for (const int frames : {0, 1, 60}) {
        th::Host h("", false, true, frames);
        const cascade::gui::WindowHost wh = h.windowHost("");
        CHECK(static_cast<bool>(wh.bounded.stepPass));
        CHECK(static_cast<bool>(wh.bounded.onFrame));
    }
    th::Host product("", false, true, -1);
    const cascade::gui::WindowHost wp = product.windowHost("");
    CHECK(!static_cast<bool>(wp.bounded.stepPass));       // nothing to step: the product's pass free-runs
    CHECK(!static_cast<bool>(wp.bounded.onFrame));
    th::BareWindow w(product);
    CHECK(!A::hasStepHook(w));
    CHECK(!A::hasFrameHook(w));
    th::Host bounded("");
    th::BareWindow wb(bounded);
    CHECK(A::hasStepHook(wb));
    CHECK(A::hasFrameHook(wb));
}

// B. A PRODUCT WINDOW STEPS NOTHING
void testProductWindowStepsNothing() {
    std::printf("  B: a window with no hook steps nothing, pauses nothing and tells the engine nothing\n");
    th::Host h("", false, true, -1);
    Engine& e = h.engine();
    cascade::engine::ControlThreadOptions o;
    o.stepped = true;                                      // (a stepped thread would run a part for every step asked: it is asked none)
    o.watchdogThresholdMs = 20000;
    e.startControlThread(o);
    th::BareWindow w(h);
    const std::uint64_t begun0 = A::begun(e);
    const unsigned pauses0 = A::pauses(w);
    A::step(w, PassPart::Front);
    A::step(w, PassPart::Mute);
    A::step(w, PassPart::Back);
    CHECK(A::begun(e) == begun0);                          // the pass did not move
    CHECK(A::pauses(w) == pauses0);                        // the window's watchdog was not paused
    CHECK(A::endOfFrame(w, 41));                           // "carry on" ...
    CHECK(A::reportFrame(e) == 0);                         // ... and the engine was told nothing
    e.stopControlThread();
}

// C. A BOUNDED WINDOW STEPS THE PASS, part by part, under its own pause
void testBoundedWindowSteps() {
    std::printf("  C: a bounded window's three steps are one pass of a stepped control thread, each under the window's pause\n");
    th::Host h("");
    Engine& e = h.engine();
    cascade::engine::ControlThreadOptions o;
    o.stepped = true;
    o.watchdogThresholdMs = 20000;
    e.startControlThread(o);
    th::BareWindow w(h);
    const std::uint64_t begun0 = A::begun(e);
    const std::uint64_t ended0 = A::ended(e);
    const unsigned pauses0 = A::pauses(w);
    constexpr int kPasses = 5;
    for (int i = 0; i < kPasses; ++i) {
        A::step(w, PassPart::Front);
        CHECK(A::begun(e) == begun0 + static_cast<std::uint64_t>(i) + 1u);   // a pass begins with its front part ...
        CHECK(A::ended(e) == ended0 + static_cast<std::uint64_t>(i));        // ... and is not over until its back part ends
        A::step(w, PassPart::Mute);
        A::step(w, PassPart::Back);
        CHECK(A::ended(e) == ended0 + static_cast<std::uint64_t>(i) + 1u);
    }
    CHECK(A::pauses(w) == pauses0 + 3u * kPasses);         // one pause of the window's own watchdog per step
    // The pause's release makes the next heartbeat skip its gap (a pause is an excused stall); the step gives the watchdog the heartbeat that takes that
    // skip, so the frame's own beat still measures a gap (tests/test_diag_hang.cpp reads "worst frame gap" from a bounded run and wants a number): a
    // frame that beats, steps, waits 25 ms and beats again has a worst gap of at least 25 ms - not the zero a left-over skip would make of it.
    A::beat(w);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    A::step(w, PassPart::Front);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    A::beat(w);
    CHECK(A::worstGapMs(w) >= 20.0);
    A::step(w, PassPart::Mute);
    A::step(w, PassPart::Back);
    e.stopControlThread();
}

// C2. THE PAUSE IS HELD ACROSS THE WAIT (was test_pass_control D3: "a front end blocked in stepPass beats its watchdog from the wait" - the engine's
// wait beat, Engine::setFrontEndWaitBeat, is gone with R9). A long front part: the window's watchdog is paused BEFORE the part and still for as long as it
// runs - taken at the start of the wait, not after it - and let go of when it returns.
void testPauseHeldAcrossALongPart() {
    std::printf("  C2: the window's pause is taken before a long part and held for all of it\n");
    th::Host h("");
    Engine& e = h.engine();
    cascade::engine::ControlThreadOptions o;
    o.stepped = true;
    o.watchdogThresholdMs = 20000;
    e.startControlThread(o);
    th::BareWindow w(h);
    A::stall(e, 200);                                       // the front part sleeps 200 ms before it runs
    const unsigned before = A::pauses(w);
    std::atomic<bool> returned{false};
    std::thread stepper([&] {
        A::step(w, PassPart::Front);
        returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(!returned.load());                                // still inside the part ...
    CHECK(A::pauses(w) == before + 1u);                     // ... and the pause is already taken (one: this step's)
    stepper.join();
    CHECK(returned.load());
    A::stall(e, 0);
    A::step(w, PassPart::Mute);
    A::step(w, PassPart::Back);
    e.stopControlThread();
}

// D. THE FRAME THE CATALOGUE REPORT NAMES
void testTheReportsFrame() {
    std::printf("  D: the end of each frame tells the engine the frame about to begin; the report is armed by the host from CASCADE_PLUGIN_TEST only\n");
    setEnv("CASCADE_PLUGIN_TEST", "");
    {
        th::Host h("");
        th::BareWindow w(h);
        CHECK(!A::reportArmed(th::engine(w)));              // a host built with the variable unset arms nothing
        CHECK(A::endOfFrame(w, 12));
        CHECK(A::reportFrame(th::engine(w)) == 12);         // ... but the frame is told all the same (the report, if armed, would name it)
        CHECK(A::endOfFrame(w, 13));
        CHECK(A::reportFrame(th::engine(w)) == 13);
    }
    setEnv("CASCADE_PLUGIN_TEST", "http://127.0.0.1:9/catalogue.json");
    {
        th::Host h("");
        th::BareWindow w(h);
        CHECK(A::reportArmed(th::engine(w)));               // the variable named an address when the hooks were built
    }
    setEnv("CASCADE_PLUGIN_TEST", "");
    {
        // A real session's host never builds the hooks, so the variable can arm nothing there.
        setEnv("CASCADE_PLUGIN_TEST", "http://127.0.0.1:9/catalogue.json");
        th::Host h("", false, true, -1);
        th::BareWindow w(h);
        CHECK(!A::reportArmed(th::engine(w)));
        setEnv("CASCADE_PLUGIN_TEST", "");
    }
}

// E. THE MEASUREMENT RUN IS THE HOST'S
void testTheMeasurementRun(const std::filesystem::path& scratch) {
    std::printf("  E: FOXSDR_MEASURE=run, driven by the end-of-frame hook over a free-running pass, writes its result with the pass's figures and ends once\n");
    const std::string out = (scratch / "measure.json").string();
    setEnv("FOXSDR_MEASURE", "run");
    setEnv("FOXSDR_MEASURE_OUT", out);
    setEnv("FOXSDR_MEASURE_SECONDS", "2.5");
    setEnv("FOXSDR_MEASURE_WARMUP", "1");
    std::unique_ptr<cascade::core::EngineMeasure> measure = cascade::core::EngineMeasure::fromEnvironment();
    setEnv("FOXSDR_MEASURE", "");
    CHECK(measure != nullptr);
    if (measure == nullptr) { return; }
    th::Host h("");
    Engine& e = h.engine();
    cascade::engine::ControlThreadOptions o;               // a measurement times the product's own arrangement: the pass free-runs
    o.period = std::chrono::milliseconds(16);
    o.watchdogThresholdMs = 20000;
    e.startControlThread(o);
    th::BareWindow w(h);
    A::installHooks(w, cascade::engine::makeBoundedHooks(e, std::move(measure)));
    // The window's own client, with a count of the waits the harness brackets with blocked(): the pause is held for the length of a wait the host makes,
    // and for no other time - a pause on every end-of-frame call would drop every frame's gap.
    cascade::core::BoundedClient client = A::client(w);
    int blockedOn = 0, blockedOff = 0;
    const std::function<void(bool)> windowBlocked = client.blocked;
    CHECK(static_cast<bool>(windowBlocked));
    client.blocked = [&](bool on) {
        (on ? blockedOn : blockedOff)++;
        windowBlocked(on);
    };
    const unsigned pauses0 = A::pauses(w);
    const auto t0 = std::chrono::steady_clock::now();
    int frames = 0;
    bool more = true;
    while (more && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30)) {
        more = A::endOfFrameWith(w, ++frames, client);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    CHECK(!more);                                          // the measurement finished and said "close"
    CHECK(A::endOfFrameWith(w, frames + 1, client));       // ... once: the hook is spent, the window is not asked again
    e.stopControlThread();
    std::printf("      %d frames; the harness waited for the pass %d time(s) (the generator's install) and the window held its pause for exactly those\n", frames, blockedOn);
    CHECK(blockedOn >= 1);                                 // the source was installed through the pass, one whole pass awaited
    CHECK(blockedOn == blockedOff);                        // every wait released
    CHECK(A::pauses(w) == pauses0 + static_cast<unsigned>(blockedOn));   // ... and no other frame took a pause
    std::ifstream in(out);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string json = ss.str();
    std::printf("      %d frames; result %zu bytes\n", frames, json.size());
    CHECK(json.find("\"format\": \"foxsdr-measure/2\"") != std::string::npos);
    CHECK(json.find("\"mode\": \"run\"") != std::string::npos);
    CHECK(json.find("\"error\": \"\"") != std::string::npos);
    CHECK(json.find("\"inputRateHz\": 2048000") != std::string::npos);   // the generator was installed THROUGH THE PASS and the chain follows its rate
    CHECK(json.find("\"pass\": {") != std::string::npos);                // the harness marked and read the pass's figures
    CHECK(json.find("\"ticks\":") != std::string::npos);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_bounded_hooks\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const auto scratch = std::filesystem::temp_directory_path() / ("foxsdr_bounded_hooks_" + std::to_string(pid));
    std::filesystem::create_directories(scratch);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    testTheGate();
    testProductWindowStepsNothing();
    testBoundedWindowSteps();
    testPauseHeldAcrossALongPart();
    testTheReportsFrame();
    testTheMeasurementRun(scratch);
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_bounded_hooks");
}
