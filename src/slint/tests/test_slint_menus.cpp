// The Menus family (ui/menus.slint): MenuItem in every kind (item, checked, radio, submenu, separator, heading, slider), and the Menu
// popup - rendered headlessly from ui/specimens/menus_specimen.slint and probed against the plugin kit's `.menu` / `.mi` / `.msep` /
// `.mh` CSS (Workbench-PluginKit.dc.html, Workbench-Menus.dc.html).
//
// The sheet (600 x 380): a column of standalone MenuItems on a Theme.menu panel at x 17..250 (rows listed in the specimen's header), and
// the Menu panel opened at (300, 16) by open-menu(): 12 entries from y 21, 320 high. Row heights: item / checked / radio / submenu 26,
// separator 9, heading 21, slider 36.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "menus_specimen.h"

#include <chrono>
#include <cmath>
#include <vector>

using namespace foxsdr::slinttest;

namespace {

Rgb tok(slint::Color c) { return {c.red(), c.green(), c.blue()}; }

int firstInkX(const Frame& f, int x0, int y0, int x1, int y1, Rgb bg) {
    for (int x = x0; x < x1; ++x) { if (f.countInk(x, y0, x + 1, y1, bg) > 0) { return x; } }
    return -1;
}
int lastInkX(const Frame& f, int x0, int y0, int x1, int y1, Rgb bg) {
    for (int x = x1 - 1; x >= x0; --x) { if (f.countInk(x, y0, x + 1, y1, bg) > 0) { return x; } }
    return -1;
}
int lastInkY(const Frame& f, int x0, int y0, int x1, int y1, Rgb bg) {
    for (int y = y1 - 1; y >= y0; --y) { if (f.countInk(x0, y, x1, y + 1, bg) > 0) { return y; } }
    return -1;
}

MenuEntry entry(MenuItemKind kind, int id, const char* label, const char* hint = "", bool checked = false, bool enabled = true, float value = 0.0f) {
    return MenuEntry{kind, id, slint::SharedString(label), slint::SharedString(hint), checked, enabled, value};
}

constexpr int kRowX = 17, kRowW = 234, kRowRight = kRowX + kRowW;                    // the standalone rows: x 17..250
constexpr int kHeadingY = 17, kItemY = 38, kCheckedOnY = 64, kCheckedOffY = 90, kRadioOnY = 116, kRadioOffY = 142, kSubmenuY = 168;
constexpr int kHintY = 194, kSepY = 220, kSliderY = 229, kSliderOffY = 265, kDisabledY = 301, kHotY = 327;
constexpr int kPopX = 300, kPopY = 16, kPopH = 320, kInset = 5;                       // the Menu panel: border 1 + padding 4

// The popup's row tops, from its entries' heights.
struct Rows {
    int top[12] = {};
    Rows() {
        const int h[12] = {21, 26, 26, 26, 26, 26, 26, 9, 36, 36, 26, 26};
        int y = kPopY + kInset;
        for (int i = 0; i < 12; ++i) { top[i] = y; y += h[i]; }
        total = y + kInset - kPopY;
    }
    int total = 0;
};

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = MenusSpecimen::create();
    slint::Window& win = ui->window();

    const Rgb bg = tok(ui->global<Theme>().get_bg());
    const Rgb menu = tok(ui->global<Theme>().get_menu());
    const Rgb hot = tok(ui->global<Theme>().get_menu_hot());
    const Rgb fg = tok(ui->global<Theme>().get_fg());
    const Rgb muted = tok(ui->global<Theme>().get_muted());
    const Rgb disabled = tok(ui->global<Theme>().get_disabled());
    const Rgb accent = tok(ui->global<Theme>().get_accent());
    const Rgb knobRing = tok(ui->global<Theme>().get_knob_ring());
    const Rgb lineStrong = tok(ui->global<Theme>().get_line_strong());
    const Rgb sliderTrack = tok(ui->global<Theme>().get_slider_track());
    const Rgb sliderFill = tok(ui->global<Theme>().get_slider_fill());
    const Rgb sliderKnob = tok(ui->global<Theme>().get_slider_knob());
    const Rgb sliderKnobRing = tok(ui->global<Theme>().get_slider_knob_ring());
    const Rgb sliderOffFill = tok(ui->global<Theme>().get_slider_disabled_fill());
    const Rgb sliderOffKnob = tok(ui->global<Theme>().get_slider_disabled_knob());
    CHECK(nearColour(menu, rgb(0x262a32), 0) && nearColour(hot, rgb(0x363b45), 0) && nearColour(knobRing, rgb(0x434955), 0));   // the artboard's values
    CHECK(nearColour(disabled, rgb(0x8a93a0), 0) && nearColour(muted, rgb(0x9aa3ad), 0) && nearColour(lineStrong, rgb(0x363b45), 0));

    const auto away = [&] { moveTo(win, 590.0f, 372.0f); };

    // ===== the standalone rows =============================================================================
    away();
    const Frame f0 = render(ui, 600, 380);
    CHECK(f0.width == 600 && f0.height == 380);
    CHECK(f0.near(100, 60, menu, 0) && f0.near(14, 30, menu, 0));                         // the panel ground the rows sit on
    CHECK(f0.near(12, 100, knobRing, 0) && f0.near(100, 12, knobRing, 0));                // ...with its border
    // nothing of the Menu yet: the popup is closed
    CHECK(f0.countInk(kPopX, kPopY, kPopX + 240, kPopY + kPopH, bg) == 0);

