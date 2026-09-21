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
#include "gui/app_window.hpp"
#include "gui/fonts.hpp"
#include "gui/page_geometry.hpp"
#include "gui/tune_control.hpp"
#include "gui/ui_scale.hpp"
#include "test_check.hpp"

using cascade::gui::fittedUiScale;
using cascade::gui::kUiScaleMin;
using cascade::gui::uiScale;
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

    // -- gui::px(), WHICH IS WHERE THE SCALE ACTUALLY REACHES THE INTERFACE --
    //
    // THE DEFECT THIS HALF OF THE FILE EXISTS FOR. The factor above was
    // computed correctly and then handed only to Dear ImGui - ScaleAllSizes
    // for the style, FontScaleDpi for the type - which scaled everything ImGui
    // owns and nothing this application owns. Every dimension of this
    // interface is one of the application's own raw pixel counts, so on the
    // tablet emulator at x2 the picture came out at two sizes at once:
    // desktop-sized brass carrying double-sized lettering. The SAMPLE RATE and
    // FRAME TIME meters were where it showed, because a meter is the only
    // thing on the top bar whose height is part layout figure and part type
    // metric - and the arithmetic of exactly that is the last block below.
    //
    // ONE MULTIPLICATION, AND THE WHOLE CONTRACT IS THAT IT IS EXACT AT 1:1.
    // Every constant here is still the desktop's own figure; px() is applied
    // where it is used. A float multiplied by 1.0f is the same float, bit for
    // bit, which is what lets the desktop suite go on pinning these numbers as
    // literals (test_app_rail, test_tune_control, test_spectrum_waterfall_type
    // all do) without a single expectation being edited for this change.
    CHECK(uiScale() == 1.0f);  // the default, before anything sets it

    // A table rather than a paragraph of one-liners: every figure this change
    // set now scales, named with the file it lives in, so a constant that
    // grows a px() at its call site and is not listed here is visible as an
    // omission rather than as nothing at all.
    struct Figure {
        const char* name;
        float units;
    };
    const Figure figures[] = {
        // gui/app_window.hpp - the function rail's column and its rows
        {"kMenuWidth", cascade::gui::kMenuWidth},
        {"kRailPlatePad", cascade::gui::kRailPlatePad},
        {"kRailKeyInset", cascade::gui::kRailKeyInset},
        {"kRailKeyGap", cascade::gui::kRailKeyGap},
        {"kRailLabelPadX", cascade::gui::kRailLabelPadX},
        {"kRailKeyMin", cascade::gui::kRailKeyMin},
        {"kRailKeyMax", cascade::gui::kRailKeyMax},
        {"kRailRowMinH", cascade::gui::kRailRowMinH},
        {"kRailRowPadY", cascade::gui::kRailRowPadY},
        // gui/tune_control.hpp - the top bar and the two meters on it
        {"kDeckBarH", cascade::gui::kDeckBarH},
        {"kDeckCoreW", cascade::gui::kDeckCoreW},
        {"kMeterW", cascade::gui::kMeterW},
        {"kMeterGap", cascade::gui::kMeterGap},
        {"kMeterRightMargin", cascade::gui::kMeterRightMargin},
        {"kMeterCoreClearance", cascade::gui::kMeterCoreClearance},
        {"kMeterTopY", cascade::gui::kMeterTopY},
        {"kMeterFaceH", cascade::gui::kMeterFaceH},
        {"kMeterTextGap", cascade::gui::kMeterTextGap},
        {"kMuteBannerMinW", cascade::gui::kMuteBannerMinW},
        {"kMuteBannerEdgeClearance", cascade::gui::kMuteBannerEdgeClearance},
        {"kFreqPlateW", cascade::gui::kFreqPlateW},
        {"kFreqPlateH", cascade::gui::kFreqPlateH},
        // gui/page_geometry.hpp - a torn-off page's floor
        {"kPageMinW", cascade::gui::kPageMinW},
        {"kPageMinH", cascade::gui::kPageMinH},
        {"kPageInsideMargin", cascade::gui::kPageInsideMargin},
        // gui/fonts.hpp - the four type sizes, which scale through the SAME
        // helper because most of this bench's lettering goes straight into a
        // draw list at an explicit size, where no ImGui global reaches it.
        {"fonts::kUiSize", cascade::gui::fonts::kUiSize},
        {"fonts::kLegendSize", cascade::gui::fonts::kLegendSize},
        {"fonts::kReadingSize", cascade::gui::fonts::kReadingSize},
        {"fonts::kTinySize", cascade::gui::fonts::kTinySize},
    };

    // At 1:1 the helper is the IDENTITY, and identity is meant literally: the
    // same value, not a value within a tolerance of it.
    for (const Figure& f : figures) {
        CHECK(cascade::gui::px(f.units) == f.units);
        CHECK(cascade::gui::units(f.units) == f.units);
    }

    // ...and at x2 every one of them is exactly double, and units() takes it
    // back. These are the two properties every call site depends on: px() to
    // draw with, units() to ask a rule written in reference units about a
    // measurement ImGui handed back in screen pixels.
    cascade::gui::setUiScale(2.0f);
    CHECK(uiScale() == 2.0f);
    for (const Figure& f : figures) {
        CHECK(cascade::gui::px(f.units) == f.units * 2.0f);
        CHECK(cascade::gui::units(cascade::gui::px(f.units)) == f.units);
    }
    // Two of them spelled out, because a table is easy to read past: the rail
    // column and the deck.
    CHECK(cascade::gui::px(cascade::gui::kMenuWidth) == 768.0f);
    CHECK(cascade::gui::px(cascade::gui::kDeckBarH) == 320.0f);

    // A composed rule, not just a constant: a rail row is sized in reference
    // units and THEN scaled, so the word and the air round it stay in
    // proportion. Sized from the scaled type instead - railRowHeight(34) -
    // the answer would be 44, a row with the desktop's 5 px of padding round
    // double-sized lettering.
    CHECK(cascade::gui::px(cascade::gui::railRowHeight(cascade::gui::fonts::kUiSize)) ==
          56.0f);
    CHECK(cascade::gui::railRowHeight(cascade::gui::px(cascade::gui::fonts::kUiSize)) ==
          44.0f);  // ...which is what NOT doing it that way gives

    // -- AND THE CLIPPED CAPTIONS, IN ARITHMETIC -----------------------------
    //
    // Three states of the same sum, and the middle one is the bug as reported:
    // "at scale 2.0 the SAMPLE RATE and FRAME TIME captions on the top plate
    // are clipped by the top bar's bottom edge". A meter block is a face plus
    // two lines of text plus their air, hung kMeterTopY below the bar's top
    // edge, and it has to stand inside a bar kDeckBarH tall.
    using cascade::gui::meterBlockH;
    using cascade::gui::meterBlockHAtScale;
    using cascade::gui::metersStandInsideBar;
    using cascade::gui::px;

    // (1) The desktop, where this was always right: 28 + 66 + 2x17 + 8 = 136,
    // inside 160 with room to spare.
    cascade::gui::setUiScale(1.0f);
    {
        const float lineH = cascade::gui::fonts::kUiSize;  // the ambient face
        // meterBlockHAtScale is the composition drawToolbar itself makes, so
        // a px() dropped from either layout figure inside it fails here.
        const float block = meterBlockHAtScale(lineH);
        CHECK(block == 136.0f - cascade::gui::kMeterTopY);
        CHECK(metersStandInsideBar(px(cascade::gui::kDeckBarH),
                                   px(cascade::gui::kMeterTopY), block));
    }

    // (2) THE DEFECT. The type is scaled (ImGui's own globals did that much)
    // and the layout is not: the face stays 66, the gap 8, the top 28, the bar
    // 160 - and the two lines of text are 34 each. 28 + 66 + 68 + 8 = 170,
    // which is 10 px past the bottom of a bar that never grew, and the bar's
    // child clips the difference off the captions. This is the case that goes
    // RED against the code as it shipped, and the reason the block below is
    // not simply "it fits at both scales".
    {
        const float scaledLineH = cascade::gui::fonts::kUiSize * 2.0f;
        const float block = meterBlockH(cascade::gui::kMeterFaceH, scaledLineH,
                                        cascade::gui::kMeterTextGap);
        CHECK(!metersStandInsideBar(cascade::gui::kDeckBarH, cascade::gui::kMeterTopY,
                                    block));
    }

    // (3) THE FIX: one factor, reaching the layout as well as the type.
    // 56 + 132 + 68 + 16 = 272, inside a 320 px bar - the same proportion as
    // (1), which is the whole claim this change set makes.
    cascade::gui::setUiScale(2.0f);
    {
        const float lineH = px(cascade::gui::fonts::kUiSize);
        const float block = meterBlockHAtScale(lineH);
        CHECK(block == 216.0f);
        CHECK(metersStandInsideBar(px(cascade::gui::kDeckBarH),
                                   px(cascade::gui::kMeterTopY), block));
        // ...and the proportion is the same one, to the pixel: twice (1).
        CHECK(px(cascade::gui::kMeterTopY) + block ==
              2.0f * (cascade::gui::kMeterTopY + 108.0f));
    }

    // The two meters keep their place on a bar that is itself scaled: the
    // rules in gui/tune_control.hpp are written in reference units, so they
    // are asked about a bar width taken back through units(). A first-launch
    // bar at x2 is 2464 px wide and the meters fit; the SAME 1232 px bar it
    // would have been at 1:1 no longer does, because the fixed cluster it has
    // to clear is 1776 px now.
    CHECK(cascade::gui::metersFitOnBar(
        cascade::gui::units(px(cascade::gui::kFirstLaunchBarW)), cascade::gui::kDeckCoreW));
    CHECK(!cascade::gui::metersFitOnBar(cascade::gui::units(cascade::gui::kFirstLaunchBarW),
                                        cascade::gui::kDeckCoreW));

    // An out-of-range scale is REFUSED and leaves the last good one standing,
    // the same refusal uiScaleOverride makes, so a bad number cannot make the
    // interface unreadable at the one moment it is set.
    cascade::gui::setUiScale(0.25f);
    CHECK(uiScale() == 2.0f);
    cascade::gui::setUiScale(9.0f);
    CHECK(uiScale() == 2.0f);
    cascade::gui::setUiScale(nanSource / nanSource);
    CHECK(uiScale() == 2.0f);
    cascade::gui::setUiScale(1.0f);
    CHECK(uiScale() == 1.0f);

    // --- THE USER'S OWN MAGNIFICATION ------------------------------------
    //
    // THE REPORT (Android tester, through the owner, 2026-09-21): "Much of
    // the text is very small -even on a 14-inch display- and cannot be
    // enlarged (I wear glasses...)". The fitted scale is capped at the
    // screen's density, and a 14-inch tablet at about 160 dpi reports a
    // density of about 1.0 - so the cap held the interface at desktop sizes
    // on a screen held at arm's length.
    using cascade::gui::clampUiZoomPercent;
    using cascade::gui::zoomedUiScale;

    // 100% IS EXACTLY TODAY, which is what makes the setting safe to ship:
    // every config written before it carries no such field at all.
    CHECK(clampUiZoomPercent(100) == 100);
    CHECK(zoomedUiScale(1.0f, 100) == 1.0f);
    CHECK(zoomedUiScale(2.0f, 100) == 2.0f);

    // The magnification multiplies the fitted scale.
    CHECK(zoomedUiScale(1.0f, 150) == 1.5f);
    CHECK(zoomedUiScale(2.0f, 150) == 3.0f);

    // NEVER PAST THE CEILING EVERY OTHER SCALE OBEYS: beyond 4x the layout
    // stops being a layout, whoever asked for it.
    CHECK(zoomedUiScale(2.0f, 250) == cascade::gui::kUiScaleMax);
    CHECK(zoomedUiScale(4.0f, 200) == cascade::gui::kUiScaleMax);

    // ...and never below 1:1, whatever it is handed: a fitted scale of zero
    // is a window that has not been measured yet.
    CHECK(zoomedUiScale(0.0f, 100) == 1.0f);
    CHECK(zoomedUiScale(-3.0f, 100) == 1.0f);

    // OUT OF RANGE IS CLAMPED, NOT REFUSED: this value comes from a slider
    // and from a config file a user may have edited by hand, and an
    // unusable interface must not be reachable from either.
    CHECK(clampUiZoomPercent(10) == cascade::gui::kUiZoomMinPercent);
    CHECK(clampUiZoomPercent(9999) == cascade::gui::kUiZoomMaxPercent);
    // ...and snapped to the step, so every size is one a key can return from.
    CHECK(clampUiZoomPercent(137) == 125);
    CHECK(clampUiZoomPercent(138) == 150);
    CHECK(clampUiZoomPercent(175) == 175);

    return testSummary("test_ui_scale");
}
