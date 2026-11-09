// THE SETTINGS ROUND TRIP THROUGH THE REAL WINDOWS (docs/engine-stage4.md 6.48): a real-shaped settings profile - every list, map and
// companion file filled - is handed to foxsdr-slint.exe and to cascade.exe for one bounded run each, untouched, and to foxsdr-slint.exe
// once more with ONE scripted change (the Radio body's Stereo tick), and what each run wrote back is compared with what it was given,
// field by field.
//
// WHY. On 2026-11-03 a tester's config.json went from 8,228 to 5,770 bytes across the first run of the new window and every list came
// back empty; nothing proved which field a run keeps and which it drops. This test is that proof: a field that is MISSING afterwards,
// a list or map that SHRANK, or a scalar that CHANGED without the run having a reason to change it, fails the test and is named.
//
// WHAT MAY DIFFER. (1) The `moves` rows of core/config_halves.hpp (the usage counters, the exit marker, the crash-upload memory): they
// change by themselves. (2) The run's own state, listed in kRunState below with the reason each is the window's to write (the view
// shown, the bank, the folds...). (3) In the scripted run, stereoEnabled - and nothing else. Every difference is printed, allowed or not.
//
// THE SEED is built as an AppConfig and written by ConfigStore::save, then loaded and saved once more: what the parser keeps of it is the
// document the comparison starts from, so a value the loader sanitises on its own (a cap, a range) is never mistaken for a loss. The
// companion files (bookmarks.json, markers.json beside the profile's %APPDATA%\foxsdr) are written as the engine writes them and read
// back the same way.
//
// Isolated like every test that starts the application: APPDATA and the rest point into a scratch folder with this process's id in its
// name, CASCADE_CONFIG_TEST names the run's settings file, every network endpoint is a dead port. Runs on the hidden desktop (gui label).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
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

#include <nlohmann/json.hpp>

#include "core/config.hpp"
#include "core/config_halves.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
using cascade::core::AppConfig;
using cascade::core::ConfigStore;

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

struct Run {
    std::string out;
    int exitCode = -1;
};

