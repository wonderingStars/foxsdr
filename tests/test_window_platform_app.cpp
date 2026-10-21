// THE WINDOW'S START-UP, THROUGH THE REAL APPLICATION (0.99.78).
//
// Field report A0E0967441A8E6B4 (0.99.75, Ubuntu 24.04, an AppImage): the process ended in the
// phase "creating the window" after audio had opened, and its log could not say which window
// system GLFW had chosen or what GLFW had complained of. tests/test_window_platform.cpp holds
// each line's words and width; this runs cascade and reads the diagnostics log it leaves
// (FOXSDR_DIAG_DIR), which is where the owner will read the next such report:
//
//   order      the `window:` lines are in the log in the order the steps happen - what the
//              machine offers, the platform GLFW chose, the window made, the GL strings, the
//              window shown - and every one of them is before the first frame's line;
//   env        the value of DISPLAY is not in the log, only that it was set (Windows, where a
//              made-up DISPLAY cannot change which window system is used);
//   bad        FOXSDR_WINDOW_PLATFORM=bad is logged as ignored, after the environment line
//              and before the platform is chosen, and the run still starts and renders;
//   any        =any says GLFW chooses;
//   missing    (Windows) =x11 asks for a platform this GLFW does not have: ignored, said so,
//              the run still starts on win32;
//   forced     (Linux with a window system) =<the platform the first run chose> is asked for
//              and is the one chosen.
//
// Every run points FOXSDR_DIAG_DIR, CASCADE_CONFIG_TEST, APPDATA and LOCALAPPDATA (the XDG
// directories on Linux) at a scratch tree: the user's configuration, log and plugins are never
// touched. Each run is a bounded `--frames 3` run with no radio and nothing uploaded.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int pid() {
#if defined(_WIN32)
    return static_cast<int>(::GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
#endif
}

// An EMPTY value unsets the variable.
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

fs::path g_dir;

struct Result {
    bool ran = false;      // the run rendered its frames and ended
    std::string log;       // the diagnostics log it left
    std::vector<std::string> windowLines;  // the lines of it that carry `window:`, in order
};

std::vector<std::string> linesOf(const std::string& text) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) { eol = text.size(); }
        std::string l = text.substr(pos, eol - pos);
        if (!l.empty() && l.back() == '\r') { l.pop_back(); }
        out.push_back(l);
        pos = eol + 1;
    }
    return out;
}

