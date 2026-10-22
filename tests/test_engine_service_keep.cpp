// The headless engine KEEPS its settings saved while it runs (engine extraction stage 7c): the keeper of
// src/engine/engine_service.cpp, held against what the window's frame loop does about the same file
// (AppWindow::pollConfigWriter and maybeSaveConfig), in-process with a real EngineService.
//
// WHAT THE KEEPER MUST DO, which the process-level tests (test_engine_main_*) cannot drive because a headless
// engine has no way yet to change a setting from outside (the provisioning of stage 7e is what will give it one):
//   1. QUIET WHEN NOTHING CHANGES. After the forced first save the file is not written again for as long as the
//      settings stand still, though the usage report in the snapshot grows a second every second (the compare leaves
//      it, the exit marker and the session start out, as the window's configsEqual does);
//   2. DEBOUNCED. A change is saved about two seconds after the last change, not at once: nothing is requested within a
//      second of it, one write is requested a little over two seconds after it, and the file then holds the new value;
//   3. COALESCED. A burst of changes spaced under the debounce window is ONE save, after the burst, of the last value
//      - an in-progress drag never writes mid-gesture;
//   4. BACKED OFF. When the write fails (here: a folder where the file should be, so the atomic replace is refused) the
//      keeper does not ask again every debounce window: the retry waits twice as long, and when the obstacle is gone
//      the next write lands and the value is on disk;
//   5. THE STOP. The run ends with the clean-exit marker on disk and the last value in the file.
//
// Break-it (docs/engine-stage4.md 6.17): the keeper's compare left the usage report in (comparable() without
// `telemetryPending.clear()`) -> 2 is RED (the debounce never settles, so the change is never saved); the debounce taken as zero -> 2 and 3 RED; a failed
// write retried at the debounce window -> 4 RED.
//
// HERMETIC: this process's per-user folders and temp are a scratch tree for the whole run, the settings file is a
// scratch path, the volume is seeded 0 (silent), the telemetry endpoint is a closed local port.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "engine/engine_service.hpp"
#include "engine_main_proc.hpp"
#include "test_check.hpp"

namespace emt = engine_main_test;
namespace fs = std::filesystem;
namespace cmd = cascade::core::cmd;
using json = nlohmann::json;
using cascade::engine::EngineService;
using cascade::engine::ServiceOptions;

namespace {

// Call the keeper the way foxsdr-engine's wait loop does (every ~20 ms) for `ms`.
void tick(EngineService& svc, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        svc.keep();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// Tick until `cond()` or `ms` has passed. True when it came true.
template <class F>
bool tickUntil(EngineService& svc, int ms, F cond) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        svc.keep();
        if (cond()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return cond();
}

json readConfig(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) { return json(); }
    const json j = json::parse(emt::readFile(p), nullptr, false);
    return j.is_object() ? j : json();
}

double squelchOnDisk(const fs::path& p) {
    const json j = readConfig(p);
    return j.is_object() && j.contains("squelchDb") ? j["squelchDb"].get<double>() : 9999.0;
}

void setSquelch(EngineService& svc, double db) { svc.engine().submitCommand(cmd::makeNum(FOXAPI_OP_SET_SQUELCH, db)); }

}  // namespace

