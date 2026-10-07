/*
 * THE RADIO SETUP PAGE, IN THE REAL APPLICATION (2026-10-06). The decision
 * table is pinned in tests/test_radio_setup.cpp; this runs cascade itself,
 * given a synthetic machine by FOXSDR_FAKE_RADIO_SETUP (so it needs no radio,
 * and a radio on the desk cannot change the answer), and checks from the UI
 * census and the diagnostic log it wrote:
 *
 *   rtl-dvbt       an RTL-SDR on the television driver: the page opens by
 *                  itself, shows the wrong-driver finding, its link, its CHECK
 *                  AGAIN key and the tick, and the log names the driver Windows
 *                  reported;
 *   the others     each machine raises the page, or leaves it down, as the
 *                  decision table says - no device, bound but unlisted, no SDRplay
 *                  API, no UHD, a radio that is fine, an unreadable bus;
 *   dont-show      the same machine with the choice persisted in the config
 *                  raises no page, and says in the log that it was the tick;
 *   the tick       pressed on the page it reaches the config file, and the NEXT
 *                  launch raises nothing;
 *   the key        the Source section's RADIO SETUP key opens the page for
 *                  someone who ticked the box;
 *   check again    the page's CHECK AGAIN key runs a second probe;
 *   no seam        a run without the variable reads the real bus and prints
 *                  what it found (reported, not asserted - the desk decides).
 *
 * The presses are the application's own input script, aimed from the census.
 * The vendor links are NOT pressed: that would open a browser on the desktop.
 * Isolated like every other test that starts the application: its own config,
 * APPDATA, LOCALAPPDATA and diagnostics tree, and every FOXSDR_*_URL pointed at
 * a closed local port.
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
    std::string log;     // the run's diagnostic log, whole
    std::string config;  // the config file as the run left it
    bool has(const char* item) const { return items.count(item) == 1; }
    bool logs(const char* text) const { return log.find(text) != std::string::npos; }
    int count(const char* text) const {
        int n = 0;
        for (std::size_t at = log.find(text); at != std::string::npos;
             at = log.find(text, at + 1)) {
            ++n;
        }
        return n;
    }
};

constexpr int kFrames = 90;
fs::path g_dir;

struct Run {
    std::string tag;
    std::string fake;           // FOXSDR_FAKE_RADIO_SETUP; empty = the real bus
    bool dontShow = false;      // radioSetupDontShow in the config
    std::string script;
    std::string reuseConfig;    // the tag of an earlier run whose config this one starts from
};

Result once(const Run& r) {
    Result out;
    const fs::path cfg = g_dir / (r.tag + ".json");
    const fs::path census = g_dir / (r.tag + ".census");
    const fs::path diag = g_dir / (r.tag + "-diag");
    std::error_code ec;
    fs::remove_all(diag, ec);
    fs::create_directories(diag, ec);
    if (!r.reuseConfig.empty()) {
        fs::copy_file(g_dir / (r.reuseConfig + ".json"), cfg, fs::copy_options::overwrite_existing, ec);
    } else {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"diagnosticsEnabled\": true, "
          << "\"updateCheckEnabled\": false, \"sourceKind\": \"siggen\", \"uiTheme\": \"today\", "
             "\"interfaceScale\": \"100\", \"railBank\": 0, \"radioSetupDontShow\": "
          << (r.dontShow ? "true" : "false") << " }\n";
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    setEnv("FOXSDR_WINDOW_SIZE", "1920x1080");
    setEnv("FOXSDR_FAKE_RADIO_SETUP", r.fake);
    if (r.script.empty()) {
        setEnv("FOXSDR_INPUT_SCRIPT", "");
    } else {
        const fs::path sp = g_dir / (r.tag + ".script");
        std::ofstream f(sp);
        f << r.script;
        f.close();
        setEnv("FOXSDR_INPUT_SCRIPT", sp.string());
    }
    const std::string stdoutText =
        run("\"" + exePath() + "\" --frames " + std::to_string(kFrames) + " 2>&1");
    const bool rendered =
        stdoutText.find("rendered " + std::to_string(kFrames) + " frames") != std::string::npos;
    const bool written = stdoutText.find("ui census written") != std::string::npos;
    const bool scripted =
        r.script.empty() || stdoutText.find("script steps run") != std::string::npos;
    if (!rendered || !written || !scripted) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", r.tag.c_str(),
                    stdoutText.c_str());
        return out;
    }
    {
        std::ifstream in(census);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("item ", 0) == 0) { out.items.insert(line.substr(5)); }
            if (line.rfind("rect ", 0) != 0) { continue; }
            std::istringstream ss(line.substr(5));
            std::string name;
            Rect rc;
            ss >> name >> rc.x0 >> rc.y0 >> rc.x1 >> rc.y1;
            out.rects[name] = rc;
        }
    }
    {
        std::ifstream in(diag / "logs" / "foxsdr.log", std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        out.log = ss.str();
    }
    {
        std::ifstream in(cfg, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        out.config = ss.str();
    }
    out.ok = !out.log.empty();
    if (!out.ok) { std::printf("  %s: no diagnostic log was written\n", r.tag.c_str()); }
    return out;
}

// A click at a rectangle's centre, at the given frame, then the pointer moved
// away so no hover state outlives it.
std::string pressAt(int frame, const Rect& k) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%d screen %.0f %.0f\n%d down\n%d up\n%d screen %.0f %.0f\n",
                  frame, k.cx(), k.cy(), frame + 2, frame + 4, frame + 6, k.cx() + 400.0f,
                  k.cy() + 300.0f);
    return buf;
}

bool configSaysDontShow(const Result& r) {
    // The file is JSON this program wrote itself; the key and its value, in
    // either spacing the writer might use.
    return r.config.find("\"radioSetupDontShow\": true") != std::string::npos ||
           r.config.find("\"radioSetupDontShow\":true") != std::string::npos;
}

// --- the page, raised by a machine with an RTL-SDR on the television driver ----

void testTheTelevisionDriverRaisesThePage() {
    std::printf("  rtl-dvbt\n");
    Run r;
    r.tag = "dvbt";
    r.fake = "rtl-dvbt";
    const Result a = once(r);
    CHECK(a.ok);
    // THE PAGE, ITS FINDING AND ITS THREE CONTROLS.
    CHECK(a.has("radio-setup:page"));
    CHECK(a.has("radio-setup:finding:wrong-driver"));
    CHECK(!a.has("radio-setup:finding:usable"));
    CHECK(a.has("radio-setup:link"));
    CHECK(a.rects.count("radio-setup:link0") == 1);
    CHECK(a.has("radio-setup:check-again"));
    CHECK(a.has("radio-setup:dont-show"));
    // THE LOG: the synthetic machine was named, the driver Windows reported is
    // in the summary, and the page's raising is a line of its own.
    CHECK(a.logs("radio setup: FOXSDR_FAKE_RADIO_SETUP=rtl-dvbt - a synthetic machine"));
    CHECK(a.logs("wrong-driver [RTL2832UUSB (Realtek)]"));
    CHECK(a.logs("radio setup: page raised on its own - 1 finding(s), first: wrong-driver"));
    CHECK(a.logs("page wanted: yes"));
    // Nothing was pressed, nothing was opened, nothing was saved as a choice.
    CHECK(!a.logs("radio setup: opening"));
    CHECK(!configSaysDontShow(a));
}

// --- every machine, raising the page or not as the table says ----------------------

void testEveryMachine() {
    struct Row {
        const char* fake;
        const char* finding;  // the census item of the one finding
        bool page;
    };
    const Row rows[] = {
        {"none", "radio-setup:finding:no-device", true},
        {"rtl-nodriver", "radio-setup:finding:wrong-driver", true},
        {"rtl-winusb", "radio-setup:finding:not-visible", true},
        {"rsp-noapi", "radio-setup:finding:vendor-missing", true},
        {"usrp-nouhd", "radio-setup:finding:vendor-missing", true},
        {"usrp-unlisted", "radio-setup:finding:not-visible", true},
        // A radio that is fine, listed by the application: no page.
        {"rtl-winusb-listed", "radio-setup:finding:usable", false},
        {"rsp-ok", "radio-setup:finding:usable", false},
        {"usrp-ok", "radio-setup:finding:usable", false},
        // A bus that could not be read: said once in the log, never a page.
        {"unreadable", "radio-setup:finding:not-checked", false},
    };
    for (const Row& row : rows) {
        std::printf("  %s\n", row.fake);
        Run r;
        r.tag = std::string("m-") + row.fake;
        r.fake = row.fake;
        const Result a = once(r);
        CHECK(a.ok);
        CHECK(a.has("radio-setup:page") == row.page);
        if (row.page) {
            CHECK(a.has(row.finding));
            CHECK(a.logs("radio setup: page raised on its own"));
        } else {
            CHECK(!a.has(row.finding));  // nothing is drawn, so nothing is censused
            CHECK(!a.logs("radio setup: page raised on its own"));
            CHECK(a.logs("radio setup: page not raised"));
        }
        // Every machine is probed and summarised exactly once at start-up.
        CHECK(a.count("radio setup: ") >= 2);
        CHECK(a.logs(row.page ? "page wanted: yes" : "page wanted: no") || std::string(row.fake) == "unreadable");
    }
}

// --- the persisted choice ------------------------------------------------------------

void testDontShowAgainPersistedRaisesNothing() {
    std::printf("  dont-show\n");
    Run r;
    r.tag = "dontshow";
    r.fake = "rtl-dvbt";
    r.dontShow = true;
    const Result a = once(r);
    CHECK(a.ok);
    CHECK(!a.has("radio-setup:page"));
    CHECK(!a.has("radio-setup:finding:wrong-driver"));
    // The check still RAN and logged what it found: only the page stayed down,
    // and the log says it was the tick that did it.
    CHECK(a.logs("wrong-driver [RTL2832UUSB (Realtek)]"));
    CHECK(a.logs("radio setup: page not raised - \"Don't show this again\" is ticked"));
    CHECK(!a.logs("radio setup: page raised on its own"));
    // ...and the choice survives the run's own config write.
    CHECK(configSaysDontShow(a));
}

// THE TICK, PRESSED ON THE PAGE, REACHES THE FILE AND SILENCES THE NEXT LAUNCH.
void testTheTickIsSavedAndSilencesTheNextLaunch() {
    std::printf("  the tick\n");
    Run first;
    first.tag = "tick-layout";
    first.fake = "rtl-dvbt";
    const Result layout = once(first);
    CHECK(layout.ok);
    CHECK(layout.rects.count("radio-setup:dont-show") == 1);
    if (layout.rects.count("radio-setup:dont-show") != 1) { return; }
    const Rect box = layout.rects.at("radio-setup:dont-show");
    std::printf("    tick at %.0f,%.0f .. %.0f,%.0f\n", box.x0, box.y0, box.x1, box.y1);

    Run press;
    press.tag = "tick";
    press.fake = "rtl-dvbt";
    press.script = pressAt(30, box);
    const Result pressed = once(press);
    CHECK(pressed.ok);
    CHECK(pressed.has("radio-setup:page"));
    // THE FILE: written by the run itself, with the tick in it.
    CHECK(configSaysDontShow(pressed));

    // THE NEXT LAUNCH, from the file the first one left: no page.
    Run next;
    next.tag = "after-tick";
    next.fake = "rtl-dvbt";
    next.reuseConfig = "tick";
    const Result after = once(next);
    CHECK(after.ok);
    CHECK(!after.has("radio-setup:page"));
    CHECK(after.logs("radio setup: page not raised - \"Don't show this again\" is ticked"));
    CHECK(configSaysDontShow(after));
}

// THE KEY IN THE SOURCE SECTION opens the page for a person who ticked the box.
void testTheSourceSectionKeyOpensThePage() {
    std::printf("  the key\n");
    setEnv("FOXSDR_CENSUS_HOLD_BANK", "1");
    Run first;
    first.tag = "key-layout";
    first.fake = "rtl-dvbt";
    first.dontShow = true;
    const Result layout = once(first);
    CHECK(layout.ok);
    CHECK(layout.has("source:radio-setup"));
    CHECK(!layout.has("radio-setup:page"));
    CHECK(layout.rects.count("source:radio-setup") == 1);
    if (layout.rects.count("source:radio-setup") == 1) {
        const Rect key = layout.rects.at("source:radio-setup");
        std::printf("    key at %.0f,%.0f .. %.0f,%.0f\n", key.x0, key.y0, key.x1, key.y1);
        Run press;
        press.tag = "key";
        press.fake = "rtl-dvbt";
        press.dontShow = true;
        press.script = pressAt(30, key);
        const Result k = once(press);
        CHECK(k.ok);
        // Silenced at launch (the log says so) and open because it was asked for.
        CHECK(k.logs("radio setup: page not raised - \"Don't show this again\" is ticked"));
        CHECK(k.has("radio-setup:page"));
        CHECK(k.has("radio-setup:finding:wrong-driver"));
    }
    setEnv("FOXSDR_CENSUS_HOLD_BANK", "");
}

// CHECK AGAIN runs a second probe, and the page redraws from it.
void testCheckAgainRunsASecondProbe() {
    std::printf("  check again\n");
    Run first;
    first.tag = "again-layout";
    first.fake = "rtl-dvbt";
    const Result layout = once(first);
    CHECK(layout.ok);
    CHECK(layout.rects.count("radio-setup:check-again") == 1);
    if (layout.rects.count("radio-setup:check-again") != 1) { return; }
    // One probe at start-up and no more without a press.
    CHECK(layout.count("a synthetic machine, the bus is not read") == 1);

    Run press;
    press.tag = "again";
    press.fake = "rtl-dvbt";
    press.script = pressAt(30, layout.rects.at("radio-setup:check-again"));
    const Result a = once(press);
    CHECK(a.ok);
    CHECK(a.count("a synthetic machine, the bus is not read") == 2);
    // Each finished probe wrote its summary: the start-up one and the re-check.
    CHECK(a.count("devnode(s) of known radios on the bus") == 2);
    CHECK(a.has("radio-setup:page"));
    CHECK(a.has("radio-setup:finding:wrong-driver"));
}

// A seam value that names no machine reads as an unreadable bus and raises
// nothing: a typo in a test must not look like a radio that is missing.
void testAnUnknownMachineRaisesNothing() {
    std::printf("  unknown machine\n");
    Run r;
    r.tag = "unknown";
    r.fake = "no-such-machine";
    const Result a = once(r);
    CHECK(a.ok);
    CHECK(!a.has("radio-setup:page"));
    CHECK(a.logs("radio setup: the USB bus could not be listed"));
}

// THE REAL BUS, reported and not asserted: the desk decides what is on it. Only
// that the application runs, probes and logs coherently is checked.
void testTheRealBusIsProbed() {
    std::printf("  the real bus\n");
    Run r;
    r.tag = "real";
    const Result a = once(r);
    CHECK(a.ok);
    CHECK(!a.logs("FOXSDR_FAKE_RADIO_SETUP"));
    CHECK(a.logs("radio setup: checking the USB bus and what is installed for it"));
#if defined(_WIN32)
    CHECK(a.logs("devnode(s) of known radios on the bus"));
#else
    CHECK(a.logs("not probed (not probed on this platform)"));
#endif
    // A BOUNDED RUN NEVER RAISES THE PAGE ON THE REAL BUS (a CI runner has no
    // radio, and the page would sit over every scripted click of every other
    // test): the decision is logged once, with what it would have been.
    CHECK(!a.has("radio-setup:page"));
    CHECK(!a.logs("radio setup: page raised on its own"));
    CHECK(a.count("radio setup: page not raised - a bounded run raises it only for a synthetic machine") == 1);
    std::istringstream ss(a.log);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.find("radio setup: ") != std::string::npos) { std::printf("    %s\n", line.c_str()); }
    }
}

}  // namespace

int main() {
    // Unbuffered, so a run that ctest kills on its time limit still shows which
    // case it had reached: the 0.99.69 Linux timeout printed nothing at all.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_radio_setup_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-radio-setup-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir / "appdata", ec);
    fs::create_directories(g_dir / "local", ec);
    setEnv("APPDATA", (g_dir / "appdata").string());
    setEnv("LOCALAPPDATA", (g_dir / "local").string());
    setEnv("XDG_CONFIG_HOME", (g_dir / "appdata").string());
    setEnv("XDG_DATA_HOME", (g_dir / "local").string());
    setEnv("XDG_STATE_HOME", (g_dir / "local").string());
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL",
                            "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL",
                            "FOXSDR_BETA_API_URL", "FOXSDR_TESTER_USAGE_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("FOXSDR_SINGLE_VIEWPORT", "1");
    setEnv("FOXSDR_PATCH_START", "");

    testTheTelevisionDriverRaisesThePage();
    testEveryMachine();
    testDontShowAgainPersistedRaisesNothing();
    testTheTickIsSavedAndSilencesTheNextLaunch();
    testTheSourceSectionKeyOpensThePage();
    testCheckAgainRunsASecondProbe();
    testAnUnknownMachineRaisesNothing();
    testTheRealBusIsProbed();

    const int rc = testSummary("test_radio_setup_app");
    if (rc == 0) { fs::remove_all(g_dir, ec); }
    return rc;
}
