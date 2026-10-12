// test_status_fold.cpp - the STATUS column on the right folds to a strip, the
// way the FUNCTION SELECT rail on the left does (0.99.74 beta feedback: "Just
// like you added the << and >> to expand and collapse the Function Select
// panel on the left side, can you do this for the right side Status panel? I
// am just looking at it from the perspective if that information is useful to
// the end user while they are looking at the spectrum or whatever.").
//
//   - the column's width, open and folded (the open width is the 230 that used
//     to be a local in drawUi), and where the fold key sits: at the LEFT end of
//     the title row, nearest the spectrum, as the rail's sits at its right end;
//     centred on the strip when folded, at the rail key's size and height;
//   - the choice is REMEMBERED: AppConfig::statusCollapsed goes to the file and
//     comes back, startupState keeps it, an old config without it opens the
//     column, and the debounced save notices it changing;
//   - the real AppWindow's key handler (toggleStatusFold, the one function the
//     key calls) flips the field the config carries, with no window and no
//     ImGui frame, and the rail's own flag is a separate thing.
//
// tests/test_status_fold_app.cpp runs the application and presses the keys;
// tests/test_app_rail.cpp checks the title still fits beside the key in every
// language.
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

// ISOLATED FROM THE REAL CONFIG, the rule every test that builds an AppWindow
// follows.
void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = std::filesystem::temp_directory_path() / ("foxsdr_status_fold_" + std::to_string(pid));
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
    // THE KEY'S HANDLER, itself: drawStatusFoldKey calls toggleStatusFold() and
    // nothing else, so pressing it here is pressing it there. (The rail test
    // copies its handler's one-line assignment instead; a copy cannot go red
    // when the real handler stops toggling, and this must.)
    static void press(AppWindow& a) { a.toggleStatusFold(); }
    static bool folded(const AppWindow& a) { return a.statusCollapsed_; }
    static bool railFolded(const AppWindow& a) { return a.railCollapsed_; }
    static cascade::core::AppConfig config(AppWindow& a) { return a.currentConfig(); }
    static void restore(AppWindow& a, const cascade::core::AppConfig& c) { a.applyConfig(c); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

int main() {
    std::printf("test_status_fold\n");
    isolate();
    namespace g = cascade::gui;

    // --- the geometry ------------------------------------------------------------
    CHECK(g::statusColumnWidth(false) == g::kStatusW);
    CHECK(g::statusColumnWidth(true) == g::kStatusFoldedW);
    // The 230 the column has always been: hoisting the local must not move it.
    CHECK_NEAR(g::kStatusW, 230.0, 0.001);
    CHECK(g::statusColumnWidth(true) < g::kStatusW / 6.0f);  // a strip, not a column
    // The strip is the rail's strip: two folded panels, one key size.
    CHECK_NEAR(g::kStatusFoldedW, g::kRailFoldedW, 0.001);
    for (const float s : {1.0f, 1.5f, 2.0f}) {
        const float colX = 900.0f, colY = 200.0f, colW = g::kStatusW * s;
        const g::RailFoldKeyRect open = g::statusFoldKeyRect(colX, colY, colW, false, s);
        // At the LEFT end of the title row, inside the plate's inset: the end
        // nearest the spectrum.
        CHECK_NEAR(open.x0, colX + g::kRailPlatePad * s, 0.01);
        CHECK(open.x1 < colX + colW * 0.5f);
        // Above the rule under the title, at the rail key's height.
        CHECK(open.y0 >= colY + 4.0f * s);
        CHECK(open.y1 <= colY + (7.0f + 15.0f + 5.0f) * s);
        // Folded: centred on the strip, the same key at the same height.
        const float stripW = g::kStatusFoldedW * s;
        const g::RailFoldKeyRect strip = g::statusFoldKeyRect(colX, colY, stripW, true, s);
        CHECK_NEAR((strip.x0 + strip.x1) * 0.5f, colX + stripW * 0.5f, 0.01);
        CHECK(strip.x0 >= colX + 2.0f * s && strip.x1 <= colX + stripW - 2.0f * s);
        CHECK_NEAR(strip.y0, open.y0, 0.01);
        CHECK_NEAR(strip.x1 - strip.x0, open.x1 - open.x0, 0.01);
        // The rail's key, mirrored: the same size and height as the left
        // panel's, and as far in from the OTHER end of its title row.
        const g::RailFoldKeyRect rail = g::railFoldKeyRect(colX, colY, colW, false, s);
        CHECK_NEAR(open.x1 - open.x0, rail.x1 - rail.x0, 0.01);
        CHECK_NEAR(open.y0, rail.y0, 0.01);
        CHECK_NEAR(open.y1, rail.y1, 0.01);
        CHECK_NEAR(open.x0 - colX, (colX + colW) - rail.x1, 0.01);
    }

    // --- the config: saved, loaded, kept at start-up ---------------------------------
    {
        cascade::core::AppConfig c;
        CHECK(!c.statusCollapsed);  // a fresh install opens the column
        c.statusCollapsed = true;
        const std::string text = cascade::core::ConfigStore::serialize(c);
        CHECK(text.find("\"statusCollapsed\": true") != std::string::npos);
        // ...and the rail's flag is its own key, untouched by this one.
        CHECK(text.find("\"railCollapsed\": false") != std::string::npos);
        const std::filesystem::path f = g_scratch / "status.json";
        {
            std::ofstream o(f, std::ios::binary | std::ios::trunc);
            o << text;
        }
        cascade::core::AppConfig back;
        std::string err;
        CHECK(cascade::core::ConfigStore::load(f.string(), back, err));
        CHECK(back.statusCollapsed);
        CHECK(!back.railCollapsed);
        // false round-trips as false, too (a default that happens to be read
        // as the default proves nothing about the key).
        c.statusCollapsed = false;
        {
            std::ofstream o(f, std::ios::binary | std::ios::trunc);
            o << cascade::core::ConfigStore::serialize(c);
        }
        cascade::core::AppConfig again;
        again.statusCollapsed = true;  // whatever was there before, the file decides
        CHECK(cascade::core::ConfigStore::load(f.string(), again, err));
        CHECK(!again.statusCollapsed);
        // A config from before 0.99.74 has no such field: the column is open.
        {
            std::ofstream o(f, std::ios::binary | std::ios::trunc);
            o << "{ \"sourceKind\": \"siggen\" }\n";
        }
        cascade::core::AppConfig old;
        old.statusCollapsed = true;
        CHECK(cascade::core::ConfigStore::load(f.string(), old, err));
        CHECK(!old.statusCollapsed);
        // A layout preference, not an open window: startupState keeps it.
        c.statusCollapsed = true;
        CHECK(cascade::core::startupState(c).statusCollapsed);
        // The debounced save sees it change.
        cascade::core::AppConfig a;
        cascade::core::AppConfig b = a;
        CHECK(g::configsEqual(a, b));
        b.statusCollapsed = true;
        CHECK(!g::configsEqual(a, b));
    }

    // --- the real window writes it and reads it back -----------------------------
    cascade::core::AppConfig saved;
    {
        g::AppWindow app;
        CHECK(!Access::folded(app));
        CHECK(!Access::config(app).statusCollapsed);
        Access::press(app);
        CHECK(Access::folded(app));
        CHECK(!Access::railFolded(app));  // the other panel did not follow
        saved = Access::config(app);
        CHECK(saved.statusCollapsed);
        CHECK(!saved.railCollapsed);
    }
    {
        g::AppWindow app;
        Access::restore(app, saved);
        CHECK(Access::folded(app));  // the next launch comes back folded
        Access::press(app);
        CHECK(!Access::folded(app));
        CHECK(!Access::config(app).statusCollapsed);
    }

    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_status_fold");
}
