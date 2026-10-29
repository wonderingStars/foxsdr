// test_host.hpp - THE TEST HOST: THE ONE PLACE A WINDOW TEST REACHES THE ENGINE BEHIND ITS WINDOW (shell retirement, batch R1 = survey step M0, and
// R7 = survey step M1; docs/engine-stage4.md 6.25).
//
// WHY. A hermetic window test is, in truth, a test of the ENGINE written with a window in front of it: 69 test files reached the Engine
// 1,979 times through `rawEngine(a.engine_)` (786 of them private-field reads), each from a local AppWindowTestAccess. The shell was retired (the
// window reaches the engine through the API table and nothing else), and `rawEngine` was the shell's back door, so the tests could not keep a door of
// their own scattered across 69 files: when the Engine's owner changed (R7: the host process owns it, as main does in the product), every one of those
// files would have had to change again. This header is the answer the survey (3.3, 3.4) chose: tests name the engine through ONE function,
// th::engine(window), and build their windows through ONE type, th::TestWindow. The day the owner changed (R7, M1) only this header changed; the files
// that moved here did not; and when the shell and `rawEngine` were deleted (R10, M2, 2026-10-29) again only this header changed.
//
//   th::Host             WHAT main.cpp IS, for a test: an EngineService (hermetic when the window has no settings path, persistent on a scratch
//                        path when it has one), its restore, and the CASCADE_CONFIG_TEST line when asked. It owns the Engine. It does NOT start the
//                        service on its own (most tests drive the pass themselves through the pass seam; th::run below is main's start, run, stop and
//                        closeWindow for the one in-process test that runs a window) and takes no lock (a test window never did). Its options are
//                        the window application's, from the SAME function main.cpp calls (engine/window_service_options.hpp) - R8. Everything a test
//                        builds is in main's order: service, restore, window. A window built for a bounded run (frames >= 0, which every test window
//                        is) is handed the bounded run's two hooks, built by the SAME function main.cpp calls (engine/host_measure.hpp) - R9.
//   th::TestWindow       an AppWindow, built the way `AppWindow app;` / `AppWindow app(path[, announce])` always built one - and built the way
//                        main builds it since R7: the HOST FIRST (base-from-member: the Host is a base that is constructed before the window and
//                        destroyed after it), then the window on the restored engine. It is a distinct type so that every test window is made in
//                        one place - tests/test_engine_shell_guard.cpp holds that no test names `AppWindow` as a thing it constructs.
//   th::BareWindow      an AppWindow on a Host the TEST built and owns, for the tests that must act on the engine before its window exists.
//   th::HostedWindow     the base of both: an AppWindow that knows the Host it sits on (the window itself holds no engine of any kind since R10).
//   th::engine(a)        the Engine the window `a` sits on - the Host's own (what `rawEngine(a.engine_)` was, the same reference). Tests that hold the
//                        window as an `AppWindow&` (every AppWindowTestAccess helper does) use it unchanged: every window a test builds is a HostedWindow
//                        (TestWindow or BareWindow - tests/test_engine_shell_guard.cpp holds that no test constructs an AppWindow another way).
//   th::applyConfig(a,c) what the window's own `applyConfig(c)` was: the start-up state of `c` onto the engine, then the window's half of it from the table
//                        (R7 split the window's applyConfig in two: the engine's half is the host's, applied here; the window's is AppWindow's).
//
// THE AUDIT. `rawEngine` is deleted (R10): nothing names it, in src/ or tests/ (tests/test_engine_shell_guard.cpp holds it, in code). The window has no
// engine member to reach. What TestHostAccess is left with is the window's OWN seams - the plugin UI port, the late attach, the window's half of a
// start-up state - through ONE friend declaration beside AppWindow's AppWindowTestAccess (the survey wanted the friend deleted with rawEngine; these
// seams are window-private and no table call stands in for them, so it stays, and the guard holds it to one declaration and no Engine in it).
// Private FIELDS of the Engine still come through each test's own AppWindowTestAccess/Engine friend, exactly as before - this batch
// moved who hands out the Engine, not what a test may read of it.
//
// Engine::testHooks_ are installed by the test BEFORE any window exists, as today (engine.hpp: "Set by the test before any AppWindow
// exists"); the Host is built by TestWindow's constructor, after the test's hooks, so the order is kept.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include "engine/engine.hpp"
#include "engine/engine_service.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/host_measure.hpp"
#include "engine/plugin_ui_port.hpp"
#include "engine/window_service_options.hpp"
#include "gui/app_window.hpp"

