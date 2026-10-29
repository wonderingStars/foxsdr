// bounded_hooks.hpp - THE TWO CALLS A BOUNDED RUN'S HOST HANDS ITS WINDOW (shell retirement R9, 2026-10-28; docs/engine-stage4.md 6.25).
//
// WHY THEY EXIST. A bounded run (`--frames N`) is a test or a tool, and two things about it cannot be a call of the engine API's table, because the
// table's calls return at once and these do not, or because they need an object and not a word:
//
//   stepPass   makes the run DETERMINISTIC. The engine's pass is stepped in a bounded run (engine/window_service_options.hpp): its thread runs one
//              PART of a pass (core/pass_part.hpp) for each call and the window waits for it, at the three places in its frame where the pump ran
//              them before the pass moved to a thread of its own. A table op returns at once; a step blocks until the part is done.
//   onFrame    is the end of the frame, once (it is not bracketed by the window's pause - it blocks only to install a source, and says so, BoundedClient::blocked): the measurement run (FOXSDR_MEASURE, tools/measure_engine.ps1) takes its one step there, and the
//              bounded run's catalogue diagnostic (CASCADE_PLUGIN_TEST) learns which frame the window has reached. The measurement installs a
//              custom I/Q source and reads the pipeline and the pass's figures - an object, not a word.
//
// WHO OWNS THEM. The HOST does. main() - or the test host - owns the engine, so it owns the stepping and the measurement (engine/host_measure.hpp:
// makeBoundedHooks), and the window is handed two std::functions. They name no engine type: a PassPart is a word, and the three things only the
// WINDOW can do for a measurement (press START, pick a mode, tune) are handed the other way, in BoundedClient. This is host glue, not a door: the
// guard holds that the hooks are installed in a bounded run ONLY (never in a product run, whose pass is free-running and needs no stepping) and
// that the window calls them from the two places it does and nowhere else (tests/test_engine_shell_guard.cpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <functional>

#include "core/pass_part.hpp"

namespace cascade::core {

// What only the interface can do for a measurement, as the interface does it (the same commands the START key, the mode buttons and a click on the
// spectrum submit, so a tune-to-audio latency measured through them includes the command path's own wait for the next frame).
struct BoundedClient {
    std::function<void()> startReceiver;               // the START key's own path
    std::function<void(const char* mode)> setMode;     // "WFM", "USB", ... as the buttons
    std::function<void(double offsetHz)> tuneVfoHz;    // click-to-tune, offset from the centre the receiver last published
    // The window's frame is BLOCKED, not hung, for as long as the host waits for the engine's pass on its behalf (the measurement's source install waits
    // one whole pass): blocked(true) just before the wait, blocked(false) just after. The window holds its hang watchdog's pause between the two. The
    // host brackets ONLY the wait - the hook is called at the end of every frame of a bounded run, and a pause on each would excuse every frame's gap.
    std::function<void(bool on)> blocked;
};

struct BoundedHooks {
    // Run `part` of the engine's pass and wait for it. Empty in a product run (the pass free-runs); the window then does nothing.
    std::function<void(PassPart part)> stepPass;
    // The end of a frame: `nextFrame` is the index of the frame about to begin (the number of frames drawn so far), `client` what the window does for
    // a measurement. False: the measurement has finished and written its result - the window closes. Empty in a product run.
    std::function<bool(int nextFrame, const BoundedClient& client)> onFrame;
};

}  // namespace cascade::core
