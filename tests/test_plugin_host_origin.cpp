// test_plugin_host_origin.cpp - a plugin site's module never takes a declared name from an
// official one, on REAL modules, through the real scan and the real AppWindow (0.99.74).
//
// THE RULE (core/plugin_host.hpp, resolveDuplicatePlugins). The host keeps one loaded module
// per declared name, and used to keep the highest version. The declared name is written by
// whoever built the binary, so a module installed from a plugin site that declared an official
// plugin's name with a higher version switched the official one off. Now the install records
// decide: an OFFICIAL record holds its name against every SITE record of it, whatever the
// versions; two site modules, an unrecorded module and a scan given no records behave as before.
// tests/test_plugin_host.cpp pins the policy on records built by hand; this proves the two lines
// of plumbing around it - scan() stamping each record from the file names it is given
// (case-insensitively), and AppWindow::rescanPlugins() building that map from installed.json -
// on the probe module of tests/fixtures/rescan_probe_plugin.cpp built twice: version 1.0.0
// (rescan_probe_a) and the same plugin's 1.1.0 (rescan_probe_a_newer), which declare ONE name.
//
//   host.scan with origins
//     official 1.0.0 + site 1.1.0   -> the official runs; the site's is not loaded, unmapped, says
//                                      why in the host's words and names the file that runs;
//     site + site                   -> the higher version (1.1.0) runs, as before;
//     official 1.1.0 + site 1.0.0   -> the official 1.1.0 runs (it would have anyway);
//     no origins                    -> the higher version runs, as before;
//     a key in upper case           -> found (NTFS names are case-insensitive);
//   the real AppWindow, installed.json naming both files
//     record without source + record with a source -> the official runs, the site's is NOT
//       loaded, the log says "NOT loaded ... the official plugin", and the scan is not marked
//       refused (a rescan that changes nothing can be skipped);
//     both records with a source    -> 1.1.0 runs;
//     no manifest                   -> 1.1.0 runs.
//
// Hermetic: the per-user directories are a scratch folder with the process id in its name, the
// plugins folder is the packaged build's per-user one inside it (a fake package identity).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
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
#include "engine/engine.hpp"
#include "gui/app_window.hpp"
#include "pass_seam.hpp"
#include "gui/plugins_view.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;

using cascade::core::DiagLog;
using cascade::core::LoadedPlugin;
using cascade::core::PluginFileOrigins;
using cascade::core::PluginHost;

namespace cascade::gui {

struct AppWindowTestAccess {
    // THE ENGINE'S PLUGIN SYSTEM, on the pass's own thread (tests/pass_seam.hpp); what is read out is a COPY made there.
    static void rescan(AppWindow& a) { passseam::on(rawEngine(a.engine_), [&] { rawEngine(a.engine_).rescanPlugins(); }); }
    static void takeDown(AppWindow& a) { passseam::on(rawEngine(a.engine_), [&] { (void)rawEngine(a.engine_).detachAndUnloadPlugins(); }); }
    static std::vector<LoadedPlugin> plugins(AppWindow& a) {
        return passseam::read<std::vector<LoadedPlugin>>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).pluginHost_.plugins(); });
    }
    static bool scanSkippable(AppWindow& a) {
        return passseam::read<bool>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).pluginScanSigValid_; });
    }
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
std::string g_oldBuild;  // probe a, 1.0.0
std::string g_newBuild;  // probe a, 1.1.0

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    ::SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

std::string modName(const std::string& stem) { return stem + kExt; }

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

std::size_t logged(const std::string& needle) {
    std::size_t n = 0;
    for (const std::string& line : DiagLog::instance().ringSnapshot()) {
        if (line.find(needle) != std::string::npos) { ++n; }
    }
    return n;
}

