/*
 * NOTHING IS DRAWN WHILE THE SESSION'S DISPLAY IS OFF (2026-10-27 tester report), END TO END.
 *
 * Runs cascade itself, isolated like every test that starts the application, and hands its
 * window procedure the notification Windows sends when the display goes off and comes back
 * (FOXSDR_TEST_DISPLAY_OFF, which sends a real WM_POWERBROADCAST / PBT_POWERSETTINGCHANGE carrying
 * GUID_SESSION_DISPLAY_STATUS through SendMessage), then reads the log and the self-capture:
 *
 *   watched   the window registered for the notification ("display: watching this session's
 *             display state") - the real RegisterPowerSettingNotification, not the seam;
 *   off/on    the display reported off at frame 60 and on at frame 140: one "display: off" line,
 *             one "display: on - drawing resumed" line saying 80 frames were not drawn - frames
 *             60 to 139, each one a swap that did not happen - and the run still renders all 200;
 *   nothing   a screenshot asked for at frame 100, while the display is off, is NOT written: there
 *             is nothing drawn to read back. One at frame 180, after it came back, is;
 *   stays off an "on" frame of none leaves the display off to the end: no "on" line, and the run
 *             still finishes (the loop is paced by its own wait, not a vertical sync);
 *   control   no notification at all: no "display:" off or on line, every screenshot written.
 *
 * THE FIELD REPORT. A tester's laptop with Intel UHD graphics, FoxSDR left on ADS-B: ten minutes
 * after the start (the screen's own time-out) a 500 ms frame began every minute or two, and 29 to
 * 91 minutes in the driver refused a second window and the main window stopped showing anything
 * new - black after a minimise and restore - while the program kept running. His System log held
 * no display-driver reset. The application had kept drawing to a display that was off.
 *
 * Windows only: everywhere else there is no such notification and the test says so.
 *
 * NO NETWORK: every telemetry address is a discard port and the update check is off.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "test_check.hpp"

namespace fs = std::filesystem;

#if defined(_WIN32)
namespace {

void setEnv(const char* name, const std::string& value) {
    ::SetEnvironmentVariableA(name, value.empty() ? nullptr : value.c_str());
    _putenv_s(name, value.c_str());
}

std::string run(const std::string& cmd) {
    std::string out;
    FILE* p = _popen(("\"" + cmd + "\"").c_str(), "r");
    char buf[512];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
    if (p != nullptr) { _pclose(p); }
    return out;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

int countOf(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) {
        ++n;
    }
    return n;
}

struct Result {
    bool ok = false;
    std::string out;
    std::string log;
    fs::path shots;
};

fs::path g_dir;

Result once(const std::string& tag, const std::string& displayOff) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path diag = g_dir / (tag + "-diag");
    r.shots = g_dir / (tag + "-shots");
    std::error_code ec;
    fs::remove_all(diag, ec);
    fs::remove_all(r.shots, ec);
    fs::create_directories(diag, ec);
    fs::create_directories(r.shots, ec);
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": \"siggen\", "
             "\"uiTheme\": \"today\", \"interfaceScale\": \"100\", \"mainView\": \"receiver\" }\n";
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    setEnv("FOXSDR_SHOT_DIR", r.shots.string());
    setEnv("FOXSDR_SHOT_AT_FRAME", "30,100,180");
    setEnv("FOXSDR_TEST_DISPLAY_OFF", displayOff);
    const std::string exe = std::string(CASCADE_APP_BINDIR) + "/cascade.exe";
    r.out = run("\"" + exe + "\" --frames 200 2>&1");
    if (r.out.find("rendered 200 frames") == std::string::npos) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), r.out.c_str());
        return r;
    }
    r.log = readFile(diag / "logs" / "foxsdr.log");
    r.ok = !r.log.empty();
    if (!r.ok) { std::printf("  %s: no log was written\n", tag.c_str()); }
    return r;
}

bool shot(const Result& r, int frame) {
    return fs::exists(r.shots / ("shot-" + std::to_string(frame) + ".bmp"));
}

void printDisplayLines(const Result& r, const char* tag) {
    std::size_t at = 0;
    while ((at = r.log.find("display: ", at)) != std::string::npos) {
        const std::size_t end = r.log.find('\n', at);
        std::printf("    %s: %s\n", tag, r.log.substr(at, end - at).c_str());
        at = end;
    }
}

}  // namespace
#endif

int main() {
    std::printf("test_display_off_app\n");
#if !defined(_WIN32)
    std::printf("    Windows only: no session display notification on this platform\n");
    return testSummary("test_display_off_app");
#else
    g_dir = fs::temp_directory_path() / ("cascade-displayoff-" + std::to_string(::GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir / "appdata", ec);
    fs::create_directories(g_dir / "local", ec);
    fs::create_directories(g_dir / "home", ec);
    setEnv("APPDATA", (g_dir / "appdata").string());
    setEnv("LOCALAPPDATA", (g_dir / "local").string());
    setEnv("USERPROFILE", (g_dir / "home").string());
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL",
                            "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL",
                            "FOXSDR_REGIONAL_CATALOGUE_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("FOXSDR_SINGLE_VIEWPORT", "1");
    setEnv("FOXSDR_WINDOW_SIZE", "1280x720");
    for (const char* v : {"FOXSDR_OPEN_MAP", "FOXSDR_OPEN_PLUGIN_STORE", "FOXSDR_OPEN_FITTED_MODULES",
                          "FOXSDR_INPUT_SCRIPT", "FOXSDR_UI_CENSUS", "FOXSDR_TEST_WINDOW_FAIL",
                          "CASCADE_DECODE_TEST", "CASCADE_PLUGIN_TEST"}) {
        setEnv(v, "");
    }

    // --- OFF AT 60, ON AT 140 -------------------------------------------------------------------
    {
        const Result r = once("offon", "60,140");
        CHECK(r.ok);
        if (r.ok) {
            printDisplayLines(r, "off/on");
            CHECK(r.log.find("display: watching this session's display state") != std::string::npos);
            CHECK(countOf(r.log, "display: off - Windows says this session's display is off") == 1);
            CHECK(countOf(r.log, "display: on - drawing resumed") == 1);
            CHECK(r.log.find("; 80 frames not drawn") != std::string::npos);
            CHECK(shot(r, 30));
            CHECK(!shot(r, 100));  // nothing drawn while off, so nothing read back
            CHECK(shot(r, 180));
        }
    }

    // --- OFF AT 60, NEVER BACK ------------------------------------------------------------------
    {
        const Result r = once("stays", "60");
        CHECK(r.ok);
        if (r.ok) {
            printDisplayLines(r, "stays off");
            CHECK(countOf(r.log, "display: off - Windows says this session's display is off") == 1);
            CHECK(r.log.find("display: on - drawing resumed") == std::string::npos);
            CHECK(shot(r, 30));
            CHECK(!shot(r, 100));
            CHECK(!shot(r, 180));
        }
    }

    // --- CONTROL: no notification ---------------------------------------------------------------
    {
        const Result r = once("control", "");
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.log.find("display: off") == std::string::npos);
            CHECK(r.log.find("drawing resumed") == std::string::npos);
            CHECK(shot(r, 30));
            CHECK(shot(r, 100));
            CHECK(shot(r, 180));
        }
    }

    fs::remove_all(g_dir, ec);
    return testSummary("test_display_off_app");
#endif
}
