/*
 * THE MAP'S COVERAGE COUNTS ONLY WHAT A RECEIVER COULD HEAR (0.99.58), END TO END.
 *
 * The coverage overlay records, for each 5 degree wedge round the receiver, the furthest target
 * heard that way, and the map page says how many wedges are filled and how far the best one
 * reaches ("2 of 72 bearings, best 130 km"). Three kinds of target must not feed it:
 *
 *   a satellite   its position is a prediction from orbital elements, published whether or not
 *                 anything was heard, wherever the satellite is: counted, it would stretch
 *                 wedges across the planet and bury the antenna's real pattern;
 *   an aircraft beyond line of sight
 *                 (more than 1,000 km) is a mis-decode, and one of them pins its wedge and the
 *                 "best" for the rest of the session (a tester's ADS-B page once read "best
 *                 17082 km" from a receiver near Chicago);
 *   (and a target with no position at all, which the host keeps apart from the placed ones.)
 *
 * The rule itself is gui/track_metrics.hpp's recordCoverage, held by test_track_metrics. What
 * that test cannot hold is that the WINDOW applies it: the loop in drawPluginWindows that feeds
 * the accumulator once a frame, for every target, whether or not a map is open. This runs the
 * application itself against the track fixture of tests/fixtures/track_probe_plugin.cpp with
 * TRACK_PROBE_COVERAGE=1 and a receiver at 52.0 N, 0.0 E:
 *
 *   PLACED  51.0 N  1.0 W   about 130 km south-west   an aircraft   counted
 *   NEAR    52.0 N  1.5 E   about 103 km east          an aircraft   counted
 *   FAR     52.0 N 40.0 E   about 2,740 km east        an aircraft   NOT counted (past 1,000 km)
 *   SAT     53.0 N  0.5 E   about 115 km north         a satellite   NOT counted (never)
 *
 * so the page reads two wedges filled and a best of about 130 km. It reads the page's own line
 * through the UI census ("map:coverage:<wedges>/<best km>"), the numbers the line prints and not
 * pixels. The control run has no receiver position: the line says so and nothing is measured.
 *
 * NO NETWORK: every telemetry address is a discard port and the update check is off.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
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

#if defined(_WIN32)
const char* const kExt = ".dll";
#elif defined(__APPLE__)
const char* const kExt = ".dylib";
#else
const char* const kExt = ".so";
#endif

fs::path g_dir;
std::string g_fixtureDll;  // the track fixture, copied into the scratch plugins folder

fs::path pluginsDir() { return g_dir / "local" / "foxsdr" / "plugins"; }

void writeFile(const fs::path& p, const std::string& body) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

void installFixture() {
    std::error_code ec;
    fs::remove_all(pluginsDir(), ec);
    fs::create_directories(pluginsDir(), ec);
    fs::copy_file(g_fixtureDll, pluginsDir() / (std::string("track_probe_fixture") + kExt),
                  fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
}

struct Result {
    bool ok = false;
    std::set<std::string> items;  // the "map:coverage:" values the census saw, prefix removed
    std::string out;              // what the process printed
};

// One run of the application with the map page up (FOXSDR_OPEN_MAP) and the receiver where
// `rx` says (or nowhere). The census keeps each distinct name drawn in the run, so the line's
// values from the frames before the plugin's first poll are in the set beside the last one.
Result once(const std::string& tag, bool rx, int frames) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    const fs::path diag = g_dir / (tag + "-diag");
    {
        std::error_code ec;
        fs::remove_all(diag, ec);
        fs::create_directories(diag, ec);
    }
    writeFile(cfg, std::string("{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
                               "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", "
                               "\"interfaceScale\": \"100\", \"mainView\": \"receiver\", "
                               "\"bandPlanOverlay\": false") +
                       (rx ? ", \"rxPositionSet\": true, \"rxLatDeg\": 52.0, \"rxLonDeg\": 0.0" : "") +
                       " }\n");
    setEnv("FOXSDR_OPEN_MAP", "Track Probe");
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    r.out = run("\"" + exePath() + "\" --frames " + std::to_string(frames) + " 2>&1");
    const bool rendered = r.out.find("rendered " + std::to_string(frames) + " frames") != std::string::npos;
    const bool written = r.out.find("ui census written") != std::string::npos;
    if (!rendered || !written) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), r.out.c_str());
        return r;
    }
    std::ifstream in(census);
    std::string line;
    const std::string prefix = "item map:coverage:";
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.rfind(prefix, 0) == 0) { r.items.insert(line.substr(prefix.size())); }
    }
    r.ok = true;
    return r;
}

std::string join(const std::set<std::string>& s) {
    std::string out;
    for (const std::string& v : s) { out += (out.empty() ? "" : ",") + v; }
    return out;
}

// "<wedges>/<best km>" -> the two numbers; false for any other value ("unset").
bool parse(const std::string& v, int& wedges, int& bestKm) {
    return std::sscanf(v.c_str(), "%d/%d", &wedges, &bestKm) == 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("test_map_coverage_app\n");
    if (argc < 2) {
        std::printf("usage: test_map_coverage_app <track probe module>\n");
        return 2;
    }
    g_fixtureDll = argv[1];
    g_dir = fs::temp_directory_path() / ("cascade-coverage-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir / "appdata", ec);
    fs::create_directories(g_dir / "local", ec);
    fs::create_directories(g_dir / "home", ec);
    setEnv("APPDATA", (g_dir / "appdata").string());
    setEnv("LOCALAPPDATA", (g_dir / "local").string());
    setEnv("XDG_CONFIG_HOME", (g_dir / "appdata").string());
    setEnv("XDG_DATA_HOME", (g_dir / "local").string());
    setEnv("XDG_STATE_HOME", (g_dir / "local").string());
    setEnv("USERPROFILE", (g_dir / "home").string());
    setEnv("HOME", (g_dir / "home").string());
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL",
                            "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("FOXSDR_SINGLE_VIEWPORT", "1");
    setEnv("FOXSDR_WINDOW_SIZE", "1600x1000");
    // A fake package identity puts the plugins folder under LOCALAPPDATA (the scratch tree), not
    // beside the executable in the build tree.
    setEnv("FOXSDR_FAKE_PACKAGE", "FoxSDR.StoreTest_1.0.0.0_x64__test");
    setEnv("FOXSDR_PATCH_START", "");
    setEnv("FOXSDR_PATCH_FILE", "");
    setEnv("FOXSDR_OPEN_PLUGIN_STORE", "");
    setEnv("FOXSDR_OPEN_FITTED_MODULES", "");
    setEnv("FOXSDR_OPEN_DECODER_OUTPUT", "");
    setEnv("FOXSDR_OPEN_DECODERS", "");
    setEnv("FOXSDR_PRESS_PRESET", "");
    setEnv("FOXSDR_SHOT_DIR", "");
    setEnv("FOXSDR_SHOT_AT_FRAME", "");
    setEnv("FOXSDR_WINDOW_POS", "");
    setEnv("FOXSDR_INPUT_SCRIPT", "");
    setEnv("CASCADE_DECODE_TEST", "");
    setEnv("CASCADE_PLUGIN_TEST", "");
    setEnv("TRACK_PROBE_ASSUME_OLD_HOST", "");
    setEnv("TRACK_PROBE_BARE_NAN", "");
    setEnv("TRACK_PROBE_ONLY_HEARD", "");
    setEnv("TRACK_PROBE_COVERAGE", "1");
    installFixture();

    // --- THE RECEIVER IS SET: two aircraft in line of sight count, the far one and the satellite do not ----
    {
        const Result r = once("rx", true, 80);
        CHECK(r.ok);
        if (r.ok) {
            std::printf("    coverage line (every value the run drew): %s\n", join(r.items).c_str());
            CHECK(!r.items.empty());
            int best = -1;     // the most wedges any frame drew
            int bestKm = -1;   // and the furthest reach any frame drew
            int lastWedges = -1;
            int lastKm = -1;
            for (const std::string& v : r.items) {
                int wedges = 0;
                int km = 0;
                if (!parse(v, wedges, km)) { continue; }
                if (wedges > best) { best = wedges; lastWedges = wedges; lastKm = km; }
                if (km > bestKm) { bestKm = km; }
            }
            // The line the settled frames read: PLACED's wedge and NEAR's, 130 km at the furthest.
            CHECK(best == 2);
            CHECK(lastWedges == 2);
            CHECK(lastKm >= 120 && lastKm <= 140);
            // No frame, at any time, drew more than that: the 2,740 km aircraft and the satellite
            // 115 km away never filled a wedge, and the best reach never passed the line of sight.
            CHECK(bestKm >= 120 && bestKm <= 140);
        }
    }

    // --- NO RECEIVER POSITION: the line says so, and nothing is measured ----------------------------
    {
        const Result r = once("norx", false, 60);
        CHECK(r.ok);
        if (r.ok) {
            std::printf("    no receiver position: %s\n", join(r.items).c_str());
            CHECK(r.items == (std::set<std::string>{"unset"}));
        }
    }

    std::error_code ec2;
    fs::remove_all(g_dir, ec2);
    return testSummary("test_map_coverage_app");
}
