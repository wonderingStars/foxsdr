// Tests for core/scanner.hpp — scripted (nowMs, squelchOpen) timelines with
// EXACT expected (retune?, frequency, state, currentHz) at every tick.
//
// No tolerances anywhere: all frequencies are integer-hertz values exactly
// representable in double (sums of exact doubles below 2^53), and all times
// are integers, so == is the correct comparison. No clock, no randomness.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/scanner.hpp"

#include "test_check.hpp"

#include <cstdio>
#include <optional>

using cascade::core::Scanner;
using St = Scanner::State;

namespace {

// One scripted timeline step: ticks the scanner and compares every
// observable against exact expectations. Returns false (after printing the
// full got/want picture) on any mismatch so each CHECK(stepIs(...)) call
// site reports its own line number.
bool stepIs(Scanner& sc, double t, bool sq, bool wantRetune, double wantHz,
            St wantState, double wantCur) {
    const std::optional<double> r = sc.tick(t, sq);
    const bool ok = (r.has_value() == wantRetune)
                    && (!wantRetune || (r.has_value() && *r == wantHz))
                    && (sc.state() == wantState) && (sc.currentHz() == wantCur);
    if (!ok) {
        std::printf("  tick(t=%.10g, sq=%d): got retune=%d hz=%.10g state=%d "
                    "cur=%.10g; want retune=%d hz=%.10g state=%d cur=%.10g\n",
                    t, sq ? 1 : 0, r.has_value() ? 1 : 0,
                    r.has_value() ? *r : 0.0, static_cast<int>(sc.state()),
                    sc.currentHz(), wantRetune ? 1 : 0, wantHz,
                    static_cast<int>(wantState), wantCur);
    }
    return ok;
}

// The three-point lattice used by most timelines: 100.0 / 100.2 / 100.4 MHz
// (100e6 + k*200e3; 100.6e6 > stop, so the wrap happens after index 2).
Scanner::Params latticeParams() {
    Scanner::Params p;
    p.startHz = 100e6;
    p.stopHz = 100.4e6;
    p.stepHz = 200e3;
    p.dwellMs = 50;
    p.holdMs = 200;
    p.resumeMs = 100;
    return p;
}

}  // namespace

