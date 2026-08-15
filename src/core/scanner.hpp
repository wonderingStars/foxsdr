// Frequency scanner: a PURE, deterministic state machine.
//
// The scanner owns no thread, reads no clock, and touches no pipeline. The
// GUI feeds it (nowMs, squelchOpen) once per frame via tick(); the scanner
// answers with a frequency exactly when a retune is due and nothing
// otherwise. Purity is the design goal: every behavior below is exactly
// table-testable — a scripted timeline of ticks has one correct output, with
// no tolerances and no races.
//
// State machine
// -------------
//
//   Idle ──start()──▶ Scanning ──squelch open──▶ Paused
//     ▲                  ▲  ▲                      │ ▲
//     │                  │  └────── after ─────┐   │ │ squelch
//   stop()               │       holdMs quiet  │squelch  reopens
//     │                  │       + resumeMs    │closes   (re-trigger)
//     └── (from any) ────┘                     │   ▼ │
//                                              └─ Holding
//
//   - Scanning: steps one lattice point per dwellMs of squelch-closed time,
//     wrapping stop -> start. A retune is emitted on the tick that advances.
//   - Scanning + squelch open -> Paused: a signal was found; hold the
//     frequency and listen instead of stepping past it. The squelch check
//     outranks a due dwell advance on the same tick for the same reason.
//   - Paused + squelch closes -> Holding: the station went quiet; give it
//     holdMs to come back before moving on (conversations have gaps).
//   - Holding + squelch reopens -> Paused: re-trigger. The hold timer is
//     abandoned; a later quiet period earns a FULL fresh holdMs, because the
//     station just proved it is still live.
//   - Holding for holdMs of unbroken quiet -> back to Scanning, but the next
//     retune waits resumeMs (a grace period so brief squelch flutter at the
//     tail does not instantly yank the tuner) and then advances to the NEXT
//     lattice step — the held frequency already had its chance.
//
// Timing model (documented because callers depend on every detail)
// ----------------------------------------------------------------
//   - Time is whatever monotonic-ish milliseconds the caller supplies.
//     Elapsed time is accumulated per tick as max(0, nowMs - previous nowMs):
//     a BACKWARD time jump counts as ZERO elapsed, and subsequent time
//     accumulates from the new (earlier) nowMs. This makes a wall-clock
//     adjustment lose a little progress instead of firing a burst of retunes
//     or arming a phase for hours.
//   - Each phase (dwell, hold, resume wait) starts fresh at the tick that
//     entered it; excess elapsed time is never carried across a transition
//     or a retune. Consequence: a UI stall longer than dwellMs produces ONE
//     immediate step, not a catch-up burst — at most one retune per tick,
//     which also gives dwellMs = 0 a well-defined meaning (one step per
//     tick). Phase thresholds are met with >=, so a tick landing exactly on
//     the boundary fires.
//   - The machine takes at most one transition per tick, so with holdMs = 0
//     and resumeMs = 0 each phase still consumes one tick — every state
//     remains observable through state().
//   - start(nowMs) arms the scan; the FIRST tune (to the sanitized startHz)
//     is emitted on the first tick at/after start. That tick ignores
//     squelchOpen entirely: the flag describes whatever the radio was tuned
//     to BEFORE the scan began, which is stale. Squelch is honored from the
//     next tick on.
//
// Frequency lattice
// -----------------
//   The current frequency is always startHz + k*stepHz with integer k >= 0,
//   recomputed from k rather than accumulated, so no floating-point drift
//   builds up over hours of wrapping. The advance wraps to k = 0 when the
//   next point would exceed stopHz (exact comparison, no epsilon), so every
//   emitted frequency lies inside [startHz, stopHz] by construction.
//
// configure()
// -----------
//   Sanitizes in this order: stopHz < startHz -> the two are swapped;
//   stepHz not > 0 (zero, negative, or NaN) -> 1 kHz; each time that is not
//   >= 0 (negative or NaN) -> 0. startHz == stopHz is legal: the lattice is
//   a single point and every advance re-emits it.
//   Reconfiguring while ACTIVE resets the scan under the new parameters: the
//   machine returns to Scanning at the new startHz and re-emits a first tune
//   on the next tick (in-flight dwell/hold/resume progress is meaningless
//   against new parameters, and the current frequency may not even lie in
//   the new range). Reconfiguring while Idle just stores the parameters.
//
// stop() drops to Idle from any state; currentHz() keeps reporting the last
// frequency (harmless, and lets the GUI keep displaying where the scan
// stopped). start() always begins a fresh scan from startHz, even when
// called while already active.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <utility>

namespace cascade::core {

class Scanner {
public:
    struct Params {
        double startHz = 88e6;
        double stopHz = 108e6;
        double stepHz = 100e3;
        double dwellMs = 50;
        double holdMs = 2000;
        double resumeMs = 500;
    };

    enum class State { Idle, Scanning, Paused, Holding };

