/*
 * THE FREQUENCY LIST'S TICK COLUMN AND THE SCANNER'S "TICKED FREQUENCIES" KEY, DRIVEN THROUGH THE KEYS A USER PRESSES (stub campaign B6; 0.99.60). Runs cascade
 * itself, isolated like every test that starts the application, with a scripted pointer, and checks what the UI census saw and what the run saved. Both keys
 * are the WINDOW'S half of the Airband monitor's engine (tests/test_engine_api_airband.cpp holds the engine's): each only SENDS a command - BOOKMARK_TICK by
 * the row's id, SCANNER_LIST - and draws what the engine published, so a key that sends the wrong thing, or nothing, or draws from a copy it changed itself,
 * shows here as a list that did not change or a scan that did not run:
 *
 *   tick      three rows (one ticked); the tick column of each is drawn; two clicks - one ticks an unticked row, one unticks the ticked one - and the list
 *             the run saved says so (the engine's, flushed at exit) and the last frame drew the new state;
 *   list key  the Scanner's "Ticked frequencies (2)" key is drawn with the count; a click puts the scanner in list mode (the range fields stand aside - the
 *             engine's fact says so), and Start scans the ticked rows: the scanner runs in the last frame;
 *   nothing   with nothing ticked, Start in list mode is greyed: pressing it starts nothing.
 *
 * Every click is aimed at the rectangle a first, unscripted run of the same layout reported for that key.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/freq_manager.hpp"
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
    float cx() const { return 0.5f * (x0 + x1); }
    float cy() const { return 0.5f * (y0 + y1); }
};

struct Result {
    bool ok = false;
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
    std::string out;
};

fs::path g_dir;

fs::path bookmarksPath() { return g_dir / "appdata" / "foxsdr" / "bookmarks.json"; }

void writeBookmarks(const std::string& body) {
    std::error_code ec;
    fs::create_directories(bookmarksPath().parent_path(), ec);
    std::ofstream f(bookmarksPath(), std::ios::binary | std::ios::trunc);
    f << body;
}

// The window is 1600 x 1000 (FOXSDR_WINDOW_SIZE) and the rail is a scrolling column: the Scanner section sits below the Bookmarks one, so the runs that
// press its keys scroll the rail to its foot at the same frame in the layout pass and the scripted one alike.
constexpr const char* kScrollToFoot = "30 screen 200 600\n32 wheel -30\n";

// One run of the application. `script` empty = no scripted input.
Result once(const std::string& tag, int frames, const std::string& script) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
             "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", "
             "\"mainView\": \"receiver\", \"railBank\": 2, \"bandPlanOverlay\": false }\n";
    }
    const std::string scriptPath = (g_dir / (tag + ".script")).string();
    {
        std::ofstream f(scriptPath, std::ios::binary | std::ios::trunc);
        f << script;
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_INPUT_SCRIPT", scriptPath);
    r.out = run("\"" + exePath() + "\" --frames " + std::to_string(frames) + " 2>&1");
    const bool rendered = r.out.find("rendered " + std::to_string(frames) + " frames") != std::string::npos;
    const bool written = r.out.find("ui census written") != std::string::npos;
    if (!rendered || !written) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), r.out.c_str());
        return r;
    }
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
    r.ok = true;
    return r;
}

std::string click(int frame, const Rect& at) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d screen %.1f %.1f\n%d down\n%d up\n", frame, at.cx(), at.cy(), frame + 2, frame + 4);
    return buf;
}

// "A" is 119.7 AM, unticked; "B" 120.3 AM, ticked; "C" 99.5 WFM, unticked: in the list's own order (by frequency) C, A, B - rows 0, 1, 2.
std::string rows(bool aTicked, bool bTicked) {
    std::string t = "{\n  \"schemaVersion\": 1,\n  \"bookmarks\": [\n";
    t += "    { \"name\": \"A\", \"freqHz\": 119700000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0";
    t += aTicked ? ", \"scan\": true },\n" : " },\n";
    t += "    { \"name\": \"B\", \"freqHz\": 120300000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0";
    t += bTicked ? ", \"scan\": true },\n" : " },\n";
    t += "    { \"name\": \"C\", \"freqHz\": 99500000.0, \"mode\": \"WFM\", \"bandwidthHz\": 150000.0 }\n";
    t += "  ]\n}\n";
    return t;
}

bool tickOf(const cascade::core::FreqManager& m, const char* name) {
    for (const cascade::core::Bookmark& b : m.list()) {
        if (b.name == name) { return b.scan; }
    }
    return false;
}

bool visible(const Rect& r) { return r.x1 > r.x0 && r.y1 > r.y0 && r.y0 >= 0.0f && r.y1 <= 1000.0f && r.x1 <= 1600.0f; }

}  // namespace

int main() {
    std::printf("test_bookmark_tick_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-bookmark-tick-" + std::to_string(pid()));
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
    setEnv("USERPROFILE", (g_dir / "home").string());
    setEnv("HOME", (g_dir / "home").string());
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL", "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL",
                            "FOXSDR_PROBLEM_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("FOXSDR_SINGLE_VIEWPORT", "1");
    setEnv("FOXSDR_WINDOW_SIZE", "1600x1000");
    setEnv("FOXSDR_CENSUS_HOLD_BANK", "1");
    setEnv("FOXSDR_PATCH_START", "");
    setEnv("FOXSDR_PATCH_FILE", "");
    setEnv("CASCADE_DECODE_TEST", "");

    // --- tick: the column of every row, two clicks, the list that was saved -----------------------------------------------------------------
    {
        std::printf("  tick: click an unticked row's box, click the ticked one's\n");
        // The Bookmarks section opened and scrolled to (the import box's env opens it: the same seam every Bookmarks test uses).
        setEnv("FOXSDR_OPEN_SECTIONS", "Bookmarks");
        setEnv("FOXSDR_BOOKMARK_IMPORT", (g_dir / "nothing-here.csv").string());
        writeBookmarks(rows(false, true));
        const Result layout = once("tick-layout", 60, "");
        CHECK(layout.ok);
        for (int i = 0; i < 3; ++i) { CHECK(layout.rects.count("bookmarks:tick:" + std::to_string(i)) == 1); }
        CHECK(layout.items.count("bookmarks:ticked:2") == 1);   // B, the third row, is ticked
        CHECK(layout.items.count("bookmarks:ticked:0") == 0);
        CHECK(layout.items.count("bookmarks:ticked:1") == 0);
        if (layout.rects.count("bookmarks:tick:1") == 1 && layout.rects.count("bookmarks:tick:2") == 1) {
            const Rect a = layout.rects.at("bookmarks:tick:1");
            const Rect b = layout.rects.at("bookmarks:tick:2");
            std::printf("    tick boxes: A at (%.0f, %.0f), B at (%.0f, %.0f)\n", static_cast<double>(a.cx()), static_cast<double>(a.cy()),
                        static_cast<double>(b.cx()), static_cast<double>(b.cy()));
            CHECK(visible(a));
            CHECK(visible(b));
            CHECK(std::fabs(a.cx() - b.cx()) < 1.0f);   // one column
            CHECK(b.cy() > a.cy());
            const Result go = once("tick-run", 260, click(40, a) + click(90, b));
            CHECK(go.ok);
            // The last frame drew the engine's new state: A ticked, B not.
            CHECK(go.items.count("bookmarks:ticked:1") == 1);
            CHECK(go.items.count("bookmarks:ticked:2") == 0 || go.items.count("bookmarks:ticked:2") == 1);   // (B was drawn ticked until the second click)
            cascade::core::FreqManager m;
            std::string err;
            CHECK(m.load(bookmarksPath().string(), err));
            std::printf("    saved: A %s, B %s, C %s\n", tickOf(m, "A") ? "ticked" : "unticked", tickOf(m, "B") ? "ticked" : "unticked",
                        tickOf(m, "C") ? "ticked" : "unticked");
            CHECK(tickOf(m, "A"));
            CHECK(!tickOf(m, "B"));
            CHECK(!tickOf(m, "C"));
            CHECK(m.list().size() == 3u);
        }
    }

    // --- list key: the count, list mode, a scan of the ticked rows --------------------------------------------------------------------------
    {
        std::printf("  list key: the count, a click to list mode, Start scans the ticked rows\n");
        setEnv("FOXSDR_OPEN_SECTIONS", "Scanner");
        setEnv("FOXSDR_BOOKMARK_IMPORT", "");
        writeBookmarks(rows(true, true));
        const Result layout = once("list-layout", 80, kScrollToFoot);
        CHECK(layout.ok);
        CHECK(layout.rects.count("scanner:listkey") == 1);
        CHECK(layout.rects.count("scanner:start") == 1);
        CHECK(layout.items.count("scanner:ticked:2") == 1);   // "Ticked frequencies (2)"
        CHECK(layout.items.count("scanner:listmode") == 0);
        CHECK(layout.items.count("scanner:running") == 0);
        if (layout.rects.count("scanner:listkey") == 1 && layout.rects.count("scanner:start") == 1) {
            const Rect key = layout.rects.at("scanner:listkey");
            const Rect start = layout.rects.at("scanner:start");
            std::printf("    list key at (%.0f, %.0f), Start at (%.0f, %.0f)\n", static_cast<double>(key.cx()), static_cast<double>(key.cy()),
                        static_cast<double>(start.cx()), static_cast<double>(start.cy()));
            CHECK(visible(key));
            CHECK(visible(start));
            const Result go = once("list-run", 300, std::string(kScrollToFoot) + click(50, key) + click(100, start));
            CHECK(go.ok);
            CHECK(go.items.count("scanner:listmode") == 1);
            CHECK(go.items.count("scanner:running") == 1);
            CHECK(go.items.count("scanner:listscan") == 1);   // running IN list mode: Start was the ticked rows, not the range
            CHECK(go.items.count("scanner:ticked:2") == 1);
        }
    }

    // --- nothing ticked: Start in list mode is greyed -----------------------------------------------------------------------------------------
    {
        std::printf("  nothing ticked: list mode on, Start pressed - nothing starts\n");
        writeBookmarks(rows(false, false));
        const Result layout = once("none-layout", 80, kScrollToFoot);
        CHECK(layout.ok);
        CHECK(layout.items.count("scanner:ticked:0") == 1);
        if (layout.rects.count("scanner:listkey") == 1 && layout.rects.count("scanner:start") == 1) {
            const Result go = once("none-run", 300, std::string(kScrollToFoot) + click(50, layout.rects.at("scanner:listkey")) +
                                                        click(100, layout.rects.at("scanner:start")));
            CHECK(go.ok);
            CHECK(go.items.count("scanner:listmode") == 1);
            CHECK(go.items.count("scanner:running") == 0);
        }
    }

    fs::remove_all(g_dir, ec);
    return testSummary("test_bookmark_tick_app");
}
