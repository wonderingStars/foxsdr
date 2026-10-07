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
 *             monitor starts once it is handed back;
 *   nfm       (0.99.66) the same with one row AM and one NFM, and a ticked WFM
 *             row that is neither listed nor played;
 *   add row   (0.99.66) a frequency typed into the add row - "121.500", NFM
 *             picked from the mode list - lands in bookmarks.json as a ticked
 *             NFM row in the group "Manual", named by its frequency; a decimal
 *             comma and a typed name are read; text that is no frequency adds
 *             nothing;
 *   spectrum  (0.99.66) while it listens the receiver's spectrum shows the
 *             channels heard - a mark per channel of the block, noted bright
 *             for the open one - and not the VFO's band, which is drawn
 *             again, on the last frame, once STOP has been pressed;
 *   one channel per frequency   (0.99.66) an AM and an NFM row ticked on one
 *             frequency are one channel, named and credited to the first;
 *   presets   (0.99.66) the preset field typed over ("Marine") names the group
 *             an added frequency joins; a CSV whose path is typed into the
 *             import field and Import pressed lands in the preset, its AM and
 *             NFM rows ticked, whatever group and tick the file gave it (a WFM
 *             row is kept, unticked); Remove preset
 *             takes the default preset's rows out; Export CSV writes the
 *             preset's rows - tick kept - into the recordings folder, which
 *             is %USERPROFILE%\Documents\SDR-recordings and so is pointed at
 *             this run's scratch tree (never the tester's own folder), and
 *             offers a key that opens the folder (found, never pressed).
 *             test_gui_file_jobs covers the same two workers in more depth,
 *             through a window of its own;
 *   remove while listening   (0.99.66 review) Remove preset pressed on the
 *             preset the monitor is playing stops the monitor - it is not cut
 *             again from "every ticked row" of the rest of the list;
 *   caption   (0.99.71) while it listens, the block on the air and the name
 *             heard are lettered at the top of the spectrum, and in the rail
 *             the two status lines sit above the row list - not under the
 *             keys, where O'Hare's twenty-nine rows put them below the fold
 *             (a tester, 2026-10-07): with O'Hare's list and LISTEN pressed
 *             the lines are on screen in a 1000-pixel window without the
 *             rail being scrolled further, and the caption is on the
 *             spectrum; idle, neither is drawn.
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

// ONE AM AND ONE NFM ROW, both ticked (0.99.66), and a ticked WFM row the
// monitor must not take: "High" is the NFM one, on the frequency the signal
// generator's tone is 300 kHz above the centre of.
const char* kAmAndNfm =
    "{\n"
    "  \"schemaVersion\": 1,\n"
    "  \"bookmarks\": [\n"
    "    { \"name\": \"Low\", \"freqHz\": 119700000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0,\n"
    "      \"scan\": true },\n"
    "    { \"name\": \"High\", \"freqHz\": 120300000.0, \"mode\": \"NFM\", \"bandwidthHz\": 12500.0,\n"
    "      \"scan\": true },\n"
    "    { \"name\": \"Broadcast\", \"freqHz\": 121800000.0, \"mode\": \"WFM\", \"bandwidthHz\": 150000.0,\n"
    "      \"scan\": true },\n"
    "    { \"name\": \"Unticked\", \"freqHz\": 121500000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0 }\n"
    "  ]\n"
    "}\n";

// AN AM AND AN NFM ROW TICKED ON ONE FREQUENCY (0.99.66), "High" first in the file
// and so first in the list among the two on 120.300: the monitor plays one channel
// for it, in the first row's mode and width.
const char* kSameFrequencyTwice =
    "{\n"
    "  \"schemaVersion\": 1,\n"
    "  \"bookmarks\": [\n"
    "    { \"name\": \"Low\", \"freqHz\": 119700000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0,\n"
    "      \"scan\": true },\n"
    "    { \"name\": \"High\", \"freqHz\": 120300000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0,\n"
    "      \"scan\": true },\n"
    "    { \"name\": \"HighNfm\", \"freqHz\": 120300000.0, \"mode\": \"NFM\", \"bandwidthHz\": 12500.0,\n"
    "      \"scan\": true }\n"
    "  ]\n"
    "}\n";

// The width on screen of the VFO's shaded band on the run's LAST frame, as
// drawCenterPanels reports it ("rx:vfo", the band's rectangle, or an empty one
// while the AIRBAND monitor listens); -1 when the receiver view never drew.
float vfoRectWidth(const Result& r) {
    const auto it = r.rects.find("rx:vfo");
    return it == r.rects.end() ? -1.0f : it->second.x1 - it->second.x0;
}

