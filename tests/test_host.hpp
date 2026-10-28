// test_host.hpp - THE TEST HOST: THE ONE PLACE A WINDOW TEST REACHES THE ENGINE BEHIND ITS WINDOW (shell retirement, batch R1 = survey step M0, and
// R7 = survey step M1; docs/engine-stage4.md 6.25).
//
// WHY. A hermetic window test is, in truth, a test of the ENGINE written with a window in front of it: 69 test files reached the Engine
// 1,979 times through `rawEngine(a.engine_)` (786 of them private-field reads), each from a local AppWindowTestAccess. The shell is being
// retired (the window will reach the engine through the API table and nothing else), and `rawEngine` is the shell's back door, so the tests
// cannot keep a door of their own scattered across 69 files: when the Engine's owner changes (R7: the host process owns it, as main does in
// the product), every one of those files would have to change again. This header is the answer the survey (3.3, 3.4) chose: tests name the
// engine through ONE function, th::engine(window), and build their windows through ONE type, th::TestWindow. The day the owner changed
// (R7, M1) only this header changed; the files that moved here did not.
//
//   th::Host             WHAT main.cpp IS, for a test: an EngineService (hermetic when the window has no settings path, persistent on a scratch
//                        path when it has one), its restore, and the CASCADE_CONFIG_TEST line when asked. It owns the Engine. It does NOT start the
//                        service (a window's run() starts the engine until R8, and most tests drive the pass themselves through the pass seam) and
//                        takes no lock (a test window never did). Everything a test builds is in main's order: service, restore, window.
//   th::TestWindow       an AppWindow, built the way `AppWindow app;` / `AppWindow app(path[, announce])` always built one - and built the way
//                        main builds it since R7: the HOST FIRST (base-from-member: the Host is a base that is constructed before the window and
//                        destroyed after it), then the window on the restored engine. It is a distinct type so that every test window is made in
//                        one place - tests/test_engine_shell_guard.cpp holds that no test names `AppWindow` as a thing it constructs.
//   th::BareWindow      an AppWindow on a Host the TEST built and owns, for the tests that must act on the engine before its window exists.
//   th::engine(a)        the Engine the window `a` sits on - the Host's own, the one the shell views (what `rawEngine(a.engine_)` was, the same
//                        reference). Tests that hold the window as an `AppWindow&` (every AppWindowTestAccess helper does) use it unchanged.
//   th::applyConfig(a,c) what the window's own `applyConfig(c)` was: the start-up state of `c` onto the engine, then the window's half of it from the table
//                        (R7 split the window's applyConfig in two: the engine's half is the host's, applied here; the window's is AppWindow's).
//
// THE AUDIT. This is the one place that names rawEngine: tests/test_engine_shell_guard.cpp scans tests/ and holds that no other test file
// does (before R1, 72 files named it). It reaches the window's private `engine_` through ONE friend declaration, `TestHostAccess`, beside
// AppWindow's AppWindowTestAccess; deleting it is a compile failure here and in every test (proved: docs/engine-stage4.md 6.25).
// Private FIELDS of the Engine still come through each test's own AppWindowTestAccess/Engine friend, exactly as before - this batch
// moved who hands out the Engine, not what a test may read of it.
//
// Engine::testHooks_ are installed by the test BEFORE any window exists, as today (engine.hpp: "Set by the test before any AppWindow
// exists"); the Host is built by TestWindow's constructor, after the test's hooks, so the order is kept.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "engine/engine.hpp"
#include "engine/engine_service.hpp"
#include "engine/engine_shell.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/plugin_ui_port.hpp"
#include "gui/app_window.hpp"

