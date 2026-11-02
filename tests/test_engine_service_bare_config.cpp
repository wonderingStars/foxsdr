// test_engine_service_bare_config.cpp - A SETTINGS FILE NAMED WITHOUT A DIRECTORY (deep review of engine/stage4, batch A; docs/engine-stage4.md 6.31).
//
// `foxsdr-engine --config foxsdr.json` names the settings file relative to the working directory. EngineService::restore() cut the path at its last separator to
// find the folder the crash-loop breaker keeps its marker and its strike records in (and the telemetry ledgers sit in), found none, and passed an EMPTY folder -
// which means a hermetic run: no marker, no records, nothing kept. A plugin that ended the process inside create() then did it again at every start, with no
// strike and no start without it: the breaker was off for exactly the form of the flag a supervisor's unit file is most likely to use, and under a supervisor
// that restarts on failure the engine boot-looped. (The lock, the settings file and the writer all handled the relative path correctly; only the derived
// folder was wrong. The window host always passes an absolute path, so only the headless binary was hit.)
//
//   A  THE FOLDER AND THE NAME, as a table: a path with a directory part gives that part as written, a bare name gives the working directory (absolute), a
//      file at the root gives the root, the empty path gives nothing.
//   B  A SERVICE WITH A BARE NAME KEEPS THE BREAKER'S MARKER beside the settings file, under the name the file earns (core/plugin_marker.hpp
//      markerFileNameFor): the default file name config.json keeps the names the marker has always had; any other settings file has a pair of its own.
//   C  A SERVICE THAT DOES NOT HOLD THE SETTINGS FILE'S LOCK KEEPS NO RECORD BESIDE IT: the window, best effort, started beside a program that has the lock on
//      the same file must not open - read as a dead run's evidence, and zero - the marker the other program is using.
//
// HERMETIC: every per-user folder is a scratch tree, the working directory is a folder of this test's (and is put back), the telemetry endpoint is a closed
// local port. The real settings file is never named.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <filesystem>
#include <string>

#include "core/config.hpp"
#include "core/health_events.hpp"
#include "core/plugin_marker.hpp"
#include "core/telemetry.hpp"
#include "engine/engine.hpp"
#include "engine/engine_service.hpp"
#include "engine_main_proc.hpp"
#include "test_check.hpp"

namespace emt = engine_main_test;
namespace fs = std::filesystem;
using cascade::engine::EngineService;
using cascade::engine::ServiceOptions;

namespace cascade::gui {
// The friend the Engine names for its tests.
struct AppWindowTestAccess {
    static std::string stallLedgerPath(cascade::engine::Engine& e) { return e.telemetryLedgerPath_; }
    static std::string healthLedgerPath(cascade::engine::Engine& e) { return e.healthLedgerPath_; }
};
}  // namespace cascade::gui

// (test_check.hpp has CHECK only; the equality form reads better in a table of strings)
#define CHECK_EQ(a, b) CHECK((a) == (b))

