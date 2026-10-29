/*
 * A PLUGIN LEFT OUT OF THIS START IS SAID ONCE, AND TURNED BACK ON WITH ONE KEY, IN THE REAL APPLICATION (plugin containment P1; docs/engine-stage4.md
 * 6.28). The crash-loop breaker's rules are pinned in tests/test_plugin_marker.cpp and, against real dead processes, tests/test_plugin_crashloop.cpp; the
 * engine's side of the start (the verdict, the notices fact, the re-enable op and its locks) in tests/test_engine_api_plugin_fault.cpp. This runs
 * cascade itself, with the marker a run that did not end in order leaves behind, the real fault probe fitted as a plugin, and checks from the UI census,
 * the diagnostic log and the files the run wrote:
 *
 *   shown       a marker that says the plugin was inside create(): the card and both its keys are drawn, the plugin was not mapped (the probe's own
 *               log has no line), the strike store holds it as a suspect;
 *   clean       no marker: no card, the plugin is mapped and the store has no record;
 *   reenable    one press of RE-ENABLE: the log says the person re-enabled it, and the plugin is mapped after it (the probe's log shows the load);
 *   seen        one press of Seen: the card is put away and the plugin is NOT re-enabled;
 *   second      a record of one strike and a marker that blames it again: the card says it stays off until re-enabled (the second strike).
 *
 * The press is the application's own input script, aimed from the census. Isolated like every other test that starts the application: its own config,
 * APPDATA, LOCALAPPDATA and diagnostics tree, and every FOXSDR_*_URL pointed at a closed local port.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
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

#include "core/plugin_marker.hpp"
#include "fault_probe_names.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
namespace marker = cascade::core::marker;

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

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float cx() const { return (x0 + x1) * 0.5f; }
    float cy() const { return (y0 + y1) * 0.5f; }
};

struct Result {
    bool ok = false;
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
    std::string log;      // the run's diagnostic log, whole
    std::string probeLog; // what the probe module wrote (one line a call)
    std::string store;    // plugin-faults.json as the run left it
    bool has(const char* item) const { return items.count(item) == 1; }
    bool logs(const char* text) const { return log.find(text) != std::string::npos; }
};

constexpr int kFrames = 80;
fs::path g_dir;
std::string g_probe;

struct Run {
    std::string tag;
    bool deadRun = true;            // leave the marker a run that did not end in order leaves
    std::string store;              // plugin-faults.json to start from ("" none)
    std::string script;
};

// One launch of the application with the probe fitted as "crashy" in a plugins folder of its own, beside a settings file of its own.
Result once(const Run& r) {
    Result out;
    const fs::path home = g_dir / r.tag;
    const fs::path cfgDir = home / "cfg";
    const fs::path cfg = cfgDir / "config.json";
    const fs::path census = home / "census.txt";
    const fs::path diag = home / "diag";
    const fs::path local = home / "local";
    const fs::path plugins = local / "foxsdr" / "plugins";
    const fs::path probeLog = home / "probe.log";
    std::error_code ec;
    fs::remove_all(home, ec);
    fs::create_directories(cfgDir, ec);
    fs::create_directories(diag, ec);
    fs::create_directories(plugins, ec);
    fs::copy_file(g_probe, plugins / ("crashy" CASCADE_PROBE_SUFFIX), fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"diagnosticsEnabled\": true, \"updateCheckEnabled\": false, "
             "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\" }\n";
    }
    if (!r.store.empty()) {
        std::ofstream f(cfgDir / "plugin-faults.json", std::ios::binary | std::ios::trunc);
        f << r.store;
    }
    if (r.deadRun) {
        // what a run that ended inside create() leaves: the marker file with the plugin in play
        marker::resetForTest();
        marker::open((cfgDir / marker::kFileName).string());
        marker::set("crashy", marker::Phase::Create, "1.2.3");
        marker::resetForTest();
    }
    setEnv("APPDATA", (home / "appdata").string());
    setEnv("LOCALAPPDATA", local.string());
    setEnv("XDG_CONFIG_HOME", (home / "appdata").string());
    setEnv("XDG_DATA_HOME", local.string());
    setEnv("XDG_STATE_HOME", local.string());
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    setEnv("FAULT_PROBE_LOG", probeLog.string());
    if (r.script.empty()) {
        setEnv("FOXSDR_INPUT_SCRIPT", "");
    } else {
        const fs::path sp = home / "script.txt";
        std::ofstream f(sp);
        f << r.script;
        f.close();
        setEnv("FOXSDR_INPUT_SCRIPT", sp.string());
    }
    // For a person who wants to LOOK at the card: FAULTNOTICE_SHOT_DIR names a folder, and the "shown" run leaves a screenshot of its frame 70 there.
    const char* const shotDir = std::getenv("FAULTNOTICE_SHOT_DIR");
    const bool shoot = shotDir != nullptr && r.tag == "shown";
    setEnv("FOXSDR_SHOT_DIR", shoot ? std::string(shotDir) : std::string());
    setEnv("FOXSDR_SHOT_AT_FRAME", shoot ? std::string("70") : std::string());
    std::printf("    launching %s (%d frames)\n", r.tag.c_str(), kFrames);
    const std::string stdoutText = run("\"" + exePath() + "\" --frames " + std::to_string(kFrames) + " 2>&1");
    const bool rendered = stdoutText.find("rendered " + std::to_string(kFrames) + " frames") != std::string::npos;
    const bool written = stdoutText.find("ui census written") != std::string::npos;
    const bool scripted = r.script.empty() || stdoutText.find("script steps run") != std::string::npos;
    if (!rendered || !written || !scripted) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", r.tag.c_str(), stdoutText.c_str());
        return out;
    }
    {
        std::ifstream in(census);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("item ", 0) == 0) { out.items.insert(line.substr(5)); }
            if (line.rfind("rect ", 0) != 0) { continue; }
            std::istringstream ss(line.substr(5));
            std::string name;
            Rect rc;
            ss >> name >> rc.x0 >> rc.y0 >> rc.x1 >> rc.y1;
            out.rects[name] = rc;
        }
    }
    out.log = readFile(diag / "logs" / "foxsdr.log");
    out.probeLog = readFile(probeLog);
    out.store = readFile(cfgDir / "plugin-faults.json");
    out.ok = !out.log.empty();
    if (!out.ok) { std::printf("  %s: no diagnostic log was written\n", r.tag.c_str()); }
    std::printf("    %s found: card %s%s, probe log %zu byte(s), store %zu byte(s)\n", r.tag.c_str(), out.has("faultnotice:card") ? "yes" : "no",
                out.has("faultnotice:permanent") ? " (permanent)" : "", out.probeLog.size(), out.store.size());
    return out;
}

std::string press(const Rect& k) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "30 screen %.0f %.0f\n32 down\n34 up\n36 screen %.0f %.0f\n", k.cx(), k.cy(), k.cx() + 400.0f, k.cy() + 300.0f);
    return buf;
}

constexpr const char* kOneStrike =
    "{\n \"plugins\": [\n  {\"id\": \"crashy\", \"phase\": \"create\", \"site\": false, \"state\": \"probation\", \"strikes\": 1, \"version\": \"1.2.3\", "
    "\"when\": 1}\n ],\n \"schema\": 1\n}\n";

void testShown() {
    std::printf("  shown\n");
    Run r;
    r.tag = "shown";
    const Result s = once(r);
    CHECK(s.ok);
    CHECK(s.has("faultnotice:card"));
    CHECK(s.has("faultnotice:id:crashy"));
    CHECK(!s.has("faultnotice:permanent"));
    CHECK(s.rects.count("faultnotice:reenable") == 1);
    CHECK(s.rects.count("faultnotice:seen") == 1);
    // the plugin was left out of the start: not mapped, so the probe was never asked for anything
    CHECK(s.probeLog.empty());
    CHECK(s.store.find("\"state\": \"suspect\"") != std::string::npos);
    CHECK(s.store.find("\"id\": \"crashy\"") != std::string::npos);
}

void testClean() {
    std::printf("  clean\n");
    Run r;
    r.tag = "clean";
    r.deadRun = false;
    const Result s = once(r);
    CHECK(s.ok);
    CHECK(!s.has("faultnotice:card"));
    CHECK(s.rects.count("faultnotice:reenable") == 0);
    CHECK(s.store.empty());
}

void testReenable() {
    std::printf("  reenable\n");
    Run r;
    r.tag = "reenable-find";
    const Result f = once(r);
    CHECK(f.ok);
    CHECK(f.rects.count("faultnotice:reenable") == 1);
    if (f.rects.count("faultnotice:reenable") != 1) { return; }
    const Rect key = f.rects.at("faultnotice:reenable");
    std::printf("    RE-ENABLE at %.0f,%.0f .. %.0f,%.0f\n", key.x0, key.y0, key.x1, key.y1);
    r.tag = "reenable";
    r.script = press(key);
    const Result s = once(r);
    CHECK(s.ok);
    CHECK(s.logs("re-enabled by the user"));
    // after the press the plugin is MAPPED - the probe, which the start left out (its log was empty in the run above), writes a line when the host
    // attaches it - and the record is on probation, strikes kept
    CHECK(!s.probeLog.empty());
    CHECK(s.store.find("\"state\": \"probation\"") != std::string::npos);
    CHECK(s.store.find("\"strikes\": 1") != std::string::npos);
}

void testSeen() {
    std::printf("  seen\n");
    Run r;
    r.tag = "seen-find";
    const Result f = once(r);
    CHECK(f.ok);
    CHECK(f.rects.count("faultnotice:seen") == 1);
    if (f.rects.count("faultnotice:seen") != 1) { return; }
    r.tag = "seen";
    r.script = press(f.rects.at("faultnotice:seen"));
    const Result s = once(r);
    CHECK(s.ok);
    CHECK(!s.logs("re-enabled by the user"));   // Seen puts the card away; it switches nothing on
    CHECK(s.store.find("\"state\": \"suspect\"") != std::string::npos);
}

void testSecondStrike() {
    std::printf("  second\n");
    Run r;
    r.tag = "second";
    r.store = kOneStrike;
    const Result s = once(r);
    CHECK(s.ok);
    CHECK(s.has("faultnotice:card"));
    CHECK(s.has("faultnotice:permanent"));
    CHECK(s.store.find("\"state\": \"disabled\"") != std::string::npos);
    CHECK(s.store.find("\"strikes\": 2") != std::string::npos);
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_plugin_fault_notice_app\n");
    if (argc < 2) {
        std::printf("usage: test_plugin_fault_notice_app <fault probe module>\n");
        return 2;
    }
    g_probe = argv[1];
    g_dir = fs::temp_directory_path() / ("cascade-faultnotice-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL", "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL",
                            "FOXSDR_PROBLEM_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("FOXSDR_SINGLE_VIEWPORT", "1");
    setEnv("FOXSDR_WINDOW_SIZE", "1920x1080");
    setEnv("FOXSDR_FAKE_PACKAGE", "FoxSDR.StoreTest_1.0.0.0_x64__test");
    setEnv("FOXSDR_CENSUS_HOLD_BANK", "1");
    setEnv("FOXSDR_PATCH_START", "");
    setEnv("FOXSDR_PATCH_FILE", "");
    setEnv("FOXSDR_OPEN_PLUGIN_STORE", "");
    setEnv("FOXSDR_PRESS_PRESET", "");
    setEnv("FOXSDR_WINDOW_POS", "");
    setEnv("FOXSDR_FAKE_UPDATE_MANIFEST", "");
    setEnv("FOXSDR_FAKE_STORE_UPDATE", "");
    setEnv("CASCADE_DECODE_TEST", "");
    setEnv("CASCADE_PLUGIN_TEST", "");
    setEnv("FAULT_PROBE", "");

    testShown();
    testClean();
    testReenable();
    testSeen();
    testSecondStrike();

    fs::remove_all(g_dir, ec);
    return testSummary("test_plugin_fault_notice_app");
}