namespace cascade::gui {

// The window's friend for this header and nothing else (src/gui/app_window.hpp: `friend struct TestHostAccess;`).
struct TestHostAccess {
    static cascade::engine::Engine& engine(AppWindow& a) { return cascade::engine::rawEngine(a.engine_); }
    // THE PLUGIN UI PORT behind a window (shell retirement R4): what rawPluginUi(*a.pluginFeed_) was, and the two steps a late-attach test needs - the
    // window lets go of its port (the destructor disconnects it, the engine drives its own plugin UI again), and asks for one again (the same call the
    // constructor makes: a window that arrives after the engine's scan).
    static cascade::core::PluginUiPort& port(AppWindow& a) { return *a.ui_; }
    static void dropPort(AppWindow& a) { a.ui_.reset(); }
    static void attachPort(AppWindow& a) { a.attachPluginUi(); }
    // THE WINDOW'S HALF of a start-up state (shell retirement R7): what the constructor does with the table's fact, for a test that restores a config
    // into a window it already has.
    static cascade::core::AppConfig applyWindowHalf(AppWindow& a, const cascade::core::AppConfig& startup) { return a.applyWindowHalf(startup); }
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
    explicit Host(const std::string& configPath, bool announceConfig = false, bool restoreNow = true)
        : service_(options(configPath)), announce_(announceConfig) {
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
        h.engine = &service_.engine();
        h.configPath = configPath;
        h.configLoaded = restored_.loaded;
        h.writesForbidden = restored_.writesForbidden;
        h.lastRunUnclean = restored_.lastRunUnclean;
        h.sourceKind = restored_.sourceKind;
        h.iqFilePath = restored_.iqFilePath;
        return h;
    }

private:
    static cascade::engine::ServiceOptions options(const std::string& configPath) {
        cascade::engine::ServiceOptions o;
        o.configPath = configPath;
        o.lock = cascade::engine::ServiceLock::BestEffort;
        o.hermetic = cascade::engine::ServiceHermetic::Window;
        o.label = "cascade";
        o.frontEndFollows = true;   // main's options: the window attaches after the restore
        return o;
    }
    cascade::engine::EngineService service_;
    bool announce_ = false;
    cascade::engine::EngineService::Restored restored_;
};

// The base-from-member holder: the Host is a base of TestWindow listed BEFORE AppWindow, so it is constructed first and destroyed last - main's order.
struct HostHolder {
    explicit HostHolder(const std::string& configPath, bool announceConfig) : host(configPath, announceConfig) {}
    Host host;
};

class TestWindow : private HostHolder, public cascade::gui::AppWindow {
public:
    explicit TestWindow(std::string configPath = {}, bool announceConfig = false)
        : HostHolder(configPath, announceConfig), cascade::gui::AppWindow(HostHolder::host.windowHost(configPath)) {}

    Host& host() { return HostHolder::host; }
};

// A window on a Host the TEST owns (tests/test_engine_service_window.cpp): what a test that has to act on the engine BEFORE its window exists - start the
// control thread, count what the restore's scan built - needs. The Host is built first, restored by its constructor, and outlives the window.
class BareWindow : public cascade::gui::AppWindow {
public:
    explicit BareWindow(Host& host, const std::string& configPath = {}) : cascade::gui::AppWindow(host.windowHost(configPath)) {}
};

// THE ENGINE behind a window: the Host's own, which the window's shell views (rawEngine(a.engine_) is the same reference, for a TestWindow and a
// BareWindow alike; tests/test_engine_service_window.cpp holds that it is the Host's).
inline cascade::engine::Engine& engine(cascade::gui::AppWindow& a) { return cascade::gui::TestHostAccess::engine(a); }

// A start-up state restored into a window that exists: the engine's half applied the way the host's restore applies it, then the window's own half
// from the table (what AppWindow::applyConfig did in one call before R7).
inline void applyConfig(cascade::gui::AppWindow& a, const cascade::core::AppConfig& cfg) {
    const cascade::core::AppConfig startup = cascade::core::startupState(cfg);
    engine(a).applyConfig(startup);
    (void)cascade::gui::TestHostAccess::applyWindowHalf(a, startup);
}

// The plugin UI behind the window's port (what the window polls), and the engine's side of the port itself (its thread tripwire).
inline cascade::core::PluginUi& pluginUi(cascade::gui::AppWindow& a) { return cascade::engine::rawPluginUi(cascade::gui::TestHostAccess::port(a)); }
inline cascade::engine::PluginUiPortImpl& pluginUiPort(cascade::gui::AppWindow& a) {
    return cascade::engine::rawPluginUiPort(cascade::gui::TestHostAccess::port(a));
}

}  // namespace th
