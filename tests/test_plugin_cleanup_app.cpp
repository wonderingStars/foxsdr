// test_plugin_cleanup_app.cpp - the REAL AppWindow removing the old plugin
// copies updates leave behind (0.99.49 beta feedback), with no window, no
// network and no plugin loaded: the old copies are what a scan would have
// found (core::supersededPlugins is pinned in tests/test_plugin_cleanup.cpp),
// set on the window through AppWindowTestAccess, and the files are real files
// in a scratch plugins directory.
//
//   - the store's model lists every old copy, and its key says how many;
//   - CLEAN UP OLD VERSIONS, once confirmed, removes every one and says so;
//   - an UPDATE removes the copies of the plugin it updated and no other;
//   - a copy that cannot be deleted (TestHooks::pluginRemove plays Windows
//     refusing a mapped module) is queued, the panel says it goes at the next
//     start, and the next start's processPendingPluginRemovals takes it -
//     unless that scan no longer finds it an old copy.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/plugin_cleanup.hpp"
#include "gui/app_window.hpp"
#include "gui/plugin_store_view.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)
const char* const kExt = ".dll";
#elif defined(__APPLE__)
const char* const kExt = ".dylib";
#else
const char* const kExt = ".so";
#endif

std::string mod(const std::string& stem) { return stem + kExt; }

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

fs::path g_scratch;

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_plugin_cleanup_app_" + std::to_string(pid));
    fs::create_directories(g_scratch);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_CRASH_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_UPDATE_URL", "http://127.0.0.1:9");
}

fs::path pluginsDir(const std::string& tag, const std::vector<std::string>& files) {
    const fs::path d = g_scratch / tag;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    for (const std::string& f : files) {
        std::ofstream o(d / f, std::ios::binary);
        o << "module";
    }
    return d;
}

// THE LOCKED FILES: what Windows refuses while another process has them.
std::set<std::string> g_locked;
bool lockingRemove(const std::string& dir, const std::string& file, std::string& error) {
    if (g_locked.count(file) != 0) {
        error = "the file is in use";
        return false;
    }
    return cascade::core::defaultPluginFileRemover()(dir, file, error);
}

cascade::core::SupersededPlugin old(const std::string& file, const std::string& name,
                                    const std::string& v, const std::string& kept,
                                    const std::string& keptV) {
    return {file, name, v, kept, keptV};
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static void installHooks() { AppWindow::testHooks_.pluginRemove = &lockingRemove; }
    // What a scan of `dir` found superseded.
    static void scanned(AppWindow& a, const fs::path& dir,
                        const std::vector<cascade::core::SupersededPlugin>& found) {
        a.pluginDir_ = dir.string();
        a.pluginOldCopies_ = found;
    }
    static PluginStoreModel model(AppWindow& a) {
        PluginStoreModel m;
        a.buildPluginStoreModel(m);
        return m;
    }
    // The store's key, once its one confirmation is accepted.
    static void confirmCleanup(AppWindow& a) { a.cleanUpOldVersionsConfirmed(); }
    static std::string afterUpdate(AppWindow& a, const fs::path& installed) {
        return a.cleanUpAfterUpdate(installed.string());
    }
    static std::string report(AppWindow& a) { return model(a).cleanupReport; }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

int main() {
    std::printf("test_plugin_cleanup_app\n");
    isolate();
    Access::installHooks();
    cascade::gui::AppWindow app;

    // --- the store lists them, and the key says how many -------------------------
    {
        const fs::path d = pluginsDir("all", {mod("adsb-1.0.0"), mod("adsb-1.1.0"), mod("apt-1.0.0"),
                                              mod("apt-1.0.5"), mod("apt-1.2.0")});
        Access::scanned(app, d,
                        {old(mod("adsb-1.0.0"), "ADS-B", "1.0.0", mod("adsb-1.1.0"), "1.1.0"),
                         old(mod("apt-1.0.0"), "NOAA APT", "1.0.0", mod("apt-1.2.0"), "1.2.0"),
                         old(mod("apt-1.0.5"), "NOAA APT", "1.0.5", mod("apt-1.2.0"), "1.2.0")});
        const cascade::gui::PluginStoreModel m = Access::model(app);
        CHECK(m.oldCopies.size() == 3u);
        CHECK(cascade::gui::storeCleanupKeyLabel(m.oldCopies.size()) == "CLEAN UP OLD VERSIONS (3)");
        CHECK(!m.oldCopies.empty() && cascade::gui::storeOldCopyLine(m.oldCopies[0]) ==
                                          "ADS-B 1.0.0 - " + mod("adsb-1.0.0") + " (1.1.0 stays)");
        CHECK(cascade::gui::storeCleanupConfirmLabel(3) == "Remove 3 files");
        CHECK(cascade::gui::storeCleanupConfirmLabel(1) == "Remove 1 file");

        // --- ONE confirmation removes every one of them --------------------------
        Access::confirmCleanup(app);
        for (const char* f : {"adsb-1.0.0", "apt-1.0.0", "apt-1.0.5"}) { CHECK(!fs::exists(d / mod(f))); }
        CHECK(fs::exists(d / mod("adsb-1.1.0")) && fs::exists(d / mod("apt-1.2.0")));
        std::printf("  report: %s\n", Access::report(app).c_str());
        CHECK(Access::report(app).find("Removed 3 old versions") == 0);
    }

    // --- an update removes its own plugin's old copies, and no other's -----------
    {
        const fs::path d = pluginsDir("update", {mod("adsb-1.0.0"), mod("adsb-1.1.0"),
                                                 mod("apt-1.0.0"), mod("apt-1.2.0")});
        Access::scanned(app, d,
                        {old(mod("adsb-1.0.0"), "ADS-B", "1.0.0", mod("adsb-1.1.0"), "1.1.0"),
                         old(mod("apt-1.0.0"), "NOAA APT", "1.0.0", mod("apt-1.2.0"), "1.2.0")});
        const std::string said = Access::afterUpdate(app, d / mod("adsb-1.1.0"));
        std::printf("  after the update: %s\n", said.c_str());
        CHECK(!fs::exists(d / mod("adsb-1.0.0")));
        CHECK(fs::exists(d / mod("adsb-1.1.0")));
        CHECK(fs::exists(d / mod("apt-1.0.0")));  // another plugin's: the key's to offer
        CHECK(said == "Removed the old version " + mod("adsb-1.0.0") + ".");
        // An update that superseded nothing says nothing.
        Access::scanned(app, d, {});
        CHECK(Access::afterUpdate(app, d / mod("adsb-1.1.0")).empty());
    }

    // --- a locked copy is queued, said so, and the next start takes it ----------
    {
        const fs::path d = pluginsDir("locked", {mod("sat-1.0.0"), mod("sat-1.1.0")});
        g_locked = {mod("sat-1.0.0")};
        Access::scanned(app, d, {old(mod("sat-1.0.0"), "Satellites", "1.0.0", mod("sat-1.1.0"), "1.1.0")});
        const std::string said = Access::afterUpdate(app, d / mod("sat-1.1.0"));
        std::printf("  locked: %s\n", said.c_str());
        CHECK(said == "In use, so removed the next time FoxSDR starts: " + mod("sat-1.0.0") + ".");
        CHECK(fs::exists(d / mod("sat-1.0.0")));
        CHECK(cascade::core::loadPendingRemovals(d.string()) ==
              std::vector<std::string>{mod("sat-1.0.0")});
        g_locked.clear();
    }

    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return testSummary("test_plugin_cleanup_app");
}
