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
//   - skip(): the operator's "next", from any active state. The machine
//     advances to the next lattice point at once and the next tick emits it,
//     ignoring squelch on that tick as the first tune does (the flag still
//     describes the frequency just left). A broadcast station never goes
//     quiet, so without this a scan across the FM band stopped at the first
//     one and stayed there for good.
//   - listenMs (0 = no limit): the same "next" taken by the machine itself,
//     so an unattended scan keeps moving. The budget starts on the tick that
//     found the signal, runs through Paused AND Holding — a re-trigger does
//     not refill it, or a chatty repeater with short gaps would hold the scan
//     as surely as a carrier — and is checked while Paused: the tick it runs
//     out on emits the retune. Running out during a hold changes nothing;
//     the hold ends the ordinary way.
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
// List mode (the airband request, 2026-10)
// ----------------------------------------
//   A non-empty Params::list replaces the lattice with those frequencies, in
//   the order given: the point is "the ticked rows of the frequency list" (or
//   the AIRBAND monitor's blocks), which are anywhere, not on a step. Index k
//   then walks the list and wraps from its last entry to its first; every
//   timing rule above is unchanged. configure() drops entries that are not
//   finite or are negative; a list that ends up empty falls back to the
//   range, so the machine always has somewhere to tune. start/stop/step are
//   ignored while a list is in use.
//
// Screen lane C additions (2026-11-11; the engine's tickScanner calls tick(now, squelch) as before and none of this changes what it sees until
// it sets the new Params fields)
// -----------------------------------------------------------------------------------------------------------------------------------------
//   Lockouts (C3): lockOut(hz, forMs) passes a frequency over (forMs > 0: until that long from the last tick; 0 or less: for good). advance() and
//   the first tune step past every locked point (within kLockoutEpsHz), so no retune is ever emitted TO a locked frequency; with every point locked
//   the scan stays where it is and emits nothing. Params::lockedHz is the same set, stored with the configuration. clearLockouts() lifts the
//   operator's (not Params::lockedHz, which is the configuration's).
//   Priority channels (C2): with Params::priorityHz and priorityIntervalMs > 0, after each priorityIntervalMs of SCANNING time (not of listening)
//   the next priority frequency is tuned for dwellMs; a signal there holds the scan on it (Paused, Holding) like any other; none, and the scan
//   returns to the frequency it left. After a hold on a priority channel the scan also goes back to the frequency it left (that point did not
//   finish its dwell). currentHz() names the priority frequency while the tuner is on it.
//   Per-entry thresholds (C1): tick(nowMs, squelchOpen, levelDb) with a level, on a list entry that has Params::thresholdDb, counts the signal as
//   present when levelDb >= that entry's threshold and ignores the squelch flag for that entry; no level (NaN) or no threshold: the flag decides.
//   Mute while scanning (C5): muted() is true while the scan steps (Scanning) when Params::muteWhileScanning is set, false while it listens.
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
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "core/scanner_params.hpp"

namespace cascade::core {

class Scanner {
public:
    using Params = ScannerParams;  // (core/scanner_params.hpp: the window holds drafts of it)

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
        p.listenMs = sanitizeTimeMs(p.listenMs);
        std::vector<double> kept;
        std::vector<double> keptThr;
        const bool thrAligned = p.thresholdDb.size() == p.list.size();
        kept.reserve(p.list.size());
        for (std::size_t i = 0; i < p.list.size(); ++i) {
            const double hz = p.list[i];
            if (std::isfinite(hz) && hz >= 0.0) {
                kept.push_back(hz);
                if (thrAligned) { keptThr.push_back(p.thresholdDb[i]); }
            }
        }
        p.list = std::move(kept);
        p.thresholdDb = std::move(keptThr);  // empty when it did not line up with the list: then no entry has one
        const auto badHz = [](double hz) { return !(std::isfinite(hz) && hz >= 0.0); };
        p.lockedHz.erase(std::remove_if(p.lockedHz.begin(), p.lockedHz.end(), badHz), p.lockedHz.end());
        p.priorityHz.erase(std::remove_if(p.priorityHz.begin(), p.priorityHz.end(), badHz), p.priorityHz.end());
        p.priorityIntervalMs = sanitizeTimeMs(p.priorityIntervalMs);
        params_ = std::move(p);
        stepIndex_ = 0;
        phaseMs_ = 0.0;
        onPriority_ = false;
        priorityTimerMs_ = 0.0;
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
        onPriority_ = false;
        priorityTimerMs_ = 0.0;
    }

    void stop() {
        state_ = State::Idle;
        firstTunePending_ = false;
        onPriority_ = false;
    }

    // A frequency within this many hertz of a locked one is locked (the 8.33 kHz channel frequencies are not whole hertz).
    static constexpr double kLockoutEpsHz = 5.0;

