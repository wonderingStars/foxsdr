// test_plugin_update_cleanup_app.cpp - an update that succeeds takes the copy it replaced with
// it, by itself, and one that fails touches nothing (0.99.72, the plugin store redesign).
//
// THE BEHAVIOUR. A plugin's file name carries its version, so updating adsb-1.0.0 to 1.1.0
// ADDS a file; the next scan loads both, keeps the newer and turns the older off as another
// copy. Until 0.99.72's store the old file then sat in the folder until someone pressed the
// clean-up key. The update now removes the copy it superseded as soon as the new one is loaded
// (AppWindow::pollPluginAsync -> cleanUpAfterUpdate -> core/plugin_cleanup.hpp), for THAT
// plugin only, under the same rules and with the same log line; the general CLEAN UP key stays
// for leftovers. tests/test_plugin_cleanup_app.cpp pins the clean-up function with a faked scan;
// this proves the whole path on REAL plugin modules, so what is checked is what is on the disk
// and what the host has loaded - not what the application says it did.
//
// HOW. The real AppWindow, a scratch plugins folder, and the probe module of
// tests/fixtures/rescan_probe_plugin.cpp built twice: version 1.0.0 (rescan_probe_a) and the
// same plugin's version 1.1.0 (rescan_probe_a_newer), plus the unrelated probe b. The update is
// started and collected exactly as the store's UPDATE key does (startUpdate, then the per-frame
// pollPluginAsync); the "server" is the transport stand-in (PluginRepo::setTransportForTest),
// so the download, its hash, the temporary file, the manifest record and the rescan all run for
// real and nothing touches a network.
//
//   - success: the old file is gone, the new one is there and is what runs (loaded, 1.1.0),
//     the install record names the new file, one "removed old copy" line is logged, and the
//     report says so; an UNRELATED plugin's file is byte-for-byte what it was and still runs;
//   - the newer copy is never touched: after the clean-up the 1.1.0 file is exactly the bytes
//     that were downloaded;
//   - a failed update (the download refused, the bytes not the published digest): the old file
//     is untouched and still the one that runs, no new file, the record unchanged, nothing
//     logged as removed - and the next, good update of the same plugin then succeeds.
//
// Hermetic: the per-user directories are a scratch folder with the process id in its name, the
// plugins folder is the packaged build's per-user one inside it (a fake package identity).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/diag_log.hpp"
#include "core/package_identity.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_host.hpp"
#include "core/plugin_repo.hpp"
#include "gui/app_window.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;

using cascade::core::DiagLog;
using cascade::core::PluginCatalogEntry;
using cascade::core::PluginPlatform;
using cascade::core::PluginRepo;

