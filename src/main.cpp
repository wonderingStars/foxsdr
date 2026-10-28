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
// WHO BUILDS THE ENGINE (shell retirement R7, 2026-10-28; docs/engine-stage4.md 6.25). This program is the HOST: it builds the engine's service
// (engine/engine_service.hpp), takes the settings file's lock, restores the settings into the engine, and only then constructs the window,
// handing it the engine's API table, its handle and the engine itself. The window used to build the engine inside its own constructor and do
// all of this there; it is a client of an engine that already exists now, and what it needs of the restore comes to it through WindowHost.
// The engine is still STARTED and STOPPED by the window (run(): until R8 moves them to the service).
#include <string>

#include "core/diag_log.hpp"
#include "core/sentinel.hpp"
#include "engine/engine_service.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/process_boot.hpp"
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
        // THE ENGINE'S SERVICE, built first and destroyed LAST in this scope: the window is a client of it and goes before it. Window application
        // options (docs/engine-stage4.md 6.25): the lock is BEST EFFORT, the engine of a run with no settings file is left exactly as it was
        // built, and the lines the service writes to the user are "cascade: ..." as the window's have always been. Empty diagnosticsDir unless
        // this run is ALLOWED to write - which is not the same as whether the user wants it: the Settings toggle works in a session that started
        // with diagnostics off, so the directory is named whenever boot allows it. The service is NOT started here and not stopped here in R7:
        // the window starts the engine in run() and takes it down at the end, as it always has (R8 moves both).
        cascade::engine::ServiceOptions options;
        options.configPath = boot.configPath;
        options.diagnosticsDir = boot.mayWrite ? cascade::core::diagCrashDir() : std::string();
        options.lock = cascade::engine::ServiceLock::BestEffort;
        options.hermetic = cascade::engine::ServiceHermetic::Window;
        options.label = "cascade";
        options.frontEndFollows = true;   // the window attaches its plugin UI after the restore: the scan is the window's engine's scan (Engine::expectFrontEnd)
        cascade::engine::EngineService service(options);

        // THE SETTINGS FILE'S LOCK (stage 7c, core/config_lock.hpp), for a real session only: the headless engine shares this one file by design
        // and refuses to run beside the window on it, so the window application says it is here by taking the lock the engine looks for. BEST
        // EFFORT, and never a refusal: a second window has always been allowed to start, and the window application behaves exactly as it did
        // when it cannot get the lock (another window holds it, or an engine does - the one case the engine's own refusal cannot cover, recorded
        // in docs/engine-stage4.md 6.17); the service logs the line ("settings: the lock beside the settings file is ...; carrying on") and
        // answers true. A bounded --frames run is a test or a tool, stays as hermetic as it was, and takes none. The lock is the service's, so it is
        // released when the service is destroyed - after the window's last save, before the software-GL relaunch below starts the second process
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
        exitCode = app.run(boot.frames);
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
