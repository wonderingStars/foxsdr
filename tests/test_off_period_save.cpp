// THE EDGE OF A DATA SWITCH IS DURABLE AT ONCE (0.99.115, brief 80, limit 1 of lane 79).
//
// Rule R: nothing that happened while a switch was OFF is ever sent. The engine clears what the family kept in memory at each edge; but config.json is written by the keeper, a
// debounce window (about two seconds) after the last change - and in that gap the file still held the earlier period's pending usage record and the "enabled" flag, which a crash
// right after the click would have left for the next start to send. The edge now asks the keeper for a save at once (Engine::switchEdges_, EngineService::maybeSave).
//
// Held here with a real EngineService on a scratch settings file, the keeper driven as foxsdr-engine's wait loop drives it, and the file read as a fresh start would read it,
// without any periodic save (the debounce is two seconds; every wait below is under 1.4 s):
//
//   E1  Usage reporting OFF: within the wait, config.json holds no "enabled" flag, no install id and no pending record; ON: the enabled flag and a NEW install id
//   E2  Diagnostics OFF (the interface's half, as both windows send it): within the wait, config.json says diagnosticsEnabled false; ON: true
//   E3  Usage ON throughout, a setting changed: the save is still debounced (nothing is saved at once for an ordinary change: 0.99.114's behaviour)
//
// Break-it: EngineService::maybeSave without the edge branch (or the consent command / diagnosticsFamilyEdge without switchEdges_.fetch_add) -> E1 and E2 RED.
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
#include "core/config_halves.hpp"
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

void tick(EngineService& svc, int ms) {
    tickUntil(svc, ms, [] { return false; });
}

json readConfig(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) { return json(); }
    const json j = json::parse(emt::readFile(p), nullptr, false);
    return j.is_object() ? j : json();
}

bool enabledOnDisk(const fs::path& p) { const json j = readConfig(p); return j.is_object() && j.value("telemetryEnabled", false); }
std::string idOnDisk(const fs::path& p) { const json j = readConfig(p); return j.is_object() ? j.value("telemetryInstallId", std::string()) : std::string(); }
std::string pendingOnDisk(const fs::path& p) { const json j = readConfig(p); return j.is_object() ? j.value("telemetryPending", std::string()) : std::string(); }
bool diagOnDisk(const fs::path& p) { const json j = readConfig(p); return j.is_object() && j.value("diagnosticsEnabled", true); }

constexpr int kWaitMs = 1400;   // under the keeper's two-second debounce window

}  // namespace

int main() {
    std::printf("test_off_period_save\n");
    const fs::path root = emt::scratch("off-period-save");
    const fs::path home = root / "home";
    fs::create_directories(home / "tmp");
    emt::isolate(home);

    const fs::path cfg = home / "k" / "config.json";
    fs::create_directories(cfg.parent_path());
    {
        cascade::core::AppConfig seed;
        seed.volume = 0.0f;
        seed.telemetryEnabled = true;
        seed.telemetryInstallId = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
        seed.telemetryFirstRun = "2026-01-02";
        seed.telemetryFirstVersion = "0.99.80";
        seed.telemetryCleanExit = true;
        seed.diagnosticsEnabled = true;
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
    const std::uint64_t savesAtStart = svc.savesRequested();
    CHECK(savesAtStart == 1);
    CHECK(tickUntil(svc, 5000, [&] { return enabledOnDisk(cfg); }));
    const std::string firstId = idOnDisk(cfg);
    CHECK(!firstId.empty());
    // an ON period's file carries the pending usage record (what a crash would leave for the next start to send)
    CHECK(!pendingOnDisk(cfg).empty());

    // --- E1: Usage reporting ------------------------------------------------------------------------------------------------------------------------
    {
        std::printf("  E1  Usage reporting OFF then ON: config.json holds the edge within %d ms, before any periodic save\n", kWaitMs);
        svc.engine().submitCommand(cmd::makeInt(FOXAPP_OP_TELEMETRY_CONSENT, 0));
        const bool offDurable = tickUntil(svc, kWaitMs, [&] { return !enabledOnDisk(cfg); });
        CHECK(offDurable);                                         // (without the edge branch this is the debounce: 2 s, and the wait gives up first)
        CHECK(idOnDisk(cfg).empty());
        CHECK(pendingOnDisk(cfg).empty());                         // the earlier period's pending record is gone from the file
        svc.engine().submitCommand(cmd::makeInt(FOXAPP_OP_TELEMETRY_CONSENT, 1));
        const bool onDurable = tickUntil(svc, kWaitMs, [&] { return enabledOnDisk(cfg); });
        CHECK(onDurable);
        CHECK(!idOnDisk(cfg).empty() && idOnDisk(cfg) != firstId);   // a new life under a new id
    }

    // --- E2: Diagnostics, as the interface's half carries it -----------------------------------------------------------------------------------------
    {
        std::printf("  E2  Diagnostics OFF then ON (the interface's half): config.json holds the edge within %d ms\n", kWaitMs);
        CHECK(diagOnDisk(cfg));
        cascade::core::AppConfig half = svc.engine().configSnapshot();
        half.diagnosticsEnabled = false;
        svc.engine().submitCommand(cmd::makeText(FOXAPP_OP_INTERFACE_CONFIG, cascade::core::serializeInterfaceHalf(half)));
        CHECK(tickUntil(svc, kWaitMs, [&] { return !diagOnDisk(cfg); }));
        half.diagnosticsEnabled = true;
        svc.engine().submitCommand(cmd::makeText(FOXAPP_OP_INTERFACE_CONFIG, cascade::core::serializeInterfaceHalf(half)));
        CHECK(tickUntil(svc, kWaitMs, [&] { return diagOnDisk(cfg); }));
    }

    // --- E3: an ordinary change is still debounced -----------------------------------------------------------------------------------------------------
    {
        std::printf("  E3  an ordinary setting is still saved after the debounce window, not at once (0.99.114's behaviour)\n");
        tick(svc, 600);
        const std::uint64_t before = svc.savesRequested();
        svc.engine().submitCommand(cmd::makeNum(FOXAPI_OP_SET_SQUELCH, -66.0));
        tick(svc, 900);
        CHECK(svc.savesRequested() == before);
        CHECK(tickUntil(svc, 6000, [&] { return svc.savesRequested() == before + 1; }));
    }

    svc.stop();
    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(root, ec); }
    return testSummary("test_off_period_save");
}
