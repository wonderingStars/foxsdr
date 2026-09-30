/*
 * THE SPECTRUM'S TRACE MODES, THROUGH THE APPLICATION (a user's request,
 * 2026-09-30: "if you right click on the spectrum display ... give the user
 * the option to select different display modes ... normal, peak, average ...
 * have this trace drawn in bold with the normal trace shown as before ...
 * reset the trace ... set the length of the average, maybe in
 * milliseconds"). The hold itself is pinned in tests/test_trace_hold.cpp;
 * this runs cascade, right-clicks with the application's own input script and
 * reads back the UI census and the config the run saved:
 *
 *   menu     a right-click on the receiver's spectrum opens the menu - Normal,
 *            Peak hold, Average, Reset trace, Average length - and changes
 *            nothing by itself;
 *   peak     choosing Peak hold is saved in the config and labelled on the
 *            spectrum;
 *   length   a preset under Average length is saved in the config;
 *   drawn    with the receiver running (CASCADE_DECODE_TEST starts it in a
 *            bounded run) the held trace is drawn over the receiver's
 *            spectrum in both modes, and over a patch Spectrum part's, whose
 *            trace opens the same menu on a right-click.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
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
};

fs::path g_dir;

struct Run {
    std::string tag;
    bool patch = false;
    bool receiving = false;       // CASCADE_DECODE_TEST: the receiver runs
    std::string extraConfig;      // more JSON members, each ", \"k\": v"
    std::string script;
    int frames = 45;
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
    const std::string out =
        run("\"" + exePath() + "\" --frames " + std::to_string(r.frames) + " 2>&1");
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

std::string hover(int f, float x, float y) {
    std::ostringstream s;
    s << f << " screen " << x << ' ' << y << '\n';
    return s.str();
}

}  // namespace

int main() {
    std::printf("test_spectrum_trace_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-trace-" + std::to_string(pid()));
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

    // --- the receiver's spectrum, found ----------------------------------------
    const Census learn = once({"learn"});
    CHECK(learn.ok && learn.hasRect("trc:spec:receiver"));
    if (!learn.hasRect("trc:spec:receiver")) { return testSummary("test_spectrum_trace_app"); }
    const Rect sp = learn.rects.at("trc:spec:receiver");
    // Left of the tuned band (the VFO sits right of centre on the generator),
    // high enough that the menu opens below and right of the pointer.
    const float sx = sp.x0 + 0.2f * (sp.x1 - sp.x0);
    const float sy = sp.y0 + 0.3f * (sp.y1 - sp.y0);
    std::printf("    spectrum %.0f,%.0f..%.0f,%.0f; right-clicking %.0f,%.0f\n", sp.x0, sp.y0, sp.x1,
                sp.y1, sx, sy);
    CHECK(learn.saved.spectrumTraceMode == "normal");
    CHECK(!learn.has("trc:label:normal"));

    // --- the menu, and nothing changed by opening it -----------------------------
    const Census menu = once({"menu", false, false, "", rightClick(20, sx, sy)});
    CHECK(menu.ok && menu.has("trc:menu"));
    for (const char* item : {"trc:menu:normal", "trc:menu:peak", "trc:menu:average",
                             "trc:menu:reset", "trc:menu:length"}) {
        CHECK(menu.hasRect(item));
    }
    CHECK(menu.saved.spectrumTraceMode == "normal");
    if (!menu.hasRect("trc:menu:peak") || !menu.hasRect("trc:menu:length")) {
        return testSummary("test_spectrum_trace_app");
    }
    const Rect peak = menu.rects.at("trc:menu:peak");
    const Rect length = menu.rects.at("trc:menu:length");

    // --- Peak hold: saved, and labelled -----------------------------------------
    const Census chosen =
        once({"peak", false, false, "", rightClick(20, sx, sy) + click(30, peak.cx(), peak.cy())});
    CHECK(chosen.ok);
    std::printf("    after choosing Peak hold the config says \"%s\"\n",
                chosen.saved.spectrumTraceMode.c_str());
    CHECK(chosen.saved.spectrumTraceMode == "peak");
    CHECK(chosen.has("trc:label:peak"));

    // --- Average length: a preset from the submenu is saved ---------------------
    const Census sub = once({"length-learn", false, false, ", \"spectrumTraceMode\": \"average\"",
                             rightClick(20, sx, sy) + hover(30, length.cx(), length.cy()), 50});
    CHECK(sub.ok && sub.hasRect("trc:menu:len:250"));
    if (sub.hasRect("trc:menu:len:250")) {
        const Rect p250 = sub.rects.at("trc:menu:len:250");
        const Census len = once({"length", false, false, ", \"spectrumTraceMode\": \"average\"",
                                 rightClick(20, sx, sy) + hover(30, length.cx(), length.cy()) +
                                     click(40, p250.cx(), p250.cy()),
                                 55});
        CHECK(len.ok);
        std::printf("    after choosing 250 ms the config says %d ms, mode \"%s\"\n",
                    len.saved.spectrumAverageMs, len.saved.spectrumTraceMode.c_str());
        CHECK(len.saved.spectrumAverageMs == 250);
        CHECK(len.saved.spectrumTraceMode == "average");  // the length does not change the mode
        CHECK(len.has("trc:label:average"));
    }

    // --- drawn: the held trace over the running receiver's spectrum -------------
    for (const char* mode : {"peak", "average"}) {
        const Census live = once({std::string("live-") + mode, false, true,
                                  std::string(", \"spectrumTraceMode\": \"") + mode + "\"", "", 60});
        CHECK(live.ok);
        std::printf("    receiving, %s: overlay %s\n", mode,
                    live.has("trc:overlay:receiver") ? "drawn" : "NOT drawn");
        CHECK(live.has("trc:overlay:receiver"));
        CHECK(live.has(std::string("trc:label:") + mode));
    }
    // ...and in Normal, none.
    const Census plain = once({"live-normal", false, true, "", "", 60});
    CHECK(plain.ok && !plain.has("trc:overlay:receiver"));

    // --- a patch Spectrum part: drawn too, and its trace opens the menu --------
    const Census pl = once({"patch-learn", true, false, ", \"spectrumTraceMode\": \"peak\"", "", 90});
    CHECK(pl.ok);
    CHECK(pl.has("trc:overlay:patch"));
    CHECK(pl.has("trc:label:peak"));
    CHECK(pl.hasRect("trc:patch:trace"));
    if (pl.hasRect("trc:patch:trace")) {
        const Rect tr = pl.rects.at("trc:patch:trace");
        const float px = tr.x0 + 0.3f * (tr.x1 - tr.x0);
        const float py = tr.y0 + 0.4f * (tr.y1 - tr.y0);
        const Census pm = once({"patch-menu", true, false, ", \"spectrumTraceMode\": \"peak\"",
                                rightClick(50, px, py), 70});
        CHECK(pm.ok && pm.has("trc:menu"));
    }

    const int rc = testSummary("test_spectrum_trace_app");
    if (rc == 0) {
        fs::remove_all(g_dir, ec);
    } else {
        std::printf("  kept for inspection: %s\n", g_dir.string().c_str());
    }
    return rc;
}
