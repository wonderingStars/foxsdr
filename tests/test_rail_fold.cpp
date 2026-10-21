// test_rail_fold.cpp - the FUNCTION SELECT rail folds to a strip (0.99.49
// beta feedback: a small laptop, where the 384 px rail is more than a quarter
// of the screen and the patch page beside it is what the user is working in).
//
//   - the column's width, open and folded, and where the fold key sits: at
//     the right end of the title row, inside the plate, above the rule under
//     the title; centred on the strip when folded;
//   - the choice is REMEMBERED: AppConfig::railCollapsed goes to the file and
//     comes back, startupState keeps it, an old config without it opens the
//     rail, and the debounced save notices it changing;
//   - the real AppWindow writes it into the config it saves and reads it back
//     (AppWindowTestAccess), with no window and no ImGui frame.
//
// tests/test_rail_fold_app.cpp runs the application and presses the keys;
// tests/test_app_rail.cpp checks the title still fits beside the key in
// every language.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/config.hpp"
#include "gui/app_window.hpp"
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
    g_scratch = std::filesystem::temp_directory_path() / ("foxsdr_rail_fold_" + std::to_string(pid));
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
    // THE KEY'S HANDLER is `railCollapsed_ = !railCollapsed_` (drawRailFoldKey);
    // this is that assignment, on the same field.
    static void press(AppWindow& a) { a.railCollapsed_ = !a.railCollapsed_; }
    static bool folded(const AppWindow& a) { return a.railCollapsed_; }
    static cascade::core::AppConfig config(AppWindow& a) { return a.currentConfig(); }
    static void restore(AppWindow& a, const cascade::core::AppConfig& c) { a.applyConfig(c); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

int main() {
    std::printf("test_rail_fold\n");
    isolate();
    namespace g = cascade::gui;

    // --- the geometry ------------------------------------------------------------
    CHECK(g::railColumnWidth(false) == g::kMenuWidth);
    CHECK(g::railColumnWidth(true) == g::kRailFoldedW);
    CHECK(g::railColumnWidth(true) < g::kMenuWidth / 8.0f);  // a strip, not a column
    for (const float s : {1.0f, 1.5f, 2.0f}) {
        const float colX = 20.0f, colY = 200.0f, colW = g::kMenuWidth * s;
        const g::RailFoldKeyRect open = g::railFoldKeyRect(colX, colY, colW, false, s);
        // At the right end of the title row, inside the plate's inset.
        CHECK_NEAR(open.x1, colX + colW - g::kRailPlatePad * s, 0.01);
        CHECK(open.x0 > colX + colW * 0.5f);
        // Above the rule under the title: addBenchPlate starts the title 7 px
        // (times s) down and rules it at least a title's height below that.
        CHECK(open.y0 >= colY + 4.0f * s);
        CHECK(open.y1 <= colY + (7.0f + 15.0f + 5.0f) * s);
        // Folded: centred on the strip, the same key at the same height.
        const float stripW = g::kRailFoldedW * s;
        const g::RailFoldKeyRect strip = g::railFoldKeyRect(colX, colY, stripW, true, s);
        CHECK_NEAR((strip.x0 + strip.x1) * 0.5f, colX + stripW * 0.5f, 0.01);
        CHECK(strip.x0 >= colX + 2.0f * s && strip.x1 <= colX + stripW - 2.0f * s);
        CHECK_NEAR(strip.y0, open.y0, 0.01);
        CHECK_NEAR(strip.x1 - strip.x0, open.x1 - open.x0, 0.01);
    }

    // --- the config: saved, loaded, kept at start-up ---------------------------------
    {
        cascade::core::AppConfig c;
        CHECK(!c.railCollapsed);  // a fresh install opens the rail
        c.railCollapsed = true;
        const std::string text = cascade::core::ConfigStore::serialize(c);
        CHECK(text.find("\"railCollapsed\": true") != std::string::npos);
        const std::filesystem::path f = g_scratch / "rail.json";
        {
            std::ofstream o(f, std::ios::binary | std::ios::trunc);
            o << text;
        }
        cascade::core::AppConfig back;
        std::string err;
        CHECK(cascade::core::ConfigStore::load(f.string(), back, err));
        CHECK(back.railCollapsed);
        // A config from before 0.99.49 has no such field: the rail is open.
        {
            std::ofstream o(f, std::ios::binary | std::ios::trunc);
            o << "{ \"sourceKind\": \"siggen\" }\n";
        }
        cascade::core::AppConfig old;
        old.railCollapsed = true;  // whatever was there before, the file decides
        CHECK(cascade::core::ConfigStore::load(f.string(), old, err));
        CHECK(!old.railCollapsed);
        // A layout preference, not an open window: startupState keeps it.
        CHECK(cascade::core::startupState(c).railCollapsed);
        // The debounced save sees it change.
        cascade::core::AppConfig a;
        cascade::core::AppConfig b = a;
        b.railCollapsed = true;
        CHECK(!g::configsEqual(a, b));
    }

    // --- the real window writes it and reads it back -----------------------------
    cascade::core::AppConfig saved;
    {
        g::AppWindow app;
        CHECK(!Access::folded(app));
        CHECK(!Access::config(app).railCollapsed);
        Access::press(app);
        CHECK(Access::folded(app));
        saved = Access::config(app);
        CHECK(saved.railCollapsed);
    }
    {
        g::AppWindow app;
        Access::restore(app, saved);
        CHECK(Access::folded(app));  // the next launch comes back folded
        Access::press(app);
        CHECK(!Access::config(app).railCollapsed);
    }

    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_rail_fold");
}
