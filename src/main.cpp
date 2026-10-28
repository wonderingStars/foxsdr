// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// Thin entry point: boot the process, then hand off to the GUI shell.
// `--frames N` renders exactly N frames then exits 0 - the bounded-run
// contract the app_smoke ctest entry relies on. `--selftest` runs the
// pipeline headless (no window, no GL, no audio device) and checks that the
// demo signal's peak lands on the right FFT bin AND that the audio chain
// demodulates a CW carrier to its 700 Hz sidetone. No flag: run until the
// window is closed.
//
// WHAT USED TO BE HERE (engine extraction stage 7a, a pure move). This file
// was 1,449 lines: the tool modes (selftest and the bench checks), the
// child-process helper modes, the crash handlers, the diagnostics switch, the
// single-instance claim, the beta-link activation, the argument parse, the
// stderr capture, the console handler and the sentinel. They are process boot,
// not window code, and the headless engine executable (stage 7b) needs the same
// sequence, so they live in the engine library now:
//
//     src/engine/process_boot.{hpp,cpp}   bootProcess(): the sequence, in its order
//     src/engine/tool_modes.{hpp,cpp}     the headless checks it can dispatch to
//
// The kind passed below decides five things and nothing else (the instance
// claim, link activation, the stderr capture, the console handler and the signal
// path): a Host is the window application. Nothing about how this program
// behaves changed with the move; tests/test_process_boot_order.cpp holds the
// order of the boot from the text of process_boot.cpp.
//
// WHO BUILDS THE ENGINE, AND WHO STARTS AND STOPS IT (shell retirement R7 and R8, 2026-10-28; docs/engine-stage4.md 6.25). This program is the HOST: it
// builds the engine's service (engine/engine_service.hpp) with the options of a window application (engine/window_service_options.hpp), takes the
// settings file's lock, restores the settings into the engine, constructs the window on the restored engine, STARTS the service, runs the window,
// STOPS the service, and only then lets the window close its own GL context. The window used to build the engine in its constructor (until R7) and
// start and stop it inside run() (until R8); it is a client of an engine whose whole life is the service's now, and what it needs of the restore
// comes to it through WindowHost. The order of those calls is the contract, held from this file's text by tests/test_engine_service_order.cpp:
//
//     service built, lock, restore, window constructed, service started, window run, service stopped, window closed
//
// The start comes after the window's constructor because the window attaches its plugin UI there (a late attach: the engine replays its current
// directive to it, Engine::attachPluginUi) and a control thread that is already running would race that attach; it comes before run() because the
// engine has always been running by the time the first frame is drawn. The stop comes BEFORE the window closes because the window's hang watchdog
// is stopped last, by closeWindow(), so that a teardown that wedges is reported (core/hang_watchdog.hpp).
#include <cstdio>
#include <memory>
#include <string>

#include "core/diag_log.hpp"
#include "core/engine_measure.hpp"
#include "core/sentinel.hpp"
#include "engine/engine_service.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/process_boot.hpp"
#include "engine/window_service_options.hpp"
#include "gui/app_window.hpp"
#include "gui/software_gl.hpp"

