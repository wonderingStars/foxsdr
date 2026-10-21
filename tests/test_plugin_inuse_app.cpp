/*
 * A FITTED PLUGIN RUNS ONLY WHILE IT IS USED (0.99.73, core/plugin_run.hpp).
 *
 * Runs cascade itself, isolated like every test that starts the application, in ONE viewport
 * (FOXSDR_SINGLE_VIEWPORT=1) with REAL plugin modules in a scratch plugins folder, and reads what
 * happened to the modules from the modules themselves:
 *
 *   - tests/fixtures/rescan_probe_plugin.cpp (a TEXT decoder, "Rescan Probe a") and
 *     tests/fixtures/image_probe_plugin.cpp (a PICTURE decoder, "Image Probe") each append a line to
 *     the file RESCAN_PROBE_LOG names whenever the host maps them, unmaps them, creates one of their
 *     decoders and destroys one. So "no create at start" is not what the application SAYS it did:
 *     it is what the module saw.
 *
 * WHAT IS PROVED, scenario by scenario (each one a real process):
 *
 *   start      two plugins fitted, nothing open: the modules are mapped and NEITHER decoder is
 *              created; the Fitted modules window letters both IDLE (the state, the chip count) and
 *              the log has no "woke" line;
 *   row        a scripted click on the picture decoder's window row on the rail creates exactly
 *              ITS decoder within a few frames of the press (and not the text decoder's), and the
 *              log says `plugin: Image Probe woke - window open`; the control run that stops
 *              before the press creates nothing;
 *   output     the Decoder output window opened by the capture seam wakes the TEXT decoder, and
 *              only it (`woke - decoder output window open`);
 *   close      the window closed again lets the plugin go dormant after the interval (shortened
 *              by FOXSDR_DORMANT_AFTER_MS, which the run states in full), the log says
 *              `dormant after 30 s without a use`, and opening the window AGAIN creates a second
 *              decoder - which could only happen if the first had really been destroyed;
 *   kept       the same run with the window left open never goes dormant;
 *   start key  START on an idle row pins it (ALWAYS): the saved config names the plugin's ID, the
 *              next launch creates its decoder with nothing open and no "woke" line, and it does
 *              not go dormant however long the run is;
 *   tick       the KEEP RUNNING tick on the module's page does the same, and unticking it takes
 *              the entry out again;
 *   preset     a plugin with a preset and a window (the preset probe): PRESSING the preset pins it
 *              (saved, never dormant however long the run), while the tune that merely OPENING its
 *              window does for you does not - the open window wakes it, the closed one lets it go,
 *              and nothing is saved;
 *   migrate    a config carrying the OLD file-name stop list stops the plugin by its ID - here a
 *              DIFFERENT version of the file - creates nothing, and the next save carries the
 *              stop in `pluginRun` and an empty `pluginsStopped`.
 *
 * AND THE FITTED MODULES LIST (0.99.77), which the owner and a US tester asked for in these words
 * (2026-10-13): "when you open the plugins Fitted modules window, when I double click on the plugin
 * it should show the plugin window", and "I really like how the store groups plugins by function
 * ... Implement this sorting on the Plugins panel and the 'Turn on and off plugins' menu as well":
 *
 *   groups     the three fitted modules, two of them known to a kept catalogue (the picture decoder as
 *              AIRCRAFT, the text decoder as VOICE AND DATA) and one side-loaded: the Fitted modules
 *              window lists them under the store's headings in the store's order, OTHER last, and the
 *              DECODE bank's "Turn on and off plugins" section does the same; with no catalogue kept
 *              neither has a heading and the Fitted modules list is the name-sorted list it was;
 *   dblclick   a double-click on a row's empty part, and on its name, opens the picture decoder's own
 *              window and wakes the plugin (`woke - window open`, one decoder created, nothing
 *              pinned), and the list comes back; the text decoder, which has no window of its own,
 *              gets its page; a stopped plugin gets its page and no window.
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

#include "core/plugin_abi.h"
#include "core/plugin_repo.hpp"
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
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
    std::string out;     // what the process printed
    std::string log;     // the diagnostic log it wrote
    std::string probe;   // what the modules wrote about themselves
    std::string config;  // the settings file as the run left it
    bool has(const std::string& item) const { return items.count(item) != 0; }
    const Rect* rect(const std::string& name) const {
        const auto it = rects.find(name);
        return it == rects.end() ? nullptr : &it->second;
    }
    // How many times the module `id` ("a" for the text decoder, "image" for the picture one) wrote
    // `event` (attach, detach, create, destroy).
    int count(const char* id, const char* event) const {
        const std::string want = std::string(id) + " " + event;
        int n = 0;
        std::istringstream in(probe);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') { line.pop_back(); }
            if (line == want) { ++n; }
        }
        return n;
    }
    bool logHas(const std::string& needle) const { return log.find(needle) != std::string::npos; }
    int logCount(const std::string& needle) const {
        int n = 0;
        for (std::size_t at = log.find(needle); at != std::string::npos; at = log.find(needle, at + 1)) { ++n; }
        return n;
    }
};

fs::path g_dir;
std::string g_textDll;   // the text decoder fixture
std::string g_imageDll;  // the picture decoder fixture
std::string g_presetDll; // the picture decoder that also has a preset

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

std::string readTree(const fs::path& dir) {
    std::string all;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) { all += readFile(it->path()) + "\n"; }
    }
    return all;
}

// THE PLUGINS FOLDER AS THE NEXT RUN FINDS IT. `textFile` and `imageFile` are the FILE NAMES the
// two fixture modules are fitted under ("" = not fitted): the file name is the plugin's identity
// for the host, and the id is that name less its extension and version.
void installPlugins(const std::string& textFile, const std::string& imageFile,
                    const std::string& presetFile = std::string()) {
    std::error_code ec;
    fs::remove_all(pluginsDir(), ec);
    fs::create_directories(pluginsDir(), ec);
    if (!presetFile.empty()) {
        fs::copy_file(g_presetDll, pluginsDir() / (presetFile + kExt), fs::copy_options::overwrite_existing, ec);
        CHECK(!ec);
    }
    if (!textFile.empty()) {
        fs::copy_file(g_textDll, pluginsDir() / (textFile + kExt), fs::copy_options::overwrite_existing, ec);
        CHECK(!ec);
    }
    if (!imageFile.empty()) {
        fs::copy_file(g_imageDll, pluginsDir() / (imageFile + kExt), fs::copy_options::overwrite_existing, ec);
        CHECK(!ec);
    }
}

// A CATALOGUE THAT KNOWS TWO OF THE THREE FITTED MODULES (0.99.77), for the runs that look at how the
// lists are grouped: the picture decoder is "grp-image" in AIRCRAFT and the text decoder "grp-text" in
// VOICE AND DATA (the catalogue's `broadcast`); the preset probe is in neither the records nor the
// catalogue, like a module dropped into the folder by hand. The kept copy is what the application loads
// at start-up; the same text is the local index the config names, so nothing is ever fetched.
std::string catalogueEntry(const std::string& id, const std::string& name, const std::string& category,
                           const std::string& file) {
    std::ostringstream o;
    o << "    {\n"
      << "      \"id\": \"" << id << "\", \"name\": \"" << name << "\", \"version\": \"1.0.0\",\n"
      << "      \"author\": \"Grouping Test Maker\", \"licence\": \"MIT\", \"abiVersion\": "
      << CASCADE_PLUGIN_ABI_VERSION << ",\n"
      << "      \"capabilities\": [\"CASCADE_CAP_DECODER\"],\n"
      << "      \"summary\": \"The summary of " << name << ".\",\n"
      << "      \"description\": \"What " << name << " does.\",\n"
      << "      \"category\": \"" << category << "\", \"experimental\": false,\n"
      << "      \"whatsNew\": \"1.0.0: first release.\", \"published\": \"2026-10-13\",\n"
      << "      \"homepage\": \"https://example.invalid/" << id << "\",\n"
      << "      \"platforms\": [ { \"os\": \"" << cascade::core::PluginRepo::hostOs() << "\", \"arch\": \""
      << cascade::core::PluginRepo::hostArch() << "\", \"file\": \"" << file << "\", \"url\": "
      << "\"https://example.invalid/" << id << "/" << file << "\", \"sha256\": \"" << std::string(64, 'c')
      << "\", \"sizeBytes\": 4096 } ]\n"
      << "    }";
    return o.str();
}

std::string catalogueText() {
    std::ostringstream o;
    o << "{\n  \"schemaVersion\": 1,\n  \"generated\": \"2026-10-13T00:00:00Z\",\n  \"plugins\": [\n"
      << catalogueEntry("grp-image", "Image Probe", "aircraft", std::string("image_probe") + kExt) << ",\n"
      << catalogueEntry("grp-text", "Rescan Probe a", "broadcast", std::string("rescan_probe_a") + kExt)
      << "\n  ]\n}\n";
    return o.str();
}

fs::path indexPath() { return g_dir / "index.json"; }

// The install records and the kept catalogue, written over whatever installPlugins() left.
void writeCatalogue() {
    std::ostringstream m;
    m << "{ \"schemaVersion\": 1, \"plugins\": [ "
      << "{ \"id\": \"grp-image\", \"name\": \"Image Probe\", \"version\": \"1.0.0\", \"file\": \"image_probe"
      << kExt << "\", \"abiVersion\": " << CASCADE_PLUGIN_ABI_VERSION << ", \"installedAt\": 1790000000 }, "
      << "{ \"id\": \"grp-text\", \"name\": \"Rescan Probe a\", \"version\": \"1.0.0\", \"file\": "
         "\"rescan_probe_a"
      << kExt << "\", \"abiVersion\": " << CASCADE_PLUGIN_ABI_VERSION << ", \"installedAt\": 1790000000 } ] }\n";
    writeFile(pluginsDir() / "installed.json", m.str());
    writeFile(pluginsDir() / "catalogue.json", catalogueText());
    writeFile(pluginsDir() / "catalogue.json.time", "1790000000\n");
    writeFile(indexPath(), catalogueText());
}

// The config line that points the catalogue at that local file (so no run reaches the network).
std::string catalogueConfig() { return ", \"pluginCatalogueUrl\": \"" + indexPath().generic_string() + "\""; }

struct RunOptions {
    int frames = 60;
    std::string script;           // FOXSDR_INPUT_SCRIPT, empty = none
    bool openDecoders = false;    // FOXSDR_OPEN_DECODERS: the DECODE bank's "Turn on and off plugins" open
    bool openFitted = false;      // FOXSDR_OPEN_FITTED_MODULES, in a rectangle inside the main window
    bool openDecoderOutput = false;  // FOXSDR_OPEN_DECODER_OUTPUT (the capture seam)
    std::string dormantAfterMs;   // FOXSDR_DORMANT_AFTER_MS, empty = the real 30 s
    std::string press;            // FOXSDR_PRESS_PRESET: the plugin whose first preset is pressed at frame 30
    std::string extraJson;        // more config JSON (a leading comma)
    bool keepConfig = false;      // keep the config the previous run with this tag wrote
    bool keepTuned = false;       // fixture requests automatic reception setup without retuning
    bool textDecoder = false;    // fixture also declares text output, alongside its own image window
    bool showTextOutput = false;  // preset requests the shared output window despite its own window
};

Result once(const std::string& tag, const RunOptions& o) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    const fs::path diag = g_dir / (tag + "-diag");
    const fs::path probe = g_dir / (tag + ".probe");
    {
        std::error_code ec;
        fs::remove_all(diag, ec);
        fs::create_directories(diag, ec);
        fs::remove(probe, ec);
    }
    if (!o.keepConfig) {
        std::ostringstream c;
        c << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": \"siggen\", "
             "\"uiTheme\": \"today\", \"interfaceScale\": \"100\", \"mainView\": \"receiver\", "
             "\"railBank\": 1, \"bandPlanOverlay\": false";
        if (o.openFitted) {
            // The Fitted modules window opens a stagger slot past the main window's right edge when
            // nothing is saved, which in a single viewport is outside the only window there is.
            c << ", \"fittedModulesX\": 60, \"fittedModulesY\": 50, \"fittedModulesWidth\": 1400, "
                 "\"fittedModulesHeight\": 880";
        }
        c << o.extraJson << " }\n";
        writeFile(cfg, c.str());
    }
    const std::string scriptPath = (g_dir / (tag + ".script")).string();
    writeFile(scriptPath, o.script);
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    setEnv("RESCAN_PROBE_LOG", probe.string());
    setEnv("FOXSDR_INPUT_SCRIPT", o.script.empty() ? std::string() : scriptPath);
    setEnv("FOXSDR_SCRIPT_TRACE", (g_dir / (tag + ".trace")).string());
    setEnv("FOXSDR_OPEN_FITTED_MODULES", o.openFitted ? "1" : "");
    setEnv("FOXSDR_OPEN_DECODERS", o.openDecoders ? "1" : "");
    setEnv("FOXSDR_OPEN_DECODER_OUTPUT", o.openDecoderOutput ? "1" : "");
    setEnv("FOXSDR_DORMANT_AFTER_MS", o.dormantAfterMs);
    setEnv("FOXSDR_PRESS_PRESET", o.press);
    setEnv("PRESET_PROBE_KEEP_TUNED", o.keepTuned ? "1" : "");
    setEnv("PRESET_PROBE_TEXT_CAP", o.textDecoder ? "1" : "");
    setEnv("PRESET_PROBE_SHOW_TEXT", o.showTextOutput ? "1" : "");
    r.out = run("\"" + exePath() + "\" --frames " + std::to_string(o.frames) + " 2>&1");
    r.log = readTree(diag);
    r.probe = readFile(probe);
    r.config = readFile(cfg);
    const bool rendered = r.out.find("rendered " + std::to_string(o.frames) + " frames") != std::string::npos;
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
        // The name may hold spaces ("switchrow:Image Probe image"); the four numbers are the last four.
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

// A click at the centre of `at`: the pointer arrives at `frame`, goes down two frames later and up
// two after that - the release at frame + 4 is what presses a key.
std::string click(int frame, const Rect& at) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d screen %.1f %.1f\n%d down\n%d up\n", frame, at.cx(), at.cy(), frame + 2,
                  frame + 4);
    return buf;
}

// A DOUBLE-CLICK at the centre of `at`: the pointer arrives at `frame` (two frames before the first press, so the
// row under it has been hovered for a frame), and the button goes down and up in consecutive frames twice, so the
// second press follows the first by two frames and the release by one - inside ImGui's 0.30 s double-click time
// unless a frame takes 150 ms, and on the same pixel.
std::string dblclick(int frame, const Rect& at) {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%d screen %.1f %.1f\n%d down\n%d up\n%d down\n%d up\n", frame, at.cx(), at.cy(),
                  frame + 2, frame + 3, frame + 4, frame + 5);
    return buf;
}

// A point on a Fitted modules row that belongs to no item of its own: on the row's centre line, a third of
// the way from the end of the name to the START/STOP key (clear of the picture, the name, the state word
// and the keys). A one-pixel rectangle, so click() and dblclick() aim at it.
Rect emptySpot(const Rect& row, const Rect& name, const Rect& key) {
    Rect r;
    r.x0 = r.x1 = name.x1 + 0.3f * (key.x0 - name.x1);
    r.y0 = r.y1 = 0.5f * (row.y0 + row.y1);
    return r;
}

// Real time between frames: `16 ms` in every frame from `from` to `to`, so a run that has to outlast an
// interval measured in milliseconds does (a bounded run otherwise draws as fast as it can).
std::string pace(int from, int to) {
    std::string s;
    for (int f = from; f <= to; ++f) { s += std::to_string(f) + " sleep 16\n"; }
    return s;
}

Rect need(const Result& r, const std::string& name) {
    const Rect* p = r.rect(name);
    CHECK(p != nullptr);
    if (p == nullptr) { std::printf("    missing rect %s\n", name.c_str()); }
    return p != nullptr ? *p : Rect{};
}

// The two plugins' identities, as the Fitted modules window letters them (the file name, for a module
// the plugin store did not install).
const std::string kTextFile = "rescan_probe_a";
const std::string kImageFile = "image_probe";

}  // namespace

int main(int argc, char** argv) {
    std::printf("test_plugin_inuse_app\n");
    // Unbuffered, so a run that a timeout stops still says how far it got.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 4) {
        std::printf("usage: test_plugin_inuse_app <text probe module> <image probe module> "
                    "<preset probe module>\n");
        return 2;
    }
    g_textDll = argv[1];
    g_imageDll = argv[2];
    g_presetDll = argv[3];
    g_dir = fs::temp_directory_path() / ("cascade-inuse-" + std::to_string(pid()));
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
    setEnv("FOXSDR_PRESS_PRESET", "");
    setEnv("FOXSDR_SHOT_DIR", "");
    setEnv("FOXSDR_SHOT_AT_FRAME", "");
    setEnv("FOXSDR_WINDOW_POS", "");
    setEnv("CASCADE_DECODE_TEST", "");
    setEnv("CASCADE_PLUGIN_TEST", "");

    // --- START: two plugins fitted, nothing open ----------------------------------------------------
    Rect imageRow, textKey, textName, fImage, fImageName, fImageKey, fImageRemove, fText;
    {
        installPlugins(kTextFile, kImageFile);
        RunOptions o;
        o.frames = 70;
        o.openFitted = true;
        const Result r = once("start", o);
        CHECK(r.ok);
        if (r.ok) {
            // The modules were mapped (the host found and loaded them) ...
            CHECK(r.count("a", "attach") == 1);
            CHECK(r.count("image", "attach") == 1);
            // ... and NEITHER decoder was created: nothing is using them.
            CHECK(r.count("a", "create") == 0);
            CHECK(r.count("image", "create") == 0);
            CHECK(r.count("a", "destroy") == 0);
            CHECK(r.count("image", "destroy") == 0);
            // The Fitted modules window says IDLE, for both, and counts two.
            CHECK(r.has("fitted:state:" + kTextFile + kExt + ":idle"));
            CHECK(r.has("fitted:state:" + kImageFile + kExt + ":idle"));
            CHECK(!r.has("fitted:state:" + kTextFile + kExt + ":notfed"));
            CHECK(!r.has("fitted:state:" + kImageFile + kExt + ":notfed"));
            CHECK(r.has("fitted:count:idle:2"));
            CHECK(r.rect("fitted:chip:idle") != nullptr);
            CHECK(!r.logHas("woke -"));
            CHECK(!r.logHas("dormant after"));
            // Nothing is saved for an AUTO plugin.
            CHECK(r.config.find("\"pluginRun\": {}") != std::string::npos ||
                  r.config.find("\"pluginRun\":{}") != std::string::npos);
            // The picture decoder's window row is on the rail all the same: listed from what the
            // module declares, so that pressing it can wake the plugin.
            CHECK(r.rect("switchrow:Image Probe image") != nullptr);
            imageRow = need(r, "switchrow:Image Probe image");
            textKey = need(r, "fitted:row:" + kTextFile + kExt + ":key");
            textName = need(r, "fitted:row:" + kTextFile + kExt + ":name");
            // THE ROWS THEMSELVES, for the double-click scenarios below.
            fImage = need(r, "fitted:row:" + kImageFile + kExt);
            fImageName = need(r, "fitted:row:" + kImageFile + kExt + ":name");
            fImageKey = need(r, "fitted:row:" + kImageFile + kExt + ":key");
            fImageRemove = need(r, "fitted:row:" + kImageFile + kExt + ":remove");
            fText = need(r, "fitted:row:" + kTextFile + kExt);
            std::printf("    idle at start: a create %d, image create %d; image row at %.0f,%.0f\n",
                        r.count("a", "create"), r.count("image", "create"), imageRow.cx(), imageRow.cy());
        }
    }

    // --- THE ROW: a press on the window row creates THAT plugin's decoder, a few frames later -------
    {
        installPlugins(kTextFile, kImageFile);
        // CONTROL: the run ends before the release of the click (the press is at frame 40, the release at
        // 44): nothing has been opened, so nothing has been created.
        RunOptions before;
        before.frames = 43;
        before.script = click(40, imageRow);
        const Result b = once("row-before", before);
        CHECK(b.ok);
        if (b.ok) { CHECK(b.count("image", "create") == 0); }

        // (The Fitted modules window is NOT opened for these runs: it floats over the rail in a single
        // viewport, and a press on the rail would land on it.)
        RunOptions o;
        o.frames = 52;
        o.script = click(40, imageRow);
        const Result r = once("row", o);
        CHECK(r.ok);
        if (r.ok) {
            std::printf("    after the press: image create %d, a create %d\n", r.count("image", "create"),
                        r.count("a", "create"));
            // Exactly the picture decoder's decoder, once, and the text decoder's untouched.
            CHECK(r.count("image", "create") == 1);
            CHECK(r.count("a", "create") == 0);
            CHECK(r.logHas("plugin: Image Probe woke - window open"));
            CHECK(!r.logHas("plugin: Rescan Probe a woke"));
            // A window is not a pin: nothing is saved for it.
            CHECK(r.config.find("\"image_probe\"") == std::string::npos);
        }
        // HOW SOON: the release is at frame 44, the press toggles the window in that frame's rail, and
        // the next frame's lifecycle step wakes the plugin - so a run that ends three frames after the
        // release has the decoder.
        RunOptions soon = o;
        soon.frames = 48;
        const Result s = once("row-soon", soon);
        CHECK(s.ok);
        if (s.ok) {
            std::printf("    three frames after the release: image create %d\n", s.count("image", "create"));
            CHECK(s.count("image", "create") == 1);
        }
    }

    // --- THE DECODER OUTPUT WINDOW wakes the TEXT decoder, and only it -----------------------------
    {
        installPlugins(kTextFile, kImageFile);
        RunOptions o;
        o.frames = 40;
        o.openDecoderOutput = true;
        const Result r = once("output", o);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.count("a", "create") == 1);
            CHECK(r.count("image", "create") == 0);
            CHECK(r.logHas("plugin: Rescan Probe a woke - decoder output window open"));
            CHECK(!r.logHas("plugin: Image Probe woke"));
        }
    }

    // --- CLOSE: the plugin goes dormant after the interval, and wakes again when it is used again ----
    //
    // The interval is shortened to 250 ms for the run; frames are paced at 16 ms of real time each, so
    // the 90 frames between the close and the second press are about three seconds - twelve times the
    // interval. Open at 30 (the release is at 34), close at 50 (release 54), open again at 140.
    {
        installPlugins(kTextFile, kImageFile);
        RunOptions o;
        o.frames = 170;
        o.dormantAfterMs = "250";
        o.script = click(30, imageRow) + click(50, imageRow) + pace(58, 135) + click(140, imageRow);
        const Result r = once("close", o);
        CHECK(r.ok);
        if (r.ok) {
            std::printf("    open, close, wait, open: image create %d destroy %d; woke %d dormant %d\n",
                        r.count("image", "create"), r.count("image", "destroy"),
                        r.logCount("plugin: Image Probe woke"), r.logCount("plugin: Image Probe dormant"));
            CHECK(r.logCount("plugin: Image Probe woke - window open") == 2);
            CHECK(r.logCount("plugin: Image Probe dormant after 30 s without a use") == 1);
            // A SECOND create can only happen if the first instance was destroyed: the host does not
            // create a second one for a plugin that has one.
            CHECK(r.count("image", "create") == 2);
            // The first instance was destroyed when the plugin went dormant, the second at exit.
            CHECK(r.count("image", "destroy") == 2);
            // The text decoder was never in it.
            CHECK(r.count("a", "create") == 0);
        }
        // THE CONTROL: the same run with the window left open never goes dormant.
        RunOptions kept = o;
        kept.script = click(30, imageRow) + pace(58, 135);
        kept.frames = 150;
        const Result k = once("kept", kept);
        CHECK(k.ok);
        if (k.ok) {
            CHECK(k.logCount("plugin: Image Probe woke - window open") == 1);
            CHECK(!k.logHas("plugin: Image Probe dormant"));
            CHECK(k.count("image", "create") == 1);
            CHECK(k.count("image", "destroy") == 1);  // at exit, when the application ends
        }
    }

    // --- START PINS: ALWAYS survives a restart through the settings file --------------------------------
    {
        installPlugins(kTextFile, kImageFile);
        RunOptions o;
        o.frames = 80;
        o.openFitted = true;
        o.script = click(40, textKey);
        const Result r = once("pin", o);
        CHECK(r.ok);
        if (r.ok) {
            // A START press is the user's own: the decoder is created, and there is no "woke" line (that
            // is for a plugin the SCREEN started).
            CHECK(r.count("a", "create") == 1);
            CHECK(!r.logHas("plugin: Rescan Probe a woke"));
            // Saved against the plugin's ID.
            CHECK(r.config.find("\"" + kTextFile + "\": \"always\"") != std::string::npos);
            CHECK(r.config.find("\"" + kImageFile + "\"") == std::string::npos);
            std::printf("    pinned: a create %d; config names %s\n", r.count("a", "create"),
                        r.config.find("always") != std::string::npos ? "always" : "nothing");
        }
        // THE NEXT LAUNCH, from the config that run wrote, with nothing open: the pinned plugin's
        // decoder is created at start and the other is not. Paced and with a short interval, so a
        // pinned plugin that WAS subject to the clock would show it.
        RunOptions again;
        again.frames = 120;
        again.keepConfig = true;
        again.openFitted = true;
        again.dormantAfterMs = "200";
        again.script = pace(10, 110);
        const Result n = once("pin", again);
        CHECK(n.ok);
        if (n.ok) {
            CHECK(n.count("a", "create") == 1);
            CHECK(n.count("image", "create") == 0);
            CHECK(!n.logHas("woke -"));
            CHECK(!n.logHas("dormant after"));
            CHECK(!n.has("fitted:state:" + kTextFile + kExt + ":idle"));
            CHECK(n.has("fitted:state:" + kImageFile + kExt + ":idle"));
            // It lasted the whole run: its one destroy is the application ending.
            CHECK(n.count("a", "destroy") == 1);
        }
    }

    // --- THE TICK on the module's page does the same, and unticking takes the entry out ------------------
    {
        installPlugins(kTextFile, kImageFile);
        // A layout run with the page open, for the tick's rectangle.
        RunOptions lay;
        lay.frames = 70;
        lay.openFitted = true;
        lay.script = click(30, textName);
        const Result l = once("tick-layout", lay);
        CHECK(l.ok);
        const Rect tick = need(l, "fitted:page:keeprunning");
        if (l.ok) {
            CHECK(l.rect("fitted:page:keeprunning") != nullptr);
            // An idle module's page offers the tick unticked, and nothing was saved by looking.
            CHECK(l.config.find("always") == std::string::npos);
        }
        installPlugins(kTextFile, kImageFile);
        RunOptions o = lay;
        o.frames = 100;
        o.script = click(30, textName) + click(60, tick);
        const Result r = once("tick", o);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.config.find("\"" + kTextFile + "\": \"always\"") != std::string::npos);
            CHECK(r.count("a", "create") == 1);
            CHECK(!r.logHas("plugin: Rescan Probe a woke"));
        }
        // Ticked and then UNTICKED: the entry is gone (AUTO is never written), and the plugin, which
        // was running, is not put out at once - it goes dormant only after nothing has wanted it for the
        // interval, which the run is too short to reach.
        // (Ticked, the module is no longer IDLE, so the sentence above the tick is another length and the
        // tick has moved: its place is read from a layout run of a module that starts ticked.)
        installPlugins(kTextFile, kImageFile);
        RunOptions layOn = lay;
        layOn.extraJson = ", \"pluginRun\": { \"" + kTextFile + "\": \"always\" }";
        const Result lOn = once("tick-layout-on", layOn);
        CHECK(lOn.ok);
        const Rect tickOn = need(lOn, "fitted:page:keeprunning");
        installPlugins(kTextFile, kImageFile);
        RunOptions off = lay;
        off.frames = 130;
        off.script = click(30, textName) + click(60, tick) + click(90, tickOn);
        const Result f = once("tick-off", off);
        CHECK(f.ok);
        if (f.ok) {
            CHECK(f.config.find("always") == std::string::npos);
            CHECK(f.count("a", "create") == 1);
            CHECK(f.count("a", "destroy") == 1);  // only the application ending
            CHECK(!f.logHas("dormant after"));
        }
    }

    // --- A PRESET PRESS PINS; THE TUNE THAT OPENING A WINDOW DOES DOES NOT -----------------------------
    //
    // Both go through the same function, and the difference is the whole of whether a plugin with a preset
    // (which is most of them) can ever go dormant: pinned by merely opening its window once, it would run
    // for ever after and AUTO would mean nothing. The preset probe has a window of its own AND a preset.
    {
        const std::string presetFile = "preset_probe";
        installPlugins("", "", presetFile);
        // Its row's place, from a run that opens nothing.
        RunOptions lay;
        lay.frames = 50;
        const Result l = once("preset-layout", lay);
        CHECK(l.ok);
        const Rect presetRow = need(l, "switchrow:Preset Probe image");
        if (l.ok) {
            CHECK(l.count("preset", "attach") == 1);
            CHECK(l.count("preset", "create") == 0);  // fitted and idle, like the others
        }

        // Real AppWindow: opening from a UK tune and from a listed frequency must repair
        // AM/75-us/40-kHz settings without changing the centre or VFO offset.
        for (const double centre : {153.0e6, 100.15e6}) {
            RunOptions setup;
            setup.frames = 65;
            setup.keepTuned = true;
            setup.script = click(30, presetRow);
            setup.extraJson = ", \"centerHz\": " + std::to_string(centre) +
                ", \"vfoOffsetHz\": 350000, \"mode\": \"AM\", \"bandwidthHz\": 40000, \"deemphasisIndex\": 1";
            const Result a = once(centre > 150.0e6 ? "preset-uk" : "preset-matched", setup);
            CHECK(a.ok);
            if (a.ok) {
                std::printf("    automatic reception setup at %.3f MHz: %s\n", (centre + 350000.0) / 1e6,
                            a.logHas("applied its preset") ? "applied" : "skipped");
                CHECK(a.config.find("\"centerHz\": " + std::to_string(static_cast<int>(centre)) + ".0") != std::string::npos);
                CHECK(a.config.find("\"vfoOffsetHz\": 350000.0") != std::string::npos);
                CHECK(a.config.find("\"mode\": \"NFM\"") != std::string::npos);
                CHECK(a.config.find("\"bandwidthHz\": 12500.0") != std::string::npos);
                CHECK(a.config.find("\"deemphasisIndex\": 2") != std::string::npos);
                CHECK(a.count("preset", "create") == 1);
            }
        }

        RunOptions explicitTune;
        explicitTune.frames = 65;
        explicitTune.keepTuned = true;
        explicitTune.press = "Preset Probe";
        explicitTune.extraJson = ", \"centerHz\": 153000000, \"vfoOffsetHz\": 350000, \"deemphasisIndex\": 1";
        const Result e = once("preset-explicit-tune", explicitTune);
        CHECK(e.ok);
        if (e.ok) {
            CHECK(e.config.find("\"centerHz\": 100150000.0") != std::string::npos);
            CHECK(e.config.find("\"vfoOffsetHz\": 350000.0") != std::string::npos);
            CHECK(e.config.find("\"deemphasisIndex\": 2") != std::string::npos);
        }

        for (int scenario = 0; scenario < 4; ++scenario) {
            RunOptions output;
            output.frames = 65;
            output.press = "Preset Probe";
            output.textDecoder = scenario != 2;
            output.showTextOutput = scenario != 1;
            output.openDecoderOutput = scenario == 3;
            const Result t = once("preset-own-window-text-" + std::to_string(scenario), output);
            CHECK(t.ok);
            if (t.ok) {
                CHECK(t.rect("image:window:Preset Probe") != nullptr);
                const bool expected = scenario == 0 || scenario == 3;
                CHECK((t.rect("decoder:window") != nullptr) == expected);
                const int opens = t.logCount("preset: opened the Decoder output window for Preset Probe");
                CHECK(opens == (scenario == 0 ? 1 : 0));
                std::printf("    own window with text-output request scenario %d: shared %s, opens %d\n",
                            scenario, t.rect("decoder:window") != nullptr ? "shown" : "hidden", opens);
            }
        }

        // THE PRESS (the capture seam presses the plugin's first preset at frame 30): the plugin is created,
        // PINNED - saved against its id - and, paced past the interval, never goes dormant.
        installPlugins("", "", presetFile);
        RunOptions press;
        press.frames = 110;
        press.press = "Preset Probe";
        press.extraJson = ", \"deemphasisIndex\": 1";
        press.dormantAfterMs = "200";
        press.script = pace(35, 105);
        const Result p = once("preset-press", press);
        CHECK(p.ok);
        if (p.ok) {
            std::printf("    preset pressed: create %d; pinned in the config: %s\n", p.count("preset", "create"),
                        p.config.find("\"preset_probe\": \"always\"") != std::string::npos ? "yes" : "no");
            CHECK(p.logHas("capture: preset for 'Preset Probe' pressed"));
            CHECK(p.count("preset", "create") == 1);
            CHECK(p.config.find("\"deemphasisIndex\": 1") != std::string::npos);
            CHECK(p.config.find("\"preset_probe\": \"always\"") != std::string::npos);
            CHECK(!p.logHas("dormant after"));
            CHECK(p.count("preset", "destroy") == 1);  // only the application ending
        }

        // THE WINDOW'S ROW (opening the window also tunes to the preset): the plugin is woken by the open
        // window, tuned, and NOT pinned - closed again, it goes dormant, and nothing is saved for it.
        installPlugins("", "", presetFile);
        RunOptions open;
        open.frames = 150;
        open.dormantAfterMs = "250";
        open.script = click(30, presetRow) + click(50, presetRow) + pace(58, 140);
        const Result w = once("preset-window", open);
        CHECK(w.ok);
        if (w.ok) {
            std::printf("    window opened and closed: create %d; woke %d dormant %d\n", w.count("preset", "create"),
                        w.logCount("plugin: Preset Probe woke"), w.logCount("plugin: Preset Probe dormant"));
            // The auto-preset DID run (opening the window tuned to the preset) ...
            CHECK(w.logHas("plugin: Preset Probe window opened - applied its preset Probe 100.5 MHz"));
            // ... the plugin woke because its window was open, and went down after it was closed ...
            CHECK(w.logCount("plugin: Preset Probe woke - window open") == 1);
            CHECK(w.logCount("plugin: Preset Probe dormant after 30 s without a use") == 1);
            CHECK(w.count("preset", "create") == 1);
            // ... and it was never pinned.
            CHECK(w.config.find("preset_probe") == std::string::npos);
        }
    }

    // --- THE OLD STOP LIST is migrated, by ID -----------------------------------------------------------
    {
        // The plugin is fitted as a DIFFERENT VERSION of the file the old list names.
        const std::string newBuild = kTextFile + "-2.0.0-abi3-win-x64";
        installPlugins(newBuild, "");
        RunOptions o;
        o.frames = 70;
        o.openFitted = true;
        o.extraJson = ", \"pluginsStopped\": [\"" + kTextFile + "-1.0.0-abi3-win-x64" + kExt + "\"]";
        const Result r = once("migrate", o);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.has("fitted:state:" + newBuild + kExt + ":stopped"));
            CHECK(r.count("a", "create") == 0);
            // Saved by id, and the old list gone.
            CHECK(r.config.find("\"" + kTextFile + "\": \"stopped\"") != std::string::npos);
            CHECK(r.config.find("\"pluginsStopped\": []") != std::string::npos ||
                  r.config.find("\"pluginsStopped\":[]") != std::string::npos);
            std::printf("    migrated: stopped by id across versions, a create %d\n", r.count("a", "create"));
        }
        // And the next launch, from that config, keeps it stopped.
        RunOptions again;
        again.frames = 50;
        again.keepConfig = true;
        again.openFitted = true;
        const Result n = once("migrate", again);
        CHECK(n.ok);
        if (n.ok) {
            CHECK(n.has("fitted:state:" + newBuild + kExt + ":stopped"));
            CHECK(n.count("a", "create") == 0);
        }

        // An explicit new AUTO state wins over a stale legacy stop for the same id.
        RunOptions explicitAuto;
        explicitAuto.frames = 50;
        explicitAuto.openFitted = true;
        explicitAuto.extraJson =
            ", \"pluginRun\": {\"" + kTextFile + "\": \"auto\"}, \"pluginsStopped\": [\"" +
            kTextFile + "-1.0.0-abi3-win-x64" + kExt + "\"]";
        const Result a = once("migrate-explicit-auto", explicitAuto);
        CHECK(a.ok);
        if (a.ok) {
            CHECK(a.has("fitted:state:" + newBuild + kExt + ":idle"));
            CHECK(!a.has("fitted:state:" + newBuild + kExt + ":stopped"));
            CHECK(a.config.find("\"" + kTextFile + "\": \"stopped\"") == std::string::npos);
        }
    }

    // --- THE LISTS UNDER THE STORE'S HEADINGS (0.99.77) -------------------------------------------------
    //
    // A US tester, through the owner (2026-10-13): "if you install them all, the list is quite long,
    // intermixed and kind-of alphabetical." Three modules are fitted: the picture decoder (AIRCRAFT in
    // the kept catalogue), the text decoder (VOICE AND DATA) and the preset probe, which no record or
    // catalogue names (OTHER). By name they would be Image Probe, Preset Probe, Rescan Probe a; under the
    // store's headings, in the store's order, Image Probe (AIRCRAFT), Rescan Probe a (VOICE AND DATA),
    // Preset Probe (OTHER, last).
    {
        const std::string presetFile = "preset_probe";
        const char* const kAllGroups[] = {"aircraft", "marine", "satellites", "meters", "voice", "maps", "other"};
        const auto yOf = [](const Result& r, const std::string& name) {
            const Rect* p = r.rect(name);
            CHECK(p != nullptr);
            if (p == nullptr) { std::printf("    missing rect %s\n", name.c_str()); }
            return p != nullptr ? p->y0 : -1.0e9f;
        };

        // THE FITTED MODULES WINDOW, the catalogue kept.
        installPlugins(kTextFile, kImageFile, presetFile);
        writeCatalogue();
        RunOptions fit;
        fit.frames = 80;
        fit.openFitted = true;
        fit.extraJson = catalogueConfig();
        const Result f = once("groups-fitted", fit);
        CHECK(f.ok);
        if (f.ok) {
            CHECK(f.has("fitted:group:aircraft"));
            CHECK(f.has("fitted:group:voice"));
            CHECK(f.has("fitted:group:other"));
            // Only the groups that have a row get a heading.
            for (const char* g : {"marine", "satellites", "meters", "maps"}) {
                CHECK(!f.has(std::string("fitted:group:") + g));
            }
            const float hAir = yOf(f, "fitted:group:aircraft"), hVoice = yOf(f, "fitted:group:voice"),
                        hOther = yOf(f, "fitted:group:other");
            const float rImage = yOf(f, "fitted:row:grp-image"), rText = yOf(f, "fitted:row:grp-text"),
                        rPreset = yOf(f, "fitted:row:" + presetFile + kExt);
            std::printf("    Fitted modules, catalogue kept: AIRCRAFT y %.0f > Image %.0f | VOICE AND DATA %.0f > "
                        "Rescan %.0f | OTHER %.0f > Preset %.0f\n",
                        hAir, rImage, hVoice, rText, hOther, rPreset);
            // Each heading above its own row, in the store's order: AIRCRAFT, VOICE AND DATA, OTHER.
            CHECK(hAir < rImage && rImage < hVoice && hVoice < rText && rText < hOther && hOther < rPreset);
            // ...which is not the name order (the preset probe would sit between the other two).
            CHECK(rPreset > rText);
        }

        // THE SAME FOLDER WITH NO CATALOGUE KEPT: no module has a category, so no heading, and the list is
        // the name-sorted list the window always was.
        installPlugins(kTextFile, kImageFile, presetFile);
        RunOptions flat = fit;
        flat.extraJson.clear();
        const Result fl = once("groups-flat", flat);
        CHECK(fl.ok);
        if (fl.ok) {
            for (const char* g : kAllGroups) { CHECK(!fl.has(std::string("fitted:group:") + g)); }
            const float rImage = yOf(fl, "fitted:row:" + kImageFile + kExt),
                        rPreset = yOf(fl, "fitted:row:" + presetFile + kExt),
                        rText = yOf(fl, "fitted:row:" + kTextFile + kExt);
            CHECK(rImage < rPreset && rPreset < rText);
        }

        // THE DECODE BANK'S "Turn on and off plugins" SECTION, the catalogue kept: the same headings over the
        // same entries.
        installPlugins(kTextFile, kImageFile, presetFile);
        writeCatalogue();
        RunOptions dec;
        dec.frames = 60;
        dec.openDecoders = true;
        dec.extraJson = catalogueConfig();
        const Result d = once("groups-decode", dec);
        CHECK(d.ok);
        if (d.ok) {
            CHECK(d.has("section:Turn on and off plugins###decoders"));
            CHECK(d.has("decoders:group:aircraft"));
            CHECK(d.has("decoders:group:voice"));
            CHECK(d.has("decoders:group:other"));
            for (const char* g : {"marine", "satellites", "meters", "maps"}) {
                CHECK(!d.has(std::string("decoders:group:") + g));
            }
            const float hAir = yOf(d, "decoders:group:aircraft"), hVoice = yOf(d, "decoders:group:voice"),
                        hOther = yOf(d, "decoders:group:other");
            const float eImage = yOf(d, "decoders:entry:Image Probe"), eText = yOf(d, "decoders:entry:Rescan Probe a"),
                        ePreset = yOf(d, "decoders:entry:Preset Probe");
            std::printf("    DECODE bank, catalogue kept: AIRCRAFT y %.0f > Image Probe %.0f | VOICE AND DATA %.0f > "
                        "Rescan Probe a %.0f | OTHER %.0f > Preset Probe %.0f\n",
                        hAir, eImage, hVoice, eText, hOther, ePreset);
            CHECK(hAir < eImage && eImage < hVoice && hVoice < eText && eText < hOther && hOther < ePreset);
        }

        // ...AND WITH NO CATALOGUE: no heading, the entries in the scan order they always had.
        installPlugins(kTextFile, kImageFile, presetFile);
        RunOptions decFlat = dec;
        decFlat.extraJson.clear();
        const Result df = once("groups-decode-flat", decFlat);
        CHECK(df.ok);
        if (df.ok) {
            CHECK(df.has("section:Turn on and off plugins###decoders"));
            for (const char* g : kAllGroups) { CHECK(!df.has(std::string("decoders:group:") + g)); }
            const float eImage = yOf(df, "decoders:entry:Image Probe"), ePreset = yOf(df, "decoders:entry:Preset Probe"),
                        eText = yOf(df, "decoders:entry:Rescan Probe a");
            CHECK(eImage < ePreset && ePreset < eText);
        }
    }

    // --- A DOUBLE-CLICK ON A FITTED MODULES ROW OPENS THE PLUGIN'S OWN WINDOW (0.99.77) -----------------
    //
    // The owner (2026-10-13): "when you open the plugins Fitted modules window, when I double click on the
    // plugin it should show the plugin window". The picture decoder's window is its own; the text decoder
    // has none (it is read in the shared Decoder output window), so its row gives its page.
    if (fImage.x1 > fImage.x0 && fImageName.x1 > fImageName.x0) {
        const Rect spotImage = emptySpot(fImage, fImageName, fImageKey);
        const Rect spotText = emptySpot(fText, textName, textKey);
        std::printf("    double-click aimed at the image row's empty part %.0f,%.0f\n", spotImage.cx(), spotImage.cy());

        // THE CONTROL: ONE click on that empty part does nothing at all.
        installPlugins(kTextFile, kImageFile);
        RunOptions one;
        one.frames = 80;
        one.openFitted = true;
        one.script = click(30, spotImage);
        const Result c = once("dbl-control", one);
        CHECK(c.ok);
        if (c.ok) {
            CHECK(c.rect("image:window:Image Probe") == nullptr);
            CHECK(c.count("image", "create") == 0);
            CHECK(!c.has("fitted:page:" + kImageFile + kExt));
            CHECK(!c.logHas("woke -"));
        }

        // ON A KEY OF THE ROW the double-click is the key's: REMOVE pressed twice arms and disarms its
        // confirmation (nothing is deleted), and no window opens and no page.
        installPlugins(kTextFile, kImageFile);
        RunOptions onKey = one;
        onKey.script = dblclick(30, fImageRemove);
        const Result k = once("dbl-key", onKey);
        CHECK(k.ok);
        if (k.ok) {
            CHECK(k.rect("image:window:Image Probe") == nullptr);
            CHECK(!k.has("fitted:page:" + kImageFile + kExt));
            CHECK(k.count("image", "create") == 0);
            CHECK(!k.logHas("woke -"));
            CHECK(fs::exists(pluginsDir() / (kImageFile + kExt)));
        }

        // ON THE EMPTY PART OF THE ROW: the picture window opens, the plugin wakes through the lifecycle
        // (one decoder, the other plugin untouched), and a window is not a pin.
        installPlugins(kTextFile, kImageFile);
        RunOptions two = one;
        two.script = dblclick(30, spotImage);
        const Result r = once("dbl-row", two);
        CHECK(r.ok);
        if (r.ok) {
            const bool win = r.rect("image:window:Image Probe") != nullptr;
            std::printf("    double-click on the row: picture window %s; image create %d; woke %d\n", win ? "open" : "NOT open",
                        r.count("image", "create"), r.logCount("plugin: Image Probe woke - window open"));
            CHECK(win);
            CHECK(r.logCount("plugin: Image Probe woke - window open") == 1);
            CHECK(r.count("image", "create") == 1);
            CHECK(r.count("a", "create") == 0);
            CHECK(!r.has("fitted:page:" + kImageFile + kExt));
            CHECK(r.config.find("\"" + kImageFile + "\"") == std::string::npos);
        }

        // ON THE NAME: the first click of the pair opens the module's page, as a single click on the name
        // always has; the second hands over the window and the list comes back. That the list is back is
        // read by pressing the row's key afterwards: the plugin is awake by then, so the key reads STOP, and
        // its press stops the plugin - which could not happen were the page still on screen.
        installPlugins(kTextFile, kImageFile);
        RunOptions three = one;
        three.frames = 100;
        three.script = dblclick(30, fImageName) + click(60, fImageKey);
        const Result n = once("dbl-name", three);
        CHECK(n.ok);
        if (n.ok) {
            const bool win = n.rect("image:window:Image Probe") != nullptr;
            std::printf("    double-click on the name: picture window %s; then the row's key stopped it: %s\n",
                        win ? "open" : "NOT open",
                        n.config.find("\"" + kImageFile + "\": \"stopped\"") != std::string::npos ? "yes" : "no");
            CHECK(win);
            CHECK(n.logCount("plugin: Image Probe woke - window open") == 1);
            CHECK(n.count("image", "create") == 1);
            CHECK(n.config.find("\"" + kImageFile + "\": \"stopped\"") != std::string::npos);
        }

        // A MODULE WITH NO WINDOW OF ITS OWN gets its page, on the row and on the name, and is not woken.
        installPlugins(kTextFile, kImageFile);
        RunOptions four = one;
        four.script = dblclick(30, spotText);
        const Result t = once("dbl-text-row", four);
        CHECK(t.ok);
        if (t.ok) {
            CHECK(t.has("fitted:page:" + kTextFile + kExt));
            CHECK(t.rect("image:window:Image Probe") == nullptr);
            CHECK(t.count("a", "create") == 0);
            CHECK(!t.logHas("woke -"));
            CHECK(!t.has("fitted:page:closed:back"));
        }
        installPlugins(kTextFile, kImageFile);
        RunOptions five = one;
        five.script = dblclick(30, textName);
        const Result tn = once("dbl-text-name", five);
        CHECK(tn.ok);
        if (tn.ok) {
            // The page opened by the first click is still the page after the second: nothing took it away.
            CHECK(tn.has("fitted:page:" + kTextFile + kExt));
            CHECK(tn.count("a", "create") == 0);
            CHECK(!tn.logHas("woke -"));
        }

        // A STOPPED PLUGIN has no instances, so no window of its could draw: the double-click gives its
        // page (which says STOPPED and has START), opens no window and wakes nothing.
        installPlugins(kTextFile, kImageFile);
        RunOptions six = one;
        six.extraJson = ", \"pluginRun\": { \"" + kImageFile + "\": \"stopped\" }";
        six.script = dblclick(30, spotImage);
        const Result s = once("dbl-stopped", six);
        CHECK(s.ok);
        if (s.ok) {
            CHECK(s.has("fitted:state:" + kImageFile + kExt + ":stopped"));
            CHECK(s.has("fitted:page:" + kImageFile + kExt));
            CHECK(s.rect("image:window:Image Probe") == nullptr);
            CHECK(s.count("image", "create") == 0);
            CHECK(!s.logHas("woke -"));
        }
    }

    std::error_code ec2;
    fs::remove_all(g_dir, ec2);
    return testSummary("test_plugin_inuse_app");
}
