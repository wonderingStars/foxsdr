/*
 * THE DISPLAY RANGE, AUTOMATIC WITH A PAIR REMEMBERED, ACROSS A RESTART (stub campaign B11). tests/test_display_menu_app.cpp (master's, 0.99.76) holds a saved Manual coming
 * back, a 0.99.75 file with a range of its own opening in Manual, and the menu's Automatic choice remembering the pair - but never the launch that follows it: a file that
 * says "waterfallRangeMode": "auto" next to a dbMin / dbMax that are NOT the standard window (the pair Manual returns to, kept while Automatic is in force). In this port the
 * ENGINE holds the window in force and restores the pair into it with the rest of its config, so Automatic has to put the standard window over it on the first frame; a window
 * that did not would draw the pair, say MANUAL on the strength key and, a moment later, adopt it as the person's choice (the follow of somebody else's range). This runs
 * cascade on the signal generator on such a file and reads the census and the config the run saved:
 *
 *   in force   the spectrum, the waterfall's strength key and the census all say -110 / 0 dB, and the key does NOT say MANUAL;
 *   kept       the config the run saved still says "auto" and still holds the pair (-60 / -20): Automatic does not forget it;
 *   and after  a second launch on the file the first one saved is the same (the pair survives a launch it was not in force for).
 *
 * Isolated like every test that starts the application: a private APPDATA, telemetry points at a black hole.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
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
    std::map<std::string, Rect> rects;
    std::map<std::string, bool> notes;
    cascade::core::AppConfig saved;
    bool hasRect(const std::string& k) const { return rects.count(k) == 1; }
    bool hasNote(const std::string& k) const { return notes.count(k) == 1; }
    Rect rect(const std::string& k) const {
        const auto it = rects.find(k);
        return it == rects.end() ? Rect{} : it->second;
    }
};

struct Run {
    std::string tag;
    std::string cfgPath;      // when set, the config this run reads is a copy of that file
    std::string extra;        // more JSON members
};

fs::path g_dir;

Census once(const Run& r) {
    Census c;
    const fs::path cfg = g_dir / (r.tag + ".json");
    const fs::path census = g_dir / (r.tag + ".census");
    if (!r.cfgPath.empty()) {
        std::error_code ec;
        fs::copy_file(r.cfgPath, cfg, fs::copy_options::overwrite_existing, ec);
    } else {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", "
             "\"mainView\": \"receiver\", \"bandPlanOverlay\": false" << r.extra << " }\n";
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_WINDOW_SIZE", "1600x900");
    setEnv("FOXSDR_INPUT_SCRIPT", "");
    setEnv("FOXSDR_SCRIPT_TRACE", "");
    setEnv("CASCADE_DECODE_TEST", "1");
    const int frames = 60;
    const std::string out = run("\"" + exePath() + "\" --frames " + std::to_string(frames) + " 2>&1");
    if (out.find("rendered " + std::to_string(frames) + " frames") == std::string::npos || out.find("ui census written") == std::string::npos) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", r.tag.c_str(), out.c_str());
        return c;
    }
    std::ifstream in(census);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.rfind("item ", 0) == 0) { c.notes[line.substr(5)] = true; }
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

}  // namespace

int main() {
    std::printf("test_display_restart_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-dispRestart-" + std::to_string(pid()));
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

    const auto check = [](const char* what, const Census& c) {
        const Rect key = c.rect("wf:key:state");
        const Rect spec = c.rect("rx:spec:range");
        std::printf("    %s: key %.0f / %.0f %s, spectrum %.0f / %.0f, saved \"%s\" %.0f / %.0f\n", what, key.x0, key.y0, key.x1 > 0.5f ? "MANUAL" : "(no MANUAL)", spec.x0, spec.y0,
                    c.saved.waterfallRangeMode.c_str(), static_cast<double>(c.saved.dbMin), static_cast<double>(c.saved.dbMax));
        CHECK(c.ok && c.hasRect("wf:key:state") && c.hasRect("rx:spec:range"));
        CHECK(key.x0 == -110.0f && key.y0 == 0.0f);          // the standard window is in force...
        CHECK(!(key.x1 > 0.5f));                              // ...and the key does not say MANUAL
        CHECK(spec.x0 == -110.0f && spec.y0 == 0.0f);
        CHECK(!c.hasNote("wf:key:manual"));
        CHECK(c.saved.waterfallRangeMode == "auto");          // Automatic is still the choice...
        CHECK(c.saved.dbMin == -60.0f && c.saved.dbMax == -20.0f);   // ...and the pair Manual returns to is still there
    };

    const Census first = once({"first", "", ", \"waterfallRangeMode\": \"auto\", \"dbMin\": -60, \"dbMax\": -20"});
    check("Automatic over a remembered -60 / -20", first);
    const Census second = once({"second", (g_dir / "first.json").string(), ""});
    check("the second launch, on the file the first saved", second);

    const int rc = testSummary("test_display_restart_app");
    if (rc == 0) {
        fs::remove_all(g_dir, ec);
    } else {
        std::printf("  kept for inspection: %s\n", g_dir.string().c_str());
    }
    return rc;
}
