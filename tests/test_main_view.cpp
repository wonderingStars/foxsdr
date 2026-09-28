/*
 * THE PATCH IS THE MAIN VIEW (0.99.40), IN THE REAL APPLICATION.
 *
 * The owner: "display the patch panel as the main". Until 0.99.39 the patch
 * was a floating 900 x 620 page opened from the SIGNAL PATH bank; now it is
 * one of the main window's two faces. This runs cascade.exe itself and checks
 * each promise the change makes, by what the application drew (the interface
 * census, gui/ui_census.hpp), what it saved (CASCADE_CONFIG_TEST), and what
 * it logged:
 *
 *   fresh     no view in the config: the PATCH view is drawn and the
 *             receiver's spectrum area is not, it fills the main window's
 *             work area, and nothing was started or scanned for showing it -
 *             no patch START, no SoapySDR scan, no native walk, no sound card
 *             listing (the
 *             SoapySDR probe runs only on the user's request, see the
 *             AppWindow constructor);
 *   larger    the same at a larger window: the view grows with it;
 *   switch    one click on the rail's RECEIVER key: the receiver view is
 *             drawn, and the config says "receiver";
 *   remember  that config again: the receiver view from the first frame,
 *             the patch view on none of them;
 *   back      one click on the PATCH key: the config says "patch" again -
 *             and neither switch started a device scan, a listing or the
 *             native walk;
 *   addradio  the control: pressing the Radio part does walk the native
 *             radios (and nothing more), so the line the checks above look
 *             for is really written when a walk happens;
 *   running   a patch STARTED, then the RECEIVER key: the log says the view
 *             closed while running - the path that stops every patch radio and
 *             hands the receiver its radio back, as closing the page did.
 *
 * The clicks are the application's own input script (FOXSDR_INPUT_SCRIPT),
 * aimed at the keys' census rectangles - nothing outside the process is
 * touched. Isolated like every other test that starts the application: its
 * own config, APPDATA / LOCALAPPDATA / XDG dirs in a scratch folder, every
 * report URL at a dead port, the signal generator as the source, and the one
 * patch that is started runs on the signal generator, never a real radio.
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
#include <vector>

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

// An EMPTY value UNSETS the variable. Set to "" it would still exist, and the
// application reads several of these hooks by presence alone - the first cut
// of this file pressed the patch's START key in every run that meant to leave
// it alone (FOXSDR_PATCH_START="" is not null to getenv).
void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value.empty() ? nullptr : value.c_str());
    _putenv_s(name, value.c_str());   // "" removes it from the CRT's copy
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
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
    float cx() const { return (x0 + x1) * 0.5f; }
    float cy() const { return (y0 + y1) * 0.5f; }
};

struct Result {
    bool ok = false;
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
    std::string mainView;              // what the application saved
    std::vector<std::string> log;      // the diagnostic log of this run
};

constexpr int kFrames = 60;
fs::path g_dir;

bool logHas(const Result& r, const char* needle) {
    for (const std::string& l : r.log) {
        if (l.find(needle) != std::string::npos) { return true; }
    }
    return false;
}

// Index of the first log line holding `needle`, or -1.
int logAt(const Result& r, const char* needle) {
    for (std::size_t i = 0; i < r.log.size(); ++i) {
        if (r.log[i].find(needle) != std::string::npos) { return static_cast<int>(i); }
    }
    return -1;
}

// One bounded run: `cfgText` as the config, `script` (empty: none) as the
// input, `patchFile` (empty: none) as FOXSDR_PATCH_FILE, `start` pressing the
// patch's START key once.
Result once(const std::string& tag, const std::string& cfgText, const std::string& script,
            const std::string& windowSize = "1280x720", const std::string& patchText = "",
            bool start = false) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path outFile = g_dir / (tag + ".census");
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << cfgText;
    }
    std::error_code ec;
    fs::remove_all(g_dir / "diag", ec);
    fs::create_directories(g_dir / "diag", ec);
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", outFile.string());
    setEnv("FOXSDR_WINDOW_SIZE", windowSize);
    if (script.empty()) {
        setEnv("FOXSDR_INPUT_SCRIPT", "");
    } else {
        const fs::path sp = g_dir / (tag + ".script");
        std::ofstream f(sp);
        f << script;
        f.close();
        setEnv("FOXSDR_INPUT_SCRIPT", sp.string());
    }
    if (patchText.empty()) {
        setEnv("FOXSDR_PATCH_FILE", "");
    } else {
        const fs::path pp = g_dir / (tag + ".patch");
        std::ofstream f(pp, std::ios::binary | std::ios::trunc);
        f << patchText;
        f.close();
        setEnv("FOXSDR_PATCH_FILE", pp.string());
    }
    setEnv("FOXSDR_PATCH_START", start ? "1" : "");
    const std::string out =
        run("\"" + exePath() + "\" --frames " + std::to_string(kFrames) + " 2>&1");
    const bool rendered =
        out.find("rendered " + std::to_string(kFrames) + " frames") != std::string::npos;
    const bool written = out.find("ui census written") != std::string::npos;
    const bool scripted = script.empty() || out.find("script steps run") != std::string::npos;
    if (!rendered || !written || !scripted) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), out.c_str());
        return r;
    }
    {
        std::ifstream in(outFile);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') { line.pop_back(); }
            if (line.rfind("item ", 0) == 0) {
                r.items.insert(line.substr(5));
            } else if (line.rfind("rect ", 0) == 0) {
                std::istringstream ss(line.substr(5));
                std::string name;
                Rect rc;
                ss >> name >> rc.x0 >> rc.y0 >> rc.x1 >> rc.y1;
                r.rects[name] = rc;
            }
        }
    }
    {
        cascade::core::AppConfig saved;
        std::string err;
        if (cascade::core::ConfigStore::load(cfg.string(), saved, err)) {
            r.mainView = saved.mainView;
        }
    }
    {
        std::ifstream in(g_dir / "diag" / "logs" / "foxsdr.log", std::ios::binary);
        std::string line;
        while (std::getline(in, line)) { r.log.push_back(line); }
    }
    r.ok = !r.items.empty();
    return r;
}

std::string click(int frame, float x, float y) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%d screen %.0f %.0f\n%d down\n%d up\n", frame, x, y,
                  frame + 2, frame + 4);
    return buf;
}

// The config every run starts from: the generator, nothing that reports, the
// diagnostic log on (it is what the "nothing started" half reads), and the
// view named by `view` - or no view at all, as a config from before 0.99.40.
std::string config(const char* view) {
    std::string c =
        "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
        "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"diagnosticsEnabled\": true";
    if (view != nullptr) { c += std::string(", \"mainView\": \"") + view + "\""; }
    return c + " }\n";
}

}  // namespace

int main() {
    g_dir = fs::temp_directory_path() / ("cascade-main-view-" + std::to_string(pid()));
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

    // --- fresh: the patch view, filling the window, and nothing started ------
    const Result fresh = once("fresh", config(nullptr), "");
    CHECK(fresh.ok);
    CHECK(fresh.items.count("view:patch") == 1);
    CHECK(fresh.items.count("view:receiver") == 0);
    CHECK(fresh.mainView == "patch");
    const auto pv = fresh.rects.find("view:patch");
    CHECK(pv != fresh.rects.end());
    if (pv != fresh.rects.end()) {
        std::printf("  patch view %.0f x %.0f at (%.0f, %.0f) in a 1280 x 720 window\n",
                    pv->second.w(), pv->second.h(), pv->second.x0, pv->second.y0);
        // The spectrum's area and the status column's: everything right of
        // the rail (kMenuWidth, 384 px, plus the cabinet's margin) and below
        // the deck, out to the cabinet's right and bottom edges. So a view
        // drawn as the old centred 900 x 620 page, or in the centre column
        // only with the status column beside it, cannot pass.
        CHECK(pv->second.x0 < 384.0f + 60.0f);
        CHECK(pv->second.x1 > 1280.0f - 40.0f && pv->second.x1 <= 1280.0f);
        CHECK(pv->second.y1 > 720.0f - 40.0f && pv->second.y1 <= 720.0f);
        CHECK(pv->second.h() > 0.5f * 720.0f);
    }
    // SHOWING IT STARTED NOTHING AND PROBED NOTHING.
    CHECK(!logHas(fresh, "patch: START"));
    CHECK(!logHas(fresh, "soapy: device scan started"));
    CHECK(!logHas(fresh, "sound card input(s) listed"));
    // ...nor the native walk (review of f7d1cfc): it asks the SDRplay service
    // for its list, and before 0.99.40 ran at launch only for a user
    // restoring a native radio. The Radio part's press below is the control
    // that shows this line is written when a walk does happen.
    CHECK(!logHas(fresh, "source: listing native radios"));
    // ...and the log is really being written, or the three lines above prove
    // nothing.
    CHECK(!fresh.log.empty());

    // --- larger: the view follows the window ----------------------------------
    const Result larger = once("larger", config(nullptr), "", "1600x900");
    CHECK(larger.ok);
    const auto pvl = larger.rects.find("view:patch");
    CHECK(pvl != larger.rects.end());
    if (pv != fresh.rects.end() && pvl != larger.rects.end()) {
        std::printf("  at 1600 x 900: %.0f x %.0f\n", pvl->second.w(), pvl->second.h());
        CHECK(pvl->second.w() - pv->second.w() > 280.0f);
        CHECK(pvl->second.h() - pv->second.h() > 140.0f);
    }

    // --- switch: the RECEIVER key -------------------------------------------
    const auto keyReceiver = fresh.rects.find("viewkey:0");
    const auto keyPatch = fresh.rects.find("viewkey:1");
    CHECK(keyReceiver != fresh.rects.end());
    CHECK(keyPatch != fresh.rects.end());
    if (keyReceiver == fresh.rects.end() || keyPatch == fresh.rects.end()) {
        return testSummary("test_main_view");
    }
    const Result sw =
        once("switch", config(nullptr), click(10, keyReceiver->second.cx(), keyReceiver->second.cy()));
    CHECK(sw.ok);
    CHECK(sw.items.count("view:patch") == 1);     // the frames before the click
    CHECK(sw.items.count("view:receiver") == 1);  // and after it
    CHECK(sw.mainView == "receiver");
    const auto rv = sw.rects.find("view:receiver");
    CHECK(rv != sw.rects.end());

    // --- remember: that choice is read at start-up ----------------------------
    const Result mem = once("remember", config("receiver"), "");
    CHECK(mem.ok);
    CHECK(mem.items.count("view:receiver") == 1);
    CHECK(mem.items.count("view:patch") == 0);
    CHECK(mem.mainView == "receiver");

    // --- back: the PATCH key --------------------------------------------------
    const Result back =
        once("back", config("receiver"), click(10, keyPatch->second.cx(), keyPatch->second.cy()));
    CHECK(back.ok);
    CHECK(back.items.count("view:patch") == 1);
    CHECK(back.mainView == "patch");
    // SWITCHING PROBES NOTHING either way. The first cut treated the PATCH key
    // as opening the old page by hand, which asked for a SoapySDR scan - and
    // this very run enumerated the RTL-SDR on the desk it was built on. The
    // lists wait for a Radio's device list or "Look for radios".
    for (const Result* r : {&sw, &back}) {
        CHECK(!logHas(*r, "soapy: device scan started"));
        CHECK(!logHas(*r, "sound card input(s) listed"));
        CHECK(!logHas(*r, "source: listing native radios"));
        CHECK(!r->log.empty());
    }

    // --- the control: adding a Radio IS asking for the native list ---------------
    // Its new node starts on a free radio, so the walk happens here - which is
    // also what shows the line above is really written when there is a walk.
    const auto radioKey = fresh.rects.find("patchpart:0");
    CHECK(radioKey != fresh.rects.end());
    if (radioKey != fresh.rects.end()) {
        const Result add = once("addradio", config(nullptr),
                                click(20, radioKey->second.cx(), radioKey->second.cy()));
        CHECK(add.ok);
        CHECK(logHas(add, "source: listing native radios"));
        CHECK(!logHas(add, "soapy: device scan started"));   // the native walk only
    }

    // --- running: leaving the view stops a running patch -----------------------
    // One radio on the signal generator - a patch that can really start here,
    // and that opens no hardware.
    const std::string genPatch =
        "foxsdr-patch 6\n"
        "view 0 0 1\n"
        "node 1 0 0 40 40 0 0 100000000 0 - siggen 2000000 1 -50 1 Radio\n";
    const Result running = once("running", config(nullptr),
                                click(30, keyReceiver->second.cx(), keyReceiver->second.cy()),
                                "1280x720", genPatch, true);
    CHECK(running.ok);
    const int started = logAt(running, "patch: START");
    const int left = logAt(running, "patch: view closed while running");
    const int closed = logAt(running, "patch: 1 patch radio(s) closing");
    std::printf("  running: START at log line %d, view closed at %d, radio closed at %d\n",
                started, left, closed);
    CHECK(started >= 0);
    CHECK(left > started);
    // ...and the radio it had open really was closed, by that same path - and
    // not merely when the application closed at the end of the run.
    CHECK(closed > left);
    CHECK(!logHas(running, "patch: shutting down"));
    CHECK(running.mainView == "receiver");

    const int rc = testSummary("test_main_view");
    if (rc == 0) { fs::remove_all(g_dir, ec); }
    return rc;
}
