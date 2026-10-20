// test_tune_local.cpp - THE FIGURE A DRAG HAS SENT, as decisions (gui/tune_local.hpp; the pump move's step P4, the survey's 1.6 A).
// The window-driven half - a read of the state that precedes the pass, through a real AppWindow - is in tests/test_tuning_surface.cpp
// (testTheDragsLocalCopyIsWhatTheEngineDoes). This holds the rule by itself.
//
//   A  nothing sent: nothing stands, and the readback is shown
//   B  sent and not yet shown: it stands, and is what the panel draws and the next step starts from, however many times the state is read
//   C  the readback comes to equal it: it goes (the engine took exactly that)
//   D  the hold runs out (kTuneLocalHoldS): it goes, and the readback is the truth - at the edge, a hair before and at it
//   E  a new send replaces the old one and restarts the hold
//   F  a figure the engine took by a different route (another interface) is shown only for the hold
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>

#include "gui/tune_local.hpp"
#include "test_check.hpp"

using namespace cascade::gui;

int main() {
    std::printf("test_tune_local\n");
    TuneLocalFigure l;

    // A
    CHECK(!tuneLocalStands(l, 0.0, 10.0));
    CHECK(tuneLocalShown(l, 123.0, 10.0) == 123.0);
    CHECK(!tuneLocalRelease(l, 0.0, 10.0));

    // B
    tuneLocalSent(l, 60000.0, 10.0);
    for (int i = 0; i < 100; ++i) {                    // a hundred reads of a state that has not moved, inside the hold
        const double now = 10.0 + 0.004 * i;
        CHECK(tuneLocalStands(l, 0.0, now));
        CHECK(tuneLocalShown(l, 0.0, now) == 60000.0);
        CHECK(!tuneLocalRelease(l, 0.0, now));
        CHECK(l.valid);
    }

    // C
    CHECK(!tuneLocalStands(l, 60000.0, 10.1));
    CHECK(tuneLocalShown(l, 60000.0, 10.1) == 60000.0);
    CHECK(tuneLocalRelease(l, 60000.0, 10.1));
    CHECK(!l.valid);
    CHECK(!tuneLocalRelease(l, 60000.0, 10.2));        // already gone

    // D
    tuneLocalSent(l, 70000.0, 20.0);
    CHECK(tuneLocalStands(l, 5000.0, 20.0 + kTuneLocalHoldS - 0.001));
    CHECK(!tuneLocalStands(l, 5000.0, 20.0 + kTuneLocalHoldS));          // the hold is exclusive: at it, the readback is the truth
    CHECK(!tuneLocalStands(l, 5000.0, 20.0 + kTuneLocalHoldS + 3.0));
    CHECK(tuneLocalShown(l, 5000.0, 20.0 + kTuneLocalHoldS) == 5000.0);
    CHECK(tuneLocalRelease(l, 5000.0, 20.0 + kTuneLocalHoldS));
    CHECK(!l.valid);

    // E
    tuneLocalSent(l, 1.0, 30.0);
    tuneLocalSent(l, 2.0, 30.4);                       // a newer step of the same drag
    CHECK(l.hz == 2.0);
    CHECK(tuneLocalStands(l, 0.0, 30.4 + kTuneLocalHoldS - 0.001));      // the hold restarted with the send
    CHECK(!tuneLocalStands(l, 0.0, 30.4 + kTuneLocalHoldS));

    // F
    tuneLocalSent(l, 90000.0, 40.0);
    CHECK(tuneLocalShown(l, 25000.0, 40.1) == 90000.0);
    CHECK(tuneLocalShown(l, 25000.0, 40.0 + kTuneLocalHoldS + 0.01) == 25000.0);

    std::printf("  hold %.2f s\n", kTuneLocalHoldS);
    return testSummary("test_tune_local");
}
