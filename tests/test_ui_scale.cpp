/*
 * HOW BIG THE DESKTOP LAYOUT IS DRAWN ON A PHONE.
 *
 * THE PROBLEM THIS RULE EXISTS FOR. The Android port runs the REAL desktop
 * interface - the function rail, the spectrum, the pages - and that interface
 * is dimensioned in raw pixels measured against a 1280 x 720 desktop window on
 * a ~96 dpi monitor. On the emulator this port is first seen on (a Pixel 7,
 * 2264 x 1080, 420 dpi) neither the desktop's own 1:1 nor the screen's density
 * is right: 1:1 puts a rail key at about 4 mm, under Android's own 9 mm touch
 * target, and the density (x2.62) leaves the layout 412 logical pixels of
 * height against the 720 it was drawn for, so it overflows.
 *
 * So the scale is computed, and gui/ui_scale.hpp is the whole computation:
 * the largest factor at which 1280 x 720 still fits the framebuffer, capped at
 * the screen's density, floored at 1:1, with FOXSDR_UI_SCALE able to override
 * it outright so the two can be photographed side by side.
 *
 * WHY IT IS TESTED HERE RATHER THAN ON THE DEVICE. It is arithmetic on five
 * numbers, it decides what the whole interface looks like, and a wrong answer
 * is a screenshot nobody can read. The emulator's own numbers are one of the
 * cases below, so the figure quoted in the report is the figure this test
 * pins.
 *
 * BREAK-IT CHECK, run while writing this (and the reason the cases are shaped
 * as they are): replacing the min() of the two axes with the WIDTH ratio alone
 * - the mistake that reads as correct, because the emulator is in landscape
 * and the width has the slack - turns the emulator case from 1.5 into 1.76875
 * and fails 3 checks, starting at the kEmu line. Dropping the density cap
 * fails the tablet and desktop cases; dropping the 1:1 floor fails the
 * portrait case.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include "gui/ui_scale.hpp"
#include "test_check.hpp"

using cascade::gui::fittedUiScale;
using cascade::gui::kUiScaleMin;
using cascade::gui::uiScaleOverride;

int main() {
    // -- THE EMULATOR THIS PORT IS FIRST SEEN ON -----------------------------
    //
    // 2264 x 1080 at 420 dpi. Height is the binding axis (1080/720 = 1.5,
    // against 2264/1280 = 1.76875), and the density cap of 2.625 is not
    // reached, so the answer is the fit.
    const float kEmu = fittedUiScale(2264, 1080, 420.0f / 160.0f, nullptr);
    CHECK_NEAR(kEmu, 1.5f, 1e-6);

    // The same screen at full density is what the density screenshot shows,
    // and the point of the override is that it is reachable without a
    // rebuild. 2.625 > 1.5, so this can only come from the override.
    CHECK_NEAR(fittedUiScale(2264, 1080, 420.0f / 160.0f, "2.625"), 2.625f, 1e-6);

    // -- THE CAP: a screen with room to spare is not magnified past its own
    // density, because there is no point drawing a 9 mm key as a 15 mm one.
    // 3840 x 2160 would fit x3 by arithmetic; a 1:1 monitor caps it at 1.
    CHECK_NEAR(fittedUiScale(3840, 2160, 1.0f, nullptr), 1.0f, 1e-6);
    // A 2560 x 1600 tablet at x2: the fit (2.0 by width) equals the cap.
    CHECK_NEAR(fittedUiScale(2560, 1600, 2.0f, nullptr), 2.0f, 1e-6);
    // ...and at x1.5 the cap binds instead of the fit.
    CHECK_NEAR(fittedUiScale(2560, 1600, 1.5f, nullptr), 1.5f, 1e-6);

    // -- THE FLOOR: a phone held in PORTRAIT is narrower than the layout at
    // any scale at all (1080/1280 = 0.84), and the honest answer is 1:1 with
    // the panels overflowing rather than lettering smaller than a desktop's
    // on a screen held closer.
    CHECK_NEAR(fittedUiScale(1080, 2400, 2.625f, nullptr), kUiScaleMin, 1e-6);

    // A framebuffer nobody has measured yet (create() before the first
    // surface) must not produce 0 or a division by it.
    CHECK_NEAR(fittedUiScale(0, 0, 2.625f, nullptr), kUiScaleMin, 1e-6);
    CHECK_NEAR(fittedUiScale(-1, 720, 2.625f, nullptr), kUiScaleMin, 1e-6);

    // A density the configuration failed to report (0, or NaN from a bad
    // divide) must not collapse the scale to nothing.
    CHECK_NEAR(fittedUiScale(2264, 1080, 0.0f, nullptr), kUiScaleMin, 1e-6);
    // The NaN is produced at run time from a volatile zero: MSVC refuses a
    // constant 0.0f / 0.0f outright (C2124), and a compile-time NaN would
    // let the optimiser fold the check away in any case.
    volatile float nanSource = 0.0f;
    CHECK_NEAR(fittedUiScale(2264, 1080, nanSource / nanSource, nullptr), kUiScaleMin, 1e-6);
    // ...and a density past any shipping screen is clamped, not obeyed.
    CHECK_NEAR(fittedUiScale(99999, 99999, 100.0f, nullptr), 4.0f, 1e-6);

    // -- THE OVERRIDE, and every way it can be wrong ------------------------
    CHECK_NEAR(uiScaleOverride("1.75"), 1.75f, 1e-6);
    CHECK_NEAR(uiScaleOverride("1"), 1.0f, 1e-6);
    CHECK_NEAR(uiScaleOverride("4"), 4.0f, 1e-6);  // the inclusive top
    // Refused, not clamped: a typo must fall back to the computed scale
    // rather than silently pick the nearest legal value.
    CHECK(uiScaleOverride("0.5") == 0.0f);   // below the floor
    CHECK(uiScaleOverride("4.001") == 0.0f); // above the ceiling
    CHECK(uiScaleOverride("-2") == 0.0f);
    CHECK(uiScaleOverride("abc") == 0.0f);
    CHECK(uiScaleOverride("") == 0.0f);
    CHECK(uiScaleOverride(nullptr) == 0.0f);
    CHECK(uiScaleOverride("nan") == 0.0f);
    CHECK(uiScaleOverride("inf") == 0.0f);
    // A refused override leaves the fit exactly as it was.
    CHECK_NEAR(fittedUiScale(2264, 1080, 420.0f / 160.0f, "0"), 1.5f, 1e-6);
    CHECK_NEAR(fittedUiScale(2264, 1080, 420.0f / 160.0f, "banana"), 1.5f, 1e-6);

    return testSummary("test_ui_scale");
}
