// Tests for gui/hover_dwell.hpp - how long the pointer has rested over the spectrum or the
// waterfall, for the tip that says "Right-click to change the range and display" (0.99.76).
//
// The owner: "also add a tool tip when the mouse is over it for 4 secs saying you can right click
// to change it". The brief's list of what the clock must do: be due after four seconds of rest and
// not before, start over when the pointer moves, when a button is pressed, when the context menu
// opens, and when the pointer leaves, and come back after another four seconds and not at once.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/hover_dwell.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "test_check.hpp"

using cascade::gui::HoverDwell;
using cascade::gui::kDisplayTipDwellS;
using cascade::gui::kDisplayTipSlopPx;

namespace {

constexpr std::uint64_t kSpectrum = 1;
constexpr std::uint64_t kWaterfall = 2;

// A frame at t seconds, pointer at (100, 200), over `region`, free.
bool frame(HoverDwell& d, std::uint64_t region, double t, float x = 100.0f, float y = 200.0f,
           bool over = true, bool blocked = false) {
    return d.update(region, over, blocked, x, y, t);
}

void testTheConstant() {
    std::printf("  the dwell is the owner's four seconds, named\n");
    CHECK(kDisplayTipDwellS == 4.0);
    CHECK(kDisplayTipSlopPx > 0.0f && kDisplayTipSlopPx <= 12.0f);
}

void testNotBeforeFourSeconds() {
    std::printf("  not before four seconds, due at four, due after\n");
    HoverDwell d;
    CHECK(!frame(d, kWaterfall, 10.0));        // the rest begins here
    CHECK(!frame(d, kWaterfall, 10.5));
    CHECK(!frame(d, kWaterfall, 13.0));
    CHECK(!frame(d, kWaterfall, 13.99));
    CHECK(frame(d, kWaterfall, 14.0));         // 4.0 s of rest
    CHECK(frame(d, kWaterfall, 14.016));       // and it stays due while the pointer rests
    CHECK(frame(d, kWaterfall, 20.0));
    CHECK_NEAR(d.restS(20.0), 10.0, 1e-9);
}

void testSixtyFramesASecondGetThereToo() {
    std::printf("  frame by frame at 60 Hz: due on the first frame at or past four seconds\n");
    HoverDwell d;
    double firstDue = -1.0;
    int dueBefore = 0;
    for (int i = 0; i <= 600; ++i) {
        const double t = 5.0 + i / 60.0;
        if (frame(d, kSpectrum, t)) {
            if (firstDue < 0.0) { firstDue = t; }
            if (t - 5.0 < 4.0 - 1e-9) { ++dueBefore; }
        }
    }
    CHECK(dueBefore == 0);
    CHECK(firstDue >= 9.0 - 1e-9 && firstDue < 9.0 + 0.02);
}

void testMovementStartsANewRest() {
    std::printf("  moving more than the slop starts the rest again, where the pointer now is\n");
    HoverDwell d;
    CHECK(!frame(d, kWaterfall, 0.0, 100.0f, 200.0f));
    CHECK(!frame(d, kWaterfall, 3.0, 100.0f, 200.0f));
    CHECK(!frame(d, kWaterfall, 3.5, 100.0f + kDisplayTipSlopPx + 1.0f, 200.0f));    // moved
    CHECK(!frame(d, kWaterfall, 7.25, 100.0f + kDisplayTipSlopPx + 1.0f, 200.0f));   // 3.75 s since
    CHECK(frame(d, kWaterfall, 7.5, 100.0f + kDisplayTipSlopPx + 1.0f, 200.0f));     // 4.0 s since
    // Vertical counts too.
    HoverDwell e;
    CHECK(!frame(e, kWaterfall, 0.0, 50.0f, 50.0f));
    CHECK(!frame(e, kWaterfall, 3.75, 50.0f, 50.0f + kDisplayTipSlopPx + 1.0f));
    CHECK(!frame(e, kWaterfall, 4.5, 50.0f, 50.0f + kDisplayTipSlopPx + 1.0f));   // 0.75 s since
}

void testATremorIsStillARest() {
    std::printf("  a hand that trembles within the slop is resting\n");
    HoverDwell d;
    bool due = false;
    for (int i = 0; i <= 500; ++i) {
        const double t = i * 0.01;
        const float jitter = (i % 2 == 0) ? 0.0f : kDisplayTipSlopPx * 0.5f;
        if (frame(d, kWaterfall, t, 100.0f + jitter, 200.0f - jitter)) { due = true; }
    }
    CHECK(due);
    // The slop is measured from where the rest BEGAN, not from the last frame: a slow drift of
    // a pixel a frame is a move.
    HoverDwell e;
    bool dueDrifting = false;
    for (int i = 0; i <= 600; ++i) {
        if (frame(e, kWaterfall, i * 0.01, 100.0f + static_cast<float>(i) * 0.1f, 200.0f)) {
            dueDrifting = true;
        }
    }
    CHECK(!dueDrifting);   // 60 px in 6 s, never 4 s inside 6 px
}

void testAPressStartsOver() {
    std::printf("  a press (a button held) ends the tip, and it needs another full four seconds\n");
    HoverDwell d;
    CHECK(!frame(d, kWaterfall, 0.0));
    CHECK(frame(d, kWaterfall, 4.2));
    CHECK(!frame(d, kWaterfall, 4.25, 100.0f, 200.0f, true, /*blocked=*/true));  // the press
    CHECK(!frame(d, kWaterfall, 4.375, 100.0f, 200.0f, true, true));             // still held
    CHECK(!frame(d, kWaterfall, 4.5));                                           // released: a new rest
    CHECK(!frame(d, kWaterfall, 8.375));
    CHECK(frame(d, kWaterfall, 8.5));
    CHECK_NEAR(d.restS(8.5), 4.0, 1e-9);
}

void testAMenuOpeningStartsOver() {
    std::printf("  a context menu open (blocked) gives no tip however long, and none right after\n");
    HoverDwell d;
    CHECK(!frame(d, kSpectrum, 0.0));
    CHECK(frame(d, kSpectrum, 4.0));
    for (double t = 4.125; t < 30.0; t += 0.5) {
        CHECK(!frame(d, kSpectrum, t, 100.0f, 200.0f, true, true));
    }
    CHECK(!frame(d, kSpectrum, 30.0));       // the menu closed: not at once
    CHECK_NEAR(d.restS(30.0), 0.0, 1e-9);
    CHECK(!frame(d, kSpectrum, 33.875));
    CHECK(frame(d, kSpectrum, 34.0));
}

void testLeavingEndsIt() {
    std::printf("  leaving the panel ends the rest; coming back needs four more seconds\n");
    HoverDwell d;
    CHECK(!frame(d, kWaterfall, 0.0));
    CHECK(frame(d, kWaterfall, 4.0));
    CHECK(!frame(d, kWaterfall, 4.125, 100.0f, 200.0f, /*over=*/false));
    CHECK_NEAR(d.restS(4.125), 0.0, 1e-9);
    CHECK(!frame(d, kWaterfall, 5.0));
    CHECK(!frame(d, kWaterfall, 8.875));
    CHECK(frame(d, kWaterfall, 9.0));
}

void testResetForgetsTheRest() {
    std::printf("  reset (the panels were not drawn): the rest is forgotten\n");
    HoverDwell d;
    CHECK(!frame(d, kWaterfall, 0.0));
    CHECK(frame(d, kWaterfall, 4.0));
    d.reset();
    CHECK_NEAR(d.restS(4.0), 0.0, 1e-9);
    // The pointer is where it was, and the clock says far more than four seconds: not due.
    CHECK(!frame(d, kWaterfall, 60.0));
    CHECK(!frame(d, kWaterfall, 63.875));
    CHECK(frame(d, kWaterfall, 64.0));
}

void testALongSilenceIsNotARest() {
    std::printf("  ten seconds with nobody asking (another view had the screen) is not a rest\n");
    HoverDwell d;
    CHECK(!frame(d, kWaterfall, 0.0));
    CHECK(!frame(d, kWaterfall, 3.0));
    CHECK(!frame(d, kWaterfall, 20.0));      // 17 s unasked: the rest from 0.0 is not carried
    CHECK(!frame(d, kWaterfall, 23.875));
    CHECK(frame(d, kWaterfall, 24.0));
    // A pause a script holds a frame for (5 s at most) is still the same rest.
    HoverDwell e;
    CHECK(!frame(e, kWaterfall, 0.0));
    CHECK(!frame(e, kWaterfall, 2.5));
    CHECK(frame(e, kWaterfall, 5.0));
    CHECK(cascade::gui::kDisplayTipMaxGapS > 5.0);
}

void testTwoRegionsShareOneClockButNotOneRest() {
    std::printf("  the spectrum asking 'not over me' does not undo the waterfall's rest\n");
    HoverDwell d;
    bool due = false;
    // What drawCenterPanels does every frame: the spectrum is asked first, then the waterfall.
    for (int i = 0; i <= 500; ++i) {
        const double t = i * 0.01;
        frame(d, kSpectrum, t, 100.0f, 200.0f, /*over=*/false);
        if (frame(d, kWaterfall, t)) { due = true; }
    }
    CHECK(due);
    // Moving from one panel to the other is a new rest, not a continued one.
    HoverDwell e;
    CHECK(!frame(e, kSpectrum, 0.0));
    CHECK(frame(e, kSpectrum, 4.0));
    CHECK(!frame(e, kSpectrum, 4.125, 100.0f, 200.0f, false));
    CHECK(!frame(e, kWaterfall, 4.125));
    CHECK(!frame(e, kWaterfall, 7.875));
    CHECK(frame(e, kWaterfall, 8.125));
    // And over region B while region A is the one being timed restarts at B.
    HoverDwell f;
    CHECK(!frame(f, kSpectrum, 0.0));
    CHECK(!frame(f, kSpectrum, 3.875));
    CHECK(!frame(f, kWaterfall, 4.0));      // B takes over, A's 3.875 s do not carry
    CHECK(!frame(f, kWaterfall, 7.875));
    CHECK(frame(f, kWaterfall, 8.0));
}

void testTheClockMisbehaving() {
    std::printf("  a clock that is not a number, or runs backwards, ends the rest\n");
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    HoverDwell d;
    CHECK(!frame(d, kWaterfall, 100.0));
    CHECK(!frame(d, kWaterfall, kNaN));
    CHECK(!frame(d, kWaterfall, 104.0));    // the NaN ended the rest: 104 starts a new one
    CHECK(frame(d, kWaterfall, 108.0));
    CHECK(!frame(d, kWaterfall, 50.0));     // backwards: a new rest at 50, not a rest of -58 s
    CHECK(!frame(d, kWaterfall, 53.9));
    CHECK(frame(d, kWaterfall, 54.0));
    // A pointer position that is not a number is no position: not a rest.
    HoverDwell e;
    CHECK(!e.update(kWaterfall, true, false, std::numeric_limits<float>::quiet_NaN(), 5.0f, 0.0));
    CHECK(!e.update(kWaterfall, true, false, std::numeric_limits<float>::quiet_NaN(), 5.0f, 9.0));
    // Region 0 is nothing.
    HoverDwell g;
    CHECK(!g.update(0, true, false, 1.0f, 1.0f, 0.0));
    CHECK(!g.update(0, true, false, 1.0f, 1.0f, 9.0));
}

}  // namespace

int main() {
    std::printf("test_hover_dwell\n");
    testTheConstant();
    testNotBeforeFourSeconds();
    testSixtyFramesASecondGetThereToo();
    testMovementStartsANewRest();
    testATremorIsStillARest();
    testAPressStartsOver();
    testAMenuOpeningStartsOver();
    testLeavingEndsIt();
    testResetForgetsTheRest();
    testALongSilenceIsNotARest();
    testTwoRegionsShareOneClockButNotOneRest();
    testTheClockMisbehaving();
    return testSummary("test_hover_dwell");
}
