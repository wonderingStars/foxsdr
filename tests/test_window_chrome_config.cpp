// test_window_chrome_config.cpp - THE WINDOW'S HALF OF THE CONFIG: the four layout preferences the
// chrome keeps (stub campaign batch B1) leave the real AppWindow in the config it saves and come
// back in the one it restores, with no window and no ImGui frame.
//
//   railCollapsed    the FUNCTION SELECT rail is folded to a strip  (0.99.49)
//   statusCollapsed  the STATUS column is folded to a strip         (0.99.74)
//   displayOpen      the Display section is open                    (0.99.74)
//   interfaceScale   the Interface size: "auto" or a percentage     (0.99.44)
//
// WHY ITS OWN TEST. The window loads the config once and keeps `loadedCfg_`; currentConfig() starts
// from that copy and writes back only the fields the window owns. A field the window forgets to
// write is therefore NOT lost - it is silently kept at the value the file had, and never changes:
// the fold would work on screen and not persist. A run of the real application cannot tell that
// from a fold that persisted unless the run starts from a different value than it ends with, so
// this test does it in-process: restore a config, change the member, and read the config the window
// would save. The per-field tests (test_rail_fold, test_status_fold) pin the two folds' geometry
// and their own round trips; this pins all four together, including the two they do not reach.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/config.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "gui/ui_scale.hpp"
#include "test_check.hpp"

namespace {

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

std::filesystem::path g_scratch;

// ISOLATED FROM THE REAL CONFIG, the rule every *_app test follows.
void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = std::filesystem::temp_directory_path() / ("foxsdr_chrome_config_" + std::to_string(pid));
    std::filesystem::create_directories(g_scratch);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_CRASH_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_UPDATE_URL", "http://127.0.0.1:9");
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static cascade::core::AppConfig config(AppWindow& a) { return a.currentConfig(); }
    static void restore(AppWindow& a, const cascade::core::AppConfig& c) { a.applyConfig(c); }
    static bool rail(const AppWindow& a) { return a.railCollapsed_; }
    static bool status(const AppWindow& a) { return a.statusCollapsed_; }
    static bool display(const AppWindow& a) { return a.displayOpen_; }
    static const std::string& scale(const AppWindow& a) { return a.interfaceScale_; }
    static void toggleRail(AppWindow& a) { a.railCollapsed_ = !a.railCollapsed_; }
    static void toggleStatus(AppWindow& a) { a.toggleStatusFold(); }
    static void setDisplay(AppWindow& a, bool open) { a.displayOpen_ = open; }
    static void setScale(AppWindow& a, const std::string& s) { a.interfaceScale_ = s; }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

int main() {
    std::printf("test_window_chrome_config\n");
    isolate();
    namespace g = cascade::gui;

    // A fresh window opens everything as a fresh install does.
    {
        th::TestWindow app;
        CHECK(!Access::rail(app));
        CHECK(!Access::status(app));
        CHECK(!Access::display(app));
        CHECK(Access::scale(app) == g::uiscale::kAuto);
        const cascade::core::AppConfig c = Access::config(app);
        CHECK(!c.railCollapsed && !c.statusCollapsed && !c.displayOpen);
        CHECK(c.interfaceScale == "auto");
    }

    // RESTORE: every field the file carries reaches its member.
    cascade::core::AppConfig file;
    file.railCollapsed = true;
    file.statusCollapsed = true;
    file.displayOpen = true;
    file.interfaceScale = "175";
    {
        th::TestWindow app;
        Access::restore(app, file);
        CHECK(Access::rail(app));
        CHECK(Access::status(app));
        CHECK(Access::display(app));
        CHECK(Access::scale(app) == "175");
        // ...and the live factor follows the restored choice at once.
        CHECK(g::uiscale::choice() == "175");
        CHECK_NEAR(g::uiscale::factor(), 1.75, 0.001);
        // Nothing was changed, so nothing differs on the way out.
        const cascade::core::AppConfig out = Access::config(app);
        CHECK(out.railCollapsed && out.statusCollapsed && out.displayOpen);
        CHECK(out.interfaceScale == "175");
    }

    // SAVE: a change made AFTER the restore is what the config says - the check that a field the
    // window forgot to write (and loadedCfg_ kept) would fail. Each field starts from the file's
    // value and is moved the other way, one at a time, so a field written from the wrong member
    // cannot hide behind another.
    {
        th::TestWindow app;
        Access::restore(app, file);

        Access::toggleRail(app);
        cascade::core::AppConfig out = Access::config(app);
        CHECK(!out.railCollapsed);                       // the rail was opened
        CHECK(out.statusCollapsed && out.displayOpen);   // and nothing else moved
        CHECK(out.interfaceScale == "175");

        Access::toggleStatus(app);
        out = Access::config(app);
        CHECK(!out.statusCollapsed);
        CHECK(!out.railCollapsed && out.displayOpen);
        CHECK(out.interfaceScale == "175");

        Access::setDisplay(app, false);
        out = Access::config(app);
        CHECK(!out.displayOpen);
        CHECK(!out.railCollapsed && !out.statusCollapsed);
        CHECK(out.interfaceScale == "175");

        Access::setScale(app, "auto");
        out = Access::config(app);
        CHECK(out.interfaceScale == "auto");
        CHECK(!out.railCollapsed && !out.statusCollapsed && !out.displayOpen);
    }

    // The debounced save notices each of the four changing (the comparator reads the same fields).
    {
        cascade::core::AppConfig a;
        for (int i = 0; i < 4; ++i) {
            cascade::core::AppConfig b = a;
            if (i == 0) { b.railCollapsed = true; }
            if (i == 1) { b.statusCollapsed = true; }
            if (i == 2) { b.displayOpen = true; }
            if (i == 3) { b.interfaceScale = "200"; }
            CHECK(!g::configsEqual(a, b));
        }
    }

    // The fold flags and the interface size survive the file itself, not only the window.
    {
        const std::string text = cascade::core::ConfigStore::serialize(file);
        CHECK(text.find("\"statusCollapsed\": true") != std::string::npos);
        CHECK(text.find("\"displayOpen\": true") != std::string::npos);
        CHECK(text.find("\"interfaceScale\": \"175\"") != std::string::npos);
    }

    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_window_chrome_config");
}
