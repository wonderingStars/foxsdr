/*
 * TWO FILES OF ONE PLUGIN WHOSE DECLARED NAME CHANGED BETWEEN THEM ARE ONE SECTION IN THE DECODE BANK,
 * AND DEAR IMGUI'S "PROGRAMMER ERROR" BOX HAS NOTHING TO SAY (0.99.77).
 *
 * THE REPORT (a US tester, 2026-10-13, 0.99.75): "Looks like something isn't being called out
 * correctly with the two Tetra plugins or if there is only one, it is listed twice causing the
 * error. Does this with all entries under Tetra." The DECODE bank's "Turn on and off plugins" list
 * had TWO TETRA sections - "TETRA" and "TETRA Monitor (data only, no voice)" - each with its own
 * "+ Save 931.4375 MHz as a preset" key, and ImGui's red "Programmer error: 2 visible items with
 * conflicting ID!" box was open over the waterfall. The plugin's declared name changed between its
 * 0.1.0 and 0.1.1 files; the tester had both files, and the host grouped loaded modules by declared
 * name alone, so both ran, under ONE run id (the file name without its version) that scopes the
 * bank's widget ids.
 *
 * WHAT THIS RUNS. The real cascade.exe, isolated like every test that starts the application, in one
 * viewport, with the plugins folder holding the probe module of tests/fixtures/rescan_probe_plugin.cpp
 * built twice from one source: "Rescan Probe b" 1.0.0 as probe-dup-1.0.0-abi3-win-x64.dll (the OLD file,
 * under its old name) and "Rescan Probe a" 1.1.0 as probe-dup-1.1.0-abi3-win-x64.dll (the NEW file, under
 * its new name). Two names, ONE run id, "probe-dup" - the shape of the tester's folder, with a probe in
 * place of the TETRA decoder (no TETRA files here):
 *
 *   bank      the DECODE bank with "Turn on and off plugins" open lists ONE section, the new file's
 *             (census decoders:entry:<name> once); the old name has none;
 *   hover     the pointer swept down the section's keys: ImGui reports no ID conflict
 *             (census imgui:idconflict absent; the highlighter is ON under the test hooks);
 *   fitted    the Fitted modules window shows the old file REFUSED (fitted:state:...:refused) with a
 *             REMOVE key and no STOP key, and the new file running; the log carries the host's words:
 *             "plugin: NOT loaded <old file> (Ignored: another copy of "<new name>" is installed. ...)";
 *   cleanup   with the new file recorded in installed.json (a store install) and the old one not (a hand
 *             copy), CLEAN UP OLD VERSIONS is offered (fitted:cleanup) - the old file counts as an old copy;
 *   log       a bounded run with no test hook up writes "imgui: ID-conflict box off (shipped build)", and
 *             the runs above write "on (a test hook is up)" (this file's own build decides which of the
 *             two a Debug build writes, see below).
 *
 * THE BREAK-ITS, run while writing this (0.99.77); each one's result is in the commit message:
 *   - the run-id grouping removed from resolveDuplicatePlugins: two sections (decoders:entry:<old name>
 *     present), the old file not REFUSED, the sweep reports imgui:idconflict - which is also the proof
 *     that the sweep reaches a widget and the probe can see a conflict at all;
 *   - the loser not turned off (the policy marks it and leaves it loaded): the same;
 *   - the winner chosen by file order instead of by version: the OLD file runs and the new one is refused;
 *   - the clean-up matching on declared name only: no fitted:cleanup key.
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
    std::string log;  // the run's diagnostics log (foxsdr.log)
    bool has(const std::string& name) const { return items.count(name) != 0u; }
    const Rect* rect(const std::string& name) const {
        const auto it = rects.find(name);
        return it == rects.end() ? nullptr : &it->second;
    }
};

fs::path g_dir;
std::string g_oldBuild;  // probe b, 1.0.0: declares "Rescan Probe b"
std::string g_newBuild;  // probe a, 1.1.0: declares "Rescan Probe a"

// ONE run id, "probe-dup": the file name without its version (core/plugin_run.hpp pluginRunId).
const std::string kOldFile = std::string("probe-dup-1.0.0-abi3-win-x64") + kExt;
const std::string kNewFile = std::string("probe-dup-1.1.0-abi3-win-x64") + kExt;
const char* const kOldName = "Rescan Probe b";
const char* const kNewName = "Rescan Probe a";

fs::path pluginsDir() { return g_dir / "local" / "foxsdr" / "plugins"; }

void writeFile(const fs::path& p, const std::string& body) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

// The plugins folder: the old file under its old name and the new file under its new one. `manifest` is
// installed.json's body (empty = none).
void installFixtures(const std::string& manifest) {
    std::error_code ec;
    fs::remove_all(pluginsDir(), ec);
    fs::create_directories(pluginsDir(), ec);
    fs::copy_file(g_oldBuild, pluginsDir() / kOldFile, fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    fs::copy_file(g_newBuild, pluginsDir() / kNewFile, fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    if (!manifest.empty()) { writeFile(pluginsDir() / "installed.json", manifest); }
}

// installed.json as a store install of the NEW file leaves it (no source: the FoxSDR catalogue's); the
// old file is a hand copy and has no record.
std::string storeRecordOfNewFile() {
    return "{\"schemaVersion\":1,\"plugins\":[{\"id\":\"probe-dup\",\"version\":\"1.1.0\",\"file\":\"" + kNewFile +
           "\"}]}";
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct RunOpts {
    std::string script;        // FOXSDR_INPUT_SCRIPT's body (empty = none)
    bool openFitted = false;   // also open the Fitted modules window
    bool hooks = true;         // FOXSDR_UI_CENSUS up (false: a bounded run with no test hook at all)
};

// One run of the application. With `hooks` false there is no census, so nothing is read from one: only the
// process output and the log.
Result once(const std::string& tag, int frames, const RunOpts& o = RunOpts{}) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    const fs::path diag = g_dir / (tag + "-diag");
    {
        std::error_code ec;
        fs::remove_all(diag, ec);
        fs::create_directories(diag, ec);
    }
    // railBank 1 is DECODE. FOXSDR_OPEN_DECODERS puts the rail there itself and opens the section; the
    // config says it too so the census (which cycles the banks unless it is held) has nothing to move.
    writeFile(cfg,
              std::string("{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": "
                          "\"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", \"mainView\": "
                          "\"receiver\", \"railBank\": 1, \"bandPlanOverlay\": false, \"diagnosticsEnabled\": true") +
                  (o.openFitted ? ", \"fittedModulesX\": 60, \"fittedModulesY\": 50, \"fittedModulesWidth\": 1400, "
                                  "\"fittedModulesHeight\": 880"
                                : "") +
                  " }\n");
    const std::string scriptPath = (g_dir / (tag + ".script")).string();
    writeFile(scriptPath, o.script);
    setEnv("FOXSDR_OPEN_FITTED_MODULES", o.openFitted ? "1" : "");
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", o.hooks ? census.string() : std::string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    setEnv("FOXSDR_INPUT_SCRIPT", o.script.empty() ? std::string() : scriptPath);
    setEnv("FOXSDR_SCRIPT_TRACE", (g_dir / (tag + ".trace")).string());
    r.out = run("\"" + exePath() + "\" --frames " + std::to_string(frames) + " 2>&1");
    r.log = readAll(diag / "logs" / "foxsdr.log");
    const bool rendered = r.out.find("rendered " + std::to_string(frames) + " frames") != std::string::npos;
    const bool written = !o.hooks || r.out.find("ui census written") != std::string::npos;
    if (!rendered || !written) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), r.out.c_str());
        return r;
    }
    if (o.hooks) {
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
            // The name may hold spaces ("decoders:entry:Rescan Probe a"); the four numbers are the last four.
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
    }
    r.ok = true;
    return r;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// The census lines this file is about, for the transcript.
void printCensus(const Result& r, const char* what) {
    std::printf("    %s:\n", what);
    for (const auto& kv : r.rects) {
        if (kv.first.rfind("decoders:entry:", 0) == 0) {
            std::printf("      rect %s  y %.0f..%.0f\n", kv.first.c_str(), kv.second.y0, kv.second.y1);
        }
    }
    for (const std::string& it : r.items) {
        if (it.rfind("fitted:state:probe-dup", 0) == 0 || it.rfind("imgui:", 0) == 0) {
            std::printf("      item %s\n", it.c_str());
        }
    }
}

// The pointer swept down the section's keys: one position every three frames (a hovered id is counted
// the frame after it was hovered and reported the frame after that), 6 px apart, starting just under the
// section's heading and going 150 px down - past the section's two keys (STOP and the mute tick).
std::string sweepDown(const Rect& entry) {
    std::string s;
    char buf[120];
    int frame = 14;
    for (int k = 0; k < 26; ++k, frame += 3) {
        std::snprintf(buf, sizeof(buf), "%d screen %.1f %.1f\n", frame, entry.x0 + 60.0f,
                      entry.y1 + 4.0f + 6.0f * static_cast<float>(k));
        s += buf;
    }
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("test_plugin_dup_app\n");
    // Unbuffered, so a run that a timeout stops still says how far it got.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        std::printf("usage: test_plugin_dup_app <probe b module (1.0.0)> <probe a module (1.1.0)>\n");
        return 2;
    }
    g_oldBuild = argv[1];
    g_newBuild = argv[2];
    g_dir = fs::temp_directory_path() / ("cascade-plugindup-" + std::to_string(pid()));
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
    // A fake package identity puts the plugins folder under LOCALAPPDATA (the scratch tree), not beside the
    // executable in the build tree.
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
    setEnv("RESCAN_PROBE_LOG", "");
    setEnv("CASCADE_DECODE_TEST", "");
    setEnv("CASCADE_PLUGIN_TEST", "");

    // The two files differ in name and version, and share the run id: say so before anything is judged on it.
    CHECK(kOldFile != kNewFile);

    // --- BANK: ONE SECTION FOR THE PLUGIN, THE NEW FILE'S ------------------------------------------------
    installFixtures("");
    Rect entry;
    bool haveEntry = false;
    {
        const Result r = once("bank", 40);
        CHECK(r.ok);
        if (r.ok) {
            printCensus(r, "the DECODE bank, both files in the folder");
            // The Decoders section was drawn at all (a missing bank would make every absence below pass for
            // the wrong reason).
            CHECK(r.has("section:Turn on and off plugins###decoders"));
            const Rect* n = r.rect(std::string("decoders:entry:") + kNewName);
            CHECK(n != nullptr);
            CHECK(r.rect(std::string("decoders:entry:") + kOldName) == nullptr);  // THE TESTER'S SECOND SECTION
            std::size_t entries = 0;
            for (const auto& kv : r.rects) {
                if (kv.first.rfind("decoders:entry:", 0) == 0) { ++entries; }
            }
            CHECK(entries == 1u);
            CHECK(!r.has("imgui:idconflict"));
            if (n != nullptr) {
                entry = *n;
                haveEntry = true;
                CHECK(n->x1 > n->x0 && n->y1 > n->y0);
            }
            // The host's own words, in the log, once: the old file is turned off as another copy of the
            // section that stays, and the new file's name is the one the message gives. (A log that was
            // never written would make the absence below pass for the wrong reason.)
            CHECK(!r.log.empty());
            CHECK(contains(r.log, "plugin: loaded " + std::string(kNewName) + " 1.1.0"));
            CHECK(!contains(r.log, "plugin: loaded " + std::string(kOldName)));
            // (The log cuts a line at its fixed width, so the sentence is checked as far as the file that
            // runs; the rest of the words are test_plugin_host's.)
            CHECK(contains(r.log, "plugin: NOT loaded " + kOldFile + " (Ignored: another copy of \"" + kNewName +
                                       "\" is installed. Version 1.1.0 in " + kNewFile));
            // The log says which way ImGui's highlighter went in a run with a test hook up. A Debug build
            // writes the other reason, which is also "on": this file may be built either way.
#ifdef NDEBUG
            CHECK(contains(r.log, "imgui: ID-conflict box on (a test hook is up)"));
#else
            CHECK(contains(r.log, "imgui: ID-conflict box on (a debug build)"));
#endif
        }
    }

    // --- HOVER: IMGUI REPORTS NO CONFLICT WHILE THE POINTER SWEEPS THE SECTION'S KEYS -------------------
    if (haveEntry) {
        RunOpts o;
        o.script = sweepDown(entry);
        const Result r = once("hover", 110, o);
        CHECK(r.ok);
        if (r.ok) {
            printCensus(r, "the sweep down the section's keys");
            CHECK(r.rect(std::string("decoders:entry:") + kNewName) != nullptr);
            CHECK(r.rect(std::string("decoders:entry:") + kOldName) == nullptr);
            CHECK(!r.has("imgui:idconflict"));
        }
    }

    // --- FITTED: THE OLD FILE IS REFUSED, WITH REMOVE AND NO STOP; THE NEW ONE IS NOT -------------------
    {
        RunOpts o;
        o.openFitted = true;
        const Result r = once("fitted", 50, o);
        CHECK(r.ok);
        if (r.ok) {
            printCensus(r, "the Fitted modules window");
            CHECK(r.has("fitted:state:" + kOldFile + ":refused"));
            CHECK(!r.has("fitted:state:" + kNewFile + ":refused"));
            // The new file has a state of its own (it is running: fed, idle or not decoding), so a row for it
            // was drawn at all.
            bool newRow = false;
            for (const std::string& it : r.items) {
                if (it.rfind("fitted:state:" + kNewFile + ":", 0) == 0) { newRow = true; }
            }
            CHECK(newRow);
            // The old file's row offers REMOVE and no STOP/START (it is not running); the new one offers a key.
            CHECK(r.rect("fitted:row:" + kOldFile + ":remove") != nullptr);
            CHECK(r.rect("fitted:row:" + kOldFile + ":key") == nullptr);
            CHECK(r.rect("fitted:row:" + kNewFile + ":key") != nullptr);
            // Nothing recorded in installed.json: no store install to clean up after.
            CHECK(r.rect("fitted:cleanup") == nullptr);
        }
    }

    // --- CLEANUP: THE OLD FILE IS AN OLD COPY THE CLEAN-UP COUNTS -----------------------------------------
    {
        installFixtures(storeRecordOfNewFile());
        RunOpts o;
        o.openFitted = true;
        const Result r = once("cleanup", 50, o);
        CHECK(r.ok);
        if (r.ok) {
            std::printf("    cleanup (new file recorded, old file a hand copy): fitted:cleanup %s\n",
                        r.rect("fitted:cleanup") != nullptr ? "offered" : "NOT offered");
            CHECK(r.has("fitted:state:" + kOldFile + ":refused"));
            CHECK(r.rect("fitted:cleanup") != nullptr);
        }
        installFixtures("");
    }

    // --- LOG: A BOUNDED RUN WITH NO TEST HOOK IS A SHIPPED RUN, AND ImGui'S BOX IS OFF -----------------
    {
        RunOpts o;
        o.hooks = false;
        const Result r = once("nohooks", 12, o);
        CHECK(r.ok);
        if (r.ok) {
#ifdef NDEBUG
            std::printf("    no hook up: %s\n",
                        contains(r.log, "imgui: ID-conflict box off (shipped build)") ? "imgui: ID-conflict box off (shipped build)"
                                                                                      : "(the off line is missing)");
            CHECK(contains(r.log, "imgui: ID-conflict box off (shipped build)"));
            CHECK(!contains(r.log, "imgui: ID-conflict box on"));
#else
            CHECK(contains(r.log, "imgui: ID-conflict box on (a debug build)"));
#endif
        }
    }

    std::error_code ec2;
    fs::remove_all(g_dir, ec2);
    return testSummary("test_plugin_dup_app");
}