    // --- a MenuItem is 26px high, and hover paints Theme.menu-hot over exactly that ---------------------------------
    CHECK(f0.near(200, kItemY + 12, menu, 0));                                         // not hot at rest
    CHECK(f0.near(200, kHotY + 12, hot, 0));                                           // ...except the row forced hot (its submenu is open)
    CHECK(f0.rowNear(kHotY + 2, kRowX + 5, kRowRight - 5, hot, 0));
    moveTo(win, 200.0f, kItemY + 13.0f);
    const Frame h1 = render(ui, 600, 380);
    CHECK(h1.firstRowNear(200, kItemY - 8, kItemY + 40, hot, 0) == kItemY);         // the hot ground starts on the row's top...
    CHECK(h1.countNear(200, kItemY - 8, 201, kItemY + 40, hot, 0) == 26);           // ...and is 26 rows high (Theme.menu-item-h)
    CHECK(26 == static_cast<int>(ui->global<Theme>().get_menu_item_h()));
    CHECK(h1.rowNear(kItemY + 2, kRowX + 6, kRowRight - 6, hot, 0));                   // the whole width, inside the rounded corners
    CHECK(h1.near(kRowX, kItemY + 13, hot, 0) && h1.near(kRowRight - 1, kItemY + 13, hot, 0));
    CHECK(h1.near(kRowX - 1, kItemY + 13, menu, 0) && h1.near(kRowRight, kItemY + 13, menu, 0));
    CHECK(!h1.near(kRowX, kItemY, hot, 0) && h1.near(kRowX, kItemY, menu, 0));         // radius 5: the corner pixel is not painted
    CHECK(h1.near(200, kCheckedOnY + 13, menu, 0));                                    // the neighbour is not
    moveTo(win, 200.0f, kCheckedOnY + 13.0f);
    const Frame h2 = render(ui, 600, 380);
    CHECK(h2.near(200, kItemY + 13, menu, 0) && h2.near(200, kCheckedOnY + 13, hot, 0));  // the highlight follows the pointer
    for (const int y : {kCheckedOffY, kRadioOnY, kRadioOffY, kSubmenuY, kHintY}) {  // every pickable kind lights
        moveTo(win, 200.0f, y + 13.0f);
        CHECK(render(ui, 600, 380).near(200, y + 3, hot, 0));
    }
    // a disabled row does not light; neither do the separator, the heading and the sliders
    for (const int y : {kDisabledY, kSepY + 1, kHeadingY + 4, kSliderY + 4, kSliderOffY + 4}) {
        moveTo(win, 225.0f, y + 0.0f);
        const Frame nl = render(ui, 600, 380);
        CHECK(nl.countNear(kRowX + 6, y - 1, kRowRight - 6, y + 2, hot, 0) == 0);
        CHECK(!nl.near(225, y, hot, 0));
    }
    away();

    // --- the check mark: a 12px tick in Theme.fg at the left of a `checked` row that is on ---------------------------
    const Frame f1 = render(ui, 600, 380);
    CHECK(f1.countNear(27, kCheckedOnY + 7, 39, kCheckedOnY + 19, fg, 30) >= 4);    // the tick's strokes
    CHECK(f1.countInk(27, kCheckedOnY + 7, 39, kCheckedOnY + 19, menu) >= 10);
    CHECK(f1.countInk(27, kCheckedOffY + 7, 39, kCheckedOffY + 19, menu) == 0);     // off: no mark
    CHECK(f1.countInk(27, kItemY + 7, 39, kItemY + 19, menu) == 0);                 // a plain item: none
    CHECK(f1.countInk(27, kCheckedOnY + 1, 39, kCheckedOnY + 6, menu) == 0);        // the tick sits inside its 12px box (y 71..83)
    CHECK(f1.countInk(27, kCheckedOnY + 20, 39, kCheckedOnY + 25, menu) == 0);
    // labels line up past the mark column: 10px padding + 20px column
    int xs[7];
    int n = 0;
    for (const int y : {kItemY, kCheckedOnY, kCheckedOffY, kRadioOnY, kRadioOffY, kSubmenuY, kHintY}) {
        xs[n++] = firstInkX(f1, 40, y + 4, 140, y + 22, menu);
    }
    for (int i = 0; i < 7; ++i) { CHECK(xs[i] >= kRowX + 30 && xs[i] <= kRowX + 32); CHECK(xs[i] - xs[0] >= -2 && xs[i] - xs[0] <= 2); }

    // the label sits in the middle of its 26px row: at least 5px clear of the top and 4px of the bottom (a descender may hang)
    for (const int y : {kItemY, kCheckedOnY, kRadioOnY, kSubmenuY, kHintY}) {
        int first = -1, last = -1;
        for (int yy = y; yy < y + 26; ++yy) {
            if (f1.countInk(kRowX + 30, yy, kRowX + 120, yy + 1, menu) > 0) { if (first < 0) { first = yy; } last = yy; }
        }
        CHECK(first - y >= 5 && first - y <= 9 && y + 25 - last >= 4);
    }

    // --- the radio: a 6px dot, Theme.accent when on, a Theme.knob-ring ring when off --------------------------------
    CHECK(f1.near(33, kRadioOnY + 13, accent, 0));
    CHECK(f1.countNear(30, kRadioOnY + 10, 36, kRadioOnY + 16, accent, 12) >= 14);                 // the 4x4 core of a 6px dot, and its edge
    CHECK(f1.countNear(26, kRadioOnY + 6, 40, kRadioOnY + 20, accent, 30) <= 40);   // a dot, not a bar
    CHECK(f1.near(33, kRadioOffY + 13, menu, 0));                                      // off: hollow...
    CHECK(f1.countNear(29, kRadioOffY + 9, 37, kRadioOffY + 17, knobRing, 10) >= 8);   // ...inside a ring
    CHECK(f1.countNear(26, kRadioOffY + 6, 40, kRadioOffY + 20, accent, 30) == 0);
    CHECK(f1.countNear(26, kCheckedOffY + 6, 40, kCheckedOffY + 20, accent, 30) == 0);   // only radios draw dots

