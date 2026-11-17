// drive_handover_run.cpp - LEGACY IS A CHOICE INSIDE THE NEW WINDOW, through the real programs (engine fixes 7; src/core/window_handover.hpp has the rules).
// Every run is on a settings folder of its own (APPDATA and LOCALAPPDATA pointed at a scratch copy), the radio is the signal generator, nothing of the owner's is touched, and
// the whole test runs on the hidden desktop (the GUI gate's fixture).
//
//   D1  NEW -> LEGACY, THE RECORD SEAM, a bounded run: the Layouts menu's Legacy entry is chosen at tick 20; the window closes cleanly (exit 0, long before its frame count) and
//       the request is RECORDED - one line, program cascade.exe, from the new window, the settings file - and no second window is started; the choice is saved ("legacy")
//   D2  THE SAME in a REAL (not bounded) run, which takes the settings lock: the record line says lock=free - the launch was asked for only after the lock was let go
//   D3  THE REMEMBERED CHOICE: the next start opens Legacy (the request is recorded, no window is shown); a window started BY a hand-over does not hand back (the loop guard)
//   D4  THE PROGRAM MISSING: the Legacy program is not there: the window stays, nothing recorded, the choice is not written
//   D5  LEGACY -> NEW, THE RECORD SEAM: the Legacy window's own switch at its frame 30: one line, program foxsdr-slint.exe, from legacy; the choice goes back to "new"
//   D6  REAL HAND-OVER, NEW -> LEGACY: the first window is retuned (3.620 MHz), switches at tick 120; the Legacy window comes up (started bounded, 60 frames) and reads the SAME
//       settings: the frequency the first window left
//   D7  REAL HAND-OVER, LEGACY -> NEW: the same the other way
//
// Registered with the GUI gate's label (windows are opened).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/config.hpp"
#include "core/window_handover.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
namespace h = cascade::core::handover;

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
    if (value.empty()) { ::unsetenv(name); } else { ::setenv(name, value.c_str(), 1); }
#endif
}

