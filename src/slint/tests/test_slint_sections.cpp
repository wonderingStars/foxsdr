// The sections family (ui/sections.slint): Section, BodyRow and SelectKey, rendered headlessly from the specimen
// sheet and probed against the artboards (Workbench.dc.html's sections panel and open Radio body,
// Workbench-PluginKit.dc.html's .sec.s / .dn .sec.s rows). Geometry and colours are asserted; the text is in a
// fallback typeface on this machine, so for text only "ink in this region" and "no ink in that gutter" are.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "sections_specimen.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>

using namespace foxsdr::slinttest;

namespace {

// The Workbench sheet's tokens (the <helmet><style> blocks of the two artboards).
constexpr Rgb kBg = rgb(0x15171b);          // the specimen window, and the panel behind a closed row
constexpr Rgb kPanel = rgb(0x1d2026);       // .panel: the open row's tint and the open body
constexpr Rgb kLine = rgb(0x2b2f37);        // every 1px divider
constexpr Rgb kMuted = rgb(0x9aa3ad);       // the chevron, a cap
constexpr Rgb kFg = rgb(0xe6e8ec);          // the name
constexpr Rgb kDisabled = rgb(0x8a93a0);    // disabled text
constexpr Rgb kChipBg = rgb(0x262a32);
constexpr Rgb kChipGoodBg = rgb(0x163626);
constexpr Rgb kChipWarnBg = rgb(0x3a2a0e);
constexpr Rgb kChipErrBg = rgb(0x3a1d1d);
constexpr Rgb kChipErrLine = rgb(0x6b2a2a);
constexpr Rgb kLampGreen = rgb(0x3ddc84);
constexpr Rgb kLampAmber = rgb(0xf5a524);
constexpr Rgb kLampRed = rgb(0xff5c5c);
constexpr Rgb kKeyBg = rgb(0x262a32);       // .btn
constexpr Rgb kKeyLine = rgb(0x363b45);
constexpr Rgb kMenuHot = rgb(0x363b45);     // a hovered key
constexpr Rgb kMenu = rgb(0x262a32);        // a tooltip's ground

// The specimen's table (the header of sections_specimen.slint).
constexpr int kW = 660, kH = 480;
constexpr int kX = 16, kPanelW = 296, kRight = kX + kPanelW, kChipRight = kRight - 10;
constexpr int kFirstY = 16, kDotY = 60, kOpenY = 104, kSmallY = 222, kDenseY = 260, kDisabledY = 293;
constexpr int kStackX = 340, kStackY = 16, kStackRight = kStackX + kPanelW, kLongY = 200, kSelectOffY = 250, kNoChipY = 290, kBoxesY = 340;

struct Box {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1, n = 0;   // inclusive bounds of the ink, and its pixel count
    int w() const { return n > 0 ? x1 - x0 + 1 : 0; }
    int h() const { return n > 0 ? y1 - y0 + 1 : 0; }
};

// The bounding box of everything in [x0,x1) x [y0,y1) that differs from `bg`.
Box inkBox(const Frame& f, int x0, int y0, int x1, int y1, Rgb bg, int tol = 24) {
    Box b;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (f.near(x, y, bg, tol)) { continue; }
            if (b.n == 0) { b.x0 = b.x1 = x; b.y0 = b.y1 = y; }
            b.x0 = std::min(b.x0, x); b.x1 = std::max(b.x1, x);
            b.y0 = std::min(b.y0, y); b.y1 = std::max(b.y1, y);
            ++b.n;
        }
    }
    return b;
}

// The bounding box of the pixels near `colour`.
Box colourBox(const Frame& f, int x0, int y0, int x1, int y1, Rgb colour, int tol = 10) {
    Box b;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (!f.near(x, y, colour, tol)) { continue; }
            if (b.n == 0) { b.x0 = b.x1 = x; b.y0 = b.y1 = y; }
            b.x0 = std::min(b.x0, x); b.x1 = std::max(b.x1, x);
            b.y0 = std::min(b.y0, y); b.y1 = std::max(b.y1, y);
            ++b.n;
        }
    }
    return b;
}