int main() {
    // --- Idle: fresh scanner does nothing, defaults visible ------------------
    {
        Scanner sc;
        CHECK(!sc.active());
        CHECK(sc.state() == St::Idle);
        CHECK(sc.currentHz() == 88e6);  // contract default startHz
        CHECK(!sc.tick(0.0, false).has_value());
        CHECK(!sc.tick(100.0, true).has_value());
        CHECK(sc.state() == St::Idle);
    }

    // --- One long timeline: cadence, wrap, no-catch-up, pause, hold, resume,
    //     re-trigger, backward time, stop/restart, reconfigure -----------------
    {
        Scanner sc;
        sc.configure(latticeParams());
        CHECK(!sc.active());  // configure while Idle stores params only
        CHECK(sc.currentHz() == 100e6);

        sc.start(1000.0);
        CHECK(sc.active());
        CHECK(sc.state() == St::Scanning);

        // Scan cadence: first tune on the first tick at start time, then one
        // step per dwellMs (>= boundary is exact), wrapping stop -> start.
        CHECK(stepIs(sc, 1000.0, false, true, 100.0e6, St::Scanning, 100.0e6));
        CHECK(stepIs(sc, 1010.0, false, false, 0, St::Scanning, 100.0e6));
        CHECK(stepIs(sc, 1049.0, false, false, 0, St::Scanning, 100.0e6));  // 49 < 50
        CHECK(stepIs(sc, 1050.0, false, true, 100.2e6, St::Scanning, 100.2e6));  // == 50
        CHECK(stepIs(sc, 1100.0, false, true, 100.4e6, St::Scanning, 100.4e6));
        CHECK(stepIs(sc, 1150.0, false, true, 100.0e6, St::Scanning, 100.0e6));  // wrap

        // Dwell restarts at the emitting tick: a 250 ms stall yields exactly
        // ONE step, and the next dwell starts fresh (no carried excess).
        CHECK(stepIs(sc, 1400.0, false, true, 100.2e6, St::Scanning, 100.2e6));
        CHECK(stepIs(sc, 1401.0, false, false, 0, St::Scanning, 100.2e6));
        CHECK(stepIs(sc, 1451.0, false, true, 100.4e6, St::Scanning, 100.4e6));

        // Pause on activity: squelch open holds the frequency indefinitely,
        // far beyond dwellMs.
        CHECK(stepIs(sc, 1460.0, true, false, 0, St::Paused, 100.4e6));
        CHECK(stepIs(sc, 1600.0, true, false, 0, St::Paused, 100.4e6));
        CHECK(stepIs(sc, 2000.0, true, false, 0, St::Paused, 100.4e6));

        // Hold then resume: quiet starts a fresh 200 ms hold; expiry emits
        // nothing on its own tick; the retune comes resumeMs (100) later and
        // goes to the NEXT step — which wraps here, since 100.4e6 was last.
        CHECK(stepIs(sc, 2050.0, false, false, 0, St::Holding, 100.4e6));
        CHECK(stepIs(sc, 2100.0, false, false, 0, St::Holding, 100.4e6));  // 50/200
        CHECK(stepIs(sc, 2249.0, false, false, 0, St::Holding, 100.4e6));  // 199/200
        CHECK(stepIs(sc, 2250.0, false, false, 0, St::Scanning, 100.4e6));  // 200: expire
        CHECK(stepIs(sc, 2349.0, false, false, 0, St::Scanning, 100.4e6));  // 99/100
        CHECK(stepIs(sc, 2350.0, false, true, 100.0e6, St::Scanning, 100.0e6));  // NEXT
        // Cadence after resume is dwellMs again (50, not the 100 resume wait).
        CHECK(stepIs(sc, 2400.0, false, true, 100.2e6, St::Scanning, 100.2e6));

        // Re-trigger during hold: squelch reopening returns to Paused and
        // abandons the countdown, so the next quiet spell earns a FULL fresh
        // holdMs (150 ms after restart must NOT expire; 200 ms must).
        CHECK(stepIs(sc, 2410.0, true, false, 0, St::Paused, 100.2e6));
        CHECK(stepIs(sc, 2500.0, false, false, 0, St::Holding, 100.2e6));
        CHECK(stepIs(sc, 2600.0, true, false, 0, St::Paused, 100.2e6));   // re-trigger
        CHECK(stepIs(sc, 2650.0, false, false, 0, St::Holding, 100.2e6));  // fresh hold
        CHECK(stepIs(sc, 2800.0, false, false, 0, St::Holding, 100.2e6));  // 150/200
        CHECK(stepIs(sc, 2850.0, false, false, 0, St::Scanning, 100.2e6));  // 200: expire
        CHECK(stepIs(sc, 2950.0, false, true, 100.4e6, St::Scanning, 100.4e6));

        // Backward time counts as zero elapsed, and time then accumulates
        // from the new nowMs: 49 ms after the jump is short of the 50 ms
        // dwell, 50 ms fires.
        CHECK(stepIs(sc, 2000.0, false, false, 0, St::Scanning, 100.4e6));
        CHECK(stepIs(sc, 2049.0, false, false, 0, St::Scanning, 100.4e6));
        CHECK(stepIs(sc, 2050.0, false, true, 100.0e6, St::Scanning, 100.0e6));

        // stop() -> Idle; ticks are inert; currentHz still readable.
        sc.stop();
        CHECK(!sc.active());
        CHECK(sc.state() == St::Idle);
        CHECK(!sc.tick(3000.0, false).has_value());
        CHECK(sc.currentHz() == 100.0e6);

        // Restart is fresh: first tune at startHz on the first tick again.
        sc.start(5000.0);
        CHECK(stepIs(sc, 5000.0, false, true, 100.0e6, St::Scanning, 100.0e6));

        // Reconfigure while ACTIVE resets: still active, back to Scanning at
        // the NEW startHz, first tune re-emitted on the next tick, then the
        // new lattice cadence.
        Scanner::Params np = latticeParams();
        np.startHz = 200e6;
        np.stopHz = 200.2e6;
        np.stepHz = 100e3;
        sc.configure(np);
        CHECK(sc.active());
        CHECK(sc.state() == St::Scanning);
        CHECK(sc.currentHz() == 200e6);
        CHECK(stepIs(sc, 5010.0, false, true, 200.0e6, St::Scanning, 200.0e6));
        CHECK(stepIs(sc, 5060.0, false, true, 200.1e6, St::Scanning, 200.1e6));

        // Reconfigure while PAUSED also resets to Scanning at the new start.
        CHECK(stepIs(sc, 5070.0, true, false, 0, St::Paused, 200.1e6));
        sc.configure(np);
        CHECK(sc.state() == St::Scanning);
        CHECK(sc.currentHz() == 200e6);
        CHECK(stepIs(sc, 5080.0, false, true, 200.0e6, St::Scanning, 200.0e6));

        // start() while active also restarts from scratch.
        CHECK(stepIs(sc, 5130.0, false, true, 200.1e6, St::Scanning, 200.1e6));
        sc.start(6000.0);
        CHECK(stepIs(sc, 6000.0, false, true, 200.0e6, St::Scanning, 200.0e6));
    }

    // --- First tick ignores squelch (it describes the pre-scan tuning) -------
    {
        Scanner sc;
        sc.configure(latticeParams());
        sc.start(0.0);
        CHECK(stepIs(sc, 0.0, true, true, 100.0e6, St::Scanning, 100.0e6));
        CHECK(stepIs(sc, 10.0, true, false, 0, St::Paused, 100.0e6));  // honored now
    }

    // --- Sanitize: inverted range is swapped ----------------------------------
    {
        Scanner sc;
        Scanner::Params p = latticeParams();
        p.startHz = 100.4e6;  // inverted on purpose
        p.stopHz = 100e6;
        sc.configure(p);
        sc.start(0.0);
        CHECK(stepIs(sc, 0.0, false, true, 100.0e6, St::Scanning, 100.0e6));
        CHECK(stepIs(sc, 50.0, false, true, 100.2e6, St::Scanning, 100.2e6));
        CHECK(stepIs(sc, 100.0, false, true, 100.4e6, St::Scanning, 100.4e6));
        CHECK(stepIs(sc, 150.0, false, true, 100.0e6, St::Scanning, 100.0e6));
    }

    // --- Sanitize: step <= 0 becomes 1 kHz ------------------------------------
    {
        Scanner sc;
        Scanner::Params p;
        p.startHz = 100e6;
        p.stopHz = 100.002e6;  // lattice: +0, +1000, +2000
        p.stepHz = -5.0;       // negative -> 1 kHz
        p.dwellMs = 10;
        sc.configure(p);
        sc.start(0.0);
        CHECK(stepIs(sc, 0.0, false, true, 100000000.0, St::Scanning, 100000000.0));
        CHECK(stepIs(sc, 10.0, false, true, 100001000.0, St::Scanning, 100001000.0));
        CHECK(stepIs(sc, 20.0, false, true, 100002000.0, St::Scanning, 100002000.0));
        CHECK(stepIs(sc, 30.0, false, true, 100000000.0, St::Scanning, 100000000.0));

        Scanner sc0;
        p.stepHz = 0.0;  // zero -> 1 kHz as well
        sc0.configure(p);
        sc0.start(0.0);
        CHECK(stepIs(sc0, 0.0, false, true, 100000000.0, St::Scanning, 100000000.0));
        CHECK(stepIs(sc0, 10.0, false, true, 100001000.0, St::Scanning, 100001000.0));
    }

    // --- Sanitize: negative times clamp to 0; dwell 0 = one step per tick;
    //     hold 0 / resume 0 still consume one observable tick per phase -------
    {
        Scanner sc;
        Scanner::Params p = latticeParams();
        p.dwellMs = -10.0;   // -> 0
        p.holdMs = -1.0;     // -> 0
        p.resumeMs = -3.0;   // -> 0
        sc.configure(p);
        sc.start(0.0);
        CHECK(stepIs(sc, 0.0, false, true, 100.0e6, St::Scanning, 100.0e6));
        // dwell 0: exactly one step per tick — even at an unchanged nowMs.
        CHECK(stepIs(sc, 0.0, false, true, 100.2e6, St::Scanning, 100.2e6));
        CHECK(stepIs(sc, 1.0, false, true, 100.4e6, St::Scanning, 100.4e6));
        CHECK(stepIs(sc, 2.0, false, true, 100.0e6, St::Scanning, 100.0e6));
        // hold 0 and resume 0: Paused -> Holding -> Scanning -> retune takes
        // one tick per phase (at most one transition per tick).
        CHECK(stepIs(sc, 3.0, true, false, 0, St::Paused, 100.0e6));
        CHECK(stepIs(sc, 4.0, false, false, 0, St::Holding, 100.0e6));
        CHECK(stepIs(sc, 5.0, false, false, 0, St::Scanning, 100.0e6));
        CHECK(stepIs(sc, 6.0, false, true, 100.2e6, St::Scanning, 100.2e6));
    }

    // --- Frequencies never leave [start, stop]: non-commensurate range cycles
    //     the exact 3-point lattice for 300 retunes ----------------------------
    {
        Scanner sc;
        Scanner::Params p;
        p.startHz = 100e6;
        p.stopHz = 100.35e6;  // 100e6 + 3*150e3 = 100.45e6 > stop -> 3 points
        p.stepHz = 150e3;
        p.dwellMs = 0;
        sc.configure(p);
        sc.start(0.0);
        const double lattice[3] = {100.0e6, 100.15e6, 100.30e6};
        bool ok = true;
        int badAt = -1;
        for (int i = 0; i < 300 && ok; ++i) {
            const std::optional<double> r = sc.tick(static_cast<double>(i), false);
            const double want = lattice[i % 3];
            ok = r.has_value() && *r == want && sc.currentHz() == want
                 && want >= p.startHz && want <= p.stopHz;
            if (!ok) { badAt = i; }
        }
        if (!ok) { std::printf("  lattice cycle: first mismatch at tick %d\n", badAt); }
        CHECK(ok);
    }

    // --- Degenerate range start == stop: a one-point lattice, wraps to itself -
    {
        Scanner sc;
        Scanner::Params p = latticeParams();
        p.startHz = 100e6;
        p.stopHz = 100e6;
        p.dwellMs = 10;
        sc.configure(p);
        sc.start(0.0);
        CHECK(stepIs(sc, 0.0, false, true, 100.0e6, St::Scanning, 100.0e6));
        CHECK(stepIs(sc, 10.0, false, true, 100.0e6, St::Scanning, 100.0e6));
        CHECK(stepIs(sc, 20.0, false, true, 100.0e6, St::Scanning, 100.0e6));
    }

    return testSummary("test_scanner");
}
