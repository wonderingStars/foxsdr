// test_tx_audio_plate_app.cpp - the TRANSMIT grant, offered, pressed and kept, through the REAL
// cascade.exe and a REAL plugin module (0.99.74, CASCADE_CAP_TX_AUDIO).
//
// WHAT THIS ADDS. tests/test_tx_audio_app.cpp calls the grant's setter directly and
// tests/test_tx_audio_page_app.cpp draws the TRANSMIT page; neither draws the Fitted modules
// window's page for a module, where the GRANT TRANSMIT key lives. This starts the program with
// tests/fixtures/tx_audio_plugin.cpp fitted, pinned to run, and wanting the transmitter (its
// "send" setting is on), opens the module's page, and checks what the plate DREW and what a
// scripted click on the key did - through the plate's own action, to the saved settings file
// the next launch reads.
//
//   asks      a module whose active() has been non-zero is offered GRANT TRANSMIT; one that
//             never asked (its setting is off) is not offered the key at all
//   press     a click on the key saves the grant, by module file name, in `pluginTransmitAllowed`
//   remember  the next launch draws the key as REVOKE (the grant was kept) and nothing is keyed
//   revoke    a click on it takes the grant out of the saved file again
//
// Nothing here transmits: no radio is open in any run.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
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
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
    std::string out;
    std::string log;
    std::string config;
    bool has(const std::string& item) const { return items.count(item) != 0; }
    const Rect* rect(const std::string& name) const {
        const auto it = rects.find(name);
        return it == rects.end() ? nullptr : &it->second;
    }
    bool logHas(const std::string& needle) const { return log.find(needle) != std::string::npos; }
};

fs::path g_dir;
std::string g_fixture;
const std::string kFile = "tx-audio-fixture";  // the module file name, less its extension

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