    // --- the submenu: a chevron at the right ---------------------------------------------------------------------
    CHECK(f1.countInk(kRowRight - 24, kSubmenuY + 6, kRowRight - 10, kSubmenuY + 20, menu) >= 6);
    CHECK(f1.countNear(kRowRight - 24, kSubmenuY + 6, kRowRight - 10, kSubmenuY + 20, muted, 40) >= 3);
    CHECK(f1.countNear(kRowRight - 24, kSubmenuY + 6, kRowRight - 10, kSubmenuY + 20, fg, 8) == 0);   // Theme.muted, not Theme.fg
    CHECK(f1.countInk(kRowRight - 24, kItemY + 6, kRowRight - 10, kItemY + 20, menu) == 0);         // a plain row has none
    CHECK(f1.countInk(kRowRight - 9, kSubmenuY + 2, kRowRight, kSubmenuY + 24, menu) == 0);         // 10px of padding after it

    // --- the hint: 11px Theme.muted, right-aligned, at least 16px from the label --------------------------------------
    CHECK(f1.countNear(150, kHintY + 4, kRowRight - 9, kHintY + 22, muted, 12) >= 6);
    CHECK(f1.countNear(150, kHintY + 4, kRowRight - 9, kHintY + 22, fg, 8) == 0);   // the hint is Theme.muted, never the label's Theme.fg
    const int hintEnd = lastInkX(f1, 150, kHintY + 4, kRowRight, kHintY + 22, menu);
    CHECK(hintEnd >= kRowRight - 13 && hintEnd <= kRowRight - 10);                  // ends at the 10px padding
    const int hintStart = firstInkX(f1, 150, kHintY + 4, kRowRight, kHintY + 22, menu);
    const int labelEnd = lastInkX(f1, 40, kHintY + 4, 150, kHintY + 22, menu);
    CHECK(labelEnd > 80 && hintStart - labelEnd >= 14);
    CHECK(f1.countInk(150, kItemY + 4, kRowRight, kItemY + 22, menu) == 0);         // no hint, no ink at the right

    // --- the separator: a 1px Theme.line-strong rule, 4px margins above and below, inset 2px ------------------------------
    CHECK(f1.rowNear(kSepY + 4, kRowX + 2, kRowRight - 2, lineStrong, 0));
    CHECK(f1.near(kRowX + 1, kSepY + 4, menu, 0) && f1.near(kRowRight - 2, kSepY + 4, menu, 0));
    CHECK(f1.countInk(kRowX + 2, kSepY, kRowRight - 2, kSepY + 4, menu) == 0);      // 4px above
    CHECK(f1.countInk(kRowX + 2, kSepY + 5, kRowRight - 2, kSepY + 9, menu) == 0);  // 4px below
    CHECK(f1.rowNear(kSepY + 3, kRowX + 2, kRowRight - 2, menu, 0) && f1.rowNear(kSepY + 5, kRowX + 2, kRowRight - 2, menu, 0));   // 1px thick

    // --- the heading: the cap in Theme.muted, 4px of padding above and 10px at the left ---------------------------------------
    CHECK(f1.countNear(27, kHeadingY + 4, 140, kHeadingY + 18, muted, 12) >= 6);
    CHECK(f1.countInk(kRowX + 1, kHeadingY, kRowX + 9, kHeadingY + 21, menu) == 0);                  // 10px left padding
    CHECK(f1.countInk(kRowX + 1, kHeadingY, kRowRight - 1, kHeadingY + 4, menu) == 0);               // 4px above the cap band
    CHECK(f1.countInk(kRowX + 10, kHeadingY + 4, kRowX + 100, kHeadingY + 17, menu) > 25);           // ink inside the band
    const int firstHeadingX = firstInkX(f1, kRowX + 8, kHeadingY + 4, kRowX + 60, kHeadingY + 17, menu);
    CHECK(firstHeadingX >= kRowX + 10 && firstHeadingX <= kRowX + 11);
    CHECK(f1.countNear(kRowX + 10, kHeadingY + 4, kRowX + 100, kHeadingY + 21, fg, 8) == 0);          // Theme.muted, not Theme.fg
    // upper-case: "Display" would drop descenders (p, y) below the baseline of its first letter if it were left in mixed case
    const int headingBase = lastInkY(f1, kRowX + 10, kHeadingY + 4, kRowX + 16, kHeadingY + 21, menu);   // the bottom of the D
    CHECK(headingBase > kHeadingY + 4 && lastInkY(f1, kRowX + 10, kHeadingY + 4, kRowX + 100, kHeadingY + 21, menu) == headingBase);

