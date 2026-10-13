/*
 * THE DISPLAY MENU, THROUGH THE APPLICATION (0.99.76). A French user on 0.99.75, RTL-SDR V4,
 * thumbs-down: "the spectrum and the waterfall are very milky, one cannot see the signals ... an
 * effort on this problem would be welcome". The owner: "allow the users to set it themselves to
 * what they prefer by right clicking on it", and: "also add a tool tip when the mouse is over it
 * for 4 secs saying you can right click to change it". The arithmetic is pinned in
 * tests/test_display_range.cpp and the rest clock in tests/test_hover_dwell.cpp; this runs
 * cascade, right-clicks and hovers with the application's own input script, and reads back the UI
 * census and the config the run saved:
 *
 *   menu      a right-click on the receiver's waterfall (the marker menu) or its spectrum (the
 *             trace menu) carries the same Display block - Range: Automatic / Manual, Floor,
 *             Ceiling, Reset - and opening it changes nothing;
 *   manual    Manual and the two sliders move the spectrum's scale, the waterfall's colour map
 *             and the strength key together, and are saved; the key says MANUAL;
 *   ten dB    the ceiling stays 10 dB above the floor, the end that moved giving way;
 *   reset     Reset puts back Automatic, the standard window and the normal trace;
 *   restart   a saved Manual comes back as the key and the spectrum show it; a config from
 *             before the key existed with a range of its own opens in Manual;
 *   patch     a patch Spectrum part takes the same menu and, in Manual, the same window;
 *   tip       after four seconds of rest on the spectrum or the waterfall the tip appears; not
 *             before; a press, a menu, or a move takes it away and it needs another four.
 *
 * Each run is a real process; the tip runs hold the frame loop with the script's `sleep` verb
 * because a bounded run's frames are about a millisecond each and are not a clock.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <chrono>
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
    cascade::core::AppConfig saved;
    bool has(const std::string& k) const { return items.count(k) == 1; }
    bool hasRect(const std::string& k) const { return rects.count(k) == 1; }
    Rect rect(const std::string& k) const {
        const auto it = rects.find(k);
        return it == rects.end() ? Rect{} : it->second;
    }
};

fs::path g_dir;

struct Run {
    std::string tag;
    bool patch = false;
    bool receiving = true;        // CASCADE_DECODE_TEST: the receiver runs, so lines flow
    std::string extraConfig;      // more JSON members, each ", \"k\": v"
    std::string script;
    int frames = 60;
};

Census once(const Run& r) {
    Census c;
    const fs::path cfg = g_dir / (r.tag + ".json");
    const fs::path census = g_dir / (r.tag + ".census");
    const fs::path scriptFile = g_dir / (r.tag + ".script");
    const fs::path patchFile = g_dir / (r.tag + ".patch");
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
             "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", "
             "\"mainView\": \""
          << (r.patch ? "patch" : "receiver") << "\", \"bandPlanOverlay\": false" << r.extraConfig
          << " }\n";
    }
    if (r.patch) {
        std::ofstream f(patchFile, std::ios::binary | std::ios::trunc);
        f << "foxsdr-patch 6\n"
             "view 0 0 1\n"
             "node 1 0 0 40 40 0 0 100000000 0 - siggen 2000000 1 -50 1 Radio\n"
             "node 2 4 0 420 40 640 480 0 0 - - 0 0 -50 1 Display\n"
             "wire 1 0 2 0\n";
    }
    {
        std::ofstream f(scriptFile, std::ios::binary | std::ios::trunc);
        f << r.script;
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_WINDOW_SIZE", "1600x900");
    setEnv("FOXSDR_INPUT_SCRIPT", r.script.empty() ? "" : scriptFile.string());
    setEnv("FOXSDR_SCRIPT_TRACE", r.script.empty() ? "" : (g_dir / (r.tag + ".trace")).string());
    setEnv("FOXSDR_PATCH_FILE", r.patch ? patchFile.string() : "");
    setEnv("FOXSDR_PATCH_START", r.patch ? "1" : "");
    setEnv("CASCADE_DECODE_TEST", r.receiving ? "1" : "");
    const auto t0 = std::chrono::steady_clock::now();
    std::printf("  [run %s: %d frames]\n", r.tag.c_str(), r.frames);
    const std::string out =
        run("\"" + exePath() + "\" --frames " + std::to_string(r.frames) + " 2>&1");
    std::printf("  [run %s took %.1f s]\n", r.tag.c_str(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    if (out.find("rendered " + std::to_string(r.frames) + " frames") == std::string::npos ||
        out.find("ui census written") == std::string::npos) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", r.tag.c_str(), out.c_str());
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
    std::string err;
    if (!cascade::core::ConfigStore::load(cfg.string(), c.saved, err)) {
        std::printf("  %s: the saved config did not load: %s\n", r.tag.c_str(), err.c_str());
        return c;
    }
    c.ok = true;
    return c;
}

std::string at(int f, float x, float y) {
    std::ostringstream s;
    s << f << " screen " << x << ' ' << y << '\n';
    return s.str();
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

std::string clickRect(int f, const Rect& r) { return click(f, r.cx(), r.cy()); }

// A click a fraction of the way along a slider's frame (the census records the frame, not the
// label beside it).
std::string clickAlong(int f, const Rect& r, float frac) {
    return click(f, r.x0 + frac * (r.x1 - r.x0), r.cy());
}

std::string sleepMs(int f, int ms) { return std::to_string(f) + " sleep " + std::to_string(ms) + "\n"; }

// What the strength key said on the LAST frame it was drawn: floor, ceiling, manual (1 or 0).
struct Key {
    bool drawn = false;
    float floorDb = 0.0f, ceilingDb = 0.0f;
    bool manual = false;
};
Key keyOf(const Census& c) {
    Key k;
    if (!c.hasRect("wf:key:state")) { return k; }
    const Rect r = c.rect("wf:key:state");
    k.drawn = true;
    k.floorDb = r.x0;
    k.ceilingDb = r.y0;
    k.manual = r.x1 > 0.5f;
    return k;
}

// The window the SPECTRUM was drawn through on its last frame.
Rect specRange(const Census& c) { return c.rect("rx:spec:range"); }

bool menuComplete(const Census& c) {
    for (const char* n : {"wfm:menu:auto", "wfm:menu:manual", "wfm:menu:floor", "wfm:menu:ceiling",
                          "wfm:menu:reset"}) {
        if (!c.hasRect(n)) { return false; }
    }
    return c.has("wfm:menu");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_display_menu_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-dispmenu-" + std::to_string(pid()));
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

    const std::string manualCfg =
        ", \"waterfallRangeMode\": \"manual\", \"dbMin\": -60, \"dbMax\": -20";

    // --- the panels, found, and the range as it has always been ----------------------------
    const Census learn = once({"learn"});
    CHECK(learn.ok && learn.hasRect("fmk:wf:receiver") && learn.hasRect("trc:spec:receiver"));
    if (!learn.ok || !learn.hasRect("fmk:wf:receiver") || !learn.hasRect("trc:spec:receiver")) {
        return testSummary("test_display_menu_app");
    }
    const Rect wf = learn.rect("fmk:wf:receiver");
    const Rect sp = learn.rect("trc:spec:receiver");
    // Left of the tuned band, high in each panel so a menu opens below and right of the pointer.
    const float wx = wf.x0 + 0.2f * (wf.x1 - wf.x0);
    const float wy = wf.y0 + 0.15f * (wf.y1 - wf.y0);
    const float sx = sp.x0 + 0.2f * (sp.x1 - sp.x0);
    const float sy = sp.y0 + 0.3f * (sp.y1 - sp.y0);
    std::printf("    waterfall %.0f,%.0f..%.0f,%.0f; spectrum %.0f,%.0f..%.0f,%.0f\n", wf.x0, wf.y0,
                wf.x1, wf.y1, sp.x0, sp.y0, sp.x1, sp.y1);
    {
        const Key k = keyOf(learn);
        std::printf("    untouched: key %.0f / %.0f %s, spectrum %.0f..%.0f, config \"%s\" %.0f / %.0f\n",
                    k.floorDb, k.ceilingDb, k.manual ? "MANUAL" : "(no MANUAL)", specRange(learn).x0,
                    specRange(learn).y0, learn.saved.waterfallRangeMode.c_str(),
                    static_cast<double>(learn.saved.dbMin), static_cast<double>(learn.saved.dbMax));
        CHECK(k.drawn);
        CHECK(k.floorDb == -110.0f && k.ceilingDb == 0.0f);
        CHECK(!k.manual);
        CHECK(!learn.has("wf:key:manual"));
        CHECK(specRange(learn).x0 == -110.0f && specRange(learn).y0 == 0.0f);
        CHECK(learn.saved.waterfallRangeMode == "auto");
        CHECK(learn.saved.dbMin == -110.0f && learn.saved.dbMax == 0.0f);
        CHECK(!learn.has("wfm:menu"));
    }

    // --- the menu, on the waterfall, and nothing changed by opening it --------------------
    const Census wm = once({"menu-wf", false, true, "", rightClick(20, wx, wy)});
    CHECK(wm.ok && menuComplete(wm));
    CHECK(wm.hasRect("fmk:menu:drop"));   // the marker menu is still the marker menu
    CHECK(!wm.has("trc:menu"));
    CHECK(wm.saved.waterfallRangeMode == "auto");
    CHECK(wm.saved.dbMin == -110.0f && wm.saved.dbMax == 0.0f);
    // (Every step that aims at the menu's items is guarded by menuComplete(wm), so a build without
    // the menu fails the checks above and below instead of stopping at the first.)
    const Rect wAuto = wm.rect("wfm:menu:auto");
    const Rect wManual = wm.rect("wfm:menu:manual");
    const Rect wFloor = wm.rect("wfm:menu:floor");
    const Rect wCeil = wm.rect("wfm:menu:ceiling");
    const Rect wReset = wm.rect("wfm:menu:reset");
    std::printf("    waterfall menu: manual %.0f,%.0f floor %.0f..%.0f ceiling %.0f..%.0f\n",
                wManual.cx(), wManual.cy(), wFloor.x0, wFloor.x1, wCeil.x0, wCeil.x1);

    // --- ...and the same block on the spectrum's own menu --------------------------------------
    const Census sm = once({"menu-sp", false, true, "", rightClick(20, sx, sy)});
    CHECK(sm.ok && menuComplete(sm));
    CHECK(sm.has("trc:menu") && sm.hasRect("trc:menu:peak"));   // the trace menu is still the trace menu
    CHECK(!sm.hasRect("fmk:menu:drop"));
    CHECK(sm.saved.waterfallRangeMode == "auto");

    // --- Manual from the waterfall: the scale, the colours and the key move together ------------
    if (wm.ok && menuComplete(wm)) {
        const Census man = once(
            {"manual-wf", false, true, "",
             rightClick(20, wx, wy) + clickRect(30, wManual) + clickAlong(40, wFloor, 0.7f) +
                 clickAlong(50, wCeil, 0.7f) + at(60, wx, wy),
             90});
        CHECK(man.ok);
        const Key k = keyOf(man);
        const Rect s = specRange(man);
        std::printf("    Manual, floor 70%%, ceiling 70%%: config \"%s\" %.0f / %.0f; key %.0f / %.0f %s; "
                    "spectrum %.0f / %.0f\n",
                    man.saved.waterfallRangeMode.c_str(), static_cast<double>(man.saved.dbMin),
                    static_cast<double>(man.saved.dbMax), k.floorDb, k.ceilingDb,
                    k.manual ? "MANUAL" : "(no MANUAL)", s.x0, s.y0);
        CHECK(man.saved.waterfallRangeMode == "manual");
        // 70% along the frame, but the grab's own width and padding mean the value reads a little
        // further along (measured: -59 and -14), so the tolerance is wide enough for that and far
        // too narrow to be mistaken for the standard window (-110 / 0) or the other slider.
        CHECK_NEAR(man.saved.dbMin, -62.0, 6.0);       // 70% along -160..-20
        CHECK_NEAR(man.saved.dbMax, -16.0, 6.0);       // 70% along -100..20
        CHECK(man.saved.dbMax - man.saved.dbMin >= 10.0f);
        CHECK(k.drawn && k.manual);
        CHECK(man.has("wf:key:manual"));
        // One range, three views: the key, the spectrum's scale and the saved pair are the same
        // two numbers (the census prints to 0.01 dB).
        CHECK_NEAR(k.floorDb, man.saved.dbMin, 0.01);
        CHECK_NEAR(k.ceilingDb, man.saved.dbMax, 0.01);
        CHECK_NEAR(s.x0, man.saved.dbMin, 0.01);
        CHECK_NEAR(s.y0, man.saved.dbMax, 0.01);
    }

    // --- the ceiling stays 10 dB above the floor: the end that moved gives way -------------------
    if (wm.ok && menuComplete(wm)) {
        const Census tight = once(
            {"tight", false, true, manualCfg,
             rightClick(20, wx, wy) + clickAlong(30, wCeil, 0.05f) + clickAlong(40, wFloor, 0.95f) +
                 at(50, wx, wy),
             80});
        CHECK(tight.ok);
        std::printf("    from -60 / -20: ceiling dragged to 5%% then floor to 95%%: config %.0f / %.0f\n",
                    static_cast<double>(tight.saved.dbMin), static_cast<double>(tight.saved.dbMax));
        CHECK(tight.saved.waterfallRangeMode == "manual");
        CHECK(tight.saved.dbMax - tight.saved.dbMin >= 10.0f - 0.01f);
        // The ceiling could not go below floor + 10 (-50); then the floor could not go above
        // ceiling - 10 (-60).
        CHECK_NEAR(tight.saved.dbMax, -50.0, 1.5);
        CHECK_NEAR(tight.saved.dbMin, -60.0, 1.5);
    }

    // --- Manual from the spectrum's menu ---------------------------------------------------------
    if (sm.ok && menuComplete(sm)) {
        const Census man = once({"manual-sp", false, true, "",
                                 rightClick(20, sx, sy) + clickRect(30, sm.rect("wfm:menu:manual")) +
                                     clickAlong(40, sm.rect("wfm:menu:floor"), 0.7f),
                                 80});
        CHECK(man.ok);
        std::printf("    spectrum menu, Manual then floor 70%%: config \"%s\" %.0f / %.0f\n",
                    man.saved.waterfallRangeMode.c_str(), static_cast<double>(man.saved.dbMin),
                    static_cast<double>(man.saved.dbMax));
        CHECK(man.saved.waterfallRangeMode == "manual");
        CHECK_NEAR(man.saved.dbMin, -62.0, 6.0);
        CHECK(man.saved.dbMax == 0.0f);
        CHECK(keyOf(man).manual);
    }

    // --- Reset: Automatic, the standard window, the normal trace -------------------------------------
    if (wm.ok && menuComplete(wm)) {
        const Census reset = once({"reset", false, true, manualCfg + ", \"spectrumTraceMode\": \"peak\"",
                                   rightClick(20, wx, wy) + clickRect(30, wReset) + at(40, wx, wy), 90});
        CHECK(reset.ok);
        const Key k = keyOf(reset);
        std::printf("    Reset from Manual -60 / -20 and Peak hold: config \"%s\" %.0f / %.0f, trace \"%s\"; "
                    "key now %.0f / %.0f %s\n",
                    reset.saved.waterfallRangeMode.c_str(), static_cast<double>(reset.saved.dbMin),
                    static_cast<double>(reset.saved.dbMax), reset.saved.spectrumTraceMode.c_str(),
                    k.floorDb, k.ceilingDb, k.manual ? "MANUAL" : "(no MANUAL)");
        CHECK(reset.saved.waterfallRangeMode == "auto");
        CHECK(reset.saved.dbMin == -110.0f && reset.saved.dbMax == 0.0f);
        CHECK(reset.saved.spectrumTraceMode == "normal");
        CHECK(k.drawn && !k.manual);
        CHECK_NEAR(k.floorDb, -110.0, 0.01);
        CHECK_NEAR(k.ceilingDb, 0.0, 0.01);
        CHECK_NEAR(specRange(reset).x0, -110.0, 0.01);
        CHECK_NEAR(specRange(reset).y0, 0.0, 0.01);
        // ...and the same run showed Manual before the press (the control for the lines above).
        CHECK(reset.has("wf:key:manual"));
    }

    // --- Automatic chosen again: the standard window, the pair remembered ------------------------------
    if (wm.ok && menuComplete(wm)) {
        const Census autoAgain = once({"auto-again", false, true, manualCfg,
                                       rightClick(20, wx, wy) + clickRect(30, wAuto) + at(40, wx, wy), 90});
        CHECK(autoAgain.ok);
        const Key k = keyOf(autoAgain);
        std::printf("    Automatic from Manual -60 / -20: config \"%s\" %.0f / %.0f; key %.0f / %.0f\n",
                    autoAgain.saved.waterfallRangeMode.c_str(), static_cast<double>(autoAgain.saved.dbMin),
                    static_cast<double>(autoAgain.saved.dbMax), k.floorDb, k.ceilingDb);
        CHECK(autoAgain.saved.waterfallRangeMode == "auto");
        CHECK(autoAgain.saved.dbMin == -60.0f && autoAgain.saved.dbMax == -20.0f);   // remembered
        CHECK(k.floorDb == -110.0f && k.ceilingDb == 0.0f && !k.manual);             // not in force
    }

    // --- restart: a saved Manual comes back; so does a range set before the key existed -------------
    {
        const Census back = once({"restart", false, true, manualCfg});
        const Key k = keyOf(back);
        std::printf("    restart in Manual: key %.0f / %.0f %s, spectrum %.0f / %.0f\n", k.floorDb,
                    k.ceilingDb, k.manual ? "MANUAL" : "(no MANUAL)", specRange(back).x0, specRange(back).y0);
        CHECK(back.ok && k.drawn && k.manual);
        CHECK(k.floorDb == -60.0f && k.ceilingDb == -20.0f);
        CHECK(specRange(back).x0 == -60.0f && specRange(back).y0 == -20.0f);
        CHECK(back.has("wf:key:manual"));
        CHECK(back.saved.waterfallRangeMode == "manual");

        // A config from 0.99.75: Display > Min dB / Max dB moved, no mode key at all.
        const Census old = once({"old-config", false, true, ", \"dbMin\": -95, \"dbMax\": -20"});
        const Key ko = keyOf(old);
        std::printf("    a 0.99.75 config with -95 / -20: key %.0f / %.0f %s, saved \"%s\"\n", ko.floorDb,
                    ko.ceilingDb, ko.manual ? "MANUAL" : "(no MANUAL)", old.saved.waterfallRangeMode.c_str());
        CHECK(old.ok && ko.drawn && ko.manual);
        CHECK(ko.floorDb == -95.0f && ko.ceilingDb == -20.0f);
        CHECK(old.saved.waterfallRangeMode == "manual");

        // ...and one that never moved them stays Automatic.
        const Census plain = once({"old-plain", false, true, ", \"dbMin\": -110, \"dbMax\": 0"});
        CHECK(plain.ok && !keyOf(plain).manual && plain.saved.waterfallRangeMode == "auto");
    }

    // --- the tip -------------------------------------------------------------------------------------------
    //
    // Real time is what it measures, so each run holds the frame loop with `sleep` steps (the script
    // allows 5 s at most in one) and the pointer is put on the panel first, a few frames ahead so
    // the rest has begun when the sleeping starts.
    //
    // WHAT EACH RUN ASSERTS COMES FROM THE REST THE RUN ITSELF MEASURED, not from the rest it
    // planned. The census reports the pointer's rest on the last frame ("resting N s", wfm:tip:state),
    // and that is the number the product compared with its dwell: so the tip must be up exactly when
    // it reached four seconds. A planned 3 s rest on a slow machine (GitHub's Linux runners draw at
    // about half this desktop's speed, and 0.99.74's first CI run failed on exactly this class of
    // real-time assertion) can last past four seconds once the frames either side of the sleeps are
    // counted, and the product is right to show the tip then. The "short" side - a rest under the
    // dwell shows nothing - is exercised only when the measured rest really was under it; when a
    // machine overshoots, the run says it could not exercise that side and does not fail. The long
    // runs plan 5 s, which only a clock that stopped could bring under four, so they assert the
    // measured rest reached the dwell as well as the tip.
    //
    // THE FOUR SECONDS HERE ARE THE OWNER'S ("when the mouse is over it for 4 secs"), a number of
    // this file's own and not gui/hover_dwell.hpp's kDisplayTipDwellS: a product whose dwell were
    // changed would otherwise move the test's with it and no run could disagree
    // (tests/test_hover_dwell.cpp pins the constant itself).
    constexpr double kOwnerDwellS = 4.0;
    // Within this of the dwell the float the census carries and the double the product compared
    // cannot be told apart: assert nothing about the tip for such a run.
    constexpr double kDwellEdgeS = 0.01;
    struct TipRun {
        const char* tag;
        float x, y;
        int sleepsMs[3];
        const char* panel;      // "waterfall" or "spectrum"
        bool plannedLong;       // planned past the dwell (else planned under it)
    };
    const TipRun tips[] = {
        // 1.5 + 1.5 s planned: under the four seconds.
        {"tip-short-wf", wx, wy + 40.0f, {1500, 1500, 0}, "waterfall", false},
        // 2.5 + 2.5 s planned: past it.
        {"tip-long-wf", wx, wy + 40.0f, {2500, 2500, 0}, "waterfall", true},
        {"tip-short-sp", sx, sy, {1500, 1500, 0}, "spectrum", false},
        {"tip-long-sp", sx, sy, {2500, 2500, 0}, "spectrum", true},
    };
    for (const TipRun& t : tips) {
        std::string script = at(20, t.x, t.y);
        int f = 22;
        for (const int ms : t.sleepsMs) {
            if (ms > 0) {
                script += sleepMs(f, ms);
                f += 2;
            }
        }
        const Census c = once({t.tag, false, true, "", script, f + 12});
        const std::string note = std::string("wfm:tip:") + t.panel;
        const Rect st = c.rect("wfm:tip:state");
        const double measuredS = static_cast<double>(st.y0);
        const bool atEdge = std::fabs(measuredS - kOwnerDwellS) < kDwellEdgeS;
        const bool reached = measuredS >= kOwnerDwellS;
        std::printf("    %s: tip %s (last frame: shown %.0f, resting %.2f s; planned %s the %.0f s dwell)\n",
                    t.tag, c.has(note) ? "SHOWN" : "not shown", st.x0, measuredS,
                    t.plannedLong ? "past" : "under", kOwnerDwellS);
        CHECK(c.ok);
        CHECK(c.hasRect("wfm:tip:state"));
        CHECK(!c.has(std::string("wfm:tip:") + (std::string(t.panel) == "waterfall" ? "spectrum" : "waterfall")));
        if (atEdge) {
            std::printf("    %s: the measured rest is within %.2f s of the dwell; the tip is not asserted\n",
                        t.tag, kDwellEdgeS);
        } else {
            // The rule: the tip was up exactly when the measured rest reached the dwell.
            CHECK(c.has(note) == reached);
            CHECK((st.x0 > 0.5f) == reached);
        }
        if (t.plannedLong) {
            // Sleeps only lengthen a rest, so this cannot fail for want of speed.
            CHECK(reached);
        } else if (reached) {
            std::printf("    %s: could not exercise the short side on this machine: the measured rest "
                        "%.2f s overshot the %.0f s dwell (the tip shown is the right answer)\n",
                        t.tag, measuredS, kOwnerDwellS);
        } else {
            std::printf("    %s: the short side was exercised: %.2f s of rest, under the %.0f s dwell; "
                        "tip %s\n",
                        t.tag, measuredS, kOwnerDwellS, c.has(note) ? "SHOWN (wrong)" : "not shown (right)");
        }
        // A resting hand is not a menu: nothing opened.
        CHECK(!c.has("wfm:menu"));
    }

    // A right-click while the tip is up opens the menu and takes the tip away.
    {
        const Census c = once({"tip-menu", false, true, "",
                               at(20, wx, wy + 40.0f) + sleepMs(22, 2500) + sleepMs(24, 2500) +
                                   rightClick(40, wx, wy + 40.0f),
                               60});
        const Rect st = c.rect("wfm:tip:state");
        std::printf("    tip-menu: tip %s before the right-click; menu %s; last frame shown %.0f\n",
                    c.has("wfm:tip:waterfall") ? "SHOWN" : "not shown",
                    c.has("wfm:menu") ? "open" : "NOT open", st.x0);
        CHECK(c.ok && c.has("wfm:tip:waterfall"));
        CHECK(c.has("wfm:menu"));
        CHECK(st.x0 < 0.5f);          // the last frame: no tip
    }

    // WHAT A GESTURE DOES, MEASURED INSIDE THE ONE RUN. A move past the slop and a press each END the
    // rest, and the next tip needs a full dwell from where the rest began again. The three runs below
    // do not bound the new rest by a number of seconds (an absolute bound is a statement about the
    // machine's speed: GitHub's Linux runners draw at about half this desktop's, and a rest that
    // began again can last longer there than the bound allows); they compare the rest the run ENDED
    // with against the rest it had when the tip was last up, both read from the same clock:
    //   - the tip was up before the gesture (wfm:tip:<panel> was noted, and wfm:tip:lastshown holds the
    //     rest on the last frame it was up, which reached the dwell);
    //   - the rest on the last frame is SHORTER than that one: the counter restarted on the gesture.
    //     A state machine that ignored the gesture keeps counting, and the two are equal;
    //   - the rest was counted again after the gesture (more than the script slept, less a margin; a
    //     cleared state machine reads 0), and the tip on the last frame follows the same measured
    //     rule as the tip-short / tip-long runs: up exactly when the new rest has itself reached the
    //     dwell. When it has not, the property the run is named after was exercised; when a slow
    //     machine let it reach the dwell, the run says it could not exercise that side and does not
    //     fail.
    const auto checkRestarted = [&](const char* tag, const Census& c, const char* panel, double sleptS) {
        const std::string note = std::string("wfm:tip:") + panel;
        const Rect st = c.rect("wfm:tip:state");
        const Rect was = c.rect("wfm:tip:lastshown");
        const double afterS = static_cast<double>(st.y0);   // the rest on the last frame
        const double wasS = static_cast<double>(was.x0);    // the rest on the last frame the tip was up
        std::printf("    %s: tip %s earlier (up at %.2f s of rest); last frame shown %.0f, resting %.2f s\n",
                    tag, c.has(note) ? "SHOWN" : "not shown", wasS, st.x0, afterS);
        CHECK(c.ok && c.has(note));
        CHECK(c.hasRect("wfm:tip:state"));
        CHECK(c.hasRect("wfm:tip:lastshown"));
        CHECK(wasS >= kOwnerDwellS);              // it was up, so that rest had reached the dwell
        CHECK(afterS < wasS);                     // the counter restarted on the gesture
        CHECK(afterS > sleptS - 0.5);             // and counted again afterwards (0 = cleared)
        if (std::fabs(afterS - kOwnerDwellS) < kDwellEdgeS) {
            std::printf("    %s: the new rest is within %.2f s of the dwell; the tip is not asserted\n", tag,
                        kDwellEdgeS);
        } else {
            CHECK((st.x0 > 0.5f) == (afterS >= kOwnerDwellS));
            if (afterS >= kOwnerDwellS) {
                std::printf("    %s: could not exercise the tip-down side on this machine: the new rest "
                            "%.2f s reached the %.0f s dwell\n",
                            tag, afterS, kOwnerDwellS);
            }
        }
    };

    // Moving takes the tip away, and the next one needs a full four seconds from the new place.
    {
        const Census c = once({"tip-move", false, true, "",
                               at(20, wx, wy + 40.0f) + sleepMs(22, 2500) + sleepMs(24, 2500) +
                                   at(40, wx + 200.0f, wy + 40.0f) + sleepMs(42, 1500),
                               60});
        checkRestarted("tip-move", c, "waterfall", 1.5);
    }

    // A press takes it away too - on the spectrum as on the waterfall. A click on either tunes the
    // receiver; that does not matter here, only the tip does. (On the waterfall a press also starts
    // a pan, which takes the pointer off the panel for the tip's purposes by itself; on the spectrum
    // a plain click on the empty well starts nothing, so the held button is the only thing there
    // that puts the tip away.)
    {
        const Census c = once({"tip-press-sp", false, true, "",
                               at(20, sx, sy) + sleepMs(22, 2500) + sleepMs(24, 2500) +
                                   click(40, sx, sy) + sleepMs(46, 1000),
                               70});
        checkRestarted("tip-press-sp", c, "spectrum", 1.0);
    }
    {
        const Census c = once({"tip-press", false, true, "",
                               at(20, wx, wy + 40.0f) + sleepMs(22, 2500) + sleepMs(24, 2500) +
                                   click(40, wx, wy + 40.0f) + sleepMs(46, 1000),
                               70});
        checkRestarted("tip-press", c, "waterfall", 1.0);
    }

    // --- a patch Spectrum part: the same menu, and in Manual the same window ----------------------------------
    //
    // REAL TIME FIRST, as test_spectrum_trace_app: the patch's radio opens on a worker, and until it
    // has delivered a spectrum the part draws "No spectrum yet" and nothing a click can land on.
    const std::string radioOpens = sleepMs(25, 2000);
    const Census pl = once({"patch-learn", true, false, "", radioOpens, 90});
    CHECK(pl.ok && pl.hasRect("trc:patch:trace"));
    if (pl.hasRect("trc:patch:trace")) {
        const Rect trace = pl.rect("trc:patch:trace");
        const Rect pr = pl.rect("wfm:patch:range");
        std::printf("    patch part, Automatic: window %.0f..%.0f %s\n", pr.x0, pr.y0,
                    pr.x1 > 0.5f ? "MANUAL" : "(fitted to the signal)");
        CHECK(pl.hasRect("wfm:patch:range"));
        CHECK(pr.x1 < 0.5f);
        CHECK(pr.y0 - pr.x0 >= 30.0f - 0.01f);          // autoRange never gives less than 30 dB
        const float px = trace.x0 + 0.3f * (trace.x1 - trace.x0);
        const float py = trace.y0 + 0.4f * (trace.y1 - trace.y0);
        const Census pm = once({"patch-menu", true, false, "", radioOpens + rightClick(50, px, py), 70});
        CHECK(pm.ok && pm.has("trc:menu") && menuComplete(pm));

        // Manual on the receiver's window is Manual on the part.
        const Census pman = once({"patch-manual", true, false, manualCfg, radioOpens, 90});
        const Rect pm2 = pman.rect("wfm:patch:range");
        std::printf("    patch part, Manual -60 / -20: window %.0f..%.0f %s\n", pm2.x0, pm2.y0,
                    pm2.x1 > 0.5f ? "MANUAL" : "(fitted to the signal)");
        CHECK(pman.ok && pman.hasRect("wfm:patch:range"));
        CHECK(pm2.x0 == -60.0f && pm2.y0 == -20.0f && pm2.x1 > 0.5f);

        // The tip on the part's trace, after four seconds of rest.
        const Census ptip = once({"patch-tip", true, false, "",
                                  radioOpens + at(50, px, py) + sleepMs(52, 2500) + sleepMs(54, 2500), 70});
        std::printf("    patch trace rested on for 5 s: tip %s\n",
                    ptip.has("wfm:tip:spectrum") ? "SHOWN" : "not shown");
        CHECK(ptip.ok && ptip.has("wfm:tip:spectrum"));
    }

    const int rc = testSummary("test_display_menu_app");
    if (rc == 0) {
        fs::remove_all(g_dir, ec);
    } else {
        std::printf("  kept for inspection: %s\n", g_dir.string().c_str());
    }
    return rc;
}