void writeText(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

fs::path freshFolder(const char* tag) {
    const fs::path d = g_scratch / tag;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

const LoadedPlugin* recordFor(const std::vector<LoadedPlugin>& list, const std::string& file) {
    for (const LoadedPlugin& p : list) {
        if (fs::path(p.path).filename().string() == file) { return &p; }
    }
    return nullptr;
}

const std::string kOld = "probe-a-1.0.0";
const std::string kNew = "probe-a-1.1.0";

void stage(const fs::path& d) {
    std::error_code ec;
    fs::copy_file(g_oldBuild, d / modName(kOld), fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    fs::copy_file(g_newBuild, d / modName(kNew), fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
}

// --- the host, scanned with origins ------------------------------------------------------

void testHostScan() {
    const fs::path d = freshFolder("host");
    stage(d);
    PluginHost host;

    // No origins: the higher version, exactly as before the rule existed.
    host.scan(d.string());
    {
        const LoadedPlugin* o = recordFor(host.plugins(), modName(kOld));
        const LoadedPlugin* n = recordFor(host.plugins(), modName(kNew));
        CHECK(o != nullptr && n != nullptr);
        if (o != nullptr && n != nullptr) {
            CHECK(!o->loaded && n->loaded);
            CHECK(o->name == n->name && !o->name.empty());  // the probes do declare ONE name
            CHECK(o->version == "1.0.0" && n->version == "1.1.0");
            CHECK(o->origin == cascade::core::PluginOrigin::Unrecorded);
            CHECK(contains(o->error, "Ignored: another copy"));
        }
    }

    // THE CASE THE RULE EXISTS FOR: official 1.0.0, site 1.1.0. The official runs.
    {
        PluginFileOrigins origins;
        origins[modName(kOld)] = false;
        origins[modName(kNew)] = true;
        host.scan(d.string(), origins);
        const LoadedPlugin* o = recordFor(host.plugins(), modName(kOld));
        const LoadedPlugin* n = recordFor(host.plugins(), modName(kNew));
        CHECK(o != nullptr && n != nullptr);
        if (o != nullptr && n != nullptr) {
            CHECK(o->loaded && o->error.empty());
            CHECK(o->origin == cascade::core::PluginOrigin::Official);
            CHECK(!n->loaded);
            CHECK(n->origin == cascade::core::PluginOrigin::Site);
            CHECK(n->supersededBy == modName(kOld));
            CHECK(contains(n->error, "Not loaded: it declares the same name as the official plugin \"" + n->name + "\""));
            CHECK(contains(n->error, modName(kOld)));
            // Turned off AND unmapped, as every refused module is.
            CHECK(n->nativeHandle == nullptr);
            CHECK(n->decoder == nullptr && n->iqDecoder == nullptr && n->imageDecoder == nullptr);
        }
        CHECK(host.loadedCount() == 1u);
    }

    // The same, with the key spelt in the other case.
    {
        PluginFileOrigins origins;
        std::string upper = modName(kOld);
        for (char& c : upper) { if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 'a' + 'A'); } }
        origins[upper] = false;
        origins[modName(kNew)] = true;
        host.scan(d.string(), origins);
        const LoadedPlugin* o = recordFor(host.plugins(), modName(kOld));
        CHECK(o != nullptr && o->loaded && o->origin == cascade::core::PluginOrigin::Official);
    }

    // Two site modules: the higher version, as ever.
    {
        PluginFileOrigins origins;
        origins[modName(kOld)] = true;
        origins[modName(kNew)] = true;
        host.scan(d.string(), origins);
        const LoadedPlugin* o = recordFor(host.plugins(), modName(kOld));
        const LoadedPlugin* n = recordFor(host.plugins(), modName(kNew));
        CHECK(o != nullptr && n != nullptr);
        if (o != nullptr && n != nullptr) {
            CHECK(!o->loaded && n->loaded);
            CHECK(contains(o->error, "Ignored: another copy"));
        }
    }

    // Official 1.1.0 and site 1.0.0: the official runs, by the version rule and the new one alike.
    {
        PluginFileOrigins origins;
        origins[modName(kOld)] = true;
        origins[modName(kNew)] = false;
        host.scan(d.string(), origins);
        const LoadedPlugin* o = recordFor(host.plugins(), modName(kOld));
        const LoadedPlugin* n = recordFor(host.plugins(), modName(kNew));
        CHECK(o != nullptr && n != nullptr);
        if (o != nullptr && n != nullptr) {
            CHECK(!o->loaded && n->loaded);
            CHECK(contains(o->error, "official plugin"));  // the site's older copy says why, too
        }
    }

    // A file with no record is unrecorded and competes by version: official 1.0.0 (recorded) against a
    // hand-copied 1.1.0 (no record) is the old rule.
    {
        PluginFileOrigins origins;
        origins[modName(kOld)] = false;
        host.scan(d.string(), origins);
        const LoadedPlugin* o = recordFor(host.plugins(), modName(kOld));
        const LoadedPlugin* n = recordFor(host.plugins(), modName(kNew));
        CHECK(o != nullptr && n != nullptr);
        if (o != nullptr && n != nullptr) {
            CHECK(!o->loaded && n->loaded);
            CHECK(n->origin == cascade::core::PluginOrigin::Unrecorded);
        }
    }

    host.unloadAll();
    std::error_code ec;
    fs::remove_all(d, ec);
}

// --- the real AppWindow, installed.json naming the files -----------------------------------

std::string manifestWith(const std::string& oldSource, const std::string& newSource) {
    const auto rec = [](const std::string& id, const std::string& version, const std::string& file,
                        const std::string& source) {
        std::string r = "{\"id\":\"" + id + "\",";
        if (!source.empty()) { r += "\"source\":\"" + source + "\","; }
        r += "\"version\":\"" + version + "\",\"file\":\"" + file + "\"}";
        return r;
    };
    return "{\"schemaVersion\":1,\"plugins\":[" + rec("probe-a", "1.0.0", modName(kOld), oldSource) + "," +
           rec("probe-a", "1.1.0", modName(kNew), newSource) + "]}";
}

void testAppWindow() {
    const std::string site = "https://hobbyist.example/index.json";
    const fs::path dir = fs::path(PluginHost::userPluginDir());
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    stage(dir);

    // official 1.0.0 (no source) + a site's 1.1.0
    writeText(dir / "installed.json", manifestWith("", site));
    DiagLog::instance().resetForTest();
    {
        cascade::gui::AppWindow app;
        // (A COPY of the engine's records, made on the pass: the pointers below point into it, so it lives as long as they are used.)
        std::vector<LoadedPlugin> snap = Access::plugins(app);
        const LoadedPlugin* o = recordFor(snap, modName(kOld));
        const LoadedPlugin* n = recordFor(snap, modName(kNew));
        CHECK(o != nullptr && n != nullptr);
        if (o != nullptr && n != nullptr) {
            CHECK(o->loaded);
            CHECK(!n->loaded);
            CHECK(n->supersededBy == modName(kOld));
            CHECK(contains(n->error, "the official plugin"));
            // THE FITTED MODULES ROW says why, in the host's words and no others: the module is
            // REFUSED in the window's vocabulary, and its sentence is the host's reason verbatim.
            const cascade::gui::FittedModule row =
                cascade::gui::makeFittedModule(*n, false, false, std::string(), false);
            CHECK(cascade::gui::fittedState(row, false) == cascade::gui::FittedState::Refused);
            CHECK(cascade::gui::fittedStateSentence(row, false) == n->error);
            CHECK(contains(cascade::gui::fittedStateSentence(row, false),
                           "Not loaded: it declares the same name as the official plugin"));
        }
        // The log says it, once, in the host's words, with the file.
        CHECK(logged("plugin: NOT loaded " + modName(kNew)) == 1u);
        CHECK(logged("Not loaded: it declares the same name as the official plugin") == 1u);
        // A module turned off for a name is not a refused one: the scan can be skipped when nothing moves.
        CHECK(Access::scanSkippable(app));

        // Both records a site's: the higher version, as before.
        writeText(dir / "installed.json", manifestWith(site, site));
        Access::rescan(app);
        snap = Access::plugins(app);
        o = recordFor(snap, modName(kOld));
        n = recordFor(snap, modName(kNew));
        CHECK(o != nullptr && n != nullptr);
        if (o != nullptr && n != nullptr) { CHECK(!o->loaded && n->loaded); }

        // The newer is the official one: it runs (and it would have under the version rule alone).
        writeText(dir / "installed.json", manifestWith(site, ""));
        Access::rescan(app);
        snap = Access::plugins(app);
        o = recordFor(snap, modName(kOld));
        n = recordFor(snap, modName(kNew));
        CHECK(o != nullptr && n != nullptr);
        if (o != nullptr && n != nullptr) { CHECK(!o->loaded && n->loaded); }

        // No manifest at all: unrecorded, the old rule.
        fs::remove(dir / "installed.json", ec);
        Access::rescan(app);
        snap = Access::plugins(app);
        o = recordFor(snap, modName(kOld));
        n = recordFor(snap, modName(kNew));
        CHECK(o != nullptr && n != nullptr);
        if (o != nullptr && n != nullptr) { CHECK(!o->loaded && n->loaded); }

        Access::takeDown(app);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("test_plugin_host_origin\n");
    if (argc < 3) {
        std::printf("usage: test_plugin_host_origin <probe a 1.0.0> <probe a 1.1.0>\n");
        return 2;
    }
    g_oldBuild = argv[1];
    g_newBuild = argv[2];
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_host_origin_" + std::to_string(pid));
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
    id.fullName = "host-origin-test";
    cascade::core::setPackageIdentityForTest(id);

    testHostScan();
    testAppWindow();

    fs::remove_all(g_scratch, ec);
    return testSummary("test_plugin_host_origin");
}