// One bounded run. `platform` is the value of FOXSDR_WINDOW_PLATFORM (empty: not set); `display`
// the value DISPLAY is given in the child (empty: left alone).
Result once(const std::string& tag, const std::string& platform, const std::string& display = std::string()) {
    Result r;
    const fs::path dir = g_dir / tag;
    std::error_code ec;
    fs::create_directories(dir / "appdata", ec);
    fs::create_directories(dir / "localappdata", ec);
    {
        std::ofstream f(dir / "config.json", std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": \"siggen\", "
             "\"uiTheme\": \"today\", \"interfaceScale\": \"100\" }\n";
    }
    setEnv("FOXSDR_DIAG_DIR", dir.string());
    setEnv("CASCADE_CONFIG_TEST", (dir / "config.json").string());
    setEnv("FOXSDR_CRASH_URL", "");
    setEnv("CASCADE_SENTINEL_TEST", "");
    setEnv("FOXSDR_WINDOW_PLATFORM", platform);
    if (!display.empty()) { setEnv("DISPLAY", display); }
#if defined(_WIN32)
    setEnv("APPDATA", (dir / "appdata").string());
    setEnv("LOCALAPPDATA", (dir / "localappdata").string());
#else
    setEnv("XDG_DATA_HOME", (dir / "appdata").string());
    setEnv("XDG_CONFIG_HOME", (dir / "appdata").string());
    setEnv("XDG_STATE_HOME", (dir / "localappdata").string());
#endif
    std::printf("  [run %s: FOXSDR_WINDOW_PLATFORM=%s]\n", tag.c_str(), platform.empty() ? "(unset)" : platform.c_str());
    const std::string out = run("\"" + exePath() + "\" --frames 3 2>&1");
    r.ran = contains(out, "rendered 3 frames");
    r.log = readFile(dir / "logs" / "foxsdr.log");
    if (!r.ran) {
        // What the run said, and how far its log got: the two things a failure here needs.
        std::printf("  %s: the run did not finish cleanly:\n%s\n  its log (%zu bytes):\n%s\n", tag.c_str(), out.c_str(),
                    r.log.size(), r.log.c_str());
    }
    for (const std::string& l : linesOf(r.log)) {
        if (l.find(" window: ") != std::string::npos) { r.windowLines.push_back(l); }
    }
    setEnv("FOXSDR_WINDOW_PLATFORM", "");
    return r;
}

// Where the first line containing `needle` is in the log, or npos.
std::size_t at(const std::string& log, const std::string& needle) { return log.find(needle); }

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // a timeout must still show how far it got
    const char* tmp = std::getenv("TEMP");
    if (tmp == nullptr || *tmp == '\0') { tmp = std::getenv("TMPDIR"); }
    g_dir = fs::path(tmp != nullptr && *tmp != '\0' ? tmp : ".") / ("cascade-windowplatformapp-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    CHECK(fs::exists(exePath()));

    // =======================================================================
    // 1. THE ORDER, and the value of DISPLAY that is not there
    // =======================================================================
    std::string chosen;  // the platform the first run chose
    {
#if defined(_WIN32)
        // A DISPLAY that names a host. Windows has no use for it, so it cannot change which
        // window system is used - and the log must not carry it.
        const std::string secretDisplay = "secret-build-host.example.invalid:10.0";
#else
        const std::string secretDisplay;
#endif
        const Result r = once("default", std::string(), secretDisplay);
        CHECK(r.ran);
        const std::size_t env = at(r.log, " window: supports ");
        const std::size_t init = at(r.log, " window: GLFW initialised; platform chosen: ");
        const std::size_t made = at(r.log, " window: window created");
        const std::size_t gl = at(r.log, " window: GL vendor ");
        const std::size_t shown = at(r.log, " window: window shown");
        const std::size_t frames = at(r.log, "frame loop starting");
        std::printf("  the window: lines of a default run:\n");
        for (const std::string& l : r.windowLines) { std::printf("    %s\n", l.c_str()); }
        CHECK(env != std::string::npos && init != std::string::npos && made != std::string::npos &&
              gl != std::string::npos && shown != std::string::npos && frames != std::string::npos);
        // In the order the steps happen, every one before the first frame.
        CHECK(env < init && init < made && made < gl && gl < shown && shown < frames);
        // Nothing was asked for, so nothing about a request.
        CHECK(!contains(r.log, "FOXSDR_WINDOW_PLATFORM"));
        // The line says what the machine offered, by presence.
        CHECK(contains(r.log, "; WAYLAND_DISPLAY ") && contains(r.log, "; XDG_SESSION_TYPE ") && contains(r.log, "; GLFW 3."));
        CHECK(contains(r.log, "; DISPLAY "));
        if (!secretDisplay.empty()) {
            CHECK(contains(r.log, "; DISPLAY set"));
            CHECK(!contains(r.log, "secret-build-host") && !contains(r.log, "example.invalid") && !contains(r.log, "10.0"));
        }
        // The platform is a word GLFW documents; the GL strings are the driver's, not "(none)".
        const std::size_t w = init + std::string(" window: GLFW initialised; platform chosen: ").size();
        const std::size_t eol = r.log.find('\n', w);
        chosen = r.log.substr(w, eol == std::string::npos ? std::string::npos : eol - w);
        while (!chosen.empty() && (chosen.back() == '\r' || chosen.back() == ' ')) { chosen.pop_back(); }
        CHECK(chosen == "win32" || chosen == "x11" || chosen == "wayland" || chosen == "cocoa");
        CHECK(!contains(r.log, "GL vendor (none); renderer (none)"));
        // Whole lines: none was cut by the log's width (every window: line ends in its own words).
        for (const std::string& l : r.windowLines) { CHECK(l.size() <= 191); }
    }

    // =======================================================================
    // 2. A VALUE THAT NAMES NO PLATFORM is logged as ignored, and the run goes on
    // =======================================================================
    {
        const Result r = once("bad", "bad");
        CHECK(r.ran);  // still starts, still renders
        const std::size_t env = at(r.log, " window: supports ");
        const std::size_t ignored = at(r.log, " window: FOXSDR_WINDOW_PLATFORM=bad is not x11, wayland or any; ignored");
        const std::size_t init = at(r.log, " window: GLFW initialised; platform chosen: ");
        std::printf("  the window: lines with =bad:\n");
        for (const std::string& l : r.windowLines) { std::printf("    %s\n", l.c_str()); }
        CHECK(ignored != std::string::npos);
        CHECK(env != std::string::npos && init != std::string::npos && env < ignored && ignored < init);
        // GLFW chose by itself: the same platform the unset run chose.
        CHECK(contains(r.log, " window: GLFW initialised; platform chosen: " + chosen));
        CHECK(contains(r.log, "frame loop starting"));
    }

    // =======================================================================
    // 3. =any says GLFW chooses, and chooses as before
    // =======================================================================
    {
        const Result r = once("any", "any");
        CHECK(r.ran);
        CHECK(contains(r.log, " window: FOXSDR_WINDOW_PLATFORM=any; GLFW chooses the platform"));
        CHECK(contains(r.log, " window: GLFW initialised; platform chosen: " + chosen));
    }

    // =======================================================================
    // 4. A PLATFORM THIS BUILD DOES NOT HAVE (x11 on Windows) is ignored, and said to be
    // =======================================================================
#if defined(_WIN32)
    {
        const Result r = once("x11-missing", "x11");
        CHECK(r.ran);
        CHECK(contains(r.log, " window: FOXSDR_WINDOW_PLATFORM=x11 asked for, but this build of GLFW does not support it; ignored"));
        CHECK(contains(r.log, " window: GLFW initialised; platform chosen: win32"));
        const Result w = once("wayland-missing", "WAYLAND");  // and case does not matter
        CHECK(w.ran);
        CHECK(contains(w.log, " window: FOXSDR_WINDOW_PLATFORM=wayland asked for, but this build of GLFW does not support it; ignored"));
    }
#endif

    // =======================================================================
    // 5. FORCED to the platform the machine chose (Linux with a window system)
    // =======================================================================
    if (chosen == "x11" || chosen == "wayland") {
        const Result r = once("forced", chosen);
        CHECK(r.ran);
        CHECK(contains(r.log, " window: FOXSDR_WINDOW_PLATFORM=" + chosen + "; asking GLFW for the " + chosen + " platform"));
        CHECK(contains(r.log, " window: GLFW initialised; platform chosen: " + chosen));
    } else {
        std::printf("SKIP: the platform chosen here is %s, which FOXSDR_WINDOW_PLATFORM does not force\n", chosen.c_str());
        ++g_checksSkipped;
    }

    std::error_code rec;
    if (g_checksFailed == 0) { fs::remove_all(g_dir, rec); }
    return testSummary("test_window_platform_app");
}
