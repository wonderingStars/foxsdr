/*
 * THE RECORDER DRAWS THE PICTURE INSIDE ITSELF, AND A CLICK ON IT REACHES THE PLUGIN, END TO END.
 *
 * THE PRINCIPLE: a radiofax plugin declares an INSTRUMENT of kind CASCADE_INSTRUMENT_FAX (the
 * recorder) and an IMAGE DECODER (the chart). The two used to be two windows - the recorder with the
 * paper's face, and a separate picture window - so the user opened both to see the chart and press
 * on it. The recorder is now ONE window: the decoded picture inside it, fitted with its aspect
 * kept, the memory and its keys beside it (the picture on the left, the rows on the right); and a
 * left click on that picture is handed to the plugin's CascadeImageInputApi::press in the
 * picture's own pixels, exactly as a click in the separate picture window already was.
 *
 * Runs cascade itself, isolated like every test that starts the application, in ONE viewport
 * (FOXSDR_SINGLE_VIEWPORT=1, so the UI census reads every rectangle in the main window), with the
 * fixture of tests/fixtures/fax_picture_plugin.cpp fitted and pinned to run - a real plugin module
 * with an image decoder that has received a complete 400 x 100 picture, a fax instrument whose
 * memory holds one pressable key row (command 3, "SET LEFT EDGE"), and an image input that writes
 * every press it is handed to a file. The DECODE bank is put up (FOXSDR_OPEN_DECODERS) and a
 * scripted pointer presses the recorder's row on the rail to open its window, then presses in it.
 * The picture window is NEVER opened (no click goes to its row), so everything below happens with
 * "image:window:<plugin>" absent from the census: the recorder alone is enough.
 *
 *   layout   no click: the recorder's rail row is there and its window is not open;
 *   open     the window is open: the census lists the recorder's window
 *            ("instrument:window:<plugin>") and the picture inside it ("instrument:picture:<plugin>"),
 *            the picture lies inside the window, is four to one (400 x 100 fitted, not stretched),
 *            and the memory's key row ("panelrow:<plugin>:memory:3:key") lies inside the window
 *            with its left edge at or right of the picture's right edge - beside the picture, not
 *            under it; the separate picture window is absent; nothing has been handed to the plugin;
 *   picture  a click at the centre of the picture hands the plugin exactly one line, "press x y",
 *            with x and y at the middle of a 400 x 100 picture (200, 50) - native pixels, not
 *            screen ones; a run that ends before the click is released hands it nothing;
 *   key      a click on the memory's key row hands the plugin exactly "key 3", and no press of the
 *            picture - the two gestures are not confused.
 *
 * NO NETWORK: every telemetry address is a discard port and the update check is off.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <chrono>
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
#include <utility>
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
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
    // Whole of `in`, to within a pixel: a rectangle drawn on a window's edge may round either way.
    bool insideOf(const Rect& in, float slack = 1.0f) const {
        return x0 >= in.x0 - slack && y0 >= in.y0 - slack && x1 <= in.x1 + slack &&
               y1 <= in.y1 + slack;
    }
};

struct Result {
    bool ok = false;
    std::set<std::string> items;  // "item <name>" lines of the census
    std::map<std::string, Rect> rects;
    std::string out;      // what the process printed
    std::string log;      // what the fixture wrote to its log: "press <x> <y>" and "key <id>" lines
    const Rect* rect(const std::string& name) const {
        const auto it = rects.find(name);
        return it == rects.end() ? nullptr : &it->second;
    }
    // Every census name (item or rectangle) that starts with `prefix`, whole.
    std::set<std::string> namesStarting(const std::string& prefix) const {
        std::set<std::string> found;
        for (const std::string& s : items) {
            if (s.rfind(prefix, 0) == 0) { found.insert(s); }
        }
        for (const auto& kv : rects) {
            if (kv.first.rfind(prefix, 0) == 0) { found.insert(kv.first); }
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
std::string g_fixtureDll;  // the fax-picture fixture, copied into the scratch plugins folder
const std::string kFile = "fax-picture-fixture";  // the module file name, less its extension
const std::string kPlugin = "Fax Picture Fixture";  // its declared name
const std::string kRecorderRail = "switchrow:Fax Picture Recorder";  // its recorder's row on the rail: the instrument's title
const std::string kWindow = "instrument:window:" + kPlugin;    // the recorder's window, once it is open
const std::string kPicture = "instrument:picture:" + kPlugin;  // the picture drawn inside it
const std::string kKeyRow = "panelrow:" + kPlugin + ":memory:3:key";  // the memory's key row: command 3
const std::string kImageWindow = "image:window:" + kPlugin;    // the separate picture window

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

// One run of the application. `script` is FOXSDR_INPUT_SCRIPT (empty = none). The DECODE bank is put
// up, for the rail row that opens the recorder.
Result once(const std::string& tag, int frames, const std::string& script) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    const fs::path diag = g_dir / (tag + "-diag");
    const fs::path log = g_dir / (tag + ".log");
    {
        std::error_code ec;
        fs::remove_all(diag, ec);
        fs::create_directories(diag, ec);
        fs::remove(log, ec);
    }
    // railBank 1 is DECODE. The module is pinned to run ("always"), so its decoder is created, its
    // picture offered, and its memory polled from the first frame whatever is on screen.
    writeFile(cfg,
              "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": "
              "\"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", \"mainView\": "
              "\"receiver\", \"railBank\": 1, \"bandPlanOverlay\": false, "
              "\"pluginRun\": { \"" +
                  kFile + "\": \"always\" } }\n");
    const std::string scriptPath = (g_dir / (tag + ".script")).string();
    writeFile(scriptPath, script);
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    setEnv("FOXSDR_INPUT_SCRIPT", script.empty() ? std::string() : scriptPath);
    setEnv("FOXSDR_SCRIPT_TRACE", (g_dir / (tag + ".trace")).string());
    setEnv("FOXSDR_OPEN_DECODERS", "1");
    setEnv("FOXSDR_OPEN_FITTED_MODULES", "");
    setEnv("FAX_PICTURE_FIXTURE_LOG", log.string());
    std::printf("  run %s: %d frames ...\n", tag.c_str(), frames);
    const auto started = std::chrono::steady_clock::now();
    r.out = run("\"" + exePath() + "\" --frames " + std::to_string(frames) + " 2>&1");
    std::printf("  run %s: ended after %.1f s\n", tag.c_str(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    r.log = readFile(log);
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
        // The name may hold spaces ("panelrow:Fax Picture Fixture:memory:3:key"); the four numbers
        // are the last four tokens.
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

// A click at the point `fx` of the way across `at`, on its middle line: the pointer arrives at
// `frame`, goes down two frames later and up two after that - the release at frame + 4 is what
// presses it. (A press-and-release inside the picture is what the host forwards.)
std::string click(int frame, const Rect& at, float fx = 0.5f) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d screen %.1f %.1f\n%d down\n%d up\n", frame,
                  at.x0 + fx * (at.x1 - at.x0), at.cy(), frame + 2, frame + 4);
    return buf;
}

// Real time before a step: a bounded run draws as fast as it can, and the module has to have been
// polled (its picture taken, its rows drawn) before they are pressed.
std::string wait(int frame, int ms) { return std::to_string(frame) + " sleep " + std::to_string(ms) + "\n"; }

// The "press <x> <y>" lines of the fixture's log, as pairs.
std::vector<std::pair<int, int>> presses(const std::string& log) {
    std::vector<std::pair<int, int>> out;
    std::istringstream in(log);
    std::string line;
    while (std::getline(in, line)) {
        int x = 0;
        int y = 0;
        if (std::sscanf(line.c_str(), "press %d %d", &x, &y) == 2) { out.emplace_back(x, y); }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    // UNBUFFERED, so that a run stopped by the timeout still says how far it got.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_fax_picture_app\n");
    if (argc < 2) {
        std::printf("usage: test_fax_picture_app <fax picture fixture module>\n");
        return 2;
    }
    g_fixtureDll = argv[1];
    g_dir = fs::temp_directory_path() / ("cascade-faxpicture-" + std::to_string(pid()));
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
    installFixture();

    // --- LAYOUT: the recorder's rail row is there, its window is not open ----------------------------
    Rect rail;
    bool haveRail = false;
    {
        const Result r = once("layout", 60, wait(30, 400));
        CHECK(r.ok);
        if (r.ok) {
            const Rect* row = r.rect(kRecorderRail);
            CHECK(row != nullptr);
            if (row != nullptr) {
                rail = *row;
                haveRail = true;
                std::printf("    the recorder's row on the rail: x %.0f..%.0f y %.0f..%.0f\n", row->x0,
                            row->x1, row->y0, row->y1);
                CHECK(row->w() > 0.0f && row->h() > 0.0f);
                CHECK(row->x0 >= 0.0f && row->y0 >= 0.0f && row->x1 <= 1600.0f && row->y1 <= 1000.0f);
            }
            // The window is closed, so neither it nor anything drawn in it is on screen (so the
            // absences are not "the window never opened"), and the picture window is closed too.
            CHECK(r.rect(kWindow) == nullptr);
            CHECK(r.rect(kPicture) == nullptr);
            CHECK(r.rect(kKeyRow) == nullptr);
            CHECK(r.rect(kImageWindow) == nullptr);
        }
    }
    if (!haveRail) {
        std::error_code ec2;
        fs::remove_all(g_dir, ec2);
        return testSummary("test_fax_picture_app");
    }
    // The rail row is pressed at frame 15 and the window is up, its picture taken and its rows
    // polled, well before frame 40.
    const std::string openScript = click(15, rail) + wait(25, 500);

    // --- OPEN: the picture is inside the recorder, fitted, with the rows beside it -------------------
    Rect picture, key;
    bool haveParts = false;
    {
        const Result r = once("open", 80, openScript);
        CHECK(r.ok);
        if (r.ok) {
            const Rect* w = r.rect(kWindow);
            const Rect* p = r.rect(kPicture);
            const Rect* k = r.rect(kKeyRow);
            std::printf("    recorder census names: %s\n",
                        Result::join(r.namesStarting("instrument:")).c_str());
            std::printf("    memory rows in the census: %s\n",
                        Result::join(r.namesStarting("panelrow:")).c_str());
            CHECK(w != nullptr);
            CHECK(p != nullptr);
            CHECK(k != nullptr);
            if (w != nullptr && p != nullptr && k != nullptr) {
                picture = *p;
                key = *k;
                haveParts = true;
                std::printf("    window x %.0f..%.0f y %.0f..%.0f; picture x %.1f..%.1f y %.1f..%.1f"
                            " (%.1f x %.1f); key row x %.0f..%.0f y %.0f..%.0f\n",
                            w->x0, w->x1, w->y0, w->y1, p->x0, p->x1, p->y0, p->y1, p->w(), p->h(),
                            k->x0, k->x1, k->y0, k->y1);
                // The window is on screen and has a size.
                CHECK(w->w() > 0.0f && w->h() > 0.0f);
                // (a) The picture is drawn INSIDE the recorder's window.
                CHECK(p->w() > 0.0f && p->h() > 0.0f);
                CHECK(p->insideOf(*w));
                // (b) Fitted, aspect kept: 400 x 100 is four to one, whatever the window's shape. A
                // picture stretched to the window's free space would not be.
                if (p->h() > 0.0f) {
                    const float aspect = p->w() / p->h();
                    std::printf("    picture aspect %.3f (the picture is 400 x 100, 4.000)\n", aspect);
                    CHECK(aspect >= 3.9f && aspect <= 4.1f);
                }
                // (c) The memory's key row is inside the window, and BESIDE the picture: its left
                // edge is at or right of the picture's right edge.
                CHECK(k->w() > 0.0f && k->h() > 0.0f);
                CHECK(k->insideOf(*w));
                CHECK(k->x0 >= p->x1 - 1.0f);
            }
            // (f) Only the recorder is open: the separate picture window is not, and is not needed.
            CHECK(r.rect(kImageWindow) == nullptr);
            // Nothing has been pressed yet.
            CHECK(r.log.empty());
        }
    }
    if (!haveParts) {
        std::error_code ec2;
        fs::remove_all(g_dir, ec2);
        return testSummary("test_fax_picture_app");
    }

    // --- PICTURE: a click on it reaches the plugin in the picture's own pixels -----------------------
    {
        // The click's centre: a 400 x 100 picture clicked in the middle is pixel (200, 50).
        const Result r = once("press-picture", 120, openScript + click(60, picture));
        CHECK(r.ok);
        if (r.ok) {
            std::printf("    the plugin was handed: [%s]\n", r.log.c_str());
            CHECK(r.rect(kImageWindow) == nullptr);  // and still no picture window
            const std::vector<std::pair<int, int>> got = presses(r.log);
            // (d) One press, no key, at the middle of the picture - native pixels, not screen ones.
            CHECK(got.size() == 1u);
            CHECK(r.log.find("key") == std::string::npos);
            if (got.size() == 1u) {
                CHECK(std::abs(got[0].first - 200) <= 10);
                CHECK(std::abs(got[0].second - 50) <= 10);
            }
        }
    }
    {
        // CONTROL: the run ends before the click's release (frame 64), so nothing is pressed - the
        // log is empty for want of a click, not for want of a fixture that logs.
        const Result r = once("press-before", 63, openScript + click(60, picture));
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.rect(kPicture) != nullptr);
            CHECK(r.log.empty());
        }
    }

    // --- KEY: a click on the memory's key row hands the plugin command 3 -----------------------------
    {
        const Result r = once("press-key", 120, openScript + click(60, key, 0.2f));
        CHECK(r.ok);
        if (r.ok) {
            std::printf("    the plugin was handed: [%s]\n", r.log.c_str());
            // (e) The key and only the key: not a press of the picture.
            CHECK(r.log == "key 3\n");
            CHECK(presses(r.log).empty());
        }
    }

    std::error_code ec2;
    fs::remove_all(g_dir, ec2);
    return testSummary("test_fax_picture_app");
}
