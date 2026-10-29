// test_thread_beats_engine.cpp - the HEADLESS engine's workers beat, and a wedge is counted by the engine's own pass.
//
// docs/engine-stage4.md 6.27. This binary links the engine alone (no window code: tests/CMakeLists.txt links a test that includes
// no gui/ header against cascade_engine only), so it is the headless engine's coverage: the threads that exist in
// `foxsdr-engine` are a subset of the window's, and the ones a bare Engine starts are registered the same way.
//
//   1. THE ENGINE'S KEYED PROBE. A constructed Engine installs the probe over its transmitter's own flag (`transmitting: no`, not
//      `unknown`, in a report taken while it lives), and takes it away in its destructor.
//   2. THE ENGINE'S RECEIVER THREADS ARE REGISTERED. A running receiver (the built-in generator) has its `dsp` and `source`
//      registrations in the registry, with fresh beats, with no window anywhere in the process.
//   3. THE ENGINE'S PASS COUNTS A WEDGE. A worker reported late by a watchdog is counted as `recovered.threadwedge` in the health
//      ledger by the engine's own pass (it reads Registry::wedgesFound() with the other recovery counters), once per wedge, and not
//      before the pass that reads it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
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

#include "core/app_commands.hpp"
#include "core/hang_watchdog.hpp"
#include "core/health_events.hpp"
#include "core/telemetry.hpp"
#include "core/thread_beats.hpp"
#include "engine/engine.hpp"
#include "test_check.hpp"

namespace {

namespace cmd = cascade::core::cmd;
namespace fs = std::filesystem;
namespace beats = cascade::core::beats;
using cascade::core::HangWatchdog;
using cascade::engine::Engine;
using Clock = std::chrono::steady_clock;

fs::path g_scratch;
fs::path g_plugins;

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
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
    g_scratch = fs::temp_directory_path() / ("foxsdr_beats_engine_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME",
                          "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    g_plugins = g_scratch / "plugins";
    fs::create_directories(g_plugins);
}

std::vector<cascade::source::NativeDeviceInfo> noNativeRadios() { return {}; }
std::vector<cascade::source::SoapyDeviceInfo> noSoapyRadios() { return {}; }
std::string pluginDir() { return g_plugins.string(); }

bool ok(const FoxCommandResult& r) { return r.status == FOXAPI_OK && (r.flags & FOXAPI_RESULT_REFUSED) == 0u; }

cascade::core::PublishedState published(const Engine& e) {
    cascade::core::PublishedState ps = cascade::core::initialPublishedState();
    (void)e.snapshot()->read(ps);
    return ps;
}

bool pumpUntil(Engine& e, const std::function<bool(const cascade::core::PublishedState&)>& done, int ms = 8000) {
    const auto t0 = Clock::now();
    for (;;) {
        e.pump();
        if (done(published(e))) { return true; }
        if (Clock::now() - t0 > std::chrono::milliseconds(ms)) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

namespace cascade::gui {

// The friend the Engine names for its tests: used ONLY to install the Engine's test hooks (no radio scan, a scratch plugin folder).
struct AppWindowTestAccess {
    static void installHooks() {
        Engine::testHooks_.nativeScan = &noNativeRadios;
        Engine::testHooks_.soapyScan = &noSoapyRadios;
        Engine::testHooks_.pluginDir = &pluginDir;
    }
};

}  // namespace cascade::gui

int main() {
    isolate();
    cascade::gui::AppWindowTestAccess::installHooks();
    namespace health = cascade::core::health;
    auto& reg = beats::Registry::instance();

    // The probe is nobody's until an Engine exists.
    CHECK(beats::Registry::keyedNow() == -1);
    {
        Engine e;
        e.initialise();
        // 1. THE ENGINE'S PROBE: installed by its constructor, reading its transmitter's flag
        CHECK(beats::Registry::keyedNow() == 0);

        // a health ledger that counts in memory (no profile written)
        auto ledger = health::globalLedger();
        ledger->reset();
        ledger->arm("", cascade::core::newInstallId(), false);

        // 2. THE RECEIVER'S THREADS
        CHECK(ok(e.applyCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.0))));
        const cascade::core::cmd::QueuedCommand gen = cmd::makeText(FOXAPI_OP_SELECT_SOURCE, "siggen");
        const FoxCommandResult sel = e.applyCommand(gen.c, gen.longText);
        CHECK(sel.status == FOXAPI_OK || sel.status == FOXAPI_NO_CHANGE);
        CHECK(ok(e.applyCommand(cmd::makeInt(FOXAPI_OP_RUN, 1))));
        CHECK(pumpUntil(e, [](const cascade::core::PublishedState& ps) { return (ps.rx.flags & FOXAPI_RX_RUNNING) != 0u; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const std::string snap = reg.snapshotText();
        std::printf("the headless engine's registry:\n%s", snap.c_str());
        CHECK(snap.find("  dsp age-ms=") != std::string::npos);
        CHECK(snap.find("  source age-ms=") != std::string::npos);

        // 3. THE ENGINE'S PASS COUNTS A WEDGE
        // (the engine's own watch takes its baseline on its first poll: pump before the wedge)
        e.pump();
        e.pump();
        const fs::path dir = g_scratch / "reports";
        fs::create_directories(dir);
        HangWatchdog w;
        w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
        w.start(dir.string(), 600000);
        reg.setBudgetOverrideForTest(beats::Kind::Disk, 1000);
        {
            beats::Registration r(beats::kThreadDiskJob);
            r.beat("write");
            const auto until = Clock::now() + std::chrono::milliseconds(1000 + 4 * HangWatchdog::kPollMs + 3000);
            while (Clock::now() < until && w.reportsWritten() < 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            CHECK(w.reportsWritten() == 1);
            // THE REPORT TAKEN WHILE AN ENGINE LIVES says the radio was not keyed - not "unknown"
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            const std::string text = readFile(fs::path(w.lastReportPath()));
            CHECK(text.find("\ntransmitting: no\n") != std::string::npos);
            CHECK(text.find("\nthread: disk-job\n") != std::string::npos);
            r.beat("back");
        }
        // not counted until a pass reads the counter...
        {
            const auto c = ledger->counts();
            CHECK(c.find("recovered.threadwedge") == c.end());
        }
        // ...and then counted, once
        for (int i = 0; i < 4; ++i) { e.pump(); }
        {
            const auto c = ledger->counts();
            const auto it = c.find("recovered.threadwedge");
            CHECK(it != c.end() && it->second == 1);
            for (const auto& kv : c) { std::printf("ledger: %s=%u\n", kv.first.c_str(), kv.second); }
        }
        // a second wedge is a second count
        HangWatchdog::resetWedgeFileCountForTest();
        {
            beats::Registration r(beats::kThreadDiskJob);
            r.beat("write");
            const auto until = Clock::now() + std::chrono::milliseconds(1000 + 4 * HangWatchdog::kPollMs + 3000);
            while (Clock::now() < until && w.reportsWritten() < 2) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            r.beat("back");
        }
        for (int i = 0; i < 4; ++i) { e.pump(); }
        {
            const auto c = ledger->counts();
            const auto it = c.find("recovered.threadwedge");
            CHECK(it != c.end() && it->second == 2);
        }
        w.stop();
        reg.clearOverridesForTest();
        ledger->reset();
    }
    // the destructor took the probe away
    CHECK(beats::Registry::keyedNow() == -1);
    CHECK(reg.size() == 0);

    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return testSummary("test_thread_beats_engine");
}
