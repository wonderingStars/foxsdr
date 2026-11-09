// drive_retune_run.cpp - A BAND SWITCH MOVES THE PICTURE AND THE MODE IN THE SLINT WINDOW, through the application (a beta tester,
// 2026-11-09: the waterfall left on 7 MHz after a switch to 3.5 MHz, the LSB/USB mode not following the band). foxsdr-slint is run
// bounded (--frames) on a scratch profile with FOXSDR_SLINT_RETUNE_PROBE (src/slint/main.cpp): the picture is zoomed ten notches
// about 7.100 MHz at tick 40, 3.620 MHz is typed into the counter at tick 60 - the counter's whole typed path - and the run's summary
// line says where the picture, the tune and the mode ended:
//
//   follow   the picture is on 3.620 MHz at the width it had (145 kHz of the 2 MHz generator span), the tune happened, the mode is LSB
//            (the 80 m convention of the UK plan; the receiver started in USB on 40 m);
//   off      with the Radio body's "Follow the band plan's mode when retuning" unticked at tick 50 the picture still follows and the
//            mode stays USB.
//
// The engine owns both rules (core/view_follow.hpp applied by the window to its own view; the band plan's mode in Engine::applyCommand), so
// this holds the Slint window's wiring: the typed path, the checkbox's command, the view the ticks draw. Registered with the GUI gate's
// label (a window is opened), never `slint`.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

fs::path g_dir;

constexpr double kCentreHz = 7100000.0;
constexpr double kRateHz = 2000000.0;
constexpr int kZoomNotches = 10;
const double kZoomedSpanHz = kRateHz / std::pow(1.3, kZoomNotches);   // 145.1 kHz

// The summary line's numbers.
struct Probe {
    bool ok = false;
    double lowHz = 0, highHz = 0, spanHz = 0, tunedHz = 0;
    unsigned mode = 0;
    int follow = -1;
    std::string savedMode;   // from the settings file the run wrote, "" when it wrote none
};

Probe once(const std::string& tag, const std::string& probe) {
    Probe p;
    const fs::path cfg = g_dir / (tag + ".json");
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": \"siggen\", \"mainView\": \"receiver\", "
             "\"bandPlanOverlay\": false, \"bandPlanSelection\": \"uk\", \"mode\": \"USB\", \"centerHz\": "
          << static_cast<long long>(kCentreHz) << ", \"sampleRateHz\": " << static_cast<long long>(kRateHz) << ", \"vfoOffsetHz\": 0 }\n";
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_SLINT_RETUNE_PROBE", probe);
    const std::string out = run("\"" FOXSDR_SLINT_EXE "\" --frames 100 2>&1");
    const std::size_t at = out.find("retune probe picture=");
    if (at == std::string::npos) {
        std::printf("  %s: no summary line in the run's output:\n%s\n", tag.c_str(), out.c_str());
        return p;
    }
    const std::string line = out.substr(at, out.find('\n', at) - at);
    std::printf("    %s: %s\n", tag.c_str(), line.c_str());
    if (std::sscanf(line.c_str(), "retune probe picture=[%lf, %lf] Hz span=%lf Hz tuned=%lf Hz mode=%u follow=%d", &p.lowHz, &p.highHz, &p.spanHz,
                    &p.tunedHz, &p.mode, &p.follow) != 6) {
        std::printf("  %s: the summary line did not parse\n", tag.c_str());
        return p;
    }
    cascade::core::AppConfig saved;
    std::string err;
    if (cascade::core::ConfigStore::load(cfg.string(), saved, err)) { p.savedMode = saved.mode; }
    p.ok = true;
    return p;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_slint_retune_run\n");
    g_dir = fs::temp_directory_path() / ("foxsdr-slint-retune-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir / "appdata", ec);
    fs::create_directories(g_dir / "local", ec);
    fs::create_directories(g_dir / "profile", ec);
    setEnv("APPDATA", (g_dir / "appdata").string());
    setEnv("LOCALAPPDATA", (g_dir / "local").string());
    setEnv("USERPROFILE", (g_dir / "profile").string());
    setEnv("HOME", (g_dir / "profile").string());
    setEnv("XDG_CONFIG_HOME", (g_dir / "appdata").string());
    setEnv("XDG_DATA_HOME", (g_dir / "local").string());
    setEnv("XDG_STATE_HOME", (g_dir / "local").string());
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL", "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }

    // --- FOLLOW: zoomed on 7.100 MHz in USB, 3.620 MHz typed ---------------------------------------------------------------------------------
    {
        const Probe p = once("follow", std::to_string(kZoomNotches) + ";3.620");
        CHECK(p.ok);
        CHECK_NEAR(p.tunedHz, 3620000.0, 1.0);                       // the tune happened
        CHECK(p.lowHz <= 3620000.0 && 3620000.0 <= p.highHz);        // the picture went with it...
        CHECK_NEAR(p.spanHz, kZoomedSpanHz, 100.0);                  // ...at the width it had
        CHECK(p.mode == 7u);                                         // FOXAPI_DEMOD_LSB: 80 m's convention
        CHECK(p.follow == 1);
        std::printf("    saved mode \"%s\"\n", p.savedMode.c_str());
        CHECK(p.savedMode.empty() || p.savedMode == "LSB");
    }
    // --- OFF: the checkbox unticked, the picture still follows, the mode stays -----------------------------------------------------------------
    {
        const Probe p = once("off", std::to_string(kZoomNotches) + ";3.620;off");
        CHECK(p.ok);
        CHECK_NEAR(p.tunedHz, 3620000.0, 1.0);
        CHECK(p.lowHz <= 3620000.0 && 3620000.0 <= p.highHz);
        CHECK_NEAR(p.spanHz, kZoomedSpanHz, 100.0);
        CHECK(p.mode == 5u);                                         // FOXAPI_DEMOD_USB, as it started
        CHECK(p.follow == 0);
        CHECK(p.savedMode.empty() || p.savedMode == "USB");
    }

    const int rc = testSummary("test_slint_retune_run");
    if (rc == 0) {
        fs::remove_all(g_dir, ec);
    } else {
        std::printf("  kept for inspection: %s\n", g_dir.string().c_str());
    }
    return rc;
}