int main(int argc, char** argv) {
    // EVERYTHING before the window: the console, the helper modes, the crash
    // handlers, the instance claim, the argument parse, the tool modes, the
    // stderr capture, the console handler and the sentinel - in that order, in
    // bootProcess. A helper, a tool mode, a refused argument or a link handed to
    // the instance already running ends the process here.
    const cascade::engine::BootResult boot =
        cascade::engine::bootProcess(cascade::engine::ProcessKind::Host, argc, argv);
    if (boot.exit) { return boot.exitCode; }

    int exitCode = 0;
    bool softglRelaunch = false;
    {
        // THE ENGINE'S SERVICE, built first and destroyed LAST in this scope: the window is a client of it and goes before it. The options are a
        // window application's (engine/window_service_options.hpp: best-effort lock, the engine of a run with no settings file left as built, the lines
        // the service writes to the user are "cascade: ...", a keeper thread, the sentinel's breadcrumbs, and what a bounded run changes). The
        // diagnostics directory is empty unless this run is ALLOWED to write - which is not the same as whether the user wants it: the Settings toggle
        // works in a session that started with diagnostics off, so the directory is named whenever boot allows it.
        //
        // THE MEASUREMENT RUN (FOXSDR_MEASURE, bounded runs only) is read here, once, because it decides the pass (a measurement times the product's own
        // free-running arrangement) and the window drives it: it is handed over below.
        std::unique_ptr<cascade::core::EngineMeasure> measure;
        if (boot.frames >= 0) { measure = cascade::core::EngineMeasure::fromEnvironment(); }
        cascade::engine::WindowServiceRun serviceRun;
        serviceRun.configPath = boot.configPath;
        serviceRun.diagnosticsDir = boot.mayWrite ? cascade::core::diagCrashDir() : std::string();
        serviceRun.frames = boot.frames;
        serviceRun.measuring = measure != nullptr;
        const cascade::engine::ServiceOptions options = cascade::engine::windowServiceOptions(serviceRun);
        cascade::engine::EngineService service(options);

        // THE SETTINGS FILE'S LOCK (stage 7c, core/config_lock.hpp), for a real session only: the headless engine shares this one file by design
        // and refuses to run beside the window on it, so the window application says it is here by taking the lock the engine looks for. BEST
        // EFFORT, and never a refusal: a second window has always been allowed to start, and the window application behaves exactly as it did
        // when it cannot get the lock (another window holds it, or an engine does - the one case the engine's own refusal cannot cover, recorded
        // in docs/engine-stage4.md 6.17); the service logs the line ("settings: the lock beside the settings file is ...; carrying on") and
        // answers true. A bounded --frames run is a test or a tool, stays as hermetic as it was, and takes none. The lock is the service's, so it is
        // released when the service is destroyed - after the service's last save, before the software-GL relaunch below starts the second process
        // (which takes it for its own run).
        std::string lockSentence;
        if (boot.frames < 0) { (void)service.acquireConfig(lockSentence); }

        // THE RESTORE (Engine::initialise - the plugin scan among it - the settings file's load, the start-up state, the receiver's half, the usage
        // journal's start-up, the bookmarks), BEFORE the window exists: the window attaches to the plugin UI AFTER the scan, so the engine replays
        // its current directive to it (Engine::attachPluginUi). CASCADE_CONFIG_TEST asks for the read-back line, printed here as the window
        // printed it from its constructor.
        const cascade::engine::EngineService::Restored restored = service.restore();
        if (boot.announceConfig && service.persistent()) { cascade::engine::EngineService::announce(restored); }

        cascade::gui::WindowHost host;
        host.api = cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
        host.handle = service.engine().apiHandle();
        host.engine = &service.engine();
        host.configPath = boot.configPath;
        host.configLoaded = restored.loaded;
        host.writesForbidden = restored.writesForbidden;
        host.lastRunUnclean = restored.lastRunUnclean;
        host.sourceKind = restored.sourceKind;
        host.iqFilePath = restored.iqFilePath;
        cascade::gui::AppWindow app(host);
        // Empty unless this run is ALLOWED to write - which is not the same as
        // whether the user wants it. run() gates the writing on the stored switch;
        // handing over the directory regardless is what lets the Settings toggle
        // work in a session that started with diagnostics off. The watchdog still
        // runs and still logs a recovered stall to the ring either way, but it puts
        // no file on a machine that only asked for a smoke test.
        app.setDiagnosticsDir(boot.mayWrite ? cascade::core::diagCrashDir() : std::string());
        app.setDiagStallMs(boot.diagStallMs);
        app.setDiagToggle(boot.diagToggle);
        app.setLinkTesterReveal(boot.linkTesterReveal);
        app.setMeasurement(std::move(measure));

        // THE ENGINE STARTS HERE: after the window object exists (its plugin UI has attached - a control thread already running would race that
        // attach) and before run() creates the GL window (the engine has been running since before the first frame, as it was when the window
        // started it itself). The receiver (a real session), the usage heartbeat, the settings forced to disk once with the unclean-exit marker
        // false, the first pass, the control thread and the keeper thread, in the order engine/engine_service.cpp holds.
        std::string startWhy;
        if (service.start(startWhy)) {
            exitCode = app.run(boot.frames);
        } else {
            std::fprintf(stderr, "cascade: %s\n", startWhy.c_str());
            exitCode = 1;
        }
        // THE ENGINE STOPS HERE, with the window still on screen and its hang watchdog still armed: the window's own part of the shutdown (the
        // crash-report sweep, the feature and problem senders, the last half of the settings handed over and acknowledged, the plugin UI released
        // and disconnected) was the end of run(); this is everything else, in the order the service holds - the engine's first phase, the final
        // save, the pipeline's stop, the plugins' release, the clean-exit marker, the last save waited for within its bound, the drain. It is also
        // reached when run() returned early (the window could not be created, or is to be handed to the software renderer): the receiver this
        // process started is stopped, and the marker says the run ended cleanly, before the second process starts.
        service.stop();
        // THE WINDOW'S GL CONTEXT GOES LAST of the window's parts, and its hang watchdog with it: a teardown that wedges anywhere above is judged
        // against the shutdown budget until this line (core/hang_watchdog.hpp). A no-op when run() never made a window.
        app.closeWindow();
        softglRelaunch = app.relaunchWithSoftwareGl();
    }
    // THE GRAPHICS DRIVER COULD NOT BE USED, AND THE SOFTWARE RENDERER IS TO BE (0.99.90,
    // gui/software_gl.hpp): a second process, started only now that this one's application object -
    // its plugins, its sound device, its listening ports - is gone, so the two never hold anything
    // at once. This one waits and ends with its exit code, which is what a script or a test that
    // started FoxSDR sees. HOST ONLY BY CONSTRUCTION: this is the window application's main(); the
    // headless engine (engine_main.cpp) has no window to fail and never reaches this. The marker
    // the second process reads is taken in bootProcess's instance claim, at the position master
    // took it (after the crash handlers are armed, before the flags are parsed).
    if (softglRelaunch) {
        // This process's watcher goes first: from here it only waits, and an ending it filed would
        // be a second report of the second process's run, which that process's own watcher files.
        cascade::core::sentinelSetEnabled(false);
        // THE SETTINGS LOCK went with the window (7c; the service held it, and was destroyed with the scope above): the second process is the
        // window application from here on and takes the lock for its own run; held across the wait, it would find it "held by another FoxSDR
        // program" and carry on without it.
        const cascade::gui::softgl::RelaunchResult r = cascade::gui::softgl::relaunchAndWait();
        if (!r.started) {
            cascade::gui::softgl::tellUserRelaunchFailed(r.error, boot.frames);
            return 1;
        }
        cascade::core::diagLogf("window: the second FoxSDR process ended with exit code %lu", r.exitCode);
        return static_cast<int>(r.exitCode);
    }
    return exitCode;
}