Run run(const std::string& cmd) {
    Run r;
#if defined(_WIN32)
    FILE* p = _popen(("\"" + cmd + "\"").c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    char buf[1024];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { r.out += buf; }
#if defined(_WIN32)
    if (p != nullptr) { r.exitCode = _pclose(p); }
#else
    if (p != nullptr) { const int st = pclose(p); r.exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : -1; }
#endif
    return r;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

bool movesByItself(const std::string& key) {
    for (const auto& f : cascade::core::kConfigFields) { if (key == f.name) { return f.moves; } }
    return false;
}

// The fields a bounded run writes as ITS OWN STATE, not the user's (each with the reason): a difference here is noted, never a failure.
const std::map<std::string, const char*> kRunState = {
    {"telemetryLaunches", "the launch counter steps on every start"},
    {"telemetrySessionStarted", "the session's start stamp"},
    {"telemetryCleanExit", "the exit marker"},
    {"telemetryExitsEnded", "the exit tally"},
    {"telemetryFirstRun", "set on the first run of an install"},
    {"telemetryFirstVersion", "set on the first run of an install"},
    {"telemetryInstallId", "minted on the first run of an install"},
    {"pluginLastUpdateCheck", "the store's check stamp"},
    // the engine writes the OPEN source's radio slots and antenna on every save (engine.cpp sourceToSave): a profile that says
    // sourceKind siggen with a radio's soapyArgs comes back with the generator's empty args - both windows, observed 2026-11-09
    {"soapyArgs", "the open source's args (the generator has none)"},
    {"soapyAntenna", "the open source's antenna (the generator has none)"},
    // closedWindows is written empty since 0.79.1 (engine.cpp), pluginsStopped is migrated into pluginRun (engine_plugin_run.cpp)
    {"closedWindows", "written empty by design since 0.79.1 (nothing reads it)"},
    {"pluginsStopped", "migrated into pluginRun as \"stopped\" entries"},
    {"pluginRun", "gains the migrated pluginsStopped entries"},
    {"mapPages", "a page's open flag is false while its plugin is not fitted"},
};

// The seed: every list and map filled with marker entries in the parser's shapes; the scalars a run must not touch set away from
// their defaults where that is safe for a hermetic run (the generator source, no network, the receiver view).
AppConfig seedConfig() {
    AppConfig c;
    c.sourceKind = "siggen";
    c.telemetryEnabled = false;
    c.updateCheckEnabled = false;
    c.mainView = "receiver";
    c.interfaceScale = "100";
    c.uiTheme = "night";
    c.volume = 0.37f;
    c.stereoEnabled = true;
    c.centerHz = 100000000.0;
    c.bandwidthHz = 150000.0;
    c.country = "GB";
    c.language = "en";
    c.aircraftIconPx = 64;
    c.mapTrailWidthPx = 6;
    c.mapTrailStyle = 1;
    c.soapyArgs = "driver=uhd, label=B200 PROBE, serial=PROBE0001, type=b200";
    c.soapyAntenna = "RX2";
    c.biasTee["rtlsdr|serial=00000001"] = true;
    c.biasTee["airspy|serial=PROBE"] = false;
    // no converter on the generator: a converter shifts the SAVED centre to the air frequency (radio + LO), which would read as a change
    c.converters["airspy|serial=PROBE"] = {cascade::core::ConverterMode::Down, 125000000.0, false};
    c.converters["rtlsdr|serial=00000001"] = {cascade::core::ConverterMode::Up, 1250000000.0, true};
    c.ppm["rtlsdr|serial=00000001"] = 12.5;
    c.ppm["sdrplay|serial=PROBE"] = -3.0;
    c.airspy["airspy|serial=PROBE"] = {};
    c.autoGainByRadio["rtlsdr|serial=00000001"] = true;
    c.manualGainByRadio["uhd|serial=PROBE0001"] = {{"PGA", 76.0}};
    c.manualGainByRadio["rtlsdr|serial=00000001"] = {{"TUNER", 28.0}, {"LNA", 12.0}};
    c.patchPresets.push_back({"PROBE-PRESET", "foxsdr-patch 6\nview 0 0 1\nnode 1 0 0 60 80 232 128 0 0 - siggen 0 1 -50 1 Radio\n"});
    c.keyBindings = {"startStop=F2", "mute=F6"};
    c.mapPages.push_back({"ADS-B Aircraft Decoder", 10, 20, 640, 480, true});
    c.mapPages.push_back({"AIS Ship Tracking Decoder", 30, 40, 800, 600, false});
    c.pluginSites = {"https://plugins.example.invalid/index.json"};
    c.pluginTuneAllowed = {"adsb-decoder-1.9.1-abi3-win-x64.dll", "pocsag-decoder-1.0.5-abi3-win-x64.dll"};
    c.pluginTransmitAllowed = {"packet-tnc-0.1.0-abi3-win-x64.dll"};
    c.pluginSettingsAllowed = {"ft8-decoder-1.1.1-abi3-win-x64.dll"};
    c.closedWindows = {"PROBE-WINDOW-A", "PROBE-WINDOW-B"};
    c.pluginsStopped = {"tetra-decoder-0.1.2-abi3-win-x64.dll"};
    c.pluginRun["adsb-decoder"] = "always";
    c.pluginRun["pocsag-decoder"] = "stopped";
    c.pluginMuteOverride = {"PROBE-MUTE"};
    c.pluginSettings["FT8"] = {{"psk.locator", "IO91"}, {"psk.locator_prefilled", "1"}};
    c.pluginSettings["POCSAG"] = {{"frame_details", "1"}};
    c.userPresets.push_back({"pocsag-decoder", "PROBE 153.350", 153350000.0, 1u, 12500.0});
    c.messagesSeen = {"PROBE-MESSAGE-1"};
    // testerUsagePending is NOT seeded: the loader keeps only reports carrying the active tester token (none here), and the queue moves by itself
    c.crashUploadRecent = {"25B7DE2F459BFB4A 1793828707"};
    c.telemetryPending = "PROBE-PENDING";
    c.webUsername = "probe";
    c.webPort = 8090;
    c.catPort = 4540;
    c.gpsPort = "COM9";
    c.gpsBaud = 4800;
    c.rxLatDeg = 51.5;
    c.rxLonDeg = -0.12;
    c.rxPositionSet = true;
    return c;
}

json bookmarksSeed() {
    json b;
    b["schemaVersion"] = 1;
    b["bookmarks"] = json::array();
    b["bookmarks"].push_back({{"name", "PROBE-ONE"}, {"freqHz", 100300000.0}, {"mode", "WFM"}, {"bandwidthHz", 150000.0}, {"group", "Probe"}, {"favourite", true}});
    b["bookmarks"].push_back({{"name", "PROBE-TWO"}, {"freqHz", 145500000.0}, {"mode", "NFM"}, {"bandwidthHz", 12500.0}, {"group", "Probe"}, {"scan", true}});
    b["bookmarks"].push_back({{"name", "PROBE-THREE"}, {"freqHz", 7074000.0}, {"mode", "USB"}, {"bandwidthHz", 2700.0}});
    return b;
}

json markersSeed() {
    json m;
    m["schemaVersion"] = 1;   // the loader refuses a file without it
    m["markers"] = json::array();
    // the engine's own shape (core/freq_markers.cpp): freqHz, the marker's number n, an optional note
    m["markers"].push_back({{"freqHz", 100100000.0}, {"n", 1}, {"note", "PROBE-MARK-A"}});
    m["markers"].push_back({{"freqHz", 100700000.0}, {"n", 2}, {"note", "PROBE-MARK-B"}});
    m["next"] = 3;
    return m;
}

// One comparison of two documents: what is missing, what shrank, what changed. `allowedChanges` are the keys the run may change.
struct Diff {
    std::vector<std::string> missing, shrank, changed, allowed;
};

Diff compare(const json& before, const json& after, const std::set<std::string>& allowedChanges) {
    Diff d;
    for (auto it = before.begin(); it != before.end(); ++it) {
        const std::string& key = it.key();
        const auto at = after.find(key);
        if (at == after.end()) { d.missing.push_back(key); continue; }
        const json& b = it.value();
        const json& a = *at;
        if ((b.is_array() || b.is_object()) && a.type() == b.type() && a.size() < b.size()) {
            const std::string line = key + " (" + std::to_string(b.size()) + " -> " + std::to_string(a.size()) + ")";
            if (kRunState.count(key) != 0) { d.allowed.push_back(line + "  [" + kRunState.at(key) + "]"); } else { d.shrank.push_back(line); }
            continue;
        }
        if (a == b) { continue; }
        const std::string line = key + ": " + b.dump(-1, ' ', false, json::error_handler_t::replace) + " -> " + a.dump(-1, ' ', false, json::error_handler_t::replace);
        if (movesByItself(key)) { d.allowed.push_back(line + "  [moves by itself]"); }
        else if (kRunState.count(key) != 0) { d.allowed.push_back(line + "  [" + kRunState.at(key) + "]"); }
        else if (allowedChanges.count(key) != 0) { d.allowed.push_back(line + "  [the scripted change]"); }
        else { d.changed.push_back(line); }
    }
    return d;
}

void print(const char* what, const std::vector<std::string>& lines) {
    if (lines.empty()) { return; }
    std::printf("  %s (%zu):\n", what, lines.size());
    for (const auto& l : lines) { std::printf("    %s\n", l.c_str()); }
}

}  // namespace

int main() {
    std::printf("test_slint_config_roundtrip\n");
    const fs::path dir = fs::temp_directory_path() / ("foxsdr-config-roundtrip-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "appdata" / "foxsdr", ec);
    fs::create_directories(dir / "local", ec);
    fs::create_directories(dir / "home", ec);
    fs::create_directories(dir / "diag", ec);
    setEnv("APPDATA", (dir / "appdata").string());
    setEnv("LOCALAPPDATA", (dir / "local").string());
    setEnv("XDG_CONFIG_HOME", (dir / "appdata").string());
    setEnv("XDG_DATA_HOME", (dir / "local").string());
    setEnv("XDG_STATE_HOME", (dir / "local").string());
    setEnv("FOXSDR_DIAG_DIR", (dir / "diag").string());
    setEnv("USERPROFILE", (dir / "home").string());
    setEnv("HOME", (dir / "home").string());
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL", "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("CASCADE_DECODE_TEST", "1");   // the receiver runs, as a session has it

    // ---- the seed, normalised through the parser once ----
    const fs::path seedPath = dir / "seed.json";
    std::string err;
    CHECK(ConfigStore::save(seedPath.string(), seedConfig(), err));
    {
        AppConfig back;
        CHECK(ConfigStore::load(seedPath.string(), back, err));
        CHECK(ConfigStore::save(seedPath.string(), back, err));
    }
    const std::string seedText = readFile(seedPath);
    const json seed = json::parse(seedText, nullptr, false);
    CHECK(!seed.is_discarded());
    std::printf("  the seed: %zu bytes, %zu keys\n", seedText.size(), seed.size());
    // every container the seed fills is non-empty, or the test proves nothing about it
    for (const char* k : {"biasTee", "converters", "ppm", "airspy", "autoGainByRadio", "manualGainByRadio", "patchPresets", "keyBindings", "mapPages", "pluginSites",
                          "pluginTuneAllowed", "pluginTransmitAllowed", "pluginSettingsAllowed", "closedWindows", "pluginsStopped", "pluginRun", "pluginMuteOverride",
                          "pluginSettings", "userPresets", "messagesSeen", "crashUploadRecent"}) {
        const bool filled = seed.contains(k) && !seed[k].empty();
        if (!filled) { std::printf("  the seed's %s is empty after the parser: the loader dropped the marker\n", k); }
        CHECK(filled);
    }
    const json bookmarks = bookmarksSeed();
    const json markers = markersSeed();
    const fs::path bookmarksPath = dir / "appdata" / "foxsdr" / "bookmarks.json";
    const fs::path markersPath = dir / "appdata" / "foxsdr" / "markers.json";

    struct Case {
        const char* name;
        std::string exe;
        std::string script;   // FOXSDR_INPUT_SCRIPT text, "" for an untouched run
        std::set<std::string> allowed;
        std::map<std::string, std::string> env;
    };
    std::vector<Case> cases;
    cases.push_back({"slint untouched", FOXSDR_SLINT_EXE, "", {}, {}});
#ifdef FOXSDR_CASCADE_EXE
    cases.push_back({"classic untouched", FOXSDR_CASCADE_EXE, "", {}, {}});
#else
    std::printf("  classic untouched: NOT RUN (cascade.exe is not built in this tree)\n");
#endif
    cases.push_back({"slint with one change (Stereo off)", FOXSDR_SLINT_EXE,
                     "# the Radio body: Stereo on -> off, read back from the engine\n20 state stereo == 1\n+1 click \"Stereo\"\n+20 state stereo == 0\n",
                     {"stereoEnabled"}, {{"FOXSDR_SLINT_BANK", "0"}, {"FOXSDR_SLINT_SECTIONS", "0:2"}}});

    for (const Case& c : cases) {
        std::printf("---- %s ----\n", c.name);
        const fs::path cfg = dir / "config.json";
        writeFile(cfg, seedText);
        writeFile(bookmarksPath, bookmarks.dump(4, ' ', false, json::error_handler_t::replace) + "\n");
        writeFile(markersPath, markers.dump(4, ' ', false, json::error_handler_t::replace) + "\n");
        setEnv("CASCADE_CONFIG_TEST", cfg.string());
        const fs::path scriptPath = dir / "input.txt";
        if (!c.script.empty()) { writeFile(scriptPath, c.script); }
        setEnv("FOXSDR_INPUT_SCRIPT", c.script.empty() ? std::string() : scriptPath.string());
        for (const auto& [k, v] : c.env) { setEnv(k.c_str(), v); }
        const Run r = run("\"" + c.exe + "\" --frames 150 2>&1");
        for (const auto& [k, v] : c.env) { setEnv(k.c_str(), ""); }
        const bool finished = r.out.find("150 of 150") != std::string::npos || r.exitCode == 0;
        CHECK(r.exitCode == 0);
        if (r.exitCode != 0 || !finished) { std::printf("  exit %d\n--- the run's output ---\n%s\n--- end ---\n", r.exitCode, r.out.c_str()); }
        if (!c.script.empty()) {
            const bool clean = r.out.find(", 0 failed, 0 not reached") != std::string::npos;
            CHECK(clean);
            if (!clean) { std::printf("--- the run's output ---\n%s\n--- end ---\n", r.out.c_str()); }
        }
        const std::string afterText = readFile(cfg);
        const json after = json::parse(afterText, nullptr, false);
        CHECK(!after.is_discarded());
        std::printf("  config.json: %zu -> %zu bytes, %zu -> %zu keys\n", seedText.size(), afterText.size(), seed.size(), after.is_discarded() ? 0 : after.size());
        if (after.is_discarded()) { continue; }
        const Diff d = compare(seed, after, c.allowed);
        print("MISSING", d.missing);
        print("SHRANK", d.shrank);
        print("CHANGED", d.changed);
        print("allowed differences", d.allowed);
        CHECK(d.missing.empty());
        CHECK(d.shrank.empty());
        CHECK(d.changed.empty());
        for (const std::string& k : c.allowed) {
            bool seen = false;
            for (const auto& l : d.allowed) { if (l.rfind(k + ":", 0) == 0) { seen = true; } }
            if (!seen) { std::printf("  the scripted change did not reach the file: %s unchanged\n", k.c_str()); }
            CHECK(seen);
        }
        // the companion files
        const json bAfter = json::parse(readFile(bookmarksPath), nullptr, false);
        const json mAfter = json::parse(readFile(markersPath), nullptr, false);
        CHECK(!bAfter.is_discarded() && bAfter.contains("bookmarks") && bAfter["bookmarks"].size() == bookmarks["bookmarks"].size());
        CHECK(!mAfter.is_discarded() && mAfter.contains("markers") && mAfter["markers"].size() == markers["markers"].size());
        if (!bAfter.is_discarded() && bAfter.contains("bookmarks")) {
            for (const auto& b : bookmarks["bookmarks"]) {
                bool found = false;
                for (const auto& a : bAfter["bookmarks"]) { if (a.value("name", "") == b.value("name", "") && a.value("group", "") == b.value("group", "") && a.value("favourite", false) == b.value("favourite", false)) { found = true; } }
                if (!found) { std::printf("  bookmark %s lost or changed\n", b.value("name", "").c_str()); }
                CHECK(found);
            }
        }
        std::printf("  bookmarks %zu -> %zu, markers %zu -> %zu\n", bookmarks["bookmarks"].size(), bAfter.is_discarded() ? 0 : bAfter.value("bookmarks", json::array()).size(),
                    markers["markers"].size(), mAfter.is_discarded() ? 0 : mAfter.value("markers", json::array()).size());
    }
    return testSummary("test_slint_config_roundtrip");
}
