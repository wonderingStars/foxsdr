// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// WHAT THE WINDOW APPLICATION IS, AS A HOST OF THE ENGINE'S SERVICE (shell retirement R8, 2026-10-28; docs/engine-stage4.md 6.25).
//
// The window application and foxsdr-engine are two hosts of one class, EngineService, which restores the settings, starts the engine, keeps the
// settings saved while it runs and stops it in order. Until R8 the window's run() started and stopped the engine itself and the service did it only
// for the headless binary; since R8 main() starts the service before the window runs and stops it after, so every statement of the engine's life
// is written once. What is left to say is HOW the window application wants that life lived, and that is this function: the options the service is
// built with for a window application's run. src/main.cpp calls it; so does the test host (tests/test_host.hpp), so that a test window is hosted by
// exactly the options the product's is and not by a parallel arrangement of its own.
//
// THE RUN KINDS it distinguishes, from `frames` (main's --frames; negative is a real session) and the environment of a bounded run:
//
//   a real session        the receiver starts at once (a radio that opens with dead panels reads as broken), the usage heartbeat is armed,
//                         the pass is free-running, the lock is best effort, the settings are kept by a thread of the
//                         service's own and the stop sets the sentinel's breadcrumbs.
//   a bounded run         stays hermetic: the receiver is NOT started (CI keeps its timing and spawns no DSP threads it does not need) unless
//     (--frames N)        CASCADE_DECODE_TEST asks - the decoder runner cannot be verified any other way, 28 test files use it; no heartbeat; the
//                         pass is STEPPED by the window's frames (FOXSDR_PASS=free asks for the free-running pass, and so does a measurement
//                         run, which times the product's own arrangement); CASCADE_DIAG_SHUTDOWN_STALL_MS and CASCADE_DIAG_UNLOAD_STALL_MS wedge
//                         the stop in the two places the real wedges live. Every one of these variables is read in a bounded run ONLY, so a
//                         stray variable cannot change an interactive session.
//
// Engine-clean: no gui/, imgui, GLFW or OpenGL.
#pragma once

#include <string>

#include "engine/engine_service.hpp"

namespace cascade::engine {

struct WindowServiceRun {
    // The settings file this run owns (BootResult::configPath); empty is hermetic.
    std::string configPath;
    // Where hang reports may go (diagCrashDir() when boot let this run write at all, else empty).
    std::string diagnosticsDir;
    // main's --frames: negative runs until the window is closed.
    int frames = -1;
    // A bounded measurement run (FOXSDR_MEASURE, tools/measure_engine.ps1): it times the product's own arrangement, so its pass is free-running.
    bool measuring = false;
};

// The options a window application's service is built with; see the header comment for what each run kind sets.
ServiceOptions windowServiceOptions(const WindowServiceRun& run);

}  // namespace cascade::engine
