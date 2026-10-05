/*
 * THE AIRBAND SECTION, DRIVEN THROUGH THE KEYS A USER PRESSES (the airband
 * request, 2026-10). Runs cascade itself, isolated like every test that starts
 * the application, with a scripted pointer and keyboard, and checks what the
 * UI census saw and what the run saved:
 *
 *   lookup    "ORD" typed into the code field and Enter pressed: O'Hare's
 *             frequencies land in the frequency list (bookmarks.json) as
 *             their own group, mode AM, ticked - ATIS left unticked - and
 *             the section shows them;
 *   listen    two ticked AM rows 600 kHz apart, LISTEN pressed: the monitor
 *             tunes the receiver to the one centre that holds both (120.000
 *             MHz, between them), the signal generator's tone 300 kHz above
 *             the centre opens 120.300's squelch and lights its lamp while
 *             119.700 stays dark, and the time 120.300 was heard is saved
 *             into the list at exit - and 119.700's is not;
 *   hand-back the same with the PATCH view showing: LISTEN switches to the
 *             receiver view (the patch has the receiver's radio) and the
 *             monitor starts once it is handed back.
 *
 * Every click is aimed at the rectangle a first, unscripted run of the same
 * layout reported for that key, so nothing here depends on where the rail
 * happens to put the section.
 *
 * REAL TIME, NOT FRAMES (2026-10). "Heard" is seconds of the wall clock, and
 * the radio, the patch runner and the squelch work in real time on their own
 * threads, so how long a listen lasts is how long the run takes - and a bounded
 * run's frames are not a clock. With the display pacing them (vsync) the 299
 * frames of the listen run last 2.5 s and the monitor heard 1.8 to 2.4 s; with
 * the desktop not presenting (display idle or asleep, the window covered, a
 * remote session) they last 0.26 s and it heard 0.03 to 0.18 s. The same
 * listening passed or failed on the display alone. The two listen runs
 * therefore hold the frame loop for real time themselves (the script's `sleep`
 * step, see hold()) and say so in the check: the run must have lasted that
 * long, so a script whose holds were lost fails on that, not on "heard".
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

#include "core/freq_manager.hpp"
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
};

fs::path g_dir;

// The rail is a scrolling column and the Airband section sits at the foot of
// the VIEW bank, below the fold of a 1000-pixel window. Every run - the layout
// pass and the scripted one alike - scrolls it the same way at the same frame,
// after the layout has settled, so the rectangles the layout pass reports are
// where the keys are when the script presses them.
constexpr const char* kScroll = "30 screen 200 600\n32 wheel -30\n";

fs::path bookmarksPath() { return g_dir / "appdata" / "foxsdr" / "bookmarks.json"; }

void writeBookmarks(const std::string& body) {
    std::error_code ec;
    fs::create_directories(bookmarksPath().parent_path(), ec);
    std::ofstream f(bookmarksPath(), std::ios::binary | std::ios::trunc);
    f << body;
}

// One run of the application. `script` empty = no scripted input.
Result once(const std::string& tag, int frames, const std::string& mainView, const std::string& script) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
             "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", "
             "\"mainView\": \""
          << mainView << "\", \"railBank\": 2, \"bandPlanOverlay\": false }\n";
    }
    const std::string scriptPath = (g_dir / (tag + ".script")).string();
    {
        std::ofstream f(scriptPath, std::ios::binary | std::ios::trunc);
        f << kScroll << script;
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_INPUT_SCRIPT", scriptPath);
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
        std::string name;
        Rect rc;
        ss >> name >> rc.x0 >> rc.y0 >> rc.x1 >> rc.y1;
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

// Real time held inside the script after LISTEN: kHoldSteps sleeps of kHoldMs,
// ten frames apart from `first`, 2 s in all - about what the display gave the
// run before (heard 1.8 to 2.4 s) and four times the threshold below.
//
// EACH SLEEP MUST BE WELL UNDER 0.5 S. The script sleeps after the frame's
// time step has been taken, so the sleep shows up as the next frame's elapsed
// time, and the monitor treats a runner whose block count has not moved for
// 0.5 s (kAirbandStaleS) as stalled and drops that frame's squelch reports.
// Six holds of 500 ms lost two to four of their frames that way (measured:
// "live" false on them, though the runner's block count had risen by about a
// thousand over each); 250 ms keeps the monitor's view whole. Nor may one
// exceed the one second a single frame is allowed to credit.
constexpr int kHoldSteps = 8;
constexpr int kHoldMs = 250;
constexpr double kHoldS = kHoldSteps * kHoldMs / 1000.0;

std::string hold(int first) {
    std::string s;
    for (int i = 0; i < kHoldSteps; ++i) {
        s += std::to_string(first + 10 * i) + " sleep " + std::to_string(kHoldMs) + "\n";
    }
    return s;
}

// The last frame time (seconds, the application's own clock) a run's script
// trace recorded; -1 when there is none.
double traceEnd(const std::string& tag) {
    std::ifstream in(g_dir / (tag + ".trace"));
    std::string line, last;
    while (std::getline(in, line)) {
        if (!line.empty()) { last = line; }
    }
    const std::size_t at = last.find(" t=");
    return at == std::string::npos ? -1.0 : std::atof(last.c_str() + at + 3);
}

const char* kTwoRows =
    "{\n"
    "  \"schemaVersion\": 1,\n"
    "  \"bookmarks\": [\n"
    "    { \"name\": \"Low\", \"freqHz\": 119700000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0,\n"
    "      \"scan\": true },\n"
    "    { \"name\": \"High\", \"freqHz\": 120300000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0,\n"
    "      \"scan\": true },\n"
    "    { \"name\": \"Unticked\", \"freqHz\": 121500000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0 }\n"
    "  ]\n"
    "}\n";

void checkListen(const Result& r, const std::string& tag, const char* what) {
    std::printf("  %s\n", what);
    CHECK(r.ok);
    // The premise of the heard-time check below: the run lasted the real time
    // its script held, whatever the display did to its frames.
    const double ran = traceEnd(tag);
    std::printf("    ran %.2f s (script holds %.1f s)\n", ran, kHoldS);
    CHECK(ran >= kHoldS - 0.01);
    CHECK(r.items.count("airband:listening") == 1);
    CHECK(r.items.count("airband:blocks:1") == 1);
    // The one centre that holds both, between them: the generator's tone
    // 300 kHz above it is on 120.300.
    CHECK(r.items.count("airband:centre:120000") == 1);
    CHECK(r.items.count("airband:open:120300") == 1);
    CHECK(r.items.count("airband:open:119700") == 0);
    CHECK(r.items.count("airband:open:121500") == 0);
    CHECK(r.items.count("view:receiver") == 1);

    cascade::core::FreqManager m;
    std::string err;
    CHECK(m.load(bookmarksPath().string(), err));
    double heardLow = -1.0, heardHigh = -1.0;
    for (const cascade::core::Bookmark& b : m.list()) {
        if (b.name == "Low") { heardLow = b.heardSeconds; }
        if (b.name == "High") { heardHigh = b.heardSeconds; }
    }
    std::printf("    heard: High %.2f s, Low %.2f s\n", heardHigh, heardLow);
    CHECK(heardHigh > 0.5);
    CHECK(heardLow == 0.0);
}

}  // namespace

int main() {
    std::printf("test_airband_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-airband-" + std::to_string(pid()));
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
    setEnv("FOXSDR_WINDOW_SIZE", "1600x1000");
    setEnv("FOXSDR_OPEN_SECTIONS", "airband");
    // The census walks the rail through every bank unless told to hold the
    // one the config chose (VIEW, where the Airband section is).
    setEnv("FOXSDR_CENSUS_HOLD_BANK", "1");
    setEnv("FOXSDR_PATCH_START", "");
    setEnv("FOXSDR_PATCH_FILE", "");
    setEnv("CASCADE_DECODE_TEST", "");

    // --- lookup: type the code, press Enter --------------------------------
    {
        writeBookmarks("{ \"schemaVersion\": 1, \"bookmarks\": [] }\n");
        const Result layout = once("lookup-layout", 60, "receiver", "");
        CHECK(layout.ok);
        CHECK(layout.rects.count("airband:code") == 1);
        CHECK(layout.items.count("airband:rows:0") == 1);
        if (layout.rects.count("airband:code") == 1) {
            std::string s = click(60, layout.rects.at("airband:code"));
            s += "70 text ORD\n72 key enter\n";
            const Result r = once("lookup", 100, "receiver", s);
            CHECK(r.ok);
            CHECK(r.items.count("airband:rows:29") == 1);
            CHECK(r.items.count("airband:ticked:28") == 1);

            cascade::core::FreqManager m;
            std::string err;
            CHECK(m.load(bookmarksPath().string(), err));
            std::size_t n = 0, ticked = 0;
            bool atisUnticked = false, groundTicked = false, allAm = true;
            for (const cascade::core::Bookmark& b : m.list()) {
                if (b.group != "KORD Chicago O'Hare International Airport") { continue; }
                ++n;
                ticked += b.scan ? 1u : 0u;
                allAm = allAm && b.mode == "AM" && b.bandwidthHz == 10000.0;
                if (b.freqHz == 135400000.0) { atisUnticked = !b.scan; }
                if (b.freqHz == 121900000.0) { groundTicked = b.scan; }
            }
            std::printf("    saved: %zu O'Hare rows, %zu ticked\n", n, ticked);
            CHECK(n == 29);
            CHECK(ticked == 28);
            CHECK(atisUnticked);
            CHECK(groundTicked);
            CHECK(allAm);
        }
    }

    // --- listen: two ticked rows, LISTEN pressed, on the receiver view ------
    {
        writeBookmarks(kTwoRows);
        const Result layout = once("listen-layout", 60, "receiver", "");
        CHECK(layout.ok);
        CHECK(layout.items.count("airband:rows:2") == 1);
        CHECK(layout.rects.count("airband:listen") == 1);
        if (layout.rects.count("airband:listen") == 1) {
            writeBookmarks(kTwoRows);
            const Result r = once("listen", 300, "receiver",
                                  click(60, layout.rects.at("airband:listen")) + hold(70));
            checkListen(r, "listen", "listen on the receiver view");
        }
    }

    // --- hand-back: the same, with the patch view showing -------------------
    {
        writeBookmarks(kTwoRows);
        const Result layout = once("handback-layout", 60, "patch", "");
        CHECK(layout.ok);
        CHECK(layout.rects.count("airband:listen") == 1);
        if (layout.rects.count("airband:listen") == 1) {
            writeBookmarks(kTwoRows);
            const Result r = once("handback", 360, "patch",
                                  click(60, layout.rects.at("airband:listen")) + hold(70));
            checkListen(r, "handback", "listen pressed on the patch view");
        }
    }

    const int rc = testSummary("test_airband_app");
    if (rc == 0) { fs::remove_all(g_dir, ec); }
    return rc;
}
