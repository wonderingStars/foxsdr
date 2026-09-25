/*
 * THE DECK'S BIAS TEE KEY, PRESSED IN THE REAL APPLICATION.
 *
 * WHY THIS EXISTS (repair round 1 of the deck key, 2026-09-25). The review of
 * 97815fc drove two mutants past every test the change had: the drawn key
 * calling switchBiasTee directly instead of biasKeyPressed, and the queued
 * question answering itself "yes" instead of opening the dialog. Both put
 * power on the connector from one stray click, which is the one thing the key
 * was designed not to do - and both passed, because test_bias_key_app calls
 * the members the drawing code is SUPPOSED to call, and nothing checked that
 * the drawing code calls them. This runs cascade.exe itself and presses the
 * key the way a user does:
 *
 *   run 0  no input: where the key is drawn (the census's "deck:bias");
 *   run 1  one click on the key: the confirmation dialog is up, and the
 *          stand-in bias tee is STILL OFF on every frame of the run;
 *   run 2  a click on the key, then one on the dialog's "Turn it on": the
 *          lamp lights.
 *
 * The radio is the census/capture stand-in (FOXSDR_FORCE_BIAS_KEY=accept,
 * read only in a --frames run), the clicks are the application's own input
 * script (FOXSDR_INPUT_SCRIPT - nothing outside the process is touched), and
 * what happened is read back from the interface census: "deck:bias.lit" is
 * noted on any frame the lamp is lit, "dialog:bias_key_confirm" on any frame
 * the dialog is drawn, and the "Turn it on" key's rectangle is recorded.
 *
 * Isolated like every other test that starts the application: its own config,
 * APPDATA / LOCALAPPDATA / XDG dirs in a scratch folder, every report URL at a
 * dead port, the signal generator as the source.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
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
    ::SetEnvironmentVariableA(name, value.c_str());
    _putenv_s(name, value.c_str());
#else
    ::setenv(name, value.c_str(), 1);
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
    float cx() const { return (x0 + x1) * 0.5f; }
    float cy() const { return (y0 + y1) * 0.5f; }
};

struct Census {
    bool ok = false;
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
};

constexpr int kFrames = 60;
fs::path g_dir;

// One bounded run of the application with `script` (empty: none) as its
// input, the census on, and the stand-in bias tee behind the key.
Census once(const std::string& tag, const std::string& script) {
    Census c;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path outFile = g_dir / (tag + ".census");
    {
        std::ofstream f(cfg);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
             "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\" }\n";
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", outFile.string());
    if (script.empty()) {
        setEnv("FOXSDR_INPUT_SCRIPT", "");
    } else {
        const fs::path sp = g_dir / (tag + ".script");
        std::ofstream f(sp);
        f << script;
        f.close();
        setEnv("FOXSDR_INPUT_SCRIPT", sp.string());
    }
    const std::string out =
        run("\"" + exePath() + "\" --frames " + std::to_string(kFrames) + " 2>&1");
    const bool rendered =
        out.find("rendered " + std::to_string(kFrames) + " frames") != std::string::npos;
    const bool written = out.find("ui census written") != std::string::npos;
    const bool scripted = script.empty() || out.find("script steps run") != std::string::npos;
    if (!rendered || !written || !scripted) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), out.c_str());
        return c;
    }
    std::ifstream in(outFile);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.rfind("item ", 0) == 0) {
            c.items.insert(line.substr(5));
        } else if (line.rfind("rect ", 0) == 0) {
            std::istringstream ss(line.substr(5));
            std::string name;
            Rect r;
            ss >> name >> r.x0 >> r.y0 >> r.x1 >> r.y1;
            c.rects[name] = r;
        }
    }
    c.ok = !c.items.empty();
    return c;
}

std::string click(int frame, float x, float y) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%d screen %.0f %.0f\n%d down\n%d up\n", frame, x, y,
                  frame + 2, frame + 4);
    return buf;
}

}  // namespace

int main() {
    g_dir = fs::temp_directory_path() / ("cascade-bias-key-run-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir / "appdata", ec);
    fs::create_directories(g_dir / "local", ec);
    fs::create_directories(g_dir / "diag", ec);
    setEnv("APPDATA", (g_dir / "appdata").string());
    setEnv("LOCALAPPDATA", (g_dir / "local").string());
    setEnv("XDG_CONFIG_HOME", (g_dir / "appdata").string());
    setEnv("XDG_DATA_HOME", (g_dir / "local").string());
    setEnv("XDG_STATE_HOME", (g_dir / "local").string());
    setEnv("FOXSDR_DIAG_DIR", (g_dir / "diag").string());
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL",
                            "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("FOXSDR_SINGLE_VIEWPORT", "1");
    setEnv("FOXSDR_WINDOW_SIZE", "1280x720");
    setEnv("FOXSDR_FORCE_BIAS_KEY", "accept");

    // run 0: where the key is.
    const Census where = once("locate", "");
    CHECK(where.ok);
    const auto key = where.rects.find("deck:bias");
    CHECK(key != where.rects.end());
    CHECK(where.items.count("deck:bias.lit") == 0);  // the stand-in starts off
    if (!where.ok || key == where.rects.end()) { return testSummary("test_bias_key_run"); }
    // The key square is the census rectangle's left end: its height, square.
    const Rect& k = key->second;
    const float kx = k.x0 + (k.y1 - k.y0) * 0.5f;
    const float ky = k.cy();
    std::printf("  the key at (%.0f, %.0f)\n", kx, ky);

    // run 1: ONE CLICK on the key. The dialog must be up, and the lamp must
    // not have lit on any frame.
    const Census one = once("press", click(10, kx, ky));
    CHECK(one.ok);
    const bool asked = one.items.count("dialog:bias_key_confirm") == 1;
    const bool litByOneClick = one.items.count("deck:bias.lit") == 1;
    std::printf("  one click: dialog %s, lamp %s\n", asked ? "UP" : "not drawn",
                litByOneClick ? "LIT - power from one click" : "still off");
    CHECK(asked);
    CHECK(!litByOneClick);
    const auto yes = one.rects.find("dialog:bias_key_confirm.yes");
    CHECK(yes != one.rects.end());
    if (yes == one.rects.end()) { return testSummary("test_bias_key_run"); }
    std::printf("  \"Turn it on\" at (%.0f, %.0f)\n", yes->second.cx(), yes->second.cy());

    // run 2: the click, then "Turn it on": the lamp lights.
    const Census two =
        once("confirm", click(10, kx, ky) + click(30, yes->second.cx(), yes->second.cy()));
    CHECK(two.ok);
    const bool litAfterYes = two.items.count("deck:bias.lit") == 1;
    std::printf("  click and \"Turn it on\": lamp %s\n", litAfterYes ? "lit" : "STILL OFF");
    CHECK(two.items.count("dialog:bias_key_confirm") == 1);
    CHECK(litAfterYes);

    const int rc = testSummary("test_bias_key_run");
    if (rc == 0) { fs::remove_all(g_dir, ec); }
    return rc;
}
