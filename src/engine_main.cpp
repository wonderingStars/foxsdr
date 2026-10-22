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
// WHAT IT DOES TODAY, deliberately little (7b is the skeleton; 7c gives it a
// life-cycle and a config, 7d the signals, 7e the provisioning):
//   * `--version`, the tool modes (`--selftest` and the hidden bench checks) and
//     the child-process helper modes the engine re-spawns itself for
//     (`--enumerate-json`, `--sentinel`, `--sdrplay-probe`, `--sdrplay-service-state`)
//     all answer inside bootProcess, exactly as they do in `cascade`;
//   * otherwise it constructs an Engine, selects the built-in signal generator,
//     starts the receiver MUTED (a service has no business playing a demo tone
//     at a sound card) and runs the engine's own pass on the engine's own
//     thread, with the window's period and floor. It reads no config, writes no
//     config and serves nothing; it writes no file at all (the diagnostics and
//     config gates are in boot);
//   * `--seconds N` ends the run after N seconds: the stop is the window's order
//     (the sound-card close wait off, then the engine's four teardown phases) and
//     the exit code is 0. Without it the run goes on until the process is ended
//     from outside - which, until 7d's signal path, is a kill, and the engine
//     says so on its stderr line.
//
// EXIT CODES (cascade's: 0 success, 1 a refused or failed run, 2 a refused
// helper argument):
//   0   the run was asked to end and did: `--seconds` elapsed and the engine stopped; a
//       tool mode that passed; `--version`
//   1   a refused argument; a tool mode that failed; the engine could not be started (it
//       threw, or the receiver did not come up) - the reason is on stderr
//   2   a helper mode refused its arguments (bootProcess)
//
// stdout carries only what a mode prints (`--version`'s one line, a tool mode's
// verdict); the engine's own lines go to stderr, where a service manager keeps them.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>

#include "core/app_commands.hpp"
#include "core/receiver_snapshot.hpp"
#include "core/version.hpp"
#include "engine/engine.hpp"
#include "engine/process_boot.hpp"
#include "source/soundcard_source.hpp"

namespace {

namespace cmd = cascade::core::cmd;
using cascade::engine::Engine;

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;

// How long the receiver is given to come up before the run is called failed: the
// first pass has published well inside this on every machine measured; a slower
// one is told, not waited on for ever.
constexpr std::chrono::milliseconds kStartWait{5000};
// The sleep between looks at the clock and the stop request. The engine runs on
// its own thread; this thread only decides when to end it.
constexpr std::chrono::milliseconds kWaitSlice{20};

// Set by whatever ends the run from outside. Nothing sets it in 7b (7d's
// SIGTERM/SIGINT/SIGHUP path and the console-close handler will); the wait loop
// reads it so that path needs no change here.
std::atomic<bool> g_stopRequested{false};

bool receiverRunning(const Engine& engine) {
    cascade::core::PublishedState ps = cascade::core::initialPublishedState();
    (void)engine.snapshot()->read(ps);
    return ps.app.published && (ps.rx.flags & FOXAPI_RX_RUNNING) != 0u;
}

// The whole run: construct, start, wait, stop. Returns the exit code.
int runEngine(const cascade::engine::BootResult& boot) {
    Engine engine;
    engine.initialise();

    // THE SOURCE IS THE GENERATOR, chosen explicitly (it is the engine's default,
    // and 7c's config restore will overrule it; naming it here keeps the
    // skeleton's behaviour from depending on a default). MUTED: the receiver
    // opens its audio sink on RUN whatever the level, and at level 0 there is
    // nothing to hear.
    (void)engine.applyCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.0));
    const cmd::QueuedCommand generator = cmd::makeText(FOXAPI_OP_SELECT_SOURCE, "siggen");
    const FoxCommandResult selected = engine.applyCommand(generator.c, generator.longText);
    if (selected.status != FOXAPI_OK && selected.status != FOXAPI_NO_CHANGE) {
        std::fprintf(stderr, "foxsdr-engine: the signal generator could not be selected (status %d)\n",
                     static_cast<int>(selected.status));
        engine.shutdown();
        return kExitFailed;
    }
    const FoxCommandResult started = engine.applyCommand(cmd::makeInt(FOXAPI_OP_RUN, 1));
    if (started.status != FOXAPI_OK || (started.flags & FOXAPI_RESULT_REFUSED) != 0u) {
        std::fprintf(stderr, "foxsdr-engine: the receiver could not be started (status %d)\n",
                     static_cast<int>(started.status));
        engine.shutdown();
        return kExitFailed;
    }

    // THE PASS ON ITS OWN THREAD, as the window starts it (app_window.cpp: period
    // 16 ms, floor 2 ms, free-running), and the first pass made here so the first
    // read is a real state, as the table's own create() does it. The control
    // thread's hang watchdog keeps its empty report directory: a run that may not
    // write writes no report (the Diagnostics switch, 7c, moves it).
    cascade::engine::ControlThreadOptions options;
    options.period = std::chrono::milliseconds(16);
    options.floor = std::chrono::milliseconds(2);
    options.stepped = false;
    options.watchdogReportDir.clear();
    engine.controlPass();
    engine.startControlThread(options);

    const auto t0 = std::chrono::steady_clock::now();
    const auto elapsedS = [&t0] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };

    bool up = false;
    while (!up && !g_stopRequested.load() &&
           std::chrono::steady_clock::now() - t0 < kStartWait) {
        up = receiverRunning(engine);
        if (!up) { std::this_thread::sleep_for(kWaitSlice); }
    }
    int code = kExitOk;
    if (!up && !g_stopRequested.load()) {
        std::fprintf(stderr, "foxsdr-engine: the receiver did not report running within %lld ms\n",
                     static_cast<long long>(kStartWait.count()));
        code = kExitFailed;
    } else {
        std::fprintf(stderr, "foxsdr-engine %s: running, signal generator, muted, no config%s\n",
                     cascade::versionString(),
                     boot.seconds >= 0.0 ? "" : " (until ended from outside)");
        std::fflush(stderr);
        while (!g_stopRequested.load() && (boot.seconds < 0.0 || elapsedS() < boot.seconds)) {
            std::this_thread::sleep_for(kWaitSlice);
        }
    }

    // THE STOP, in the window's order (app_window.cpp, "SoundCardSource::
    // setCloseWaitEnabled(false)" then the engine's phases): no sound-card close is
    // waited for from here on, then quiesce (the transmitter first, the servers,
    // the control thread joined), stop (the pipeline), release (the plugins and
    // modules) and drain. There is no config to save between the phases until 7c
    // gives the engine one.
    cascade::source::SoundCardSource::setCloseWaitEnabled(false);
    const std::uint64_t passes = engine.passesEnded();
    engine.shutdown();
    std::fprintf(stderr, "foxsdr-engine: stopped after %.1f s (%llu passes)\n", elapsedS(),
                 static_cast<unsigned long long>(passes));
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
