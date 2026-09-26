// test_engine_headless.cpp - the ENGINE with no window (engine extraction
// stage 3a, docs/engine-stage3.md).
//
// Since stage 3a the receiver - the Pipeline, the source, the recorders, the
// transmitter, the plugins, the scanner, the one receiver snapshot - is owned
// by cascade::engine::Engine, and gui::AppWindow is only one front end that
// holds one. This test is a SECOND front end: it constructs an Engine with no
// window at all, drives it the way the engine API will (commands in through
// applyCommand / submitCommand, the per-frame pump, a config in through
// applyConfig) and reads everything back from the published snapshot - the
// same block the plugin host API, the web server and CAT read.
//
// IT LINKS THE ENGINE ALONE. tests/CMakeLists.txt links a test that includes
// nothing of the window (no gui/ header, no imgui, no GLFW) against
// cascade_engine only, so this binary existing is itself the proof that an
// Engine can be built, started, tuned, recorded from and torn down with no
// window code in the process.
//
// HERMETIC: every per-user folder is a scratch directory (the recordings land
// in it: Engine::initialise takes the record folder from USERPROFILE/HOME),
// telemetry points at 127.0.0.1:9, no USB or SoapySDR scan runs (the Engine's
// test hooks answer "no radios"), and the plugin directory is an empty
// scratch folder. The source is the built-in signal generator; no radio is
// opened. The volume is set to 0 before the receiver starts.
//
// Each numbered check was seen RED against a mutant of the engine (the
// mutant, the failing line and the green rerun are in docs/engine-stage3.md).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
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
#include "core/config.hpp"
#include "core/receiver_snapshot.hpp"
#include "engine/engine.hpp"
#include "test_check.hpp"

namespace {

namespace cmd = cascade::core::cmd;
namespace fs = std::filesystem;
using cascade::engine::Engine;

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
    g_scratch = fs::temp_directory_path() / ("foxsdr_engine_headless_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    g_plugins = g_scratch / "plugins";
    fs::create_directories(g_plugins);
}

std::vector<cascade::source::NativeDeviceInfo> noNativeRadios() { return {}; }
std::vector<cascade::source::SoapyDeviceInfo> noSoapyRadios() { return {}; }
std::string pluginDir() { return g_plugins.string(); }

bool ok(const FoxCommandResult& r) {
    return r.status == FOXAPI_OK && (r.flags & FOXAPI_RESULT_REFUSED) == 0u;
}

cascade::core::PublishedState published(const Engine& e) {
    cascade::core::PublishedState ps = cascade::core::initialPublishedState();
    (void)e.snapshot()->read(ps);
    return ps;
}

// Pumps the engine like a front end's loop (about 200 passes a second) until
// `done` holds for what it published, or the time runs out.
bool pumpUntil(Engine& e, const std::function<bool(const cascade::core::PublishedState&)>& done,
               int ms = 5000) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        e.pump();
        if (done(published(e))) { return true; }
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(ms)) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void pumpFor(Engine& e, int ms) {
    (void)pumpUntil(e, [](const cascade::core::PublishedState&) { return false; }, ms);
}

std::vector<fs::path> takesIn(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".wav") { out.push_back(entry.path()); }
    }
    return out;
}

}  // namespace

namespace cascade::gui {

// The friend the Engine names for its tests: used here ONLY to install the
// Engine's test hooks (no radio scan, a scratch plugin folder). Everything the
// test drives and reads goes through the Engine's public surface.
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
    const fs::path recDir = g_scratch / "Documents" / "SDR-recordings";

