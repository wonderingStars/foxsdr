// test_view_follow.cpp - THE PICTURE FOLLOWS A RETUNE THAT LEAVES IT (a beta tester, 2026-11-09: "if I am on the 7 MHz band and
// switch to the 3.5 MHz band, the waterfall display remains on 7 MHz - I am forced to move the waterfall with the mouse").
//
// core::recentreOnRetune is the one rule both windows apply to their own view state (the ImGui window's FreqScale, the Slint
// window's ViewWindow): given the picture as it was drawn, the baseband the source now delivers and the tuned frequency before
// and after, it answers whether the picture moves and where to. Every branch of the rule is a row here, with the numbers written
// out, because the value is in the boundaries: the zoom width kept, the baseband edge, a picture wider than the new baseband, the
// full-span picture that needs no help, and the inputs that must never move anything.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/view_follow.hpp"

#include <cmath>
#include <cstdio>

#include "test_check.hpp"

using cascade::core::recentreOnRetune;
using cascade::core::ViewFollow;

int main() {
    std::printf("test_view_follow\n");
    const double nan = std::nan("");

    // --- THE TESTER'S CASE: a zoomed picture on 7 MHz, a retune to 3.62 MHz inside a wide baseband (an SDRplay at 10 MS/s) ---
    {
        // The picture: 300 kHz around 7.1 MHz. The baseband after the tune: 10 MHz centred near 3.62 MHz, which still holds the
        // old picture - so nothing else would have moved it. The picture goes to the new frequency at the same width.
        const ViewFollow f = recentreOnRetune(6.95e6, 7.25e6, -1.38e6, 8.62e6, 7.1e6, 3.62e6);
        CHECK(f.moved);
        CHECK_NEAR(f.lowHz, 3.47e6, 1.0);
        CHECK_NEAR(f.highHz, 3.77e6, 1.0);
        CHECK_NEAR(f.highHz - f.lowHz, 0.3e6, 1.0);   // the zoom width is kept
    }
    // --- THE 2 MS/s MINIATURE (the generator's span, as the application tests drive it): 100 kHz around 7.1 MHz, typed 7.9 MHz -----
    {
        const ViewFollow f = recentreOnRetune(7.05e6, 7.15e6, 6.9e6, 8.9e6, 7.1e6, 7.9e6);
        CHECK(f.moved);
        CHECK_NEAR(f.lowHz, 7.85e6, 1.0);
        CHECK_NEAR(f.highHz, 7.95e6, 1.0);
    }
    // --- A RETUNE THAT LANDS INSIDE THE PICTURE moves nothing: the hand's zoom is left where it is ------------------------------------
    {
        const ViewFollow f = recentreOnRetune(7.05e6, 7.15e6, 6.9e6, 8.9e6, 7.1e6, 7.12e6);
        CHECK(!f.moved);
        // ...including a landing exactly on an edge of the picture (inclusive).
        CHECK(!recentreOnRetune(7.05e6, 7.15e6, 6.9e6, 8.9e6, 7.1e6, 7.15e6).moved);
        CHECK(!recentreOnRetune(7.05e6, 7.15e6, 6.9e6, 8.9e6, 7.1e6, 7.05e6).moved);
    }
    // --- NO RETUNE, NO MOVE: the same frequency before and after, even with the picture elsewhere (a zoom the hand parked away) -----
    {
        CHECK(!recentreOnRetune(7.05e6, 7.15e6, 6.9e6, 8.9e6, 7.9e6, 7.9e6).moved);
    }
    // --- THE FULL-SPAN PICTURE needs no help: the baseband itself follows the tune, and the rule never invents a zoom ----------------
    {
        CHECK(!recentreOnRetune(6.1e6, 8.1e6, 6.9e6, 8.9e6, 7.1e6, 7.9e6).moved);          // the old full span
        CHECK(!recentreOnRetune(6.0e6, 8.5e6, 6.9e6, 8.9e6, 7.1e6, 7.9e6).moved);          // wider than the new baseband
    }
    // --- THE BASEBAND EDGE: the picture is brought as near the edge as the baseband allows, never past it ---------------------------
    {
        const ViewFollow f = recentreOnRetune(7.05e6, 7.15e6, 6.9e6, 8.9e6, 7.1e6, 8.88e6);
        CHECK(f.moved);
        CHECK_NEAR(f.lowHz, 8.8e6, 1.0);
        CHECK_NEAR(f.highHz, 8.9e6, 1.0);
        const ViewFollow g = recentreOnRetune(7.05e6, 7.15e6, 6.9e6, 8.9e6, 7.1e6, 6.91e6);
        CHECK(g.moved);
        CHECK_NEAR(g.lowHz, 6.9e6, 1.0);
        CHECK_NEAR(g.highHz, 7.0e6, 1.0);
    }
    // --- A PICTURE AS WIDE AS OR WIDER THAN THE NEW BASEBAND (a rate change under the tune) is not a zoom: both windows already show the
    // whole baseband for it (FreqScale::setSpan resets such a view; the Slint window's currentView answers the full span), so nothing moves.
    {
        CHECK(!recentreOnRetune(5.0e6, 6.0e6, 7.0e6, 7.5e6, 5.5e6, 7.2e6).moved);
        CHECK(!recentreOnRetune(5.0e6, 5.5e6, 7.0e6, 7.5e6, 5.2e6, 7.2e6).moved);
    }
    // --- A TUNE THAT LANDS OUTSIDE THE NEW BASEBAND (the source refused, the readback lags): the rule still clamps inside it --------
    {
        const ViewFollow f = recentreOnRetune(7.05e6, 7.15e6, 6.9e6, 8.9e6, 7.1e6, 9.5e6);
        CHECK(f.moved);
        CHECK_NEAR(f.highHz, 8.9e6, 1.0);
    }
    // --- NOTHING MOVES ON BAD INPUT: a first frame (no previous tune), a non-finite anything, an empty or inverted picture or baseband
    {
        CHECK(!recentreOnRetune(7.05e6, 7.15e6, 6.9e6, 8.9e6, nan, 7.9e6).moved);
        CHECK(!recentreOnRetune(7.05e6, 7.15e6, 6.9e6, 8.9e6, 7.1e6, nan).moved);
        CHECK(!recentreOnRetune(nan, 7.15e6, 6.9e6, 8.9e6, 7.1e6, 7.9e6).moved);
        CHECK(!recentreOnRetune(7.05e6, 7.15e6, nan, 8.9e6, 7.1e6, 7.9e6).moved);
        CHECK(!recentreOnRetune(7.15e6, 7.05e6, 6.9e6, 8.9e6, 7.1e6, 7.9e6).moved);   // inverted picture
        CHECK(!recentreOnRetune(7.1e6, 7.1e6, 6.9e6, 8.9e6, 7.1e6, 7.9e6).moved);     // empty picture
        CHECK(!recentreOnRetune(7.05e6, 7.15e6, 8.9e6, 6.9e6, 7.1e6, 7.9e6).moved);   // inverted baseband
        CHECK(!recentreOnRetune(7.05e6, 7.15e6, 0.0, 0.0, 7.1e6, 7.9e6).moved);       // an inert scale
    }
    return testSummary("test_view_follow");
}