// THE RAIL'S STATUS LINES ARE ABOVE THE ROW LIST (0.99.71): on the run's last
// frame, still listening, the block line and the "Hearing:" line are both drawn,
// both end above the list's top edge, and the second follows the first. Until
// 0.99.71 they were the last lines of the section, under the keys.
void checkStatusAboveList(const Result& r) {
    const auto status = r.rects.find("airband:status");
    const auto hearing = r.rects.find("airband:hearing");
    const auto rows = r.rects.find("airband:rowlist");
    CHECK(status != r.rects.end());
    CHECK(hearing != r.rects.end());
    CHECK(rows != r.rects.end());
    if (status == r.rects.end() || hearing == r.rects.end() || rows == r.rects.end()) { return; }
    std::printf("    rail: status y %.1f..%.1f, hearing y %.1f..%.1f, row list y %.1f..%.1f\n",
                static_cast<double>(status->second.y0), static_cast<double>(status->second.y1),
                static_cast<double>(hearing->second.y0), static_cast<double>(hearing->second.y1),
                static_cast<double>(rows->second.y0), static_cast<double>(rows->second.y1));
    CHECK(status->second.y1 <= rows->second.y0 + 0.01f);
    CHECK(hearing->second.y0 >= status->second.y1 - 0.01f);
    CHECK(hearing->second.y1 <= rows->second.y0 + 0.01f);
}

