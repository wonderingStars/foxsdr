// test_host.hpp - THE TEST HOST: THE ONE PLACE A WINDOW TEST REACHES THE ENGINE BEHIND ITS WINDOW (shell retirement, batch R1 = survey step M0;
// docs/engine-stage4.md 6.25).
//
// WHY. A hermetic window test is, in truth, a test of the ENGINE written with a window in front of it: 69 test files reached the Engine
// 1,979 times through `rawEngine(a.engine_)` (786 of them private-field reads), each from a local AppWindowTestAccess. The shell is being
// retired (the window will reach the engine through the API table and nothing else), and `rawEngine` is the shell's back door, so the tests
// cannot keep a door of their own scattered across 69 files: when the Engine's owner changes (R7: the host process owns it, as main does in
// the product), every one of those files would have to change again. This header is the answer the survey (3.3, 3.4) chose: tests name the
// engine through ONE function, th::engine(window), and build their windows through ONE type, th::TestWindow. The day the owner changes
// (R7, M1) only this header changes; the files that moved here do not.
//
//   th::TestWindow       an AppWindow, built the way `AppWindow app;` / `AppWindow app(path[, announce])` always built one. It is a distinct
//                        type so that every test window is made in one place - tests/test_engine_shell_guard.cpp holds that no test names
//                        `AppWindow` as a thing it constructs. (R7 makes it own the Engine and its service first and the window second;
//                        until then it is the window and nothing more.)
//   th::engine(a)        the Engine the window `a` holds - what `rawEngine(a.engine_)` was, byte for byte the same reference. Tests that
//                        hold the window as an `AppWindow&` (every AppWindowTestAccess helper does) use it unchanged.
//
// THE AUDIT. This is the one place that names rawEngine: tests/test_engine_shell_guard.cpp scans tests/ and holds that no other test file
// does (before R1, 72 files named it). It reaches the window's private `engine_` through ONE friend declaration, `TestHostAccess`, beside
// AppWindow's AppWindowTestAccess; deleting it is a compile failure here and in every test (proved: docs/engine-stage4.md 6.25).
// Private FIELDS of the Engine still come through each test's own AppWindowTestAccess/Engine friend, exactly as before - this batch
// moved who hands out the Engine, not what a test may read of it.
//
// Engine::testHooks_ are installed by the test BEFORE any window exists, as today (engine.hpp: "Set by the test before any AppWindow
// exists"); R7 keeps that order (the host is built by TestWindow's constructor, after the test's hooks).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>
#include <utility>

#include "engine/engine.hpp"
#include "engine/engine_shell.hpp"
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
};

}  // namespace cascade::gui

namespace th {

class TestWindow : public cascade::gui::AppWindow {
public:
    explicit TestWindow(std::string configPath = {}, bool announceConfig = false)
        : cascade::gui::AppWindow(std::move(configPath), announceConfig) {}
};

inline cascade::engine::Engine& engine(cascade::gui::AppWindow& a) { return cascade::gui::TestHostAccess::engine(a); }

// The plugin UI behind the window's port (what the window polls), and the engine's side of the port itself (its thread tripwire).
inline cascade::core::PluginUi& pluginUi(cascade::gui::AppWindow& a) { return cascade::engine::rawPluginUi(cascade::gui::TestHostAccess::port(a)); }
inline cascade::engine::PluginUiPortImpl& pluginUiPort(cascade::gui::AppWindow& a) {
    return cascade::engine::rawPluginUiPort(cascade::gui::TestHostAccess::port(a));
}

}  // namespace th