    // C3. forMs > 0: a temporary lockout that ends forMs after the last tick (the caller's clock); 0 or less: for good (until clearLockouts()).
    void lockOut(double hz, double forMs = 0.0) {
        if (!(std::isfinite(hz) && hz >= 0.0)) { return; }
        if (forMs > 0.0) {
            temp_.push_back({hz, lastMs_ + forMs});
        } else if (std::none_of(perm_.begin(), perm_.end(), [hz](double x) { return std::fabs(x - hz) <= kLockoutEpsHz; })) {
            perm_.push_back(hz);
        }
    }
    // Locks out the frequency the scan is on and moves on (the next tick retunes). Nothing while Idle.
    void lockOutCurrent(double forMs = 0.0) {
        if (state_ == State::Idle) { return; }
        lockOut(currentHz(), forMs);
        skip();
    }
    void clearLockouts() { temp_.clear(); perm_.clear(); }
    bool isLocked(double hz) const {
        const auto close = [hz](double x) { return std::fabs(x - hz) <= kLockoutEpsHz; };
        if (std::any_of(params_.lockedHz.begin(), params_.lockedHz.end(), close) || std::any_of(perm_.begin(), perm_.end(), close)) { return true; }
        return std::any_of(temp_.begin(), temp_.end(), [&](const TempLock& t) { return close(t.hz) && t.untilMs > lastMs_; });
    }
    // The operator's lockouts (not Params::lockedHz): permanent ones, then the temporary ones still running.
    std::vector<double> lockouts() const {
        std::vector<double> out = perm_;
        for (const TempLock& t : temp_) { if (t.untilMs > lastMs_) { out.push_back(t.hz); } }
        return out;
    }

    // C5.
    bool muted() const { return params_.muteWhileScanning && state_ == State::Scanning; }

    // Moves on from the current frequency now: the next tick retunes to the
    // next lattice point (wrapping stop -> start) whatever the squelch says
    // on that tick. Nothing while Idle.
    void skip() {
        if (state_ == State::Idle) { return; }
        advance();
        onPriority_ = false;
        state_ = State::Scanning;
        firstTunePending_ = true;
        phaseMs_ = 0.0;
        advanceAfterMs_ = params_.dwellMs;
    }

    bool active() const { return state_ != State::Idle; }

    // One scheduler step. Returns a frequency exactly when a retune is due
    // (the first tune after start/reconfigure, or a lattice advance);
    // std::nullopt otherwise. Timing rules are in the header comment.
    std::optional<double> tick(double nowMs, bool squelchOpen) {
        return tick(nowMs, squelchOpen, std::numeric_limits<double>::quiet_NaN());
    }