// `centreKhz` is where the one block's centre lands. Two 10 kHz rows put it
// halfway between them, 120 000 kHz; an NFM row 12.5 kHz wide beside a 10 kHz
// AM one has a wider upper edge, so the middle of the block's extent
// (119.695 to 120.30625 MHz) is 120.000625 MHz - 120 001 kHz in the census,
// which rounds to the kilohertz.
void checkListen(const Result& r, const std::string& tag, const char* what, int centreKhz = 120000) {
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
    CHECK(r.items.count("airband:centre:" + std::to_string(centreKhz)) == 1);
    CHECK(r.items.count("airband:open:120300") == 1);
    CHECK(r.items.count("airband:open:119700") == 0);
    CHECK(r.items.count("airband:open:121500") == 0);
    CHECK(r.items.count("view:receiver") == 1);

    // THE SPECTRUM SHOWS WHAT IS HEARD (0.99.66): the open channel's mark is
    // drawn bright (a census note), the shut one's is not noted, and the VFO's
    // band is suppressed - by the note, and by the LAST frame of the run (still
    // listening) having an empty VFO rectangle.
    CHECK(r.items.count("airband:mark:120300") == 1);
    CHECK(r.items.count("airband:mark:119700") == 0);
    CHECK(r.items.count("airband:mark:121500") == 0);
    CHECK(r.items.count("airband:vfo-hidden") == 1);
    CHECK(vfoRectWidth(r) == 0.0f);
    // Both channels of the block have a mark, shut or open, where the channel
    // is: inside the spectrum, 120.300 right of 119.700, at least 3 px wide, and
    // - the receiver sits on the midpoint, unzoomed - the pair's midpoint on the
    // panel's middle.
    const auto lo = r.rects.find("airband:mk:119700");
    const auto hi = r.rects.find("airband:mk:120300");
    const auto spec = r.rects.find("trc:spec:receiver");
    CHECK(lo != r.rects.end());
    CHECK(hi != r.rects.end());
    CHECK(spec != r.rects.end());
    CHECK(r.rects.count("airband:mk:121500") == 0);
    if (lo != r.rects.end() && hi != r.rects.end() && spec != r.rects.end()) {
        std::printf("    marks: 119.700 at x %.1f (%.1f px), 120.300 at x %.1f (%.1f px), spectrum %.1f..%.1f\n",
                    static_cast<double>(lo->second.cx()), static_cast<double>(lo->second.x1 - lo->second.x0),
                    static_cast<double>(hi->second.cx()), static_cast<double>(hi->second.x1 - hi->second.x0),
                    static_cast<double>(spec->second.x0), static_cast<double>(spec->second.x1));
        CHECK(lo->second.cx() < hi->second.cx());
        CHECK(lo->second.x0 >= spec->second.x0 && hi->second.x1 <= spec->second.x1);
        CHECK(lo->second.x1 - lo->second.x0 >= 3.0f - 0.01f);
        CHECK(hi->second.x1 - hi->second.x0 >= 3.0f - 0.01f);
        CHECK(std::fabs(0.5f * (lo->second.cx() + hi->second.cx()) - spec->second.cx()) < 1.5f);
    }

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

    // THE CAPTION (0.99.71): on the run's last frame, still listening, the block
    // line is lettered on the spectrum - inside the panel, in its top third, at
    // least a few words wide - and the open channel's name was lettered with it
    // at some point of the run (the note). In the rail the two status lines sit
    // ABOVE the row list, the second straight under the first.
    const auto cap = r.rects.find("airband:caption");
    CHECK(cap != r.rects.end());
    CHECK(r.items.count("airband:caption:hearing") == 1);
    if (cap != r.rects.end() && spec != r.rects.end()) {
        std::printf("    caption: x %.1f..%.1f, y %.1f..%.1f (spectrum y %.1f..%.1f)\n",
                    static_cast<double>(cap->second.x0), static_cast<double>(cap->second.x1),
                    static_cast<double>(cap->second.y0), static_cast<double>(cap->second.y1),
                    static_cast<double>(spec->second.y0), static_cast<double>(spec->second.y1));
        CHECK(cap->second.x0 >= spec->second.x0 && cap->second.x1 <= spec->second.x1 + 0.01f);
        CHECK(cap->second.y0 >= spec->second.y0);
        CHECK(cap->second.y1 <= spec->second.y0 + (spec->second.y1 - spec->second.y0) / 3.0f);
        CHECK(cap->second.x1 - cap->second.x0 > 40.0f);
    }
    checkStatusAboveList(r);
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
    // The recordings folder is %USERPROFILE%\Documents\SDR-recordings (defaultRecordDir):
    // the export run below must write inside the scratch tree, not the tester's Documents.
    setEnv("USERPROFILE", (g_dir / "home").string());
    setEnv("HOME", (g_dir / "home").string());
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

            // THE TESTER'S CASE (0.99.71): O'Hare's rows are in the list now; LISTEN
            // pressed on them. The two status lines are above the row list and ON
            // SCREEN in this 1000-pixel window with the rail scrolled no further than
            // every run scrolls it - until 0.99.71 they were the last lines of the
            // section, under the keys, which with this list sit at pixel row 909 of
            // 1000 (measured on 0.99.70), so the lines were off the bottom of any
            // shorter window - and the spectrum carries the caption. Several blocks:
            // the generator's one tone opens no squelch here, so what is held is the
            // placing, not a name. (A fresh run does not remember the group the
            // lookup put on show, so it lists "every ticked AM or NFM frequency":
            // the 28 ticked rows, ATIS left out.)
            const Result layout29 = once("ord-layout", 60, "receiver", "");
            CHECK(layout29.ok);
            CHECK(layout29.items.count("airband:rows:28") == 1);
            CHECK(layout29.rects.count("airband:listen") == 1);
            if (layout29.rects.count("airband:listen") == 1) {
                const Rect key = layout29.rects.at("airband:listen");
                std::printf("  listen to O'Hare's list (29 rows; the LISTEN key is at y %.0f)\n",
                            static_cast<double>(key.y0));
                const Result r = once("ord-listen", 300, "receiver", click(60, key) + hold(70));
                CHECK(r.ok);
                CHECK(r.items.count("airband:listening") == 1);
                CHECK(r.items.count("airband:blocks:1") == 0);   // O'Hare spans 17 MHz: several blocks
                checkStatusAboveList(r);
                const auto hearing = r.rects.find("airband:hearing");
                if (hearing != r.rects.end()) { CHECK(hearing->second.y1 <= 1000.0f); }
                const auto cap = r.rects.find("airband:caption");
                const auto spec = r.rects.find("trc:spec:receiver");
                CHECK(cap != r.rects.end());
                CHECK(spec != r.rects.end());
                if (cap != r.rects.end() && spec != r.rects.end()) {
                    CHECK(cap->second.y0 >= spec->second.y0);
                    CHECK(cap->second.y1 <= spec->second.y0 + (spec->second.y1 - spec->second.y0) / 3.0f);
                    CHECK(cap->second.x0 >= spec->second.x0 && cap->second.x1 <= spec->second.x1 + 0.01f);
                }
            }
        }
    }

    // --- listen: two ticked rows, LISTEN pressed, on the receiver view ------
    {
        writeBookmarks(kTwoRows);
        const Result layout = once("listen-layout", 60, "receiver", "");
        CHECK(layout.ok);
        CHECK(layout.items.count("airband:rows:2") == 1);
        CHECK(layout.rects.count("airband:listen") == 1);
        // Not listening: the VFO's band is drawn as ever and nothing of the
        // monitor is on the spectrum.
        CHECK(layout.items.count("airband:vfo-hidden") == 0);
        CHECK(layout.items.count("airband:mark:120300") == 0);
        CHECK(layout.rects.count("airband:mk:120300") == 0);
        std::printf("  idle: the VFO band is %.1f px wide\n", static_cast<double>(vfoRectWidth(layout)));
        CHECK(vfoRectWidth(layout) > 1.0f);
        // Idle, nothing is said about hearing: no caption on the spectrum, no
        // status lines in the rail (0.99.71).
        CHECK(layout.rects.count("airband:caption") == 0);
        CHECK(layout.items.count("airband:caption:hearing") == 0);
        CHECK(layout.rects.count("airband:status") == 0);
        CHECK(layout.rects.count("airband:hearing") == 0);
        CHECK(layout.rects.count("airband:rowlist") == 1);
        if (layout.rects.count("airband:listen") == 1) {
            writeBookmarks(kTwoRows);
            const Result r = once("listen", 300, "receiver",
                                  click(60, layout.rects.at("airband:listen")) + hold(70));
            checkListen(r, "listen", "listen on the receiver view");
        }
    }

    // --- stop: the VFO's band comes back -------------------------------------
    // LISTEN, hold, then the same key again (it is STOP while listening, in the
    // same place), and run on. The census list is a set over the whole run, so
    // "vfo-hidden" is there for the frames that listened; what shows the band
    // back is the LAST frame's rectangle for it, which is empty only while the
    // monitor listens. A STOP that did not land would leave it empty.
    {
        writeBookmarks(kTwoRows);
        const Result layout = once("stop-layout", 60, "receiver", "");
        CHECK(layout.rects.count("airband:listen") == 1);
        if (layout.rects.count("airband:listen") == 1) {
            const Rect key = layout.rects.at("airband:listen");
            writeBookmarks(kTwoRows);
            const Result r = once("stop", 260, "receiver", click(60, key) + hold(70) + click(160, key));
            std::printf("  listen, then stop\n");
            CHECK(r.ok);
            CHECK(r.items.count("airband:listening") == 1);
            CHECK(r.items.count("airband:vfo-hidden") == 1);
            CHECK(r.items.count("airband:mark:120300") == 1);
            std::printf("    after stop the VFO band is %.1f px wide\n",
                        static_cast<double>(vfoRectWidth(r)));
            CHECK(vfoRectWidth(r) > 1.0f);
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

    // --- an AM row and an NFM row (0.99.66) -----------------------------------
    // Both ticked rows are listed and played - the same block, the same centre,
    // the NFM row's squelch opening on the generator's carrier and its lamp
    // lighting while the AM row's stays dark - and the ticked WFM row is neither
    // listed nor counted. What this cannot say is how the NFM channel SOUNDS:
    // the generator's tone is an unmodulated carrier, which opens a squelch in
    // any mode, so the audio of an NFM channel is measured in
    // test_airband_monitor, not here.
    {
        writeBookmarks(kAmAndNfm);
        const Result layout = once("nfm-layout", 60, "receiver", "");
        CHECK(layout.ok);
        CHECK(layout.items.count("airband:rows:2") == 1);
        CHECK(layout.items.count("airband:ticked:2") == 1);
        CHECK(layout.items.count("airband:rows:3") == 0);   // the WFM row is not a row
        CHECK(layout.items.count("airband:ticked:3") == 0);
        CHECK(layout.rects.count("airband:listen") == 1);
        if (layout.rects.count("airband:listen") == 1) {
            writeBookmarks(kAmAndNfm);
            const Result r = once("nfm", 300, "receiver",
                                  click(60, layout.rects.at("airband:listen")) + hold(70));
            checkListen(r, "nfm", "listen with one AM and one NFM row ticked", 120001);
            CHECK(r.items.count("airband:rows:2") == 1);
            CHECK(r.items.count("airband:ticked:2") == 1);
            CHECK(r.items.count("airband:open:121800") == 0);
        }
    }

    // --- one channel per frequency: an AM and an NFM row on one frequency (0.99.66) ----
    // 120.300 is ticked twice - "High" in AM, "HighNfm" in NFM - and 119.700 once. The
    // carrier is one transmitter, so the monitor plays TWO channels, not three; the first
    // ticked row on the frequency in the sorted list ("High", the AM one: the list keeps
    // ties in the order the file gave them) names the channel and is the one credited with
    // what was heard. Three channels would put the block's centre at 120.000625 (the NFM
    // row's wider edge) and credit both rows - or the wrong one.
    {
        writeBookmarks(kSameFrequencyTwice);
        const Result layout = once("firstwins-layout", 60, "receiver", "");
        CHECK(layout.ok);
        CHECK(layout.items.count("airband:rows:3") == 1);   // all three rows are listed
        CHECK(layout.items.count("airband:ticked:3") == 1);
        CHECK(layout.rects.count("airband:listen") == 1);
        if (layout.rects.count("airband:listen") == 1) {
            writeBookmarks(kSameFrequencyTwice);
            const Result r = once("firstwins", 300, "receiver",
                                  click(60, layout.rects.at("airband:listen")) + hold(70));
            checkListen(r, "firstwins", "listen with an AM and an NFM row ticked on one frequency");
            CHECK(r.items.count("airband:channels:2") == 1);
            CHECK(r.items.count("airband:channels:3") == 0);
            cascade::core::FreqManager m;
            std::string err;
            CHECK(m.load(bookmarksPath().string(), err));
            double heardAm = -1.0, heardNfm = -1.0;
            for (const cascade::core::Bookmark& b : m.list()) {
                if (b.name == "High") { heardAm = b.heardSeconds; }
                if (b.name == "HighNfm") { heardNfm = b.heardSeconds; }
            }
            std::printf("    heard: High (AM, first) %.2f s, HighNfm (second) %.2f s\n", heardAm, heardNfm);
            CHECK(heardAm > 0.5);
            CHECK(heardNfm == 0.0);
        }
    }

    // --- the add row: a frequency typed in joins the list as "Manual" ----------
    {
        writeBookmarks("{ \"schemaVersion\": 1, \"bookmarks\": [] }\n");
        const Result layout = once("add-layout", 60, "receiver", "");
        CHECK(layout.ok);
        CHECK(layout.rects.count("airband:addfreq") == 1);
        CHECK(layout.rects.count("airband:addname") == 1);
        CHECK(layout.rects.count("airband:mode") == 1);
        CHECK(layout.rects.count("airband:add") == 1);
        CHECK(layout.items.count("airband:rows:0") == 1);
        if (layout.rects.count("airband:addfreq") == 1 && layout.rects.count("airband:mode") == 1 &&
            layout.rects.count("airband:add") == 1) {
            // The mode list's rows exist only while it is open: open it and read
            // where they were drawn.
            const Result open = once("add-mode-layout", 90, "receiver", click(60, layout.rects.at("airband:mode")));
            CHECK(open.ok);
            CHECK(open.rects.count("airband:mode:0") == 1);   // AM
            CHECK(open.rects.count("airband:mode:1") == 1);   // NFM
            if (open.rects.count("airband:mode:1") == 1) {
                // 121.500, NFM, no name: the frequency names it.
                std::string s = click(60, layout.rects.at("airband:addfreq"));
                s += "70 text 121.500\n";
                s += click(80, layout.rects.at("airband:mode"));
                s += click(90, open.rects.at("airband:mode:1"));
                s += click(100, layout.rects.at("airband:add"));
                writeBookmarks("{ \"schemaVersion\": 1, \"bookmarks\": [] }\n");
                const Result r = once("add", 130, "receiver", s);
                CHECK(r.ok);
                // The group it joined is shown: one row, ticked.
                CHECK(r.items.count("airband:rows:1") == 1);
                CHECK(r.items.count("airband:ticked:1") == 1);

                cascade::core::FreqManager m;
                std::string err;
                CHECK(m.load(bookmarksPath().string(), err));
                CHECK(m.list().size() == 1);
                if (m.list().size() == 1) {
                    const cascade::core::Bookmark& b = m.list()[0];
                    std::printf("    saved: %s, %.4f MHz, %s, %.0f Hz, group %s, ticked %d\n",
                                b.name.c_str(), b.freqHz / 1e6, b.mode.c_str(), b.bandwidthHz,
                                b.group.c_str(), b.scan ? 1 : 0);
                    CHECK_NEAR(b.freqHz, 121500000.0, 1.0);
                    CHECK(b.mode == "NFM");
                    CHECK(b.bandwidthHz == 12500.0);
                    CHECK(b.group == "Manual");
                    CHECK(b.scan);
                    CHECK(b.name == "121.5000 MHz");
                }
            }

            // A decimal comma, the mode left on AM, a name typed: 118.750 AM "Tower".
            {
                std::string s = click(60, layout.rects.at("airband:addfreq"));
                s += "70 text 118,75\n";
                s += click(80, layout.rects.at("airband:addname"));
                s += "90 text Tower\n";
                s += click(100, layout.rects.at("airband:add"));
                writeBookmarks("{ \"schemaVersion\": 1, \"bookmarks\": [] }\n");
                const Result r = once("add-comma", 130, "receiver", s);
                CHECK(r.ok);
                cascade::core::FreqManager m;
                std::string err;
                CHECK(m.load(bookmarksPath().string(), err));
                CHECK(m.list().size() == 1);
                if (m.list().size() == 1) {
                    const cascade::core::Bookmark& b = m.list()[0];
                    CHECK_NEAR(b.freqHz, 118750000.0, 1.0);
                    CHECK(b.mode == "AM");
                    CHECK(b.bandwidthHz == 10000.0);
                    CHECK(b.group == "Manual");
                    CHECK(b.scan);
                    CHECK(b.name == "Tower");
                }
            }

            // Text that is not a frequency adds nothing.
            {
                std::string s = click(60, layout.rects.at("airband:addfreq"));
                s += "70 text abc\n";
                s += click(100, layout.rects.at("airband:add"));
                writeBookmarks("{ \"schemaVersion\": 1, \"bookmarks\": [] }\n");
                const Result r = once("add-bad", 130, "receiver", s);
                CHECK(r.ok);
                CHECK(r.items.count("airband:rows:0") == 1);
                cascade::core::FreqManager m;
                std::string err;
                CHECK(m.load(bookmarksPath().string(), err));
                CHECK(m.list().empty());
            }
        }
    }

    // --- a preset named by hand (0.99.66) ---------------------------------------------
    // The preset field is typed over ("Manual" selected, then "Marine"), 156.800 NFM is
    // added, and the row is in the group "Marine", ticked - and the section shows that
    // group, one row, ticked.
    {
        writeBookmarks("{ \"schemaVersion\": 1, \"bookmarks\": [] }\n");
        const Result layout = once("preset-layout", 60, "receiver", "");
        CHECK(layout.ok);
        CHECK(layout.rects.count("airband:preset") == 1);
        CHECK(layout.rects.count("airband:addfreq") == 1);
        CHECK(layout.rects.count("airband:mode") == 1);
        CHECK(layout.rects.count("airband:add") == 1);
        if (layout.rects.count("airband:preset") == 1 && layout.rects.count("airband:addfreq") == 1 &&
            layout.rects.count("airband:mode") == 1 && layout.rects.count("airband:add") == 1) {
            const Result open = once("preset-mode-layout", 90, "receiver", click(60, layout.rects.at("airband:mode")));
            CHECK(open.ok);
            CHECK(open.rects.count("airband:mode:1") == 1);   // NFM
            if (open.rects.count("airband:mode:1") == 1) {
                std::string s = click(60, layout.rects.at("airband:preset"));
                s += "66 key ctrl+a\n68 text Marine\n";
                s += click(72, layout.rects.at("airband:addfreq"));
                s += "78 text 156.800\n";
                s += click(82, layout.rects.at("airband:mode"));
                s += click(92, open.rects.at("airband:mode:1"));
                s += click(102, layout.rects.at("airband:add"));
                writeBookmarks("{ \"schemaVersion\": 1, \"bookmarks\": [] }\n");
                const Result r = once("preset-add", 140, "receiver", s);
                std::printf("  a frequency added to a preset named Marine\n");
                CHECK(r.ok);
                CHECK(r.items.count("airband:rows:1") == 1);
                CHECK(r.items.count("airband:ticked:1") == 1);
                cascade::core::FreqManager m;
                std::string err;
                CHECK(m.load(bookmarksPath().string(), err));
                CHECK(m.list().size() == 1);
                if (m.list().size() == 1) {
                    const cascade::core::Bookmark& b = m.list()[0];
                    std::printf("    saved: %s, %.4f MHz, %s, group %s, ticked %d\n", b.name.c_str(),
                                b.freqHz / 1e6, b.mode.c_str(), b.group.c_str(), b.scan ? 1 : 0);
                    CHECK_NEAR(b.freqHz, 156800000.0, 1.0);
                    CHECK(b.mode == "NFM");
                    CHECK(b.group == "Marine");   // not "Manual", not "ManualMarine"
                    CHECK(b.scan);
                }
            }
        }
    }

    // --- a file imported into a preset (0.99.66) -----------------------------------------
    // The CSV's path is typed into the import field and Import pressed: the rows - which
    // the file puts in other groups and does not tick - join "Harbour", the AM and NFM
    // ones ticked, and the WFM one is kept as a bookmark, unticked. The read is on a worker, so the script sleeps after
    // the press (real time) and the frames that follow collect it.
    {
        const fs::path csv = g_dir / "preset-import.csv";
        {
            std::ofstream f(csv, std::ios::binary | std::ios::trunc);
            f << "frequency_mhz,name,group,mode,bandwidth_hz\n"
                 "156.800,Ch 16,Somewhere,NFM,12500\n"
                 "156.375,Ch 75,Elsewhere,NFM,12500\n"
                 "118.700,Tower,,AM,10000\n"
                 "98.500,Broadcast,Radio,WFM,150000\n";
        }
        writeBookmarks("{ \"schemaVersion\": 1, \"bookmarks\": [] }\n");
        const Result layout = once("import-layout", 60, "receiver", "");
        CHECK(layout.ok);
        CHECK(layout.rects.count("airband:preset") == 1);
        CHECK(layout.rects.count("airband:importpath") == 1);
        CHECK(layout.rects.count("airband:import") == 1);
        CHECK(layout.items.count("airband:rows:0") == 1);
        if (layout.rects.count("airband:preset") == 1 && layout.rects.count("airband:importpath") == 1 &&
            layout.rects.count("airband:import") == 1) {
            std::string s = click(60, layout.rects.at("airband:preset"));
            s += "66 key ctrl+a\n68 text Harbour\n";
            s += click(72, layout.rects.at("airband:importpath"));
            s += "78 text " + csv.string() + "\n";
            s += click(90, layout.rects.at("airband:import"));
            s += "100 sleep 600\n";
            writeBookmarks("{ \"schemaVersion\": 1, \"bookmarks\": [] }\n");
            const Result r = once("import", 160, "receiver", s);
            std::printf("  a CSV imported into a preset named Harbour\n");
            CHECK(r.ok);
            // Shown: the preset's three AM and NFM rows, all ticked (the WFM one is not a row).
            CHECK(r.items.count("airband:rows:3") == 1);
            CHECK(r.items.count("airband:ticked:3") == 1);
            cascade::core::FreqManager m;
            std::string err;
            CHECK(m.load(bookmarksPath().string(), err));
            std::size_t inHarbour = 0, ticked = 0;
            bool wfmKept = false;
            for (const cascade::core::Bookmark& b : m.list()) {
                inHarbour += (b.group == "Harbour") ? 1u : 0u;
                ticked += b.scan ? 1u : 0u;
                // Kept as a bookmark of the preset, UNTICKED: a tick would put it in the
                // Scanner's list mode with no row in this section to take it out again.
                if (b.name == "Broadcast") { wfmKept = b.mode == "WFM" && b.group == "Harbour" && !b.scan; }
            }
            std::printf("    saved: %zu rows, %zu in Harbour, %zu ticked\n", m.list().size(), inHarbour, ticked);
            CHECK(m.list().size() == 4);
            CHECK(inHarbour == 4);
            CHECK(ticked == 3);
            CHECK(wfmKept);
        }
    }

    // --- Remove preset (0.99.66) --------------------------------------------------------
    // The default preset is "Manual": its key takes that group's rows out of the list and
    // leaves every other group alone.
    {
        writeBookmarks(
            "{\n"
            "  \"schemaVersion\": 1,\n"
            "  \"bookmarks\": [\n"
            "    { \"name\": \"Mine A\", \"freqHz\": 121500000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0,\n"
            "      \"group\": \"Manual\", \"scan\": true },\n"
            "    { \"name\": \"Mine B\", \"freqHz\": 122800000.0, \"mode\": \"NFM\", \"bandwidthHz\": 12500.0,\n"
            "      \"group\": \"Manual\" },\n"
            "    { \"name\": \"Keep\", \"freqHz\": 118700000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0,\n"
            "      \"group\": \"Tower\", \"scan\": true }\n"
            "  ]\n"
            "}\n");
        const Result layout = once("remove-layout", 60, "receiver", "");
        CHECK(layout.ok);
        CHECK(layout.rects.count("airband:remove") == 1);
        CHECK(layout.rects.count("airband:export") == 1);
        if (layout.rects.count("airband:remove") == 1) {
            writeBookmarks(
                "{\n"
                "  \"schemaVersion\": 1,\n"
                "  \"bookmarks\": [\n"
                "    { \"name\": \"Mine A\", \"freqHz\": 121500000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0,\n"
                "      \"group\": \"Manual\", \"scan\": true },\n"
                "    { \"name\": \"Mine B\", \"freqHz\": 122800000.0, \"mode\": \"NFM\", \"bandwidthHz\": 12500.0,\n"
                "      \"group\": \"Manual\" },\n"
                "    { \"name\": \"Keep\", \"freqHz\": 118700000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0,\n"
                "      \"group\": \"Tower\", \"scan\": true }\n"
                "  ]\n"
                "}\n");
            const Result r = once("remove", 100, "receiver", click(60, layout.rects.at("airband:remove")));
            std::printf("  Remove preset on the default preset\n");
            CHECK(r.ok);
            cascade::core::FreqManager m;
            std::string err;
            CHECK(m.load(bookmarksPath().string(), err));
            CHECK(m.list().size() == 1);
            if (m.list().size() == 1) { CHECK(m.list()[0].name == "Keep" && m.list()[0].group == "Tower"); }
        }
    }

    // --- Export CSV (0.99.66) --------------------------------------------------------------
    // The default preset's two rows - one ticked AM, one unticked NFM - go out as a CSV in
    // the recordings folder (this run's scratch tree), tick and mode kept, the other
    // group's row left out; the key that opens the folder appears once the file is written
    // (it is found, not pressed: it would open Explorer on the desktop of whoever runs this).
    {
        const std::string rows =
            "{\n"
            "  \"schemaVersion\": 1,\n"
            "  \"bookmarks\": [\n"
            "    { \"name\": \"Mine A\", \"freqHz\": 121500000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0,\n"
            "      \"group\": \"Manual\", \"scan\": true },\n"
            "    { \"name\": \"Mine B\", \"freqHz\": 122800000.0, \"mode\": \"NFM\", \"bandwidthHz\": 12500.0,\n"
            "      \"group\": \"Manual\" },\n"
            "    { \"name\": \"Keep\", \"freqHz\": 118700000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0,\n"
            "      \"group\": \"Tower\", \"scan\": true }\n"
            "  ]\n"
            "}\n";
        const fs::path recordings = g_dir / "home" / "Documents" / "SDR-recordings";
        writeBookmarks(rows);
        const Result layout = once("export-layout", 60, "receiver", "");
        CHECK(layout.ok);
        CHECK(layout.rects.count("airband:export") == 1);
        CHECK(layout.items.count("airband:openfolder") == 0);   // nothing written yet: no key
        if (layout.rects.count("airband:export") == 1) {
            writeBookmarks(rows);
            const Result r = once("export", 140, "receiver",
                                  click(60, layout.rects.at("airband:export")) + "70 sleep 600\n");
            std::printf("  Export CSV on the default preset\n");
            CHECK(r.ok);
            CHECK(r.rects.count("airband:openfolder") == 1);
            std::vector<fs::path> csvs;
            std::error_code lec;
            for (const auto& e : fs::directory_iterator(recordings, lec)) {
                if (e.path().extension() == ".csv") { csvs.push_back(e.path()); }
            }
            CHECK(csvs.size() == 1);
            if (csvs.size() == 1) {
                const std::string name = csvs[0].filename().string();
                std::ifstream in(csvs[0], std::ios::binary);
                const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                std::printf("    wrote %s (%zu bytes)\n", name.c_str(), text.size());
                CHECK(name.rfind("foxsdr-Manual-", 0) == 0);
                // The UTF-8 byte order mark (Excel), then the header.
                CHECK(text.rfind("\xEF\xBB\xBF" "frequency_mhz,name,group,mode,bandwidth_hz,favourite,ticked\r\n", 0) == 0);
                CHECK(text.find("121.500000,Mine A,Manual,AM,10000,0,1\r\n") != std::string::npos);
                CHECK(text.find("122.800000,Mine B,Manual,NFM,12500,0,0\r\n") != std::string::npos);
                CHECK(text.find("Keep") == std::string::npos);
            }
        }
    }

    // --- Remove preset while the monitor plays that preset (0.99.66 review) ---------------
    // "Marine" is typed into the preset field and 119.900 added to it - which puts Marine on
    // show - so the monitor plays that preset (its two rows and the new one); LISTEN, hold,
    // then Remove preset. The monitor must STOP (the VFO's band comes back on the last
    // frame): with the preset's group gone it used to be cut again from "every ticked row"
    // of the whole list and carry on, here playing "Tower"'s 121.000 - a channel nobody had
    // chosen, which would have drawn a mark of its own. The rows of the other preset stay in
    // the list; the preset's are gone. (The sentence the stop says is checked in
    // test_gui_file_jobs, which can read the note; this run reads what the user sees.)
    {
        const std::string rows =
            "{\n"
            "  \"schemaVersion\": 1,\n"
            "  \"bookmarks\": [\n"
            "    { \"name\": \"Low\", \"freqHz\": 119700000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0,\n"
            "      \"group\": \"Marine\", \"scan\": true },\n"
            "    { \"name\": \"High\", \"freqHz\": 120300000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0,\n"
            "      \"group\": \"Marine\", \"scan\": true },\n"
            "    { \"name\": \"Tower\", \"freqHz\": 121000000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0,\n"
            "      \"group\": \"Tower\", \"scan\": true }\n"
            "  ]\n"
            "}\n";
        // Typed and added by the script itself: the preset field takes "Marine", the add row
        // 119.900 (AM, the default), Add pressed at frame 90.
        const auto prepare = [](const std::map<std::string, Rect>& at) {
            std::string s = click(60, at.at("airband:preset"));
            s += "66 key ctrl+a\n68 text Marine\n";
            s += click(72, at.at("airband:addfreq"));
            s += "78 text 119.900\n";
            s += click(90, at.at("airband:add"));
            return s;
        };
        writeBookmarks(rows);
        const Result first = once("rmlisten-layout0", 60, "receiver", "");
        CHECK(first.ok);
        CHECK(first.rects.count("airband:preset") == 1 && first.rects.count("airband:addfreq") == 1 &&
              first.rects.count("airband:add") == 1);
        if (first.rects.count("airband:preset") == 1 && first.rects.count("airband:addfreq") == 1 &&
            first.rects.count("airband:add") == 1) {
            // Where the keys are once Marine is on show, with the added row in it.
            writeBookmarks(rows);
            const Result layout = once("rmlisten-layout", 120, "receiver", prepare(first.rects));
            CHECK(layout.ok);
            CHECK(layout.items.count("airband:rows:3") == 1);   // Marine's two rows and the added one
            CHECK(layout.rects.count("airband:listen") == 1);
            CHECK(layout.rects.count("airband:remove") == 1);
            if (layout.rects.count("airband:listen") == 1 && layout.rects.count("airband:remove") == 1) {
                writeBookmarks(rows);
                const Result r = once("rmlisten", 320, "receiver",
                                      prepare(first.rects) + click(110, layout.rects.at("airband:listen")) +
                                          hold(120) + click(210, layout.rects.at("airband:remove")));
                std::printf("  Remove preset while the monitor plays it\n");
                CHECK(r.ok);
                // It was listening - to Marine's three channels, none of Tower's - ...
                CHECK(r.items.count("airband:listening") == 1);
                CHECK(r.items.count("airband:channels:3") == 1);
                CHECK(r.items.count("airband:channels:4") == 0);
                // ...and it stopped: nothing re-targeted it onto what was left in the list.
                CHECK(r.rects.count("airband:mk:121000") == 0);
                CHECK(r.items.count("airband:mark:121000") == 0);
                std::printf("    after Remove preset the VFO band is %.1f px wide\n",
                            static_cast<double>(vfoRectWidth(r)));
                CHECK(vfoRectWidth(r) > 1.0f);
                cascade::core::FreqManager m;
                std::string err;
                CHECK(m.load(bookmarksPath().string(), err));
                CHECK(m.list().size() == 1);
                if (m.list().size() == 1) { CHECK(m.list()[0].name == "Tower" && m.list()[0].group == "Tower"); }
            }
        }
    }

    const int rc = testSummary("test_airband_app");
    if (rc == 0) { fs::remove_all(g_dir, ec); }
    return rc;
}
