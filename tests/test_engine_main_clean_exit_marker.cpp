// The clean-exit marker of the headless engine (engine extraction stage 7c): the analogue of
// tests/test_clean_exit_marker.cpp, on the real foxsdr-engine executable.
//
// WHAT THE MARKER IS. telemetryCleanExit in the settings file is the one fact that makes the NEXT start say "the
// last run did not end normally": it is the crash counter and the trigger for the report offer. It is FALSE on
// disk the whole time a run is alive and becomes TRUE only on the very last save of an orderly stop - after the
// pipeline join and after the plugins are unloaded, the two places a shutdown wedges. A service that wrote it
// early would count a wedged `systemctl stop` as a clean exit, and a service that never forced the first
// save would look clean after a crash in its first seconds (the window's own history, app_window.cpp).
//
// WHAT IS ASSERTED, as the file on disk looks to the next start:
//   A. RUNNING: while a bounded run is alive the marker is FALSE, read over two seconds; the run ends
//      with exit 0 and the marker TRUE, one launch and no crash counted; and an idle run SAVES THREE TIMES (the
//      forced first, the final-state, the marker's rewrite) - the keeper does not rewrite the file while
//      nothing changes (its compare ignores what moves by itself: the usage report, the session start, the marker);
//   B. KILLED INSIDE THE UNLOAD STAGE: the run is wedged (the bounded-run seam FOXSDR_ENGINE_UNLOAD_STALL_MS, the
//      engine's CASCADE_DIAG_UNLOAD_STALL_MS) right after the plugins are released and before the marker, with
//      the final-state save already on disk; read then, and after End-task-style TerminateProcess, the marker is
//      FALSE both times;
//   C. COUNTED: the next start counts the crash (telemetryCrashes 0 -> 1, a launch more), and - the positive control -
//      the same stall left to finish writes the marker (TRUE, exit 0), so a "fix" that simply stopped
//      writing the marker is red too;
//   D. KILLED WHILE RUNNING: a plain TerminateProcess of a run that is up leaves the marker FALSE, and the next start
//      counts it (crashes 1 -> 2) and ends clean.
//
// Break-it (docs/engine-stage4.md 6.17): EngineService::stop writes the marker (markCleanExit and the rewrite) BEFORE
// shutdownRelease -> B is RED (the marker is TRUE while the process is wedged in the unload) and
// test_engine_service_order is RED; the forced first save removed from start() -> A and D are RED (no marker
// FALSE on disk).
//
// HERMETIC: every per-user folder is a scratch tree, the settings file a scratch path named on the command line,
// the volume seeded 0 (a service plays what it restores: this test is silent), the telemetry endpoint a closed
// local port. The real %APPDATA% config is never named.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/config.hpp"
#include "engine_main_proc.hpp"
#include "test_check.hpp"

namespace emt = engine_main_test;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// The settings file as the next start would read it, or null when it is not there or not whole right now (the
// engine replaces it by rename; a reader can land in the gap).
json readConfig(const fs::path& p) {
    const std::string text = emt::readFile(p);
    if (text.empty()) { return json(); }
    json j = json::parse(text, nullptr, false);
    return j.is_object() ? j : json();
}

std::optional<bool> marker(const fs::path& p) {
    const json j = readConfig(p);
    if (!j.is_object() || !j.contains("telemetryCleanExit")) { return std::nullopt; }
    return j["telemetryCleanExit"].get<bool>();
}

std::uint64_t counter(const fs::path& p, const char* key) {
    const json j = readConfig(p);
    return j.is_object() && j.contains(key) ? j[key].get<std::uint64_t>() : ~std::uint64_t{0};
}

