// The slider family (Slider, LevelBar, SliderGroup) measured against the artboards' CSS (Workbench.dc.html /
// Workbench-PluginKit.dc.html: .sl, .sl i, .sl b, .sl.dis and the gain / squelch / volume lanes of the receiver strip),
// and driven the way the OS backend would drive it: click, drag, wheel, keys, hover.
//
// Every position below is the specimen's own table (ui/specimens/sliders_specimen.slint). The knob is 16px across IN
// THE KIT'S border-box terms: a 2px ring (slider-knob-ring) round 12px of knob colour, so "16px across" is measured as
// ring + knob + ring on the knob's centre row, and the knob colour alone is the 12px in the middle.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "sliders_specimen.h"

#include <cmath>
#include <cstdio>

using namespace foxsdr::slinttest;

namespace {

// The Workbench filling's colours, as the sheets' CSS names them.
constexpr Rgb kBg = rgb(0x15171b);
constexpr Rgb kStrip = rgb(0x191c21);
constexpr Rgb kTrack = rgb(0x2b2f37);
constexpr Rgb kFill = rgb(0xf5a524);
constexpr Rgb kKnob = rgb(0xe6e8ec);
constexpr Rgb kRing = rgb(0x1d2026);
constexpr Rgb kDisFill = rgb(0x5a606b);
constexpr Rgb kDisKnob = rgb(0x8a93a0);
constexpr Rgb kLevel = rgb(0x3ddc84);
constexpr Rgb kAccent = rgb(0xf5a524);
constexpr Rgb kKeyBg = rgb(0x262a32);
constexpr Rgb kKeyLine = rgb(0x363b45);
constexpr Rgb kKeyDisBg = rgb(0x1d2026);
constexpr Rgb kTip = rgb(0x262a32);      // Theme.menu: the tooltip's ground
constexpr Rgb kFocus = rgb(0x7ad7ff);
constexpr int kTight = 3;                // tolerance that still tells the ring from the ground

// The specimen's sliders: x, track top, width. The knob's centre is x + value * width.
constexpr int kX = 40, kW = 200;
constexpr int kYA = 44, kYB = 94, kYC = 144, kYD = 194, kYE = 244;
inline int cx(double value) { return kX + static_cast<int>(std::lround(value * kW)); }

// The 12px of knob colour on one row near the knob's centre. The two pixels where the disc meets its ring are blended
// (about 90 % knob), so the count takes a tolerance that includes them: 10 exact pixels + 2 blended = the 12px disc.
int knobRun(const Frame& f, int centreX, int y, Rgb knob) { return f.countNear(centreX - 14, y, centreX + 14, y + 1, knob, 30); }
// The same, down the knob's vertical axis (blended rows are ~65 % knob, hence the wider tolerance).
int knobCol(const Frame& f, int centreX, int y0, int y1, Rgb knob) { return f.countNear(centreX, y0, centreX + 1, y1, knob, 60); }

bool near2(const Frame& f, int x, int y, Rgb want, int tol) { return f.near(x, y, want, tol); }

// A tooltip's ground is ONE flat colour, so it is counted exactly: anti-aliased text pixels on a nearby label can land
// within a loose tolerance of it, but not within 2.
int tipPixels(const Frame& f, int x0, int y0, int x1, int y1) { return f.countNear(x0, y0, x1, y1, kTip, 2); }

// Pixels where the w x h rectangles at (ax, ay) and (bx, by) differ by more than tol in any channel.
int differingRects(const Frame& f, int ax, int ay, int bx, int by, int w, int h, int tol = 8) {
    int n = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const Rgb p = f.at(ax + x, ay + y), q = f.at(bx + x, by + y);
            if (std::abs(int(p.r) - int(q.r)) > tol || std::abs(int(p.g) - int(q.g)) > tol || std::abs(int(p.b) - int(q.b)) > tol) { ++n; }
        }
    }
    return n;
}

