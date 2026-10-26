/*
 * THE MODE FOLLOWS A TYPED BAND CHANGE, THROUGH THE APPLICATION (0.99.83; stub campaign B11). A field report (a09fe097, 0.99.78): type 3.620 MHz and be in LSB,
 * because 80 m phone is LSB and retyping the mode after every band change is toil the band plan already knows how to spare. The rule is pure and pinned in
 * tests/test_band_mode.cpp (core::conventionalModeForBand and core::modeFollowsTypedBandChange); what the master's 0.99.83 merge left out of the port was the
 * CALLER - the counter's typed entry - and no test of master's reaches it (the ledger said so: "none own - NEW assertion"). This runs cascade on the signal
 * generator, clicks the first figure of the frequency counter to open the typed editor (the census names each tube, cnt:tube:<n>), types a frequency and
 * presses Enter with the application's own input script, and reads back the mode the run saved:
 *
 *   crossing  from the FM broadcast band (WFM's), in USB, typing 3.620 MHz lands in LSB - the conventional mode of the band typed into;
 *   same band typing within the band the receiver is already in leaves the mode alone: FT8 on 7.074 USB survives a retype to 7.040;
 *   raw       a receiver parked in RAW is never moved (a diagnostics hold);
 *   retune    in every case the receiver IS tuned to what was typed (the tune is not lost to the mode change).
 *
 * The band plan is the UK's (bandPlanSelection in the config): the default WORLD plan leaves out 80 m and the FM edges, because the three ITU regions disagree about
 * them, and a typed band change needs a plan that has both bands.
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
    cascade::core::AppConfig saved;
    bool hasRect(const std::string& k) const { return rects.count(k) == 1; }
    Rect rect(const std::string& k) const {
        const auto it = rects.find(k);
        return it == rects.end() ? Rect{} : it->second;
    }
};

fs::path g_dir;

constexpr double kRateHz = 2000000.0;

struct Run {
    std::string tag;
    const char* mode = "USB";
    double centreHz = 100000000.0;
    double offsetHz = 0.0;
    std::string script;
    int frames = 60;
};

Census once(const Run& r) {
    Census c;
    const fs::path cfg = g_dir / (r.tag + ".json");
    const fs::path census = g_dir / (r.tag + ".census");
    const fs::path scriptFile = g_dir / (r.tag + ".script");
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", "
             "\"mainView\": \"receiver\", \"bandPlanOverlay\": false, \"bandPlanSelection\": \"uk\", \"mode\": \""
          << r.mode << "\", \"centerHz\": " << static_cast<long long>(r.centreHz) << ", \"sampleRateHz\": " << static_cast<long long>(kRateHz)
          << ", \"vfoOffsetHz\": " << static_cast<long long>(r.offsetHz) << " }\n";
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
    setEnv("CASCADE_DECODE_TEST", "1");
    const std::string out = run("\"" + exePath() + "\" --frames " + std::to_string(r.frames) + " 2>&1");
    if (out.find("rendered " + std::to_string(r.frames) + " frames") == std::string::npos || out.find("ui census written") == std::string::npos) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", r.tag.c_str(), out.c_str());
        return c;
    }
    std::ifstream in(census);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
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

// Click a figure of the counter (it opens the typed editor, seeded and all selected), type, Enter.
std::string typeFrequency(const Rect& tube, const char* text) {
    std::ostringstream s;
    s << "20 screen " << tube.cx() << ' ' << tube.cy() << "\n22 down\n24 up\n30 text " << text << "\n34 key enter\n";
    return s.str();
}

}  // namespace

int main() {
    std::printf("test_typed_band_mode_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-typedband-" + std::to_string(pid()));
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

    // --- the counter's figures, found; and a run that types nothing leaves the mode as configured ---------------------------------------------------------
    const Census learn = once({"learn", "USB"});
    CHECK(learn.ok && learn.hasRect("cnt:tube:0"));
    if (!learn.ok || !learn.hasRect("cnt:tube:0")) { return testSummary("test_typed_band_mode_app"); }
    CHECK(learn.saved.mode == "USB");
    const Rect tube = learn.rect("cnt:tube:0");

    // --- crossing: 100 MHz (the FM broadcast band) to 80 m amateur -----------------------------------------------------------------------------------------
    {
        const Census c = once({"cross", "USB", 100000000.0, 0.0, typeFrequency(tube, "3.620")});
        std::printf("    typed 3.620 MHz from 100 MHz in USB: mode \"%s\", tuned %.0f Hz\n", c.saved.mode.c_str(), c.saved.centerHz + c.saved.vfoOffsetHz);
        CHECK(c.ok);
        CHECK_NEAR(c.saved.centerHz + c.saved.vfoOffsetHz, 3620000.0, 1.0);   // the tune happened
        CHECK(c.saved.mode == "LSB");                                           // and the band's mode followed it
    }
    // --- the other way: into the broadcast FM band, WFM --------------------------------------------------------------------------------------------------
    {
        const Census c = once({"cross-fm", "USB", 7000000.0, 74000.0, typeFrequency(tube, "99.5")});
        std::printf("    typed 99.5 MHz from 7.074 MHz in USB: mode \"%s\", tuned %.0f Hz\n", c.saved.mode.c_str(), c.saved.centerHz + c.saved.vfoOffsetHz);
        CHECK(c.ok);
        CHECK_NEAR(c.saved.centerHz + c.saved.vfoOffsetHz, 99500000.0, 1.0);
        CHECK(c.saved.mode == "WFM");
    }
    // --- the same band: the mode is the person's (FT8 on 7.074 USB, retyped to 7.040, both inside the 40 m band) -----------------------------------------------
    {
        const Census c = once({"same", "USB", 7000000.0, 74000.0, typeFrequency(tube, "7.040")});
        std::printf("    typed 7.040 MHz from 7.074 MHz in USB: mode \"%s\", tuned %.0f Hz\n", c.saved.mode.c_str(), c.saved.centerHz + c.saved.vfoOffsetHz);
        CHECK(c.ok);
        CHECK_NEAR(c.saved.centerHz + c.saved.vfoOffsetHz, 7040000.0, 1.0);
        CHECK(c.saved.mode == "USB");
    }
    // --- RAW is a diagnostics hold ---------------------------------------------------------------------------------------------------------------------------
    {
        const Census c = once({"raw", "RAW", 100000000.0, 0.0, typeFrequency(tube, "3.620")});
        std::printf("    typed 3.620 MHz from 100 MHz in RAW: mode \"%s\"\n", c.saved.mode.c_str());
        CHECK(c.ok);
        CHECK_NEAR(c.saved.centerHz + c.saved.vfoOffsetHz, 3620000.0, 1.0);
        CHECK(c.saved.mode == "RAW");
    }

    const int rc = testSummary("test_typed_band_mode_app");
    if (rc == 0) {
        fs::remove_all(g_dir, ec);
    } else {
        std::printf("  kept for inspection: %s\n", g_dir.string().c_str());
    }
    return rc;
}