    // --- the slider: 180px track 10px in, fill to the value, a 16px knob -----------------------------------------------
    const int trackX = kRowX + 10;                                                    // 27 .. 206
    const int trackY = kSliderY + 23;                                                 // 252 .. 257
    CHECK(f1.rowNear(trackY + 3, trackX + 4, trackX + 62, sliderFill, 0));               // the fill, left of the knob
    CHECK(f1.rowNear(trackY + 3, trackX + 84, trackX + 176, sliderTrack, 0));            // the bare track, right of it
    CHECK(f1.colNear(trackX + 30, trackY, trackY + 6, sliderFill, 0));                   // 6px thick (Theme.slider-track-h)
    CHECK(f1.near(trackX + 30, trackY - 1, menu, 0) && f1.near(trackX + 30, trackY + 6, menu, 0));
    CHECK(f1.colNear(trackX + 120, trackY, trackY + 6, sliderTrack, 0));
    CHECK(f1.near(trackX + 120, trackY - 1, menu, 0) && f1.near(trackX + 120, trackY + 6, menu, 0));
    CHECK(f1.near(trackX + 180 - 3, trackY + 3, sliderTrack, 0) && f1.near(trackX + 180 + 3, trackY + 3, menu, 0));   // 180px long
    CHECK(f1.near(trackX - 1, trackY + 3, menu, 0) && f1.near(trackX + 2, trackY + 3, sliderFill, 0));
    const int knobFirst = f1.firstColNear(trackY + 2, trackX + 50, trackX + 100, sliderKnob, 6);
    int knobLast = -1;
    for (int x = trackX + 100; x >= trackX + 50; --x) { if (f1.near(x, trackY + 2, sliderKnob, 6)) { knobLast = x; break; } }
    CHECK(knobFirst > 0 && knobLast > knobFirst);
    CHECK(knobLast - knobFirst >= 8 && knobLast - knobFirst <= 11);                   // 16px with its 2px ring: 12px of face (10 solid pixels)
    CHECK((knobFirst + knobLast) / 2.0 >= trackX + 72 - 1.6 && (knobFirst + knobLast) / 2.0 <= trackX + 72 + 1.6);   // centred on 40%
    CHECK(f1.near(trackX + 72 - 8, trackY + 2, sliderKnobRing, 12) || f1.near(trackX + 72 - 7, trackY + 2, sliderKnobRing, 12));
    CHECK(f1.countNear(trackX + 72 - 9, trackY - 8, trackX + 72 + 9, trackY + 14, sliderKnob, 6) >= 80);   // a round 12px face
    CHECK(f1.countInk(trackX + 72 - 12, trackY - 12, trackX + 72 + 12, trackY - 9, menu) == 0);              // never taller than 16
    // its caption row: label left (muted), mono readout right (muted)
    CHECK(f1.countNear(trackX, kSliderY + 4, trackX + 60, kSliderY + 18, muted, 12) >= 3);
    CHECK(f1.countNear(trackX, kSliderY + 4, kRowRight - 9, kSliderY + 18, fg, 8) == 0);   // the caption and the readout: Theme.muted, not Theme.fg
    CHECK(lastInkX(f1, trackX, kSliderY + 3, kRowRight, kSliderY + 19, menu) >= kRowRight - 13);
    CHECK(f1.countInk(trackX, kSliderY, kRowRight - 10, kSliderY + 2, menu) == 0);
    // disabled: Theme.slider-disabled-* and the caption in Theme.disabled
    const int offTrackY = kSliderOffY + 23;
    CHECK(f1.rowNear(offTrackY + 3, trackX + 4, trackX + 150, sliderOffFill, 0));        // 90% of the track
    CHECK(f1.near(trackX + 162, offTrackY + 2, sliderOffKnob, 6) || f1.near(trackX + 162, offTrackY + 6, sliderOffKnob, 6));
    CHECK(f1.rowNear(offTrackY + 3, trackX + 176, trackX + 178, sliderTrack, 0));
    CHECK(f1.countNear(trackX, kSliderOffY + 4, trackX + 70, kSliderOffY + 18, disabled, 12) >= 3);
    CHECK(f1.countNear(trackX, kSliderOffY + 4, trackX + 70, kSliderOffY + 18, muted, 6) == 0);
    CHECK(f1.countNear(trackX + 150, trackY - 8, trackX + 170, trackY + 14, sliderOffKnob, 6) == 0);

    // --- disabled text: Theme.disabled, never Theme.fg ------------------------------------------------------------
    CHECK(f1.countNear(kRowX + 30, kDisabledY + 4, kRowX + 130, kDisabledY + 22, disabled, 12) >= 4);
    CHECK(f1.countNear(kRowX + 30, kDisabledY + 4, kRowX + 130, kDisabledY + 22, fg, 12) == 0);
    CHECK(f1.countNear(kRowX + 30, kItemY + 4, kRowX + 130, kItemY + 22, fg, 12) >= 6);               // an enabled row: Theme.fg
    CHECK(f1.countNear(kRowX + 30, kItemY + 4, kRowX + 130, kItemY + 22, disabled, 4) == 0);

    // --- the standalone rows act: clicked, changed(float) ---------------------------------------------------------------------
    CHECK(ui->get_clicks() == 0 && ui->get_last_click() == -1 && ui->get_row_slider() < 0.0f);
    click(win, 200.0f, kItemY + 13.0f);
    CHECK(ui->get_clicks() == 1 && ui->get_last_click() == 1);
    click(win, 200.0f, kCheckedOnY + 13.0f);
    CHECK(ui->get_clicks() == 2 && ui->get_last_click() == 2);
    click(win, 200.0f, kRadioOffY + 13.0f);
    CHECK(ui->get_clicks() == 3 && ui->get_last_click() == 5);
    click(win, 200.0f, kSubmenuY + 13.0f);
    CHECK(ui->get_clicks() == 4 && ui->get_last_click() == 6);
    click(win, 200.0f, kHotY + 13.0f);
    CHECK(ui->get_clicks() == 5 && ui->get_last_click() == 9);
    click(win, 200.0f, kDisabledY + 13.0f);                                          // disabled: never acts
    click(win, 200.0f, kHeadingY + 10.0f);                                           // a heading and a separator are not buttons
    click(win, 200.0f, kSepY + 4.0f);
    CHECK(ui->get_clicks() == 5 && ui->get_last_click() == 9);
    click(win, static_cast<float>(trackX + 45), static_cast<float>(trackY + 3));     // a click on the track: 25%
    CHECK(std::fabs(ui->get_row_slider() - 0.25f) < 0.02f);
    press(win, static_cast<float>(trackX + 36), static_cast<float>(trackY + 3));     // press at 20%, drag to 80%
    moveTo(win, static_cast<float>(trackX + 90), static_cast<float>(trackY + 3));
    CHECK(std::fabs(ui->get_row_slider() - 0.5f) < 0.02f);
    moveTo(win, static_cast<float>(trackX + 144), static_cast<float>(trackY + 3));
    release(win, static_cast<float>(trackX + 144), static_cast<float>(trackY + 3));
    CHECK(std::fabs(ui->get_row_slider() - 0.8f) < 0.02f);
    click(win, static_cast<float>(trackX - 5), static_cast<float>(trackY + 3));      // left of the track, inside the knob's reach: clamps to 0
    CHECK(ui->get_row_slider() == 0.0f);
    click(win, static_cast<float>(trackX + 186), static_cast<float>(trackY + 3));     // and right of it: 1
    CHECK(ui->get_row_slider() == 1.0f);
    click(win, static_cast<float>(trackX + 45), static_cast<float>(offTrackY + 3));  // the disabled slider never acts
    CHECK(ui->get_row_slider() == 1.0f);
    away();