namespace cascade::gui {

struct AppWindowTestAccess {
    static void rescan(AppWindow& a) { a.rescanPlugins(); }
    // Every instance destroyed and every module unmapped, so a folder can be emptied (a mapped
    // module cannot be deleted on Windows).
    static void takeDown(AppWindow& a) { a.detachAndUnloadPlugins(); }
    static void setCatalogue(AppWindow& a, std::vector<PluginCatalogEntry> c) {
        a.catalog_ = std::move(c);
    }
    // The store's UPDATE key, to the frame that collects it: plan, start, then poll as the
    // frame loop does. True when the transfer finished within the wait.
    static bool update(AppWindow& a, const std::string& id) {
        const std::vector<cascade::core::PluginUpdate> plans = a.plannedPluginUpdates();
        for (const cascade::core::PluginUpdate& u : plans) {
            if (u.id != id) { continue; }
            a.startUpdate(u);
            for (int i = 0; i < 6000; ++i) {
                a.pollPluginAsync();
                if (!a.installPending_) { return true; }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return false;
        }
        return false;
    }
    static std::size_t plannedCount(AppWindow& a) { return a.plannedPluginUpdates().size(); }
    static const std::string& report(const AppWindow& a) { return a.installReport_; }
    static const std::string& error(const AppWindow& a) { return a.installError_; }
    static const std::vector<cascade::core::LoadedPlugin>& plugins(const AppWindow& a) {
        return a.pluginHost_.plugins();
    }
    static const std::vector<cascade::core::InstalledPlugin>& records(const AppWindow& a) {
        return a.pluginInventory_.plugins;
    }
    static std::size_t oldCopies(const AppWindow& a) { return a.pluginOldCopies_.size(); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

#if defined(_WIN32)
const char* const kExt = ".dll";
#elif defined(__APPLE__)
const char* const kExt = ".dylib";
#else
const char* const kExt = ".so";
#endif

fs::path g_scratch;
fs::path g_dir;  // the plugins folder
std::string g_oldBuild;    // probe a, 1.0.0
std::string g_newBuild;    // probe a, 1.1.0
std::string g_otherBuild;  // probe b

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    ::SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_update_cleanup_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_CRASH_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_UPDATE_URL", "http://127.0.0.1:9");
    cascade::core::PackageIdentity id;
    id.packaged = true;
    id.fullName = "update-cleanup-test";
    cascade::core::setPackageIdentityForTest(id);
    g_dir = fs::path(cascade::core::PluginHost::userPluginDir());
    fs::create_directories(g_dir, ec);
}

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string sha256Of(const std::string& path) {
    std::string hex;
    std::string err;
    CHECK(PluginRepo::sha256File(path, hex, err));
    return hex;
}

std::string modName(const std::string& stem) { return stem + kExt; }

std::size_t logged(const std::string& needle) {
    std::size_t n = 0;
    for (const std::string& line : DiagLog::instance().ringSnapshot()) {
        if (line.find(needle) != std::string::npos) { ++n; }
    }
    return n;
}

// A catalogue entry for this host whose one build is `built`, published as `file`.
PluginCatalogEntry entryFor(const std::string& id, const std::string& version,
                            const std::string& file, const std::string& built,
                            const std::string& shaOverride = std::string()) {
    PluginCatalogEntry e;
    e.id = id;
    e.name = "Rescan Probe " + id.substr(id.size() - 1);
    e.version = version;
    e.licence = "PolyForm-Noncommercial-1.0.0";
    e.abiVersion = static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION);
    e.compatible = true;
    PluginPlatform p;
    p.os = PluginRepo::hostOs();
    p.arch = PluginRepo::hostArch();
    p.file = file;
    p.url = "https://plugins.example.invalid/" + file;
    p.sha256 = shaOverride.empty() ? sha256Of(built) : shaOverride;
    e.platforms.push_back(p);
    return e;
}

// What the "server" answers with: the body for each file name, or a failure.
struct Server {
    std::string body;
    bool fail = false;
    int hits = 0;
};

void serve(const std::shared_ptr<Server>& s) {
    PluginRepo::setTransportForTest(
        [s](const std::string&, std::uint64_t,
            const std::function<bool(const void*, std::size_t)>& sink, std::string& error) {
            ++s->hits;
            if (s->fail) {
                error = "the server returned HTTP 503";
                return false;
            }
            return sink(s->body.data(), s->body.size());
        });
}

// The plugin the host has LOADED under `declaredName`, or null.
const cascade::core::LoadedPlugin* loadedAs(const cascade::gui::AppWindow& app,
                                            const std::string& declaredName) {
    for (const cascade::core::LoadedPlugin& p : Access::plugins(app)) {
        if (p.loaded && p.name == declaredName) { return &p; }
    }
    return nullptr;
}

// A fresh plugins folder holding probe a 1.0.0 as the STORE would have installed it (file,
// install record) and probe b as an unrelated plugin, both loaded by a scan.
void freshFolder(cascade::gui::AppWindow& app) {
    Access::takeDown(app);
    std::error_code ec;
    std::vector<fs::path> leftovers;
    for (const auto& e : fs::directory_iterator(g_dir, ec)) { leftovers.push_back(e.path()); }
    for (const fs::path& p : leftovers) { fs::remove_all(p, ec); }
    CHECK(fs::is_empty(g_dir, ec));
    fs::copy_file(g_oldBuild, g_dir / modName("probe-a-1.0.0"), fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    fs::copy_file(g_otherBuild, g_dir / modName("probe-b-1.0.0"), fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    std::string err;
    CHECK(PluginRepo::recordInstall(
        g_dir.string(), entryFor("probe-a", "1.0.0", modName("probe-a-1.0.0"), g_oldBuild), err));
    CHECK(PluginRepo::recordInstall(
        g_dir.string(), entryFor("probe-b", "1.0.0", modName("probe-b-1.0.0"), g_otherBuild), err));
    Access::rescan(app);
    const auto* a = loadedAs(app, "Rescan Probe a");
    CHECK(a != nullptr && a->version == "1.0.0");
    CHECK(loadedAs(app, "Rescan Probe b") != nullptr);
}

void testSuccessRemovesTheSupersededCopy(cascade::gui::AppWindow& app) {
    freshFolder(app);
    const std::string bBytes = readAll(g_dir / modName("probe-b-1.0.0"));
    const std::string newBytes = readAll(g_newBuild);

    auto server = std::make_shared<Server>();
    server->body = newBytes;
    serve(server);
    Access::setCatalogue(app, {entryFor("probe-a", "1.1.0", modName("probe-a-1.1.0"), g_newBuild),
                               entryFor("probe-b", "1.0.0", modName("probe-b-1.0.0"), g_otherBuild)});
    CHECK(Access::plannedCount(app) == 1u);  // only a is an update

    DiagLog::instance().resetForTest();
    CHECK(Access::update(app, "probe-a"));
    std::printf("  report: %s\n", Access::report(app).c_str());
    std::printf("  error: %s\n", Access::error(app).c_str());
    CHECK(server->hits == 1);
    CHECK(Access::error(app).empty());

    // ON THE DISK: the superseded file is gone, the new one is there and is the bytes downloaded.
    CHECK(!fs::exists(g_dir / modName("probe-a-1.0.0")));
    CHECK(fs::exists(g_dir / modName("probe-a-1.1.0")));
    CHECK(readAll(g_dir / modName("probe-a-1.1.0")) == newBytes);  // the newer copy was never touched
    // IN THE HOST: the new one is what runs - and the only copy of the plugin there is.
    const auto* running = loadedAs(app, "Rescan Probe a");
    CHECK(running != nullptr && running->version == "1.1.0");
    std::size_t recordsOfA = 0;
    for (const auto& p : Access::plugins(app)) {
        if (p.name == "Rescan Probe a") { ++recordsOfA; }
    }
    CHECK(recordsOfA == 1u);
    // IN THE RECORD: the plugin is recorded at its new file and version.
    bool recorded = false;
    for (const auto& r : Access::records(app)) {
        if (r.id == "probe-a") {
            recorded = true;
            CHECK(r.file == modName("probe-a-1.1.0"));
            CHECK(r.version == "1.1.0");
        }
    }
    CHECK(recorded);
    CHECK(Access::oldCopies(app) == 0u);
    // SAID, ONCE: one log line, and the report (the store's line under its key) carries it.
    CHECK(logged("plugin: removed old copy " + modName("probe-a-1.0.0")) == 1u);
    CHECK(logged("plugin: removed old copy") == 1u);
    CHECK(Access::report(app).find("Removed the old version " + modName("probe-a-1.0.0")) !=
          std::string::npos);
    // THE UNRELATED PLUGIN is exactly as it was: same bytes, still running.
    CHECK(readAll(g_dir / modName("probe-b-1.0.0")) == bBytes);
    CHECK(loadedAs(app, "Rescan Probe b") != nullptr);
    // Nothing else appeared or went: a, b, the manifest - and nothing queued for the next start.
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(g_dir)) { names.push_back(e.path().filename().string()); }
    CHECK(names.size() == 3u);
    CHECK(!fs::exists(g_dir / "pending-removal.json"));
    PluginRepo::setTransportForTest(nullptr);
}

void testFailureTouchesNothing(cascade::gui::AppWindow& app) {
    freshFolder(app);
    const std::string aBytes = readAll(g_dir / modName("probe-a-1.0.0"));
    const std::string bBytes = readAll(g_dir / modName("probe-b-1.0.0"));
    const std::string newBytes = readAll(g_newBuild);

    auto server = std::make_shared<Server>();
    serve(server);
    const auto untouched = [&](const char* what) {
        std::printf("  %s: %s\n", what, Access::error(app).c_str());
        CHECK(!Access::error(app).empty());
        CHECK(Access::report(app).find("Updated") == std::string::npos);
        CHECK(fs::exists(g_dir / modName("probe-a-1.0.0")));
        CHECK(readAll(g_dir / modName("probe-a-1.0.0")) == aBytes);
        CHECK(!fs::exists(g_dir / modName("probe-a-1.1.0")));
        CHECK(readAll(g_dir / modName("probe-b-1.0.0")) == bBytes);
        const auto* running = loadedAs(app, "Rescan Probe a");
        CHECK(running != nullptr && running->version == "1.0.0");
        for (const auto& r : Access::records(app)) {
            if (r.id == "probe-a") {
                CHECK(r.file == modName("probe-a-1.0.0"));
                CHECK(r.version == "1.0.0");
            }
        }
        CHECK(logged("plugin: removed old copy") == 0u);
        CHECK(logged("plugin: old copy") == 0u);
        // No debris: a, b and the manifest.
        std::size_t n = 0;
        for (const auto& e : fs::directory_iterator(g_dir)) { (void)e; ++n; }
        CHECK(n == 3u);
    };

    // The download is refused by the server.
    Access::setCatalogue(app, {entryFor("probe-a", "1.1.0", modName("probe-a-1.1.0"), g_newBuild)});
    server->fail = true;
    DiagLog::instance().resetForTest();
    CHECK(Access::update(app, "probe-a"));
    CHECK(server->hits == 1);
    CHECK(Access::error(app).find("HTTP 503") != std::string::npos);
    untouched("server refused");

    // The bytes that arrive are not the published digest (a substituted file).
    server->fail = false;
    server->body = readAll(g_otherBuild);  // a real module, but not the one the catalogue hashed
    DiagLog::instance().resetForTest();
    CHECK(Access::update(app, "probe-a"));
    CHECK(server->hits == 2);
    CHECK(Access::error(app).find("integrity") != std::string::npos);
    untouched("wrong digest");

    // ...and the very next, good update of the same plugin works: a failure is not a state.
    server->body = newBytes;
    DiagLog::instance().resetForTest();
    CHECK(Access::update(app, "probe-a"));
    CHECK(Access::error(app).empty());
    CHECK(!fs::exists(g_dir / modName("probe-a-1.0.0")));
    CHECK(fs::exists(g_dir / modName("probe-a-1.1.0")));
    const auto* running = loadedAs(app, "Rescan Probe a");
    CHECK(running != nullptr && running->version == "1.1.0");
    CHECK(logged("plugin: removed old copy " + modName("probe-a-1.0.0")) == 1u);
    PluginRepo::setTransportForTest(nullptr);
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("test_plugin_update_cleanup_app\n");
    if (argc < 4) {
        std::printf("usage: test_plugin_update_cleanup_app <probe a 1.0.0> <probe a 1.1.0> <probe b>\n");
        return 2;
    }
    g_oldBuild = argv[1];
    g_newBuild = argv[2];
    g_otherBuild = argv[3];
    isolate();
    {
        cascade::gui::AppWindow app;
        testSuccessRemovesTheSupersededCopy(app);
        testFailureTouchesNothing(app);
    }
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return testSummary("test_plugin_update_cleanup_app");
}
