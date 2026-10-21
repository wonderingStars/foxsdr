/*
 * A PANEL ROW CAN BE A KEY BOUND TO ONE OF THE PLUGIN'S COMMANDS (0.99.77), END TO END.
 *
 * THE TESTER'S WORDS (a US tester via the owner, 2026-10-13): "If I click on HD2 on the screen
 * below, it should select that program, instead of having to go into the plugin settings and
 * change the Programme to 1." The IBOC Radio plugin lists HD1..HD8 in its panel; the plugin ABI
 * gave a panel no way to send a press back. CASCADE_ROW_FLAG_KEY (the command id in bits 8..15 of
 * a row's flags) is that way.
 *
 * Runs cascade itself, isolated like every test that starts the application, in ONE viewport
 * (FOXSDR_SINGLE_VIEWPORT=1, so the UI census reads every rectangle in the main window), with the
 * fixture of tests/fixtures/row_keys_plugin.cpp fitted and pinned to run - a real plugin module
 * that reads CASCADE_STATE_ROW_KEYS from get_state, registers its commands with add_command, marks
 * its HD1 and HD2 rows as keys (one more row with an id it never registered, and an old-style row),
 * takes the presses with poll_command and writes each one it is handed to a file. The DECODE bank
 * is put up (FOXSDR_OPEN_DECODERS) and a scripted pointer presses the fixture's row on the rail to
 * open its panel window, then presses rows in it:
 *
 *   layout    no click: the panel's rail row is there, and the window is not open - no pressable
 *             row is in the census (so the absences below are not "the window never opened");
 *   open      the window is open: the census lists HD1 and HD2 as keys (panelrow:<plugin>:<id>:key),
 *             lists the row bound to the id the plugin never registered as TEXT
 *             (panelrow:<plugin>:99:text, no :key), and lists nothing else - the old-style row is
 *             not mentioned; the three sit one below another;
 *   press     a click on HD2 hands the plugin id 12 through poll_command, once - the file holds
 *             exactly "press 12"; a click on HD1 then HD2 hands it 11 then 12, in that order;
 *   not keys  a click on the row bound to the unregistered id, and a click on the old-style row,
 *             hand it nothing (the same one-row shift from HD1 lands on HD2 and presses it, so the
 *             click on the old-style row is a click in the table); a run that ends before the
 *             click is released hands it nothing;
 *   memory    the same rows beneath an INSTRUMENT's face (the host draws them with the same code):
 *             the instrument's window is opened from its own row, the census lists its rows as
 *             "<plugin>:memory", and a click on HD2 there hands the plugin id 12 too;
 *   plate     with the fixture registering sixteen commands (the cap from 0.99.77, eight before),
 *             its page in Fitted modules draws sixteen command keys, two to a line, eight lines,
 *             and not a seventeenth.
 *
 * NO NETWORK: every telemetry address is a discard port and the update check is off.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <chrono>
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
    std::string out;        // what the process printed
    std::string presses;    // what the fixture wrote to its log: one "press <id>" line per id it was handed
    bool has(const std::string& name) const { return items.count(name) != 0u; }
    const Rect* rect(const std::string& name) const {
        const auto it = rects.find(name);
        return it == rects.end() ? nullptr : &it->second;
    }
    // Every census item that starts with `prefix`, whole.
    std::set<std::string> itemsStarting(const std::string& prefix) const {
        std::set<std::string> found;
        for (const std::string& s : items) {
            if (s.rfind(prefix, 0) == 0) { found.insert(s); }
        }
        return found;
    }
    static std::string join(const std::set<std::string>& s) {
        std::string out;
        for (const std::string& v : s) { out += (out.empty() ? "" : ", ") + v; }
        return out;
    }
};

fs::path g_dir;
std::string g_fixtureDll;  // the row-keys fixture, copied into the scratch plugins folder
const std::string kFile = "row-keys-fixture";  // the module file name, less its extension
const std::string kPlugin = "Row Keys Fixture";  // its declared name
const std::string kPanelRail = "switchrow:Row Keys Panel";  // its row on the rail: the panel's title
const std::string kPanelWindow = "panel:window:" + kPlugin;  // its window, once it is open
const std::string kInstrumentRail = "switchrow:Row Keys Instrument";  // its instrument's row

fs::path pluginsDir() { return g_dir / "local" / "foxsdr" / "plugins"; }

void writeFile(const fs::path& p, const std::string& body) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void installFixture() {
    std::error_code ec;
    fs::remove_all(pluginsDir(), ec);
    fs::create_directories(pluginsDir(), ec);
    fs::copy_file(g_fixtureDll, pluginsDir() / (kFile + kExt), fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
}

// One run of the application. `script` is FOXSDR_INPUT_SCRIPT (empty = none). `many` makes the
// fixture register sixteen commands and `fitted` opens the Fitted modules window, for the plate;
// otherwise the DECODE bank is put up, for the rail row that opens the panel.
Result once(const std::string& tag, int frames, const std::string& script, bool many = false,
            bool fitted = false) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    const fs::path diag = g_dir / (tag + "-diag");
    const fs::path log = g_dir / (tag + ".presses");
    {
        std::error_code ec;
        fs::remove_all(diag, ec);
        fs::create_directories(diag, ec);
        fs::remove(log, ec);
    }
    // railBank 1 is DECODE. The module is pinned to run ("always"), so its panel is polled and its
    // commands registered from the first frame whatever is on screen. The Fitted modules window's
    // rectangle is pinned so the module's row is where the first launch found it.
    writeFile(cfg,
              "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": "
              "\"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", \"mainView\": "
              "\"receiver\", \"railBank\": 1, \"bandPlanOverlay\": false, "
              "\"fittedModulesX\": 60, \"fittedModulesY\": 50, \"fittedModulesWidth\": 1400, "
              "\"fittedModulesHeight\": 880, \"pluginRun\": { \"" +
                  kFile + "\": \"always\" } }\n");
    const std::string scriptPath = (g_dir / (tag + ".script")).string();
    writeFile(scriptPath, script);
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    setEnv("FOXSDR_INPUT_SCRIPT", script.empty() ? std::string() : scriptPath);
    setEnv("FOXSDR_SCRIPT_TRACE", (g_dir / (tag + ".trace")).string());
    setEnv("FOXSDR_OPEN_DECODERS", fitted ? "" : "1");
    setEnv("FOXSDR_OPEN_FITTED_MODULES", fitted ? "1" : "");
    setEnv("ROW_KEYS_FIXTURE_LOG", log.string());
    setEnv("ROW_KEYS_FIXTURE_MANY", many ? "1" : "");
    std::printf("  run %s: %d frames ...\n", tag.c_str(), frames);
    const auto started = std::chrono::steady_clock::now();
    r.out = run("\"" + exePath() + "\" --frames " + std::to_string(frames) + " 2>&1");
    std::printf("  run %s: ended after %.1f s\n", tag.c_str(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    r.presses = readFile(log);
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
        // The name may hold spaces ("panelrow:Row Keys Fixture:12:key"); the four numbers are the
        // last four tokens.
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

// A click at the point `fx` of the way across `at` (a row is as wide as its table, and the middle
// of a two-column table is the column border, where a press starts a resize instead): the pointer
// arrives at `frame`, goes down two frames later and up two after that - the release at frame + 4
// is what presses it.
std::string click(int frame, const Rect& at, float fx = 0.5f) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d screen %.1f %.1f\n%d down\n%d up\n", frame,
                  at.x0 + fx * (at.x1 - at.x0), at.cy(), frame + 2, frame + 4);
    return buf;
}

// Real time before a step: a bounded run draws as fast as it can, and the module has to have been
// polled (and its rows drawn) before they are pressed.
std::string wait(int frame, int ms) { return std::to_string(frame) + " sleep " + std::to_string(ms) + "\n"; }

std::string keyItem(const std::string& id) { return "panelrow:" + kPlugin + ":" + id + ":key"; }
std::string textItem(const std::string& id) { return "panelrow:" + kPlugin + ":" + id + ":text"; }

}  // namespace

int main(int argc, char** argv) {
    // UNBUFFERED, so that a run stopped by the timeout still says how far it got.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_row_keys_app\n");
    if (argc < 2) {
        std::printf("usage: test_row_keys_app <row keys fixture module>\n");
        return 2;
    }
    g_fixtureDll = argv[1];
    g_dir = fs::temp_directory_path() / ("cascade-rowkeys-" + std::to_string(pid()));
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
    setEnv("FOXSDR_PATCH_START", "");
    setEnv("FOXSDR_PATCH_FILE", "");
    setEnv("FOXSDR_OPEN_PLUGIN_STORE", "");
    setEnv("FOXSDR_OPEN_DECODER_OUTPUT", "");
    setEnv("FOXSDR_OPEN_MAP", "");
    setEnv("FOXSDR_PRESS_PRESET", "");
    setEnv("FOXSDR_SHOT_DIR", "");
    setEnv("FOXSDR_SHOT_AT_FRAME", "");
    setEnv("FOXSDR_WINDOW_POS", "");
    setEnv("CASCADE_DECODE_TEST", "");
    setEnv("CASCADE_PLUGIN_TEST", "");
    setEnv("ROW_KEYS_FIXTURE_NO_STATE_CHECK", "");
    installFixture();

    // --- LAYOUT: the rail row is there, the window is not open, nothing is pressable ------------------
    Rect rail;
    Rect instrumentRail;
    bool haveRail = false;
    {
        const Result r = once("layout", 60, wait(30, 400));
        CHECK(r.ok);
        if (r.ok) {
            const Rect* row = r.rect(kPanelRail);
            const Rect* irow = r.rect(kInstrumentRail);
            CHECK(row != nullptr);
            CHECK(irow != nullptr);
            if (row != nullptr && irow != nullptr) {
                rail = *row;
                instrumentRail = *irow;
                haveRail = true;
                std::printf("    the panel's row on the rail: x %.0f..%.0f y %.0f..%.0f\n", row->x0,
                            row->x1, row->y0, row->y1);
                CHECK(row->x1 > row->x0 && row->y1 > row->y0);
                CHECK(row->x0 >= 0.0f && row->y0 >= 0.0f && row->x1 <= 1600.0f && row->y1 <= 1000.0f);
            }
            // The window is closed: it is not on screen, and no row of it was drawn, pressable or not.
            CHECK(r.rect(kPanelWindow) == nullptr);
            CHECK(r.itemsStarting("panelrow:").empty());
        }
    }
    if (!haveRail) {
        std::error_code ec2;
        fs::remove_all(g_dir, ec2);
        return testSummary("test_row_keys_app");
    }
    // The rail row is pressed at frame 15 and the window is up and polled well before frame 40.
    const std::string openScript = click(15, rail) + wait(25, 500);

    // --- OPEN: the window is up; the census lists the keys, and the row that is only text ------------
    Rect r11, r12, r99;
    bool haveRows = false;
    {
        const Result r = once("open", 80, openScript);
        CHECK(r.ok);
        if (r.ok) {
            // The plugin is running with its panel up and five rows polled, and the window the
            // click opened is on screen: whatever the census lists below, it is not "the window
            // never opened" or "the plugin never ran".
            CHECK(r.has("panel:rows:" + kPlugin + ":5"));
            CHECK(r.rect(kPanelWindow) != nullptr);
            const std::set<std::string> rows = r.itemsStarting("panelrow:");
            std::printf("    panel rows in the census: %s\n", Result::join(rows).c_str());
            // The plugin SAW the bit (it marks its rows as keys only when get_state says the host
            // draws them), so this also proves CASCADE_STATE_ROW_KEYS reached it in the real app.
            CHECK(r.has(keyItem("11")));
            CHECK(r.has(keyItem("12")));
            // The id the plugin never registered is drawn as text, and is not a key.
            CHECK(r.has(textItem("99")));
            CHECK(!r.has(keyItem("99")));
            CHECK(!r.has(textItem("11")) && !r.has(textItem("12")));
            // And that is all: the heading that carries a key flag and the old-style row are not
            // keys and not mentioned.
            CHECK(rows.size() == 3u);
            const Rect* a = r.rect(keyItem("11"));
            const Rect* b = r.rect(keyItem("12"));
            const Rect* c = r.rect(textItem("99"));
            CHECK(a != nullptr && b != nullptr && c != nullptr);
            if (a != nullptr && b != nullptr && c != nullptr) {
                r11 = *a;
                r12 = *b;
                r99 = *c;
                haveRows = true;
                std::printf("    HD1 y %.0f..%.0f, HD2 y %.0f..%.0f, HD3 (unregistered) y %.0f..%.0f, x %.0f..%.0f\n",
                            a->y0, a->y1, b->y0, b->y1, c->y0, c->y1, a->x0, a->x1);
                // One below another, in the order the plugin gave them.
                CHECK(b->y0 > a->y0 && c->y0 > b->y0);
                // The whole row is the key: the two keys' rectangles are the table's width, not
                // one word's, and are the same; the third row (text, so its rectangle is its first
                // cell's) sits inside that width.
                CHECK(a->x1 - a->x0 > 200.0f);
                CHECK(a->x0 == b->x0 && a->x1 == b->x1);
                CHECK(c->x0 >= a->x0 && c->x1 <= a->x1);
                CHECK(a->y1 > a->y0);
            }
            // Nothing has been pressed yet.
            CHECK(r.presses.empty());
        }
    }
    if (!haveRows) {
        std::error_code ec2;
        fs::remove_all(g_dir, ec2);
        return testSummary("test_row_keys_app");
    }

    // --- PRESS: a click on HD2 hands the plugin id 12, once -----------------------------------------
    {
        const Result r = once("press12", 120, openScript + click(60, r12, 0.2f));
        CHECK(r.ok);
        if (r.ok) {
            std::printf("    the plugin was handed: [%s]\n", r.presses.c_str());
            CHECK(r.presses == "press 12\n");
        }
    }
    // HD1 then HD2: both ids, whole, in the order they were pressed.
    {
        const Result r = once("press-both", 160, openScript + click(60, r11, 0.2f) + click(90, r12, 0.2f));
        CHECK(r.ok);
        if (r.ok) {
            std::printf("    the plugin was handed: [%s]\n", r.presses.c_str());
            CHECK(r.presses == "press 11\npress 12\n");
        }
    }

    // --- NOT KEYS: nothing reaches the plugin ----------------------------------------------------------
    {
        // The row bound to an id the plugin never registered.
        const Result r = once("unregistered", 120, openScript + click(60, r99, 0.2f));
        CHECK(r.ok);
        if (r.ok) {
            // The window was open and the row was there to be pressed (the census saw it).
            CHECK(r.has(textItem("99")));
            std::printf("    a click on the row bound to an unregistered id: [%s]\n", r.presses.c_str());
            CHECK(r.presses.empty());
        }
    }
    {
        // The old-style row: HD4 sits one row below HD3, so one row's pitch (HD2 to HD1) below it.
        const float pitch = r12.y0 - r11.y0;
        CHECK(pitch > 4.0f);
        const auto below = [pitch](Rect at) {
            at.y0 += pitch;
            at.y1 += pitch;
            return at;
        };
        // CONTROL FOR THE ARITHMETIC: the same shift from HD1 lands in HD2's row, and presses it. So
        // the click on the old-style row below really is a click one row down, in the table.
        const Result shifted = once("shifted", 120, openScript + click(60, below(r11), 0.2f));
        CHECK(shifted.ok);
        if (shifted.ok) {
            std::printf("    a click one row below HD1: [%s]\n", shifted.presses.c_str());
            CHECK(shifted.presses == "press 12\n");
        }
        const Rect plain = below(r99);
        const Result r = once("plain", 120, openScript + click(60, plain, 0.2f));
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.has(keyItem("12")));
            std::printf("    a click on the old-style row: [%s]\n", r.presses.c_str());
            CHECK(r.presses.empty());
        }
    }
    {
        // CONTROL: the run ends before the click's release (frame 64), so nothing is pressed.
        const Result r = once("press-before", 63, openScript + click(60, r12, 0.2f));
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.has(keyItem("12")));
            CHECK(r.presses.empty());
        }
    }

    // --- INSTRUMENT MEMORY: the host's other table of rows is the same drawing, the same key ------------
    // The fixture's instrument (the generic face, nothing to read) has the same five rows as its
    // memory beneath the face. Its window is opened from its own row on the rail, the panel's is not
    // open, and a click on HD2 there hands the plugin 12 - from the instrument's rows, which the
    // census calls "<plugin>:memory".
    {
        const std::string memKey = "panelrow:" + kPlugin + ":memory:12:key";
        const std::string memScript = click(15, instrumentRail) + wait(25, 500);
        const Result o = once("memory-open", 80, memScript);
        CHECK(o.ok);
        const Rect* m12 = o.ok ? o.rect(memKey) : nullptr;
        if (o.ok) {
            const std::set<std::string> rows = o.itemsStarting("panelrow:");
            std::printf("    instrument memory rows in the census: %s\n", Result::join(rows).c_str());
            CHECK(o.has("panelrow:" + kPlugin + ":memory:11:key"));
            CHECK(o.has(memKey));
            CHECK(o.has("panelrow:" + kPlugin + ":memory:99:text"));
            // The panel's window is NOT open in this run: none of the rows the census holds is its.
            CHECK(o.rect(kPanelWindow) == nullptr);
            CHECK(!o.has(keyItem("12")));
            CHECK(rows.size() == 3u);
        }
        CHECK(m12 != nullptr);
        if (m12 != nullptr) {
            const Result p = once("memory-press", 120, memScript + click(60, *m12, 0.2f));
            CHECK(p.ok);
            if (p.ok) {
                std::printf("    a click on HD2 under the instrument's face: [%s]\n", p.presses.c_str());
                CHECK(p.presses == "press 12\n");
            }
        }
    }

    // --- PLATE: sixteen commands are sixteen keys on the module's page ------------------------------
    // The module's page in Fitted modules (open it from its row) draws the module's own command
    // keys, two to a line; the cap on a plugin's commands went from eight to sixteen, and the page
    // must lay all of them out.
    {
        const std::string rowName = "fitted:row:" + kFile + kExt + ":name";
        const Result l = once("plate-rows", 80, wait(30, 800), true, true);
        CHECK(l.ok);
        const Rect* name = l.ok ? l.rect(rowName) : nullptr;
        CHECK(name != nullptr);
        if (name != nullptr) {
            const Result p = once("plate", 120, wait(30, 800) + click(40, *name), true, true);
            CHECK(p.ok);
            if (p.ok) {
                CHECK(p.has("fitted:page:" + kFile + kExt));  // the page IS open...
                std::set<float> lines;
                bool allThere = true;
                std::vector<Rect> keys;
                for (int i = 0; i < 16; ++i) {
                    const Rect* k = p.rect("fitted:page:cmd:" + std::to_string(i));
                    allThere = allThere && k != nullptr && k->x1 > k->x0 && k->y1 > k->y0;
                    if (k != nullptr) {
                        keys.push_back(*k);
                        lines.insert(k->y0);
                    }
                }
                std::printf("    command keys drawn on the module's page: %zu of 16, on %zu lines\n",
                            keys.size(), lines.size());
                CHECK(allThere);
                CHECK(keys.size() == 16u);
                CHECK(lines.size() == 8u);  // two to a line, as with eight, and the lines keep coming
                if (keys.size() == 16u) {
                    for (std::size_t i = 0; i + 1 < keys.size(); i += 2) {
                        CHECK(keys[i].y0 == keys[i + 1].y0);  // a pair shares a line...
                        CHECK(keys[i + 1].x0 > keys[i].x1);   // ...side by side
                    }
                    for (std::size_t i = 2; i < keys.size(); ++i) {
                        CHECK(keys[i].y0 > keys[i - 2].y0);   // and the next pair is lower
                    }
                }
                CHECK(p.rect("fitted:page:cmd:16") == nullptr);  // the cap is sixteen
            }
        }
    }

    std::error_code ec2;
    fs::remove_all(g_dir, ec2);
    return testSummary("test_row_keys_app");
}