    // --- the tooltip: a rested pointer shows it 6px under the row ---------------------------------------------------------
    const auto tipShown = [&] { return render(ui, 600, 380).countNear(kRowX + 8, kItemY + 26 + 6, kRowX + 100, kItemY + 26 + 7, knobRing, 2) > 20; };   // its top border, on y 70
    CHECK(!tipShown());
    moveTo(win, 200.0f, kItemY + 13.0f);
    bool shown = false;
    for (int i = 0; i < 50 && !shown; ++i) { settle(std::chrono::milliseconds(100)); shown = tipShown(); }     // after about 600 ms of rest
    CHECK(shown);
    away();
    CHECK(!tipShown());                                                                                          // and it closes when the pointer leaves

    // ===== the Menu popup ===================================================================================
    const Rows rows;
    CHECK(rows.total == kPopH);
    ui->invoke_open_menu();
    away();
    const Frame p0 = render(ui, 600, 380);
    CHECK(p0.savePng(shotDir() + "menus-specimen.png"));
    // the panel: Theme.menu ground, Theme.knob-ring border, at the requested corner
    CHECK(p0.near(400, kPopY, knobRing, 0) && p0.near(400, kPopY - 1, bg, 0));                          // top edge on y 16
    CHECK(p0.near(400, kPopY + kPopH - 1, knobRing, 0) && p0.near(400, kPopY + kPopH, bg, 0));          // 320 high
    CHECK(p0.near(kPopX, 100, knobRing, 0) && p0.near(kPopX - 1, 100, bg, 0));                          // left edge on x 300
    CHECK(p0.near(kPopX + 2, 100, menu, 0) && p0.near(kPopX + 3, 300, menu, 0));                        // the ground inside
    CHECK(p0.near(kPopX, kPopY, bg, 0) && p0.near(kPopX + 1, kPopY + 1, bg, 0));                        // radius 8: the corner is cut
    int right = -1;
    for (int x = kPopX; x < 600; ++x) { if (p0.near(x, rows.top[7] + 4 - 2, knobRing, 2)) { right = x; } }   // the border on the separator's margin row
    const int panelW = right - kPopX + 1;
    CHECK(right > 0 && panelW >= 180 && panelW >= 210);                                           // min-width 180, and the 200px slider row needs 210
    CHECK(p0.near(kPopX + panelW, 100, bg, 0) && p0.near(kPopX + panelW - 1, 100, knobRing, 0));
    CHECK(p0.near(kPopX + panelW - 3, 300, menu, 0));
    // rows from the entries: the separator's rule, the slider's track, the radio's dot, at the heights the entry list implies
    const int sepRuleY = rows.top[7] + 4;
    CHECK(p0.rowNear(sepRuleY, kPopX + kInset + 2, kPopX + panelW - kInset - 2, lineStrong, 0));
    CHECK(p0.rowNear(sepRuleY - 1, kPopX + kInset + 2, kPopX + panelW - kInset - 2, menu, 0));
    CHECK(p0.rowNear(sepRuleY + 1, kPopX + kInset + 2, kPopX + panelW - kInset - 2, menu, 0));
    const int popTrackX = kPopX + kInset + 10;                                                    // 315 .. 494
    const int popTrackY = rows.top[8] + 23;
    CHECK(p0.rowNear(popTrackY + 3, popTrackX + 4, popTrackX + 62, sliderFill, 0));
    CHECK(p0.rowNear(popTrackY + 3, popTrackX + 84, popTrackX + 176, sliderTrack, 0));
    CHECK(p0.near(popTrackX + 72, popTrackY + 3, sliderKnob, 6) || p0.near(popTrackX + 72, popTrackY - 1, sliderKnob, 6));
    CHECK(p0.rowNear(rows.top[9] + 26, popTrackX + 4, popTrackX + 150, sliderOffFill, 0));           // the disabled Ceiling
    CHECK(p0.near(popTrackX + 162, rows.top[9] + 23 + 3, sliderOffKnob, 6) || p0.near(popTrackX + 162, rows.top[9] + 23 + 1, sliderOffKnob, 6));
    CHECK(p0.near(kPopX + kInset + 10 + 3 + 3, rows.top[4] + 13, accent, 0));                        // the radio dot of "Automatic"
    CHECK(p0.countNear(kPopX + kInset + 10, rows.top[2] + 7, kPopX + kInset + 22, rows.top[2] + 19, fg, 30) >= 4);   // the tick of "Peak hold"
    // the mark column: every item label starts 30px in (10 padding + the 20px column), the checkable rows' and the plain ones'
    int px[7];
    n = 0;
    for (const int i : {1, 2, 3, 4, 5, 6, 11}) { px[n++] = firstInkX(p0, kPopX + kInset + 25, rows.top[i] + 4, kPopX + kInset + 100, rows.top[i] + 22, menu); }
    for (int i = 0; i < 7; ++i) { CHECK(px[i] >= kPopX + kInset + 30 && px[i] <= kPopX + kInset + 32); CHECK(px[i] - px[0] >= -2 && px[i] - px[0] <= 2); }
    // the heading and the slider captions stay at 10px
    const int headingInk = firstInkX(p0, kPopX + kInset + 2, rows.top[0] + 4, kPopX + kInset + 60, rows.top[0] + 17, menu);
    CHECK(headingInk >= kPopX + kInset + 10 && headingInk <= kPopX + kInset + 11);
    const int captionInk = firstInkX(p0, kPopX + kInset + 2, rows.top[8] + 3, kPopX + kInset + 60, rows.top[8] + 19, menu);
    CHECK(captionInk >= kPopX + kInset + 10 && captionInk <= kPopX + kInset + 12);
    // the hint of the last row ends 10px from the panel's inner edge
    const int popHintEnd = lastInkX(p0, kPopX + kInset + 100, rows.top[11] + 4, kPopX + panelW - 2, rows.top[11] + 22, menu);
    CHECK(popHintEnd >= kPopX + panelW - kInset - 13 && popHintEnd <= kPopX + panelW - kInset - 10);
    // the chevron of the submenu row, 10px from the inner edge
    CHECK(p0.countInk(kPopX + panelW - kInset - 24, rows.top[6] + 6, kPopX + panelW - kInset - 10, rows.top[6] + 20, menu) >= 6);
    CHECK(p0.countInk(kPopX + panelW - kInset - 24, rows.top[1] + 6, kPopX + panelW - kInset - 10, rows.top[1] + 20, menu) == 0);

