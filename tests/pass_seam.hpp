// pass_seam.hpp - THE SEAM THAT RUNS THE WINDOW TESTS AGAINST THE ENGINE'S PASS ON ITS OWN THREAD (the pump move's step P5).
//
// A hermetic window test has always been its own pump: it constructs an AppWindow, never starts the engine's control thread, and drives
// one frame's engine part by hand - pumpFrameBegin, pumpInputs, pumpPlugins, pumpPublish, then the window's look - from the test's
// thread. That proves the same-thread behaviour, which is all the product had until the pump moved; it proves nothing about the thing
// the move created: a window that LOOKS at what another thread published. This header is the one place those tests say "one pass", so
// that the same test can be run against the pass on its own thread without being rewritten:
//
//   FOXSDR_TEST_PASS unset     INLINE - the test's own phases on the test's own thread (what every test did until the move)
//   FOXSDR_TEST_PASS=stepped   the control thread exists and runs the PART of the pass that the test's inline phases are - the front part
//                              for "one pass", the mute part for the mute, the back part for the workers - and the test waits for it:
//                              the same order as inline, on the real thread, with the real call paths (onControlThread(), the directive
//                              handshake, the table's thread gating). This is the mode the application's bounded runs use
//   FOXSDR_TEST_PASS=free      the control thread free-runs (period 3 ms, floor 1 ms) and "one pass" is Engine::runPassNow(): a
//                              whole pass that STARTED after the call finished - so what the test submitted before is applied and
//                              published when it returns, with passes of the thread's own beside it. A test that reads or writes
//                              an engine field directly races the pass in this mode: that is a finding about the test, listed in
//                              docs/engine-stage4.md with its reason, never papered over
//
// FREE-RUNNING "ONE PASS" IS A WHOLE PASS - including the parts an inline look leaves out (the patch step, the remote controls, the workers,
// the sink) - and that is the point: a window test that passes only when the workers have NOT run since its look has a hidden ordering
// assumption the real frame never made. Where a test needs the workers or the mute at a place of its own it says so (workers(), mute()).
//
// Include AFTER engine/engine.hpp (the test includes the window's header, which does).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>

#include "engine/engine.hpp"

namespace passseam {

enum class Mode { Inline, Stepped, Free };

inline Mode mode() {
    static const Mode m = [] {
        const char* v = std::getenv("FOXSDR_TEST_PASS");
        if (v == nullptr || *v == '\0') { return Mode::Inline; }
        if (std::strcmp(v, "stepped") == 0) { return Mode::Stepped; }
        if (std::strcmp(v, "free") == 0) { return Mode::Free; }
        return Mode::Inline;
    }();
    return m;
}

inline const char* name() {
    switch (mode()) {
        case Mode::Stepped: return "stepped";
        case Mode::Free: return "free";
        case Mode::Inline: break;
    }
    return "inline";
}

inline bool threaded() { return mode() != Mode::Inline; }

// The engine's control thread, started in the mode's way if the mode wants one and it is not running. Idempotent. The thread is
// stopped by the window's own teardown (AppWindow::~AppWindow -> shutdownQuiesce), like the product's.
inline void attach(cascade::engine::Engine& e) {
    if (!threaded() || e.controlThreadRunning()) { return; }
    cascade::engine::ControlThreadOptions o;
    o.stepped = mode() == Mode::Stepped;
    o.period = std::chrono::milliseconds(3);
    o.floor = std::chrono::milliseconds(1);
    o.watchdogThresholdMs = 20000;
    e.startControlThread(o);
}

// ONE PASS. Inline: `inlinePhases`, as the test always wrote them (window-side calls interleaved among them included). STEPPED: the FRONT
// part of the pass on the control thread (the part the inline phases are - up to and including the publish), so the order of effects is the
// inline one on the real thread; then `afterThreaded`, the window-side work the inline phases interleave with the engine's (a frame's
// once-a-frame commit, say), which submits commands the NEXT pass applies, as it does inline. FREE: a whole pass that started after the call
// (Engine::runPassNow) with `afterThreaded` after it.
inline void pass(cascade::engine::Engine& e, const std::function<void()>& inlinePhases,
                 const std::function<void()>& afterThreaded = {}) {
    if (!threaded()) {
        inlinePhases();
        return;
    }
    attach(e);
    if (mode() == Mode::Stepped) {
        e.stepPass(cascade::engine::PassPart::Front);
    } else {
        (void)e.runPassNow();
    }
    if (afterThreaded) { afterThreaded(); }
}

// The workers and the sink (the engine half of what a frame does after it has drawn): inline, `inlinePhases`; stepped, the BACK part; free,
// a whole pass.
inline void workers(cascade::engine::Engine& e, const std::function<void()>& inlinePhases) {
    if (!threaded()) {
        inlinePhases();
        return;
    }
    attach(e);
    if (mode() == Mode::Stepped) {
        e.stepPass(cascade::engine::PassPart::Back);
    } else {
        (void)e.runPassNow();
    }
}

// The audio mute, which a frame decides after its look and before it draws: inline, `inlinePhases`; stepped, the MUTE part; free, a whole pass.
inline void mute(cascade::engine::Engine& e, const std::function<void()>& inlinePhases) {
    if (!threaded()) {
        inlinePhases();
        return;
    }
    attach(e);
    if (mode() == Mode::Stepped) {
        e.stepPass(cascade::engine::PassPart::Mute);
    } else {
        (void)e.runPassNow();
    }
}

// A CUSTOM SUBSET of the pass's phases (a test that drives exactly the phases it names, in its own order - a drain without a publish, a
// scanner tick on request): inline, the phases; stepped, the same phases run on the test's own thread while the control thread is PARKED
// between parts (ordered by the stepping handshake: what the same-thread test always proved, with the thread present); free, a whole
// pass, the subset being something no free-running pass can be asked to do.
inline void custom(cascade::engine::Engine& e, const std::function<void()>& phases) {
    if (!threaded()) {
        phases();
        return;
    }
    attach(e);
    if (mode() == Mode::Stepped) {
        phases();
    } else {
        (void)e.runPassNow();
    }
}

// A WHOLE pump (Engine::pump()): inline, the phases; stepped and free, a whole pass.
inline void whole(cascade::engine::Engine& e, const std::function<void()>& inlinePump) {
    if (!threaded()) {
        inlinePump();
        return;
    }
    attach(e);
    (void)e.runPassNow();
}

// A READ of an engine field the pass owns, made from the test's thread: inline and stepped it is made right here (the pass is parked
// between parts); free-running it is made between passes by the pass's own thread. Returns whatever `read` returns.
template <class R>
R read(cascade::engine::Engine& e, const std::function<R()>& readIt) {
    if (!threaded()) { return readIt(); }
    attach(e);
    R out{};
    e.runOnPass([&] { out = readIt(); });
    return out;
}

// A WRITE or a call into the engine the pass owns, same rules as read().
inline void on(cascade::engine::Engine& e, const std::function<void()>& work) {
    if (!threaded()) {
        work();
        return;
    }
    attach(e);
    e.runOnPass(work);
}

}  // namespace passseam
