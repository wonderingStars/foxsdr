/*
 * A MONITOR THAT CANNOT BE QUERIED FOR A WHILE IS ONE DISPLAY CHANGE (2026-10-28 tester log), END
 * TO END.
 *
 * Runs cascade itself, isolated like every test that starts the application, and hands its GLFW
 * error callback the error the Win32 backend raises while a monitor is detached - 65544, "Win32:
 * Failed to query display settings" - on every frame of a range (FOXSDR_TEST_DISPLAY_QUERY_FAIL),
 * exactly as the Dear ImGui backend's per-frame monitor poll raises it in the field, then reads
 * the log:
 *
 *   once      one "display: GLFW could not query display settings" line for the whole range, not
 *             one a frame; one "display changed" line more than the control run, not forty-one;
 *   settled   one "display: display settings answered again" line, naming the 41 failed queries,
 *             once the queries have answered for the quiet period;
 *   cap kept  none of the forty-one is written as a "window: GLFW error 65544" line, and the
 *             "further GLFW errors are not logged" cap is not reached - so a GLFW error that
 *             matters later in the session is still written;
 *   control   no range: none of those lines.
 *
 * THE FIELD LOG. A tester's desktop (Intel UHD 630) whose monitor sleeps: the error on every frame
 * for the three minutes the capped log still held, two lines a frame, the hang watchdog's grace
 * restarted every frame - off for the whole episode - and the session's fifty GLFW lines spent in
 * three seconds, so the driver's later refusal of a second context (65543) was never logged.
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
#else
#include <unistd.h>
#endif

#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)
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
const char* kExeName = "/cascade.exe";
#else
void setEnv(const char* name, const std::string& value) {
    if (value.empty()) {
        ::unsetenv(name);
    } else {
        ::setenv(name, value.c_str(), 1);
    }
}
std::string run(const std::string& cmd) {
    std::string out;
    FILE* p = ::popen(cmd.c_str(), "r");
    char buf[512];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
    if (p != nullptr) { ::pclose(p); }
    return out;
}
const char* kExeName = "/cascade";
#endif

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
};

fs::path g_dir;

Result once(const std::string& tag, const std::string& failRange, const std::string& glfwErrorAt = std::string()) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path diag = g_dir / (tag + "-diag");
    std::error_code ec;
    fs::remove_all(diag, ec);
    fs::create_directories(diag, ec);
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": \"siggen\", "
             "\"uiTheme\": \"today\", \"interfaceScale\": \"100\", \"mainView\": \"receiver\" }\n";
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    setEnv("FOXSDR_TEST_DISPLAY_QUERY_FAIL", failRange);
    setEnv("FOXSDR_TEST_GLFW_ERROR_AT", glfwErrorAt);
    const std::string exe = std::string(CASCADE_APP_BINDIR) + kExeName;
    r.out = run("\"" + exe + "\" --frames 400 2>&1");
    if (r.out.find("rendered 400 frames") == std::string::npos) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), r.out.c_str());
        return r;
    }
    r.log = readFile(diag / "logs" / "foxsdr.log");
    r.ok = !r.log.empty();
    if (!r.ok) { std::printf("  %s: no log was written\n", tag.c_str()); }
    return r;
}

void printDisplayLines(const Result& r, const char* tag) {
    std::size_t at = 0;
    while ((at = r.log.find("display", at)) != std::string::npos) {
        const std::size_t start = r.log.rfind('\n', at);
        const std::size_t end = r.log.find('\n', at);
        const std::size_t from = start == std::string::npos ? 0 : start + 1;
        std::printf("    %s: %s\n", tag, r.log.substr(from, end - from).c_str());
        at = end == std::string::npos ? r.log.size() : end;
    }
}

}  // namespace

int main() {
    std::printf("test_display_query_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-displayquery-" + std::to_string(static_cast<long long>(
#if defined(_WIN32)
                                             ::GetCurrentProcessId()
#else
                                             ::getpid()
#endif
                                             )));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir / "appdata", ec);
    fs::create_directories(g_dir / "local", ec);
    fs::create_directories(g_dir / "home", ec);
    setEnv("APPDATA", (g_dir / "appdata").string());
    setEnv("LOCALAPPDATA", (g_dir / "local").string());
    setEnv("USERPROFILE", (g_dir / "home").string());
    setEnv("HOME", (g_dir / "home").string());
    setEnv("XDG_CONFIG_HOME", (g_dir / "home" / ".config").string());
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL",
                            "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL",
                            "FOXSDR_REGIONAL_CATALOGUE_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("FOXSDR_SINGLE_VIEWPORT", "1");
    setEnv("FOXSDR_WINDOW_SIZE", "1280x720");
    for (const char* v : {"FOXSDR_OPEN_MAP", "FOXSDR_OPEN_PLUGIN_STORE", "FOXSDR_OPEN_FITTED_MODULES",
                          "FOXSDR_INPUT_SCRIPT", "FOXSDR_UI_CENSUS", "FOXSDR_TEST_WINDOW_FAIL",
                          "FOXSDR_TEST_DISPLAY_OFF", "FOXSDR_SHOT_AT_FRAME", "FOXSDR_FRAME_SITUATION",
                          "CASCADE_DECODE_TEST", "CASCADE_PLUGIN_TEST"}) {
        setEnv(v, "");
    }

    const char* kOnce = "display: GLFW could not query display settings";
    const char* kSettled = "display: display settings answered again";
    const char* kChanged = "display changed - presentation stalls for the next";
    const char* kRaw = "window: GLFW error 65544";
    const char* kCap = "further GLFW errors are not logged";

    // --- CONTROL FIRST: its "display changed" count is the baseline ---------------------------
    int controlChanges = -1;
    {
        const Result r = once("control", "");
        CHECK(r.ok);
        if (r.ok) {
            controlChanges = countOf(r.log, kChanged);
            std::printf("    control: %d display-changed line(s)\n", controlChanges);
            CHECK(countOf(r.log, kOnce) == 0);
            CHECK(countOf(r.log, kSettled) == 0);
            CHECK(countOf(r.log, kRaw) == 0);
        }
    }

    // --- THE ERROR ON EVERY FRAME FROM 20 TO 60 ---------------------------------------------------
    {
        const Result r = once("flood", "20,60");
        CHECK(r.ok);
        if (r.ok) {
            printDisplayLines(r, "flood");
            CHECK(countOf(r.log, kOnce) == 1);
            CHECK(countOf(r.log, kSettled) == 1);
            CHECK(r.log.find("41 queries failed") != std::string::npos);
            if (controlChanges >= 0) { CHECK(countOf(r.log, kChanged) == controlChanges + 1); }
            CHECK(countOf(r.log, kRaw) == 0);
            CHECK(r.log.find(kCap) == std::string::npos);
        }
    }

    // --- THE LINE THAT MATTERS LATER IS STILL WRITTEN (the 0.99.100 merge) -----------------------------------
    // The field log's other half: after the flood, at frame 120, the driver refuses a second shared context (65543) with its Win32 code. That
    // line is what an issue report of the refused window is filed to carry (core/issue_report.hpp), so it must be in the log - the error
    // callback writes the first fifty GLFW errors of a session there - and the flood must not have spent them. (On this branch the callback's
    // log block had been lost in the 0.99.84 merge, and with it any proof that this held; the control run proves the block is back.)
    const char* kRefusal = "window: GLFW error 65543: WGL: Failed to create OpenGL context (Win32 error 0x00000008)";
    {
        const Result r = once("refusal-control", "", "120");
        CHECK(r.ok);
        if (r.ok) { CHECK(countOf(r.log, kRefusal) == 1); }
    }
    {
        const Result r = once("refusal-after-flood", "20,60", "120");
        CHECK(r.ok);
        if (r.ok) {
            CHECK(countOf(r.log, kOnce) == 1);
            CHECK(countOf(r.log, kRefusal) == 1);
            CHECK(countOf(r.log, kRaw) == 0);
            CHECK(r.log.find(kCap) == std::string::npos);
        }
    }

    fs::remove_all(g_dir, ec);
    return testSummary("test_display_query_app");
}
