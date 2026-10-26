// test_plugin_cleanup_app.cpp - the ENGINE removing the old plugin copies updates leave behind (0.99.49 beta feedback), with no network and no
// plugin loaded: the old copies are what a scan would have found (core::supersededPlugins is pinned in tests/test_plugin_cleanup.cpp), set on
// the engine through AppWindowTestAccess, and the files are real files in a scratch plugins directory.
//
// CONVERTED (stub campaign B3, 0.99.84 house pattern): master ran this in the window (AppWindow::cleanUpOldPluginVersions); it is the engine's
// now (src/engine/engine_store.cpp), reached by FOXAPP_OP_STORE_CLEAN_OLD and an update's own call, and read back through the table
// (fact.store.oldCopies, fact.store.cleanupReport) and the engine's members. What changed in the way it is checked, and why:
//   - the deletes run on a WORKER (the pass never waits on a disk): each case starts the clean-up and waits for the pass to collect it
//     (cleanupPending_), where master's returned the sentence at once. The assertions on the disk and on the sentence are unchanged;
//   - the store window's model and key labels (m.oldCopies, storeCleanupKeyLabel, storeOldCopyLine, storeCleanupConfirmLabel) are the
//     WINDOW's, and the window half comes back in the next batch (tests/test_plugin_store_window.cpp holds them over the 0.99.72 model); what
//     is held here is what the window reads, the table's old-copies fact.
//
//   - the table lists every old copy, and says what the last clean-up did;
//   - CLEAN UP OLD VERSIONS, once confirmed, removes every one and says so;
//   - an UPDATE removes the copies of the plugin it updated and no other;
//   - a copy that cannot be deleted (TestHooks::pluginRemove plays Windows refusing a mapped module) is queued, the report says it goes at
//     the next start, and the next start's processPendingPluginRemovals takes it - unless that scan no longer finds it an old copy;
//   - (0.99.69) REMOVE FILE on an orphaned file judges the file again against the engine's own scan: a row gone stale - a file this
//     engine's host has no record of - removes nothing and says why, and a name that tries to leave the folder removes nothing. The positive
//     path (an orphan that goes, a running or indexed file that stays) is on real files in tests/test_plugin_cleanup.cpp: nothing here loads a
//     module.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/plugin_cleanup.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "gui/app_window.hpp"
#include "pass_seam.hpp"
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
fs::path g_dir;  // the folder the engine's scan reads in the case being run (Engine::testHooks_.pluginDir)

std::string hookedPluginDir() { return g_dir.string(); }

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_plugin_cleanup_app_" + std::to_string(pid));
    fs::create_directories(g_scratch);
    g_dir = g_scratch / "start";  // what the engine's first scan reads, before any case has chosen a folder
    fs::create_directories(g_dir);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME",
                          "XDG_CACHE_HOME"}) {
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

cascade::core::SupersededPlugin old(const std::string& file, const std::string& name, const std::string& v, const std::string& kept,
                                    const std::string& keptV) {
    return {file, name, v, kept, keptV};
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static void installHooks() {
        cascade::engine::Engine::testHooks_.pluginRemove = &lockingRemove;
        cascade::engine::Engine::testHooks_.pluginDir = &hookedPluginDir;
    }
    // What a scan of `dir` found superseded.
    static void scanned(AppWindow& a, const fs::path& dir, const std::vector<cascade::core::SupersededPlugin>& found) {
        g_dir = dir;
        cascade::engine::Engine& e = rawEngine(a.engine_);
        passseam::on(e, [&] {
            e.pluginDir_ = dir.string();
            e.pluginOldCopies_ = found;
        });
    }
    // One poll of the pass's own collect, until the worker that deletes has been collected (the way a pass does it).
    static bool waitCleanup(AppWindow& a) {
        cascade::engine::Engine& e = rawEngine(a.engine_);
        for (int i = 0; i < 6000; ++i) {
            bool pending = true;
            passseam::on(e, [&] {
                e.pollPluginAsync();
                pending = e.cleanupPending_;
            });
            if (!pending) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    // The store's CLEAN UP key, once its one confirmation is accepted: the op the window sends. Waits for the worker.
    static void confirmCleanup(AppWindow& a) {
        cascade::engine::Engine& e = rawEngine(a.engine_);
        passseam::on(e, [&] { (void)e.applyCommand(cascade::core::cmd::makeInt(FOXAPP_OP_STORE_CLEAN_OLD, 0)); });
        CHECK(waitCleanup(a));
    }
    // An update's own clean-up, as the install's landing makes it: the copies THAT file superseded. Waits for the worker, then returns what
    // the store's line says.
    static std::string afterUpdate(AppWindow& a, const fs::path& installed) {
        cascade::engine::Engine& e = rawEngine(a.engine_);
        passseam::on(e, [&] { e.cleanUpAfterUpdate(installed.string()); });
        CHECK(waitCleanup(a));
        return report(a);
    }
    static std::string report(AppWindow& a) {
        return passseam::read<std::string>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).pluginCleanupReport_; });
    }
    // The table's two facts, built the way a publish builds them.
    static std::string fact(AppWindow& a, const char* key) {
        cascade::engine::Engine& e = rawEngine(a.engine_);
        return passseam::read<std::string>(e, [&] {
            cascade::engine::ApiListBlock b;
            e.buildApiListsStore(b);
            const auto it = b.facts.find(key);
            return it == b.facts.end() ? std::string("<no such fact>") : it->second.value;
        });
    }
    // The Fitted modules window's REMOVE FILE, once confirmed: the op the window sends.
    static void removeOrphan(AppWindow& a, const std::string& file) {
        cascade::engine::Engine& e = rawEngine(a.engine_);
        passseam::on(e, [&] {
            const cascade::core::cmd::QueuedCommand q = cascade::core::cmd::makeText(FOXAPP_OP_STORE_CLEAN_OLD, file, 1);
            (void)e.applyCommand(q.c, q.longText);
        });
    }
    static std::string installError(AppWindow& a) {
        return passseam::read<std::string>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).installError_; });
    }
    static std::string installReport(AppWindow& a) {
        return passseam::read<std::string>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).installReport_; });
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

