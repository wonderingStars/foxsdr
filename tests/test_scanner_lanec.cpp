// Tests for the screen-lane-C additions to core/scanner.hpp: lockouts (C3), priority channels (C2), per-entry thresholds (C1), mute while scanning
// (C5). Scripted (nowMs, squelch, level) timelines with exact expectations, as test_scanner.cpp does: integer hertz and milliseconds, no tolerances.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/scanner.hpp"

#include "test_check.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>

using cascade::core::Scanner;
using St = Scanner::State;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool stepIs(Scanner& sc, double t, bool sq, bool wantRetune, double wantHz, St wantState, double level = kNaN) {
    const std::optional<double> r = sc.tick(t, sq, level);
    const bool ok = (r.has_value() == wantRetune) && (!wantRetune || *r == wantHz) && sc.state() == wantState;
    if (!ok) {
        std::printf("  tick(t=%.10g, sq=%d): got retune=%d hz=%.10g state=%d; want retune=%d hz=%.10g state=%d\n", t, sq ? 1 : 0,
                    r.has_value() ? 1 : 0, r.has_value() ? *r : 0.0, static_cast<int>(sc.state()), wantRetune ? 1 : 0, wantHz,
                    static_cast<int>(wantState));
    }
    return ok;
}

// 100.0 / 100.2 / 100.4 MHz, dwell 50 ms.
Scanner::Params lattice() {
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
    // --- C3: a permanent lockout is stepped over, wrapping included -----------------------------------------------------------------------
    {
        Scanner sc;
        sc.configure(lattice());
        sc.start(0.0);
        sc.lockOut(100.2e6);
        CHECK(stepIs(sc, 0.0, false, true, 100.0e6, St::Scanning));
        CHECK(stepIs(sc, 50.0, false, true, 100.4e6, St::Scanning));  // 100.2 skipped
        CHECK(stepIs(sc, 100.0, false, true, 100.0e6, St::Scanning)); // wrap
        CHECK(sc.isLocked(100.2e6 + 3.0));                             // within the epsilon
        CHECK(!sc.isLocked(100.2e6 + 50.0));
        sc.clearLockouts();
        CHECK(stepIs(sc, 150.0, false, true, 100.2e6, St::Scanning));  // lifted: visited again
    }
    // The first tune lands on the first UNLOCKED point.
    {
        Scanner sc;
        sc.configure(lattice());
        sc.start(0.0);
        sc.lockOut(100.0e6);
        CHECK(stepIs(sc, 0.0, false, true, 100.2e6, St::Scanning));
    }
    // Params::lockedHz (the stored configuration) does the same as the operator's lockOut.
    {
        Scanner sc;
        Scanner::Params p = lattice();
        p.lockedHz = {100.4e6, -1.0, std::nan("")};  // a negative and a NaN are dropped
        sc.configure(p);
        sc.start(0.0);
        CHECK(stepIs(sc, 0.0, false, true, 100.0e6, St::Scanning));
        CHECK(stepIs(sc, 50.0, false, true, 100.2e6, St::Scanning));
        CHECK(stepIs(sc, 100.0, false, true, 100.0e6, St::Scanning));  // 100.4 never emitted
        CHECK(sc.lockouts().empty());                                   // the configuration's set is not the operator's
    }
    // A temporary lockout ends on its own; a list scan walks around it.
    {
        Scanner sc;
        Scanner::Params p = lattice();
        p.list = {121.6e6, 121.7e6, 121.8e6};
        sc.configure(p);
        sc.start(0.0);
        CHECK(stepIs(sc, 0.0, false, true, 121.6e6, St::Scanning));
        sc.lockOut(121.7e6, 120.0);  // until 120 ms (last tick 0 + 120)
        CHECK(sc.lockouts().size() == 1);
        CHECK(stepIs(sc, 50.0, false, true, 121.8e6, St::Scanning));
        CHECK(stepIs(sc, 100.0, false, true, 121.6e6, St::Scanning));
        CHECK(stepIs(sc, 150.0, false, true, 121.7e6, St::Scanning));  // expired at 120: back in the walk
        CHECK(sc.lockouts().empty());
    }
    // lockOutCurrent while listening to a signal: the scan moves on at once and never returns to it.
    {
        Scanner sc;
        sc.configure(lattice());
        sc.start(0.0);
        CHECK(stepIs(sc, 0.0, false, true, 100.0e6, St::Scanning));
        CHECK(stepIs(sc, 50.0, false, true, 100.2e6, St::Scanning));
        CHECK(stepIs(sc, 60.0, true, false, 0, St::Paused));
        sc.lockOutCurrent();
        CHECK(stepIs(sc, 70.0, true, true, 100.4e6, St::Scanning));  // the flag on the first tick is stale and ignored
        CHECK(stepIs(sc, 120.0, false, true, 100.0e6, St::Scanning));
        CHECK(stepIs(sc, 170.0, false, true, 100.4e6, St::Scanning));  // 100.2 is passed over
    }
    // Every point locked: nothing is emitted, nothing breaks; lifting one resumes.
    {
        Scanner sc;
        Scanner::Params p = lattice();
        sc.configure(p);
        sc.start(0.0);
        sc.lockOut(100.0e6);
        sc.lockOut(100.2e6);
        sc.lockOut(100.4e6);
        CHECK(stepIs(sc, 0.0, false, false, 0, St::Scanning));
        CHECK(stepIs(sc, 50.0, false, false, 0, St::Scanning));
        CHECK(stepIs(sc, 500.0, false, false, 0, St::Scanning));
        sc.clearLockouts();
        sc.lockOut(100.2e6);
        CHECK(stepIs(sc, 600.0, false, true, 100.4e6, St::Scanning));  // the dwell that ran during the lock-up has expired
    }
    // lockOut on an Idle scanner is stored; lockOutCurrent does nothing; a NaN or negative frequency is refused.
    {
        Scanner sc;
        sc.configure(lattice());
        sc.lockOutCurrent();
        sc.lockOut(std::nan(""));
        sc.lockOut(-3.0);
        CHECK(sc.lockouts().empty());
        sc.lockOut(100.2e6);
        CHECK(sc.lockouts().size() == 1);
        sc.lockOut(100.2e6 + 2.0);  // the same channel: not stored twice
        CHECK(sc.lockouts().size() == 1);
    }

    // --- C2: priority channels --------------------------------------------------------------------------------------------------------------
    {
        Scanner sc;
        Scanner::Params p = lattice();
        p.priorityHz = {162.55e6};
        p.priorityIntervalMs = 120;
        sc.configure(p);
        sc.start(0.0);
        CHECK(stepIs(sc, 0.0, false, true, 100.0e6, St::Scanning));
        CHECK(stepIs(sc, 50.0, false, true, 100.2e6, St::Scanning));
        CHECK(stepIs(sc, 100.0, false, true, 100.4e6, St::Scanning));
        CHECK(stepIs(sc, 150.0, false, true, 162.55e6, St::Scanning));  // 150 ms of scanning: the priority visit outranks the step
        CHECK(sc.currentHz() == 162.55e6);
        CHECK(stepIs(sc, 180.0, false, false, 0, St::Scanning));         // settling (dwell 50)
        CHECK(stepIs(sc, 200.0, false, true, 100.4e6, St::Scanning));    // nothing there: back where it left
        CHECK(sc.currentHz() == 100.4e6);
    }
    // A signal on the priority channel holds the scan there, and after the hold the scan goes back to the point it left.
    {
        Scanner sc;
        Scanner::Params p = lattice();
        p.priorityHz = {162.55e6};
        p.priorityIntervalMs = 60;
        sc.configure(p);
        sc.start(0.0);
        CHECK(stepIs(sc, 0.0, false, true, 100.0e6, St::Scanning));
        CHECK(stepIs(sc, 60.0, false, true, 162.55e6, St::Scanning));
        CHECK(stepIs(sc, 100.0, true, false, 0, St::Scanning));         // inside the settling time: the flag is not believed yet
        CHECK(stepIs(sc, 110.0, true, false, 0, St::Paused));
        CHECK(sc.currentHz() == 162.55e6);
        CHECK(stepIs(sc, 300.0, true, false, 0, St::Paused));
        CHECK(stepIs(sc, 310.0, false, false, 0, St::Holding));
        CHECK(stepIs(sc, 510.0, false, false, 0, St::Scanning));        // the hold (200) ran out
        CHECK(sc.currentHz() == 162.55e6);                               // the tuner is still there
        CHECK(stepIs(sc, 610.0, false, true, 100.0e6, St::Scanning));    // resume wait over: the point it left, not the next
    }
    // Priority channels in turn; a locked-out one is skipped; all locked: no visit and the scan goes on.
    {
        Scanner sc;
        Scanner::Params p = lattice();
        p.priorityHz = {150.0e6, 151.0e6};
        p.priorityIntervalMs = 40;
        p.dwellMs = 30;
        sc.configure(p);
        sc.start(0.0);
        CHECK(stepIs(sc, 0.0, false, true, 100.0e6, St::Scanning));
        CHECK(stepIs(sc, 40.0, false, true, 150.0e6, St::Scanning));
        CHECK(stepIs(sc, 70.0, false, true, 100.0e6, St::Scanning));
        CHECK(stepIs(sc, 110.0, false, true, 151.0e6, St::Scanning));   // the second in turn
        sc.lockOut(150.0e6);
        sc.lockOut(151.0e6);
        CHECK(stepIs(sc, 140.0, false, true, 100.0e6, St::Scanning));   // back from 151
        CHECK(stepIs(sc, 190.0, false, true, 100.2e6, St::Scanning));   // both locked: a normal step, not a visit
    }
    // Priority off (interval 0, the default) changes nothing; a priority list with interval 0 never visits.
    {
        Scanner sc;
        Scanner::Params p = lattice();
        p.priorityHz = {162.55e6};
        sc.configure(p);
        sc.start(0.0);
        CHECK(stepIs(sc, 0.0, false, true, 100.0e6, St::Scanning));
        CHECK(stepIs(sc, 50.0, false, true, 100.2e6, St::Scanning));
        CHECK(stepIs(sc, 100000.0, false, true, 100.4e6, St::Scanning));
    }

    // --- C1: per-entry thresholds --------------------------------------------------------------------------------------------------------------
    {
        Scanner sc;
        Scanner::Params p = lattice();
        p.list = {121.6e6, 121.7e6};
        p.thresholdDb = {-60.0, kNaN};  // the first entry has its own threshold, the second none
        sc.configure(p);
        sc.start(0.0);
        CHECK(stepIs(sc, 0.0, false, true, 121.6e6, St::Scanning, -90.0));
        // Squelch closed but the level reaches this entry's threshold: a signal.
        CHECK(stepIs(sc, 10.0, false, false, 0, St::Paused, -55.0));
        CHECK(stepIs(sc, 20.0, false, false, 0, St::Paused, -55.0));    // level still up: stays (squelch flag ignored for this entry)
        // Squelch open but the level under the threshold: not a signal here.
        Scanner sd;
        sd.configure(p);
        sd.start(0.0);
        CHECK(stepIs(sd, 0.0, false, true, 121.6e6, St::Scanning, -90.0));
        CHECK(stepIs(sd, 10.0, true, false, 0, St::Scanning, -70.0));
        CHECK(stepIs(sd, 50.0, true, true, 121.7e6, St::Scanning, -70.0));
        // The entry without a threshold follows the flag, level or not.
        CHECK(stepIs(sd, 60.0, true, false, 0, St::Paused, -120.0));
        // No level supplied: the flag decides, on an entry with a threshold too.
        Scanner se;
        se.configure(p);
        se.start(0.0);
        CHECK(stepIs(se, 0.0, false, true, 121.6e6, St::Scanning));
        CHECK(stepIs(se, 10.0, true, false, 0, St::Paused));
    }
    // A list entry dropped by configure takes its threshold with it; a misaligned threshold list means none.
    {
        Scanner sc;
        Scanner::Params p = lattice();
        p.list = {std::nan(""), 119.0e6, 124.0e6};
        p.thresholdDb = {-10.0, -50.0, -90.0};
        sc.configure(p);
        sc.start(0.0);
        CHECK(stepIs(sc, 0.0, false, true, 119.0e6, St::Scanning, -90.0));
        CHECK(stepIs(sc, 10.0, false, false, 0, St::Paused, -40.0));    // -50 applies to 119.0, not -10
        Scanner sd;
        p.thresholdDb = {-50.0};                                          // does not line up: ignored
        sd.configure(p);
        sd.start(0.0);
        CHECK(stepIs(sd, 0.0, false, true, 119.0e6, St::Scanning, -90.0));
        CHECK(stepIs(sd, 10.0, false, false, 0, St::Scanning, -40.0));
    }

    // --- C5: mute while scanning ----------------------------------------------------------------------------------------------------------------
    {
        Scanner sc;
        Scanner::Params p = lattice();
        p.muteWhileScanning = true;
        sc.configure(p);
        CHECK(!sc.muted());  // idle
        sc.start(0.0);
        CHECK(sc.muted());
        CHECK(stepIs(sc, 0.0, false, true, 100.0e6, St::Scanning));
        CHECK(sc.muted());
        CHECK(stepIs(sc, 10.0, true, false, 0, St::Paused));
        CHECK(!sc.muted());  // listening
        CHECK(stepIs(sc, 20.0, false, false, 0, St::Holding));
        CHECK(!sc.muted());
        sc.stop();
        CHECK(!sc.muted());
        Scanner sd;
        sd.configure(lattice());
        sd.start(0.0);
        CHECK(!sd.muted());  // the option is off by default
    }

    return testSummary("test_scanner_lanec");
}
