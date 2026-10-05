// test_plugin_rescan_skip.cpp - a plugin rescan that has nothing to find does not
// tear the plugins down (docs/DIAGNOSTICS.md, "A plugin rescan runs on the GUI
// thread", option C).
//
// THE FAULT. rescanPlugins() destroys every decoder instance, unmaps every module
// and maps them all again, on the thread that draws the window, with no bound -
// and the catalogue fetch ran it every time it finished, whether or not a single
// file or retirement floor had moved. One field session did that twice, three
// seconds apart, and froze for two minutes inside one of them.
//
// WHAT IS PROVED, through the REAL AppWindow against a REAL plugins folder of
// REAL plugin modules (tests/fixtures/rescan_probe_plugin.cpp, which writes a
// line to a file whenever it is mapped, unmapped, asked for a decoder and
// asked to destroy one - so every check below is about what happened to a
// module, not about what the application says it did). The automatic trigger is
// the catalogue fetch, run exactly as CHECK NOW runs it: a worker reads a local
// catalogue and caches its policies, and the GUI thread collects it.
//
//   - with the folder as the last scan left it, the fetch's completion tears
//     nothing down: no module is unmapped or mapped again, no decoder is
//     destroyed - and one log line says so;
//   - the same trigger does the whole rescan when a file was added, removed,
//     replaced by one of another size, or replaced by one of the SAME size and a
//     newer time; and when the retirement floors changed - after which the
//     retired plugin is NOT loaded, and when the floor is lifted it is again;
//   - the same trigger does the whole rescan when the last scan left nothing to
//     compare with, when the folder's listing cannot be made, when the last scan
//     left a module refused (what refused it may be outside the folder), and
//     when the manifest changed while that scan was under way (a floor the scan
//     did not read must not be taken for one it did);
//   - an EXPLICIT rescan (the Fitted modules window's key, an install, a
//     removal) does the whole rescan with nothing changed, always;
//   - and the signature itself (core/plugin_dir_signature.hpp) moves with each
//     of a file's name, size and last-write time, and with nothing else.
//
// Hermetic: the per-user directories are a scratch folder with the process id in
// its name, the plugins folder is the packaged build's per-user one inside it (a
// fake package identity), and the catalogue is a local file - no network, no
// real profile.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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
#include "core/plugin_dir_signature.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_repo.hpp"
#include "gui/app_window.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;

