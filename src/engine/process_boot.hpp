// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// PROCESS BOOT: everything a FoxSDR executable does between the operating
// system handing it control and the moment it has an application object to run
// (engine extraction stage 7a: moved out of src/main.cpp, unchanged).
//
// WHY IT IS ITS OWN UNIT. src/main.cpp was 1,449 lines, of which about 150 were
// the entry point proper. The rest was boot: the console, the child-process
// helper modes, the crash handlers, the diagnostics switch read ahead of the
// window, the single-instance claim, the beta-link activation, the argument
// parse, the tool modes, the stderr capture, the console-close handler and the
// sentinel. They are process-level, not window-level, and a second executable
// (the headless engine, stage 7b) needs the same sequence. bootProcess() is that
// sequence, in the order main() had it. The order is the contract: a helper
// must return before any handler is armed, the handlers must be armed before the
// config is read (the read can fault), the instance must be claimed before the
// flags are parsed, and the console must be settled before anything prints.
// tests/test_process_boot_order.cpp pins it from the source text, and
// process_boot.cpp says at each step why that step is where it is.
//
// THE KIND. A process is a Host (the window application, `cascade`) or an
// Engine (the window-less service, `foxsdr-engine`, stage 7b). The kind decides
// these things and nothing else:
//   * whether the process claims the single-instance name (Host only);
//   * whether it acts on a foxsdr:// link in argv[1] (Host only; an Engine
//     neither writes a link-request file nor claims the instance - the link
//     channel belongs to the interface that is on the desktop);
//   * whether it installs the stderr capture (Host only: a service's stderr is
//     its journal; the capture exists for "a window nobody was reading");
//   * whether it installs the console-close handler (Host only, step 11; the
//     Engine's is the signal path, next);
//   * the signal path itself (Engine only, step 14, stage 7d: SIGTERM, SIGINT and
//     SIGHUP on Linux, the same console handler on Windows - core/stop_signal.hpp);
// and, added by 7b with the Engine's main, three more - the ones the Engine's
// hermetic rule is made of (an Engine stores nothing until stage 7c wires its
// config, and a service is not a user session):
//   * whether this run may put diagnostics on the disk (Host: a run with no
//     arguments, as ever; Engine: only when FOXSDR_DIAG_DIR names a directory,
//     because a bare `foxsdr-engine` is not a "real session" in the sense that
//     rule meant, and nothing else may write a log or a crash directory);
//   * which config the run reads ahead of the parse and hands on (Host: as ever;
//     Engine: none unless it was ASKED - `--config PATH` or `--use-config`, stage
//     7c; never the user's file by default, never CASCADE_CONFIG_TEST's);
//   * which flags the parse accepts (Engine: `--seconds N`, `--config PATH` and
//     `--use-config`, and none of the window's - --frames, --diag-stall,
//     --diag-toggle, --link-tester are the Host's, refused by an Engine as
//     unknown arguments; the Host refuses --seconds, --config and --use-config,
//     exactly as it refused any flag it did not have before there was an Engine).
// (The settings LOCK is not a boot step: the Engine takes it in its service, before
// it reads the file, and the Host takes it in main(), best effort - src/main.cpp.)
// Every other step is kind-blind. A Host behaves exactly as it did before 7a.
//
// Engine-clean: no gui/, imgui, GLFW or OpenGL. main() constructs the window; it
// never happens here.
#pragma once

#include <string>

#include "engine/tool_modes.hpp"

namespace cascade::engine {

enum class ProcessKind {
    Host,    // the window application (cascade / cascade.exe)
    Engine,  // the window-less engine service (foxsdr-engine, stage 7b)
};

// What boot hands back. Either the process is finished (`exit`: return
// `exitCode` from main(), nothing else to do - a helper mode, a tool mode, a
// refused argument, a link handed to the instance already running), or it goes
// on and the fields below are what the application object is built from.
struct BootResult {
    bool exit = false;
    int exitCode = 0;

    // --- when the process goes on ---------------------------------------------
    std::string configPath;        // empty: no stored config (a hermetic bounded run)
    bool announceConfig = false;   // CASCADE_CONFIG_TEST named the path
    int frames = -1;               // negative: run until the window is closed (Host)
    double seconds = -1.0;         // negative: run until stopped; --seconds N (Engine only)
    std::string engineConfigArg;   // --config PATH (Engine only): the settings file this run owns
    bool engineUseConfig = false;  // --use-config (Engine only): the per-user settings file, the one the window uses
    int diagStallMs = 0;           // --diag-stall
    int diagToggle = 0;            // --diag-toggle on (1) / off (-1)
    bool linkTesterReveal = false; // --link-tester
    bool mayWrite = false;         // this run may put diagnostics on disk (a real session)

    // --- boot's own bookkeeping, public so a test can read what boot decided ---
    bool wanted = false;           // the stored Diagnostics switch, honoured before the disk
    bool primaryInstance = false;  // this process holds the instance name (Host only)
    bool argv1IsLinkActivation = false;  // argv[1] was a foxsdr: link, consumed by boot
    bool sentinelTest = false;     // CASCADE_SENTINEL_TEST on a bounded run
    ToolRequest tool;              // the tool mode the command line asked for, if any
};

// Run the whole boot sequence for this kind of process. Prints and exits for the
// modes that print and exit; otherwise returns with everything armed.
BootResult bootProcess(ProcessKind kind, int argc, char** argv);

}  // namespace cascade::engine