    // --- hover inside the panel: the row lights, item-hovered names it ---------------------------------------------------
    CHECK(ui->get_hovered_id() == -1);
    moveTo(win, static_cast<float>(kPopX + panelW - 30), rows.top[1] + 13.0f);
    const Frame ph = render(ui, 600, 380);
    CHECK(ph.near(kPopX + panelW - 30, rows.top[1] + 3, hot, 0));
    CHECK(ph.firstRowNear(kPopX + panelW - 30, rows.top[1] - 4, rows.top[1] + 40, hot, 0) == rows.top[1]);
    CHECK(ph.countNear(kPopX + panelW - 30, rows.top[1] - 4, kPopX + panelW - 29, rows.top[1] + 40, hot, 0) == 26);
    CHECK(ui->get_hovered_id() == 1);
    moveTo(win, static_cast<float>(kPopX + panelW - 30), rows.top[11] + 13.0f);
    CHECK(ui->get_hovered_id() == 11);
    moveTo(win, static_cast<float>(kPopX + panelW - 30), rows.top[10] + 13.0f);                // a disabled row: no light
    CHECK(!render(ui, 600, 380).near(kPopX + panelW - 30, rows.top[10] + 3, hot, 0));
    away();

    // --- activated(id): an enabled item fires it and the popup is gone on the next render ----------------------------
    CHECK(ui->get_activations() == 0 && ui->get_last_id() == -1);
    click(win, static_cast<float>(kPopX + panelW - 30), rows.top[1] + 13.0f);
    CHECK(ui->get_activations() == 1 && ui->get_last_id() == 1);
    away();
    const Frame gone = render(ui, 600, 380);
    CHECK(gone.countInk(kPopX - 2, kPopY - 2, 560, kPopY + kPopH + 4, bg) == 0);                 // not a pixel of the panel is left
    CHECK(gone.near(400, kPopY, bg, 0) && gone.near(400, kPopY + kPopH - 1, bg, 0));

    // --- a click on a disabled row fires nothing; the popup stays; so for the heading and the separator ---------------
    ui->invoke_open_menu();
    away();
    CHECK(render(ui, 600, 380).near(400, kPopY, knobRing, 0));
    click(win, static_cast<float>(kPopX + panelW - 30), rows.top[10] + 13.0f);                   // "Reset trace", disabled
    click(win, static_cast<float>(kPopX + panelW - 30), rows.top[0] + 10.0f);                    // the heading
    click(win, static_cast<float>(kPopX + panelW - 30), rows.top[7] + 4.0f);                     // the separator
    click(win, 420.0f, rows.top[9] + 8.0f);                                                       // the disabled slider's caption row
    click(win, static_cast<float>(popTrackX + 45), static_cast<float>(rows.top[9] + 26));         // ...and its track
    CHECK(ui->get_activations() == 1 && ui->get_last_id() == 1);
    CHECK(ui->get_slider_changes() == 0 && ui->get_submenu_requests() == 0);
    away();
    CHECK(render(ui, 600, 380).near(400, kPopY, knobRing, 0));                                       // still open

    // --- checked and radio rows activate and close ------------------------------------------------------------------------
    click(win, static_cast<float>(kPopX + panelW - 30), rows.top[2] + 13.0f);
    CHECK(ui->get_activations() == 2 && ui->get_last_id() == 2);
    away();
    CHECK(render(ui, 600, 380).near(400, kPopY, bg, 0));
    ui->invoke_open_menu();
    away();
    render(ui, 600, 380);
    click(win, static_cast<float>(kPopX + panelW - 30), rows.top[4] + 13.0f);
    CHECK(ui->get_activations() == 3 && ui->get_last_id() == 4);
    away();
    CHECK(render(ui, 600, 380).near(400, kPopY, bg, 0));                                             // a radio closes it by default
    // ...unless the host says radios keep it open (the Display block's behaviour)
    ui->set_keep_open_on_radio(true);
    ui->invoke_open_menu();
    away();
    render(ui, 600, 380);
    click(win, static_cast<float>(kPopX + panelW - 30), rows.top[5] + 13.0f);
    CHECK(ui->get_activations() == 4 && ui->get_last_id() == 5);
    away();
    CHECK(render(ui, 600, 380).near(400, kPopY, knobRing, 0));
    click(win, static_cast<float>(kPopX + panelW - 30), rows.top[11] + 13.0f);                   // an ordinary item still closes it
    CHECK(ui->get_activations() == 5 && ui->get_last_id() == 11);
    away();
    CHECK(render(ui, 600, 380).near(400, kPopY, bg, 0));
    ui->set_keep_open_on_radio(false);