// Wait up to `ms` for the marker on disk to read `want` (true/false). True when it did.
bool waitMarker(const fs::path& p, bool want, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        const std::optional<bool> m = marker(p);
        if (m.has_value() && *m == want) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

// Wait up to `ms` for `needle` to appear in the child's stderr file.
bool waitErr(const fs::path& errFile, const std::string& needle, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (emt::contains(emt::readText(errFile), needle)) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

}  // namespace

int main() {
    std::printf("test_engine_main_clean_exit_marker\n");
    const fs::path root = emt::scratch("engine-marker");
    const fs::path home = root / "home";
    const fs::path cwd = home / "cwd";
    fs::create_directories(home / "tmp");
    fs::create_directories(cwd);
    emt::isolate(home);
    CHECK(cascade::core::ConfigStore::defaultPath().rfind(home.string(), 0) == 0);

    const fs::path engineExe = emt::appExe("foxsdr-engine");
    CHECK(fs::exists(engineExe));
    const fs::path cfg = home / "cx" / "config.json";
    fs::create_directories(cfg.parent_path());
    {
        // A predecessor that ended cleanly (the default), silent.
        cascade::core::AppConfig seed;
        seed.volume = 0.0f;
        std::ofstream out(cfg, std::ios::binary);
        out << cascade::core::ConfigStore::serialize(seed);
    }
    CHECK(marker(cfg).value_or(false) == true);
    CHECK(counter(cfg, "telemetryLaunches") == 0 && counter(cfg, "telemetryCrashes") == 0);
    const fs::path outDir = root / "out";
    fs::create_directories(outDir);

    // --- A. a bounded run: FALSE while it lives, TRUE after it stops, and quiet while it idles -----------------
    {
        emt::Child run;
        CHECK(run.start(engineExe, {"--config", cfg.string(), "--seconds", "7"}, cwd, outDir / "a.out", outDir / "a.err"));
        // The forced first save reaches the disk (the marker FALSE is what puts the crash on record)...
        CHECK(waitMarker(cfg, false, 30000));
        CHECK(!run.waitExit(0));  // ...while the run is alive
        // ...and stays FALSE for as long as the run lives: read every 100 ms for two seconds.
        bool everTrue = false;
        for (int i = 0; i < 20; ++i) {
            const std::optional<bool> m = marker(cfg);
            if (m.has_value() && *m) { everTrue = true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        CHECK(!everTrue);
        CHECK(!run.waitExit(0));  // still alive: what was read was a live run's file
        CHECK(run.waitExit(60000));
        CHECK(run.code() == 0);
        const std::string err = emt::readText(outDir / "a.err");
        std::printf("  A: stderr:\n%s", err.c_str());
        CHECK(emt::contains(err, "settings file in use (restored)"));
        CHECK(marker(cfg).value_or(false) == true);
        CHECK(counter(cfg, "telemetryLaunches") == 1);
        CHECK(counter(cfg, "telemetryCrashes") == 0);
        // THE KEEPER IS QUIET WHEN NOTHING CHANGES: the forced first save, the final-state save and the marker's
        // rewrite are three; a keeper that asked for a save at every look (four a second) would show dozens in these
        // seven seconds. One more is allowed for a setting that really moved by itself.
        const std::size_t at = err.find("settings saved ");
        const long saves = at == std::string::npos ? -1 : std::strtol(err.c_str() + at + 15, nullptr, 10);
        std::printf("  A: saves %ld\n", saves);
        CHECK(saves >= 3 && saves <= 4);
    }

    // --- B. killed INSIDE the plugin unload: the marker is not on disk yet --------------------------------------
    {
        emt::setEnv("FOXSDR_ENGINE_UNLOAD_STALL_MS", "60000");
        emt::Child run;
        const bool started = run.start(engineExe, {"--config", cfg.string(), "--seconds", "1"}, cwd, outDir / "b.out", outDir / "b.err");
        emt::unsetEnv("FOXSDR_ENGINE_UNLOAD_STALL_MS");
        CHECK(started);
        // PROVABLY inside the stage: the engine says so when it starts the wedge, which is after the final-state
        // save and the pipeline join, and before the marker.
        const bool inside = waitErr(outDir / "b.err", "--diag-unload-stall wedging the plugin unload", 60000);
        std::printf("  B: reached the plugin-unload stall: %s\n", inside ? "yes" : "NO");
        CHECK(inside);
        CHECK(!run.waitExit(0));
        // WHAT THE NEXT START WOULD READ, with the process wedged where a plugin's destroy() runs: the final-state
        // save is on disk (this run's launch is counted) and the marker is not.
        CHECK(counter(cfg, "telemetryLaunches") == 2);
        const std::optional<bool> whileWedged = marker(cfg);
        CHECK(whileWedged.has_value() && *whileWedged == false);
        run.kill();  // End task is TerminateProcess
        const std::optional<bool> afterKill = marker(cfg);
        CHECK(afterKill.has_value() && *afterKill == false);
    }

    // --- C. the next start COUNTS the crash; and the same stall left to finish writes the marker -----------------
    {
        emt::setEnv("FOXSDR_ENGINE_UNLOAD_STALL_MS", "1500");
        emt::Child run;
        const bool started = run.start(engineExe, {"--config", cfg.string(), "--seconds", "1"}, cwd, outDir / "c.out", outDir / "c.err");
        emt::unsetEnv("FOXSDR_ENGINE_UNLOAD_STALL_MS");
        CHECK(started);
        CHECK(waitErr(outDir / "c.err", "--diag-unload-stall wedging the plugin unload", 60000));
        // Inside the (short) stall the marker is still not there...
        const std::optional<bool> during = marker(cfg);
        CHECK(during.has_value() && *during == false);
        // ...and the crash B left is already on disk, counted by THIS start.
        CHECK(counter(cfg, "telemetryCrashes") == 1);
        CHECK(counter(cfg, "telemetryLaunches") == 3);
        CHECK(run.waitExit(60000));
        CHECK(run.code() == 0);
        CHECK(marker(cfg).value_or(false) == true);   // the positive control: the stage finished, the marker arrived
        CHECK(counter(cfg, "telemetryCrashes") == 1);  // a clean end counts nothing more
    }

    // --- D. a run that is UP is killed: FALSE on disk, and the next start counts it --------------------------------
    {
        emt::Child run;  // no --seconds: it runs until it is ended from outside
        CHECK(run.start(engineExe, {"--config", cfg.string()}, cwd, outDir / "d.out", outDir / "d.err"));
        CHECK(waitMarker(cfg, false, 30000));
        CHECK(waitErr(outDir / "d.err", "settings file in use (restored) (until ended from outside)", 30000));
        CHECK(!run.waitExit(500));
        run.kill();
        const std::optional<bool> afterKill = marker(cfg);
        CHECK(afterKill.has_value() && *afterKill == false);
        CHECK(counter(cfg, "telemetryCrashes") == 1);  // not counted yet: it is the NEXT start that counts a death
        const emt::RunResult next =
            emt::run(engineExe, {"--config", cfg.string(), "--seconds", "1"}, cwd, outDir, "e", 120000);
        CHECK(next.started && !next.timedOut && next.exitCode == 0);
        CHECK(counter(cfg, "telemetryCrashes") == 2);
        CHECK(counter(cfg, "telemetryLaunches") == 5);
        CHECK(marker(cfg).value_or(false) == true);
    }

    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(root, ec); }
    return testSummary("test_engine_main_clean_exit_marker");
}