// Runs a command line and returns its output (stdout and stderr) and, through `code`, its exit code. A program the first one started with the same handles keeps the pipe open,
// so this returns when the whole chain has ended.
std::string run(const std::string& cmd, int* code = nullptr) {
    std::string out;
#if defined(_WIN32)
    FILE* p = _popen(("\"" + cmd + "\"").c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    char buf[512];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
    int rc = -1;
#if defined(_WIN32)
    if (p != nullptr) { rc = _pclose(p); }
#else
    if (p != nullptr) { rc = pclose(p); }
#endif
    if (code != nullptr) { *code = rc; }
    return out;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

fs::path g_dir;
fs::path g_cfg;   // the settings file of this scratch profile: %APPDATA%/foxsdr/config.json
int g_runs = 0;

void writeConfig(double centreHz) {
    fs::create_directories(g_cfg.parent_path());
    std::ofstream f(g_cfg, std::ios::binary | std::ios::trunc);
    f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": \"siggen\", \"mainView\": \"receiver\", "
         "\"bandPlanOverlay\": false, \"bandPlanSelection\": \"uk\", \"mode\": \"USB\", \"centerHz\": "
      << static_cast<long long>(centreHz) << ", \"sampleRateHz\": 2000000, \"vfoOffsetHz\": 0 }\n";
}

// A clean slate: the settings file, no remembered choice, no seams.
void fresh(double centreHz = 7100000.0) {
    std::error_code ec;
    fs::remove(h::choicePath(g_cfg.string()), ec);
    writeConfig(centreHz);
    for (const char* k : {h::kRecordEnv, h::kAtEnv, h::kChildArgsEnv, h::kChildLogEnv, h::kFromEnv, "FOXSDR_SLINT_RETUNE_PROBE", "FOXSDR_SLINT_MENU"}) { setEnv(k, ""); }
    setEnv(h::kLegacyExeEnv, FOXSDR_LEGACY_EXE);
    setEnv(h::kNewExeEnv, FOXSDR_SLINT_EXE);
    setEnv("CASCADE_CONFIG_TEST", g_cfg.string());   // a bounded run's settings file: the one a real run of this profile uses
    ++g_runs;
}

std::string record(const char* tag) { return readFile(g_dir / (std::string(tag) + ".record.txt")); }
void seamRecord(const char* tag) {
    std::error_code ec;
    fs::remove(g_dir / (std::string(tag) + ".record.txt"), ec);
    setEnv(h::kRecordEnv, (g_dir / (std::string(tag) + ".record.txt")).string());
}

// The second window was started and not waited for: its output file is complete when it holds the settings line it prints ("config applied: ..."), or after a bound.
std::string waitChildLog(const fs::path& p) {
    std::string text;
    for (int i = 0; i < 400; ++i) {   // up to 40 s
        text = readFile(p);
        // the settings line is printed on the standard OUTPUT (buffered until the run ends), the rest on the error stream: wait for that line, not for the end of the stream
        if (text.find("config applied") != std::string::npos) { return text; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return text;
}

long long elapsedMs(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

int count(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t at = 0; (at = hay.find(needle, at)) != std::string::npos; at += needle.size()) { ++n; }
    return n;
}

double savedCentre() {
    cascade::core::AppConfig saved;
    std::string err;
    return cascade::core::ConfigStore::load(g_cfg.string(), saved, err) ? saved.centerHz : -1.0;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_slint_handover_run\n");
    g_dir = fs::temp_directory_path() / ("foxsdr-handover-" + std::to_string(pid()));
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
    setEnv("FOXSDR_NO_OUTSIDE_REPORTS", "1");   // no report leaves this computer from a test or a drive (core/report_send_guard.hpp)
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL", "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    g_cfg = g_dir / "appdata" / "foxsdr" / "config.json";
    const std::string slint = std::string("\"") + FOXSDR_SLINT_EXE + "\"";
    const std::string legacy = std::string("\"") + FOXSDR_LEGACY_EXE + "\"";
    CHECK(fs::exists(FOXSDR_SLINT_EXE));
    CHECK(fs::exists(FOXSDR_LEGACY_EXE));

    // --- D1: NEW -> LEGACY, the record seam, a bounded run ---------------------------------------------------------------------------------------
    {
        fresh();
        seamRecord("d1");
        setEnv(h::kAtEnv, "20");
        int rc = -1;
        const auto t0 = std::chrono::steady_clock::now();
        const std::string out = run(slint + " --frames 600 2>&1", &rc);
        const std::string rec = record("d1");
        std::printf("    d1: exit %d in %lld ms; record: %s", rc, elapsedMs(t0), rec.c_str());
        CHECK(rc == 0);   // the window closed cleanly
        CHECK(count(rec, "launch ") == 1 && rec.find("launch target=legacy program=cascade.exe from=new lock=free") != std::string::npos);
        CHECK(rec.find("settings=" + g_cfg.string()) != std::string::npos);
        CHECK(out.find("600 frames") == std::string::npos);   // it did not run its 600 ticks: the choice ended it
        CHECK(h::readChoice(g_cfg.string()) == h::Window::Legacy);   // remembered
    }
    // --- D2: the same in a REAL run, which holds the settings lock ---------------------------------------------------------------------------------
    {
        fresh();
        seamRecord("d2");
        setEnv(h::kAtEnv, "30");
        int rc = -1;
        const std::string out = run(slint + " 2>&1", &rc);
        const std::string rec = record("d2");
        std::printf("    d2: exit %d; record: %s", rc, rec.c_str());
        CHECK(rc == 0);
        CHECK(count(rec, "launch ") == 1 && rec.find("from=new lock=free parent=") != std::string::npos);   // asked for only after the lock was let go
        CHECK(h::readChoice(g_cfg.string()) == h::Window::Legacy);
        (void)out;
    }
    // --- D3: the remembered choice is honoured at the next start; a window started by a hand-over does not hand back --------------------------------------------
    {
        fresh();
        CHECK(h::writeChoice(g_cfg.string(), h::Window::Legacy));
        seamRecord("d3");
        int rc = -1;
        const std::string out = run(slint + " --frames 60 2>&1", &rc);
        const std::string rec = record("d3");
        std::printf("    d3: exit %d; record: %s", rc, rec.c_str());
        CHECK(rc == 0);
        CHECK(count(rec, "launch ") == 1 && rec.find("target=legacy") != std::string::npos);
        CHECK(out.find("the Legacy window was the last one chosen") != std::string::npos);
        CHECK(out.find("frames painted") == std::string::npos);   // no window was made
        // the loop guard: the same profile, started by the other window's hand-over
        seamRecord("d3b");
        setEnv(h::kFromEnv, "4242:legacy:");
        const std::string out2 = run(slint + " --frames 60 2>&1", &rc);
        std::printf("    d3b: exit %d; record: [%s]\n", rc, record("d3b").c_str());
        CHECK(rc == 0 && record("d3b").empty());
        CHECK(out2.find("frames painted") != std::string::npos);   // it ran as a window
        setEnv(h::kFromEnv, "");
    }
    // --- D4: the Legacy program is not there: the window stays ----------------------------------------------------------------------------------------------------
    {
        fresh();
        setEnv(h::kLegacyExeEnv, (g_dir / "no-such-cascade.exe").string());
        seamRecord("d4");
        setEnv(h::kAtEnv, "20");
        int rc = -1;
        const std::string out = run(slint + " --frames 80 2>&1", &rc);
        std::printf("    d4: exit %d; record: [%s]\n", rc, record("d4").c_str());
        CHECK(rc == 0);
        CHECK(record("d4").empty());
        CHECK(h::readChoice(g_cfg.string()) == h::Window::New);
        CHECK(out.find("frames painted") != std::string::npos);   // it stayed and ran its frames
    }
    // --- D5: LEGACY -> NEW, the record seam ------------------------------------------------------------------------------------------------------------------------
    {
        fresh();
        CHECK(h::writeChoice(g_cfg.string(), h::Window::Legacy));
        seamRecord("d5");
        setEnv(h::kAtEnv, "30");
        int rc = -1;
        const auto t0 = std::chrono::steady_clock::now();
        const std::string out = run(legacy + " --frames 900 2>&1", &rc);
        const std::string rec = record("d5");
        std::printf("    d5: exit %d in %lld ms; record: %s", rc, elapsedMs(t0), rec.c_str());
        CHECK(rc == 0);
        CHECK(count(rec, "launch ") == 1 && rec.find("launch target=new program=foxsdr-slint.exe from=legacy") != std::string::npos);
        CHECK(h::readChoice(g_cfg.string()) == h::Window::New);   // the way back is remembered
        (void)out;
    }
    // --- D6: THE REAL HAND-OVER, NEW -> LEGACY ------------------------------------------------------------------------------------------------------------------
    {
        fresh();
        setEnv("FOXSDR_SLINT_RETUNE_PROBE", "10;3.620");   // the first window is retuned (zoom at tick 40, 3.620 MHz typed at tick 60)
        setEnv(h::kAtEnv, "120");
        setEnv(h::kChildArgsEnv, "--frames 60");   // the Legacy window is started bounded, so it ends itself; it prints the settings it came up with
        setEnv(h::kChildLogEnv, (g_dir / "d6.child.txt").string());
        int rc = -1;
        (void)run(slint + " 2>&1", &rc);
        const std::string out = waitChildLog(g_dir / "d6.child.txt");
        const double left = savedCentre();
        std::printf("    d6: exit %d; settings left centre %.0f Hz\n", rc, left);
        const std::size_t at = out.find("config applied: mode=");
        const std::string line = at == std::string::npos ? std::string() : out.substr(at, out.find('\n', at) - at);
        std::printf("    d6: the Legacy window's line: [%s]\n", line.c_str());
        CHECK(rc == 0);
        CHECK(!line.empty());   // the second window came up, and read the first one's settings
        CHECK(count(out, "config applied") == 1);   // ONE second window: the test seams do not pass to it, so the two never hand over to each other
        double centre = -1;
        if (!line.empty()) { std::sscanf(line.c_str(), "config applied: mode=%*s center=%lf", &centre); }
        CHECK(centre > 0 && left > 0 && centre == left);   // the same frequency the first window left
        CHECK(left != 7100000.0);                           // ...and it is the retuned one, not the one the profile started with
        CHECK(h::readChoice(g_cfg.string()) == h::Window::Legacy);
    }
    // --- D7: THE REAL HAND-OVER, LEGACY -> NEW ------------------------------------------------------------------------------------------------------------------
    {
        fresh(14070000.0);
        CHECK(h::writeChoice(g_cfg.string(), h::Window::Legacy));
        setEnv(h::kAtEnv, "40");
        setEnv(h::kChildArgsEnv, "--frames 60");
        setEnv(h::kChildLogEnv, (g_dir / "d7.child.txt").string());
        int rc = -1;
        (void)run(legacy + " 2>&1", &rc);
        const std::string out = waitChildLog(g_dir / "d7.child.txt");
        const std::size_t at = out.find("config applied: mode=");
        const std::string line = at == std::string::npos ? std::string() : out.substr(at, out.find('\n', at) - at);
        std::printf("    d7: exit %d; the new window's line: [%s]\n", rc, line.c_str());
        CHECK(rc == 0);
        CHECK(!line.empty());
        double centre = -1;
        if (!line.empty()) { std::sscanf(line.c_str(), "config applied: mode=%*s center=%lf", &centre); }
        CHECK(centre == 14070000.0);
        CHECK(count(out, "config applied") == 1);
        CHECK(h::readChoice(g_cfg.string()) == h::Window::New);
    }

    const int rc = testSummary("test_slint_handover_run");
    if (rc == 0) {
        fs::remove_all(g_dir, ec);
    } else {
        std::printf("  kept for inspection: %s\n", g_dir.string().c_str());
    }
    return rc;
}