    // The same with the signal level (dB, the unit of Params::thresholdDb) of the frequency the radio is on; NaN = not known.
    std::optional<double> tick(double nowMs, bool squelchFlag, double levelDb) {
        if (state_ == State::Idle) { return std::nullopt; }
        bool squelchOpen = squelchFlag;
        if (!onPriority_) {
            const double thr = thresholdAt(stepIndex_);
            if (!std::isnan(thr) && !std::isnan(levelDb)) { squelchOpen = levelDb >= thr; }
        }

        // Backward jumps contribute zero; time then accumulates from the
        // new nowMs (lastMs_ is updated unconditionally for that reason).
        const double delta = (nowMs > lastMs_) ? (nowMs - lastMs_) : 0.0;
        lastMs_ = nowMs;
        temp_.erase(std::remove_if(temp_.begin(), temp_.end(), [nowMs](const TempLock& t) { return t.untilMs <= nowMs; }), temp_.end());

        switch (state_) {
        case State::Scanning:
            if (firstTunePending_) {
                // Emitted before the squelch check: on this tick the flag
                // still describes the pre-scan tuning (see header comment).
                firstTunePending_ = false;
                phaseMs_ = 0.0;
                advanceAfterMs_ = params_.dwellMs;
                priorityTimerMs_ = 0.0;
                if (!onPriority_ && isLocked(currentHz()) && !advance()) { return std::nullopt; }  // every point locked: nowhere to tune
                onPriority_ = false;
                return currentHz();
            }
            if (onPriority_) {
                // On a priority channel: the squelch is honoured once the radio has settled (dwellMs, or the resume wait after a hold).
                phaseMs_ += delta;
                if (phaseMs_ < advanceAfterMs_) { return std::nullopt; }
                if (squelchOpen) {
                    state_ = State::Paused;
                    listenedMs_ = 0.0;
                    return std::nullopt;
                }
                onPriority_ = false;  // nothing there: back to the frequency the scan left
                priorityTimerMs_ = 0.0;
                phaseMs_ = 0.0;
                advanceAfterMs_ = params_.dwellMs;
                return currentHz();
            }
            if (squelchOpen) {
                // Signal found: listen, don't step past it — even if the
                // dwell also expired this very tick. The listen budget
                // starts here.
                state_ = State::Paused;
                listenedMs_ = 0.0;
                return std::nullopt;
            }
            phaseMs_ += delta;
            priorityTimerMs_ += delta;
            if (params_.priorityIntervalMs > 0.0 && priorityTimerMs_ >= params_.priorityIntervalMs) {
                if (const std::optional<double> p = nextPriority()) {
                    // A due priority visit outranks a due step.
                    onPriority_ = true;
                    priorityCur_ = *p;
                    priorityTimerMs_ = 0.0;
                    phaseMs_ = 0.0;
                    advanceAfterMs_ = params_.dwellMs;
                    return priorityCur_;
                }
                priorityTimerMs_ = 0.0;  // every priority channel locked out: try again next interval
            }
            if (phaseMs_ >= advanceAfterMs_) {
                // Fresh dwell from the emitting tick: excess is discarded
                // so a UI stall cannot cause a catch-up burst, and the
                // resume-wait threshold reverts to the normal dwell.
                phaseMs_ = 0.0;
                advanceAfterMs_ = params_.dwellMs;
                if (!advance()) { return std::nullopt; }  // every point locked: stay
                return currentHz();
            }
            return std::nullopt;

        case State::Paused:
            listenedMs_ += delta;
            if (params_.listenMs > 0.0 && listenedMs_ >= params_.listenMs) {
                // Listened long enough: the machine's own skip, emitted on
                // this tick. The dwell runs fresh from the retune.
                const bool moved = advance();
                state_ = State::Scanning;
                phaseMs_ = 0.0;
                advanceAfterMs_ = params_.dwellMs;
                if (!moved) { return std::nullopt; }  // every other point is locked out: stay
                return currentHz();
            }
            if (!squelchOpen) {
                // Station went quiet: the hold countdown starts fresh here.
                state_ = State::Holding;
                phaseMs_ = 0.0;
            }
            return std::nullopt;

        case State::Holding:
            listenedMs_ += delta;
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

    // Always startHz + k*stepHz, inside [startHz, stopHz] by construction -
    // or, in list mode, list[k].
    double currentHz() const {
        if (onPriority_) { return priorityCur_; }
        if (!params_.list.empty()) {
            return params_.list[static_cast<std::size_t>(stepIndex_) % params_.list.size()];
        }
        return params_.startHz + static_cast<double>(stepIndex_) * params_.stepHz;
    }

    // Which list entry (or lattice point) the scan is on.
    long long index() const { return stepIndex_; }

private:
    // "not >= 0" catches NaN as well as negatives; both collapse to zero.
    static double sanitizeTimeMs(double t) { return (t >= 0.0) ? t : 0.0; }

    struct TempLock { double hz; double untilMs; };

    double thresholdAt(long long k) const {
        if (params_.list.empty() || k < 0 || static_cast<std::size_t>(k) >= params_.thresholdDb.size()) { return std::numeric_limits<double>::quiet_NaN(); }
        return params_.thresholdDb[static_cast<std::size_t>(k)];
    }

    // The next priority frequency in turn that is not locked out; nullopt when there is none.
    std::optional<double> nextPriority() {
        const std::size_t n = params_.priorityHz.size();
        for (std::size_t i = 0; i < n; ++i) {
            const double hz = params_.priorityHz[(priorityIdx_ + i) % n];
            if (!isLocked(hz)) {
                priorityIdx_ = (priorityIdx_ + i + 1) % n;
                return hz;
            }
        }
        return std::nullopt;
    }

    long long pointCount() const {
        if (!params_.list.empty()) { return static_cast<long long>(params_.list.size()); }
        return static_cast<long long>(std::floor((params_.stopHz - params_.startHz) / params_.stepHz)) + 1;
    }

    double indexHz(long long k) const {
        if (!params_.list.empty()) { return params_.list[static_cast<std::size_t>(k) % params_.list.size()]; }
        return params_.startHz + static_cast<double>(k) * params_.stepHz;
    }

    void advanceOne() {
        const long long next = stepIndex_ + 1;
        if (!params_.list.empty()) {
            stepIndex_ = (next >= static_cast<long long>(params_.list.size())) ? 0 : next;
            return;
        }
        if (params_.startHz + static_cast<double>(next) * params_.stepHz > params_.stopHz) {
            stepIndex_ = 0;  // wrap stop -> start
        } else {
            stepIndex_ = next;
        }
    }

    // Moves to the next point that is not locked out (wrapping). False, and the index unchanged, when every point is locked.
    bool advance() {
        onPriority_ = false;
        const long long was = stepIndex_;
        const long long limit = std::min<long long>(pointCount(), 1LL << 20);
        for (long long i = 0; i < limit; ++i) {
            advanceOne();
            if (!isLocked(indexHz(stepIndex_))) { return true; }
        }
        stepIndex_ = was;
        return false;
    }

    Params params_{};  // member initializers double as sane defaults
    State state_ = State::Idle;
    long long stepIndex_ = 0;      // lattice index k; frequency derives from it
    bool firstTunePending_ = false;
    double lastMs_ = 0.0;          // previous tick's nowMs (delta source)
    double phaseMs_ = 0.0;         // elapsed inside the current phase
    double listenedMs_ = 0.0;      // time on the current signal (Paused + Holding)
    double advanceAfterMs_ = 0.0;  // dwellMs normally, resumeMs after a hold
    std::vector<TempLock> temp_;
    std::vector<double> perm_;
    bool onPriority_ = false;      // the tuner is on a priority channel, not on the lattice point stepIndex_ names
    double priorityCur_ = 0.0;
    double priorityTimerMs_ = 0.0; // scanning time since the last priority visit
    std::size_t priorityIdx_ = 0;
};

}  // namespace cascade::core
