/*
 * CLOSE BOOKMARK NAMES STACK ONTO UP TO THREE ROWS instead of the second one
 * being silently dropped (an Italian user's request, 0.99.54): two bookmarks
 * a couple of kHz apart on a 2 MHz siggen span land a handful of pixels
 * apart, and the un-stacked code drew the first name and left the second's
 * text off entirely (its tick still showed) the moment their extents
 * touched. The row choice itself is pinned without a GL window in
 * tests/test_bookmark_marker_geometry.cpp; this runs cascade itself and
 * checks from the UI census and the config it saved:
 *
 *   stacking on   both names drawn - one on row 0, the other stacked onto
 *                 row 1 - and the config that shipped with them survives
 *                 the run unchanged;
 *   stacking off  only the first (row 0) name is drawn, the second is left
 *                 off exactly as the pre-0.99.54 single-row code left any
 *                 clashing name off - and the config still says so afterward;
 *   markers off   "On the spectrum" itself off: no bookmark name is drawn at
 *                 all, and the config round-trips both switches together.
 *
 * Isolated like every other test that starts the application - a private
 * APPDATA holds the bookmark file this seeds (FreqManager::defaultPath() has
 * no FOXSDR_BOOKMARK_* seam of its own; the file is read from disk the same
 * way a real install's is).
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

#include "core/config.hpp"
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

struct Result {
    bool ok = false;
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
    bool bookmarkMarkers = false;
    bool bookmarkStackNames = false;
};

constexpr int kFrames = 70;
fs::path g_dir;

// Two favourites, close enough (2 kHz on a 2 MHz siggen span - roughly a
// thousandth of the width) that their names are guaranteed to clash
// regardless of window size: favourite so they are attempted whether or not
// the span is "sparse" (drawBookmarkMarkers, pass 0).
void writeBookmarks(const fs::path& appdataDir) {
    const fs::path dir = appdataDir / "foxsdr";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f(dir / "bookmarks.json", std::ios::binary | std::ios::trunc);
    f << "{\n"
         "  \"schemaVersion\": 1,\n"
         "  \"bookmarks\": [\n"
         "    { \"name\": \"Alpha Test Bookmark Long Name One\", \"freqHz\": 100000000.0,\n"
         "      \"mode\": \"WFM\", \"bandwidthHz\": 150000.0, \"favourite\": true },\n"
         "    { \"name\": \"Bravo Test Bookmark Long Name Two\", \"freqHz\": 100002000.0,\n"
         "      \"mode\": \"WFM\", \"bandwidthHz\": 150000.0, \"favourite\": true }\n"
         "  ]\n"
         "}\n";
}

Result once(const std::string& tag, bool markers, bool stackNames) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
             "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", "
             "\"mainView\": \"receiver\", \"bandPlanOverlay\": false, "
             "\"bookmarkMarkers\": "
          << (markers ? "true" : "false") << ", \"bookmarkStackNames\": "
          << (stackNames ? "true" : "false") << " }\n";
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_WINDOW_SIZE", "1600x900");
    setEnv("FOXSDR_INPUT_SCRIPT", "");
    setEnv("FOXSDR_PATCH_FILE", "");
    const std::string out =
        run("\"" + exePath() + "\" --frames " + std::to_string(kFrames) + " 2>&1");
    const bool rendered =
        out.find("rendered " + std::to_string(kFrames) + " frames") != std::string::npos;
    const bool written = out.find("ui census written") != std::string::npos;
    if (!rendered || !written) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), out.c_str());
        return r;
    }
    {
        std::ifstream in(census);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("item ", 0) == 0) { r.items.insert(line.substr(5)); }
            if (line.rfind("rect ", 0) != 0) { continue; }
            std::istringstream ss(line.substr(5));
            std::string name;
            Rect rc;
            ss >> name >> rc.x0 >> rc.y0 >> rc.x1 >> rc.y1;
            r.rects[name] = rc;
        }
    }
    cascade::core::AppConfig saved;
    std::string err;
    if (!cascade::core::ConfigStore::load(cfg.string(), saved, err)) {
        std::printf("  %s: the saved config did not load: %s\n", tag.c_str(), err.c_str());
        return r;
    }
    r.bookmarkMarkers = saved.bookmarkMarkers;
    r.bookmarkStackNames = saved.bookmarkStackNames;
    r.ok = r.items.count("view:receiver") == 1;
    if (!r.ok) { std::printf("  %s: no receiver view drawn\n", tag.c_str()); }
    return r;
}

}  // namespace

int main() {
    std::printf("test_bookmark_stacking_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-bm-stack-" + std::to_string(pid()));
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
    setEnv("FOXSDR_PATCH_START", "");
    writeBookmarks(g_dir / "appdata");

    // --- stacking on: both names drawn, one on each of two rows -------------
    {
        const Result r = once("stack-on", /*markers=*/true, /*stackNames=*/true);
        CHECK(r.ok);
        CHECK(r.items.count("bm:name:0") == 1);
        CHECK(r.items.count("bm:name:1") == 1);
        CHECK(r.rects.count("bm:name:0") == 1);
        CHECK(r.rects.count("bm:name:1") == 1);
        if (r.rects.count("bm:name:0") == 1 && r.rects.count("bm:name:1") == 1) {
            const Rect& row0 = r.rects.at("bm:name:0");
            const Rect& row1 = r.rects.at("bm:name:1");
            std::printf("    stacking on: row0 y %.1f..%.1f, row1 y %.1f..%.1f\n", row0.y0,
                        row0.y1, row1.y0, row1.y1);
            // Row 1 sits BELOW row 0 (larger y - ImGui's origin is top-left),
            // by roughly one row's height, and the two names' x-extents
            // overlap, which is exactly why the un-stacked code could not
            // have drawn both on one row.
            CHECK(row1.y0 > row0.y0);
            CHECK(row0.x0 < row1.x1 && row1.x0 < row0.x1);
        }
        CHECK(r.bookmarkMarkers);
        CHECK(r.bookmarkStackNames);
    }

    // --- stacking off: only the first name is drawn --------------------------
    {
        const Result r = once("stack-off", /*markers=*/true, /*stackNames=*/false);
        CHECK(r.ok);
        CHECK(r.items.count("bm:name:0") == 1);
        // THE BREAK-IT PROOF (see the task notes) forces this branch to -1
        // in drawBookmarkMarkers and confirms this exact CHECK goes RED.
        CHECK(r.items.count("bm:name:1") == 0);
        CHECK(r.bookmarkMarkers);
        CHECK(!r.bookmarkStackNames);
    }

    // --- markers off: no bookmark name drawn at all, both switches persist ---
    {
        const Result r = once("markers-off", /*markers=*/false, /*stackNames=*/true);
        CHECK(r.ok);
        CHECK(r.items.count("bm:name:0") == 0);
        CHECK(r.items.count("bm:name:1") == 0);
        CHECK(!r.bookmarkMarkers);
        CHECK(r.bookmarkStackNames);
    }

    const int rc = testSummary("test_bookmark_stacking_app");
    if (rc == 0) { fs::remove_all(g_dir, ec); }
    return rc;
}
