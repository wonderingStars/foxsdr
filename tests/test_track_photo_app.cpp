/*
 * A TARGET'S PHOTO IN ITS DETAILS (0.99.98, CASCADE_CAP_TRACK_PHOTO), END TO END.
 *
 * Runs cascade itself, isolated like every test that starts the application, in ONE viewport
 * (FOXSDR_SINGLE_VIEWPORT=1, so the UI census reads every rectangle in the main window), with two
 * fixtures fitted: tests/fixtures/track_probe_plugin.cpp publishing one placed aircraft under a
 * real ICAO address (TRACK_PROBE_PLACED_ID), its map page opened by FOXSDR_OPEN_MAP; and
 * tests/fixtures/photo_probe_plugin.cpp, a registry source that also has photos, which writes
 * every photo call it receives to PHOTO_PROBE_LOG. Three runs:
 *
 *   list      the map page and its list only, nothing clicked: the photo is NEVER asked for.
 *             Asking is what makes a real plugin go to a photo service, and a target merely on
 *             the map is not one the user chose to look at;
 *   opened    the row's Details button clicked: the details window asks, the picture is drawn
 *             (census "trackphoto:<id>") with the photographer's credit under it
 *             ("trackphoto:credit:..."), and the plugin's lent pixels are released;
 *   closed    opened, the picture clicked, the window closed, opened again: the photo's page is
 *             what the click opens (FOXSDR_SHELL_OPEN_LOG records it instead of starting a
 *             browser); the close tells the plugin to forget the photo, so the second opening
 *             fetches it afresh rather than finding it kept.
 *
 * NO NETWORK: every telemetry address is a discard port and the update check is off.
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

#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

const char* const kIcao = "4CA7B5";

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
    std::vector<std::string> probeLog;  // PHOTO_PROBE_LOG, line by line
    std::vector<std::string> opened;    // FOXSDR_SHELL_OPEN_LOG, line by line
    std::string out;
    bool has(const std::string& name) const { return items.count(name) != 0u; }
    const Rect* rect(const std::string& name) const {
        const auto it = rects.find(name);
        return it == rects.end() ? nullptr : &it->second;
    }
    int count(const std::string& line) const {
        int n = 0;
        for (const std::string& l : probeLog) { n += (l == line) ? 1 : 0; }
        return n;
    }
    // Index of the first line equal to `line`, or -1.
    int first(const std::string& line) const {
        for (std::size_t i = 0; i < probeLog.size(); ++i) {
            if (probeLog[i] == line) { return static_cast<int>(i); }
        }
        return -1;
    }
};

fs::path g_dir;
std::string g_trackDll;
std::string g_photoDll;

fs::path pluginsDir() { return g_dir / "local" / "foxsdr" / "plugins"; }

void writeFile(const fs::path& p, const std::string& body) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

std::vector<std::string> readLines(const fs::path& p) {
    std::vector<std::string> out;
    std::ifstream in(p);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (!line.empty()) { out.push_back(line); }
    }
    return out;
}

void installFixtures() {
    std::error_code ec;
    fs::remove_all(pluginsDir(), ec);
    fs::create_directories(pluginsDir(), ec);
    fs::copy_file(g_trackDll, pluginsDir() / (std::string("track_probe_fixture") + kExt),
                  fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    fs::copy_file(g_photoDll, pluginsDir() / (std::string("photo_probe_fixture") + kExt),
                  fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
}

Result once(const std::string& tag, int frames, const std::string& script) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    const fs::path diag = g_dir / (tag + "-diag");
    const fs::path probeLog = g_dir / (tag + ".probe.log");
    const fs::path openLog = g_dir / (tag + ".open.log");
    {
        std::error_code ec;
        fs::remove_all(diag, ec);
        fs::create_directories(diag, ec);
        fs::remove(probeLog, ec);
        fs::remove(openLog, ec);
    }
    writeFile(cfg, "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
                   "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", "
                   "\"interfaceScale\": \"100\", \"mainView\": \"receiver\", "
                   "\"bandPlanOverlay\": false, \"mapListOnScreenOnly\": false }\n");
    const std::string scriptPath = (g_dir / (tag + ".script")).string();
    writeFile(scriptPath, script);
    setEnv("FOXSDR_INPUT_SCRIPT", script.empty() ? std::string() : scriptPath);
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    setEnv("PHOTO_PROBE_LOG", probeLog.string());
    setEnv("FOXSDR_SHELL_OPEN_LOG", openLog.string());
    r.out = run("\"" + exePath() + "\" --frames " + std::to_string(frames) + " 2>&1");
    const bool rendered =
        r.out.find("rendered " + std::to_string(frames) + " frames") != std::string::npos;
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
    r.probeLog = readLines(probeLog);
    r.opened = readLines(openLog);
    r.ok = true;
    return r;
}

// One press and release at (x, y): the pointer two frames before, down, up a frame later.
std::string click(int frame, float x, float y) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d screen %.1f %.1f\n%d down\n%d up\n", frame, x, y, frame + 2,
                  frame + 3);
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("test_track_photo_app\n");
    if (argc < 3) {
        std::printf("usage: test_track_photo_app <track probe module> <photo probe module>\n");
        return 2;
    }
    g_trackDll = argv[1];
    g_photoDll = argv[2];
    g_dir = fs::temp_directory_path() / ("cascade-trackphoto-" + std::to_string(pid()));
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
    setEnv("FOXSDR_OPEN_MAP", "Track Probe");
    for (const char* off : {"FOXSDR_PATCH_START", "FOXSDR_PATCH_FILE", "FOXSDR_OPEN_PLUGIN_STORE",
                            "FOXSDR_OPEN_FITTED_MODULES", "FOXSDR_OPEN_DECODER_OUTPUT",
                            "FOXSDR_OPEN_DECODERS", "FOXSDR_PRESS_PRESET", "FOXSDR_SHOT_DIR",
                            "FOXSDR_SHOT_AT_FRAME", "FOXSDR_WINDOW_POS", "CASCADE_DECODE_TEST",
                            "CASCADE_PLUGIN_TEST", "TRACK_PROBE_ASSUME_OLD_HOST",
                            "TRACK_PROBE_BARE_NAN", "TRACK_PROBE_ONLY_HEARD"}) {
        setEnv(off, "");
    }
    setEnv("TRACK_PROBE_PLACED_ID", kIcao);
    installFixtures();

    const std::string detailsKey = std::string("tracklist:details:") + kIcao;
    const std::string photoKey = std::string("trackphoto:") + kIcao;
    const std::string credit = "trackphoto:credit:Photo: Ada Test / Photo Probe";
    const std::string get = std::string("get ") + kIcao;
    const std::string forget = std::string("forget ") + kIcao;

    // --- THE LIST ALONE: the aircraft is listed, its photo never asked for ----------------------
    const Result list = once("list", 80, std::string());
    CHECK(list.ok);
    const Rect* details = list.ok ? list.rect(detailsKey) : nullptr;
    if (list.ok) {
        CHECK(list.rect(std::string("tracklist:row:") + kIcao) != nullptr);
        CHECK(details != nullptr);
        CHECK(list.count(get) == 0);
        CHECK(list.rect(photoKey) == nullptr);
        std::printf("    list only: %zu photo calls\n", list.probeLog.size());
    }
    if (details == nullptr) {
        std::printf("  no Details button in the census; cannot go on\n");
        fs::remove_all(g_dir, ec);
        return testSummary("test_track_photo_app");
    }

    // --- OPENED: asked, drawn with its credit, the lent pixels released --------------------------
    const std::string open = click(20, details->cx(), details->cy());
    const Result opened = once("opened", 80, open);
    CHECK(opened.ok);
    const Rect* photo = opened.ok ? opened.rect(photoKey) : nullptr;
    const Rect* close = opened.ok ? opened.rect("details:close") : nullptr;
    if (opened.ok) {
        CHECK(opened.count(get) >= 2);  // PENDING once, then READY
        CHECK(opened.count(std::string("ready ") + kIcao) == 1);
        CHECK(opened.count("release -") == 1);
        CHECK(opened.has(std::string("trackphoto:pending:") + kIcao));
        CHECK(photo != nullptr);
        CHECK(opened.has(credit));
        // Drawn at its own 40 x 20, never enlarged.
        if (photo != nullptr) {
            CHECK(photo->x1 - photo->x0 > 39.0f && photo->x1 - photo->x0 < 41.0f);
            CHECK(photo->y1 - photo->y0 > 19.0f && photo->y1 - photo->y0 < 21.0f);
        }
        // Still on screen at the last frame: forgotten only by the exit's teardown, so the one
        // forget there is the log's last line.
        CHECK(opened.count(forget) <= 1);
        if (opened.count(forget) == 1) { CHECK(opened.probeLog.back() == forget); }
        CHECK(opened.opened.empty());
        std::printf("    opened: %zu photo calls, picture %s, credit %s\n", opened.probeLog.size(),
                    photo != nullptr ? "drawn" : "MISSING", opened.has(credit) ? "drawn" : "MISSING");
    }

    // --- CLICKED, CLOSED AND OPENED AGAIN: the page opened; the close let the photo go, so ------
    // the second opening fetches it afresh. Were it kept after the close, the second opening
    // would find it held and ask nothing - and the only forget would be the exit's.
    if (photo != nullptr && close != nullptr) {
        const std::string script = open + click(50, photo->cx(), photo->cy()) +
                                   click(80, close->cx(), close->cy()) +
                                   click(110, details->cx(), details->cy());
        const Result closed = once("closed", 160, script);
        CHECK(closed.ok);
        if (closed.ok) {
            CHECK(closed.opened.size() == 1);
            if (!closed.opened.empty()) {
                CHECK(closed.opened[0] == "https://example.test/photo/1");
                std::printf("    clicked: opened %s\n", closed.opened[0].c_str());
            }
            const std::string ready = std::string("ready ") + kIcao;
            CHECK(closed.count(ready) == 2);
            CHECK(closed.count("release -") == 2);
            // ready, forget (the close), ready (opened again), in that order.
            int firstReady = -1, forgotAt = -1, secondReady = -1;
            for (std::size_t i = 0; i < closed.probeLog.size(); ++i) {
                const std::string& l = closed.probeLog[i];
                if (l == ready && firstReady < 0) {
                    firstReady = static_cast<int>(i);
                } else if (l == forget && firstReady >= 0 && forgotAt < 0) {
                    forgotAt = static_cast<int>(i);
                } else if (l == ready && forgotAt >= 0 && secondReady < 0) {
                    secondReady = static_cast<int>(i);
                }
            }
            CHECK(firstReady >= 0 && forgotAt > firstReady && secondReady > forgotAt);
            std::printf("    closed and reopened: ready at %d, forget at %d, ready again at %d\n",
                        firstReady, forgotAt, secondReady);
        }
    }

    fs::remove_all(g_dir, ec);
    return testSummary("test_track_photo_app");
}
