/*
 * THE DISPLAY SECTION STARTS FOLDED, LIVES IN SYSTEM AND REMEMBERS (0.99.74), IN
 * THE REAL APPLICATION. A tester: "When I open FoxSDR and go to the VIEW tab,
 * the DISPLAY menu is always expanded. Can you have this default to closed or
 * remember the last status. I am going to the VIEW tab often to use the Airband
 * feature and I always have to collapse the DISPLAY menu." and, in the same
 * report: "the DISPLAY settings could be moved to the SYSTEM tab as all of the
 * other items listed on the VIEW tab are more than just view options".
 *
 * tests/test_config.cpp pins the config field and tests/test_rail_banks.cpp the
 * bank's name; this runs cascade itself at the SYSTEM bank (railBank 4) and the
 * TOOLS bank (railBank 2) and reads the UI census and the config it saved:
 *
 *   fresh     a config that never heard of displayOpen: the Display header is
 *             drawn, FIRST among SYSTEM's sections, and nothing of its body is
 *             (the census note "display:body" is made only inside the open
 *             section); the config saves displayOpen false;
 *   open      "displayOpen": true: the body is drawn from the first frame, and
 *             the config still says open when the run ends;
 *   forced    FOXSDR_OPEN_SECTIONS=Display, the seam the screenshot runs use,
 *             still opens a fresh (folded-by-default) Display section;
 *   tools     the TOOLS bank: no Display section there, every other section
 *             that bank always had, and a saved "displayOpen": true is not
 *             lost by running a bank that does not draw it;
 *   click     a pointer press on the folded header opens it, and the config
 *             saves displayOpen true: that is the remembered state, from the
 *             tester's side of the screen;
 *   unclick   the same press on an open header folds it and the config saves
 *             false.
 *
 * Isolated like every other test that starts the application.
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

#include "core/config.hpp"
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
    float cx() const { return (x0 + x1) * 0.5f; }
    float cy() const { return (y0 + y1) * 0.5f; }
};

struct Result {
    bool ok = false;
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
    bool displayOpen = false;  // what the application saved
    std::string out;
};

constexpr int kFrames = 70;
fs::path g_dir;

// One run at `bank`, with `extraConfig` appended to a minimal config, the
// section seam `openSections` (empty = off) and `script` (empty = no input).
Result once(const std::string& tag, int bank, const std::string& extraConfig,
            const std::string& openSections, const std::string& script) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
             "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", "
             "\"language\": \"en\", \"mainView\": \"receiver\", \"railBank\": "
          << bank << extraConfig << " }\n";
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_OPEN_SECTIONS", openSections);
    if (script.empty()) {
        setEnv("FOXSDR_INPUT_SCRIPT", "");
    } else {
        const fs::path sp = g_dir / (tag + ".script");
        std::ofstream f(sp);
        f << script;
        f.close();
        setEnv("FOXSDR_INPUT_SCRIPT", sp.string());
    }
    r.out = run("\"" + exePath() + "\" --frames " + std::to_string(kFrames) + " 2>&1");
    const bool rendered =
        r.out.find("rendered " + std::to_string(kFrames) + " frames") != std::string::npos;
    const bool written = r.out.find("ui census written") != std::string::npos;
    const bool scripted = script.empty() || r.out.find("script steps run") != std::string::npos;
    if (!rendered || !written || !scripted) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), r.out.c_str());
        return r;
    }
    {
        std::ifstream in(census);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("item ", 0) == 0) { r.items.insert(line.substr(5)); }
            if (line.rfind("rect ", 0) != 0) { continue; }
            std::istringstream ss(line.substr(5));
            std::string name;
            Rect rc;
            ss >> name >> rc.x0 >> rc.y0 >> rc.x1 >> rc.y1;
            r.rects[name] = rc;
        }
    }
    cascade::core::AppConfig saved;
    std::string err;
    if (!cascade::core::ConfigStore::load(cfg.string(), saved, err)) {
        std::printf("  %s: the saved config did not load: %s\n", tag.c_str(), err.c_str());
        return r;
    }
    r.displayOpen = saved.displayOpen;
    r.ok = true;
    return r;
}

// A press and release on the middle of a header row: the row's own item, not
// the small key at its left end or the chip at its right.
std::string press(int frame, const Rect& at) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%d screen %.1f %.1f\n%d down\n%d up\n", frame, at.cx(),
                  at.cy(), frame + 2, frame + 4);
    return buf;
}

const char* const kDisplayHeader = "section:Display";  // the rect's name: the item's, in English
bool shown(const Result& r) { return r.items.count("display:body") == 1; }

}  // namespace

int main() {
    std::printf("test_display_fold_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-display-fold-" + std::to_string(pid()));
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
    setEnv("USERPROFILE", (g_dir / "home").string());
    setEnv("HOME", (g_dir / "home").string());
    setEnv("FOXSDR_DIAG_DIR", (g_dir / "diag").string());
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL",
                            "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("FOXSDR_SINGLE_VIEWPORT", "1");
    setEnv("FOXSDR_WINDOW_SIZE", "1600x1000");
    // The census walks the rail through every bank unless told to hold the one
    // the config chose - and holding it is the point here.
    setEnv("FOXSDR_CENSUS_HOLD_BANK", "1");
    setEnv("FOXSDR_PATCH_START", "");
    setEnv("FOXSDR_PATCH_FILE", "");
    setEnv("CASCADE_DECODE_TEST", "");

    // --- fresh: folded, first in SYSTEM -----------------------------------------
    const Result fresh = once("fresh", 4, "", "", "");
    CHECK(fresh.ok);
    CHECK(fresh.items.count("bank:4") == 1);
    // The header is there to be opened...
    CHECK(fresh.items.count("section:Display") == 1);
    CHECK(fresh.rects.count(kDisplayHeader) == 1);
    // ...and its body is not drawn: THE TESTER'S FIRST REQUEST. (Break-it: pass
    // true as benchSection's defaultOpen again and this goes red.)
    CHECK(!shown(fresh));
    CHECK(!fresh.displayOpen);
    // FIRST in the bank: every other section's header is below it.
    int others = 0;
    if (fresh.rects.count(kDisplayHeader) == 1) {
        const Rect d = fresh.rects.at(kDisplayHeader);
        for (const auto& [name, rc] : fresh.rects) {
            if (name.rfind("section:", 0) != 0 || name == kDisplayHeader) { continue; }
            ++others;
            if (rc.y0 <= d.y0) { std::printf("    %s is not below Display\n", name.c_str()); }
            CHECK(rc.y0 > d.y0);
        }
    }
    std::printf("    fresh: Display folded, %d other SYSTEM sections below it\n", others);
    CHECK(others >= 3);  // Language, Updates, Key bindings, ...: a bank, not a lone row

    // --- open: remembered from the config ---------------------------------------
    const Result open = once("open", 4, ", \"displayOpen\": true", "", "");
    CHECK(open.ok);
    CHECK(shown(open));
    CHECK(open.displayOpen);  // and a run that never touches it does not lose it

    // --- forced: the screenshot seam still opens it -----------------------------
    const Result forced = once("forced", 4, "", "Display", "");
    CHECK(forced.ok);
    CHECK(shown(forced));

    // --- tools: Display is gone from the bank, the rest is not ------------------
    const Result tools = once("tools", 2, ", \"displayOpen\": true", "", "");
    CHECK(tools.ok);
    CHECK(tools.items.count("bank:2") == 1);
    CHECK(tools.items.count("section:Display") == 0);
    CHECK(!shown(tools));
    for (const char* kept : {"switch:Demod scope###demodscope", "section:Radar",
                             "section:Bookmarks", "section:Scanner", "section:Airband###airband"}) {
        if (tools.items.count(kept) != 1) { std::printf("    TOOLS lost %s\n", kept); }
        CHECK(tools.items.count(kept) == 1);
    }
    // The section was not drawn, so what the user left is what is saved.
    CHECK(tools.displayOpen);

    // --- click: the remembered state, from the tester's side ---------------------
    if (fresh.rects.count(kDisplayHeader) == 1) {
        const Result click = once("click", 4, "", "", press(20, fresh.rects.at(kDisplayHeader)));
        CHECK(click.ok);
        CHECK(shown(click));
        CHECK(click.displayOpen);  // (Break-it: drop the write-back and this goes red.)
    }
    if (open.rects.count(kDisplayHeader) == 1) {
        const Result unclick = once("unclick", 4, ", \"displayOpen\": true", "",
                                    press(20, open.rects.at(kDisplayHeader)));
        CHECK(unclick.ok);
        CHECK(!unclick.displayOpen);
    }

    const int rc = testSummary("test_display_fold_app");
    if (rc == 0) { fs::remove_all(g_dir, ec); }
    return rc;
}