    {
        // [1] A HEADLESS ENGINE STARTS AND PUBLISHES. No host is given: the
        // engine answers its own host questions (engine_host.cpp).
        Engine e;
        e.initialise();
        CHECK(!published(e).app.published);
        e.pump();
        const cascade::core::PublishedState ps0 = published(e);
        std::printf("[1] after one pump: published %d, running %d, device '%s'\n",
                    ps0.app.published ? 1 : 0, (ps0.rx.flags & FOXAPI_RX_RUNNING) ? 1 : 0,
                    ps0.rx.deviceName);
        CHECK(ps0.app.published);
        CHECK((ps0.rx.flags & FOXAPI_RX_RUNNING) == 0u);
        CHECK((ps0.rx.flags & FOXAPI_RX_DEVICE_OPEN) == 0u);

        // [2] THE GENERATOR, SILENT, STARTED - through applyCommand, read
        // back from the snapshot the next pump publishes.
        CHECK(ok(e.applyCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.0))));
        const cascade::core::cmd::QueuedCommand gen = cmd::makeText(FOXAPI_OP_SELECT_SOURCE, "siggen");
        const FoxCommandResult sel = e.applyCommand(gen.c, gen.longText);
        std::printf("[2] SELECT_SOURCE siggen: status %d flags %u\n", sel.status, sel.flags);
        CHECK(sel.status == FOXAPI_OK || sel.status == FOXAPI_NO_CHANGE);
        CHECK((sel.flags & FOXAPI_RESULT_REFUSED) == 0u);
        CHECK(ok(e.applyCommand(cmd::makeInt(FOXAPI_OP_RUN, 1))));
        CHECK(pumpUntil(e, [](const cascade::core::PublishedState& ps) {
            return (ps.rx.flags & FOXAPI_RX_RUNNING) != 0u;
        }));
        CHECK(published(e).rx.volume == 0.0);

        // [3] TUNE: the tuned frequency the snapshot reports.
        CHECK(ok(e.applyCommand(cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 100.3e6))));
        CHECK(pumpUntil(e, [](const cascade::core::PublishedState& ps) {
            return std::fabs(ps.rx.tunedHz - 100.3e6) < 1.0;
        }));
        std::printf("[3] tuned %.0f Hz (centre %.0f, offset %.0f)\n", published(e).rx.tunedHz,
                    published(e).rx.centreHz, published(e).rx.vfoOffsetHz);

        // [4] A QUEUED COMMAND IS APPLIED BY THE PUMP: submitted, it changes
        // nothing until the next pass drains the queue, and that pass's
        // publish carries it.
        CHECK(published(e).rx.demodMode != FOXAPI_DEMOD_AM);
        e.submitCommand(cmd::makeInt(FOXAPI_OP_SET_MODE, FOXAPI_DEMOD_AM));
        CHECK(published(e).rx.demodMode != FOXAPI_DEMOD_AM);
        e.pump();
        std::printf("[4] mode after one pump: %u (AM is %u)\n", published(e).rx.demodMode,
                    FOXAPI_DEMOD_AM);
        CHECK(published(e).rx.demodMode == FOXAPI_DEMOD_AM);

        // [5] RECORD AUDIO to the scratch folder: the take is flagged in the
        // snapshot while it runs, and ends as a finished WAV with samples in
        // it (a 44-byte header alone is a take that heard nothing).
        CHECK(takesIn(recDir).empty());
        CHECK(ok(e.applyCommand(cmd::makeInt(FOXAPI_OP_RECORD_AUDIO, 1))));
        CHECK(pumpUntil(e, [](const cascade::core::PublishedState& ps) {
            return (ps.rx.flags & FOXAPI_RX_RECORDING_AUDIO) != 0u;
        }));
        pumpFor(e, 1500);
        CHECK(ok(e.applyCommand(cmd::makeInt(FOXAPI_OP_RECORD_AUDIO, 0))));
        CHECK(pumpUntil(e, [](const cascade::core::PublishedState& ps) {
            return (ps.rx.flags & FOXAPI_RX_RECORDING_AUDIO) == 0u;
        }));
        const std::vector<fs::path> takes = takesIn(recDir);
        std::error_code ec;
        const std::uintmax_t bytes = takes.size() == 1u ? fs::file_size(takes[0], ec) : 0u;
        std::printf("[5] takes in the scratch folder: %zu, %ju bytes\n", takes.size(), bytes);
        CHECK(takes.size() == 1u);
        CHECK(bytes > 44u + 1000u);

        // [6] THE RECEIVER'S HALF OF A CONFIG, written by the engine alone.
        cascade::core::AppConfig out;
        e.fillConfig(out);
        std::printf("[6] fillConfig: mode %s, volume %.2f, source %s\n", out.mode.c_str(),
                    static_cast<double>(out.volume), out.sourceKind.c_str());
        CHECK(out.mode == "AM");
        CHECK(out.volume == 0.0f);
        CHECK(out.sourceKind == "siggen");

        CHECK(ok(e.applyCommand(cmd::makeInt(FOXAPI_OP_RUN, 0))));
        CHECK(pumpUntil(e, [](const cascade::core::PublishedState& ps) {
            return (ps.rx.flags & FOXAPI_RX_RUNNING) == 0u;
        }));
        // Destroyed WITHOUT stopTransfers()/teardown(): ~Engine runs both.
    }

    {
        // [7] A CONFIG RESTORED INTO A FRESH ENGINE, read back from what it
        // publishes: the mode, the centre and the volume the file named.
        cascade::core::AppConfig cfg;
        cfg.mode = "USB";
        cfg.centerHz = 145.5e6;
        cfg.volume = 0.25f;
        Engine e;
        e.initialise();
        e.applyConfig(cfg);
        e.pump();
        const cascade::core::PublishedState ps = published(e);
        std::printf("[7] restored: mode %u (USB is %u), centre %.0f, volume %.2f\n", ps.rx.demodMode,
                    FOXAPI_DEMOD_USB, ps.rx.centreHz, ps.rx.volume);
        CHECK(ps.rx.demodMode == FOXAPI_DEMOD_USB);
        CHECK(ps.rx.centreHz == 145.5e6);
        CHECK(std::fabs(ps.rx.volume - 0.25) < 1e-6);

        // [8] AN EXPLICIT TEARDOWN, as a front end takes it (the window's
        // destructor): transfers first, then the rest; ~Engine then does
        // nothing more.
        e.stopTransfers();
        e.teardown();
    }

    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return testSummary("test_engine_headless");
}