    // Sanitizes and stores parameters (rules in the header comment above).
    // While active: resets the scan — Scanning at the new startHz, first
    // tune re-emitted on the next tick. While Idle: stores only.
    void configure(Params p) {
        if (p.stopHz < p.startHz) { std::swap(p.startHz, p.stopHz); }
        // "not > 0" (rather than "<= 0") deliberately catches NaN too.
        if (!(p.stepHz > 0.0)) { p.stepHz = 1e3; }
        p.dwellMs = sanitizeTimeMs(p.dwellMs);
        p.holdMs = sanitizeTimeMs(p.holdMs);
        p.resumeMs = sanitizeTimeMs(p.resumeMs);
        params_ = p;
        stepIndex_ = 0;
        phaseMs_ = 0.0;
        if (state_ != State::Idle) {
            state_ = State::Scanning;
            firstTunePending_ = true;
            advanceAfterMs_ = params_.dwellMs;
        }
    }

    // Arms a fresh scan from startHz; the first tune is emitted by the first
    // tick at/after nowMs. Restarts from scratch if already active.
    void start(double nowMs) {
        state_ = State::Scanning;
        stepIndex_ = 0;
        firstTunePending_ = true;
        phaseMs_ = 0.0;
        advanceAfterMs_ = params_.dwellMs;
        lastMs_ = nowMs;
    }

    void stop() {
        state_ = State::Idle;
        firstTunePending_ = false;
    }

    bool active() const { return state_ != State::Idle; }

    // One scheduler step. Returns a frequency exactly when a retune is due
    // (the first tune after start/reconfigure, or a lattice advance);
    // std::nullopt otherwise. Timing rules are in the header comment.
    std::optional<double> tick(double nowMs, bool squelchOpen) {
        if (state_ == State::Idle) { return std::nullopt; }

        // Backward jumps contribute zero; time then accumulates from the
        // new nowMs (lastMs_ is updated unconditionally for that reason).
        const double delta = (nowMs > lastMs_) ? (nowMs - lastMs_) : 0.0;
        lastMs_ = nowMs;

        switch (state_) {
        case State::Scanning:
            if (firstTunePending_) {
                // Emitted before the squelch check: on this tick the flag
                // still describes the pre-scan tuning (see header comment).
                firstTunePending_ = false;
                phaseMs_ = 0.0;
                advanceAfterMs_ = params_.dwellMs;
                return currentHz();
            }
            if (squelchOpen) {
                // Signal found: listen, don't step past it — even if the
                // dwell also expired this very tick.
                state_ = State::Paused;
                return std::nullopt;
            }
            phaseMs_ += delta;
            if (phaseMs_ >= advanceAfterMs_) {
                advance();
                // Fresh dwell from the emitting tick: excess is discarded
                // so a UI stall cannot cause a catch-up burst, and the
                // resume-wait threshold reverts to the normal dwell.
                phaseMs_ = 0.0;
                advanceAfterMs_ = params_.dwellMs;
                return currentHz();
            }
            return std::nullopt;

        case State::Paused:
            if (!squelchOpen) {
                // Station went quiet: the hold countdown starts fresh here.
                state_ = State::Holding;
                phaseMs_ = 0.0;
            }
            return std::nullopt;

        case State::Holding:
            if (squelchOpen) {
                // Re-trigger: the station is live again. Abandoning the
                // countdown means the next quiet spell earns a full hold.
                state_ = State::Paused;
                return std::nullopt;
            }
            phaseMs_ += delta;
            if (phaseMs_ >= params_.holdMs) {
                // Quiet long enough: rejoin the scan, but make the next
                // advance wait resumeMs instead of dwellMs (grace period),
                // after which it moves to the NEXT lattice step.
                state_ = State::Scanning;
                phaseMs_ = 0.0;
                advanceAfterMs_ = params_.resumeMs;
            }
            return std::nullopt;

        case State::Idle:
            break;  // unreachable: handled before the switch
        }
        return std::nullopt;
    }

    State state() const { return state_; }

    // Always startHz + k*stepHz, inside [startHz, stopHz] by construction.
    double currentHz() const {
        return params_.startHz + static_cast<double>(stepIndex_) * params_.stepHz;
    }

private:
    // "not >= 0" catches NaN as well as negatives; both collapse to zero.
    static double sanitizeTimeMs(double t) { return (t >= 0.0) ? t : 0.0; }

    void advance() {
        const long long next = stepIndex_ + 1;
        if (params_.startHz + static_cast<double>(next) * params_.stepHz > params_.stopHz) {
            stepIndex_ = 0;  // wrap stop -> start
        } else {
            stepIndex_ = next;
        }
    }

    Params params_{};  // member initializers double as sane defaults
    State state_ = State::Idle;
    long long stepIndex_ = 0;      // lattice index k; frequency derives from it
    bool firstTunePending_ = false;
    double lastMs_ = 0.0;          // previous tick's nowMs (delta source)
    double phaseMs_ = 0.0;         // elapsed inside the current phase
    double advanceAfterMs_ = 0.0;  // dwellMs normally, resumeMs after a hold
};

}  // namespace cascade::core
