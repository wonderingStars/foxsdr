/*
 * WATERFALL MARKERS, THROUGH THE APPLICATION (a user's request, 2026-09-30:
 * "a right click menu on the waterfall display that would give you an option
 * to drop a marker on the current cursor position ... clear all markers, list
 * all markers and maybe copy all markers to the clipboard"). The list and its
 * rules are pinned in tests/test_freq_markers.cpp and the tab layout in
 * tests/test_freq_marker_layout.cpp; this runs cascade itself, drives its
 * pointer with a scripted right-click (FOXSDR_INPUT_SCRIPT - nothing outside
 * the process is touched) and reads back the UI census and the markers.json
 * the run saved:
 *
 *   receiver   a right-click opens the menu and drops NOTHING by itself;
 *              "Drop marker here" saves M1 at the frequency the menu named,
 *              and its tab is drawn along the waterfall's foot on the very
 *              column that was clicked; a new run draws it again from disk;
 *              right-clicking on it offers "Remove marker M1", which empties
 *              the file.
 *   patch      the same menu on a Display part's waterfall (a generator radio
 *              at 100 MHz, 2 MHz wide) drops M2 - numbers are never reused -
 *              at the absolute frequency under the pointer, not at the part's
 *              offset from its radio; "List markers..." opens the Markers
 *              window with that marker's row.
 *
 * Isolated like every test that starts the application: a private APPDATA
 * holds markers.json, telemetry points at a black hole.
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
    // The value after a "prefix" item, e.g. the menu's "fmk:menu:at:<MHz>".
    std::string after(const std::string& prefix) const {
        for (const std::string& i : items) {
            if (i.rfind(prefix, 0) == 0) { return i.substr(prefix.size()); }
        }
        return std::string();
    }
};

fs::path g_dir;
fs::path g_markers;  // the private APPDATA's markers.json

Census once(const std::string& tag, bool patch, const std::string& script, int frames) {
    Census c;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    const fs::path scriptFile = g_dir / (tag + ".script");
    const fs::path patchFile = g_dir / (tag + ".patch");
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
             "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", "
             "\"mainView\": \""
          << (patch ? "patch" : "receiver") << "\", \"bandPlanOverlay\": false }\n";
    }
    if (patch) {
        // A generator radio at 100 MHz, 2 MHz wide, wired to one Display part
        // big enough to click on.
        std::ofstream f(patchFile, std::ios::binary | std::ios::trunc);
        f << "foxsdr-patch 6\n"
             "view 0 0 1\n"
             "node 1 0 0 40 40 0 0 100000000 0 - siggen 2000000 1 -50 1 Radio\n"
             "node 2 4 0 420 40 640 480 0 0 - - 0 0 -50 1 Display\n"
             "wire 1 0 2 0\n";
    }
    {
        std::ofstream f(scriptFile, std::ios::binary | std::ios::trunc);
        f << script;
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_WINDOW_SIZE", "1600x900");
    setEnv("FOXSDR_INPUT_SCRIPT", script.empty() ? "" : scriptFile.string());
    // One line a frame of what the scripted pointer did, kept with the run
    // when the test fails (2026-10-01: a lost Remove click had no evidence).
    setEnv("FOXSDR_SCRIPT_TRACE", script.empty() ? "" : (g_dir / (tag + ".trace")).string());
    setEnv("FOXSDR_PATCH_FILE", patch ? patchFile.string() : "");
    setEnv("FOXSDR_PATCH_START", patch ? "1" : "");
    const std::string out =
        run("\"" + exePath() + "\" --frames " + std::to_string(frames) + " 2>&1");
    const bool rendered =
        out.find("rendered " + std::to_string(frames) + " frames") != std::string::npos;
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

// A right-click at (x, y): pointer there at frame f, pressed and released.
std::string rightClick(int f, float x, float y) {
    std::ostringstream s;
    s << f << " screen " << x << ' ' << y << '\n'
      << f + 2 << " rdown\n"
      << f + 3 << " rup\n";
    return s.str();
}

// A left click at (x, y) after the pointer has rested there two frames.
std::string click(int f, float x, float y) {
    std::ostringstream s;
    s << f << " screen " << x << ' ' << y << '\n'
      << f + 2 << " down\n"
      << f + 3 << " up\n";
    return s.str();
}

// The VFO offset the run saved in its config: how a tune is seen from outside.
double savedVfoOffset(const std::string& tag) {
    cascade::core::AppConfig c;
    std::string err;
    if (!cascade::core::ConfigStore::load((g_dir / (tag + ".json")).string(), c, err)) {
        std::printf("  %s: the saved config did not load: %s\n", tag.c_str(), err.c_str());
        return -1.0;
    }
    return c.vfoOffsetHz;
}

cascade::core::FreqMarkers savedMarkers() {
    cascade::core::FreqMarkers m;
    std::string err;
    if (!m.load(g_markers.string(), err)) { std::printf("  markers.json did not load: %s\n", err.c_str()); }
    return m;
}

}  // namespace

int main() {
    std::printf("test_freq_markers_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-fmk-" + std::to_string(pid()));
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
    g_markers = g_dir / "appdata" / "foxsdr" / "markers.json";

    // --- the receiver's waterfall, found -------------------------------------
    const Census learn = once("rx-learn", false, "", 40);
    CHECK(learn.ok && learn.hasRect("fmk:wf:receiver"));
    if (!learn.hasRect("fmk:wf:receiver")) { return testSummary("test_freq_markers_app"); }
    const Rect wf = learn.rects.at("fmk:wf:receiver");
    // The view the waterfall sits in: what of the picture can actually be
    // seen. Its layout runs past this view's bottom edge, and a tab drawn at
    // the layout's foot was hidden under the frame.
    CHECK(learn.hasRect("view:receiver"));
    const float visibleBottom =
        learn.hasRect("view:receiver") ? std::min(wf.y1, learn.rects.at("view:receiver").y1) : wf.y1;
    // Left of centre, clear of the VFO line, and high enough that the menu
    // opens below the pointer without being pushed back onto the screen.
    const float cx = wf.x0 + 0.37f * (wf.x1 - wf.x0);
    const float cy = wf.y0 + 0.25f * (wf.y1 - wf.y0);
    std::printf("    receiver waterfall %.0f,%.0f..%.0f,%.0f; clicking %.0f,%.0f\n", wf.x0, wf.y0,
                wf.x1, wf.y1, cx, cy);

    // --- a right-click opens the menu and drops nothing by itself ------------
    const Census menu = once("rx-menu", false, rightClick(20, cx, cy), 40);
    CHECK(menu.ok);
    CHECK(menu.hasRect("fmk:menu:drop"));
    CHECK(menu.hasRect("fmk:menu:list"));
    CHECK(!menu.hasRect("fmk:menu:remove"));  // nothing under the pointer yet
    const std::string at = menu.after("fmk:menu:at:");
    std::printf("    the menu offers \"%s\"\n", at.c_str());
    CHECK(!at.empty());
    CHECK(savedMarkers().empty());
    if (!menu.hasRect("fmk:menu:drop")) { return testSummary("test_freq_markers_app"); }
    const Rect drop = menu.rects.at("fmk:menu:drop");
    // Where the drop item sits relative to the click: the same for any
    // right-click that leaves the menu room to open below and right of it.
    const float dropDx = drop.cx() - cx;
    const float dropDy = drop.cy() - cy;

    // --- Drop marker here -----------------------------------------------------
    const Census dropped =
        once("rx-drop", false, rightClick(20, cx, cy) + click(30, cx + dropDx, cy + dropDy), 50);
    CHECK(dropped.ok);
    {
        const cascade::core::FreqMarkers m = savedMarkers();
        CHECK(m.size() == 1u);
        if (m.size() == 1u) {
            const cascade::core::FreqMarker& k = m.list()[0];
            std::printf("    saved M%d at %.3f Hz\n", k.number, k.freqHz);
            CHECK(k.number == 1);
            // The frequency the menu named is the one saved, to the hertz.
            CHECK(std::fabs(k.freqHz - std::atof(at.c_str()) * 1.0e6) < 1.0);
            CHECK(k.notedUnix > 1700000000);
        }
    }
    CHECK(dropped.has("fmk:tab:1"));
    CHECK(dropped.has("fmk:specline:1"));  // and a line through the spectrum trace
    if (dropped.hasRect("fmk:tab:1")) {
        const Rect t = dropped.rects.at("fmk:tab:1");
        std::printf("    tab M1 at %.1f,%.1f..%.1f,%.1f\n", t.x0, t.y0, t.x1, t.y1);
        // On the column that was clicked (it reads rightward from its line,
        // or leftward at the edge), and along the waterfall's foot.
        CHECK(std::fabs(t.x0 - cx) <= 1.5f || std::fabs(t.x1 - cx) <= 1.5f);
        CHECK(t.y1 <= visibleBottom && t.y1 >= visibleBottom - 12.0f);
    }

    // --- a new run draws it from disk, and offers to remove it ---------------
    const Census again = once("rx-again", false, rightClick(20, cx, cy), 40);
    CHECK(again.ok);
    CHECK(again.has("fmk:tab:1"));
    CHECK(again.hasRect("fmk:menu:remove"));
    CHECK(again.hasRect("fmk:menu:list"));
    if (!again.hasRect("fmk:menu:remove") || !again.hasRect("fmk:menu:list")) {
        return testSummary("test_freq_markers_app");
    }
    const Rect remove = again.rects.at("fmk:menu:remove");
    const Rect list = again.rects.at("fmk:menu:list");
    const float listDx = list.cx() - cx;
    const float listDy = list.cy() - cy;

    const Census removed =
        once("rx-remove", false, rightClick(20, cx, cy) + click(30, remove.cx(), remove.cy()), 50);
    CHECK(removed.ok);
    CHECK(savedMarkers().empty());

    // --- a long press is the menu, and tunes nothing ---------------------------
    // Android has no right button: its touch layer turns a finger held still
    // for 450 ms into "left up, right down" in one batch. That exact sequence,
    // after 600 ms of real time, must open the menu and leave the VFO where it
    // was. A quick click at the same spot is the control: it DOES tune, so the
    // saved offset is shown to be able to see one.
    {
        std::ostringstream lp;
        lp << "20 screen " << cx << ' ' << cy << "\n"
           << "22 down\n"
           << "23 sleep 600\n"
           << "24 up\n"
           << "24 rdown\n"
           << "26 rup\n";
        const Census held = once("rx-longpress", false, lp.str(), 45);
        CHECK(held.ok);
        CHECK(!held.after("fmk:menu:at:").empty());
        const Census tapped = once("rx-tap", false, click(20, cx, cy), 45);
        CHECK(tapped.ok);
        const double base = savedVfoOffset("rx-learn");
        const double afterHold = savedVfoOffset("rx-longpress");
        const double afterTap = savedVfoOffset("rx-tap");
        std::printf("    VFO offset: %.0f Hz untouched, %.0f after a long press, %.0f after a click\n",
                    base, afterHold, afterTap);
        CHECK(afterHold == base);
        CHECK(afterTap != base);
    }

    // --- a patch Display part: the same menu, in absolute hertz --------------
    //
    // REAL TIME FIRST (2026-10-01). The patch's radio opens on a worker, and
    // until it has, the part has no centre: its waterfall is not measured
    // (no fmk:wf:patch) and its right-click is not armed - the canvas's own
    // button takes the press. A bounded run's frames are about a millisecond
    // each, so a whole 90-frame run is ~80 ms, and the radio opening inside
    // that was a race: a full ctest -j lost patch-drop's first right-click
    // (frame 40, ~60 ms in; M1's drop never happened, the second right-click
    // ~15 ms later opened the list), and three runs alone in a row lost this
    // learn run's rect twice. A right-click at frame 3 lost it every time;
    // the same right-click after "1 sleep 2000" did not. Each patch run
    // therefore holds two seconds of real time at frame 25, still inside the
    // watchdog's start-up budget, so the GUI thread's sleep is not a freeze.
    const std::string radioOpens = "25 sleep 2000\n";
    const Census plearn = once("patch-learn", true, radioOpens, 90);
    CHECK(plearn.ok && plearn.hasRect("fmk:wf:patch"));
    if (!plearn.hasRect("fmk:wf:patch")) { return testSummary("test_freq_markers_app"); }
    const Rect pw = plearn.rects.at("fmk:wf:patch");
    const float px = pw.x0 + 0.30f * (pw.x1 - pw.x0);
    const float py = pw.y0 + 0.20f * (pw.y1 - pw.y0);
    std::printf("    patch waterfall %.0f,%.0f..%.0f,%.0f; clicking %.0f,%.0f\n", pw.x0, pw.y0,
                pw.x1, pw.y1, px, py);
    // Drop, then right-click the same place again and open the list - after
    // the radio has had real time to open (see radioOpens above).
    const Census pdrop = once("patch-drop", true,
                              radioOpens +
                              rightClick(40, px, py) + click(50, px + dropDx, py + dropDy) +
                                  rightClick(60, px, py) + click(70, px + listDx, py + listDy),
                              90);
    CHECK(pdrop.ok);
    {
        const cascade::core::FreqMarkers m = savedMarkers();
        CHECK(m.size() == 1u);
        if (m.size() == 1u) {
            const cascade::core::FreqMarker& k = m.list()[0];
            // The radio is 100 MHz +/- 1 MHz across the part's picture.
            const double want = 99.0e6 + 2.0e6 * (px - pw.x0) / (pw.x1 - pw.x0);
            const double hzPerPx = 2.0e6 / (pw.x1 - pw.x0);
            std::printf("    saved M%d at %.3f Hz, pointer at %.3f Hz (%.0f Hz a pixel)\n",
                        k.number, k.freqHz, want, hzPerPx);
            CHECK(k.number == 2);  // M1 was removed; its number is not handed out again
            CHECK(std::fabs(k.freqHz - want) <= hzPerPx);
        }
    }
    CHECK(pdrop.has("fmk:tab:2"));
    CHECK(pdrop.has("fmk:specline:2"));
    if (pdrop.hasRect("fmk:tab:2")) {
        const Rect t = pdrop.rects.at("fmk:tab:2");
        CHECK(std::fabs(t.x0 - px) <= 1.5f || std::fabs(t.x1 - px) <= 1.5f);
        CHECK(t.y1 <= pw.y1 && t.y1 >= pw.y1 - 12.0f);
    }
    CHECK(pdrop.has("fmk:window"));
    CHECK(pdrop.has("fmk:row:2"));

    const int rc = testSummary("test_freq_markers_app");
    if (rc == 0) {
        fs::remove_all(g_dir, ec);
    } else {
        // Each scripted run's census, config and FOXSDR_SCRIPT_TRACE are kept.
        std::printf("  kept for inspection: %s\n", g_dir.string().c_str());
    }
    return rc;
}
