// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// foxsdr-engine: the receiver with no window (engine extraction stage 7b).
//
// WHAT THIS IS. A second executable on the engine library, beside `cascade`
// (the window application, src/main.cpp). It boots as an ENGINE process - the
// same sequence the window application boots through, with the kind's gates
// (src/engine/process_boot.hpp: no instance claim, no link activation, no
// stderr capture, no console handler, hermetic diagnostics and config) - and
// then runs the engine alone: no GLFW, no OpenGL, no ImGui, no display. It links
// cascade_engine only; the configure-time walk (CMakeLists.txt, "the line
// between them") covers it and so does tests/test_engine_main_links_headless.cpp.
//
// WHAT IT DOES TODAY (7b made the skeleton; 7c gave it a life-cycle and a config, 7d the signals,
// 7e the provisioning):
//   * `--version`, the tool modes (`--selftest` and the hidden bench checks) and
//     the child-process helper modes the engine re-spawns itself for
//     (`--enumerate-json`, `--sentinel`, `--sdrplay-probe`, `--sdrplay-service-state`)
//     all answer inside bootProcess, exactly as they do in `cascade`;
//   * otherwise it runs the engine through an EngineService (src/engine/engine_service.hpp),
//     which is the window's life with the window left out: restore, start, keep, stop;
//   * HERMETIC by default, as the skeleton was: no settings file read, written or locked,
//     the built-in signal generator, MUTED (a service has no business playing a demo tone at a
//     sound card), nothing written to the disk at all (the diagnostics and config gates are
//     in boot);
//   * PERSISTENT when asked: `--config PATH` (that file) or `--use-config` (the per-user file the
//     window application uses). The run then takes the lock beside the file FIRST (a second
//     foxsdr-engine, or the window application, on the same file is refused with a sentence),
//     restores the settings the way the window does, keeps them saved while it runs and writes
//     the clean-exit marker last of all, in the window's stop order;
//   * `--seconds N` ends the run after N seconds: the stop is the window's order and the exit
//     code is 0. Without it the run goes on until the process is ended from outside - which,
//     until 7d's signal path, is a kill, and the engine says so on its stderr line.
//
// EXIT CODES (cascade's: 0 success, 1 a refused or failed run, 2 a refused
// helper argument):
//   0   the run was asked to end and did: `--seconds` elapsed and the engine stopped; a
//       tool mode that passed; `--version`
//   1   a refused argument; a tool mode that failed; the settings file is in use by another
//       program; the engine could not be started (it threw, or the receiver did not come up) -
//       the reason is on stderr
//   2   a helper mode refused its arguments (bootProcess)
//
// stdout carries only what a mode prints (`--version`'s one line, a tool mode's
// verdict); the engine's own lines go to stderr, where a service manager keeps them.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>
#include <utility>

#include "core/diag_log.hpp"
#include "core/receiver_snapshot.hpp"
#include "core/version.hpp"
#include "engine/engine.hpp"
#include "engine/engine_service.hpp"
#include "engine/process_boot.hpp"