slint::Color col(unsigned r, unsigned g, unsigned b) {
    return slint::Color::from_rgb_uint8(static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b));
}

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = SlidersSpecimen::create();
    slint::Window& w = ui->window();

    Frame f = render(ui, 780, 450);
    CHECK(f.width == 780 && f.height == 450);
    CHECK(f.near(770, 5, kBg));                          // the sheet's ground
    CHECK(f.near(770, 300, kStrip));                     // the strip ground under the lanes

    // ===== Slider: the track ================================================================================
    // 6px high in slider-track beyond the fill: the column at x = 220 is past slider A's knob (164 +/- 8).
    CHECK(f.countNear(220, 30, 221, 70, kTrack) == 6);
    CHECK(f.firstRowNear(220, 30, 70, kTrack) == kYA);
    CHECK(f.near(220, kYA - 1, kBg) && f.near(220, kYA + 6, kBg));      // and the ground right outside it
    CHECK(f.countNear(220, 120, 221, 170, kTrack) == 6);               // slider C (value 0): the same track
    CHECK(f.rowNear(kYA + 3, 190, 238, kTrack));                        // runs on to the right end ...
    CHECK(f.near(kX + kW - 2, kYA + 3, kTrack) && f.near(kX + kW + 1, kYA + 3, kBg));   // ... and stops at 240

    // ===== Slider: the fill, from the left end to the value ==================================================
    CHECK(f.near(kX + 2, kYA + 3, kFill));                              // starts at the left end of the track
    CHECK(f.near(kX - 2, kYA + 3, kBg));                                // and not before it
    CHECK(f.countNear(100, 30, 101, 70, kFill) == 6);                   // 6px high as well
    CHECK(f.near(cx(0.62) - 14, kYA + 3, kFill));                       // up to the knob ...
    CHECK(f.near(cx(0.62) + 14, kYA + 3, kTrack));                      // ... and the track after it
    CHECK(f.near(cx(0.29) - 14, kYB + 3, kFill) && f.near(cx(0.29) + 14, kYB + 3, kTrack));   // 0.29 on slider B
    CHECK(f.near(kX + 100, kYC + 3, kTrack) && f.near(kX + 14, kYC + 3, kTrack));               // value 0: no fill at all (the knob covers the first 8px)
    CHECK(f.near(kX + kW - 14, kYD + 3, kFill) && f.near(kX + 4, kYD + 3, kFill));              // value 1: all fill
    // the fill's width follows the value: the visible fill on B (0.29) is shorter than on A (0.62) by 33 % of 200px
    int fillA = 0, fillB = 0;
    for (int x = kX; x < kX + kW; ++x) {
        if (f.near(x, kYA + 3, kFill)) { ++fillA; }
        if (f.near(x, kYB + 3, kFill)) { ++fillB; }
    }
    CHECK(fillA > 100 && fillA <= 124);          // 124 = value x width, less the knob that covers its end
    CHECK(fillB > 40 && fillB <= 58);
    CHECK(fillA - fillB >= 60 && fillA - fillB <= 70);

    // ===== Slider: the knob, 16px across (ring 2 + knob 12 + ring 2), centred at value x width ===============
    for (const double v : {0.62, 0.29, 0.0, 1.0}) {
        const int y = v == 0.62 ? kYA : v == 0.29 ? kYB : v == 0.0 ? kYC : kYD;
        const int c = cx(v);
        const int row = y + 2;                                          // the knob's centre sits between rows y+2 and y+3
        CHECK(knobRun(f, c, row, kKnob) == 12);                                             // 12px of knob colour ...
        CHECK(knobRun(f, c, y + 3, kKnob) == 12);
        CHECK(near2(f, c - 7, row, kRing, kTight) && near2(f, c + 6, row, kRing, kTight));  // ... inside a 2px ring each side:
        CHECK(near2(f, c + 7, row, kRing, 6));                                              // 2 + 12 + 2 = 16 across, centred on c
        CHECK(!near2(f, c - 9, row, kRing, 6) && !near2(f, c + 8, row, kRing, 6));          // and nothing of the ring beyond that
        CHECK(f.countNear(c - 12, row, c - 9, row + 1, kKnob, 30) == 0 && f.countNear(c + 8, row, c + 12, row + 1, kKnob, 30) == 0);
        // vertically: 12 rows of knob colour, the ring above and below, 16 in all, centred on the track
        CHECK(knobCol(f, c, y - 20, y + 26, kKnob) == 12);
        CHECK(near2(f, c, y + 3 - 7, kRing, kTight) && near2(f, c, y + 3 + 6, kRing, kTight));
        CHECK(near2(f, c, y + 3 - 8, kRing, 6) && near2(f, c, y + 3 + 7, kRing, 6));
        CHECK(!near2(f, c, y + 3 - 9, kRing, kTight) && !near2(f, c, y + 3 + 8, kRing, kTight));
    }
    // C and D: the knob overhangs the track by 8px at the end it sits on
    CHECK(f.near(kX - 7, kYC + 2, kRing, kTight) && f.near(kX - 10, kYC + 2, kBg));
    CHECK(f.near(kX + kW + 6, kYD + 2, kRing, kTight) && f.near(kX + kW + 10, kYD + 2, kBg));

    // ===== Slider: disabled ==================================================================================
    CHECK(f.near(kX + 4, kYE + 3, kDisFill) && f.near(cx(0.62) - 14, kYE + 3, kDisFill));    // the .sl.dis fill
    CHECK(f.near(cx(0.62) + 14, kYE + 3, kTrack));
    CHECK(knobRun(f, cx(0.62), kYE + 2, kDisKnob) == 12);                // the .sl.dis knob
    CHECK(knobRun(f, cx(0.62), kYE + 2, kKnob) == 0);                    // and none of the enabled knob colour
    CHECK(f.countNear(cx(0.62) - 40, kYE - 12, cx(0.62) + 40, kYE + 18, kFill) == 0);   // no amber anywhere on it
    // F, bound to the host's 0.40: the same slider as the others, at x 300
    CHECK(knobRun(f, 300 + 80, kYE + 2, kKnob) == 12 && f.near(300 + 80 - 14, kYE + 3, kFill) && f.near(300 + 80 + 14, kYE + 3, kTrack));

    // ===== LevelBar: 4px, slider-track ground, level-bar fill to the value ===================================
    CHECK(f.countNear(480, 30, 481, 70, kTrack) == 4);                  // 4px high (not the slider's 6)
    CHECK(f.firstRowNear(480, 30, 70, kTrack) == 44);
    CHECK(f.countNear(350, 30, 351, 70, kLevel) == 4);                  // the fill is 4px too (the sheet's 6px fill overhangs: a CSS slip)
    CHECK(f.near(302, 45, kLevel) && f.near(300 + 110 - 4, 45, kLevel));   // fill from the left end to 0.55 x 200 = 110
    CHECK(f.near(300 + 110 + 4, 45, kTrack) && f.near(495, 45, kTrack));
    CHECK(f.near(300 - 2, 45, kBg) && f.near(300 + 200 + 1, 45, kBg));  // the bar is exactly 200 wide
    CHECK(f.near(310, 95, kTrack) && f.countNear(300, 94, 500, 98, kLevel) == 0);      // value 0: all track
    CHECK(f.near(495, 145, kLevel) && f.near(305, 145, kLevel));        // value 1: all fill
    CHECK(f.countNear(300, 144, 500, 148, kTrack) == 0);

    // ===== SliderGroup: the three lanes of the receiver strip ================================================
    // Gain (24, 296, 232 wide): caption row 20px, 7px gap, slider track at y 323, 7px gap, hint at y 336.
    const int g1x = 24, g1y = 296, g2x = 274, g3x = 524, g4y = 376;
    CHECK(f.countInk(g1x, g1y + 2, g1x + 56, g1y + 18, kStrip) > 20);               // the caption (cap, upper-case) ...
    CHECK(f.countInk(g1x + 70, g1y + 2, g1x + 126, g1y + 18, kStrip) == 0);         // ... and nothing between it and the readout
    CHECK(f.countInk(150, g1y + 2, 203, g1y + 18, kStrip) > 20);                    // the readout "32 dB"
    CHECK(f.countInk(g1x, 336, g1x + 232, 349, kStrip) > 60);                        // the hint line, below the slider
    CHECK(f.countInk(g1x, 331, g1x + 120, 336, kStrip) == 0);                        // with clear air between slider and hint
    CHECK(f.countNear(240, 318, 241, 336, kTrack) == 6);                             // the slider track: 6px at y 323
    CHECK(f.firstRowNear(240, 318, 336, kTrack) == 323);
    CHECK(f.near(100, 326, kFill) && f.near(g1x + 232 - 2, 326, kTrack));
    {
        const int k = static_cast<int>(std::lround(g1x + 0.62 * 232));
        CHECK(knobRun(f, k, 325, kKnob) >= 11 && knobRun(f, k, 325, kKnob) <= 13);   // the lane's knob at 0.62 (off the pixel grid, so +/- 1)
    }
    // the AUTO key: 20px high, 48 wide, right-aligned in the lane; key-bg ground, key-line border when off
    CHECK(f.near(212, 306, kKeyBg) && f.near(245, 306, kKeyBg));
    CHECK(f.near(208, 306, kKeyLine) && f.near(255, 306, kKeyLine) && f.near(207, 306, kStrip) && f.near(256, 306, kStrip));
    CHECK(f.near(232, 296, kKeyLine) && f.near(232, 315, kKeyLine) && f.near(232, 295, kStrip) && f.near(232, 316, kStrip));
    CHECK(f.countInk(212, 299, 252, 313, kKeyBg) > 12);                              // the word AUTO is drawn on it
    // Squelch: AUTO lit - accent ground, accent border
    CHECK(f.near(g2x + 188, 306, kAccent) && f.near(g2x + 232 - 48 + 4, 300, kAccent) && f.near(g2x + 232 - 48, 306, kAccent));
    CHECK(f.countInk(g2x + 188 + 2, 299, g2x + 228, 313, kAccent) > 12);             // the word, in accent-ink, on the accent ground
    CHECK(f.countInk(g2x, g1y + 2, g2x + 70, g1y + 18, kStrip) > 20);                // "SQUELCH"
    CHECK(f.countInk(g2x, 336, g2x + 232, 349, kStrip) > 60);                         // "Automatic: 8 dB above the noise floor"
    CHECK(f.near(300, 326, kFill) && f.near(380, 326, kTrack));                      // 0.29 of the lane
    // Volume: no AUTO key, the readout at the right end, the slider one row higher (16px caption row, track at y 319)
    CHECK(f.countInk(g3x + 70, g1y + 2, g3x + 200, g1y + 18, kStrip) == 0);          // where the AUTO key would be: nothing
    CHECK(f.countInk(g3x + 232 - 40, g1y + 2, g3x + 233, g1y + 18, kStrip) > 12);     // "0.72" at the right
    CHECK(f.countNear(740, 312, 741, 332, kTrack) == 6);
    CHECK(f.firstRowNear(740, 312, 332, kTrack) == 319);
    CHECK(f.near(600, 322, kFill) && f.near(750, 322, kTrack));                      // 0.72 of the lane
    // its Level row: the 4px LevelBar at 0.55, with the label and the figure beside it
    CHECK(f.countNear(600, 330, 601, 346, kLevel) == 4 && f.near(660, 337, kTrack));
    CHECK(f.countInk(524, 332, 556, 345, kStrip) > 8 && f.countInk(706, 332, 757, 345, kStrip) > 20);
    // the disabled lane: slider in the disabled colours, AUTO disabled (key-disabled-bg, half-alpha border)
    CHECK(f.near(100, 406, kDisFill) && f.near(240, 406, kTrack));
    CHECK(knobRun(f, static_cast<int>(std::lround(g1x + 0.62 * 232)), 405, kDisKnob) >= 11);
    CHECK(f.near(212, g4y + 10, kKeyDisBg) && f.near(245, g4y + 10, kKeyDisBg));
    CHECK(!f.near(208, g4y + 10, kKeyLine, kTight) && f.near(208, g4y + 10, over(kKeyLine, 0.5, kKeyDisBg), 6));   // half-alpha border
    CHECK(f.countInk(212, g4y + 3, 252, g4y + 17, kKeyDisBg) > 12);                  // AUTO still legible in key-disabled-fg
    CHECK(f.countNear(212, g4y + 3, 252, g4y + 17, rgb(0x8a93a0), 20) > 6);
    CHECK(f.countInk(g1x, 416, g1x + 232, 429, kStrip) > 40);                        // its hint
    CHECK(f.countNear(g1x + 100, 395, g1x + 101, 420, kAccent) == 0);                // nothing lit anywhere on it
    // 34 languages: "AUTOMATIQUE" on the key of the fifth lane (274, 376). The key grows to its word instead of cutting it,
    // stays right-aligned on the lane's edge (x 506), and the readout makes room on its left.
    {
        const int ky = 376 + 10;
        int left = -1;
        for (int x = 380; x < 505 && left < 0; ++x) { if (f.near(x, ky, kKeyLine, kTight) && f.near(x + 1, ky, kKeyBg, kTight)) { left = x; } }
        CHECK(left > 0);                                                              // the key's left border was found
        CHECK(506 - left > 60 && 506 - left <= 110);                                  // wider than the English key's 48, but a word's width
        CHECK(f.near(505, ky, kKeyLine, kTight) && f.near(506, ky, kStrip));          // and it still ends on the lane's right edge
        CHECK(f.countInk(left + 8, 379, 498, 393, kKeyBg) > 40);                      // all eleven letters are on the key
        CHECK(f.countInk(left - 56, 378, left - 6, 394, kStrip) > 15);                // the readout "-85 dB" moved left to make room
        CHECK(f.countInk(left - 5, 378, left, 394, kStrip) == 0);                     // with 6px of air before the key
        CHECK(f.countInk(274, 378, 340, 394, kStrip) > 20);                           // the caption is still there
    }
    // the caption is upper-cased by the component: "Gain" (the disabled lane, x 24) and "GAIN" (the sixth lane, x 524) are the
    // same row of pixels, whatever typeface stands in for Inter
    CHECK(differingRects(f, 24, 376, 524, 376, 80, 20) == 0);
    CHECK(f.countInk(24, 378, 104, 394, kStrip) > 20);                                // (and it is ink, not two empty rows)
    f.savePng(shotDir() + "sliders-specimen.png");

    // ===== interaction: slider B (0.29), the one the test drives ============================================
    int n0 = ui->get_changes();
    moveTo(w, 600, 20);                                   // keep the pointer clear of every tooltip
    const int yB = kYB + 3;

    // a click at 75 % of the track: changed(~0.75), and the knob has moved there
    click(w, kX + 150, yB);
    CHECK(ui->get_changes() == n0 + 1);
    CHECK_NEAR(ui->get_last_value(), 0.75, 0.03);
    Frame g = render(ui, 780, 450);
    CHECK(knobRun(g, cx(0.75), kYB + 2, kKnob) == 12);    // the knob is at 190
    CHECK(g.near(cx(0.29), kYB + 3, kFill));              // where it was, there is fill now
    CHECK(g.near(cx(0.75) - 14, kYB + 3, kFill) && g.near(cx(0.75) + 14, kYB + 3, kTrack));
    CHECK(g.countNear(cx(0.75) - 12, kYB - 14, cx(0.75) + 12, kYB + 20, kFocus) == 0);   // a click leaves no focus ring
    // the same click again changes nothing, so it reports nothing
    n0 = ui->get_changes();
    click(w, kX + 150, yB);
    CHECK(ui->get_changes() == n0);
    // a press on the knob's overhang past either end lands on the end of the track
    click(w, kX - 5, yB);
    CHECK(ui->get_changes() == n0 + 1);
    CHECK_NEAR(ui->get_last_value(), 0.0, 0.001);
    click(w, kX + kW + 5, yB);
    CHECK(ui->get_changes() == n0 + 2);
    CHECK_NEAR(ui->get_last_value(), 1.0, 0.001);
    click(w, kX + 150, yB);                               // and back to 0.75
    CHECK_NEAR(ui->get_last_value(), 0.75, 0.03);

    // a press at 20 %, moved to 60 %, released: changed fires with rising values and ends near 0.6
    ui->set_last_value(0.0f);
    const int back0 = ui->get_backsteps();
    n0 = ui->get_changes();
    moveTo(w, kX + 40, yB);
    press(w, kX + 40, yB);
    CHECK_NEAR(ui->get_last_value(), 0.20, 0.03);         // the press itself moves the knob there
    float prev = ui->get_last_value();
    bool rising = true;
    for (const int dx : {60, 80, 100, 120}) {
        moveTo(w, kX + dx, yB);
        const float v = ui->get_last_value();
        rising = rising && v > prev;
        CHECK_NEAR(v, dx / 200.0, 0.03);                  // the knob follows the pointer
        prev = v;
    }
    CHECK(rising);
    Frame mid = render(ui, 780, 450);
    CHECK(knobRun(mid, cx(0.6), kYB + 2, kKnob) == 12);   // while held, the picture follows too
    release(w, kX + 120, yB);
    CHECK_NEAR(ui->get_last_value(), 0.60, 0.03);
    CHECK(ui->get_changes() >= n0 + 4);
    CHECK(ui->get_backsteps() == back0);                  // never a lower value on the way up
    const int afterDrag = ui->get_changes();
    moveTo(w, kX + 30, yB);                               // released: the pointer no longer drags
    CHECK(ui->get_changes() == afterDrag);
    moveTo(w, kX + 120, yB);
    const float held = ui->get_last_value();

    // one wheel notch (60px of delta) = 5 % of the range; up raises, down lowers
    scroll(w, kX + 120, yB, 0, 60);
    CHECK_NEAR(ui->get_last_value() - held, 0.05, 0.002);
    scroll(w, kX + 120, yB, 0, -60);
    CHECK_NEAR(ui->get_last_value() - held, 0.0, 0.002);
    scroll(w, kX + 120, yB, 0, -60);
    CHECK_NEAR(held - ui->get_last_value(), 0.05, 0.002);
    scroll(w, kX + 120, yB, 0, 60);
    CHECK_NEAR(ui->get_last_value(), held, 0.002);
    const int beforeSmall = ui->get_changes();
    scroll(w, kX + 120, yB, 0, 6);                        // a touchpad's 6px is a tenth of a notch
    CHECK_NEAR(ui->get_last_value() - held, 0.005, 0.0015);
    CHECK(ui->get_changes() == beforeSmall + 1);
    scroll(w, kX + 120, yB, 0, -6);
    const float rest = ui->get_last_value();

    // Left / Right when focused (the press above focused it); a focus ring shows once the keyboard is in use
    CHECK(g.countNear(cx(0.75) - 14, kYB - 16, cx(0.75) + 14, kYB + 22, kFocus) == 0);
    keyTap(w, slint::platform::key_codes::RightArrow);
    CHECK_NEAR(ui->get_last_value() - rest, 0.05, 0.002);
    keyTap(w, slint::platform::key_codes::LeftArrow);
    keyTap(w, slint::platform::key_codes::LeftArrow);
    CHECK_NEAR(rest - ui->get_last_value(), 0.05, 0.002);
    const int beforeOther = ui->get_changes();
    keyTap(w, u8"a");                                     // any other key does nothing
    CHECK(ui->get_changes() == beforeOther);
    Frame keyed = render(ui, 780, 450);
    {
        const double v = ui->get_last_value();
        const int c = cx(v);
        CHECK(keyed.countNear(c - 13, kYB + 2, c + 13, kYB + 3, kFocus, 20) >= 3);      // the ring round the knob (22px across, 2px thick)
        CHECK(near2(keyed, c - 10, kYB + 2, kFocus, 8) && near2(keyed, c + 9, kYB + 2, kFocus, 8));   // symmetric: 11px out each side
        CHECK(keyed.countNear(c, kYB - 10, c + 1, kYB + 16, kFocus, 60) >= 4);          // ... above and below it too (2 rows each)
        CHECK(knobRun(keyed, c, kYB + 2, kKnob) == 12);                                 // the knob itself unchanged
        CHECK(near2(keyed, c - 7, kYB + 2, kRing, kTight) && near2(keyed, c + 6, kYB + 2, kRing, kTight));
    }
    // a click takes the ring away again (pointer focus)
    click(w, kX + 150, yB);
    Frame clicked = render(ui, 780, 450);
    CHECK(clicked.countNear(cx(0.75) - 14, kYB - 16, cx(0.75) + 14, kYB + 22, kFocus) == 0);
    moveTo(w, 600, 20);

    // ===== a disabled slider never acts ======================================================================
    n0 = ui->get_changes();
    const float lastBefore = ui->get_last_value();
    click(w, kX + 20, kYE + 3);
    scroll(w, kX + 100, kYE + 3, 0, 60);
    scroll(w, kX + 100, kYE + 3, 0, -60);
    press(w, kX + 100, kYE + 3);
    moveTo(w, kX + 160, kYE + 3);
    release(w, kX + 160, kYE + 3);
    CHECK(ui->get_changes() == n0);
    CHECK_NEAR(ui->get_last_value(), lastBefore, 0.0001);
    moveTo(w, 600, 20);
    Frame dis = render(ui, 780, 450);
    CHECK(knobRun(dis, cx(0.62), kYE + 2, kDisKnob) == 12);                      // still where it was, still disabled
    CHECK(dis.near(cx(0.62) - 14, kYE + 3, kDisFill));

    // ===== SliderGroup: its slider, its AUTO key ==============================================================
    n0 = ui->get_changes();
    click(w, g1x + 116, 326);                              // 50 % of the Gain lane's slider
    CHECK(ui->get_changes() == n0 + 1);
    CHECK_NEAR(ui->get_last_value(), 0.5, 0.03);
    moveTo(w, 600, 20);
    Frame lane = render(ui, 780, 450);
    CHECK(knobRun(lane, g1x + 116, 325, kKnob) == 12);     // the lane's knob moved
    CHECK(lane.near(g1x + 116 - 14, 326, kFill) && lane.near(g1x + 116 + 14, 326, kTrack));
    CHECK(lane.near(212, 306, kKeyBg));                    // AUTO not disturbed by it

    // AUTO: a click on the key fires auto-clicked and only that; the slider keeps its value
    n0 = ui->get_changes();
    int a0 = ui->get_auto_clicks();
    click(w, 232, 306);
    CHECK(ui->get_auto_clicks() == a0 + 1 && ui->get_last_auto() == 1);
    click(w, g2x + 232 - 24, 306);
    CHECK(ui->get_auto_clicks() == a0 + 2 && ui->get_last_auto() == 2);
    CHECK(ui->get_changes() == n0);                        // no changed() from an AUTO click
    click(w, 232, 336);                                    // below the key, on the hint line: not AUTO
    click(w, g1x + 20, 306);                               // on the caption: not AUTO
    CHECK(ui->get_auto_clicks() == a0 + 2);
    // the AUTO key of a lane whose auto-enabled is false never fires
    a0 = ui->get_auto_clicks();
    click(w, 232, g4y + 10);
    click(w, 214, g4y + 5);
    click(w, 250, g4y + 14);
    CHECK(ui->get_auto_clicks() == a0 && ui->get_last_auto() == 2);
    // the disabled lane's slider never acts either
    n0 = ui->get_changes();
    click(w, 100, 406);
    scroll(w, 100, 406, 0, 60);
    CHECK(ui->get_changes() == n0);
    // a hovered AUTO key takes menu-hot (when off): the strip's key hover look
    moveTo(w, 232, 306);
    Frame hot = render(ui, 780, 450);
    CHECK(hot.near(212, 306, rgb(0x363b45)));
    moveTo(w, 600, 20);

    // ===== the host stays in charge: bound one-way, a slider follows its host again after the user has moved it =====
    // main.slint binds its sliders this way (`value: root.gain-norm`). A slider that assigned `value` itself would lose that
    // binding at the first move and never follow the engine again.
    {
        moveTo(w, 600, 20);
        const int fx = 300, fyF = 244, yF = fyF + 3;
        const auto fcx = [&](double v) { return fx + static_cast<int>(std::lround(v * 200)); };
        Frame h0 = render(ui, 780, 450);
        CHECK(knobRun(h0, fcx(0.40), fyF + 2, kKnob) == 12 && h0.near(fcx(0.40) - 14, yF, kFill) && h0.near(fcx(0.40) + 14, yF, kTrack));
        int n = ui->get_changes();
        click(w, fx + 160, yF);                                      // the user puts it at 0.8
        CHECK(ui->get_changes() == n + 1);
        CHECK_NEAR(ui->get_last_value(), 0.8, 0.03);
        Frame h1 = render(ui, 780, 450);
        CHECK(knobRun(h1, fcx(0.8), fyF + 2, kKnob) == 12);          // and there it stays: the host has not answered
        CHECK_NEAR(ui->get_host_f(), 0.40, 0.0001);                  // and the slider left the host's own number alone
        ui->set_host_f(0.10f);                                       // the host moves it (an engine update, a shortcut key)
        Frame h2 = render(ui, 780, 450);
        CHECK(knobRun(h2, fcx(0.10), fyF + 2, kKnob) == 12);         // the knob follows, though the user had moved it before
        CHECK(h2.near(fcx(0.10) - 14, yF, kFill) && h2.near(fcx(0.10) + 14, yF, kTrack));
        CHECK(h2.countNear(fcx(0.8) - 12, fyF - 16, fcx(0.8) + 12, fyF + 22, kKnob, 30) == 0);   // nothing is left at 0.8
        // the user again, then the host sends back the very number it started with: that is a change too, the knob goes to it
        click(w, fx + 180, yF);
        CHECK_NEAR(ui->get_last_value(), 0.9, 0.03);
        ui->set_host_f(0.40f);
        Frame h3 = render(ui, 780, 450);
        CHECK(knobRun(h3, fcx(0.40), fyF + 2, kKnob) == 12 && knobRun(h3, fcx(0.9), fyF + 2, kKnob) == 0);
        // the wheel and the keys start from where the knob is
        scroll(w, fx + 100, yF, 0, 60);
        CHECK_NEAR(ui->get_last_value(), 0.45, 0.002);
        Frame h4 = render(ui, 780, 450);
        CHECK(knobRun(h4, fcx(0.45), fyF + 2, kKnob) == 12);
        // a host that takes the move on stores what it is told: its number and the knob then agree
        ui->set_host_f(ui->get_last_value());
        Frame h5 = render(ui, 780, 450);
        CHECK(knobRun(h5, fcx(0.45), fyF + 2, kKnob) == 12);
        // the same through a SliderGroup: the Gain lane is bound to host-g (0.62) and the user moved it to 0.5 above
        CHECK_NEAR(ui->get_host_g(), 0.62, 0.0001);
        ui->set_host_g(0.90f);
        Frame h6 = render(ui, 780, 450);
        const int laneKnob = static_cast<int>(std::lround(g1x + 0.90 * 232));
        CHECK(knobRun(h6, laneKnob, 325, kKnob) >= 11 && knobRun(h6, laneKnob, 325, kKnob) <= 13);
        CHECK(knobRun(h6, g1x + 116, 325, kKnob) == 0);              // nothing left at 0.5
    }

    // ===== tooltips: shown after the pointer rests, gone when it leaves; shown on a disabled slider too ======
    {
        Frame before = render(ui, 780, 450);
        CHECK(tipPixels(before, 40, 61, 270, 90) < 40);
        const auto resting = std::chrono::steady_clock::now();
        moveTo(w, kX + 100, kYA + 3);                      // slider A has a tooltip
        settle(std::chrono::milliseconds(150));
        Frame early = render(ui, 780, 450);
        // not yet: the tip waits 600 ms. (Should this machine have stalled for most of that, the claim is not made - but the
        // check is still counted, never skipped.)
        CHECK(std::chrono::steady_clock::now() - resting >= std::chrono::milliseconds(500) || tipPixels(early, 40, 61, 270, 90) < 40);
        settle(std::chrono::milliseconds(700));
        Frame tip = render(ui, 780, 450);
        CHECK(tipPixels(tip, 40, 61, 270, 90) > 2500);     // the tip's ground, 6px under the knob's foot, from the slider's left edge
        CHECK(tip.near(41, 70, kTip, 2) && tip.near(38, 70, kBg));                     // starts at the slider's own left edge, x = 40
        CHECK(tip.countInk(60, 66, 240, 86, kTip, 40) > 200);                          // and carries the words
        tip.savePng(shotDir() + "sliders-specimen-tooltip.png");
        moveTo(w, 600, 20);
        settle(std::chrono::milliseconds(50));
        Frame gone = render(ui, 780, 450);
        CHECK(tipPixels(gone, 40, 61, 270, 90) < 40);
        // slider B has no tooltip: resting on it shows nothing
        moveTo(w, kX + 60, kYB + 3);
        settle(std::chrono::milliseconds(750));
        Frame none = render(ui, 780, 450);
        CHECK(tipPixels(none, 40, 111, 270, 140) < 40);
        moveTo(w, 600, 20);
        // the AUTO key's tooltip
        moveTo(w, 232, 306);
        settle(std::chrono::milliseconds(750));
        Frame keytip = render(ui, 780, 450);
        CHECK(tipPixels(keytip, 208, 322, 460, 352) > 2500);
        moveTo(w, 600, 20);
        settle(std::chrono::milliseconds(50));
        // the disabled lane's slider still explains itself
        moveTo(w, 100, 406);
        settle(std::chrono::milliseconds(750));
        Frame distip = render(ui, 780, 450);
        CHECK(tipPixels(distip, 24, 420, 330, 449) > 2500);
        moveTo(w, 600, 20);
        settle(std::chrono::milliseconds(50));
        Frame quiet = render(ui, 780, 450);
        CHECK(tipPixels(quiet, 24, 420, 330, 449) < 40);
    }

    // ===== a second filling reaches every pixel, and no component borrows another's token ====================
    // Workbench's knob IS Theme.fg and its fill IS Theme.accent, so the checks above cannot tell a token from its look-alike.
    // Here every token the family reads gets a colour of its own, installed with the one assignment the theme promises.
    {
        Skin probe = ui->global<Workbench>().get_value();
        probe.slider_track = col(0x10, 0x10, 0x80);
        probe.slider_fill = col(0x00, 0xa0, 0xa0);
        probe.slider_knob = col(0xff, 0xff, 0x00);
        probe.slider_knob_ring = col(0x80, 0x00, 0x00);
        probe.slider_disabled_fill = col(0x80, 0x80, 0x00);
        probe.slider_disabled_knob = col(0xff, 0x00, 0xff);
        probe.level_bar = col(0x00, 0xff, 0x00);
        probe.key_bg = col(0x20, 0x20, 0x60);
        probe.key_line = col(0xa0, 0xa0, 0xff);
        probe.key_disabled_bg = col(0x60, 0x20, 0x20);
        probe.accent = col(0xff, 0x80, 0x00);
        probe.strip = col(0x30, 0x30, 0x30);
        ui->global<Theme>().set_skin(probe);
        moveTo(w, 600, 20);
        Frame s = render(ui, 780, 450);
        const Rgb pTrack = rgb(0x101080), pFill = rgb(0x00a0a0), pKnob = rgb(0xffff00), pRing = rgb(0x800000);
        const Rgb pDisFill = rgb(0x808000), pDisKnob = rgb(0xff00ff), pLevel = rgb(0x00ff00);
        const Rgb pKeyBg = rgb(0x202060), pKeyLine = rgb(0xa0a0ff), pKeyDisBg = rgb(0x602020), pAccent = rgb(0xff8000);
        CHECK(s.countNear(220, 30, 221, 70, pTrack, 4) == 6);                              // slider A: its own track ...
        CHECK(s.near(cx(0.62) - 14, kYA + 3, pFill, 4) && s.near(cx(0.62) + 14, kYA + 3, pTrack, 4));   // ... fill ...
        CHECK(knobRun(s, cx(0.62), kYA + 2, pKnob) == 12);                                 // ... knob ...
        CHECK(near2(s, cx(0.62) - 7, kYA + 2, pRing, 4) && near2(s, cx(0.62) + 6, kYA + 2, pRing, 4));   // ... and ring
        CHECK(s.near(cx(0.62) - 14, kYE + 3, pDisFill, 4) && knobRun(s, cx(0.62), kYE + 2, pDisKnob) == 12);   // the disabled pair
        CHECK(s.countNear(350, 30, 351, 70, pLevel, 4) == 4 && s.countNear(480, 30, 481, 70, pTrack, 4) == 4);   // the level bar
        CHECK(s.near(212, 306, pKeyBg, 4) && s.near(208, 306, pKeyLine, 4));               // AUTO off: key-bg, key-line
        CHECK(s.near(g2x + 188, 306, pAccent, 4) && s.near(g2x + 232 - 48, 306, pAccent, 4));   // AUTO on: accent ground and border
        CHECK(s.near(212, g4y + 10, pKeyDisBg, 4));                                        // AUTO disabled: key-disabled-bg
        CHECK(s.near(770, 300, rgb(0x303030), 2));                                         // the strip ground follows too
        s.savePng(shotDir() + "sliders-specimen-probe-filling.png");
    }

    // ===== Tab reaches a slider and shows the ring; the arrow keys then move it (a fresh sheet: nothing focused yet) ==
    {
        auto fresh = SlidersSpecimen::create();
        slint::Window& fw = fresh->window();
        Frame t0 = render(fresh, 780, 450);
        CHECK(t0.countNear(cx(0.62) - 14, kYA - 16, cx(0.62) + 14, kYA + 22, kFocus, 20) == 0);
        keyTap(fw, slint::platform::key_codes::Tab);
        Frame t1 = render(fresh, 780, 450);
        CHECK(t1.countNear(cx(0.62) - 13, kYA + 2, cx(0.62) + 13, kYA + 3, kFocus, 20) >= 3);       // the first slider, ringed ...
        CHECK(t1.countNear(cx(0.29) - 14, kYB - 16, cx(0.29) + 14, kYB + 22, kFocus, 20) == 0);     // ... and only it
        keyTap(fw, slint::platform::key_codes::RightArrow);
        Frame t2 = render(fresh, 780, 450);
        CHECK(knobRun(t2, cx(0.67), kYA + 2, kKnob) == 12 && t2.near(cx(0.62) - 4, kYA + 3, kFill));   // 0.62 -> 0.67: the knob moved on
        keyTap(fw, slint::platform::key_codes::Tab);
        Frame t3 = render(fresh, 780, 450);
        CHECK(t3.countNear(cx(0.29) - 13, kYB + 2, cx(0.29) + 13, kYB + 3, kFocus, 20) >= 3);       // the ring moved on to B
        CHECK(t3.countNear(cx(0.67) - 14, kYA - 16, cx(0.67) + 14, kYA + 22, kFocus, 20) == 0);     // and left A
    }

    return testSummary("test_slint_sliders");
}