// The brightest pixel of a region: a light glyph on a dark ground reaches its own colour somewhere in a stem.
Rgb brightest(const Frame& f, int x0, int y0, int x1, int y1) {
    Rgb best{};
    int bestLum = -1;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Rgb p = f.at(x, y);
            const int lum = p.r + p.g + p.b;
            if (lum > bestLum) { bestLum = lum; best = p; }
        }
    }
    return best;
}

double luminance(Rgb c) { return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b; }

// How far a pixel is from `bg` towards `ink`, 0..1 (a thin anti-aliased stroke reaches only part of the way).
double strength(Rgb p, Rgb ink, Rgb bg) {
    const double span = luminance(ink) - luminance(bg);
    return span == 0 ? 0 : (luminance(p) - luminance(bg)) / span;
}

std::string lastClick(const SectionsSpecimen& ui) { return std::string(std::string_view(ui.get_last_click())); }
void ms(int n) { settle(std::chrono::milliseconds(n)); }

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = SectionsSpecimen::create();
    slint::Window& win = ui->window();

    Frame f = render(ui, kW, kH);
    CHECK(f.width == kW && f.height == kH);
    f.savePng(shotDir() + "sections-specimen.png");

    // --- a second filling installed through Theme.skin restyles the sections with no component change -----------
    {
        Skin probe = ui->global<Workbench>().get_value();
        probe.panel = slint::Color::from_rgb_uint8(0x40, 0x20, 0x50);
        probe.line = slint::Color::from_rgb_uint8(0xff, 0xff, 0x00);
        probe.muted = slint::Color::from_rgb_uint8(0x00, 0xff, 0x00);
        probe.key_bg = slint::Color::from_rgb_uint8(0x90, 0x00, 0x00);
        probe.chip_bg = slint::Color::from_rgb_uint8(0x00, 0x00, 0x90);
        probe.hairline = 2.0f;
        ui->global<Theme>().set_skin(probe);
        Frame g0 = render(ui, kW, kH);
        CHECK(g0.near(kX + 4, kOpenY + 18, rgb(0x402050), 3));                 // the open row's tint is Theme.panel
        CHECK(g0.near(kX + 4, kOpenY + 100, rgb(0x402050), 3));                // ...and so is the body's ground
        CHECK(g0.rowNear(kDotY, kX, kRight, rgb(0xffff00)));                   // the top rule is Theme.line, Theme.hairline thick
        CHECK(g0.rowNear(kDotY + 1, kX, kRight, rgb(0xffff00)));
        CHECK(g0.rowNear(kDotY + 2, kX, kRight, kBg, 3));
        CHECK(g0.rowNear(kOpenY + 110 - 1, kX, kRight, rgb(0xffff00)));        // the body's rule follows too
        CHECK(g0.near(kChipRight - 5, kFirstY + 18, rgb(0x000090)));           // the neutral chip's ground
        CHECK(g0.near(kRight - 12 - 110 + 40, kOpenY + 36 + 4 + 24 + 10 + 3, rgb(0x900000), 3));   // the SelectKey's ground
        CHECK(nearColour(brightest(g0, kX + 8, kFirstY + 11, kX + 22, kFirstY + 25), rgb(0x00ff00), 60));   // the chevron
        ui->global<Theme>().set_skin(ui->global<Workbench>().get_value());     // and back: the first picture exactly
        Frame g1 = render(ui, kW, kH);
        int differing = 0;
        for (std::size_t i = 0; i < g1.px.size(); ++i) {
            if (g1.px[i].r != f.px[i].r || g1.px[i].g != f.px[i].g || g1.px[i].b != f.px[i].b) { ++differing; }
        }
        CHECK(g1.px.size() == f.px.size() && differing == 0);
    }

    // --- row heights: 36 / 30 / 25, and the open Section is the row plus its body ---------------------------
    CHECK_NEAR(ui->get_h_first(), 36, 0.01);
    CHECK_NEAR(ui->get_h_dot(), 36, 0.01);
    CHECK_NEAR(ui->get_h_small(), 30, 0.01);
    CHECK_NEAR(ui->get_h_dense(), 25, 0.01);
    CHECK_NEAR(ui->get_h_disabled(), 36, 0.01);
    CHECK_NEAR(ui->get_body_open(), 74, 0.01);      // 4 top + 24 + 10 gap + 24 + 12 bottom
    CHECK_NEAR(ui->get_h_open(), 110, 0.01);        // 36 + 74

    // --- the first row: no top rule, a transparent ground, chevron, name, neutral chip ----------------------
    CHECK(f.rowNear(kFirstY, kX, kRight, kBg));                          // no hairline on the first row
    CHECK(f.near(kX + 4, kFirstY + 18, kBg, 3));                         // closed: the window ground shows through
    CHECK(f.near(kX + 150, kFirstY + 30, kBg, 3));
    {
        const Box chev = inkBox(f, kX + 4, kFirstY + 4, kX + 26, kFirstY + 32, kBg);
        CHECK(chev.n > 8);                                               // the chevron is drawn in the gutter
        CHECK(chev.x0 >= kX + 8 && chev.x1 < kX + 8 + 14);               // inside its 14px box at x 8
        CHECK(chev.h() > chev.w());                                      // chevron-right: taller than wide
        CHECK(strength(brightest(f, chev.x0, chev.y0, chev.x1 + 1, chev.y1 + 1), kMuted, kBg) > 0.85);   // muted ink
        CHECK(f.countInk(kX + 23, kFirstY + 4, kX + 30, kFirstY + 32, kBg) == 0);   // 8px gap: nothing before the name
        CHECK(f.countInk(kX + 30, kFirstY + 8, kX + 100, kFirstY + 28, kBg) > 20);  // the name starts at x 30
        const Rgb nameInk = brightest(f, kX + 30, kFirstY + 8, kX + 100, kFirstY + 28);
        CHECK(nearColour(nameInk, kFg, 40));                             // the name is in the foreground colour
        // the chip: a pill 10px from the right, 20px high, centred (row 36)
        CHECK(f.near(kChipRight - 5, kFirstY + 18, kChipBg));
        CHECK(f.near(kChipRight + 1, kFirstY + 18, kBg, 3));             // nothing right of the 10px padding
        const Box chip = inkBox(f, kX + 150, kFirstY + 2, kRight, kFirstY + 34, kBg, 10);
        CHECK(chip.x1 >= kChipRight - 2 && chip.x1 <= kChipRight - 1);   // its right edge is at 10px padding
        CHECK(chip.h() >= 19 && chip.h() <= 21);                         // 20px high
        CHECK(chip.y0 >= kFirstY + 7 && chip.y0 <= kFirstY + 9);         // vertically centred in 36
    }

    // --- closed with a dot: top hairline, the state dot, the good chip ---------------------------------------
    CHECK(f.rowNear(kDotY, kX, kRight, kLine));                          // border-top: 1px solid line, first pixel row
    CHECK(f.rowNear(kDotY + 1, kX, kRight, kBg));                        // ...and one pixel thick
    CHECK(f.near(kX + 4, kDotY + 18, kBg, 3));
    CHECK(f.near(kX + 30 + 4, kDotY + 18, kLampGreen));                  // the 8px dot at x 30, centred in the row
    CHECK(f.countInk(kX + 30 + 9, kDotY + 4, kX + 46, kDotY + 32, kBg) == 0);   // 8px gap after the dot
    CHECK(f.countInk(kX + 46, kDotY + 8, kX + 110, kDotY + 28, kBg) > 20);      // the name starts at x 46
    CHECK(f.near(kChipRight - 5, kDotY + 18, kChipGoodBg));              // tone good

    // --- OPEN: the panel tint on the row, the chevron turned down, the warn chip -----------------------------
    CHECK(f.rowNear(kOpenY, kX, kRight, kLine));                         // the top rule still sits on the tinted row
    CHECK(f.near(kX + 4, kOpenY + 18, kPanel, 3));                       // open: the .panel tint, not the window ground
    CHECK(f.near(kX + 150, kOpenY + 30, kPanel, 3));
    {
        const Box chev = inkBox(f, kX + 4, kOpenY + 4, kX + 26, kOpenY + 32, kPanel);
        CHECK(chev.n > 8);
        CHECK(chev.w() > chev.h());                                      // chevron-down: wider than tall
        CHECK(chev.x0 >= kX + 8 && chev.x1 < kX + 8 + 14);
    }
    CHECK(f.near(kChipRight - 5, kOpenY + 18, kChipWarnBg));            // tone warn

    // --- the open body: panel ground, a rule underneath, children inset by the 30px left padding -------------
    CHECK(f.colNear(kX + 4, kOpenY + 36, kOpenY + 110 - 1, kPanel, 3));    // the left padding column is plain panel
    CHECK(f.colNear(kRight - 4, kOpenY + 36, kOpenY + 110 - 1, kPanel, 3)); // ...and so is the right one
    CHECK(f.rowNear(kOpenY + 110 - 1, kX, kRight, kLine));               // border-bottom: 1px solid line
    CHECK(f.rowNear(kOpenY + 110 - 2, kX, kRight, kPanel, 3));
    CHECK(f.rowNear(kOpenY + 110, kX, kRight, kBg, 3));                  // the window ground resumes below
    const int r1 = kOpenY + 36 + 4;                                      // the first BodyRow: 4px under the row
    const int r2 = r1 + 24 + 10;                                         // the second: 10px under the first
    {
        CHECK(f.countInk(kX, r1, kX + 30, r1 + 24, kPanel) == 0);        // nothing in the 30px padding
        const Box cap = inkBox(f, kX, r1, kX + 150, r1 + 24, kPanel);
        CHECK(cap.n > 20);                                               // the "PASSBAND" cap
        CHECK(cap.x0 >= kX + 30 && cap.x0 <= kX + 33);                   // starts at the padding
        CHECK(cap.y0 >= r1 + 4 && cap.y1 <= r1 + 20);                    // a 10px cap centred in the 24px row
        CHECK(nearColour(brightest(f, kX + 30, r1, kX + 120, r1 + 24), kMuted, 40));   // the cap is muted
        const Box val = inkBox(f, kX + 150, r1, kRight, r1 + 24, kPanel);
        CHECK(val.n > 20);                                               // the mono value at the right
        CHECK(val.x1 <= kRight - 12 - 1);                                // ...ends at the 12px right padding
        CHECK(val.x1 >= kRight - 12 - 4);
        CHECK(f.countInk(kRight - 12, r1, kRight, r1 + 24, kPanel) == 0);
        CHECK(nearColour(brightest(f, val.x0, val.y0, val.x1 + 1, val.y1 + 1), kFg, 40));

        // the second row: the cap and the SelectKey (110 x 24) 10px under the first
        CHECK(r2 == 178);
        CHECK(inkBox(f, kX + 30, r2, kX + 120, r2 + 24, kPanel).n > 20);
        // SelectKey: right edge at the 12px padding, 110px wide, 24px high
        const int kx0 = kRight - 12 - 110, kx1 = kRight - 12;            // 190 .. 300
        CHECK_NEAR(ui->get_select_w(), 110, 0.01);
        CHECK_NEAR(ui->get_select_h(), 24, 0.01);
        CHECK(f.colNear(kx0, r2 + 8, r2 + 16, kKeyLine));                // left border
        CHECK(f.colNear(kx1 - 1, r2 + 8, r2 + 16, kKeyLine));            // right border
        CHECK(f.near(kx0 - 1, r2 + 12, kPanel, 3));                      // the body ground just outside
        CHECK(f.near(kx1, r2 + 12, kPanel, 3));
        CHECK(f.rowNear(r2, kx0 + 8, kx1 - 8, kKeyLine));                // top border
        CHECK(f.rowNear(r2 + 23, kx0 + 8, kx1 - 8, kKeyLine));           // bottom border: 24px in all
        CHECK(f.rowNear(r2 - 1, kx0 + 8, kx1 - 8, kPanel, 3));
        CHECK(f.rowNear(r2 + 24, kx0 + 8, kx1 - 8, kPanel, 3));
        CHECK(f.near(kx0 + 40, r2 + 3, kKeyBg, 3));                      // .btn ground
        CHECK(f.near(kx0 + 40, r2 + 21, kKeyBg, 3));
        CHECK(f.countInk(kx0 + 8, r2 + 5, kx0 + 60, r2 + 19, kKeyBg) > 15);        // the value text
        const Box chev = inkBox(f, kx1 - 8 - 14, r2 + 3, kx1 - 8, r2 + 21, kKeyBg);
        CHECK(chev.n > 6 && chev.w() > chev.h());                        // the chevron-down at the right
        CHECK(strength(brightest(f, kx1 - 22, r2 + 3, kx1 - 8, r2 + 21), kFg, kKeyBg) > 0.85);
        CHECK(f.countInk(kx0 + 1, r2 + 1, kx0 + 8, r2 + 23, kKeyBg) == 0);        // 8px of padding before the text
    }

    // --- small (30) and dense (25) rows ---------------------------------------------------------------------
    CHECK(f.rowNear(kSmallY, kX, kRight, kLine));
    CHECK(f.near(kChipRight - 5, kSmallY + 15, kChipGoodBg));            // the chip centred in 30: rows 5 .. 24
    CHECK(f.near(kChipRight - 5, kSmallY + 5 - 1, kBg, 3));
    CHECK(f.near(kChipRight - 5, kSmallY + 25, kBg, 3));
    CHECK(f.near(kX + 30 + 4, kSmallY + 15, kLampAmber));                // the amber dot at the row's middle
    CHECK(f.rowNear(kDenseY, kX, kRight, kLine));
    CHECK(f.near(kChipRight - 5, kDenseY + 12, kChipBg));                // the chip in 25: about rows 2.5 .. 22.5
    CHECK(f.near(kChipRight - 5, kDenseY + 1, kBg, 3));
    CHECK(f.near(kChipRight - 5, kDenseY + 24, kBg, 3));
    CHECK(f.countInk(kX, kDenseY + 25, kRight, kDenseY + 33, kBg) == 0); // nothing under the dense row (it ends at 25)
    {
        const Box chip = inkBox(f, kX + 150, kDenseY + 1, kRight, kDenseY + 25, kBg, 10);
        CHECK(chip.h() >= 19 && chip.h() <= 21);
        CHECK(chip.y0 >= kDenseY + 2 && chip.y0 <= kDenseY + 3);         // centred in 25
        const Box name = inkBox(f, kX + 30, kDenseY + 1, kX + 150, kDenseY + 25, kBg);
        CHECK(name.n > 20 && name.y0 >= kDenseY + 3 && name.y1 <= kDenseY + 22);
    }

    // --- a disabled row: dimmed name and chevron, no ink in the foreground colour ---------------------------
    CHECK(f.rowNear(kDisabledY, kX, kRight, kLine));
    {
        const Rgb dis = brightest(f, kX + 30, kDisabledY + 8, kX + 100, kDisabledY + 28);
        CHECK(nearColour(dis, kDisabled, 40));
        CHECK(!nearColour(dis, kFg, 40));
        CHECK(f.countInk(kX + 30, kDisabledY + 8, kX + 100, kDisabledY + 28, kBg) > 20);
        CHECK(nearColour(brightest(f, kX + 8, kDisabledY + 11, kX + 22, kDisabledY + 25), kDisabled, 40));
        CHECK(f.near(kChipRight - 5, kDisabledY + 18, kChipBg));         // the chip is the same neutral chip
    }

    // --- the stack: the open body pushes the next row down ----------------------------------------------------
    CHECK_NEAR(ui->get_stack_y1(), 0, 0.01);
    CHECK_NEAR(ui->get_stack_y2(), 36, 0.01);
    CHECK_NEAR(ui->get_stack_y3(), 36 + 76, 0.01);                        // row 36 + (row 36 + body 4 + 24 + 12)
    CHECK_NEAR(ui->get_stack_h2(), 76, 0.01);
    CHECK(f.rowNear(kStackY, kStackX, kStackRight, kBg));                // the first row has no rule
    CHECK(f.rowNear(kStackY + 36, kStackX, kStackRight, kLine));         // the open row's rule
    CHECK(f.near(kStackX + 4, kStackY + 36 + 18, kPanel, 3));
    CHECK(f.near(kStackX + 4, kStackY + 36 + 50, kPanel, 3));            // inside its body
    CHECK(f.rowNear(kStackY + 36 + 76 - 1, kStackX, kStackRight, kLine));// the body's rule
    CHECK(f.rowNear(kStackY + 36 + 76, kStackX, kStackRight, kLine));    // the third row's rule: at row 36 + (36 + 40)
    CHECK(f.rowNear(kStackY + 36 + 76 + 1, kStackX, kStackRight, kBg, 3));
    CHECK(f.near(kStackX + 4, kStackY + 36 + 76 + 18, kBg, 3));          // the third row is closed: transparent
    CHECK(inkBox(f, kStackX + 30, kStackY + 36 + 76 + 6, kStackX + 120, kStackY + 36 + 76 + 30, kBg).n > 20);  // "Recorder"

    // --- a long name elides before the chip -------------------------------------------------------------------
    CHECK(f.rowNear(kLongY, kStackX, kStackRight, kLine));
    {
        const int midY = kLongY + 15;
        const int chipLeft = f.firstColNear(midY, kStackX + 100, kStackRight, kChipErrLine, 24);
        CHECK(chipLeft > kStackX + 150);                                 // the err chip is found, well to the right
        CHECK(f.near(chipLeft + 4, midY, kChipErrBg, 24) || f.near(chipLeft + 6, midY, kChipErrBg, 24));
        CHECK(f.countInk(chipLeft - 7, kLongY + 3, chipLeft, kLongY + 27, kBg) == 0);      // 8px of air before the chip
        CHECK(inkBox(f, kStackX + 46, kLongY + 5, chipLeft - 8, kLongY + 25, kBg).n > 150);  // the name fills the room
        CHECK(f.near(kStackX + 30 + 4, midY, kLampRed));                 // the red dot
    }

    // --- no chip: nothing at the right, the name has the row ---------------------------------------------------
    CHECK(f.rowNear(kNoChipY, kStackX, kStackRight, kLine));
    CHECK(f.countInk(kStackX + 230, kNoChipY + 2, kStackRight, kNoChipY + 34, kBg) == 0);            // no empty pill
    CHECK(inkBox(f, kStackX + 30, kNoChipY + 8, kStackX + 230, kNoChipY + 28, kBg).n > 100);         // "Frequency correction"
    CHECK(f.countInk(kStackX + 23, kNoChipY + 4, kStackX + 30, kNoChipY + 32, kBg) == 0);

    // --- a BodyRow centres a control shorter than the row, and grows to a taller one -----------------------------
    {
        CHECK_NEAR(ui->get_h_boxes(), 36 + 4 + 24 + 10 + 40 + 12, 0.01);
        const int row1 = kBoxesY + 36 + 4, row2 = row1 + 24 + 10;
        const Box redBox = colourBox(f, kStackX + 200, row1, kStackRight, row1 + 24, kLampRed, 6);
        CHECK(redBox.w() == 14 && redBox.h() == 10);                     // the last control keeps its own size...
        CHECK(redBox.x1 == kStackRight - 12 - 1);                        // ...at the 12 px right padding...
        CHECK(redBox.y0 == row1 + 7);                                    // ...centred in the 24 px row (7 + 10 + 7)
        const Box shortBox = colourBox(f, kStackX + 150, row1, kStackRight, row1 + 24, kLampAmber, 6);
        CHECK(shortBox.w() == 20 && shortBox.h() == 10 && shortBox.y0 == row1 + 7);
        CHECK(shortBox.x1 + 1 + 6 == redBox.x0);                         // the controls sit side by side, 6 px apart
        const Box tallBox = colourBox(f, kStackX + 200, row2, kStackRight, row2 + 60, kLampGreen, 6);
        CHECK(tallBox.w() == 20 && tallBox.h() == 40);
        CHECK(tallBox.y0 == row2);                                       // the row is as tall as the control: 40
        CHECK(f.rowNear(kBoxesY + 36 + 74 + 16 - 1, kStackX, kStackRight, kLine));   // the body ends 12 px under the tall row
        CHECK(f.near(kStackX + 4, kBoxesY + 36 + 74 + 16 - 2, kPanel, 3));
        CHECK(f.rowNear(kBoxesY + 126, kStackX, kStackRight, kBg, 3));
    }

    // --- a disabled SelectKey: the key-disabled look ----------------------------------------------------------
    {
        CHECK_NEAR(ui->get_select_off_w(), 110, 0.01);
        CHECK(f.near(kStackX + 50, kSelectOffY + 3, kPanel, 3));         // key-disabled-bg, not the live key's ground
        CHECK(!f.near(kStackX + 50, kSelectOffY + 3, kKeyBg, 3));
        const Rgb text = brightest(f, kStackX + 8, kSelectOffY + 5, kStackX + 60, kSelectOffY + 19);
        CHECK(nearColour(text, kDisabled, 40));                          // key-disabled-fg
        CHECK(!nearColour(text, kFg, 40));
        // the border is at half alpha: dimmer than the live key's border, brighter than the ground
        const Rgb border = f.at(kStackX, kSelectOffY + 12);
        CHECK(luminance(border) > luminance(kBg) + 4);
        CHECK(luminance(border) < luminance(kKeyLine) - 4);
        CHECK(f.rowNear(kSelectOffY, kStackX + 8, kStackX + 102, border, 4));
        CHECK(f.rowNear(kSelectOffY + 23, kStackX + 8, kStackX + 102, border, 4));
    }

    // --- interaction: the row fires `clicked`, the body does not, the SelectKey has its own callback ----------
    int n = 0;                                                            // the running count of row clicks
    CHECK(ui->get_clicks() == 0 && ui->get_select_clicks() == 0);
    click(win, kX + 120, kFirstY + 18);                                   // the first row
    CHECK(ui->get_clicks() == ++n && lastClick(*ui) == "first");
    click(win, kX + 120, kOpenY + 18);                                    // the open row
    CHECK(ui->get_clicks() == ++n && lastClick(*ui) == "open");
    click(win, kX + 5, kOpenY + 36 + 2);                                  // inside the body (its padding)
    click(win, kX + 120, kOpenY + 36 + 4 + 12);                           // inside the body (a BodyRow, off the controls)
    click(win, kX + 120, kOpenY + 36 + 4 + 24 + 5);                       // the gap between the two BodyRows
    CHECK(ui->get_clicks() == n);                                         // none of them reached the row
    click(win, kRight - 12 - 55, r2 + 12);                                // the SelectKey
    CHECK(ui->get_select_clicks() == 1);
    CHECK(ui->get_clicks() == n);                                         // ...and it is not a row click
    click(win, kX + 120, kDotY + 18);                                     // a row with a dot
    CHECK(ui->get_clicks() == ++n && lastClick(*ui) == "dot");
    click(win, kX + 120, kSmallY + 15);
    CHECK(ui->get_clicks() == ++n && lastClick(*ui) == "small");
    click(win, kX + 120, kDenseY + 12);
    CHECK(ui->get_clicks() == ++n && lastClick(*ui) == "dense");
    click(win, kStackX + 120, kNoChipY + 18);                             // a row without a chip is a row
    CHECK(ui->get_clicks() == ++n && lastClick(*ui) == "nochip");
    click(win, kX + 120, kDisabledY + 18);                                // disabled: never acts
    CHECK(ui->get_clicks() == n && lastClick(*ui) == "nochip");
    click(win, kX + 15, kDisabledY + 18);                                 // ...not on the chevron either
    CHECK(ui->get_clicks() == n);
    click(win, kStackX + 55, kSelectOffY + 12);                           // a disabled SelectKey never acts
    CHECK(ui->get_select_clicks() == 1);

    // --- only the left button toggles a row or opens a picker ------------------------------------------------------
    click(win, kX + 120, kFirstY + 18, slint::PointerEventButton::Right);
    click(win, kX + 120, kOpenY + 18, slint::PointerEventButton::Middle);
    click(win, kRight - 12 - 55, r2 + 12, slint::PointerEventButton::Right);
    CHECK(ui->get_clicks() == n && ui->get_select_clicks() == 1);

    // --- the stack follows the host flipping `open` -------------------------------------------------------------
    CHECK(ui->get_stack_open());
    click(win, kStackX + 120, kStackY + 36 + 18);                         // the open row of the stack
    CHECK(ui->get_clicks() == ++n && lastClick(*ui) == "stack-2");
    CHECK(!ui->get_stack_open());
    Frame g = render(ui, kW, kH);
    CHECK_NEAR(ui->get_stack_h2(), 36, 0.01);
    CHECK_NEAR(ui->get_stack_y3(), 72, 0.01);
    CHECK(g.near(kStackX + 4, kStackY + 36 + 18, kBg, 3));                // closed again: no tint
    CHECK(g.rowNear(kStackY + 36 + 36, kStackX, kStackRight, kLine));     // the third row has come up to 72
    CHECK(g.rowNear(kStackY + 36 + 76, kStackX, kStackRight, kBg, 3));    // and nothing is left at the old place
    click(win, kStackX + 120, kStackY + 36 + 18);                         // reopen
    CHECK(ui->get_stack_open());
    ++n;
    Frame g2 = render(ui, kW, kH);
    CHECK(g2.rowNear(kStackY + 36 + 76, kStackX, kStackRight, kLine));
    // a click on the stack's third row after the reopen lands where the row now is
    click(win, kStackX + 120, kStackY + 36 + 76 + 18);
    CHECK(lastClick(*ui) == "stack-3");
    CHECK(ui->get_clicks() == ++n);

    // --- hover on the SelectKey lights its ground; hover on the disabled one does not ---------------------------
    {
        const int kx0 = kRight - 12 - 110;
        moveTo(win, 700, 370);
        Frame cold = render(ui, kW, kH);
        CHECK(cold.near(kx0 + 40, r2 + 3, kKeyBg, 3));
        moveTo(win, kx0 + 40, r2 + 12);
        Frame hot = render(ui, kW, kH);
        CHECK(hot.near(kx0 + 40, r2 + 3, kMenuHot, 3));                  // hovered: the menu-hot ground
        moveTo(win, kStackX + 50, kSelectOffY + 12);
        Frame off = render(ui, kW, kH);
        CHECK(off.near(kStackX + 50, kSelectOffY + 3, kPanel, 3));       // disabled: no hover light
        CHECK(!off.near(kx0 + 40, r2 + 3, kMenuHot, 3));                 // ...and the live key has let go
        moveTo(win, 700, 370);
    }

    // --- tooltips open under the element after the hover delay ---------------------------------------------------
    {
        // a disabled row explains WHY it is disabled: the tip is 6px under the row
        const int tipY = kDisabledY + 36 + 6;
        moveTo(win, 700, 370);
        ms(20);
        Frame before = render(ui, kW, kH);
        CHECK(before.countInk(kX, tipY, kX + 200, tipY + 24, kBg) == 0);
        moveTo(win, kX + 120, kDisabledY + 18);
        Frame early = render(ui, kW, kH);
        CHECK(early.countInk(kX, tipY, kX + 200, tipY + 24, kBg) == 0);   // not yet: 600ms
        ms(750);
        Frame shown = render(ui, kW, kH);
        CHECK(shown.countInk(kX, tipY, kX + 140, tipY + 24, kBg, 8) > 1500);   // the tip's ground, border and text
        CHECK(shown.near(kX + 3, tipY + 8, kMenu, 3));                    // the menu ground, at the row's left edge
        moveTo(win, 700, 370);
        ms(20);
        Frame gone = render(ui, kW, kH);
        CHECK(gone.countInk(kX, tipY, kX + 200, tipY + 24, kBg) == 0);
        CHECK(ui->get_clicks() == n);                                     // hovering clicked nothing

        // the SelectKey's tip: 6px under the key, at its left edge
        const int kx0 = kRight - 12 - 110;
        const int selTip = r2 + 24 + 6;
        CHECK(gone.near(kx0 + 3, selTip + 3, kPanel, 3));                 // the body's bottom padding
        moveTo(win, kx0 + 40, r2 + 12);
        ms(750);
        Frame selShown = render(ui, kW, kH);
        CHECK(selShown.near(kx0 + 3, selTip + 3, kMenu, 3));              // the tip's ground covers it
        moveTo(win, 700, 370);
        ms(20);
        Frame selGone = render(ui, kW, kH);
        CHECK(selGone.near(kx0 + 3, selTip + 3, kPanel, 3));
    }

    return testSummary("test_slint_sections");
}