int main() {
    std::printf("test_engine_service_keep\n");
    const fs::path root = emt::scratch("engine-keep");
    const fs::path home = root / "home";
    fs::create_directories(home / "tmp");
    emt::isolate(home);
    CHECK(cascade::core::ConfigStore::defaultPath().rfind(home.string(), 0) == 0);

    const fs::path cfg = home / "k" / "config.json";
    fs::create_directories(cfg.parent_path());
    {
        cascade::core::AppConfig seed;
        seed.volume = 0.0f;
        seed.squelchDb = -100.0f;
        std::ofstream out(cfg, std::ios::binary);
        out << cascade::core::ConfigStore::serialize(seed);
    }

    ServiceOptions options;
    options.configPath = cfg.string();
    EngineService svc(options);
    std::string sentence;
    CHECK(svc.acquireConfig(sentence));
    const EngineService::Restored restored = svc.restore();
    CHECK(restored.loaded && !restored.damaged);
    std::string why;
    CHECK(svc.start(why));
    CHECK(svc.savesRequested() == 1);  // the forced first save, and nothing else yet

    // --- 1. quiet: three seconds of nothing changing is no save -------------------------------------------------------
    tick(svc, 3000);
    std::printf("  1: saves after 3 s idle: %llu\n", static_cast<unsigned long long>(svc.savesRequested()));
    CHECK(svc.savesRequested() == 1);
    CHECK(readConfig(cfg)["telemetryCleanExit"] == false);  // and the forced first save is what is on disk

    // --- 2. a change is saved, debounced ----------------------------------------------------------------------------------
    setSquelch(svc, -70.0);
    tick(svc, 1000);
    CHECK(svc.savesRequested() == 1);  // not at once, and not within a second of it
    CHECK(squelchOnDisk(cfg) == -100.0);
    const bool saved2 = tickUntil(svc, 6000, [&] { return svc.savesRequested() == 2; });
    CHECK(saved2);
    CHECK(tickUntil(svc, 5000, [&] { return squelchOnDisk(cfg) == -70.0; }));  // the write is on the writer's worker: it lands
    tick(svc, 1000);
    CHECK(svc.savesRequested() == 2);  // and ONE save for the one change

    // --- 3. a burst is one save, after it, of the last value ---------------------------------------------------------------
    setSquelch(svc, -60.0);
    tick(svc, 1000);
    setSquelch(svc, -61.0);
    tick(svc, 1000);
    setSquelch(svc, -62.0);
    tick(svc, 1000);                   // three seconds of changes, never two seconds still: nothing was written mid-gesture
    CHECK(svc.savesRequested() == 2);
    CHECK(squelchOnDisk(cfg) == -70.0);
    CHECK(tickUntil(svc, 6000, [&] { return svc.savesRequested() == 3; }));
    CHECK(tickUntil(svc, 5000, [&] { return squelchOnDisk(cfg) == -62.0; }));
    tick(svc, 1000);
    CHECK(svc.savesRequested() == 3);

    // --- 4. a failing write is backed off, and recovers ----------------------------------------------------------------------
    // A FOLDER where the file should be: the atomic replace is refused for as long as it stands.
    {
        std::error_code ec;
        fs::remove(cfg, ec);
        fs::create_directory(cfg, ec);
        CHECK(fs::is_directory(cfg));
    }
    setSquelch(svc, -50.0);
    CHECK(tickUntil(svc, 8000, [&] { return svc.savesRequested() == 4; }));  // the attempt is made (and fails)
    const auto failedAt = std::chrono::steady_clock::now();
    tick(svc, 3000);
    // THE BACK-OFF: the retry waits twice the debounce window (4 s), where an un-backed-off keeper would ask again at once
    // (the state still differs from what the file holds, every look).
    std::printf("  4: saves 3 s after the failed write: %llu\n", static_cast<unsigned long long>(svc.savesRequested()));
    CHECK(svc.savesRequested() == 4);
    {
        std::error_code ec;
        fs::remove(cfg, ec);  // the obstacle is gone
        CHECK(!fs::exists(cfg));
    }
    const bool retried = tickUntil(svc, 20000, [&] { return svc.savesRequested() == 5; });
    CHECK(retried);
    const double retryAfterS = std::chrono::duration<double>(std::chrono::steady_clock::now() - failedAt).count();
    std::printf("  4: the retry came %.1f s after the failure\n", retryAfterS);
    CHECK(retryAfterS >= 3.9);  // not before the doubled wait
    CHECK(tickUntil(svc, 5000, [&] { return squelchOnDisk(cfg) == -50.0; }));

    // --- 5. the stop: the marker, last of all -----------------------------------------------------------------------------------
    svc.stop();
    const json after = readConfig(cfg);
    CHECK(after.is_object() && after["telemetryCleanExit"] == true);
    CHECK(squelchOnDisk(cfg) == -50.0);

    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(root, ec); }
    return testSummary("test_engine_service_keep");
}