namespace {

using cascade::engine::Engine;
using cascade::engine::EngineService;

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;

// How long the receiver is given to come up before the run is called failed: the
// first pass has published well inside this on every machine measured; a slower
// one is told, not waited on for ever.
constexpr std::chrono::milliseconds kStartWait{5000};
// The sleep between looks at the clock, the stop request and the settings. The engine
// runs on its own thread; this thread only decides when to end it and keeps the settings
// saved (EngineService::keep, which looks at them a few times a second whatever this is).
constexpr std::chrono::milliseconds kWaitSlice{20};

// Set by whatever ends the run from outside. Nothing sets it in 7c (7d's
// SIGTERM/SIGINT/SIGHUP path and the console-close handler will); the wait loop
// reads it so that path needs no change here.
std::atomic<bool> g_stopRequested{false};

bool receiverRunning(const Engine& engine) {
    cascade::core::PublishedState ps = cascade::core::initialPublishedState();
    (void)engine.snapshot()->read(ps);
    return ps.app.published && (ps.rx.flags & FOXAPI_RX_RUNNING) != 0u;
}

// A positive whole number of milliseconds from the environment, or 0: the bounded-run test seam below.
int envMillis(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') { return 0; }
    char* end = nullptr;
    const long n = std::strtol(v, &end, 10);
    return (end != v && *end == '\0' && n > 0 && n <= 600000L) ? static_cast<int>(n) : 0;
}

// The whole run: lock, restore, start, wait (keeping the settings), stop. Returns the exit code.
int runEngine(const cascade::engine::BootResult& boot) {
    cascade::engine::ServiceOptions options;
    options.configPath = boot.configPath;
    // The hang watchdog's report directory when this run may write diagnostics at all (boot decided) and the
    // stored switch is on - the window's setDiagnosticsDir, handed over the same way (main.cpp).
    options.diagnosticsDir = boot.mayWrite ? cascade::core::diagCrashDir() : std::string();
    // THE TEST SEAM, a bounded run's only (`--seconds`; the window's CASCADE_DIAG_UNLOAD_STALL_MS is honoured
    // with `--frames` only, for the same reason: no interactive run can be wedged by a stray variable).
    if (boot.seconds >= 0.0) { options.unloadStallMs = envMillis("FOXSDR_ENGINE_UNLOAD_STALL_MS"); }
    const bool persistent = !options.configPath.empty();

    EngineService service(std::move(options));
    Engine& engine = service.engine();

    // THE LOCK FIRST, before the settings file is read: a program that is going to rewrite a file it read must
    // be the only one that may (core/config_lock.hpp). Refused: say so, in a sentence, and do not start.
    std::string sentence;
    if (!service.acquireConfig(sentence)) {
        std::fprintf(stderr, "%s\n", sentence.c_str());
        return kExitFailed;
    }

    const EngineService::Restored restored = service.restore();
    if (!restored.error.empty() && !restored.damaged) {
        std::fprintf(stderr, "foxsdr-engine: %s\n", restored.error.c_str());
        service.stop();
        return kExitFailed;
    }

    std::string why;
    if (!service.start(why)) {
        std::fprintf(stderr, "foxsdr-engine: %s\n", why.c_str());
        service.stop();
        return kExitFailed;
    }

    const auto t0 = std::chrono::steady_clock::now();
    const auto elapsedS = [&t0] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };

    bool up = false;
    while (!up && !g_stopRequested.load() &&
           std::chrono::steady_clock::now() - t0 < kStartWait) {
        up = receiverRunning(engine);
        if (!up) {
            service.keep();
            std::this_thread::sleep_for(kWaitSlice);
        }
    }
    int code = kExitOk;
    if (!up && !g_stopRequested.load()) {
        std::fprintf(stderr, "foxsdr-engine: the receiver did not report running within %lld ms\n",
                     static_cast<long long>(kStartWait.count()));
        code = kExitFailed;
    } else {
        const char* tail = boot.seconds >= 0.0 ? "" : " (until ended from outside)";
        if (persistent) {
            std::fprintf(stderr, "foxsdr-engine %s: running, settings file in use (%s)%s\n", cascade::versionString(),
                         restored.damaged ? "damaged, defaults in use" : "restored", tail);
        } else {
            std::fprintf(stderr, "foxsdr-engine %s: running, signal generator, muted, no config%s\n",
                         cascade::versionString(), tail);
        }
        std::fflush(stderr);
        while (!g_stopRequested.load() && (boot.seconds < 0.0 || elapsedS() < boot.seconds)) {
            service.keep();
            std::this_thread::sleep_for(kWaitSlice);
        }
    }

    // THE STOP, in the window's order (EngineService::stop: no sound-card close waited for, quiesce, the
    // final-state save, stop, release, the clean-exit marker, the last save waited for within its bound, the
    // drain).
    const std::uint64_t passes = engine.passesEnded();
    service.stop();
    std::fprintf(stderr, "foxsdr-engine: stopped after %.1f s (%llu passes)\n", elapsedS(),
                 static_cast<unsigned long long>(passes));
    if (persistent) {
        std::fprintf(stderr, "foxsdr-engine: settings saved %llu time(s)\n",
                     static_cast<unsigned long long>(service.savesRequested()));
    }
    return code;
}

}  // namespace

int main(int argc, char** argv) {
    // EVERYTHING before the engine: the console, the helper modes, the crash
    // handlers, the argument parse, the tool modes - in bootProcess, in its order.
    // A helper (`--enumerate-json`, `--sentinel`, ...), a tool mode, `--version`
    // or a refused argument ends the process here.
    const cascade::engine::BootResult boot =
        cascade::engine::bootProcess(cascade::engine::ProcessKind::Engine, argc, argv);
    if (boot.exit) { return boot.exitCode; }
    try {
        return runEngine(boot);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "foxsdr-engine: failed: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "foxsdr-engine: failed: unknown exception\n");
    }
    return kExitFailed;
}
