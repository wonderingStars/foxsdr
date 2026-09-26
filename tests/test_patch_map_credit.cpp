/*
 * THE PATCH MAP PART CREDITS ITS TILES, IN THE REAL APPLICATION (0.99.41).
 *
 * A basemap plugin's attribution is not optional: OpenStreetMap-derived tiles
 * are ODbL, the ABI refuses a basemap that supplies none, and every map page
 * letters it under the chart. The patch page's Map part (0.99.18) drew the
 * same tiles through the same MapView and lettered nothing - found while
 * taking the 0.99.40 publicity pictures, every one of which would have shown
 * OpenStreetMap tiles uncredited. It also never told the tile cache it had
 * been used, so with no map page open the cache's frame count stood still:
 * no eviction, and a tile the plugin once called missing never asked for
 * again.
 *
 * This runs cascade.exe on a patch holding one Map part, with the stand-in
 * basemap (FOXSDR_FORCE_BASEMAP, gui/basemap_stand_in.hpp) and without it,
 * and reads the interface census:
 *
 *   tiles      the Map part asked for tiles ("basemap:tile") - the premise;
 *   credit     the stand-in's attribution is lettered on the Map part, whole,
 *              BELOW its chart (so the zoom keys and the legend, which are in
 *              the chart, cannot cover it) and inside the part's face;
 *   endframe   the tile cache's frame ended with only the patch map on screen;
 *   none       no basemap: no credit, and the chart is still drawn.
 *
 * Isolated like every test that starts the application.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "gui/basemap_stand_in.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

int pid() {
#if defined(_WIN32)
    return static_cast<int>(::GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
#endif
}

// An EMPTY value unsets the variable (see test_main_view for why).
void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value.empty() ? nullptr : value.c_str());
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) {
        ::unsetenv(name);
    } else {
        ::setenv(name, value.c_str(), 1);
    }
#endif
}

std::string exePath() {
#if defined(_WIN32)
    return std::string(CASCADE_APP_BINDIR) + "/cascade.exe";
#else
    return std::string(CASCADE_APP_BINDIR) + "/cascade";
#endif
}

std::string run(const std::string& cmd) {
    std::string out;
#if defined(_WIN32)
    FILE* p = _popen(("\"" + cmd + "\"").c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    char buf[512];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
#if defined(_WIN32)
    if (p != nullptr) { _pclose(p); }
#else
    if (p != nullptr) { pclose(p); }
#endif
    return out;
}

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

struct Census {
    bool ok = false;
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
};

constexpr int kFrames = 60;
fs::path g_dir;

Census once(const std::string& tag, bool basemap) {
    Census c;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path outFile = g_dir / (tag + ".census");
    const fs::path patch = g_dir / (tag + ".patch");
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
             "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"mainView\": \"patch\" }\n";
    }
    {
        // One Map part, nothing wired: a Map part draws its chart - and asks
        // for tiles - whether or not anything feeds it.
        std::ofstream f(patch, std::ios::binary | std::ios::trunc);
        f << "foxsdr-patch 6\n"
             "view 0 0 1\n"
             "node 1 6 4 20 20 520 420 0 0 - - 0 1 -50 1 Map\n";
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", outFile.string());
    setEnv("FOXSDR_PATCH_FILE", patch.string());
    setEnv("FOXSDR_FORCE_BASEMAP", basemap ? "1" : "");
    const std::string out =
        run("\"" + exePath() + "\" --frames " + std::to_string(kFrames) + " 2>&1");
    const bool rendered =
        out.find("rendered " + std::to_string(kFrames) + " frames") != std::string::npos;
    const bool written = out.find("ui census written") != std::string::npos;
    if (!rendered || !written) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), out.c_str());
        return c;
    }
    std::ifstream in(outFile);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.rfind("item ", 0) == 0) {
            c.items.insert(line.substr(5));
        } else if (line.rfind("rect ", 0) == 0) {
            std::istringstream ss(line.substr(5));
            std::string name;
            Rect r;
            ss >> name >> r.x0 >> r.y0 >> r.x1 >> r.y1;
            c.rects[name] = r;
        }
    }
    c.ok = !c.items.empty();
    return c;
}

}  // namespace

int main() {
    g_dir = fs::temp_directory_path() / ("cascade-patch-map-credit-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir / "appdata", ec);
    fs::create_directories(g_dir / "local", ec);
    fs::create_directories(g_dir / "diag", ec);
    setEnv("APPDATA", (g_dir / "appdata").string());
    setEnv("LOCALAPPDATA", (g_dir / "local").string());
    setEnv("XDG_CONFIG_HOME", (g_dir / "appdata").string());
    setEnv("XDG_DATA_HOME", (g_dir / "local").string());
    setEnv("XDG_STATE_HOME", (g_dir / "local").string());
    setEnv("FOXSDR_DIAG_DIR", (g_dir / "diag").string());
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL",
                            "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("FOXSDR_SINGLE_VIEWPORT", "1");
    setEnv("FOXSDR_WINDOW_SIZE", "1600x1000");
    setEnv("FOXSDR_INPUT_SCRIPT", "");

    const std::string creditItem =
        std::string("patchmap:credit:") + cascade::gui::kStandInBasemapAttribution;

    // --- with a basemap -------------------------------------------------------------
    const Census with = once("with", true);
    CHECK(with.ok);
    CHECK(with.items.count("patchmap:chart") == 1);
    CHECK(with.items.count("basemap:tile") == 1);   // the premise: tiles were asked for
    const bool credited = with.items.count(creditItem) == 1;
    const bool ended = with.items.count("basemap:endframe") == 1;
    std::printf("  with a basemap: credit %s, tile cache frame %s\n",
                credited ? "lettered" : "MISSING", ended ? "ended" : "NEVER ENDED");
    CHECK(credited);
    CHECK(ended);
    const auto chart = with.rects.find("patchmap:chart");
    const auto face = with.rects.find("patchmap:face");
    const auto credit = with.rects.find("patchmap:credit");
    CHECK(chart != with.rects.end());
    CHECK(face != with.rects.end());
    CHECK(credit != with.rects.end());
    if (chart != with.rects.end() && face != with.rects.end() && credit != with.rects.end()) {
        const Rect& ch = chart->second;
        const Rect& fa = face->second;
        const Rect& cr = credit->second;
        std::printf("  chart (%.0f,%.0f)-(%.0f,%.0f), credit (%.0f,%.0f)-(%.0f,%.0f), face "
                    "bottom %.0f\n",
                    ch.x0, ch.y0, ch.x1, ch.y1, cr.x0, cr.y0, cr.x1, cr.y1, fa.y1);
        // Below the chart - so nothing drawn in the chart covers it - and
        // inside the part's face, left to right and top to bottom.
        CHECK(cr.y0 >= ch.y1 - 0.5f);
        CHECK(cr.y1 <= fa.y1 + 0.5f);
        CHECK(cr.x0 >= ch.x0 - 0.5f && cr.x1 <= ch.x1 + 0.5f);
        CHECK(cr.y1 - cr.y0 >= 8.0f && cr.x1 - cr.x0 >= 40.0f);   // a real line of text
    }

    // --- without one -----------------------------------------------------------------
    const Census none = once("none", false);
    CHECK(none.ok);
    CHECK(none.items.count("patchmap:chart") == 1);   // the map is still drawn
    CHECK(none.items.count("basemap:tile") == 0);
    bool anyCredit = false;
    for (const std::string& s : none.items) {
        if (s.rfind("patchmap:credit", 0) == 0) { anyCredit = true; }
    }
    CHECK(!anyCredit);

    const int rc = testSummary("test_patch_map_credit");
    if (rc == 0) { fs::remove_all(g_dir, ec); }
    return rc;
}