namespace {

void testTable(const fs::path& cwd) {
    std::printf("  A: the folder and the name\n");
    CHECK(EngineService::settingsDirOf("").empty());
    CHECK(EngineService::settingsFileNameOf("").empty());
    // a directory part is returned as written
    CHECK_EQ(EngineService::settingsDirOf("C:\\Users\\steve\\AppData\\Roaming\\foxsdr\\config.json"), std::string("C:\\Users\\steve\\AppData\\Roaming\\foxsdr"));
    CHECK_EQ(EngineService::settingsFileNameOf("C:\\Users\\steve\\AppData\\Roaming\\foxsdr\\config.json"), std::string("config.json"));
    CHECK_EQ(EngineService::settingsDirOf("/home/x/.config/foxsdr/config.json"), std::string("/home/x/.config/foxsdr"));
    CHECK_EQ(EngineService::settingsFileNameOf("/home/x/.config/foxsdr/second.json"), std::string("second.json"));
    CHECK_EQ(EngineService::settingsDirOf("rel/dir/c.json"), std::string("rel/dir"));
    CHECK_EQ(EngineService::settingsDirOf("C:\\config.json"), std::string("C:\\"));   // (the root keeps its separator: "C:" alone is the drive's current folder)
    // a bare name is in the working directory
    const std::string bareDir = EngineService::settingsDirOf("foxsdr.json");
    if (bareDir.empty()) { std::printf("FAIL a bare file name gave an empty folder (a hermetic run)\n"); }
    CHECK(!bareDir.empty());
    CHECK(fs::equivalent(fs::path(bareDir), cwd));
    CHECK(fs::path(bareDir).is_absolute());
    CHECK_EQ(EngineService::settingsFileNameOf("foxsdr.json"), std::string("foxsdr.json"));
    // the root
    CHECK(!EngineService::settingsDirOf("/x.json").empty());
    CHECK_EQ(EngineService::settingsFileNameOf("/x.json"), std::string("x.json"));
}

void testServiceKeepsTheMarker(const fs::path& work) {
    std::printf("  B: a service given a bare file name keeps the breaker's marker beside it\n");
    {
        ServiceOptions o;
        o.configPath = "bare-engine.json";   // relative to the working directory, no separator
        EngineService svc(o);
        std::string sentence;
        CHECK(svc.acquireConfig(sentence));
        (void)svc.restore();
        CHECK(svc.engine().pluginContainment().started());
        const fs::path marker = work / "plugin-play-bare-engine.marker";
        if (!fs::exists(marker)) { std::printf("FAIL no marker beside a settings file named without a directory\n"); }
        CHECK(fs::exists(marker));
        CHECK(!fs::exists(work / cascade::core::marker::kFileName));   // (not the default file's pair: this is another settings file)
        CHECK_EQ(svc.engine().pluginContainment().dir(), EngineService::settingsDirOf("bare-engine.json"));
    }
    cascade::core::marker::resetForTest();
    {
        // the default settings file name keeps the names the marker and the store have always had
        ServiceOptions o;
        o.configPath = "config.json";
        EngineService svc(o);
        std::string sentence;
        CHECK(svc.acquireConfig(sentence));
        (void)svc.restore();
        CHECK(fs::exists(work / cascade::core::marker::kFileName));
    }
    cascade::core::marker::resetForTest();
}

// C: the window (best effort) beside a program that holds the file. The marker the first program is using is read, at open, as what a DEAD run left and then
// zeroed: a second live process would charge the first one's plugins a strike and wipe the blame its own death would need. It keeps no record instead.
void testContendedServiceKeepsNoRecord(const fs::path& work) {
    std::printf("  C: a service that does not hold the lock keeps no record beside the file\n");
    ServiceOptions first;
    first.configPath = "shared-engine.json";
    EngineService holder(first);
    std::string sentence;
    CHECK(holder.acquireConfig(sentence));
    (void)holder.restore();
    const fs::path markerFile = work / "plugin-play-shared-engine.marker";
    CHECK(fs::exists(markerFile));
    cascade::core::marker::set("live-plugin-1.0.0-abi3-win-x64.dll", cascade::core::marker::Phase::Running, "1.0.0");
    const std::string before = emt::readFile(markerFile);
    CHECK(!before.empty());
    {
        ServiceOptions second;
        second.configPath = "shared-engine.json";
        second.lock = cascade::engine::ServiceLock::BestEffort;   // the window's way: carry on without the lock
        EngineService other(second);
        std::string said;
        CHECK(other.acquireConfig(said));   // (it carries on)
        (void)other.restore();
        if (!other.engine().pluginContainment().dir().empty()) { std::printf("FAIL a program without the lock opened a marker beside the file\n"); }
        CHECK(other.engine().pluginContainment().dir().empty());   // hermetic breaker: in-memory marker, no records
    }
    const std::string after = emt::readFile(markerFile);
    if (after != before) { std::printf("FAIL the first program's marker file was changed by the second\n"); }
    CHECK(after == before);
    cascade::core::marker::resetForTest();
}

// D: the telemetry ledgers sit in the settings file's folder, which is the working directory for a bare name. They were handed an EMPTY folder - "memory only" - so the
// stall ledger and the recovery ledger of a headless engine started with a bare --config were never read at start-up or kept across a run.
void testTelemetryLedgersFollowTheFolder(const fs::path& work) {
    std::printf("  D: the telemetry ledgers are beside a settings file named without a directory\n");
    const fs::path settings = work / "bare-telemetry.json";
    std::string err;
    cascade::core::AppConfig seed;
    CHECK(cascade::core::ConfigStore::writeFile(settings.string(), cascade::core::ConfigStore::serialize(seed), err));   // a settings file that loads, so that the start-up path that reads telemetry runs
    ServiceOptions o;
    o.configPath = "bare-telemetry.json";
    EngineService svc(o);
    std::string sentence;
    CHECK(svc.acquireConfig(sentence));
    const EngineService::Restored restored = svc.restore();
    CHECK(restored.loaded);
    using Access = cascade::gui::AppWindowTestAccess;
    const std::string stall = Access::stallLedgerPath(svc.engine());
    const std::string health = Access::healthLedgerPath(svc.engine());
    if (stall.empty() || health.empty()) { std::printf("FAIL a bare settings file name gave the telemetry ledgers no folder (stall \"%s\", health \"%s\")\n", stall.c_str(), health.c_str()); }
    CHECK(!stall.empty());
    CHECK(!health.empty());
    if (!stall.empty()) { CHECK(fs::equivalent(fs::path(stall).parent_path(), work)); }
    if (!health.empty()) { CHECK(fs::equivalent(fs::path(health).parent_path(), work)); }
    cascade::core::marker::resetForTest();
}

}  // namespace

int main() {
    std::printf("test_engine_service_bare_config\n");
    const fs::path root = emt::scratch("bare-config");
    const fs::path home = root / "home";
    fs::create_directories(home / "tmp");
    emt::isolate(home);
    const fs::path work = root / "work";
    fs::create_directories(work);
    std::error_code ec;
    const fs::path before = fs::current_path(ec);
    fs::current_path(work, ec);
    CHECK(!ec);

    testTable(work);
    testServiceKeepsTheMarker(work);
    testContendedServiceKeepsNoRecord(work);
    testTelemetryLedgersFollowTheFolder(work);

    fs::current_path(before, ec);
    fs::remove_all(root, ec);
    return testSummary("test_engine_service_bare_config");
}
