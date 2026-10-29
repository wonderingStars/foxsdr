/*
 * WITH SEPARATE WINDOWS OFF, EVERY PAGE IS INSIDE THE MAIN WINDOW (2026-10-27 tester report).
 *
 * Runs cascade itself, isolated like every test that starts the application, with three pages
 * that open as separate operating-system windows - the track fixture's map page
 * (FOXSDR_OPEN_MAP), the plugin store and the Fitted modules window - and reads where each one
 * is from the window list the self-capture writes beside each picture (FOXSDR_SHOT_AT_FRAME):
 *
 *   refused   FOXSDR_TEST_VIEWPORT_REFUSE_AT=60 has the backend refuse the next window it is
 *             asked for, through the same path a real "WGL: Failed to create OpenGL context"
 *             takes, and a probe window past the main window's edge asks for one. Separate
 *             windows go off for the rest of the run (the log says so), and by frame 100 every
 *             page is WHOLLY INSIDE the main window, at least the drag floor in size, with one
 *             "brought inside the main window" log line each. Before the refusal (frame 50) the
 *             pages were their own windows, at least one of them past the main window's edge - or
 *             the check after it would prove nothing. The refusal's issue report is SENT in this
 *             run (0.99.101), not left for the next start: the log records the sweep that took
 *             it, and the attempt either fails against the discard port (a sidecar says so) or
 *             is cut short by the end of the run, the shutdown rule;
 *   single    FOXSDR_SINGLE_VIEWPORT=1, separate windows off from the start: the same three pages,
 *             which open overhanging the main window's right edge on purpose, are inside it too;
 *   kept      the control - no refusal: the pages stay their own windows, where they opened, and
 *             nothing is brought inside.
 *
 * THE FIELD REPORT. A tester's 0.99.95 on Intel UHD graphics: after an hour or so of ADS-B, "the
 * ADS-B map closes when I try to interact with it". His log had "WGL: Failed to create OpenGL
 * context" and "every page stays inside the main window for this session" seconds before each
 * time. Reproduced with this test's seam on 0.99.95: the map went from its own window at
 * (1383,226) 1120x1166 to (1142,-15) inside a 1282x745 main window - 140 px of it showing, its
 * rail above the top edge - and stayed there.
 *
 * A machine whose driver will not give the application a second window at all starts with
 * separate windows off; there `refused` and `kept` have nothing to show and say so, and `single`
 * still runs.
 *
 * NO NETWORK: every telemetry address and the plugin catalogue are a discard port, and the update
 * check is off.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
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

#include "core/health_events.hpp"
#include "gui/page_geometry.hpp"
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

// The three pages, by the identity half of their ImGui names (what follows "###"), and the name
// each has in the "brought inside the main window" log line.
struct Page {
    const char* id;
    const char* logName;
};
const Page kPages[] = {
    {"###mapPage:Track Probe", "page \"Track Probe Map\""},
    {"###pluginstorewindow", "page \"Plugin store\""},
    {"###fittedmoduleswindow", "page \"Fitted modules\""},
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
};

// One self-capture's window list: "x \t y \t w \t h \t name" per window, the main window's row
// named "##cascade_root".
struct Windows {
    bool ok = false;
    Rect root;
    std::map<std::string, Rect> byName;
    const Rect* page(const char* id) const {
        for (const auto& kv : byName) {
            if (kv.first.find(id) != std::string::npos) { return &kv.second; }
        }
        return nullptr;
    }
};

Windows readWindows(const fs::path& file) {
    Windows w;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        Rect r;
        std::string name;
        if (!(ss >> r.x >> r.y >> r.w >> r.h)) { continue; }
        std::getline(ss >> std::ws, name);
        if (name == "##cascade_root") {
            w.root = r;
            w.ok = true;
        }
        w.byName[name] = r;
    }
    return w;
}

// Wholly inside, to the half pixel ImGui's float positions are drawn at.
bool inside(const Rect& r, const Rect& in) {
    return r.x >= in.x - 0.5f && r.y >= in.y - 0.5f && r.x + r.w <= in.x + in.w + 0.5f &&
           r.y + r.h <= in.y + in.h + 0.5f;
}

struct Result {
    bool ok = false;
    bool startedSingle = false;  // the run began with separate windows off
    Windows before;              // frame 50
    Windows after;               // frame 100
    std::string out;
    std::string log;
};

fs::path g_dir;
std::string g_fixtureDll;

fs::path pluginsDir() { return g_dir / "local" / "foxsdr" / "plugins"; }

void writeFile(const fs::path& p, const std::string& body) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// The files in a run's reports folder whose names begin `issue-` and end `suffix`, read,
// newest last: the issue reports themselves (core/issue_report.hpp) end ".txt", and the
// uploader's sidecar beside each (core/crash_upload.cpp) ends ".txt.upload".
std::vector<std::string> issueFilesEnding(const std::string& tag, const std::string& suffix) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(g_dir / (tag + "-diag") / "crashes", ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("issue-", 0) == 0 && name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end());
    std::vector<std::string> out;
    for (const fs::path& f : files) { out.push_back(readFile(f)); }
    return out;
}

std::vector<std::string> issueReports(const std::string& tag) { return issueFilesEnding(tag, ".txt"); }
std::vector<std::string> issueSidecars(const std::string& tag) { return issueFilesEnding(tag, ".txt.upload"); }

// The health ledger's file for every run of this test (its config files all live in g_dir).
fs::path ledgerPath() { return g_dir / cascade::core::health::HealthLedger::kFileName; }

// What the ledger counted under `recovered.`, as its file holds it ("token=count" lines).
std::string recoveredCounts() {
    std::string out;
    std::istringstream lines(readFile(ledgerPath()));
    for (std::string l; std::getline(lines, l);) {
        if (l.rfind("recovered.", 0) == 0) { out += (out.empty() ? "" : ",") + l; }
    }
    return out;
}

Result once(const std::string& tag, bool single, int refuseAt) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path diag = g_dir / (tag + "-diag");
    const fs::path shots = g_dir / (tag + "-shots");
    std::error_code ec;
    fs::remove_all(diag, ec);
    fs::remove_all(shots, ec);
    // The health ledger (core/health_events.hpp) lives beside the config file, so one run's
    // counts would be read by the next: each run starts without one.
    fs::remove(ledgerPath(), ec);
    fs::create_directories(diag, ec);
    fs::create_directories(shots, ec);
    // Usage reporting and diagnostics ON (their addresses are the discard port, set in main):
    // the refusal is a counted health event, and the ledger counts only while both are on.
    writeFile(cfg, "{ \"telemetryEnabled\": true, \"diagnosticsEnabled\": true, \"updateCheckEnabled\": false, "
                   "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", "
                   "\"mainView\": \"receiver\", \"bandPlanOverlay\": false, "
                   "\"pluginCatalogueUrl\": \"http://127.0.0.1:9/catalogue.json\" }\n");
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    setEnv("FOXSDR_SHOT_DIR", shots.string());
    setEnv("FOXSDR_SHOT_AT_FRAME", "50,100");
    setEnv("FOXSDR_SINGLE_VIEWPORT", single ? "1" : "");
    setEnv("FOXSDR_TEST_VIEWPORT_REFUSE_AT", refuseAt >= 0 ? std::to_string(refuseAt) : "");
    r.out = run("\"" + exePath() + "\" --frames 110 2>&1");
    if (r.out.find("rendered 110 frames") == std::string::npos) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), r.out.c_str());
        return r;
    }
    r.log = readFile(diag / "logs" / "foxsdr.log");
    // Off from the start: asked for (FOXSDR_SINGLE_VIEWPORT), or the start-up probe was refused.
    r.startedSingle = r.log.find("viewports: single") != std::string::npos ||
                      r.log.find("would not give a second OpenGL context") != std::string::npos;
    r.before = readWindows(shots / "shot-50.windows.txt");
    r.after = readWindows(shots / "shot-100.windows.txt");
    r.ok = r.before.ok && r.after.ok;
    if (!r.ok) { std::printf("  %s: a window list is missing or has no main window\n", tag.c_str()); }
    return r;
}

// Every page is present at frame 100 and wholly inside the main window, no smaller than the
// drag floor, with its rail (its top edge) inside too.
void checkAllInside(const Result& r, const char* tag) {
    for (const Page& p : kPages) {
        const Rect* pg = r.after.page(p.id);
        CHECK(pg != nullptr);
        if (pg == nullptr) {
            std::printf("    %s: %s is not in the frame-100 window list\n", tag, p.id);
            continue;
        }
        std::printf("    %s: %s at %.0f,%.0f %.0fx%.0f in a main window at %.0f,%.0f %.0fx%.0f\n",
                    tag, p.id, pg->x, pg->y, pg->w, pg->h, r.after.root.x, r.after.root.y,
                    r.after.root.w, r.after.root.h);
        CHECK(inside(*pg, r.after.root));
        CHECK(pg->w >= cascade::gui::kPageMinW);
        CHECK(pg->h >= cascade::gui::kPageMinH);
        CHECK(pg->y >= r.after.root.y);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("test_viewport_fallback_app\n");
    if (argc < 2) {
        std::printf("usage: test_viewport_fallback_app <track probe module>\n");
        return 2;
    }
    g_fixtureDll = argv[1];
    g_dir = fs::temp_directory_path() / ("cascade-vpfallback-" + std::to_string(pid()));
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
                            "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL",
                            "FOXSDR_REGIONAL_CATALOGUE_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("FOXSDR_WINDOW_SIZE", "1280x720");
    setEnv("FOXSDR_WINDOW_POS", "100,100");
    // A fake package identity puts the plugins folder under LOCALAPPDATA (the scratch tree), not
    // beside the executable in the build tree.
    setEnv("FOXSDR_FAKE_PACKAGE", "FoxSDR.StoreTest_1.0.0.0_x64__test");
    setEnv("FOXSDR_OPEN_MAP", "Track Probe");
    setEnv("FOXSDR_OPEN_PLUGIN_STORE", "1");
    setEnv("FOXSDR_OPEN_FITTED_MODULES", "1");
    setEnv("FOXSDR_PATCH_START", "");
    setEnv("FOXSDR_PATCH_FILE", "");
    setEnv("FOXSDR_OPEN_DECODER_OUTPUT", "");
    setEnv("FOXSDR_OPEN_DECODERS", "");
    setEnv("FOXSDR_PRESS_PRESET", "");
    setEnv("FOXSDR_INPUT_SCRIPT", "");
    setEnv("FOXSDR_UI_CENSUS", "");
    setEnv("CASCADE_DECODE_TEST", "");
    setEnv("CASCADE_PLUGIN_TEST", "");
    setEnv("FOXSDR_TEST_WINDOW_FAIL", "");
    fs::create_directories(pluginsDir(), ec);
    fs::copy_file(g_fixtureDll, pluginsDir() / (std::string("track_probe_fixture") + kExt),
                  fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);

    // --- REFUSED: separate windows go off at frame 60, and every page is brought inside ----------
    {
        const Result r = once("refused", false, 60);
        CHECK(r.ok);
        if (r.ok && r.startedSingle) {
            std::printf("    refused: this machine starts with separate windows off - nothing to refuse\n");
        } else if (r.ok) {
            CHECK(r.log.find("could not create a second OpenGL context") != std::string::npos);
            // ...AND IT WAS COUNTED, once, as the health event `recovered.viewport` (0.99.100):
            // the one way this ever reaches the website, since the program carries on.
            std::printf("    refused ledger: %s\n", recoveredCounts().c_str());
            CHECK(recoveredCounts() == "recovered.viewport=1");
            // ...AND REPORTED (0.99.100): one issue report in the run's reports folder, of
            // the viewport word, carrying the log that names the refusal.
            const std::vector<std::string> issues = issueReports("refused");
            CHECK(issues.size() == 1);
            if (issues.size() == 1) {
                CHECK(issues[0].rfind("kind: issue\n", 0) == 0);
                CHECK(issues[0].find("\nreason: issue: viewport - ") != std::string::npos);
                const std::size_t log = issues[0].find("--- log (last ");
                CHECK(log != std::string::npos);
                CHECK(issues[0].find("could not create a second OpenGL context", log) !=
                      std::string::npos);
            }
            // ...AND SENT IN THIS SESSION (0.99.101): the folder was swept again once the report
            // was filed, not left for the next start. The sweep's outcome is logged when the
            // window closes: one report considered, none sent (the crash address is the discard
            // port). What became of it depends on how much of the run was left - a connect to a
            // closed port takes WinHTTP its retries, and the fifty frames after the refusal can be
            // shorter than that - so it either FAILED, with a sidecar saying one attempt was made,
            // or was ABANDONED FOR SHUTDOWN: the shutdown rule (nothing waits for a transfer at
            // exit, and an abandoned attempt costs the report nothing). Either is the folder swept
            // again in this session, and neither happens without that: before 0.99.101 this log
            // had no "crash upload:" line at all.
            CHECK(r.log.find("crash upload: 1 considered, 0 sent, 0 duplicate, 0 held, ") !=
                  std::string::npos);
            const bool failed = r.log.find(": failed, will retry") != std::string::npos;
            const bool cutShort = r.log.find(": abandoned for shutdown") != std::string::npos;
            std::printf("    refused upload: %s\n",
                        failed     ? "attempted and failed (the discard port)"
                        : cutShort ? "cut short by the end of the run"
                                   : "neither failed nor abandoned");
            CHECK(failed != cutShort);
            const std::vector<std::string> sidecars = issueSidecars("refused");
            CHECK(sidecars.size() == (failed ? 1u : 0u));
            if (failed && sidecars.size() == 1) {
                CHECK(sidecars[0].rfind("status: failed\n", 0) == 0);
                CHECK(sidecars[0].find("\nattempts: 1\n") != std::string::npos);
            }
            // THE SETUP, or the check below proves nothing: at frame 50 the pages are their own
            // windows, and the map - the page of the field report - is past the main window's
            // edge. (The plugin store opens over the main window by its own placement,
            // pageOpenInside, so it may already be inside.)
            const Rect* mapBefore = r.before.page(kPages[0].id);
            CHECK(mapBefore != nullptr);
            CHECK(mapBefore != nullptr && !inside(*mapBefore, r.before.root));
            checkAllInside(r, "refused");
            std::istringstream logLines(r.log);
            for (std::string l; std::getline(logLines, l);) {
                if (l.find("brought inside the main window") != std::string::npos) {
                    std::printf("    refused log: %s\n", l.c_str());
                }
            }
            // One log line for every page that HAD to be brought inside, and none for a page that
            // was inside already: the pass is a rescue, not a layout.
            for (const Page& p : kPages) {
                const Rect* pg = r.before.page(p.id);
                CHECK(pg != nullptr);
                if (pg == nullptr) { continue; }
                const std::string line = std::string("viewports: ") + p.logName +
                                         " brought inside the main window";
                const bool logged = r.log.find(line) != std::string::npos;
                CHECK(logged == !inside(*pg, r.before.root));
            }
        }
    }

    // --- SINGLE: separate windows off from the start; pages open inside ----------------------------
    {
        const Result r = once("single", true, -1);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.startedSingle);
            checkAllInside(r, "single");
        }
    }

    // --- KEPT: the control - no refusal, the pages stay their own windows --------------------------
    {
        const Result r = once("kept", false, -1);
        CHECK(r.ok);
        if (r.ok && r.startedSingle) {
            std::printf("    kept: this machine starts with separate windows off - no control to run\n");
        } else if (r.ok) {
            CHECK(r.log.find("could not create a second OpenGL context") == std::string::npos);
            CHECK(r.log.find("brought inside the main window") == std::string::npos);
            // ...and nothing was counted or reported: the control run met no refusal - and with
            // nothing filed, the folder was not swept again (no sweep's outcome was logged).
            CHECK(recoveredCounts().empty());
            CHECK(issueReports("kept").empty());
            CHECK(r.log.find("crash upload:") == std::string::npos);
            bool anyOutside = false;
            for (const Page& p : kPages) {
                const Rect* pg = r.after.page(p.id);
                CHECK(pg != nullptr);
                if (pg != nullptr && !inside(*pg, r.after.root)) { anyOutside = true; }
            }
            CHECK(anyOutside);
        }
    }

    fs::remove_all(g_dir, ec);
    return testSummary("test_viewport_fallback_app");
}