// What the signature read does when the test says so (the hook is installed once,
// before any window exists, and these are what it consults).
namespace {
bool g_failListing = false;  // the listing "cannot be made"
bool g_raceOnce = false;     // a manifest lands between the inventory read and the listing
void writeFloorManifest(const std::string& floor);
bool hookedRead(const std::string& dir, cascade::core::PluginDirSignature& out) {
    if (g_failListing) { return false; }
    if (g_raceOnce) {
        g_raceOnce = false;
        writeFloorManifest("2.0.0");
    }
    return cascade::core::readPluginDirSignature(dir, out);
}
}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static void installHooks() { AppWindow::testHooks_.pluginDirSignature = &hookedRead; }
    // The catalogue fetch exactly as CHECK NOW runs it, to its completion on the
    // GUI thread: the worker reads the local file and caches the policies,
    // pollPluginAsync() collects it and decides what to do about the plugins.
    static bool fetchCatalogue(AppWindow& a, const std::string& url) {
        a.pluginCatalogueUrl_ = url;
        a.startCatalogFetch();
        for (int i = 0; i < 4000; ++i) {
            a.pollPluginAsync();
            if (!a.catalogPending_) { return a.catalogError_.empty(); }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    static void rescan(AppWindow& a) { a.rescanPlugins(); }
    static void forgetSignature(AppWindow& a) { a.pluginScanSigValid_ = false; }
    // What a removal does before its rescan: every instance destroyed, every
    // module unmapped - and nothing loaded again until a scan says so.
    static void takeDown(AppWindow& a) { a.detachAndUnloadPlugins(); }
    static bool signatureTrusted(const AppWindow& a) { return a.pluginScanSigValid_; }
    static std::size_t loadedCount(const AppWindow& a) { return a.pluginHost_.loadedCount(); }
    static std::size_t blockedCount(const AppWindow& a) { return a.pluginBlocked_.size(); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::core::DiagLog;
using cascade::core::PluginDirSignature;
using cascade::core::PluginRepo;

namespace {

#if defined(_WIN32)
const char* const kExt = ".dll";
#elif defined(__APPLE__)
const char* const kExt = ".dylib";
#else
const char* const kExt = ".so";
#endif

fs::path g_scratch;
fs::path g_pluginsDir;
fs::path g_graveyard;  // where a replaced module goes: a mapped file can be moved, not overwritten
fs::path g_probeLog;
fs::path g_catalogue;
std::string g_moduleA;  // the built fixture modules, from the command line
std::string g_moduleB;
int g_buried = 0;

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    ::SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// The per-user directories in a scratch folder with the process id in its name,
// and a package identity so the plugins folder is the per-user one in it, never
// the build tree's.
void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_plugin_rescan_skip_" + std::to_string(pid));
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
    g_probeLog = g_scratch / "probe.log";
    setEnv("RESCAN_PROBE_LOG", g_probeLog.string());
    g_graveyard = g_scratch / "graveyard";
    fs::create_directories(g_graveyard, ec);
    g_catalogue = g_scratch / "catalogue.json";
    cascade::core::PackageIdentity id;
    id.packaged = true;
    id.fullName = "rescan-skip-test";
    cascade::core::setPackageIdentityForTest(id);
    g_pluginsDir = fs::path(cascade::core::PluginHost::userPluginDir());
    fs::create_directories(g_pluginsDir, ec);
}

fs::path moduleFile(const char* which) {
    return g_pluginsDir / (std::string("probe_") + which + kExt);
}

void put(const char* which, const std::string& built) {
    std::error_code ec;
    fs::copy_file(built, moduleFile(which), fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
}

// A module the process has mapped cannot be overwritten (Windows) but can be
// moved aside, which is how a user replaces one. `extraBytes` appended to the
// copy make it a different size (a PE or ELF image tolerates trailing bytes);
// `restoreTime` puts the old last-write time back on it, so a size is the only
// thing that moved; `timeShiftSeconds` instead puts a time that much newer on
// it, so a time is.
void replaceModule(const char* which, const std::string& built, std::size_t extraBytes,
                   bool restoreTime, long timeShiftSeconds) {
    const fs::path p = moduleFile(which);
    std::error_code ec;
    const fs::file_time_type was = fs::last_write_time(p, ec);
    fs::rename(p, g_graveyard / (std::to_string(++g_buried) + "_" + p.filename().string()), ec);
    CHECK(!ec);
    fs::copy_file(built, p, fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    if (extraBytes != 0u) {
        std::ofstream o(p, std::ios::binary | std::ios::app);
        o << std::string(extraBytes, '\0');
    }
    if (restoreTime) { fs::last_write_time(p, was, ec); }
    if (timeShiftSeconds != 0) {
        fs::last_write_time(p, was + std::chrono::seconds(timeShiftSeconds), ec);
    }
    CHECK(!ec);
}

// Every line the probe modules have written, as "<id> <event>".
std::vector<std::string> probeLines() {
    std::vector<std::string> out;
    std::ifstream in(g_probeLog, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (!line.empty()) { out.push_back(line); }
    }
    return out;
}

std::size_t count(const char* id, const char* event) {
    const std::string want = std::string(id) + " " + event;
    std::size_t n = 0;
    for (const std::string& l : probeLines()) {
        if (l == want) { ++n; }
    }
    return n;
}

// What has happened to the modules so far, so a step can say what IT did.
struct Tally {
    std::size_t attach = 0, detach = 0, create = 0, destroy = 0;
    std::size_t aAttach = 0, aDetach = 0, bAttach = 0, bDetach = 0;
    bool operator==(const Tally& o) const {
        return attach == o.attach && detach == o.detach && create == o.create &&
               destroy == o.destroy;
    }
};
Tally tally() {
    Tally t;
    t.aAttach = count("a", "attach");
    t.aDetach = count("a", "detach");
    t.bAttach = count("b", "attach");
    t.bDetach = count("b", "detach");
    t.attach = t.aAttach + t.bAttach;
    t.detach = t.aDetach + t.bDetach;
    t.create = count("a", "create") + count("b", "create");
    t.destroy = count("a", "destroy") + count("b", "destroy");
    return t;
}
void show(const char* what, const Tally& t) {
    std::printf("    %-44s attach %zu  detach %zu  create %zu  destroy %zu\n", what, t.attach,
                t.detach, t.create, t.destroy);
}

// The application's own account: how many log lines carry `needle`.
std::size_t logged(const char* needle) {
    std::size_t n = 0;
    for (const std::string& line : DiagLog::instance().ringSnapshot()) {
        if (line.find(needle) != std::string::npos) { ++n; }
    }
    return n;
}
const char* const kSkipLine = "plugins: reload skipped - nothing changed since the last scan";
const char* const kTookLine = "plugins: reload took";

// A catalogue that describes plugin "a" (and nothing about "b"), with a floor
// when `floor` is not empty.
void writeCatalogue(const std::string& floor) {
    std::ofstream o(g_catalogue, std::ios::binary | std::ios::trunc);
    o << "{\"schemaVersion\":1,\"plugins\":[{\"id\":\"rescan-probe-a\","
         "\"name\":\"Rescan Probe a\",\"version\":\"1.0.0\",\"abiVersion\":"
      << static_cast<unsigned>(CASCADE_PLUGIN_ABI_VERSION);
    if (!floor.empty()) { o << ",\"minSupportedVersion\":\"" << floor << "\""; }
    o << "}]}";
}

// The manifest the catalogue's own merge would produce for "a", with `floor`:
// the plugin recorded as installed (version 1.0.0, its file) and its policy.
void writeFloorManifest(const std::string& floor) {
    cascade::core::InstalledPlugin rec;
    rec.id = "rescan-probe-a";
    rec.name = "Rescan Probe a";
    rec.version = "1.0.0";
    rec.file = std::string("probe_a") + kExt;
    rec.abiVersion = static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION);
    cascade::core::CachedPolicy pol;
    pol.id = rec.id;
    pol.known = true;
    pol.minSupportedVersion = floor;
    pol.catalogueVersion = "1.0.0";
    pol.abiVersion = static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION);
    std::string err;
    CHECK(PluginRepo::saveManifest(g_pluginsDir.string(), {rec}, {pol}, err));
}

bool fetch(cascade::gui::AppWindow& app) {
    return Access::fetchCatalogue(app, g_catalogue.string());
}

// One fetch with nothing done to the folder: true if it tore nothing down and
// said it skipped.
bool fetchSkipped(cascade::gui::AppWindow& app, const char* what) {
    DiagLog::instance().resetForTest();
    const Tally before = tally();
    CHECK(fetch(app));
    const Tally after = tally();
    show(what, after);
    return after == before && logged(kSkipLine) == 1u && logged(kTookLine) == 0u;
}

// The whole rescan happened: every module that was mapped was unmapped, one
// decoder at least was destroyed, and the application says it reloaded and not
// that it skipped. `unmapped` is how many modules were mapped before.
void expectFull(const char* what, const Tally& before, const Tally& after, std::size_t unmapped) {
    show(what, after);
    CHECK(after.detach - before.detach == unmapped);
    CHECK(after.destroy > before.destroy);
    CHECK(logged(kTookLine) == 1u);
    CHECK(logged(kSkipLine) == 0u);
}

// After a full rescan the signature is trusted again: an identical fetch skips.
void expectSettled(cascade::gui::AppWindow& app) {
    CHECK(Access::signatureTrusted(app));
    CHECK(fetchSkipped(app, "...and the next fetch"));
}

// --- the signature, in itself --------------------------------------------------

void testTheSignatureMovesWithNameSizeAndTimeAndNothingElse() {
    std::printf("the signature moves with a file's name, size and time, and with nothing else\n");
    const fs::path d = g_scratch / "sig";
    std::error_code ec;
    fs::create_directories(d, ec);
    const auto write = [&](const char* name, const std::string& text) {
        std::ofstream o(d / name, std::ios::binary | std::ios::trunc);
        o << text;
    };
    write("one.dll", "1234");
    write("two.dll", "abcdefgh");
    write("installed.json", "{}");
    fs::create_directories(d / "sub", ec);

    PluginDirSignature base;
    CHECK(cascade::core::readPluginDirSignature(d.string(), base));
    CHECK(base.listed);
    CHECK(base.files.size() == 3u);  // the folder is not listed, every regular file is
    CHECK(base.find("installed.json") != nullptr);  // not only the modules
    PluginDirSignature again;
    CHECK(cascade::core::readPluginDirSignature(d.string(), again));
    CHECK(again == base);  // reading it changes nothing

    // a file added, and removed
    write("three.dll", "x");
    PluginDirSignature s;
    CHECK(cascade::core::readPluginDirSignature(d.string(), s));
    CHECK(s != base);
    fs::remove(d / "three.dll", ec);
    CHECK(cascade::core::readPluginDirSignature(d.string(), s));
    CHECK(s == base);
    fs::remove(d / "two.dll", ec);
    CHECK(cascade::core::readPluginDirSignature(d.string(), s));
    CHECK(s != base);
    write("two.dll", "abcdefgh");
    fs::last_write_time(d / "two.dll", fs::last_write_time(d / "one.dll"), ec);
    // ...and the time put back, so it is the same file as far as a listing can tell
    {
        PluginDirSignature t0;
        CHECK(cascade::core::readPluginDirSignature(d.string(), t0));
        fs::last_write_time(d / "two.dll", fs::last_write_time(d / "two.dll") - std::chrono::hours(1),
                            ec);
        PluginDirSignature t1;
        CHECK(cascade::core::readPluginDirSignature(d.string(), t1));
        CHECK(t1 != t0);  // the same size and name, a different time: a different signature
    }
    // a size, with the time held
    {
        PluginDirSignature before;
        CHECK(cascade::core::readPluginDirSignature(d.string(), before));
        const fs::file_time_type when = fs::last_write_time(d / "one.dll");
        write("one.dll", "12345");  // one byte more
        fs::last_write_time(d / "one.dll", when, ec);
        PluginDirSignature after;
        CHECK(cascade::core::readPluginDirSignature(d.string(), after));
        CHECK(after != before);
        const PluginDirSignature::Entry* e = after.find("one.dll");
        CHECK(e != nullptr && e->bytes == 5u);
    }
    // a folder that is not there is answered, as the empty list a scan reads it as
    {
        PluginDirSignature none;
        CHECK(cascade::core::readPluginDirSignature((g_scratch / "nowhere").string(), none));
        CHECK(!none.listed);
        CHECK(none.files.empty());
        PluginDirSignature other;
        CHECK(cascade::core::readPluginDirSignature((g_scratch / "elsewhere").string(), other));
        CHECK(other != none);  // a different folder is a different signature
    }
}

// --- the real window -----------------------------------------------------------

void testTheWindow() {
    std::printf("the catalogue fetch against the real window and real modules\n");
    // The state a user's profile is in after one catalogue fetch: "a" installed
    // from it, its policy cached, no floor. The fetch below then has nothing to
    // teach the manifest.
    put("a", g_moduleA);
    writeFloorManifest("");
    writeCatalogue("");

    cascade::gui::AppWindow app;  // its own start-up scan maps probe a
    Tally t0 = tally();
    show("after the start-up scan (a full scan)", t0);
    CHECK(Access::loadedCount(app) == 1u);
    CHECK(t0.aAttach == 1u);
    CHECK(t0.create == 1u);
    CHECK(Access::signatureTrusted(app));  // it left a signature to compare with

    // 1. THE FAULT: the folder is as the scan left it, the fetch finds nothing
    //    new. No module is unmapped, no decoder destroyed.
    std::printf("  folder unchanged\n");
    CHECK(fetchSkipped(app, "the first fetch"));
    CHECK(fetchSkipped(app, "a second, identical fetch"));
    CHECK(Access::loadedCount(app) == 1u);

    // 2. An EXPLICIT rescan - the key on the Fitted modules window, an install, a
    //    removal - does everything, with nothing changed, every time.
    std::printf("  explicit rescan, nothing changed\n");
    {
        DiagLog::instance().resetForTest();
        const Tally before = tally();
        Access::rescan(app);
        expectFull("after Access::rescan", before, tally(), 1u);
        DiagLog::instance().resetForTest();
        const Tally mid = tally();
        Access::rescan(app);
        expectFull("after a second explicit rescan", mid, tally(), 1u);
        expectSettled(app);
    }

    // 3. No signature from a previous scan (the first scan of a session, or one
    //    that did not finish): the whole thing.
    std::printf("  no previous signature\n");
    {
        Access::forgetSignature(app);
        DiagLog::instance().resetForTest();
        const Tally before = tally();
        CHECK(fetch(app));
        expectFull("after the fetch", before, tally(), 1u);
        expectSettled(app);
    }

    // 4. The folder's listing cannot be made: "I do not know" is not "nothing
    //    changed".
    std::printf("  the listing cannot be made\n");
    {
        g_failListing = true;
        DiagLog::instance().resetForTest();
        const Tally before = tally();
        CHECK(fetch(app));
        expectFull("after the fetch", before, tally(), 1u);
        CHECK(!Access::signatureTrusted(app));  // and that scan could not take one either
        g_failListing = false;
        DiagLog::instance().resetForTest();
        const Tally mid = tally();
        CHECK(fetch(app));  // nothing to compare with yet: the whole thing, which takes one
        expectFull("after the next fetch", mid, tally(), 1u);
        expectSettled(app);
    }

    // 5. A file ADDED.
    std::printf("  a file added\n");
    {
        put("b", g_moduleB);
        DiagLog::instance().resetForTest();
        const Tally before = tally();
        CHECK(fetch(app));
        const Tally after = tally();
        expectFull("after the fetch", before, after, 1u);
        CHECK(after.bAttach == 1u);  // the new module is mapped
        CHECK(Access::loadedCount(app) == 2u);
        expectSettled(app);
    }

    // 6. A file REMOVED.
    std::printf("  a file removed\n");
    {
        std::error_code ec;
        fs::remove(moduleFile("b"), ec);  // (not mapped by anything but the host: moved, below)
        if (ec) {
            // A mapped module cannot be deleted on Windows: it is moved out of
            // the folder, which is what a user who removes it does.
            fs::rename(moduleFile("b"), g_graveyard / (std::to_string(++g_buried) + "_b"), ec);
        }
        CHECK(!fs::exists(moduleFile("b")));
        DiagLog::instance().resetForTest();
        const Tally before = tally();
        CHECK(fetch(app));
        const Tally after = tally();
        expectFull("after the fetch", before, after, 2u);
        CHECK(after.bAttach == before.bAttach);  // not mapped again: it is gone
        CHECK(Access::loadedCount(app) == 1u);
        expectSettled(app);
    }

    // 6b. A file RENAMED: the same size and the same time under another name. A
    //     plugin's settings are keyed by its file name (the stop, the tune grant),
    //     so a module under a new name is a different plugin to the host.
    std::printf("  a file renamed\n");
    {
        const fs::path was = moduleFile("a");
        const fs::path now = g_pluginsDir / (std::string("renamed_a") + kExt);
        std::error_code ec;
        fs::rename(was, now, ec);  // a mapped module can be moved, which is what a user does
        CHECK(!ec);
        DiagLog::instance().resetForTest();
        const Tally before = tally();
        CHECK(fetch(app));
        expectFull("after the fetch", before, tally(), 1u);
        CHECK(Access::loadedCount(app) == 1u);
        expectSettled(app);
        fs::rename(now, was, ec);  // and back, for what follows
        CHECK(!ec);
        CHECK(fetch(app));
        expectSettled(app);
    }

    // 7. A file REPLACED by one of another size: the time is held, so the size is
    //    the only thing that moved.
    std::printf("  a file replaced: another size, the time held\n");
    {
        replaceModule("a", g_moduleA, 16u, true, 0);
        DiagLog::instance().resetForTest();
        const Tally before = tally();
        CHECK(fetch(app));
        expectFull("after the fetch", before, tally(), 1u);
        CHECK(Access::loadedCount(app) == 1u);
        expectSettled(app);
    }

    // 8. A file REPLACED by one of the SAME size and a newer time: the time is the
    //    only thing that moved.
    std::printf("  a file replaced: the same size, a newer time\n");
    {
        // (the module is, since the step above, the built one and sixteen bytes)
        replaceModule("a", g_moduleA, 16u, false, 5);   // the same size, five seconds on
        DiagLog::instance().resetForTest();
        const Tally before = tally();
        CHECK(fetch(app));
        expectFull("after the fetch", before, tally(), 1u);
        expectSettled(app);
    }

    // 9. The retirement floors CHANGED: the catalogue now retires 1.0.0. The
    //    retired plugin is not loaded afterwards, and is quarantined.
    std::printf("  the floor moved: the plugin is retired\n");
    {
        writeCatalogue("2.0.0");
        DiagLog::instance().resetForTest();
        const Tally before = tally();
        CHECK(fetch(app));
        const Tally after = tally();
        expectFull("after the fetch", before, after, 1u);
        CHECK(after.aAttach == before.aAttach);        // NOT mapped again
        CHECK(Access::loadedCount(app) == 0u);
        CHECK(Access::blockedCount(app) == 1u);
        CHECK(fs::exists(g_pluginsDir / (std::string("probe_a") + kExt + ".disabled")));
        CHECK(!fs::exists(moduleFile("a")));
        // A retired plugin is not a reason to rescan on every fetch: the folder
        // is what the scan left it.
        expectSettled(app);
        // ...and the floor lifted brings it back, by the same trigger.
        std::printf("  the floor lifted: the plugin is back\n");
        writeCatalogue("");
        DiagLog::instance().resetForTest();
        const Tally mid = tally();
        CHECK(fetch(app));
        const Tally back = tally();
        show("after the fetch", back);
        CHECK(logged(kTookLine) == 1u);
        CHECK(back.aAttach == mid.aAttach + 1u);       // mapped again
        CHECK(Access::loadedCount(app) == 1u);
        CHECK(Access::blockedCount(app) == 0u);
        CHECK(fs::exists(moduleFile("a")));
        expectSettled(app);
    }

    // 10. A manifest that landed while a scan was under way is not taken for one
    //     the scan read. The retirement is in the manifest by the time the scan
    //     lists the folder, and not in what it read: the signature it took
    //     includes the new manifest, so trusting it would leave the retired
    //     plugin running through every later fetch. It is not trusted.
    std::printf("  a floor written while a scan was under way\n");
    {
        g_raceOnce = true;
        Access::rescan(app);  // reads no floor, then the floor lands, then it loads "a"
        CHECK(!g_raceOnce);   // the injection ran
        CHECK(Access::loadedCount(app) == 1u);  // "a" is loaded: that scan did not know
        CHECK(!Access::signatureTrusted(app));
        writeCatalogue("2.0.0");  // the catalogue says what the manifest now says
        DiagLog::instance().resetForTest();
        const Tally before = tally();
        CHECK(fetch(app));
        const Tally after = tally();
        expectFull("after the fetch", before, after, 1u);
        CHECK(Access::loadedCount(app) == 0u);  // the retired plugin is NOT running
        CHECK(Access::blockedCount(app) == 1u);
        expectSettled(app);
        writeCatalogue("");
        CHECK(fetch(app));  // lifted again, for what follows
        CHECK(Access::loadedCount(app) == 1u);
        expectSettled(app);
    }

    // 11. A module REFUSED by the last scan is never skipped: what refused it may
    //     be outside the folder (a runtime installed since), and no listing of the
    //     folder shows that.
    std::printf("  a module the last scan refused\n");
    {
        {
            std::ofstream o(g_pluginsDir / (std::string("junk") + kExt), std::ios::binary);
            o << "this is not a module";
        }
        DiagLog::instance().resetForTest();
        const Tally before = tally();
        CHECK(fetch(app));
        expectFull("after the fetch (the junk file is new)", before, tally(), 1u);
        CHECK(!Access::signatureTrusted(app));
        DiagLog::instance().resetForTest();
        const Tally mid = tally();
        CHECK(fetch(app));  // nothing changed, and still the whole thing
        expectFull("after the next fetch (nothing changed)", mid, tally(), 1u);
        // The refused file goes: the next scan has nothing refused, and then skips.
        std::error_code ec;
        fs::remove(g_pluginsDir / (std::string("junk") + kExt), ec);
        CHECK(fetch(app));
        expectSettled(app);
    }

    // 12. The plugins were taken down and no scan followed - a removal whose
    //     rescan fell short. What the last signature says is loaded is not
    //     loaded, and the folder, which has not moved, must not say it is.
    std::printf("  the plugins taken down, no scan after\n");
    {
        Access::takeDown(app);
        CHECK(Access::loadedCount(app) == 0u);
        CHECK(!Access::signatureTrusted(app));
        DiagLog::instance().resetForTest();
        const Tally before = tally();
        CHECK(fetch(app));
        const Tally after = tally();
        show("after the fetch", after);
        CHECK(after.aAttach == before.aAttach + 1u);  // mapped again
        CHECK(Access::loadedCount(app) == 1u);
        CHECK(logged(kTookLine) == 1u);
        CHECK(logged(kSkipLine) == 0u);
        expectSettled(app);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("test_plugin_rescan_skip\n");
    if (argc < 3) {
        std::printf("usage: test_plugin_rescan_skip <probe a module> <probe b module>\n");
        return 2;
    }
    g_moduleA = argv[1];
    g_moduleB = argv[2];
    isolate();
    Access::installHooks();
    testTheSignatureMovesWithNameSizeAndTimeAndNothingElse();
    testTheWindow();
    return testSummary("test_plugin_rescan_skip");
}
