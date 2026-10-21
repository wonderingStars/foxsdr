// test_tx_audio_page_app.cpp - the TRANSMIT page's plugin controls, drawn and pressed through the
// REAL cascade.exe (0.99.74, CASCADE_CAP_TX_AUDIO).
//
// WHAT THIS ADDS TO tests/test_tx_audio_app.cpp. That test drives the key's logic through the real
// application window but calls the page's actions directly. This one starts the program, opens
// the TRANSMIT page (FOXSDR_OPEN_TRANSMIT, a bounded-run seam), and checks what the page DREW
// and what a scripted click on a drawn key did - so the ImGui code that has the ARM FOR PLUGINS
// keys, the OFF key and the DATA (flat) tick in it has actually run, and the keys are where the
// census says they are.
//
//   draw     the page lists ARM FOR PLUGINS 5 / 15 / 60 MIN and OFF, the DATA (flat) tick (on by
//            default) and says the page is not armed; nothing is refused yet
//   refuse   a click on 5 MIN with NO transmit radio open is REFUSED, with the reason on the page,
//            and nothing is armed (no "tx: armed" line in the log)
//   tick     a click on DATA (flat) turns it off; another turns it on again
//
// Nothing here transmits: no radio is open in any run, and the saved settings file is the
// run's own scratch copy.
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
    bool has(const std::string& item) const { return items.count(item) != 0; }
    const Rect* rect(const std::string& name) const {
        const auto it = rects.find(name);
        return it == rects.end() ? nullptr : &it->second;
    }
    bool logHas(const std::string& needle) const { return log.find(needle) != std::string::npos; }
};

fs::path g_dir;

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

void writeFile(const fs::path& p, const std::string& body) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

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
    writeFile(cfg,
              "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": "
              "\"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", \"mainView\": "
              "\"receiver\", \"railBank\": 0, \"bandPlanOverlay\": false }\n");
    const std::string scriptPath = (g_dir / (tag + ".script")).string();
    writeFile(scriptPath, script);
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    setEnv("FOXSDR_INPUT_SCRIPT", script.empty() ? std::string() : scriptPath);
    setEnv("FOXSDR_SCRIPT_TRACE", (g_dir / (tag + ".trace")).string());
    setEnv("FOXSDR_OPEN_TRANSMIT", "1");
    r.out = run("\"" + exePath() + "\" --frames " + std::to_string(frames) + " 2>&1");
    r.log = readTree(diag);
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

// A click at the centre of `at`: the pointer arrives at `frame`, goes down two frames later and up
// two after that - the release at frame + 4 is what presses a key.
std::string click(int frame, const Rect& at) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d screen %.1f %.1f\n%d down\n%d up\n", frame, at.cx(), at.cy(),
                  frame + 2, frame + 4);
    return buf;
}

}  // namespace

int main() {
    std::printf("test_tx_audio_page_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-txpage-" + std::to_string(pid()));
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
    setEnv("CASCADE_DECODE_TEST", "");
    setEnv("CASCADE_PLUGIN_TEST", "");

    // --- DRAW: the page, as it opens ------------------------------------------------------------
    Rect arm5, dataFlat;
    {
        const Result r = once("draw", 60, "");
        CHECK(r.ok);
        if (r.ok) {
            for (const char* item : {"transmit:arm5", "transmit:arm15", "transmit:arm60",
                                     "transmit:armoff", "transmit:dataflat:on", "transmit:notarmed"}) {
                if (!r.has(item)) { std::printf("    missing census item %s\n", item); }
                CHECK(r.has(item));
            }
            CHECK(!r.has("transmit:armed"));
            CHECK(!r.has("transmit:dataflat:off"));
            // A page that has not been pressed refuses nothing, and nothing is armed in its log.
            CHECK(!r.has("transmit:refusal1") && !r.has("transmit:refusal2") &&
                  !r.has("transmit:refusal3"));
            CHECK(!r.logHas("tx: armed"));
            CHECK(!r.logHas("tx: plugin"));
            CHECK(r.rect("transmit:arm5") != nullptr);
            CHECK(r.rect("transmit:dataflat") != nullptr);
            if (r.rect("transmit:arm5") != nullptr) { arm5 = *r.rect("transmit:arm5"); }
            if (r.rect("transmit:dataflat") != nullptr) { dataFlat = *r.rect("transmit:dataflat"); }
            std::printf("    drawn: 5 MIN at %.0f,%.0f; DATA (flat) at %.0f,%.0f\n", arm5.cx(),
                        arm5.cy(), dataFlat.cx(), dataFlat.cy());
        }
    }

    // --- REFUSE: 5 MIN with no transmit radio open ----------------------------------------------
    {
        const Result r = once("refuse", 60, click(30, arm5));
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.has("transmit:refusal1"));        // TxArming::Refusal::NoRadio
            CHECK(r.has("transmit:notarmed"));
            CHECK(!r.has("transmit:armed"));
            CHECK(!r.logHas("tx: armed"));            // nothing was armed
        }
        // CONTROL: the run ends before the release of the click, so nothing is refused yet.
        const Result before = once("refuse-before", 33, click(30, arm5));
        CHECK(before.ok);
        if (before.ok) { CHECK(!before.has("transmit:refusal1")); }
    }

    // --- TICK: DATA (flat) is a real checkbox ----------------------------------------------------
    {
        const Result r = once("tick", 60, click(30, dataFlat));
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.has("transmit:dataflat:off"));
            CHECK(!r.has("transmit:dataflat:on") || r.has("transmit:dataflat:off"));
        }
        std::string twice = click(30, dataFlat) + click(40, dataFlat);
        const Result t = once("tick2", 70, twice);
        CHECK(t.ok);
        if (t.ok) { CHECK(t.has("transmit:dataflat:on")); }
    }

    std::error_code ec2;
    fs::remove_all(g_dir, ec2);
    return testSummary("test_tx_audio_page_app");
}
