/*
 * A DECODER'S SETTINGS FOLD INTO A CLOSED "Plugin settings" ROW ON THE DECODE BANK (0.99.74).
 * Runs cascade itself, isolated like every test that starts the application, in ONE viewport
 * (FOXSDR_SINGLE_VIEWPORT=1, so the UI census reads every rectangle in the main window), with
 * the settings fixture of tests/fixtures/settings_ui_plugin.cpp fitted - a real text decoder
 * that declares a settings form (three fields the host can store) and a panel - and the DECODE
 * bank put up with the "Turn on and off plugins" section open (FOXSDR_OPEN_DECODERS):
 *
 *   default   the tester's request, in his words (2026-10-12): "For any of the decoders that
 *             have settings, could those settings be put in a collapsed settings sub menu? ...
 *             if I am scrolling down through the various decoders, you get to the Demod
 *             Analyzer and have to scroll past a bunch of settings." Left alone, the plugin's
 *             row draws ONE folded "Plugin settings" line: the census says folded
 *             (decoders:settings:folded), never open, and the node's rectangle is reported;
 *   open      a click on that line unfolds it: the census says open, and the row that follows
 *             the plugin on the rail (the fixture's panel window, "switchrow:<panel>") sits
 *             lower than it did folded by about the height of the form - the form's three
 *             fields really are in the layout, and in the folded run they are not;
 *   again     a second click folds it back: the same row is back where it was folded, so the
 *             node is a toggle and not a one-way door.
 *
 * WHAT MOVES WITH THE DEFECT. Before the fold the form was drawn inline, always: there was no
 * folded note, no node rectangle, and the panel row sat below the whole form in the default
 * run. The assertions on the note, the rectangle and the row's height all fail on that build.
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
    float cx() const { return 0.5f * (x0 + x1); }
    float cy() const { return 0.5f * (y0 + y1); }
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
};

fs::path g_dir;
std::string g_fixtureDll;  // the settings fixture, copied into the scratch plugins folder

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
    fs::copy_file(g_fixtureDll, pluginsDir() / (std::string("settings_ui_fixture") + kExt),
                  fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
}

// One run of the application, `script` being FOXSDR_INPUT_SCRIPT (empty = none).
Result once(const std::string& tag, int frames, const std::string& script) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    const fs::path diag = g_dir / (tag + "-diag");
    {
        std::error_code ec;
        fs::remove_all(diag, ec);
        fs::create_directories(diag, ec);
    }
    // railBank 1 is DECODE. FOXSDR_OPEN_DECODERS puts the rail there itself and opens the
    // section; the config says it too so the census (which cycles the banks unless it is held)
    // has nothing to move.
    writeFile(cfg,
              "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": "
              "\"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", \"mainView\": "
              "\"receiver\", \"railBank\": 1, \"bandPlanOverlay\": false }\n");
    const std::string scriptPath = (g_dir / (tag + ".script")).string();
    writeFile(scriptPath, script);
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    setEnv("FOXSDR_INPUT_SCRIPT", script.empty() ? std::string() : scriptPath);
    setEnv("FOXSDR_SCRIPT_TRACE", (g_dir / (tag + ".trace")).string());
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
        // The name may hold spaces ("switchrow:Settings Fixture Panel"); the four numbers are
        // the last four tokens.
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

// A click at the centre of `at`: the pointer arrives at `frame`, goes down two frames later and
// up two after that - the release at frame + 4 is what presses the node.
std::string click(int frame, const Rect& at) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d screen %.1f %.1f\n%d down\n%d up\n", frame, at.cx(), at.cy(),
                  frame + 2, frame + 4);
    return buf;
}

const char* const kNode = "decoders:settingsnode";
const char* const kPanelRow = "switchrow:Settings Fixture Panel";

}  // namespace

int main(int argc, char** argv) {
    std::printf("test_decoder_settings_fold_app\n");
    if (argc < 2) {
        std::printf("usage: test_decoder_settings_fold_app <settings fixture module>\n");
        return 2;
    }
    g_fixtureDll = argv[1];
    g_dir = fs::temp_directory_path() / ("cascade-decsettings-" + std::to_string(pid()));
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
    setEnv("FOXSDR_CENSUS_HOLD_BANK", "1");
    // The DECODE bank up, "Turn on and off plugins" open: the only way a bounded run reaches it.
    setEnv("FOXSDR_OPEN_DECODERS", "1");
    setEnv("FOXSDR_PATCH_START", "");
    setEnv("FOXSDR_PATCH_FILE", "");
    setEnv("FOXSDR_OPEN_PLUGIN_STORE", "");
    setEnv("FOXSDR_OPEN_FITTED_MODULES", "");
    setEnv("FOXSDR_OPEN_DECODER_OUTPUT", "");
    setEnv("FOXSDR_PRESS_PRESET", "");
    setEnv("FOXSDR_SHOT_DIR", "");
    setEnv("FOXSDR_SHOT_AT_FRAME", "");
    setEnv("FOXSDR_WINDOW_POS", "");
    setEnv("CASCADE_DECODE_TEST", "");
    setEnv("CASCADE_PLUGIN_TEST", "");
    installFixture();

    // --- DEFAULT: folded, and the form is not in the layout ----------------------------------------
    Rect node;
    float foldedRowY = 0.0f;
    bool haveFolded = false;
    {
        const Result r = once("default", 40, "");
        CHECK(r.ok);
        if (r.ok) {
            // The Decoders section was drawn at all (a missing rail would make every absence
            // below pass for the wrong reason).
            CHECK(r.has("section:Turn on and off plugins###decoders"));
            CHECK(r.has("decoders:settings:folded"));
            CHECK(!r.has("decoders:settings:open"));
            const Rect* n = r.rect(kNode);
            CHECK(n != nullptr);
            const Rect* row = r.rect(kPanelRow);
            CHECK(row != nullptr);
            if (n != nullptr) {
                node = *n;
                std::printf("    folded: node x %.0f..%.0f y %.0f..%.0f\n", n->x0, n->x1, n->y0, n->y1);
                // On screen, so a click can reach it, and not a degenerate rectangle.
                CHECK(n->x1 > n->x0 && n->y1 > n->y0);
                CHECK(n->x0 >= 0.0f && n->y0 >= 0.0f && n->x1 <= 1600.0f && n->y1 <= 1000.0f);
            }
            if (row != nullptr) {
                foldedRowY = row->y0;
                haveFolded = true;
                std::printf("    folded: the panel row after the plugin sits at y %.0f\n", row->y0);
            }
        }
    }

    // --- OPEN: one click unfolds it, and the form takes its height in the layout --------------------
    if (haveFolded) {
        const Result r = once("open", 40, click(15, node));
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.has("decoders:settings:open"));
            const Rect* row = r.rect(kPanelRow);
            CHECK(row != nullptr);
            if (row != nullptr) {
                const float grew = row->y0 - foldedRowY;
                std::printf("    open: the panel row sits at y %.0f (%.0f lower than folded)\n", row->y0, grew);
                // Three storable fields (callsign, power, report), each a label and a box or a
                // checkbox: well over 60 px at the smallest type the app draws.
                CHECK(grew > 60.0f);
            }
        }

        // --- AGAIN: a second click folds it back ----------------------------------------------------
        const Result r2 = once("again", 60, click(15, node) + click(35, node));
        CHECK(r2.ok);
        if (r2.ok) {
            CHECK(r2.has("decoders:settings:open"));    // it was open between the two clicks
            CHECK(r2.has("decoders:settings:folded"));  // and folded before the first and after the second
            const Rect* row = r2.rect(kPanelRow);
            CHECK(row != nullptr);
            if (row != nullptr) {
                std::printf("    folded again: the panel row sits at y %.0f (folded first: %.0f)\n", row->y0,
                            foldedRowY);
                CHECK(std::fabs(row->y0 - foldedRowY) <= 1.0f);
            }
        }
    }

    std::error_code ec2;
    fs::remove_all(g_dir, ec2);
    return testSummary("test_decoder_settings_fold_app");
}
