/*
 * THE MARKERS WINDOW, THROUGH THE APPLICATION (stub campaign B10). tests/test_freq_markers_app.cpp (master's, 0.99.57) holds the waterfall's right-click: the menu
 * drops nothing by itself, Drop marker here, Remove marker, the long press, the patch Display part in absolute hertz. It reaches the Markers window only so far as
 * "the row is there". Everything the window DOES - type a note, Tune, Bookmark, the x, Clear all markers - was never held by a test that runs the application,
 * and in this port each of them is a COMMAND to the engine (FOXAPP_OP_MARKER_NOTE, FOXAPI_OP_SET_FREQUENCY, FOXAPP_OP_BOOKMARK_ADD_ROWS, MARKER_REMOVE,
 * MARKER_CLEAR) that the engine answers on its pass. This runs cascade itself on a private APPDATA that holds a markers.json this wrote, drives a scripted pointer
 * and keyboard (FOXSDR_INPUT_SCRIPT - nothing outside the process is touched), and reads back what the run saved:
 *
 *   note      clicking a row's note box and typing puts the typed words in markers.json - a keystroke at a time, so the whole of them, and nothing the box showed before;
 *   tune      Tune on M2 retunes the receiver there (the saved config's centre and VFO offset add up to the marker's frequency);
 *   bookmark  Bookmark on M2 adds a bookmark named by the note, at the marker's frequency, and leaves the marker where it was;
 *   remove    the x on M1 removes it and only it; the number M1 is not handed out again (the file's count stays);
 *   clear     Clear all markers asks again ("Yes, remove all 2"), empties the list, and the count starts at M1 again.
 *
 * Isolated like every test that starts the application: a private APPDATA, telemetry points at a black hole.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <algorithm>
#include <cmath>
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
#include "core/freq_manager.hpp"
#include "core/freq_markers.hpp"
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
    float cx() const { return 0.5f * (x0 + x1); }
    float cy() const { return 0.5f * (y0 + y1); }
};

struct Census {
    bool ok = false;
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
    bool has(const std::string& k) const { return items.count(k) == 1; }
    bool hasRect(const std::string& k) const { return rects.count(k) == 1; }
};

fs::path g_dir;
fs::path g_markers;    // the private APPDATA's markers.json
fs::path g_bookmarks;  // ...and bookmarks.json

// Two markers, written fresh before every run (the runs change them): M1 at 99.5 MHz with no note, M2 at 100.5 MHz noted "keep". The count says M3 is next.
void seedMarkers() {
    std::error_code ec;
    fs::create_directories(g_markers.parent_path(), ec);
    std::ofstream f(g_markers, std::ios::binary | std::ios::trunc);
    f << "{ \"schemaVersion\": 1, \"next\": 3, \"markers\": [\n"
         "  { \"n\": 1, \"freqHz\": 99500000.0, \"noted\": 1790000000, \"note\": \"\" },\n"
         "  { \"n\": 2, \"freqHz\": 100500000.0, \"noted\": 1790000100, \"note\": \"keep\" } ] }\n";
    std::ofstream b(g_bookmarks, std::ios::binary | std::ios::trunc);
    b << "{ \"schemaVersion\": 1, \"bookmarks\": [] }\n";
}

Census once(const std::string& tag, const std::string& script, int frames) {
    Census c;
    seedMarkers();
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    const fs::path scriptFile = g_dir / (tag + ".script");
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
             "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", "
             "\"mainView\": \"receiver\", \"bandPlanOverlay\": false }\n";
    }
    {
        std::ofstream f(scriptFile, std::ios::binary | std::ios::trunc);
        f << script;
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_WINDOW_SIZE", "1600x900");
    setEnv("FOXSDR_INPUT_SCRIPT", script.empty() ? "" : scriptFile.string());
    setEnv("FOXSDR_SCRIPT_TRACE", script.empty() ? "" : (g_dir / (tag + ".trace")).string());
    setEnv("FOXSDR_PATCH_FILE", "");
    setEnv("FOXSDR_PATCH_START", "");
    const std::string out = run("\"" + exePath() + "\" --frames " + std::to_string(frames) + " 2>&1");
    const bool rendered = out.find("rendered " + std::to_string(frames) + " frames") != std::string::npos;
    const bool written = out.find("ui census written") != std::string::npos;
    if (!rendered || !written) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), out.c_str());
        return c;
    }
    std::ifstream in(census);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.rfind("item ", 0) == 0) { c.items.insert(line.substr(5)); }
        if (line.rfind("rect ", 0) != 0) { continue; }
        std::istringstream ss(line.substr(5));
        std::string name;
        Rect rc;
        ss >> name >> rc.x0 >> rc.y0 >> rc.x1 >> rc.y1;
        c.rects[name] = rc;
    }
    c.ok = true;
    return c;
}

std::string rightClick(int f, float x, float y) {
    std::ostringstream s;
    s << f << " screen " << x << ' ' << y << '\n' << f + 2 << " rdown\n" << f + 3 << " rup\n";
    return s.str();
}

std::string click(int f, float x, float y) {
    std::ostringstream s;
    s << f << " screen " << x << ' ' << y << '\n' << f + 2 << " down\n" << f + 3 << " up\n";
    return s.str();
}

cascade::core::FreqMarkers savedMarkers() {
    cascade::core::FreqMarkers m;
    std::string err;
    if (!m.load(g_markers.string(), err)) { std::printf("  markers.json did not load: %s\n", err.c_str()); }
    return m;
}

cascade::core::FreqManager savedBookmarks() {
    cascade::core::FreqManager b;
    std::string err;
    if (!b.load(g_bookmarks.string(), err)) { std::printf("  bookmarks.json did not load: %s\n", err.c_str()); }
    return b;
}

cascade::core::AppConfig savedConfig(const std::string& tag) {
    cascade::core::AppConfig c;
    std::string err;
    if (!cascade::core::ConfigStore::load((g_dir / (tag + ".json")).string(), c, err)) {
        std::printf("  %s: the saved config did not load: %s\n", tag.c_str(), err.c_str());
    }
    return c;
}

}  // namespace

int main() {
    std::printf("test_markers_window_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-fmkw-" + std::to_string(pid()));
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
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL", "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("FOXSDR_SINGLE_VIEWPORT", "1");
    g_markers = g_dir / "appdata" / "foxsdr" / "markers.json";
    g_bookmarks = g_dir / "appdata" / "foxsdr" / "bookmarks.json";

    // --- what Copy puts on the clipboard: the window copies the engine's list, and the text must be the one FreqMarkers::clipboardText makes (the window's Copy
    // key is not reachable by a scripted pointer - the clipboard is the desktop's - so the text is held here) -----------------------------------------------
    {
        seedMarkers();
        const cascade::core::FreqMarkers m = savedMarkers();
        CHECK(m.size() == 2u);
        CHECK(!m.clipboardText(true).empty());
        CHECK(cascade::core::formatMarkersClipboard(m.list(), true) == m.clipboardText(true));
        CHECK(cascade::core::formatMarkersClipboard({}, true).empty());
    }

    // --- the receiver's waterfall, found, and the menu's "List markers..." with it ---------------------------------------------------------------
    const Census learn = once("learn", "", 40);
    CHECK(learn.ok && learn.hasRect("fmk:wf:receiver"));
    if (!learn.hasRect("fmk:wf:receiver")) { return testSummary("test_markers_window_app"); }
    const Rect wf = learn.rects.at("fmk:wf:receiver");
    // Left of centre, clear of the VFO line and of both markers (99.5 and 100.5 MHz are a megahertz either side of the span's middle), and high enough that
    // the menu opens below the pointer without being pushed back onto the screen.
    const float cx = wf.x0 + 0.37f * (wf.x1 - wf.x0);
    const float cy = wf.y0 + 0.25f * (wf.y1 - wf.y0);
    const Census menu = once("menu", rightClick(20, cx, cy), 40);
    CHECK(menu.ok && menu.hasRect("fmk:menu:list"));
    CHECK(!menu.hasRect("fmk:menu:remove"));   // no marker is under the pointer
    if (!menu.hasRect("fmk:menu:list")) { return testSummary("test_markers_window_app"); }
    const Rect list = menu.rects.at("fmk:menu:list");
    const std::string openWindow = rightClick(20, cx, cy) + click(30, list.cx(), list.cy());

    // --- the window, found ---------------------------------------------------------------------------------------------------------------------------
    const Census win = once("window", openWindow, 60);
    CHECK(win.ok);
    CHECK(win.has("fmk:window"));
    CHECK(win.has("fmk:row:1"));
    CHECK(win.has("fmk:row:2"));
    for (const char* k : {"fmk:note:1", "fmk:note:2", "fmk:tune:2", "fmk:bookmark:2", "fmk:remove:1", "fmk:clear"}) { CHECK(win.hasRect(k)); }
    if (!win.hasRect("fmk:note:1") || !win.hasRect("fmk:note:2") || !win.hasRect("fmk:tune:2") || !win.hasRect("fmk:bookmark:2") || !win.hasRect("fmk:remove:1") ||
        !win.hasRect("fmk:clear")) {
        return testSummary("test_markers_window_app");
    }
    // The window opens (and its rows are drawn) a few frames after the click that asked for it; every action below waits for frame 40 and reads the same census.
    // (A row's place follows the list's order by frequency: M1 first, M2 second.)

    // --- a note typed into M1's box: all of it, in the file ----------------------------------------------------------------------------------------
    {
        const Rect box = win.rects.at("fmk:note:1");
        const Census c = once("note", openWindow + click(40, box.cx(), box.cy()) + "46 text G4ABC beacon\n", 70);
        CHECK(c.ok);
        const cascade::core::FreqMarkers m = savedMarkers();
        CHECK(m.size() == 2u);
        if (m.size() == 2u) {
            std::printf("    typed into M1's box: \"%s\"\n", m.list()[0].note.c_str());
            CHECK(m.list()[0].number == 1);
            CHECK(m.list()[0].note == "G4ABC beacon");
            CHECK(m.list()[1].note == "keep");   // the other row's note is not touched
        }
    }

    // --- Tune on M2 retunes the receiver there --------------------------------------------------------------------------------------------------------
    {
        const Rect b = win.rects.at("fmk:tune:2");
        const Census c = once("tune", openWindow + click(40, b.cx(), b.cy()), 60);
        CHECK(c.ok);
        const cascade::core::AppConfig cfg = savedConfig("tune");
        std::printf("    after Tune on M2: centre %.0f Hz, VFO offset %.0f Hz\n", cfg.centerHz, cfg.vfoOffsetHz);
        CHECK(std::fabs((cfg.centerHz + cfg.vfoOffsetHz) - 100.5e6) < 1.0);
        CHECK(savedMarkers().size() == 2u);   // a Tune drops nothing and removes nothing
    }

    // --- Bookmark on M2 --------------------------------------------------------------------------------------------------------------------------------
    {
        const Rect b = win.rects.at("fmk:bookmark:2");
        const Census c = once("bookmark", openWindow + click(40, b.cx(), b.cy()), 60);
        CHECK(c.ok);
        const cascade::core::FreqManager bm = savedBookmarks();
        CHECK(bm.list().size() == 1u);
        if (bm.list().size() == 1u) {
            const cascade::core::Bookmark& k = bm.list()[0];
            std::printf("    bookmark: \"%s\" at %.0f Hz, %s %.0f Hz\n", k.name.c_str(), k.freqHz, k.mode.c_str(), k.bandwidthHz);
            CHECK(k.name == "keep");   // the note names it
            CHECK(std::fabs(k.freqHz - 100.5e6) < 1.0);
            CHECK(!k.mode.empty());
            CHECK(!k.scan);
        }
        CHECK(savedMarkers().size() == 2u);   // the marker stays
    }

    // --- the x on M1 removes it and only it ----------------------------------------------------------------------------------------------------------
    {
        const Rect b = win.rects.at("fmk:remove:1");
        const Census c = once("remove", openWindow + click(40, b.cx(), b.cy()), 60);
        CHECK(c.ok);
        cascade::core::FreqMarkers m = savedMarkers();
        CHECK(m.size() == 1u);
        if (m.size() == 1u) { CHECK(m.list()[0].number == 2); }
        // M1 is not handed out again: the file's count was 3 and still is.
        CHECK(m.add(101.5e6, 1790000200) == 3);
    }

    // --- Clear all markers asks again, empties the list, and the count starts at M1 ---------------------------------------------------------------------
    {
        const Rect b = win.rects.at("fmk:clear");
        const Census learnYes = once("clear-learn", openWindow + click(40, b.cx(), b.cy()), 60);
        CHECK(learnYes.ok && learnYes.hasRect("fmk:clear:yes"));
        CHECK(savedMarkers().size() == 2u);   // asking is not clearing
        if (learnYes.hasRect("fmk:clear:yes")) {
            const Rect yes = learnYes.rects.at("fmk:clear:yes");
            const Census c = once("clear", openWindow + click(40, b.cx(), b.cy()) + click(48, yes.cx(), yes.cy()), 70);
            CHECK(c.ok);
            cascade::core::FreqMarkers m = savedMarkers();
            CHECK(m.empty());
            CHECK(m.add(101.5e6, 1790000200) == 1);   // only a clear starts again at M1
        }
    }

    const int rc = testSummary("test_markers_window_app");
    if (rc == 0) {
        fs::remove_all(g_dir, ec);
    } else {
        std::printf("  kept for inspection: %s\n", g_dir.string().c_str());
    }
    return rc;
}
