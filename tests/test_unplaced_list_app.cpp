/*
 * AIRCRAFT HEARD BUT NOT YET PLACED ARE LISTED, NEVER DRAWN (0.99.75), END TO END.
 *
 * Runs cascade itself, isolated like every test that starts the application, in ONE viewport
 * (FOXSDR_SINGLE_VIEWPORT=1, so the UI census reads every rectangle in the main window), with
 * the track fixture of tests/fixtures/track_probe_plugin.cpp fitted - a real track source that
 * keeps the ADS-B plugin 1.9.0's contract: it asks the host's get_state for
 * CASCADE_STATE_LISTS_UNPLACED_TRACKS and publishes the aircraft it has heard without a position
 * (CASCADE_TRACK_FLAG_NO_POSITION, NaN coordinates) ONLY when the host says it understands - and
 * its map page opened by the bounded-run seam FOXSDR_OPEN_MAP:
 *
 *   default   the host set the bit (otherwise the fixture publishes the placed aircraft alone and
 *             the numbers below are 0): the page's target list reports 1 placed target and 2
 *             heard without a position (the third, last heard 61 s ago, is past an aircraft's
 *             60 s and is dropped), draws a row for the placed one and one for each of the two,
 *             the heard rows BELOW the placed row;
 *   bare NaN  an old or careless plugin that sends NaN coordinates with no flag: listed too, as
 *             the host's defence against putting NaN into a renderer (3 heard);
 *   on screen with "On screen only" ticked (saved in the config) the heard rows are gone - they
 *             are never on screen - and the heading still counts them;
 *   only heard nothing placed at all: the two heard rows are still drawn (the list is not the
 *             "No targets." empty state while it has rows).
 *
 * The census carries the heading's numbers ("tracklist:count:<shown>/<total>" and
 * "tracklist:unplaced:<n>") and a rectangle for each row's label ("tracklist:row:<id>" and
 * "tracklist:heardrow:<id>"). That the map draws none of the heard ones is the unit test's
 * (test_map_view: the same picture, the same vertex count, no NaN).
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
#include <iterator>
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

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

struct Result {
    bool ok = false;
    std::set<std::string> items;  // "item <name>" lines of the census
    std::map<std::string, Rect> rects;
    std::string out;  // what the process printed
    bool has(const std::string& name) const { return items.count(name) != 0u; }
    const Rect* rect(const std::string& name) const {
        const auto it = rects.find(name);
        return it == rects.end() ? nullptr : &it->second;
    }
    // What follows "<prefix>" in EVERY census item that starts with it. The census keeps each
    // distinct name drawn in the run, so a heading that read "0/0" before the plugin's first poll
    // and "1/1" after it leaves both; the last frame's is the one in the set that is not an
    // earlier state, which the checks below name (they assert what the set holds and what it
    // must not).
    std::set<std::string> itemsWith(const std::string& prefix) const {
        std::set<std::string> out;
        for (const std::string& s : items) {
            if (s.rfind(prefix, 0) == 0) { out.insert(s.substr(prefix.size())); }
        }
        return out;
    }
    static std::string join(const std::set<std::string>& s) {
        std::string out;
        for (const std::string& v : s) { out += (out.empty() ? "" : ",") + v; }
        return out;
    }
};

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

// One run of the application. `onScreenOnly` is the saved "On screen only" tick.
Result once(const std::string& tag, int frames, bool onScreenOnly, bool bareNan,
            bool onlyHeard = false) {
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
                               "\"bandPlanOverlay\": false, \"mapListOnScreenOnly\": ") +
                       (onScreenOnly ? "true" : "false") + " }\n");
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    setEnv("TRACK_PROBE_BARE_NAN", bareNan ? "1" : "");
    setEnv("TRACK_PROBE_ONLY_HEARD", onlyHeard ? "1" : "");
    r.out = run("\"" + exePath() + "\" --frames " + std::to_string(frames) + " 2>&1");
    const bool rendered = r.out.find("rendered " + std::to_string(frames) + " frames") != std::string::npos;
    const bool written = r.out.find("ui census written") != std::string::npos;
    if (!rendered || !written) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), r.out.c_str());
        return r;
    }
    std::ifstream in(census);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("item ", 0) == 0) { r.items.insert(line.substr(5)); }
        if (line.rfind("rect ", 0) != 0) { continue; }
        std::istringstream ss(line.substr(5));
        std::vector<std::string> tok;
        std::string t;
        while (ss >> t) { tok.push_back(t); }
        if (tok.size() < 5) { continue; }
        const std::size_t n = tok.size();
        Rect rc;
        rc.x0 = std::strtof(tok[n - 4].c_str(), nullptr);
        rc.y0 = std::strtof(tok[n - 3].c_str(), nullptr);
        rc.x1 = std::strtof(tok[n - 2].c_str(), nullptr);
        rc.y1 = std::strtof(tok[n - 1].c_str(), nullptr);
        std::string name = tok[0];
        for (std::size_t i = 1; i + 4 < n; ++i) { name += " " + tok[i]; }
        r.rects[name] = rc;
    }
    r.ok = true;
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("test_unplaced_list_app\n");
    if (argc < 2) {
        std::printf("usage: test_unplaced_list_app <track probe module>\n");
        return 2;
    }
    g_fixtureDll = argv[1];
    g_dir = fs::temp_directory_path() / ("cascade-unplaced-" + std::to_string(pid()));
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
    setEnv("FOXSDR_OPEN_MAP", "Track Probe");
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
    installFixture();

    // --- DEFAULT: the host set the bit, the plugin published, the list says so -----------------------
    {
        const Result r = once("default", 80, false, false);
        CHECK(r.ok);
        if (r.ok) {
            const std::set<std::string> count = r.itemsWith("tracklist:count:");
            const std::set<std::string> unplaced = r.itemsWith("tracklist:unplaced:");
            std::printf("    heading: placed %s, heard without a position %s\n",
                        Result::join(count).c_str(), Result::join(unplaced).c_str());
            // ONE placed target; the heading's two numbers (rows shown / targets live). A frame
            // before the plugin's first poll may have read nothing at all: 0/0 is allowed, 0/1
            // or 2/2 is not.
            CHECK(count.count("1/1") == 1u);
            CHECK(count == (std::set<std::string>{"0/0", "1/1"}) ||
                  count == (std::set<std::string>{"1/1"}));
            // TWO heard without a position: HEARD1 and HEARD2. HEARDOLD (61 s) is past an
            // aircraft's 60 s and is not counted, and nothing else is.
            CHECK(unplaced.count("2") == 1u);
            CHECK(unplaced == (std::set<std::string>{"0", "2"}) ||
                  unplaced == (std::set<std::string>{"2"}));
            const Rect* placed = r.rect("tracklist:row:PLACED");
            const Rect* h1 = r.rect("tracklist:heardrow:HEARD1");
            const Rect* h2 = r.rect("tracklist:heardrow:HEARD2");
            CHECK(placed != nullptr);
            CHECK(h1 != nullptr);
            CHECK(h2 != nullptr);
            CHECK(r.rect("tracklist:heardrow:HEARDOLD") == nullptr);
            CHECK(r.rect("tracklist:heardrow:PLACED") == nullptr);
            CHECK(r.rect("tracklist:row:HEARD1") == nullptr);
            if (placed != nullptr && h1 != nullptr && h2 != nullptr) {
                std::printf("    rows: PLACED x %.0f y %.0f, HEARD1 x %.0f y %.0f, HEARD2 x %.0f y %.0f\n",
                            placed->x0, placed->y0, h1->x0, h1->y0, h2->x0, h2->y0);
                // AFTER the placed row, in the order the plugin published them, in one column. (A
                // placed row's label is a Selectable that spans the cell and a heard row's is text
                // inside it, so the left edges differ by the cell's padding and no more.)
                CHECK(h1->y0 > placed->y0);
                CHECK(h2->y0 > h1->y0);
                CHECK(h1->x0 >= placed->x0 && h1->x0 - placed->x0 <= 12.0f);
                CHECK(h2->x0 >= placed->x0 && h2->x0 - placed->x0 <= 12.0f);
                // Every heard row has a second line (the altitude and the words, or just the
                // words), so it is taller than the single-line placed row: HEARD2 starts further
                // below HEARD1 than HEARD1 does below PLACED.
                CHECK(h2->y0 - h1->y0 > h1->y0 - placed->y0);
            }
        }
    }

    // --- BARE NaN: an old plugin's unflagged NaN is listed, not drawn and not dropped ----------------
    {
        const Result r = once("barenan", 80, false, true);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.itemsWith("tracklist:count:").count("1/1") == 1u);
            const std::set<std::string> unplaced = r.itemsWith("tracklist:unplaced:");
            std::printf("    bare NaN: heard without a position %s\n", Result::join(unplaced).c_str());
            CHECK(unplaced.count("3") == 1u);
            CHECK(unplaced.count("2") == 0u);  // the bare one is never left out
            CHECK(r.rect("tracklist:heardrow:HEARDBARE") != nullptr);
        }
    }

    // --- ON SCREEN ONLY: never on screen, so no rows, and the heading still says how many -----------
    {
        const Result r = once("onscreen", 80, true, false);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.itemsWith("tracklist:count:").count("1/1") == 1u);
            CHECK(r.itemsWith("tracklist:unplaced:").count("2") == 1u);
            CHECK(r.rect("tracklist:row:PLACED") != nullptr);
            CHECK(r.rect("tracklist:heardrow:HEARD1") == nullptr);
            CHECK(r.rect("tracklist:heardrow:HEARD2") == nullptr);
        }
    }

    // --- ONLY HEARD: nothing placed, two heard - the list is not "No targets" ------------------------
    // Without a placed row the early empty-state return is the only thing between the heard rows and
    // the user: it must not fire while rows exist.
    {
        const Result r = once("onlyheard", 80, false, false, true);
        CHECK(r.ok);
        if (r.ok) {
            std::printf("    only heard: heading %s, heard %s\n",
                        Result::join(r.itemsWith("tracklist:count:")).c_str(),
                        Result::join(r.itemsWith("tracklist:unplaced:")).c_str());
            CHECK(r.itemsWith("tracklist:count:").count("0/0") == 1u);
            CHECK(r.itemsWith("tracklist:unplaced:").count("2") == 1u);
            CHECK(r.rect("tracklist:row:PLACED") == nullptr);
            CHECK(r.rect("tracklist:heardrow:HEARD1") != nullptr);
            CHECK(r.rect("tracklist:heardrow:HEARD2") != nullptr);
        }
    }

    std::error_code ec2;
    fs::remove_all(g_dir, ec2);
    return testSummary("test_unplaced_list_app");
}
