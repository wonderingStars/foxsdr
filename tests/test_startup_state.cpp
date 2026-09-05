// test_startup_state.cpp - the application starts on the main screen alone.
//
// THE USER'S INSTRUCTION (2026-09-05): "make sure whenever the SDR software
// starts it only shows the main screen regardless of what was running when it
// was closed." Until 0.79.1 a saved config put back the radar scope, the plugin
// store, the fitted modules window and every map page that was open at the
// last exit, and two windows opened themselves besides - the decoder output on
// a decoder's first line, a map page on its first target.
//
// The unit half of that promise is core::startupState, pinned in
// tests/test_config.cpp. This is the other half: THE REAL APPLICATION, run
// against a config file that says everything was open, must write back a
// config that says nothing is - because it never opened any of it. A bounded
// --frames run loads and saves the config named by CASCADE_CONFIG_TEST and
// nothing else, which is exactly the hermetic seam this needs (the same one
// test_diag_hang.cpp drives). Windows only, because that is where the
// application is built and run here; elsewhere the test says so and passes.
//
// Verified RED against the 0.79.0 executable before the change: the file came
// back with scopeMode, pluginBrowserOpen, fittedModulesOpen and the map page's
// open flag all still true.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/config.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(cond)) {                                                                   \
            ++g_failed;                                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                  \
        }                                                                                \
    } while (0)

#if defined(_WIN32)
fs::path scratchDir() {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
    return base / (std::string("cascade-startup-") + std::to_string(::GetCurrentProcessId()));
}

// One environment variable, set for the child and put back afterwards.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value) : name_(name) {
        char buf[4096];
        const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
        had_ = n > 0 && n < sizeof(buf);
        if (had_) { old_.assign(buf, n); }
        ::SetEnvironmentVariableA(name, value.c_str());
    }
    ~ScopedEnv() { ::SetEnvironmentVariableA(name_, had_ ? old_.c_str() : nullptr); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name_;
    bool had_ = false;
    std::string old_;
};

// Runs the application for a few frames against `cfgPath` and returns what it
// printed. Nothing of the run leaves the scratch directory: the config is the
// one named, the profile directories point into the scratch tree (so the
// user's own fitted plugins are not loaded - a plugin with a track source
// would add a map page of its own to the file this test reads back - and no
// log lands in the user's %LOCALAPPDATA%), and every network endpoint is
// pointed at a closed local port, so a build that ever tried to report
// anything from a test would be talking to nobody.
std::string runApp(const fs::path& dir, const fs::path& cfgPath) {
    const ScopedEnv cfg("CASCADE_CONFIG_TEST", cfgPath.string());
    const ScopedEnv appdata("APPDATA", (dir / "appdata").string());
    const ScopedEnv local("LOCALAPPDATA", (dir / "localappdata").string());
    const ScopedEnv diag("FOXSDR_DIAG_DIR", (dir / "diag").string());
    const ScopedEnv t("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9/");
    const ScopedEnv c("FOXSDR_CRASH_URL", "http://127.0.0.1:9/");
    const ScopedEnv u("FOXSDR_UPDATE_URL", "http://127.0.0.1:9/");
    const ScopedEnv r("FOXSDR_REPORTS_URL", "http://127.0.0.1:9/");
    const std::string exe = std::string(CASCADE_APP_BINDIR) + "/cascade.exe";
    const std::string cmd = "\"\"" + exe + "\" --frames 3 2>&1\"";
    std::string out;
    FILE* p = _popen(cmd.c_str(), "r");
    char buf[512];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
    if (p != nullptr) { _pclose(p); }
    return out;
}
#endif

}  // namespace

int main() {
#if defined(_WIN32)
    const fs::path dir = scratchDir();
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "appdata", ec);
    fs::create_directories(dir / "localappdata", ec);
    fs::create_directories(dir / "diag", ec);
    const fs::path cfgPath = dir / "config.json";

    // Everything that can be open, open; everything that says WHERE, set to a
    // value the loader accepts, so "not restored" can be told apart from
    // "discarded as out of range".
    {
        std::ofstream cfg(cfgPath, std::ios::binary | std::ios::trunc);
        cfg << "{\n"
               "  \"scopeMode\": true,\n"
               "  \"scopeRangeNm\": 400,\n"
               "  \"pluginBrowserOpen\": true,\n"
               "  \"fittedModulesOpen\": true,\n"
               "  \"fittedModulesX\": 40,\n"
               "  \"fittedModulesY\": 50,\n"
               "  \"fittedModulesWidth\": 900,\n"
               "  \"fittedModulesHeight\": 700,\n"
               "  \"mapPages\": [\n"
               "    {\"plugin\": \"ADS-B\", \"x\": 10, \"y\": 20, \"width\": 800,"
               " \"height\": 600, \"open\": true}\n"
               "  ]\n"
               "}\n";
    }
    CHECK(fs::exists(cfgPath));

    // The file says what it was told to, before the application touches it.
    {
        cascade::core::AppConfig before;
        std::string err;
        CHECK(cascade::core::ConfigStore::load(cfgPath.string(), before, err));
        CHECK(before.scopeMode);
        CHECK(before.pluginBrowserOpen);
        CHECK(before.fittedModulesOpen);
        CHECK(before.mapPages.size() == 1 && before.mapPages[0].open);
    }

    const std::string out = runApp(dir, cfgPath);
    std::printf("%s", out.c_str());
    CHECK(out.find("rendered 3 frames") != std::string::npos);

    // What the application wrote back is what it was showing: the bench, and
    // nothing else - with every rectangle and the scope's range kept.
    {
        cascade::core::AppConfig after;
        std::string err;
        CHECK(cascade::core::ConfigStore::load(cfgPath.string(), after, err));
        CHECK(err.empty());
        CHECK(!after.scopeMode);
        CHECK(!after.pluginBrowserOpen);
        CHECK(!after.fittedModulesOpen);
        // The entry that went in comes back closed, its rectangle untouched
        // because a page that was never drawn never read a rectangle back from
        // ImGui. The list may be LONGER: the application takes its plugins
        // from the directory beside the executable when that is writable, and
        // in a build tree that is the freshly built set, satellites tracker
        // included - a plugin whose page used to open itself on the full sky
        // it computes on its first frame. Whatever pages exist, every one of
        // them is closed; that is the promise, and the tracker is the sharpest
        // test of it.
        CHECK(!after.mapPages.empty());
        bool sawAdsb = false;
        for (const cascade::core::AppConfig::MapPage& pg : after.mapPages) {
            CHECK(!pg.open);
            if (pg.plugin == "ADS-B") {
                sawAdsb = true;
                CHECK(pg.x == 10 && pg.y == 20 && pg.width == 800 && pg.height == 600);
            }
        }
        CHECK(sawAdsb);
        CHECK(after.scopeRangeNm == 400);
        CHECK(after.fittedModulesX == 40 && after.fittedModulesY == 50);
        CHECK(after.fittedModulesWidth == 900 && after.fittedModulesHeight == 700);
    }

    if (g_failed == 0) { fs::remove_all(dir, ec); }
#else
    std::printf("test_startup_state: the application is built and run on Windows here; "
                "nothing to drive on this platform\n");
#endif
    std::printf("test_startup_state: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