    // --- slider-changed(id, float): a click inside the slider row, near the click fraction ---------------------------------
    ui->invoke_open_menu();
    away();
    render(ui, 600, 380);
    const float popTrackCentreY = static_cast<float>(rows.top[8] + 26);
    click(win, static_cast<float>(popTrackX + 135), popTrackCentreY);                              // 75% of 180
    CHECK(ui->get_slider_changes() == 1 && ui->get_last_slider_id() == 8);
    CHECK(std::fabs(ui->get_last_slider() - 0.75f) < 0.02f);
    CHECK(ui->get_activations() == 5);                                                             // a slider is not an activation
    away();
    CHECK(render(ui, 600, 380).near(400, kPopY, knobRing, 0));                                        // and does not close the menu
    // the knob follows a pressed pointer, then goes back to the entry's value (the host owns it)
    press(win, static_cast<float>(popTrackX + 36), popTrackCentreY);
    CHECK(std::fabs(ui->get_last_slider() - 0.2f) < 0.02f);
    const Frame drag1 = render(ui, 600, 380);
    CHECK(drag1.near(popTrackX + 36, popTrackCentreY, sliderKnob, 6));                             // the knob is under the pointer
    CHECK(drag1.near(popTrackX + 72, popTrackCentreY, sliderKnob, 6) == false);
    moveTo(win, static_cast<float>(popTrackX + 108), popTrackCentreY + 6.0f);                      // drag: the vertical drift does not matter
    CHECK(std::fabs(ui->get_last_slider() - 0.6f) < 0.02f);
    CHECK(ui->get_slider_changes() >= 3);
    release(win, static_cast<float>(popTrackX + 108), popTrackCentreY + 6.0f);
    away();
    const Frame drag2 = render(ui, 600, 380);
    CHECK(drag2.near(popTrackX + 72, popTrackCentreY, sliderKnob, 6));                             // back at 40%: the entry still says 0.4
    CHECK(ui->get_last_slider_id() == 8);
    const int changesBefore = ui->get_slider_changes();
    click(win, static_cast<float>(popTrackX - 6), popTrackCentreY);                                // left of the track
    CHECK(ui->get_last_slider() == 0.0f && ui->get_slider_changes() == changesBefore + 1);
    click(win, static_cast<float>(popTrackX + 185), popTrackCentreY);                              // right of it
    CHECK(ui->get_last_slider() == 1.0f);
    away();

    // --- submenu-requested(id, x, y): hover or click on the submenu row -----------------------------------------------------
    CHECK(ui->get_submenu_requests() == 0);
    moveTo(win, static_cast<float>(kPopX + panelW - 30), rows.top[6] + 13.0f);
    CHECK(ui->get_submenu_requests() >= 1 && ui->get_last_submenu_id() == 6);
    CHECK(std::fabs(ui->get_last_submenu_x() - static_cast<float>(kPopX + panelW)) < 1.5f);       // the panel's right edge
    CHECK(std::fabs(ui->get_last_submenu_y() - static_cast<float>(rows.top[6] - kInset)) < 1.5f);  // a panel whose first row lines up with this one
    const int afterHover = ui->get_submenu_requests();
    click(win, static_cast<float>(kPopX + panelW - 30), rows.top[6] + 13.0f);
    CHECK(ui->get_submenu_requests() > afterHover && ui->get_last_submenu_id() == 6);              // a click asks too
    CHECK(ui->get_activations() == 5);                                                             // ...but is not an activation
    away();
    CHECK(render(ui, 600, 380).near(400, kPopY, knobRing, 0));                                        // and the menu stays open for the submenu
    // the row whose submenu is open stays lit
    CHECK(!render(ui, 600, 380).near(kPopX + panelW - 30, rows.top[6] + 3, hot, 0));
    ui->set_open_submenu_id(6);
    const Frame lit = render(ui, 600, 380);
    CHECK(lit.near(kPopX + panelW - 30, rows.top[6] + 3, hot, 0));
    CHECK(lit.countNear(kPopX + panelW - 30, rows.top[6] - 3, kPopX + panelW - 29, rows.top[6] + 30, hot, 0) == 26);
    CHECK(!lit.near(kPopX + panelW - 30, rows.top[5] + 3, hot, 0));
    ui->set_open_submenu_id(-1);
    // moving to another row tells the host (to close its submenu)
    moveTo(win, static_cast<float>(kPopX + panelW - 30), rows.top[3] + 13.0f);
    CHECK(ui->get_hovered_id() == 3);
    ui->invoke_close_menu();
    away();
    CHECK(render(ui, 600, 380).near(400, kPopY, bg, 0));                                              // close() works from the host too

    // --- clicking outside closes it without activating anything ---------------------------------------------------------------
    ui->invoke_open_menu();
    away();
    render(ui, 600, 380);
    click(win, 560.0f, 340.0f);
    CHECK(ui->get_activations() == 5);
    CHECK(render(ui, 600, 380).near(400, kPopY, bg, 0));