namespace cascade::gui {

// The window's friend for this header and nothing else (src/gui/app_window.hpp: `friend struct TestHostAccess;`). It names no Engine.
struct TestHostAccess {
    // THE PLUGIN UI PORT behind a window (shell retirement R4): what rawPluginUi(*a.pluginFeed_) was, and the two steps a late-attach test needs - the
    // window lets go of its port (the destructor disconnects it, the engine drives its own plugin UI again), and asks for one again (the same call the
    // constructor makes: a window that arrives after the engine's scan).
    static cascade::core::PluginUiPort& port(AppWindow& a) { return *a.ui_; }
    static void dropPort(AppWindow& a) { a.ui_.reset(); }
    static void attachPort(AppWindow& a) { a.attachPluginUi(); }
    // THE WINDOW'S HALF of a start-up state (shell retirement R7): what the constructor does with the table's fact, for a test that restores a config
    // into a window it already has.
    static cascade::core::AppConfig applyWindowHalf(AppWindow& a, const cascade::core::AppConfig& startup) { return a.applyWindowHalf(startup); }
    // The window's half of the settings as it is now, bookkeeping included (what the window's own currentConfig() put over the engine's snapshot until R8 deleted it).
    static void buildInterfaceHalf(AppWindow& a, cascade::core::AppConfig& cfg) { a.buildInterfaceHalf(cfg, /*bookkeeping=*/true); }
};

}  // namespace cascade::gui

namespace th {

// WHAT main.cpp BUILDS BEFORE THE WINDOW (src/main.cpp): the engine's service and its restore. The service is built hermetic - the engine left as it
// was built, which is what every window test has always started from - or persistent on `configPath`; it is restored (the plugin scan, the file's
// load, the start-up state) and, when the run asked for the CASCADE_CONFIG_TEST line, announces it. Nothing is started and no lock is taken.
class Host {
public:
    // `restoreNow` false is for the one test that must build the OLD order - a window attached BEFORE the restore's scan - to compare the new one with
    // (tests/test_engine_service_window.cpp D): it calls restore() itself, later.
    // `frames` is the --frames of the run the host is for (0: a bounded run, the default of every test window; negative: a real session's options, which
    // a test never wants): it decides the service's options exactly as main's --frames does (engine/window_service_options.hpp).
    explicit Host(const std::string& configPath, bool announceConfig = false, bool restoreNow = true, int frames = 0)
        : service_(options(configPath, frames)), announce_(announceConfig), frames_(frames) {
        if (restoreNow) { (void)restore(); }
    }
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    const cascade::engine::EngineService::Restored& restore() {
        restored_ = service_.restore();
        if (announce_ && service_.persistent()) { cascade::engine::EngineService::announce(restored_); }
        return restored_;
    }

    cascade::engine::Engine& engine() { return service_.engine(); }
    cascade::engine::EngineService& service() { return service_; }
    const cascade::engine::EngineService::Restored& restored() const { return restored_; }

    // What AppWindow(WindowHost) is given, as main.cpp fills it.
    cascade::gui::WindowHost windowHost(const std::string& configPath) {
        cascade::gui::WindowHost h;
        h.api = cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
        h.handle = service_.engine().apiHandle();
        h.configPath = configPath;
        h.configLoaded = restored_.loaded;
        h.lastRunUnclean = restored_.lastRunUnclean;
        h.sourceKind = restored_.sourceKind;
        h.iqFilePath = restored_.iqFilePath;
        // THE BOUNDED RUN'S TWO HOOKS, as main.cpp installs them: in a bounded run (frames >= 0) and never in a real session's (R9). A test window is a
        // bounded run, so its frame steps the engine's pass through them exactly as `cascade --frames N` does; no measurement is taken.
        if (frames_ >= 0) { h.bounded = cascade::engine::makeBoundedHooks(service_.engine(), nullptr); }
        return h;
    }

private:
    static cascade::engine::ServiceOptions options(const std::string& configPath, int frames) {
        cascade::engine::WindowServiceRun run;
        run.configPath = configPath;
        run.frames = frames;
        return cascade::engine::windowServiceOptions(run);   // main's options: the same function
    }
    cascade::engine::EngineService service_;
    bool announce_ = false;
    int frames_ = 0;
    cascade::engine::EngineService::Restored restored_;
};

// The base-from-member holder: the Host is a base of TestWindow listed BEFORE AppWindow, so it is constructed first and destroyed last - main's order.
struct HostHolder {
    explicit HostHolder(const std::string& configPath, bool announceConfig, int frames) : host(configPath, announceConfig, true, frames) {}
    Host host;
};

// WHAT THE OLD ~AppWindow DID TO THE ENGINE, for a test (shell retirement R8): the window no longer takes the engine down - the host stops it before the
// window goes, and the host's service tears it down when it is destroyed - but a test window is destroyed with its engine's control thread often still
// running (the pass seam starts it), and the window's session and plugin port are let go of while it does. So a test window takes its engine down
// first, in the order the window's destructor always did: the transfers cancelled, then the teardown. Idempotent (Engine::teardown is).
inline void takeEngineDown(cascade::engine::Engine& engine) {
    engine.stopTransfers();
    engine.teardown();
}

// An AppWindow that knows the Host it sits on: the window holds no engine of any kind (R10), so th::engine(window) finds the Host's through this base.
// Both test window types derive from it, and tests/test_engine_shell_guard.cpp holds that a test builds a window no other way.
class HostedWindow : public cascade::gui::AppWindow {
public:
    HostedWindow(Host& host, const std::string& configPath) : cascade::gui::AppWindow(host.windowHost(configPath)), host_(&host) {}
    Host& hostOf() { return *host_; }

private:
    Host* host_ = nullptr;
};

class TestWindow : private HostHolder, public HostedWindow {
public:
    explicit TestWindow(std::string configPath = {}, bool announceConfig = false, int frames = 0)
        : HostHolder(configPath, announceConfig, frames), HostedWindow(HostHolder::host, configPath), frames_(frames) {}
    ~TestWindow() { takeEngineDown(HostHolder::host.engine()); }