int main() {
    std::printf("test_plugin_cleanup_app\n");
    isolate();
    Access::installHooks();
    cascade::gui::AppWindow app;

    // --- the table lists them, and the clean-up says what it did -----------------------------------------------------
    {
        const fs::path d = pluginsDir("all", {mod("adsb-1.0.0"), mod("adsb-1.1.0"), mod("apt-1.0.0"), mod("apt-1.0.5"), mod("apt-1.2.0")});
        Access::scanned(app, d, {old(mod("adsb-1.0.0"), "ADS-B", "1.0.0", mod("adsb-1.1.0"), "1.1.0"),
                                 old(mod("apt-1.0.0"), "NOAA APT", "1.0.0", mod("apt-1.2.0"), "1.2.0"),
                                 old(mod("apt-1.0.5"), "NOAA APT", "1.0.5", mod("apt-1.2.0"), "1.2.0")});
        // "file TAB name TAB version TAB kept file TAB kept version", one line per old copy, in scan order: what the key counts and the
        // confirmation lists ("ADS-B 1.0.0 - adsb-1.0.0.dll (1.1.0 stays)").
        const std::string want = mod("adsb-1.0.0") + "\tADS-B\t1.0.0\t" + mod("adsb-1.1.0") + "\t1.1.0\n" + mod("apt-1.0.0") +
                                 "\tNOAA APT\t1.0.0\t" + mod("apt-1.2.0") + "\t1.2.0\n" + mod("apt-1.0.5") + "\tNOAA APT\t1.0.5\t" +
                                 mod("apt-1.2.0") + "\t1.2.0";
        CHECK(Access::fact(app, "store.oldCopies") == want);

        // --- ONE confirmation removes every one of them ----------------------------------------------------------------
        Access::confirmCleanup(app);
        for (const char* f : {"adsb-1.0.0", "apt-1.0.0", "apt-1.0.5"}) { CHECK(!fs::exists(d / mod(f))); }
        CHECK(fs::exists(d / mod("adsb-1.1.0")) && fs::exists(d / mod("apt-1.2.0")));
        std::printf("  report: %s\n", Access::report(app).c_str());
        CHECK(Access::report(app).find("Removed 3 old versions") == 0);
        CHECK(Access::fact(app, "store.cleanupReport") == Access::report(app));
    }

    // --- an update removes its own plugin's old copies, and no other's -----------------------------------------------
    {
        const fs::path d = pluginsDir("update", {mod("adsb-1.0.0"), mod("adsb-1.1.0"), mod("apt-1.0.0"), mod("apt-1.2.0")});
        Access::scanned(app, d, {old(mod("adsb-1.0.0"), "ADS-B", "1.0.0", mod("adsb-1.1.0"), "1.1.0"),
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

    // --- a locked copy is queued, said so, and the next start takes it -----------------------------------------------
    {
        const fs::path d = pluginsDir("locked", {mod("sat-1.0.0"), mod("sat-1.1.0")});
        g_locked = {mod("sat-1.0.0")};
        Access::scanned(app, d, {old(mod("sat-1.0.0"), "Satellites", "1.0.0", mod("sat-1.1.0"), "1.1.0")});
        const std::string said = Access::afterUpdate(app, d / mod("sat-1.1.0"));
        std::printf("  locked: %s\n", said.c_str());
        CHECK(said == "In use, so removed the next time FoxSDR starts: " + mod("sat-1.0.0") + ".");
        CHECK(fs::exists(d / mod("sat-1.0.0")));
        CHECK(cascade::core::loadPendingRemovals(d.string()) == std::vector<std::string>{mod("sat-1.0.0")});
        g_locked.clear();
    }

    // --- REMOVE FILE judges the file again (0.99.69) -------------------------------------------------------------------
    {
        const fs::path d = pluginsDir("orphan", {mod("stray")});
        Access::scanned(app, d, {});  // this engine's host has found no module in `d`
        const fs::path outside = d.parent_path() / mod("outside");
        {
            std::ofstream o(outside, std::ios::binary);
            o << "x";
        }
        for (const std::string& name : {mod("stray"), std::string("../") + mod("outside"), std::string()}) {
            Access::removeOrphan(app, name);
            std::printf("  REMOVE FILE on a stale row (%s): %s\n", name.c_str(), Access::installError(app).c_str());
            CHECK(Access::installError(app).find("last scan") != std::string::npos);
            CHECK(Access::installReport(app).empty());
            CHECK(fs::exists(d / mod("stray")));  // nothing was deleted
            CHECK(fs::exists(outside));
        }
    }

    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return testSummary("test_plugin_cleanup_app");
}