    // --- at-x / at-y: where the panel opens, in the frame of the Menu's parent, wherever the Menu itself sits ----------------
    ui->set_menu_x(350.0f);
    ui->set_menu_y(41.0f);
    ui->invoke_open_menu();
    away();
    const Frame moved = render(ui, 600, 380);
    CHECK(moved.near(450, 41, knobRing, 0) && moved.near(450, 40, bg, 0));                        // top edge on y 41
    CHECK(moved.near(350, 100, knobRing, 0) && moved.near(349, 100, bg, 0));                      // left edge on x 350
    CHECK(moved.near(450, 41 + kPopH - 1, knobRing, 0) && moved.near(450, 41 + kPopH, bg, 0));
    CHECK(moved.countInk(kPopX, kPopY, 349, 40, bg) == 0);                                  // nothing left at the old place
    ui->invoke_close_menu();
    ui->set_menu_x(300.0f);
    ui->set_menu_y(16.0f);
    away();
    CHECK(render(ui, 600, 380).near(450, 41, bg, 0));

    // --- the entries are the host's: a disabled submenu never asks, and a menu with no marks has no mark column ------------------
    {
        auto model = std::make_shared<slint::VectorModel<MenuEntry>>(std::vector<MenuEntry>{
            entry(MenuItemKind::Heading, 20, "Counter"),
            entry(MenuItemKind::Item, 21, "Enlarge figures"),
            entry(MenuItemKind::Submenu, 22, "Counter face", "", false, false),
            entry(MenuItemKind::Separator, 23, ""),
            entry(MenuItemKind::Item, 24, "Normal size", "Ctrl 0"),
            entry(MenuItemKind::Submenu, 25, "Clear all markers", "3"),
        });
        ui->set_entries(model);
        ui->invoke_open_menu();
        away();
        const Frame q = render(ui, 600, 380);
        const int h2 = 21 + 26 + 26 + 9 + 26 + 26 + 2 * kInset;                                      // 144
        CHECK(q.near(400, kPopY + h2 - 1, knobRing, 0) && q.near(400, kPopY + h2, bg, 0));                 // the panel is as high as its rows
        // no checkable row: the labels start at the 10px padding, not 30
        const int first = firstInkX(q, kPopX + kInset + 2, kPopY + kInset + 21 + 4, kPopX + kInset + 100, kPopY + kInset + 21 + 22, menu);
        CHECK(first >= kPopX + kInset + 10 && first <= kPopX + kInset + 12);
        // the disabled submenu: its chevron is drawn, its row neither lights nor asks
        const int subTop = kPopY + kInset + 21 + 26;
        CHECK(q.countInk(kPopX + 180 - kInset - 24, subTop + 6, kPopX + 180 - kInset - 10, subTop + 20, menu) >= 6);       // the chevron is drawn all the same
        CHECK(q.near(kPopX + 180, kPopY + 40, bg, 0) && q.near(kPopX + 179, kPopY + 40, knobRing, 0));                          // a 180px panel
        const int before = ui->get_submenu_requests();
        moveTo(win, static_cast<float>(kPopX + 100), subTop + 13.0f);
        click(win, static_cast<float>(kPopX + 100), subTop + 13.0f);
        CHECK(ui->get_submenu_requests() == before);
        CHECK(!render(ui, 600, 380).near(kPopX + 100, subTop + 3, hot, 0));
        CHECK(ui->get_hovered_id() == 22);                                                           // it still reports the hover
        // a submenu row with a hint: the hint sits left of the chevron, 6px clear of it, the chevron 10px from the edge
        const int hintRowTop = kPopY + kInset + 21 + 26 + 26 + 9 + 26;
        const int hintInkEnd = lastInkX(q, kPopX + 120, hintRowTop + 4, kPopX + 180 - kInset - 24, hintRowTop + 22, menu);
        CHECK(hintInkEnd >= kPopX + 180 - kInset - 10 - 14 - 6 - 4 && hintInkEnd <= kPopX + 180 - kInset - 10 - 14 - 6 + 1);
        CHECK(q.countInk(kPopX + 180 - kInset - 24, hintRowTop + 6, kPopX + 180 - kInset - 10, hintRowTop + 20, menu) >= 6);
        const int askedBefore = ui->get_submenu_requests();
        moveTo(win, static_cast<float>(kPopX + 100), hintRowTop + 13.0f);
        CHECK(ui->get_submenu_requests() == askedBefore + 1 && ui->get_last_submenu_id() == 25);       // an enabled one asks on hover
        CHECK(std::fabs(ui->get_last_submenu_y() - static_cast<float>(hintRowTop - kInset)) < 1.5f);
        // a click on "Normal size" activates id 24
        click(win, static_cast<float>(kPopX + 100), kPopY + kInset + 21 + 26 + 26 + 9 + 13.0f);
        CHECK(ui->get_last_id() == 24 && ui->get_activations() == 6);
        away();
        // a one-word menu is exactly the minimum 180px wide
        ui->set_entries(std::make_shared<slint::VectorModel<MenuEntry>>(std::vector<MenuEntry>{entry(MenuItemKind::Item, 30, "A")}));
        ui->invoke_open_menu();
        const Frame one = render(ui, 600, 380);
        int r1 = -1;
        for (int x = kPopX; x < 600; ++x) { if (one.near(x, kPopY + 18, knobRing, 2)) { r1 = x; } }
        CHECK(r1 - kPopX + 1 == 180);
        CHECK(one.near(kPopX + 40, kPopY + 26 + 2 * kInset - 1, knobRing, 0) && one.near(kPopX + 40, kPopY + 26 + 2 * kInset, bg, 0));   // 36 high
        ui->invoke_close_menu();
    }

    return testSummary("test_slint_menus");
}