    Host& host() { return HostHolder::host; }
    int frames() const { return frames_; }

private:
    int frames_ = 0;
};

// A window on a Host the TEST owns (tests/test_engine_service_window.cpp): what a test that has to act on the engine BEFORE its window exists - start the
// control thread, count what the restore's scan built - needs. The Host is built first, restored by its constructor, and outlives the window.
class BareWindow : public HostedWindow {
public:
    explicit BareWindow(Host& host, const std::string& configPath = {}) : HostedWindow(host, configPath) {}
    ~BareWindow() { takeEngineDown(hostOf().engine()); }
};

// MAIN'S START, RUN, STOP AND CLOSE FOR A TEST WINDOW (src/main.cpp; shell retirement R8): the service started after the window's constructor and
// before run(), the window run, the service stopped with the window still on screen, then the window's GL context closed. The one in-process test
// that runs a window (tests/test_deck_knobs_app.cpp) goes through here, so it runs the PRODUCTION lifecycle (EngineService::start and stop) and not an
// arrangement of its own. The window must have been built for this many frames (TestWindow's third argument): the service's options depend on it.
inline int run(TestWindow& window) {
    std::string why;
    int rc = 1;
    if (window.host().service().start(why)) {
        rc = window.run(window.frames());
    } else {
        std::fprintf(stderr, "cascade: %s\n", why.c_str());
    }
    window.host().service().stop();
    window.closeWindow();
    return rc;
}

// THE ENGINE behind a window: the Host's own (what rawEngine(a.engine_) was, the same reference, for a TestWindow and a BareWindow alike;
// tests/test_engine_service_window.cpp holds that it is the Host's). Every window a test builds is a HostedWindow, so the cast is sound.
inline cascade::engine::Engine& engine(cascade::gui::AppWindow& a) { return static_cast<HostedWindow&>(a).hostOf().engine(); }

// A start-up state restored into a window that exists: the engine's half applied the way the host's restore applies it, then the window's own half
// from the table (what AppWindow::applyConfig did in one call before R7).
inline void applyConfig(cascade::gui::AppWindow& a, const cascade::core::AppConfig& cfg) {
    const cascade::core::AppConfig startup = cascade::core::startupState(cfg);
    engine(a).applyConfig(startup);
    (void)cascade::gui::TestHostAccess::applyWindowHalf(a, startup);
}

// THE WHOLE SETTINGS DOCUMENT AS THE KEEPER WOULD SAVE IT RIGHT NOW (shell retirement R8): the engine's snapshot with the window's live half put over it - what the
// window's own currentConfig() returned until R8 deleted the window's copy of the keeper. The engine's half is the snapshot as the engine last published it (the
// tests that mean "before the engine publishes" and "after" hold the pass and publish by hand, as they always did).
inline cascade::core::AppConfig currentConfig(cascade::gui::AppWindow& a) {
    cascade::core::AppConfig cfg = engine(a).configSnapshot();
    cascade::gui::TestHostAccess::buildInterfaceHalf(a, cfg);
    return cfg;
}

// The plugin UI behind the window's port (what the window polls), and the engine's side of the port itself (its thread tripwire).
inline cascade::core::PluginUi& pluginUi(cascade::gui::AppWindow& a) { return cascade::engine::rawPluginUi(cascade::gui::TestHostAccess::port(a)); }
inline cascade::engine::PluginUiPortImpl& pluginUiPort(cascade::gui::AppWindow& a) {
    return cascade::engine::rawPluginUiPort(cascade::gui::TestHostAccess::port(a));
}

}  // namespace th
