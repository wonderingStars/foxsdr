// Tests for gui/display_power.hpp - whether the session's display is off, and
// the pause in drawing that follows it (the 2026-10-27 tester report: FoxSDR
// left drawing to a display that was off until the Intel driver gave up).
//
// What each leg proves:
//  1. The three MONITOR_DISPLAY_STATE values Microsoft documents decode as
//     off / on / dimmed, and anything else is Unknown rather than a guess.
//  2. Only Off pauses: Dim and Unknown are drawn, because pausing wrongly hides
//     the radio from someone looking at it.
//  3. The edges fire ONCE each - a display reported off on two frames is one
//     pause, or the log would say "off" sixty times a second.
//  4. The resume reports how long the display was off and how many frames were
//     not drawn, and a second pause starts its count from zero.
//  5. Frames are only counted while paused.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/display_power.hpp"
#include "test_check.hpp"

int main() {
    using cascade::gui::DisplayPause;
    using cascade::gui::DisplayPower;
    using cascade::gui::displayPowerFromData;
    using Edge = DisplayPause::Edge;

    // --- 1. Decoding ---------------------------------------------------------
    CHECK(displayPowerFromData(0) == DisplayPower::Off);
    CHECK(displayPowerFromData(1) == DisplayPower::On);
    CHECK(displayPowerFromData(2) == DisplayPower::Dim);
    CHECK(displayPowerFromData(3) == DisplayPower::Unknown);
    CHECK(displayPowerFromData(0xFFFFFFFFu) == DisplayPower::Unknown);

    // --- 2. Only Off pauses --------------------------------------------------
    {
        DisplayPause p;
        CHECK(p.update(DisplayPower::Unknown, 0.0) == Edge::None);
        CHECK(!p.paused());
        CHECK(p.update(DisplayPower::On, 1.0) == Edge::None);
        CHECK(p.update(DisplayPower::Dim, 2.0) == Edge::None);
        CHECK(!p.paused());
    }

    // --- 3, 4, 5. Edges once each, and what the resume reports ----------------
    {
        DisplayPause p;
        p.noteSkipped();  // not paused: not counted
        CHECK(p.skipped() == 0);
        CHECK(p.update(DisplayPower::Off, 10.0) == Edge::Paused);
        CHECK(p.paused());
        CHECK(p.update(DisplayPower::Off, 10.5) == Edge::None);  // still off: no second edge
        for (int i = 0; i < 80; ++i) { p.noteSkipped(); }
        CHECK(p.skipped() == 80);
        // Dimmed is a display coming back: drawing resumes.
        CHECK(p.update(DisplayPower::Dim, 610.0) == Edge::Resumed);
        CHECK(!p.paused());
        CHECK(p.lastOffSeconds() == 600.0);
        CHECK(p.skipped() == 80);
        CHECK(p.update(DisplayPower::On, 611.0) == Edge::None);
        p.noteSkipped();  // drawing again: not counted
        CHECK(p.skipped() == 80);
        // A second pause counts from zero.
        CHECK(p.update(DisplayPower::Off, 700.0) == Edge::Paused);
        CHECK(p.skipped() == 0);
        p.noteSkipped();
        CHECK(p.update(DisplayPower::Unknown, 701.0) == Edge::Resumed);
        CHECK(p.skipped() == 1);
        CHECK(p.lastOffSeconds() == 1.0);
    }

    return testSummary("test_display_power");
}
