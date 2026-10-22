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
#include <string>

#include "core/diag_log.hpp"
#include "engine/process_boot.hpp"
#include "gui/app_window.hpp"

int main(int argc, char** argv) {
    // EVERYTHING before the window: the console, the helper modes, the crash
    // handlers, the instance claim, the argument parse, the tool modes, the
    // stderr capture, the console handler and the sentinel - in that order, in
    // bootProcess. A helper, a tool mode, a refused argument or a link handed to
    // the instance already running ends the process here.
    const cascade::engine::BootResult boot =
        cascade::engine::bootProcess(cascade::engine::ProcessKind::Host, argc, argv);
    if (boot.exit) { return boot.exitCode; }

    cascade::gui::AppWindow app(boot.configPath, boot.announceConfig);
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
    return app.run(boot.frames);
}