void installPlugin() {
    std::error_code ec;
    fs::remove_all(pluginsDir(), ec);
    fs::create_directories(pluginsDir(), ec);
    fs::copy_file(g_fixture, pluginsDir() / (kFile + kExt), fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
}

// `send` is the module's "send" setting: on, it asks for the transmitter all the time. The module is
// pinned to run, so the pipeline's DSP thread polls it whatever is on screen.
Result once(const std::string& tag, int frames, const std::string& script, bool send,
            bool keepConfig = false) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    const fs::path diag = g_dir / (tag + "-diag");
    {
        std::error_code ec;
        fs::remove_all(diag, ec);
        fs::create_directories(diag, ec);
    }
    if (keepConfig) {
        // The app clamps the Fitted modules window to the screen and saves the rectangle it ended
        // up with, so the row is not where the first launch found it. Put the first launch's
        // rectangle back; every other saved setting (the grant) is left as the app wrote it.
        std::string saved = readFile(cfg);
        const char* pins[][2] = {{"fittedModulesX", "60"},
                                 {"fittedModulesY", "50"},
                                 {"fittedModulesWidth", "1400"},
                                 {"fittedModulesHeight", "880"}};
        for (const auto& pin : pins) {
            const std::string keyText = std::string("\"") + pin[0] + "\"";
            const std::size_t at = saved.find(keyText);
            if (at == std::string::npos) { continue; }
            const std::size_t colon = saved.find(':', at);
            std::size_t end = saved.find_first_of(",\r\n}", colon);
            if (colon == std::string::npos || end == std::string::npos) { continue; }
            saved.replace(colon + 1, end - colon - 1, std::string(" ") + pin[1]);
        }
        writeFile(cfg, saved);
    } else {
        std::ostringstream c;
        c << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": "
             "\"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", \"mainView\": "
             "\"receiver\", \"railBank\": 1, \"bandPlanOverlay\": false, "
             "\"fittedModulesX\": 60, \"fittedModulesY\": 50, \"fittedModulesWidth\": 1400, "
             "\"fittedModulesHeight\": 880, "
             "\"pluginRun\": { \"" << kFile << "\": \"always\" }, "
             "\"pluginSettings\": { \"TX Audio Fixture\": { \"send\": \"" << (send ? "1" : "0")
          << "\" } } }\n";
        writeFile(cfg, c.str());
    }
    const std::string scriptPath = (g_dir / (tag + ".script")).string();
    writeFile(scriptPath, script);
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    setEnv("FOXSDR_INPUT_SCRIPT", script.empty() ? std::string() : scriptPath);
    setEnv("FOXSDR_SCRIPT_TRACE", (g_dir / (tag + ".trace")).string());
    setEnv("FOXSDR_OPEN_FITTED_MODULES", "1");
    // THE RECEIVER RUNS in this bounded run (CASCADE_DECODE_TEST starts it, as for the decoder
    // tests): a module is asked whether it wants the transmitter by the DSP thread, and the
    // DSP thread runs only while the receiver does.
    setEnv("CASCADE_DECODE_TEST", "1");
    // SAID BEFORE AND AFTER EVERY RUN (0.99.74): on both GitHub Linux runners this test was killed
    // at ctest's 120 s with no output at all (CI run 38288445561, x64 and arm64) - eight real
    // application runs, 1013 frames, 19 to 37 s on this desktop - and nothing said whether a run was
    // slow or stuck. The run in hand is now named before it starts and timed when it ends.
    std::printf("  run %s: %d frames ...\n", tag.c_str(), frames);
    const auto started = std::chrono::steady_clock::now();
    r.out = run("\"" + exePath() + "\" --frames " + std::to_string(frames) + " 2>&1");
    std::printf("  run %s: ended after %.1f s\n", tag.c_str(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    r.log = readTree(diag);
    r.config = readFile(cfg);
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

std::string click(int frame, const Rect& at) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d screen %.1f %.1f\n%d down\n%d up\n", frame, at.cx(), at.cy(),
                  frame + 2, frame + 4);
    return buf;
}

// Real time before a step: a bounded run draws as fast as it can, and the module has to have been
// polled (asked) before its page is drawn.
std::string wait(int frame, int ms) { return std::to_string(frame) + " sleep " + std::to_string(ms) + "\n"; }

// The grant list as the saved file carries it: the text between "pluginTransmitAllowed" and its
// closing bracket, with the whitespace taken out.
std::string grantList(const std::string& config) {
    const std::size_t at = config.find("\"pluginTransmitAllowed\"");
    if (at == std::string::npos) { return "<absent>"; }
    const std::size_t open = config.find('[', at);
    const std::size_t close = config.find(']', open);
    if (open == std::string::npos || close == std::string::npos) { return "<malformed>"; }
    std::string s;
    for (std::size_t i = open; i <= close; ++i) {
        if (config[i] != ' ' && config[i] != '\n' && config[i] != '\r' && config[i] != '\t') {
            s += config[i];
        }
    }
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    // UNBUFFERED, so that a run stopped by the timeout still says how far it got.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_tx_audio_plate_app\n");
    if (argc < 2) {
        std::printf("usage: test_tx_audio_plate_app <tx_audio_fixture module>\n");
        return 2;
    }
    g_fixture = argv[1];
    g_dir = fs::temp_directory_path() / ("cascade-txplate-" + std::to_string(pid()));
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
    setEnv("FOXSDR_FAKE_PACKAGE", "FoxSDR.StoreTest_1.0.0.0_x64__test");
    setEnv("FOXSDR_CENSUS_HOLD_BANK", "1");
    setEnv("FOXSDR_PATCH_START", "");
    setEnv("FOXSDR_PATCH_FILE", "");
    setEnv("FOXSDR_OPEN_PLUGIN_STORE", "");
    setEnv("FOXSDR_PRESS_PRESET", "");
    setEnv("FOXSDR_SHOT_DIR", "");
    setEnv("FOXSDR_SHOT_AT_FRAME", "");
    setEnv("FOXSDR_WINDOW_POS", "");
    setEnv("FOXSDR_OPEN_TRANSMIT", "");
    setEnv("CASCADE_PLUGIN_TEST", "");

    const std::string rowName = "fitted:row:" + kFile + kExt + ":name";
    const std::string keyName = "fitted:page:grant:granttransmit";
    Rect name;
    Rect key;

    // --- ASKS: the row, then the page; the key is offered to a module that has asked -----------------
    {
        installPlugin();
        const Result l = once("rows", 80, wait(60, 700), true);
        CHECK(l.ok);
        if (l.ok) {
            CHECK(l.rect(rowName) != nullptr);
            if (l.rect(rowName) != nullptr) { name = *l.rect(rowName); }
            // No grant has been given and none is saved by looking.
            CHECK(grantList(l.config) == "[]");
        }
        installPlugin();
        const Result p = once("page", 120, wait(30, 800) + click(40, name), true);
        CHECK(p.ok);
        if (p.ok) {
            if (p.rect(keyName) == nullptr) {
                // What the page drew, for the failure message.
                for (const std::string& it : p.items) {
                    if (it.rfind("fitted:", 0) == 0) { std::printf("      item %s\n", it.c_str()); }
                }
                std::printf("      log: %s\n", p.log.substr(0, 1500).c_str());
            }
            CHECK(p.rect(keyName) != nullptr);
            CHECK(p.has(keyName + ":off"));  // GRANT TRANSMIT: not granted yet
            if (p.rect(keyName) != nullptr) { key = *p.rect(keyName); }
            std::printf("    the module has asked: its page offers GRANT TRANSMIT at %.0f,%.0f\n",
                        key.cx(), key.cy());
            CHECK(grantList(p.config) == "[]");
            CHECK(!p.logHas("tx: plugin"));
        }
        // CONTROL: a module that has never wanted the transmitter is never offered the key.
        installPlugin();
        const Result quiet = once("quiet", 120, wait(30, 800) + click(40, name), false);
        CHECK(quiet.ok);
        if (quiet.ok) {
            CHECK(quiet.has("fitted:page:" + kFile + kExt));  // the page IS open...
            CHECK(quiet.rect(keyName) == nullptr);             // ...and offers no TRANSMIT key
            CHECK(!quiet.has(keyName + ":off") && !quiet.has(keyName + ":on"));
        }
    }

    // --- PRESS: the key saves the grant ------------------------------------------------------------
    {
        installPlugin();
        const Result r = once("press", 160, wait(30, 800) + click(40, name) + click(90, key), true);
        CHECK(r.ok);
        if (r.ok) {
            std::printf("    saved grant list: %s\n", grantList(r.config).c_str());
            CHECK(grantList(r.config) == "[\"" + kFile + kExt + "\"]");
            // A grant is a setting. It keys nothing, and arming is another thing on another page.
            CHECK(!r.logHas("tx: plugin"));
            CHECK(!r.logHas("tx: armed"));
        }
        // CONTROL: the run ends before the click's release, so nothing is saved.
        installPlugin();
        const Result before = once("press-before", 93, wait(30, 800) + click(40, name) + click(90, key), true);
        CHECK(before.ok);
        if (before.ok) { CHECK(grantList(before.config) == "[]"); }
    }

    // --- REMEMBER, then REVOKE: the next launch reads the grant, the key undoes it ------------------
    {
        installPlugin();
        // The first launch gives the grant; the second finds it already given.
        Result gave = once("kept", 160, wait(30, 800) + click(40, name) + click(90, key), true);
        CHECK(gave.ok);
        const Result next = once("kept", 120, wait(30, 800) + click(40, name), true, true);
        CHECK(next.ok);
        if (next.ok) {
            if (!next.has(keyName + ":on")) {
                for (const std::string& it : next.items) {
                    if (it.rfind("fitted:", 0) == 0) { std::printf("      item %s\n", it.c_str()); }
                }
                std::printf("      config: %s\n", next.config.substr(0, 1500).c_str());
            }
            CHECK(next.has(keyName + ":on"));  // REVOKE TRANSMIT
            CHECK(grantList(next.config) == "[\"" + kFile + kExt + "\"]");
            CHECK(!next.logHas("tx: plugin"));  // a restored grant keys nothing
        }
        const Result revoked =
            once("kept", 160, wait(30, 800) + click(40, name) + click(90, key), true, true);
        CHECK(revoked.ok);
        if (revoked.ok) { CHECK(grantList(revoked.config) == "[]"); }
    }

    std::error_code ec2;
    fs::remove_all(g_dir, ec2);
    return testSummary("test_tx_audio_plate_app");
}
