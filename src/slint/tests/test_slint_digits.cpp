// The DIGITS family (docs 6.36): the Digit column and the FrequencyCounter plate, rendered headlessly from
// ui/specimens/digits_specimen.slint and probed against the Workbench sheet's CSS (.digit, .nud, .mono.row) and the
// counter inventory (wheel steps a digit, a click opens the typed editor, UP/DN auto-repeat, right-click menu).
//
// Text is in the embedded JetBrains Mono and Inter (third_party/fonts, the same on every machine), so glyph checks assert INK
// in a region and its COLOUR, never a shape or a width; the plate, dots, rows and cells are drawn geometry and are asserted to
// the pixel. The dot cell is a fixed 13 px (0.6 em of the 22 px counter size, rounded), whatever the dot glyph's advance.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "digits_specimen.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <vector>

using namespace foxsdr::slinttest;
using std::chrono::milliseconds;

namespace {

// Prints the numbers behind a failed check, so a red run says what was measured.
#define CHECKV(cond, ...)                         \
    do {                                          \
        const bool ok_ = static_cast<bool>(cond); \
        CHECK(ok_);                               \
        if (!ok_) {                               \
            std::printf("      ");                \
            std::printf(__VA_ARGS__);             \
            std::printf("\n");                    \
        }                                         \
    } while (0)

Rgb tok(slint::Color c) { return {c.red(), c.green(), c.blue()}; }

// The bounding box and count of the ink (pixels differing from `bg`) in a rectangle.
struct Ink {
    int n = 0, x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    double cx() const { return (x0 + x1 + 1) / 2.0; }   // centre of the box, in pixel-edge coordinates
    double cy() const { return (y0 + y1 + 1) / 2.0; }
};
Ink inkBox(const Frame& f, int x0, int y0, int x1, int y1, Rgb bg, int tol = 24) {
    Ink r;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (f.near(x, y, bg, tol)) { continue; }
            ++r.n;
            r.x0 = std::min(r.x0, x); r.y0 = std::min(r.y0, y);
            r.x1 = std::max(r.x1, x); r.y1 = std::max(r.y1, y);
        }
    }
    return r;
}

// The brightest pixel of a rectangle (the largest channel sum). Light text on a dark ground is anti-aliased into blends
// of the text colour and the ground, so a count of "pixels near the colour" cannot tell two close greys apart (a 90 %
// covered nudge-grey glyph looks like the faint engraving colour); the pixel a glyph covers fully is the brightest one
// and reads the text's own colour.
Rgb brightest(const Frame& f, int x0, int y0, int x1, int y1) {
    Rgb best{};
    int sum = -1;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Rgb p = f.at(x, y);
            const int s = int(p.r) + int(p.g) + int(p.b);
            if (s > sum) { sum = s; best = p; }
        }
    }
    return best;
}

// How many pixels of the rectangle differ between two frames.
int differing(const Frame& a, const Frame& b, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Rgb p = a.at(x, y), q = b.at(x, y);
            if (p.r != q.r || p.g != q.g || p.b != q.b) { ++n; }
        }
    }
    return n;
}

// The strength of the change between two frames in a rectangle: the sum of the channel differences. A glow is a
// faint tint over many pixels, so how MANY pixels changed says little; how much they changed says how strong it is.
long energy(const Frame& a, const Frame& b, int x0, int y0, int x1, int y1) {
    long n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Rgb p = a.at(x, y), q = b.at(x, y);
            n += std::abs(int(p.r) - int(q.r)) + std::abs(int(p.g) - int(q.g)) + std::abs(int(p.b) - int(q.b));
        }
    }
    return n;
}

// Theme.bg and Theme.field differ by only 6-7 per channel, below the harness's default tolerance of 8, and a flat
// fill is exact in the software renderer: every probe of a ground colour uses this.
constexpr int kExact = 2;

// ---- the specimen's table (digits_specimen.slint) ----
struct At { int x, y; };
constexpr At kD1{20, 22}, kD2{80, 22}, kD3{140, 22}, kD4{200, 22}, kD5{260, 22};   // Digits: 5, dim 0, no switches, disabled, tooltip
constexpr At kA{20, 96}, kB{20, 196}, kC{20, 296}, kD{20, 396}, kE{20, 496};       // FrequencyCounters
constexpr int kW = 34, kH = 52, kNudge = 10;                                          // Theme.digit-w / digit-h / nudge-h
constexpr int kPlateW = 397, kPlateH = 58;                                            // 2 + 2*8 + 10*34 + 3*13, 2 + 2*2 + 52
constexpr int kDot = 13;                                                              // a 22 px mono dot: 0.6 em, rounded
constexpr int kUnitY = kPlateH + 2, kUnitH = 12, kCaptionY = kUnitY + kUnitH, kCaptionH = 12;

// x of figure i inside a counter: border + padding, i columns, a dot cell per dot before it (after figures 1, 4, 7)
constexpr int digitX(int i) { return 1 + 8 + i * kW + ((i >= 1 ? 1 : 0) + (i >= 4 ? 1 : 0) + (i >= 7 ? 1 : 0)) * kDot; }
constexpr int kDotX[3] = {digitX(1) - kDot, digitX(4) - kDot, digitX(7) - kDot};
// the four unit labels centre under their groups: GHz = figure 0, MHz = 1..3, kHz = 4..6, Hz = 7..9
constexpr double kUnitCx[4] = {digitX(0) + kW / 2.0, (digitX(1) + digitX(3) + kW) / 2.0, (digitX(4) + digitX(6) + kW) / 2.0, (digitX(7) + digitX(9) + kW) / 2.0};

// How many pixels differ between the numeral band (a figure between its two switch rows) of figure `ia` of counter `ca`
// in frame `a` and that of figure `ib` of counter `cb` in frame `b`: 0 means the same digit drawn the same way.
int bandDiff(const Frame& a, const At& ca, int ia, const Frame& b, const At& cb, int ib) {
    int n = 0;
    for (int y = 0; y < kH - 2 * kNudge; ++y) {
        for (int x = 0; x < kW; ++x) {
            const Rgb p = a.at(ca.x + digitX(ia) + x, ca.y + 3 + kNudge + y), q = b.at(cb.x + digitX(ib) + x, cb.y + 3 + kNudge + y);
            if (p.r != q.r || p.g != q.g || p.b != q.b) { ++n; }
        }
    }
    return n;
}

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = DigitsSpecimen::create();
    slint::Window& win = ui->window();

    // The tokens, read from the Workbench skin by name (the theme test pins their values).
    const Skin wb = ui->global<Workbench>().get_value();
    const Rgb bg = tok(wb.bg), field = tok(wb.field), line = tok(wb.line), lineStrong = tok(wb.line_strong), knobRing = tok(wb.knob_ring);
    const Rgb digit = tok(wb.digit), digitDim = tok(wb.digit_dim), nudge = tok(wb.nudge), faint = tok(wb.faint);
    const Rgb warn = tok(wb.chip_warn_fg), disabledFg = tok(wb.key_disabled_fg), wellDeep = tok(wb.well_deep);
    const Rgb menu = tok(wb.menu), focus = tok(wb.focus), accent = tok(wb.accent), fgSoft = tok(wb.fg_soft);

    const auto reset = [&] {
        ui->set_nudges(0); ui->set_last_dir(0); ui->set_last_digit(-1); ui->set_wheels(0); ui->set_clicks(0);
        ui->set_right_clicks(0); ui->set_typed_text(""); ui->set_cancels(0); ui->set_edit_requests(0); ui->set_menu_requests(0);
        ui->set_last_x(-1); ui->set_last_y(-1);
    };
    // Parks the pointer on the sheet's empty corner: closes any tooltip and ends any hover.
    const auto park = [&] { moveTo(win, 437, 597); settle(milliseconds(30)); };
    const auto typed = [&](const char8_t* s) { keyTap(win, std::u8string_view(s)); };

    // =========================================================================================================
    // 1. The Digit column, Workbench filling (plain face): 34 x 52, nudge rows 10 px at top and bottom
    // =========================================================================================================
    Frame f0 = render(ui, 440, 600);
    CHECK(f0.width == 440 && f0.height == 600);
    CHECK(f0.near(437, 597, bg, kExact));                                                   // the sheet's ground

    // value 5: the numeral in the middle band is Theme.digit, the arrows sit in the 10 px rows above and below it
    CHECKV(f0.countNear(kD1.x, kD1.y + kNudge, kD1.x + kW, kD1.y + kH - kNudge, digit, 12) > 40,
           "numeral ink near digit: %d", f0.countNear(kD1.x, kD1.y + kNudge, kD1.x + kW, kD1.y + kH - kNudge, digit, 12));
    const Ink upArrow = inkBox(f0, kD1.x, kD1.y, kD1.x + kW, kD1.y + kNudge, bg);
    const Ink downArrow = inkBox(f0, kD1.x, kD1.y + kH - kNudge, kD1.x + kW, kD1.y + kH, bg);
    CHECKV(f0.countNear(kD1.x, kD1.y, kD1.x + kW, kD1.y + kNudge, nudge, 20) >= 6, "up arrow ink in nudge colour: %d", upArrow.n);
    CHECKV(f0.countNear(kD1.x, kD1.y + kH - kNudge, kD1.x + kW, kD1.y + kH, nudge, 20) >= 6, "down arrow ink in nudge colour: %d", downArrow.n);
    CHECK(f0.countNear(kD1.x, kD1.y + kNudge, kD1.x + kW, kD1.y + kH - kNudge, nudge, 8) == 0);   // no arrow colour in the numeral band
    CHECKV(nearColour(brightest(f0, kD1.x, kD1.y, kD1.x + kW, kD1.y + kNudge), nudge, 12) && nearColour(brightest(f0, kD1.x, kD1.y + kH - kNudge, kD1.x + kW, kD1.y + kH), nudge, 12),
           "arrow colour %s / %s (want %s)", hex(brightest(f0, kD1.x, kD1.y, kD1.x + kW, kD1.y + kNudge)).c_str(), hex(brightest(f0, kD1.x, kD1.y + kH - kNudge, kD1.x + kW, kD1.y + kH)).c_str(), hex(nudge).c_str());
    CHECKV(std::abs(upArrow.cx() - (kD1.x + kW / 2.0)) <= 1.5 && std::abs(downArrow.cx() - (kD1.x + kW / 2.0)) <= 1.5,
           "arrow centres %.1f / %.1f vs %.1f", upArrow.cx(), downArrow.cx(), kD1.x + kW / 2.0);
    CHECKV(std::abs(upArrow.cy() - (kD1.y + kNudge / 2.0)) <= 2.5 && std::abs(downArrow.cy() - (kD1.y + kH - kNudge / 2.0)) <= 2.5,
           "arrow rows centre %.1f / %.1f", upArrow.cy(), downArrow.cy());
    // the numeral sits between the rows and is centred on the column (the glow reaches a few px beyond the glyph)
    const Ink five = inkBox(f0, kD1.x, kD1.y + kNudge, kD1.x + kW, kD1.y + kH - kNudge, bg, 100);
    CHECKV(five.n > 0 && std::abs(five.cx() - (kD1.x + kW / 2.0)) <= 2.0 && std::abs(five.cy() - (kD1.y + kH / 2.0)) <= 3.0,
           "numeral box centre %.1f,%.1f vs %.1f,%.1f", five.cx(), five.cy(), kD1.x + kW / 2.0, kD1.y + kH / 2.0);

    // value 0, dim: the leading-zero brown, no bright amber anywhere in the column
    CHECKV(f0.countNear(kD2.x, kD2.y + kNudge, kD2.x + kW, kD2.y + kH - kNudge, digitDim, 12) > 30,
           "dim numeral ink: %d", f0.countNear(kD2.x, kD2.y + kNudge, kD2.x + kW, kD2.y + kH - kNudge, digitDim, 12));
    CHECK(f0.countNear(kD2.x, kD2.y, kD2.x + kW, kD2.y + kH, digit, 12) == 0);
    CHECK(f0.countNear(kD2.x, kD2.y, kD2.x + kW, kD2.y + kNudge, nudge, 20) >= 6);  // a dim digit keeps its arrows

    // show-nudges false: the numeral stays where it was, the two rows are empty
    CHECK(f0.countNear(kD3.x, kD3.y + kNudge, kD3.x + kW, kD3.y + kH - kNudge, digit, 12) > 40);
    CHECK(f0.countInk(kD3.x, kD3.y, kD3.x + kW, kD3.y + kNudge, bg) == 0);
    CHECK(f0.countInk(kD3.x, kD3.y + kH - kNudge, kD3.x + kW, kD3.y + kH, bg) == 0);
    CHECK(f0.countNear(kD3.x, kD3.y, kD3.x + kW, kD3.y + kH, nudge, 8) == 0);

    // disabled: grey numeral (key-disabled-fg), no amber, dimmer arrows
    CHECK(f0.countNear(kD4.x, kD4.y + kNudge, kD4.x + kW, kD4.y + kH - kNudge, disabledFg, 12) > 40);
    CHECK(f0.countNear(kD4.x, kD4.y, kD4.x + kW, kD4.y + kH, digit, 12) == 0);
    CHECK(f0.countNear(kD4.x, kD4.y, kD4.x + kW, kD4.y + kNudge, nudge, 8) == 0);   // not the full-strength arrow colour
    CHECK(f0.countInk(kD4.x, kD4.y, kD4.x + kW, kD4.y + kNudge, bg, 12) >= 6);      // ...but still drawn
    const Rgb dimArrow = brightest(f0, kD4.x, kD4.y, kD4.x + kW, kD4.y + kNudge);    // half the strength: the nudge colour at 50 % over the ground
    CHECKV(nearColour(dimArrow, over(nudge, 0.5, bg), 16), "disabled arrow colour %s (want about %s)", hex(dimArrow).c_str(), hex(over(nudge, 0.5, bg)).c_str());

    // the glow: a skin whose glow is transparent changes the bright digit and nothing else of the dim or disabled ones
    {
        Skin dark = wb;
        dark.digit_glow = slint::Color::from_argb_uint8(0, 0, 0, 0);
        ui->global<Theme>().set_skin(dark);
        Frame g = render(ui, 440, 600);
        const int bright = differing(f0, g, kD1.x, kD1.y, kD1.x + kW, kD1.y + kH);
        CHECKV(bright > 150, "glow pixels around the bright numeral: %d", bright);
        CHECK(differing(f0, g, kD2.x, kD2.y, kD2.x + kW, kD2.y + kH) == 0);     // a leading zero casts none
        CHECK(differing(f0, g, kD4.x, kD4.y, kD4.x + kW, kD4.y + kH) == 0);     // nor does a disabled digit
        CHECK(differing(f0, g, kD1.x, kD1.y, kD1.x + kW, kD1.y + kNudge) == 0); // and it never reaches the nudge row
        ui->global<Theme>().set_skin(wb);
    }

    // =========================================================================================================
    // 2. The Digit's input: nudge on press and auto-repeat, wheel, click, right-click, disabled, hidden switches
    // =========================================================================================================
    const float cx1 = kD1.x + kW / 2.0f;                                   // the column's centre line
    const float upY = kD1.y + kNudge / 2.0f, midY = kD1.y + kH / 2.0f, downY = kD1.y + kH - kNudge / 2.0f;

    reset();
    click(win, cx1, upY);
    CHECKV(ui->get_nudges() == 1 && ui->get_last_dir() == 1, "up arrow click: %d nudges, dir %d", ui->get_nudges(), ui->get_last_dir());
    CHECK(ui->get_clicks() == 0 && ui->get_wheels() == 0);
    reset();
    click(win, cx1, downY);
    CHECKV(ui->get_nudges() == 1 && ui->get_last_dir() == -1, "down arrow click: %d nudges, dir %d", ui->get_nudges(), ui->get_last_dir());
    CHECK(ui->get_clicks() == 0);
    park();

    // hit geometry, to the pixel: the column is 34 wide and 52 high; rows 0..9 and 42..51 are the switches
    reset();
    click(win, kD1.x + 0.5f, kD1.y + 0.5f);                    CHECK(ui->get_nudges() == 1 && ui->get_last_dir() == 1);     // top-left pixel
    click(win, kD1.x + kW - 0.5f, kD1.y + kNudge - 0.5f);      CHECK(ui->get_nudges() == 2 && ui->get_last_dir() == 1);     // last pixel of the up row
    click(win, kD1.x + kW - 0.5f, kD1.y + kH - 0.5f);          CHECK(ui->get_nudges() == 3 && ui->get_last_dir() == -1);    // bottom-right pixel
    click(win, kD1.x + 0.5f, kD1.y + kH - kNudge + 0.5f);      CHECK(ui->get_nudges() == 4 && ui->get_last_dir() == -1);    // first pixel of the down row
    CHECK(ui->get_clicks() == 0);
    click(win, cx1, kD1.y + kNudge + 0.5f);                    CHECK(ui->get_clicks() == 1 && ui->get_nudges() == 4);       // first pixel of the numeral
    click(win, cx1, kD1.y + kH - kNudge - 0.5f);               CHECK(ui->get_clicks() == 2 && ui->get_nudges() == 4);       // last pixel of the numeral
    const int before = ui->get_nudges() + ui->get_clicks() + ui->get_wheels() + ui->get_right_clicks();
    click(win, kD1.x - 0.5f, midY);                            // one pixel left of the column
    click(win, kD1.x + kW + 0.5f, midY);                       // one pixel right of it
    click(win, cx1, kD1.y - 0.5f);                             // one pixel above
    click(win, cx1, kD1.y + kH + 0.5f);                        // one pixel below
    CHECK(ui->get_nudges() + ui->get_clicks() + ui->get_wheels() + ui->get_right_clicks() == before);
    park();

    // a click on the numeral fires `clicked`, and only that
    reset();
    click(win, cx1, midY);
    CHECK(ui->get_clicks() == 1 && ui->get_nudges() == 0 && ui->get_wheels() == 0 && ui->get_right_clicks() == 0);
    park();

    // a 200 ms press fires exactly one nudge (the press itself); no repeat before 400 ms
    reset();
    moveTo(win, cx1, upY);
    press(win, cx1, upY);
    CHECK(ui->get_nudges() == 1);
    settle(milliseconds(200));
    release(win, cx1, upY);
    CHECKV(ui->get_nudges() == 1, "after a 200 ms press: %d nudges", ui->get_nudges());
    settle(milliseconds(500));                                  // released: nothing more ever comes
    CHECKV(ui->get_nudges() == 1, "after release and 500 ms: %d nudges", ui->get_nudges());
    park();

    // held 600 ms: the press, then repeats at 400, 480 and 560 ms
    reset();
    moveTo(win, cx1, upY);
    press(win, cx1, upY);
    settle(milliseconds(330));
    CHECKV(ui->get_nudges() == 1, "after 330 ms held (first repeat is at 400): %d nudges", ui->get_nudges());
    settle(milliseconds(270));
    const int held = ui->get_nudges();
    CHECKV(held >= 3 && held <= 6 && ui->get_last_dir() == 1, "after 600 ms held: %d nudges, dir %d (want 4: 1 + 400/480/560)", held, ui->get_last_dir());
    release(win, cx1, upY);
    settle(milliseconds(300));
    CHECKV(ui->get_nudges() <= held + 1, "after release: %d nudges (held %d)", ui->get_nudges(), held);
    park();

    // the down switch repeats too, in its own direction
    reset();
    moveTo(win, cx1, downY);
    press(win, cx1, downY);
    settle(milliseconds(600));
    release(win, cx1, downY);
    CHECKV(ui->get_nudges() >= 3 && ui->get_last_dir() == -1, "down held 600 ms: %d nudges, dir %d", ui->get_nudges(), ui->get_last_dir());
    park();

    // the wheel: a positive delta (the wheel turning away from the user) is up, negative is down; over the whole column
    reset();
    scroll(win, cx1, midY, 0, 60);
    CHECK(ui->get_wheels() == 1 && ui->get_last_dir() == 1);
    scroll(win, cx1, midY, 0, -60);
    CHECK(ui->get_wheels() == 2 && ui->get_last_dir() == -1);
    scroll(win, cx1, upY, 0, 60);                               // over the up switch
    CHECK(ui->get_wheels() == 3 && ui->get_last_dir() == 1);
    scroll(win, cx1, downY, 0, -60);                            // over the down switch
    CHECK(ui->get_wheels() == 4 && ui->get_last_dir() == -1);
    scroll(win, cx1, midY, 0, 0);                               // no vertical travel: not a notch
    scroll(win, cx1, midY, 30, 0);                              // sideways: not a notch either
    CHECK(ui->get_wheels() == 4);
    CHECK(ui->get_nudges() == 0 && ui->get_clicks() == 0);
    scroll(win, kD1.x + kW + 3.0f, midY, 0, 60);                // beside the column
    CHECK(ui->get_wheels() == 4);
    park();

    // right-click: reported with its position in the Digit, from the numeral and from a switch
    reset();
    click(win, kD1.x + 10.5f, kD1.y + 25.5f, slint::PointerEventButton::Right);
    CHECKV(ui->get_right_clicks() == 1 && std::abs(ui->get_last_x() - 10.5f) <= 1.0f && std::abs(ui->get_last_y() - 25.5f) <= 1.0f,
           "right-click on the numeral: %d at %.1f,%.1f", ui->get_right_clicks(), ui->get_last_x(), ui->get_last_y());
    click(win, kD1.x + 20.5f, kD1.y + 4.5f, slint::PointerEventButton::Right);
    CHECKV(ui->get_right_clicks() == 2 && std::abs(ui->get_last_x() - 20.5f) <= 1.0f && std::abs(ui->get_last_y() - 4.5f) <= 1.0f,
           "right-click on the up switch: %d at %.1f,%.1f", ui->get_right_clicks(), ui->get_last_x(), ui->get_last_y());
    CHECK(ui->get_nudges() == 0 && ui->get_clicks() == 0);      // a right press is not a nudge and not a click
    park();

    // show-nudges false: no switches (nothing nudges), the wheel still steps, a click anywhere in the column types
    reset();
    const float cx3 = kD3.x + kW / 2.0f;
    click(win, cx3, kD3.y + 4.5f);                              // where the up switch would be
    click(win, cx3, kD3.y + kH - 4.5f);                         // where the down switch would be
    CHECK(ui->get_nudges() == 0);
    CHECK(ui->get_clicks() == 2);                               // the blank rows are part of the click target
    scroll(win, cx3, kD3.y + 4.5f, 0, 60);
    scroll(win, cx3, kD3.y + kH / 2.0f, 0, -60);
    CHECK(ui->get_wheels() == 2 && ui->get_last_dir() == -1);
    moveTo(win, cx3, kD3.y + 4.5f);
    press(win, cx3, kD3.y + 4.5f);
    settle(milliseconds(500));                                  // a held press where no switch exists never repeats
    release(win, cx3, kD3.y + 4.5f);
    CHECK(ui->get_nudges() == 0);
    park();

    // disabled: nothing fires, however it is pressed, held, scrolled or clicked
    reset();
    const float cx4 = kD4.x + kW / 2.0f;
    click(win, cx4, kD4.y + 4.5f);
    click(win, cx4, kD4.y + kH - 4.5f);
    click(win, cx4, kD4.y + kH / 2.0f);
    click(win, cx4, kD4.y + kH / 2.0f, slint::PointerEventButton::Right);
    scroll(win, cx4, kD4.y + kH / 2.0f, 0, 60);
    scroll(win, cx4, kD4.y + 4.5f, 0, -60);
    moveTo(win, cx4, kD4.y + 4.5f);
    press(win, cx4, kD4.y + 4.5f);
    settle(milliseconds(500));
    release(win, cx4, kD4.y + 4.5f);
    CHECKV(ui->get_nudges() == 0 && ui->get_wheels() == 0 && ui->get_clicks() == 0 && ui->get_right_clicks() == 0,
           "disabled Digit fired: %d nudges, %d wheels, %d clicks, %d right-clicks", ui->get_nudges(), ui->get_wheels(), ui->get_clicks(), ui->get_right_clicks());
    park();

    // the tooltip comes through TipTimer: after the pointer rests, the shared tip face (menu ground) opens under the column
    {
        Frame before_tip = render(ui, 440, 600);
        CHECK(before_tip.near(kD5.x + 10, kD5.y + kH + 9, bg, kExact));                              // nothing there yet
        moveTo(win, kD5.x + kW / 2.0f, kD5.y + kH / 2.0f);
        settle(milliseconds(750));
        Frame tip = render(ui, 440, 600);
        CHECKV(tip.near(kD5.x + 10, kD5.y + kH + 9, menu), "tooltip ground at (%d,%d): %s", kD5.x + 10, kD5.y + kH + 9, hex(tip.at(kD5.x + 10, kD5.y + kH + 9)).c_str());
        CHECKV(tip.near(kD5.x + 40, kD5.y + kH + 6, knobRing, 4), "tip rim row: %s", hex(tip.at(kD5.x + 40, kD5.y + kH + 6)).c_str());   // its top rim: 6 px under the column
        CHECKV(tip.countNear(kD5.x, kD5.y + kH + 6, kD5.x + 170, kD5.y + kH + 6 + 30, fgSoft, 40) > 30, "the tip's sentence: %d ink pixels", tip.countNear(kD5.x, kD5.y + kH + 6, kD5.x + 170, kD5.y + kH + 6 + 30, fgSoft, 40));
        CHECK(nearColour(brightest(tip, kD5.x + 8, kD5.y + kH + 8, kD5.x + 170, kD5.y + kH + 6 + 28), fgSoft, 12));   // in Theme.fg-soft
        park();
        Frame gone = render(ui, 440, 600);
        CHECK(gone.near(kD5.x + 10, kD5.y + kH + 9, bg, kExact));                                    // and it closes when the pointer leaves
        // a Digit without a tooltip never opens one
        moveTo(win, cx1, midY);
        settle(milliseconds(750));
        Frame none = render(ui, 440, 600);
        CHECK(none.near(kD1.x + 10, kD1.y + kH + 9, bg, kExact));
        park();
    }

    // =========================================================================================================
    // 3. The three faces, from Theme.digit-face: plain (the first picture), drum, nixie; a skin copy installed and restored
    // =========================================================================================================
    // the dim zero is probed: it has no glow to tint the ground. Cell = x 2..32, y 11..41 of the column.
    const int cellL = kD2.x + 2, cellR = kD2.x + kW - 3, cellT = kD2.y + kNudge + 1, cellB = kD2.y + kH - kNudge - 2;   // border pixels
    const int midRow = (cellT + cellB) / 2, probeX = cellL + 2;
    // plain: the ground is the sheet's, the cell does not exist
    CHECK(f0.near(cellL, midRow, bg, kExact) && f0.near(probeX, midRow, bg, kExact) && f0.near(cellR, midRow, bg, kExact));
    CHECK(f0.rowNear(cellT, cellL + 6, cellR - 5, bg, kExact));

    Skin drumSkin = wb;
    drumSkin.digit_face = DigitFace::Drum;
    ui->global<Theme>().set_skin(drumSkin);
    Frame drum = render(ui, 440, 600);
    CHECK(ui->global<Theme>().get_digit_face() == DigitFace::Drum);
    CHECKV(drum.colNear(cellL, cellT + 5, cellB - 4, line) && drum.colNear(cellR, cellT + 5, cellB - 4, line),
           "drum left/right border: %s %s", hex(drum.at(cellL, midRow)).c_str(), hex(drum.at(cellR, midRow)).c_str());
    CHECK(drum.rowNear(cellT, cellL + 5, cellR - 4, line) && drum.rowNear(cellB, cellL + 5, cellR - 4, line));
    CHECK(drum.near(probeX, midRow, field, kExact));                                    // the cell ground
    CHECK(drum.near(cellL - 1, midRow, bg, kExact) && drum.near(cellR + 1, midRow, bg, kExact));// the sheet's ground just outside the cell
    CHECKV(drum.near(cellL, cellT, bg, kExact), "drum corner pixel %s", hex(drum.at(cellL, cellT)).c_str());   // radius 4: the corner is rounded away
    CHECK(drum.countNear(kD2.x, kD2.y + kNudge, kD2.x + kW, kD2.y + kH - kNudge, digitDim, 12) > 30);   // the numeral is still drawn
    // the cell is the same width on every digit: counter A's figure 1 has its cell at the same inset
    CHECK(drum.colNear(kA.x + digitX(1) + 2, kA.y + 3 + kNudge + 6, kA.y + 3 + kH - kNudge - 5, line));
    CHECK(drum.countNear(kA.x + digitX(1), kA.y + 3 + kNudge, kA.x + digitX(1) + kW, kA.y + 3 + kH - kNudge, digit, 12) > 30);
    drum.savePng(shotDir() + "digits-specimen-drum.png");
    const long drumGlow = [&] {                                                 // how much light the glow adds around the bright 5
        Skin s = drumSkin; s.digit_glow = slint::Color::from_argb_uint8(0, 0, 0, 0);
        ui->global<Theme>().set_skin(s);
        Frame g = render(ui, 440, 600);
        ui->global<Theme>().set_skin(drumSkin);
        return energy(drum, g, kD1.x, kD1.y, kD1.x + kW, kD1.y + kH);
    }();

    Skin nixieSkin = wb;
    nixieSkin.digit_face = DigitFace::Nixie;
    ui->global<Theme>().set_skin(nixieSkin);
    Frame nixie = render(ui, 440, 600);
    CHECK(ui->global<Theme>().get_digit_face() == DigitFace::Nixie);
    CHECKV(nixie.near(probeX, midRow, wellDeep, 3), "nixie tube ground: %s", hex(nixie.at(probeX, midRow)).c_str());   // Theme.well-deep
    CHECK(nixie.near(cellL, midRow, knobRing) && nixie.near(cellR, midRow, knobRing));                               // glass rim, Theme.knob-ring
    CHECK(nixie.rowNear(cellT, cellL + 9, cellR - 8, knobRing) && nixie.rowNear(cellB, cellL + 9, cellR - 8, knobRing));
    CHECKV(nixie.near(cellL, cellT, bg, kExact), "nixie corner pixel %s", hex(nixie.at(cellL, cellT)).c_str());                                    // radius-panel (8): the corner is rounded away
    CHECK(nixie.near(cellL - 1, midRow, bg, kExact) && nixie.near(cellR + 1, midRow, bg, kExact));
    CHECK(nixie.countNear(kD2.x, kD2.y + kNudge, kD2.x + kW, kD2.y + kH - kNudge, digitDim, 12) > 30);
    CHECK(nixie.near(kA.x + digitX(5) + 4, kA.y + 3 + kH / 2, wellDeep, 3));       // the same tube on a counter figure
    nixie.savePng(shotDir() + "digits-specimen-nixie.png");
    // the nixie glow is the stronger one (the drum's is the plain face's): how much light switching it off takes away
    {
        Skin s = nixieSkin; s.digit_glow = slint::Color::from_argb_uint8(0, 0, 0, 0);
        ui->global<Theme>().set_skin(s);
        Frame g = render(ui, 440, 600);
        const long nixieGlow = energy(nixie, g, kD1.x, kD1.y, kD1.x + kW, kD1.y + kH);
        Skin p = wb; p.digit_glow = slint::Color::from_argb_uint8(0, 0, 0, 0);
        ui->global<Theme>().set_skin(p);
        Frame pg = render(ui, 440, 600);
        const long plainGlow = energy(f0, pg, kD1.x, kD1.y, kD1.x + kW, kD1.y + kH);
        CHECKV(plainGlow > 5000 && nixieGlow * 2 > plainGlow * 3 && std::labs(drumGlow - plainGlow) * 10 <= plainGlow,
               "glow energy: plain %ld, drum %ld, nixie %ld", plainGlow, drumGlow, nixieGlow);
    }

    // and back to Workbench: the first picture, exactly
    ui->global<Theme>().set_skin(ui->global<Workbench>().get_value());
    Frame back = render(ui, 440, 600);
    CHECK(ui->global<Theme>().get_digit_face() == DigitFace::Plain);
    CHECK(differing(f0, back, 0, 0, 440, 600) == 0);

    // =========================================================================================================
    // 3b. Every part is drawn from ITS OWN token. Anti-aliasing blends light text into its ground and the Workbench
    //     greys lie within 17 of one another, so the colour of a glyph read from pixels cannot tell Theme.faint from
    //     Theme.nudge. A token re-coloured to something loud can: whatever turns that colour is drawn from it, and
    //     nothing else may turn it.
    // =========================================================================================================
    {
        const Rgb green = rgb(0x00ff00), red = rgb(0xff0000), blue = rgb(0x0040ff), cyan = rgb(0x00ffff), violet = rgb(0xa000ff), magenta = rgb(0xff00ff);
        const Rgb flatA = rgb(0x20c020), flatB = rgb(0xc0a020);                  // flat fills are exact, so these are probed exactly
        const auto col = [](Rgb c) { return slint::Color::from_rgb_uint8(c.r, c.g, c.b); };
        const auto loud = [&](const Skin& s) {                                   // the sheet in this filling; Workbench goes back after
            ui->global<Theme>().set_skin(s);
            Frame g = render(ui, 440, 600);
            ui->global<Theme>().set_skin(wb);
            return g;
        };
        const auto lit = [](const Frame& g, int x0, int y0, int x1, int y1, Rgb c) { return g.countNear(x0, y0, x1, y1, c, 70); };   // 70: an amber glyph edge blended into the dark ground is 105+ from pure red
        // regions: a figure's numeral band, its two switch rows, a unit label, the caption line, the plate
        const auto band = [](const At& c, int i) { return std::array<int, 4>{c.x + digitX(i), c.y + 3 + kNudge, c.x + digitX(i) + kW, c.y + 3 + kH - kNudge}; };
        const auto rows = [](const At& c, int i, bool up) { const int y = c.y + 3 + (up ? 0 : kH - kNudge); return std::array<int, 4>{c.x + digitX(i), y, c.x + digitX(i) + kW, y + kNudge}; };
        const auto litIn = [&](const Frame& g, const std::array<int, 4>& r, Rgb c) { return lit(g, r[0], r[1], r[2], r[3], c); };
        const int unitTop = kA.y + kUnitY, unitBot = unitTop + kUnitH;
        const auto unitBox = [&](int k) { const int lx = kA.x + static_cast<int>(kUnitCx[k]); return std::array<int, 4>{lx - 20, unitTop, lx + 20, unitBot}; };
        const auto capBox = [](const At& c) { return std::array<int, 4>{c.x, c.y + kCaptionY, c.x + kPlateW, c.y + kCaptionY + kCaptionH}; };
        const auto plateBox = [](const At& c) { return std::array<int, 4>{c.x, c.y, c.x + kPlateW, c.y + kPlateH}; };
        const std::array<int, 4> digitsBox{0, 0, 440, kA.y + kPlateH};          // the five Digits and counter A's plate

        {   // Theme.faint: the four unit labels and the engraving, nothing else in sight
            Skin s = wb; s.faint = col(green);
            const Frame g = loud(s);
            for (int k = 0; k < 4; ++k) { CHECKV(litIn(g, unitBox(k), green) >= 3, "unit %d is not drawn from Theme.faint", k); }
            CHECKV(litIn(g, capBox(kA), green) >= 8, "the engraving is not drawn from Theme.faint (%d)", litIn(g, capBox(kA), green));
            CHECK(litIn(g, capBox(kC), green) == 0);                            // the converter line is warning text, not engraving
            CHECK(litIn(g, digitsBox, green) == 0);                             // and no figure, arrow or dot is faint
        }
        {   // Theme.nudge: every switch arrow, and only those
            Skin s = wb; s.nudge = col(red);
            const Frame g = loud(s);
            for (int i = 0; i < 10; ++i) { CHECK(litIn(g, rows(kA, i, true), red) >= 6 && litIn(g, rows(kA, i, false), red) >= 6); }
            CHECK(lit(g, kD1.x, kD1.y, kD1.x + kW, kD1.y + kNudge, red) >= 6 && lit(g, kD1.x, kD1.y + kH - kNudge, kD1.x + kW, kD1.y + kH, red) >= 6);
            CHECKV(litIn(g, capBox(kA), red) == 0, "the engraving is drawn from Theme.nudge (%d)", litIn(g, capBox(kA), red));
            CHECK(litIn(g, capBox(kC), red) == 0);
            CHECK(lit(g, kA.x, unitTop, kA.x + kPlateW, unitBot, red) == 0);    // the unit row is not nudge grey
            for (int i = 0; i < 10; ++i) { CHECK(litIn(g, band(kA, i), red) == 0); }
        }
        {   // Theme.chip-warn-fg: the converter line, and not the engraving
            Skin s = wb; s.chip_warn_fg = col(blue);
            const Frame g = loud(s);
            CHECKV(litIn(g, capBox(kC), blue) >= 12, "the converter line is not drawn from Theme.chip-warn-fg (%d)", litIn(g, capBox(kC), blue));
            CHECK(litIn(g, capBox(kA), blue) == 0 && litIn(g, capBox(kD), blue) == 0 && litIn(g, capBox(kE), blue) == 0);
        }
        {   // Theme.knob-ring: the three dots, and not a figure. A full stop is 3 x 2 pixels, so count every pixel with the
            // magenta cast (red and blue both well above green), partly covered ones included
            Skin s = wb; s.knob_ring = col(magenta);
            const Frame g = loud(s);
            const auto magentaish = [&](int x0, int y0, int x1, int y1) {
                int n = 0;
                for (int y = y0; y < y1; ++y) { for (int x = x0; x < x1; ++x) { const Rgb p = g.at(x, y); if (p.r > p.g + 30 && p.b > p.g + 30) { ++n; } } }
                return n;
            };
            for (int k = 0; k < 3; ++k) {
                const int dx = kA.x + kDotX[k];
                CHECKV(magentaish(dx, kA.y + 3, dx + kDot, kA.y + 3 + kH) >= 5, "dot %d is not drawn from Theme.knob-ring (%d magenta pixels)", k, magentaish(dx, kA.y + 3, dx + kDot, kA.y + 3 + kH));
            }
            for (int i = 0; i < 10; ++i) { CHECK(magentaish(kA.x + digitX(i), kA.y + 3, kA.x + digitX(i) + kW, kA.y + 3 + kH) == 0); }
            CHECK(magentaish(kD1.x, kD1.y, kD5.x + kW, kD1.y + kH) == 0);
        }
        {   // Theme.digit-dim: the leading zeros (and only them)
            Skin s = wb; s.digit_dim = col(cyan);
            const Frame g = loud(s);
            CHECK(litIn(g, {kD2.x, kD2.y + kNudge, kD2.x + kW, kD2.y + kH - kNudge}, cyan) >= 30);
            CHECK(litIn(g, band(kA, 0), cyan) >= 30 && litIn(g, band(kE, 0), cyan) >= 30 && litIn(g, band(kE, 1), cyan) >= 30);
            for (int i = 0; i < 3; ++i) { CHECK(litIn(g, band(kD, i), cyan) >= 30); }
            for (int i = 1; i < 10; ++i) { CHECK(litIn(g, band(kA, i), cyan) == 0); }      // A has one leading zero
            CHECK(lit(g, kD1.x, kD1.y + kNudge, kD1.x + kW, kD1.y + kH - kNudge, cyan) == 0);   // the bright 5
        }
        {   // Theme.key-disabled-fg: a disabled Digit's numeral and a disabled counter's figures
            Skin s = wb; s.key_disabled_fg = col(violet);
            const Frame g = loud(s);
            CHECK(lit(g, kD4.x, kD4.y + kNudge, kD4.x + kW, kD4.y + kH - kNudge, violet) >= 30);
            for (int i = 2; i < 10; ++i) { CHECK(litIn(g, band(kE, i), violet) >= 30); }
            CHECK(lit(g, 0, 0, kD4.x, kA.y + kPlateH, violet) == 0 && lit(g, kD4.x + kW, 0, 440, kA.y + kPlateH, violet) == 0);   // no enabled Digit, no counter A
            CHECK(litIn(g, band(kE, 0), violet) == 0 && litIn(g, band(kE, 1), violet) == 0);                                      // a disabled leading zero stays dim
        }
        {   // Theme.digit: the bright numerals and the editor's text
            Skin s = wb; s.digit = col(blue);
            const Frame g = loud(s);
            CHECK(lit(g, kD1.x, kD1.y + kNudge, kD1.x + kW, kD1.y + kH - kNudge, blue) >= 40);
            for (int i = 1; i < 10; ++i) { CHECK(litIn(g, band(kA, i), blue) >= 30); }
            CHECK(lit(g, kB.x + digitX(0) + 8, kB.y + 3 + 8, kB.x + digitX(9), kB.y + 3 + kH - 8, blue) >= 60);   // the editor's text
            CHECK(lit(g, kD2.x, kD2.y, kD2.x + kW, kD2.y + kH, blue) == 0 && lit(g, kD4.x, kD4.y, kD4.x + kW, kD4.y + kH, blue) == 0);
            CHECK(litIn(g, band(kA, 0), blue) == 0);
        }
        {   // grounds and rims, exactly
            Skin s = wb; s.field = col(green); s.line = col(red); s.line_strong = col(blue);
            const Frame g = loud(s);
            CHECK(g.near(kA.x + 200, kA.y + 1, green, kExact) && g.near(kA.x + 200, kA.y + kPlateH - 2, green, kExact));     // the plate ground
            CHECK(g.near(kB.x + digitX(0) + 200, kB.y + 3 + 2, green, kExact));                                               // the editor's ground
            CHECK(g.near(kA.x, kA.y + kPlateH / 2, red, kExact) && g.near(kA.x + kPlateW - 1, kA.y + kPlateH / 2, red, kExact) &&
                  g.near(kA.x + 200, kA.y, red, kExact) && g.near(kA.x + 200, kA.y + kPlateH - 1, red, kExact));              // the plate border
            CHECK(g.near(kB.x + digitX(0), kB.y + 3 + 20, blue, kExact) && g.near(kB.x + digitX(0) + 20, kB.y + 3, blue, kExact));   // the editor's rim
            CHECK(g.near(437, 597, bg, kExact) && g.near(kA.x + 200, kA.y + kPlateH, bg, kExact));                            // the sheet's own ground is not any of them
        }
        {   // the drum cell: Theme.field ground, Theme.line border
            Skin s = wb; s.digit_face = DigitFace::Drum; s.field = col(flatA); s.line = col(flatB);
            const Frame g = loud(s);
            CHECK(g.near(probeX, midRow, flatA, kExact) && g.near(cellL, midRow, flatB, kExact) && g.near(cellR, midRow, flatB, kExact));
            CHECK(g.rowNear(cellT, cellL + 5, cellR - 4, flatB, kExact) && g.rowNear(cellB, cellL + 5, cellR - 4, flatB, kExact));
        }
        {   // the nixie tube: Theme.well-deep ground, Theme.knob-ring rim
            Skin s = wb; s.digit_face = DigitFace::Nixie; s.well_deep = col(flatA); s.knob_ring = col(flatB);
            const Frame g = loud(s);
            CHECK(g.near(probeX, midRow, flatA, kExact) && g.near(cellL, midRow, flatB, kExact) && g.near(cellR, midRow, flatB, kExact));
            CHECK(g.rowNear(cellT, cellL + 9, cellR - 8, flatB, kExact) && g.rowNear(cellB, cellL + 9, cellR - 8, flatB, kExact));
        }
        {   // the glow reaches a few pixels beyond the glyph (the sheet's is a 10 px blur; this is as far as the copies go)
            Skin on = wb; on.digit_glow = slint::Color::from_argb_uint8(255, 0, 255, 0);
            Skin off = wb; off.digit_glow = slint::Color::from_argb_uint8(0, 0, 0, 0);
            const Frame gOn = loud(on), gOff = loud(off);
            int hx0 = 1 << 30, hx1 = -1, hy0 = 1 << 30, hy1 = -1, hn = 0;
            for (int y = kD1.y + kNudge; y < kD1.y + kH - kNudge; ++y) {
                for (int x = kD1.x; x < kD1.x + kW; ++x) {
                    const Rgb p = gOn.at(x, y);
                    if (p.g > p.r + 12 && p.g > p.b + 12) { ++hn; hx0 = std::min(hx0, x); hx1 = std::max(hx1, x); hy0 = std::min(hy0, y); hy1 = std::max(hy1, y); }
                }
            }
            const Ink glyph = inkBox(gOff, kD1.x, kD1.y + kNudge, kD1.x + kW, kD1.y + kH - kNudge, bg, 24);
            const int reachL = glyph.x0 - hx0, reachR = hx1 - glyph.x1, reachT = glyph.y0 - hy0;
            CHECKV(hn > 100 && reachL >= 2 && reachR >= 2 && reachT >= 2 && reachL <= 6 && reachR <= 6 && reachT <= 6,
                   "glow: %d pixels, reach left %d right %d top %d beyond the glyph", hn, reachL, reachR, reachT);
        }
    }

    // =========================================================================================================
    // 4. FrequencyCounter A: the plate, the dots, the dim leading zero, the unit row, the engraving
    // =========================================================================================================
    const int ay = kA.y + kPlateH / 2;                                          // a row through the middle of the plate
    const int plateL = f0.firstColNear(ay, kA.x - 4, kA.x + 40, line);
    int plateR = -1;
    for (int x = kA.x + kPlateW + 8; x > kA.x + kPlateW - 40; --x) { if (f0.near(x, ay, line)) { plateR = x; break; } }
    CHECKV(plateL == kA.x, "plate left border at %d (want %d)", plateL, kA.x);
    const int plateW = plateR - plateL + 1;
    CHECKV(plateW == kPlateW, "plate width %d (want %d = 2 + 16 + 10*34 + 3 dot cells)", plateW, kPlateW);
    CHECK((plateW - 2 - 16 - 10 * kW) % 3 == 0 && (plateW - 2 - 16 - 10 * kW) / 3 == kDot);   // the dot cell, derived from the frame
    CHECK(f0.near(plateR + 1, ay, bg, kExact));                                         // nothing beyond the right border
    const int plateT = f0.firstRowNear(kA.x + 200, kA.y - 6, kA.y + 20, line);
    int plateB = -1;
    for (int y = kA.y + kPlateH + 6; y > kA.y + kPlateH - 20; --y) { if (f0.near(kA.x + 200, y, line)) { plateB = y; break; } }
    CHECKV(plateT == kA.y && plateB - plateT + 1 == kPlateH, "plate rows %d..%d (want %d high)", plateT, plateB, kPlateH);
    CHECK(f0.near(kA.x + 200, kA.y + 1, field, kExact) && f0.near(kA.x + 200, kA.y + kPlateH - 2, field, kExact));   // ground: Theme.field
    CHECK(f0.near(kA.x + 3, ay, field, kExact) && f0.near(kA.x + 1 + 6, ay, field, kExact));                         // ...in the padding too
    CHECKV(f0.near(kA.x, kA.y, bg, kExact), "plate corner pixel %s", hex(f0.at(kA.x, kA.y)).c_str());                                         // radius-panel: the plate's corner pixel is not border
    CHECK(f0.near(kA.x + 200, kA.y + kPlateH, bg, kExact));                             // the unit row below sits on the sheet, not the plate

    // the ten figures sit on the 34 px pitch with a dot cell after figures 1, 4, 7: arrows centre on each column
    for (int i = 0; i < 10; ++i) {
        const int fx = kA.x + digitX(i), fy = kA.y + 3;
        const Ink u = inkBox(f0, fx, fy, fx + kW, fy + kNudge, field);
        const Ink d = inkBox(f0, fx, fy + kH - kNudge, fx + kW, fy + kH, field);
        CHECKV(std::abs(u.cx() - (fx + kW / 2.0)) <= 1.5 && std::abs(d.cx() - (fx + kW / 2.0)) <= 1.5 && u.n >= 6 && d.n >= 6,
               "figure %d arrows centre %.1f / %.1f (want %.1f)", i, u.cx(), d.cx(), fx + kW / 2.0);
    }
    // the dots: Theme.knob-ring ink in the cell after figures 1, 4 and 7, centred in it, none between the figures
    for (int k = 0; k < 3; ++k) {
        const int dx = kA.x + kDotX[k];
        const Ink dot = inkBox(f0, dx, kA.y + 3, dx + kDot, kA.y + 3 + kH, field, 12);
        CHECKV(dot.n >= 2 && f0.countNear(dx, kA.y + 3, dx + kDot, kA.y + 3 + kH, knobRing, 24) >= 2 && std::abs(dot.cx() - (dx + kDot / 2.0)) <= 2.0,
               "dot %d: %d ink pixels, centre %.1f (cell %d..%d)", k, dot.n, dot.cx(), dx, dx + kDot);
        CHECK(dot.y0 > kA.y + 3 + kH / 2 - 8);                                  // a full stop sits low in the cell, not at its top
    }
    // and only there: no dot colour in any figure's numeral band. The band stops short of the nudge rows on purpose: the arrows'
    // anti-aliased edges blend the nudge grey into the field, and a few of those blends (row 0 and the last row of a column,
    // wherever the face puts its triangle's edge) land within the tolerance of the dot colour; the dots live in the numeral band.
    for (int i = 0; i < 10; ++i) {
        CHECKV(f0.countNear(kA.x + digitX(i), kA.y + 3 + kNudge, kA.x + digitX(i) + kW, kA.y + 3 + kH - kNudge, knobRing, 8) == 0,
               "figure %d: %d dot-coloured pixels in its numeral band", i,
               f0.countNear(kA.x + digitX(i), kA.y + 3 + kNudge, kA.x + digitX(i) + kW, kA.y + 3 + kH - kNudge, knobRing, 8));
    }

    // leading zero dim, next figure bright (leading-zeros 1): figure 0 brown, figure 1 amber
    CHECK(f0.countNear(kA.x + digitX(0), kA.y + 3 + kNudge, kA.x + digitX(0) + kW, kA.y + 3 + kH - kNudge, digitDim, 12) > 30);
    CHECK(f0.countNear(kA.x + digitX(0), kA.y + 3 + kNudge, kA.x + digitX(0) + kW, kA.y + 3 + kH - kNudge, digit, 12) == 0);
    CHECK(f0.countNear(kA.x + digitX(1), kA.y + 3 + kNudge, kA.x + digitX(1) + kW, kA.y + 3 + kH - kNudge, digit, 12) > 20);
    CHECK(f0.countNear(kA.x + digitX(1), kA.y + 3 + kNudge, kA.x + digitX(1) + kW, kA.y + 3 + kH - kNudge, digitDim, 12) == 0);
    for (int i = 2; i < 10; ++i) {                                              // every later figure, zeros included, is bright
        CHECKV(f0.countNear(kA.x + digitX(i), kA.y + 3 + kNudge, kA.x + digitX(i) + kW, kA.y + 3 + kH - kNudge, digit, 12) > 30 &&
               f0.countNear(kA.x + digitX(i), kA.y + 3 + kNudge, kA.x + digitX(i) + kW, kA.y + 3 + kH - kNudge, digitDim, 12) == 0, "figure %d is not bright", i);
    }
    // counter D (leading-zeros 3): three dim, the 7 bright
    CHECK(f0.countNear(kD.x + digitX(2), kD.y + 3 + kNudge, kD.x + digitX(2) + kW, kD.y + 3 + kH - kNudge, digitDim, 12) > 30);
    CHECK(f0.countNear(kD.x + digitX(3), kD.y + 3 + kNudge, kD.x + digitX(3) + kW, kD.y + 3 + kH - kNudge, digit, 12) > 30);
    CHECK(f0.countNear(kD.x + digitX(3), kD.y + 3 + kNudge, kD.x + digitX(3) + kW, kD.y + 3 + kH - kNudge, digitDim, 12) == 0);

    // the unit row: faint mono ink centred under each group, none in the gaps between
    const int uy0 = kA.y + kUnitY, uy1 = kA.y + kUnitY + kUnitH;
    for (int k = 0; k < 4; ++k) {
        const int lx = kA.x + static_cast<int>(kUnitCx[k]);
        const Ink u = inkBox(f0, lx - 20, uy0, lx + 20, uy1, bg, 12);
        CHECKV(u.n >= 8 && std::abs(u.cx() - (kA.x + kUnitCx[k])) <= 3.0 && f0.countNear(lx - 20, uy0, lx + 20, uy1, faint, 40) >= 3,
               "unit %d: %d ink pixels centre %.1f (want %.1f)", k, u.n, u.cx(), kA.x + kUnitCx[k]);
        CHECK(u.y0 >= uy0 && u.y1 < uy1);
        const Rgb ink = brightest(f0, lx - 20, uy0, lx + 20, uy1);               // the glyphs' own colour: Theme.faint (the nudge grey is 17 off)
        CHECKV(nearColour(ink, faint, 12), "unit %d text colour %s (want %s)", k, hex(ink).c_str(), hex(faint).c_str());
    }
    CHECK(f0.countInk(kA.x + 66, uy0, kA.x + 96, uy1, bg, 12) == 0);            // between GHz and MHz
    CHECK(f0.countInk(kA.x + 125, uy0, kA.x + 200, uy1, bg, 12) == 0);          // between MHz and kHz
    CHECK(f0.countInk(kA.x + 250, uy0, kA.x + 315, uy1, bg, 12) == 0);          // between kHz and Hz
    CHECK(f0.countInk(kA.x, uy0, kA.x + 10, uy1, bg, 12) == 0 && f0.countInk(kA.x + 385, uy0, kA.x + kPlateW, uy1, bg, 12) == 0);

    // the converter slot: the static engraving in `faint` where nothing is set, the converter line in warning colour
    const int cy0 = kCaptionY, cy1 = kCaptionY + kCaptionH;
    {
        const Ink eng = inkBox(f0, kA.x, kA.y + cy0, kA.x + kPlateW, kA.y + cy1, bg, 12);
        CHECKV(eng.n >= 40 && f0.countNear(kA.x, kA.y + cy0, kA.x + kPlateW, kA.y + cy1, faint, 40) >= 8 &&
               f0.countNear(kA.x, kA.y + cy0, kA.x + kPlateW, kA.y + cy1, warn, 60) == 0,
               "engraving: %d ink, %d faint, %d warn", eng.n, f0.countNear(kA.x, kA.y + cy0, kA.x + kPlateW, kA.y + cy1, faint, 40),
               f0.countNear(kA.x, kA.y + cy0, kA.x + kPlateW, kA.y + cy1, warn, 60));
        CHECK(std::abs(eng.cx() - (kA.x + kPlateW / 2.0)) <= 4.0);              // centred under the plate
        const Rgb engInk = brightest(f0, kA.x, kA.y + cy0, kA.x + kPlateW, kA.y + cy1);
        CHECKV(nearColour(engInk, faint, 12), "engraving colour %s (want %s)", hex(engInk).c_str(), hex(faint).c_str());
        const Rgb cvtInk = brightest(f0, kC.x, kC.y + cy0, kC.x + kPlateW, kC.y + cy1);
        CHECKV(nearColour(cvtInk, warn, 12), "converter line colour %s (want %s)", hex(cvtInk).c_str(), hex(warn).c_str());
        const Ink cvt = inkBox(f0, kC.x, kC.y + cy0, kC.x + kPlateW, kC.y + cy1, bg, 12);
        CHECKV(cvt.n >= 60 && f0.countNear(kC.x, kC.y + cy0, kC.x + kPlateW, kC.y + cy1, warn, 60) >= 12 &&
               f0.countNear(kC.x, kC.y + cy0, kC.x + kPlateW, kC.y + cy1, faint, 20) == 0,
               "converter line: %d ink, %d warn, %d faint", cvt.n, f0.countNear(kC.x, kC.y + cy0, kC.x + kPlateW, kC.y + cy1, warn, 60),
               f0.countNear(kC.x, kC.y + cy0, kC.x + kPlateW, kC.y + cy1, faint, 20));
        CHECK(cvt.n > eng.n);                                                   // "via Ham-It-Up 125.000 MHz LO" is longer than the engraving
        CHECK(std::abs(cvt.cx() - (kC.x + kPlateW / 2.0)) <= 4.0);
    }
    // A and C differ only in that line: the plates and unit rows are pixel-identical
    {
        int diff = 0;
        for (int y = 0; y < kCaptionY; ++y) {
            for (int x = 0; x < kPlateW; ++x) {
                const Rgb a = f0.at(kA.x + x, kA.y + y), c = f0.at(kC.x + x, kC.y + y);
                if (a.r != c.r || a.g != c.g || a.b != c.b) { ++diff; }
            }
        }
        CHECKV(diff == 0, "plate + unit row of A vs C differ in %d pixels", diff);
    }

    // =========================================================================================================
    // 5. Counter B (editing): the editor lies over the figures; D (switches off) and E (disabled) look
    // =========================================================================================================
    {
        const int ex = kB.x + digitX(0), ey = kB.y + 3, ew = digitX(9) + kW - digitX(0), eh = kH;
        CHECK(ew == 379 && eh == 52);
        // the editor's ground and its rim (Theme.line-strong while it has no keyboard focus), radius 6
        CHECKV(f0.colNear(ex, ey + 8, ey + eh - 7, lineStrong) && f0.colNear(ex + ew - 1, ey + 8, ey + eh - 7, lineStrong) &&
               f0.rowNear(ey, ex + 8, ex + ew - 7, lineStrong) && f0.rowNear(ey + eh - 1, ex + 8, ex + ew - 7, lineStrong),
               "editor rim: %s %s", hex(f0.at(ex, ey + 20)).c_str(), hex(f0.at(ex + 20, ey)).c_str());
        CHECK(f0.near(ex + 200, ey + 2, field, kExact) && f0.near(ex + ew - 4, ey + eh / 2, field, kExact));
        // nothing of the figures shows through: no arrows, no dots, no dim zero
        CHECK(f0.countNear(ex + 1, ey + 1, ex + ew - 1, ey + eh - 1, nudge, 8) == 0);
        CHECK(f0.countNear(ex + 1, ey + 1, ex + ew - 1, ey + eh - 1, knobRing, 8) == 0);
        CHECK(f0.countNear(ex + 1, ey + 1, ex + ew - 1, ey + eh - 1, digitDim, 8) == 0);
        // the typed text: Theme.digit ink, mono 22 px (a ten-character frequency is wider than 100 px, starts at the left)
        const Ink txt = inkBox(f0, ex + 2, ey + 2, ex + ew - 2, ey + eh - 2, field, 30);
        CHECKV(f0.countNear(ex + 2, ey + 2, ex + ew - 2, ey + eh - 2, digit, 12) > 60 && txt.x1 - txt.x0 > 60 && txt.x0 < ex + 24 &&
               std::abs(txt.cy() - (ey + eh / 2.0)) <= 4.0, "editor text: %d near digit, x %d..%d, cy %.1f", f0.countNear(ex + 2, ey + 2, ex + ew - 2, ey + eh - 2, digit, 12), txt.x0, txt.x1, txt.cy());
        // the unit row and the engraving are still there under it
        CHECK(f0.countInk(kB.x, kB.y + kUnitY, kB.x + kPlateW, kB.y + kUnitY + kUnitH, bg, 12) > 20);
        // counter A at rest has no editor: the figures are there
        CHECK(f0.countNear(kA.x + digitX(0), kA.y + 3, kA.x + digitX(9) + kW, kA.y + 3 + kH, nudge, 20) > 60);
    }
    // every counter's plate is the same 397 x 58 box: Theme.line on all four edges, the sheet's ground just outside
    for (const At& c : {kA, kB, kC, kD, kE}) {
        const int mid = c.y + kPlateH / 2;
        CHECKV(f0.near(c.x, mid, line) && f0.near(c.x + kPlateW - 1, mid, line) && f0.near(c.x - 1, mid, bg, kExact) && f0.near(c.x + kPlateW, mid, bg, kExact) &&
               f0.near(c.x + 200, c.y, line) && f0.near(c.x + 200, c.y + kPlateH - 1, line) && f0.near(c.x + 200, c.y - 1, bg, kExact) && f0.near(c.x + 200, c.y + kPlateH, bg, kExact),
               "plate edges of the counter at %d,%d", c.x, c.y);
    }
    // D: no switches (both rows empty), figures where they were; E: disabled (grey figures, leading zeros still dimmer)
    CHECK(f0.countInk(kD.x + digitX(0), kD.y + 3, kD.x + digitX(9) + kW, kD.y + 3 + kNudge, field, 12) == 0);
    CHECK(f0.countInk(kD.x + digitX(0), kD.y + 3 + kH - kNudge, kD.x + digitX(9) + kW, kD.y + 3 + kH, field, 12) == 0);
    CHECK(f0.countNear(kD.x + digitX(3), kD.y + 3 + kNudge, kD.x + digitX(3) + kW, kD.y + 3 + kH - kNudge, digit, 12) > 30);
    CHECK(f0.countNear(kE.x + digitX(3), kE.y + 3 + kNudge, kE.x + digitX(3) + kW, kE.y + 3 + kH - kNudge, disabledFg, 12) > 30);
    CHECK(f0.countNear(kE.x + digitX(0), kE.y + 3 + kNudge, kE.x + digitX(0) + kW, kE.y + 3 + kH - kNudge, digitDim, 12) > 30);
    CHECK(f0.countNear(kE.x, kE.y, kE.x + kPlateW, kE.y + kPlateH, digit, 12) == 0);
    CHECK(f0.near(kE.x, kE.y + kPlateH / 2, line));                             // the plate itself is unchanged

    f0.savePng(shotDir() + "digits-specimen.png");

    // =========================================================================================================
    // 6. FrequencyCounter input
    // =========================================================================================================
    const auto figX = [](const At& c, int i) { return c.x + digitX(i) + kW / 2.0f; };       // centre line of figure i
    const auto rowY = [](const At& c, int part) { return c.y + 3 + part + 0.0f; };          // part: y inside the figure column
    // the switches report their figure and direction
    reset();
    click(win, figX(kA, 4), rowY(kA, kNudge / 2.0f));
    CHECKV(ui->get_nudges() == 1 && ui->get_last_digit() == 4 && ui->get_last_dir() == 1, "A up 4: %d nudges, digit %d, dir %d", ui->get_nudges(), ui->get_last_digit(), ui->get_last_dir());
    click(win, figX(kA, 9), rowY(kA, kH - kNudge / 2.0f));
    CHECKV(ui->get_nudges() == 2 && ui->get_last_digit() == 9 && ui->get_last_dir() == -1, "A down 9: %d nudges, digit %d, dir %d", ui->get_nudges(), ui->get_last_digit(), ui->get_last_dir());
    click(win, figX(kA, 0), rowY(kA, kNudge / 2.0f));
    CHECK(ui->get_nudges() == 3 && ui->get_last_digit() == 0 && ui->get_last_dir() == 1);
    CHECK(ui->get_edit_requests() == 0 && ui->get_menu_requests() == 0);
    park();
    // held: repeats with the same figure
    reset();
    moveTo(win, figX(kA, 2), rowY(kA, kNudge / 2.0f));
    press(win, figX(kA, 2), rowY(kA, kNudge / 2.0f));
    settle(milliseconds(600));
    release(win, figX(kA, 2), rowY(kA, kNudge / 2.0f));
    CHECKV(ui->get_nudges() >= 3 && ui->get_last_digit() == 2 && ui->get_last_dir() == 1, "A held on 2: %d nudges, digit %d", ui->get_nudges(), ui->get_last_digit());
    park();
    // the wheel steps the figure under the pointer, one notch each
    reset();
    scroll(win, figX(kA, 5), rowY(kA, kH / 2.0f), 0, 60);
    CHECK(ui->get_wheels() == 1 && ui->get_last_digit() == 5 && ui->get_last_dir() == 1);
    scroll(win, figX(kA, 7), rowY(kA, kH / 2.0f), 0, -60);
    CHECK(ui->get_wheels() == 2 && ui->get_last_digit() == 7 && ui->get_last_dir() == -1);
    scroll(win, figX(kA, 3), rowY(kA, kNudge / 2.0f), 0, 60);                  // over a switch: still that figure
    CHECK(ui->get_wheels() == 3 && ui->get_last_digit() == 3);
    scroll(win, kA.x + kDotX[1] + kDot / 2.0f, rowY(kA, kH / 2.0f), 0, 60);   // over a dot cell: no figure there
    scroll(win, kA.x + 3.0f, rowY(kA, kH / 2.0f), 0, 60);                       // over the plate's padding
    CHECK(ui->get_wheels() == 3);
    CHECK(ui->get_nudges() == 0 && ui->get_edit_requests() == 0);
    park();
    // the tooltip is the counter's: it opens under a figure
    {
        moveTo(win, figX(kA, 6), rowY(kA, kH / 2.0f));
        settle(milliseconds(750));
        Frame tip = render(ui, 440, 600);
        const int tx = kA.x + digitX(6) + 10, ty = kA.y + 3 + kH + 9;                                // under the figure it was shown for
        CHECKV(tip.near(tx, ty, menu), "counter tooltip ground at (%d,%d): %s", tx, ty, hex(tip.at(tx, ty)).c_str());
        CHECK(tip.near(kA.x + digitX(1) + 10, ty, bg, kExact));                                              // one tip only, under that figure
        CHECK(tip.countNear(kA.x + digitX(6), ty - 3, kA.x + digitX(6) + 170, ty + 27, fgSoft, 40) > 30);     // and it carries the counter's sentence
        park();
    }
    // right-click anywhere on the plate and the engraved rows: the counter's menu, position in the counter
    reset();
    const struct { float x, y; const char* where; } spots[] = {
        {4.0f, kPlateH / 2.0f, "plate padding"}, {digitX(3) + 17.5f, 3 + 26.5f, "a numeral"}, {digitX(3) + 17.5f, 3 + 4.5f, "an up switch"},
        {digitX(6) + 17.5f, 3 + kH - 4.5f, "a down switch"}, {kDotX[2] + 6.5f, 3 + 30.5f, "a dot cell"}, {kUnitCx[1], kUnitY + 6.0f, "the unit row"},
        {kPlateW / 2.0f, kCaptionY + 6.0f, "the caption line"}, {kPlateW - 4.0f, 2.5f, "the top edge"},
    };
    int n = 0;
    for (const auto& s : spots) {
        click(win, kA.x + s.x, kA.y + s.y, slint::PointerEventButton::Right);
        ++n;
        CHECKV(ui->get_menu_requests() == n && std::abs(ui->get_last_x() - s.x) <= 1.0f && std::abs(ui->get_last_y() - s.y) <= 1.0f,
               "right-click on %s: %d requests (want %d), at %.1f,%.1f (want %.1f,%.1f)", s.where, ui->get_menu_requests(), n, ui->get_last_x(), ui->get_last_y(), s.x, s.y);
    }
    CHECK(ui->get_nudges() == 0 && ui->get_edit_requests() == 0 && ui->get_right_clicks() == 0);
    click(win, kA.x + kPlateW + 3.0f, kA.y + 20.0f, slint::PointerEventButton::Right);   // beside the plate: not the counter
    CHECK(ui->get_menu_requests() == n);
    park();
    // switches off (D): the wheel works over the blank rows, a click types, no nudges
    reset();
    click(win, figX(kD, 4), rowY(kD, kNudge / 2.0f));
    CHECK(ui->get_nudges() == 0 && ui->get_edit_requests() == 1);
    scroll(win, figX(kD, 8), rowY(kD, 3.0f), 0, -60);
    CHECK(ui->get_wheels() == 1 && ui->get_last_digit() == 8 && ui->get_last_dir() == -1);
    click(win, kD.x + 40.0f, kD.y + 20.0f, slint::PointerEventButton::Right);
    CHECK(ui->get_menu_requests() == 1);
    park();
    // disabled (E): nothing at all
    reset();
    click(win, figX(kE, 4), rowY(kE, kNudge / 2.0f));
    click(win, figX(kE, 4), rowY(kE, kH / 2.0f));
    scroll(win, figX(kE, 5), rowY(kE, kH / 2.0f), 0, 60);
    click(win, figX(kE, 6), rowY(kE, kH / 2.0f), slint::PointerEventButton::Right);
    click(win, kE.x + 4.0f, kE.y + 29.0f, slint::PointerEventButton::Right);
    moveTo(win, figX(kE, 2), rowY(kE, kNudge / 2.0f));
    press(win, figX(kE, 2), rowY(kE, kNudge / 2.0f));
    settle(milliseconds(500));
    release(win, figX(kE, 2), rowY(kE, kNudge / 2.0f));
    CHECKV(ui->get_nudges() == 0 && ui->get_wheels() == 0 && ui->get_edit_requests() == 0 && ui->get_menu_requests() == 0,
           "disabled counter fired: %d nudges, %d wheels, %d edit requests, %d menu requests", ui->get_nudges(), ui->get_wheels(), ui->get_edit_requests(), ui->get_menu_requests());
    park();

    // =========================================================================================================
    // 6b. The host drives counter A at run time, as main.slint will: a new model of digits, an edited row, the
    //     leading-zero count, the tuner-switches setting and `enabled` all reach the pixels
    // =========================================================================================================
    {
        const auto digitsOf = [](std::initializer_list<int> v) { return std::make_shared<slint::VectorModel<int>>(std::vector<int>(v)); };
        const auto settleFrame = [&] { settle(milliseconds(30)); return render(ui, 440, 600); };

        // a whole new model: 7.074 MHz is counter D's frequency, so A's figure 3 is now D's bright 7, and not the 5 it was
        auto fresh = digitsOf({0, 0, 0, 7, 0, 7, 4, 0, 0, 0});
        ui->set_digits_a(fresh);
        const Frame m1 = settleFrame();
        CHECK(bandDiff(m1, kA, 3, f0, kD, 3) == 0);
        CHECKV(bandDiff(m1, kA, 3, f0, kA, 3) > 100, "figure 3 still draws the old digit (%d pixels differ)", bandDiff(m1, kA, 3, f0, kA, 3));
        CHECK(bandDiff(m1, kA, 6, f0, kD, 6) == 0);                              // the 4 of 7.074, bright in both
        // one row edited in place: figure 3 becomes a 5 again, pixel for pixel the first picture's
        fresh->set_row_data(3, 5);
        const Frame m2 = settleFrame();
        CHECK(bandDiff(m2, kA, 3, f0, kA, 3) == 0);
        // three leading zeros: figures 1 and 2 turn dim, exactly as counter D draws them
        ui->set_leading_a(3);
        const Frame m3 = settleFrame();
        CHECKV(bandDiff(m3, kA, 1, f0, kD, 1) == 0 && bandDiff(m3, kA, 2, f0, kD, 2) == 0, "dim zeros differ from D's: %d, %d", bandDiff(m3, kA, 1, f0, kD, 1), bandDiff(m3, kA, 2, f0, kD, 2));
        CHECK(bandDiff(m3, kA, 3, f0, kA, 3) == 0);                              // the 5 stays bright
        // back to the first picture's model and leading zero: the plate is exactly as it was
        ui->set_digits_a(digitsOf({0, 1, 4, 5, 5, 0, 0, 0, 0, 0}));
        ui->set_leading_a(1);
        const Frame m4 = settleFrame();
        CHECKV(differing(f0, m4, kA.x, kA.y, kA.x + kPlateW, kA.y + kCaptionY) == 0, "restored plate differs in %d pixels", differing(f0, m4, kA.x, kA.y, kA.x + kPlateW, kA.y + kCaptionY));

        // "Tuner switches" off at run time: both rows empty out, the switches stop nudging, the wheel still steps
        ui->set_switches_a(false);
        const Frame s1 = settleFrame();
        CHECK(s1.countInk(kA.x + digitX(0), kA.y + 3, kA.x + digitX(9) + kW, kA.y + 3 + kNudge, field, 12) == 0);
        CHECK(s1.countInk(kA.x + digitX(0), kA.y + 3 + kH - kNudge, kA.x + digitX(9) + kW, kA.y + 3 + kH, field, 12) == 0);
        reset();
        moveTo(win, figX(kA, 4), rowY(kA, kNudge / 2.0f));
        press(win, figX(kA, 4), rowY(kA, kNudge / 2.0f));
        settle(milliseconds(500));
        moveTo(win, 437, 597);                                                    // let go away from the figure: no click, no editor
        release(win, 437, 597);
        scroll(win, figX(kA, 4), rowY(kA, 3.0f), 0, 60);
        CHECKV(ui->get_nudges() == 0 && ui->get_edit_requests() == 0 && ui->get_wheels() == 1 && ui->get_last_digit() == 4,
               "switches off: %d nudges, %d edit requests, %d wheels", ui->get_nudges(), ui->get_edit_requests(), ui->get_wheels());
        ui->set_switches_a(true);
        const Frame s2 = settleFrame();
        CHECK(differing(f0, s2, kA.x, kA.y, kA.x + kPlateW, kA.y + kCaptionY) == 0);   // the switches are back, as drawn before
        click(win, figX(kA, 4), rowY(kA, kNudge / 2.0f));
        CHECK(ui->get_nudges() == 1 && ui->get_last_dir() == 1);
        park();

        // `enabled` turned off in mid-hold: the repeat stops at once and the figures grey; turned on, the plate is as before
        reset();
        moveTo(win, figX(kA, 2), rowY(kA, kNudge / 2.0f));
        press(win, figX(kA, 2), rowY(kA, kNudge / 2.0f));
        settle(milliseconds(450));
        const int beforeOff = ui->get_nudges();
        CHECKV(beforeOff >= 2, "the hold had begun to repeat: %d nudges", beforeOff);
        ui->set_enabled_a(false);
        settle(milliseconds(30));
        const int atOff = ui->get_nudges();
        settle(milliseconds(500));
        release(win, figX(kA, 2), rowY(kA, kNudge / 2.0f));
        CHECKV(ui->get_nudges() == atOff, "repeats after the counter was disabled: %d -> %d nudges", atOff, ui->get_nudges());
        park();
        const Frame d1 = settleFrame();
        CHECK(d1.countNear(kA.x + digitX(2), kA.y + 3 + kNudge, kA.x + digitX(2) + kW, kA.y + 3 + kH - kNudge, disabledFg, 12) > 30);
        CHECK(d1.countNear(kA.x, kA.y, kA.x + kPlateW, kA.y + kPlateH, digit, 12) == 0);
        ui->set_enabled_a(true);
        const Frame d2 = settleFrame();
        CHECK(differing(f0, d2, kA.x, kA.y, kA.x + kPlateW, kA.y + kCaptionY) == 0);
        reset();
    }

    // =========================================================================================================
    // 7. The typed editor, end to end on counter A: click -> editor over the figures -> type -> Enter / Esc / focus lost
    // =========================================================================================================
    reset();
    CHECK(!ui->get_editing_a() && !ui->get_focused_a() && !ui->get_focused_b());
    click(win, figX(kA, 3), rowY(kA, kH / 2.0f));
    CHECKV(ui->get_edit_requests() == 1 && ui->get_editing_a(), "click on a figure: %d edit requests, editing %d", ui->get_edit_requests(), ui->get_editing_a());
    settle(milliseconds(30));
    Frame ed = render(ui, 440, 600);
    CHECKV(ui->get_focused_a(), "the editor takes the keyboard focus when editing turns true");
    {
        const int ex = kA.x + digitX(0), ey = kA.y + 3, ew = digitX(9) + kW - digitX(0);
        CHECK(ed.countNear(ex + 1, ey + 1, ex + ew - 1, ey + kH - 1, nudge, 8) == 0);                 // the figures are covered
        CHECK(ed.colNear(ex, ey + 8, ey + kH - 7, focus) && ed.rowNear(ey, ex + 8, ex + ew - 7, focus)); // the rim shows the focus ring
        CHECK(ed.countNear(ex + 2, ey + 2, ex + ew - 2, ey + kH - 2, accent, 12) > 100);               // AutoSelectAll: the seed is selected
    }
    ed.savePng(shotDir() + "digits-specimen-editing.png");
    {   // the figures under the editor's 8 px margin take no input while it is open: no wheel, no switch, no second request
        reset();
        const float mx = kA.x + digitX(0) + 3.0f;
        scroll(win, mx, rowY(kA, kH / 2.0f), 0, 60);
        click(win, mx, rowY(kA, kNudge / 2.0f));
        click(win, mx, rowY(kA, kH / 2.0f));
        click(win, mx, rowY(kA, kH - kNudge / 2.0f));
        click(win, mx, rowY(kA, kH / 2.0f), slint::PointerEventButton::Right);
        CHECKV(ui->get_wheels() == 0 && ui->get_nudges() == 0 && ui->get_edit_requests() == 0 && ui->get_menu_requests() == 0,
               "input leaked through the editor: %d wheels, %d nudges, %d edit requests, %d menu requests", ui->get_wheels(), ui->get_nudges(), ui->get_edit_requests(), ui->get_menu_requests());
        CHECK(ui->get_editing_a() && ui->get_focused_a() && ui->get_cancels() == 0);                   // and the editor is still open and focused
    }
    typed(u8"7"); typed(u8"."); typed(u8"0"); typed(u8"7"); typed(u8"4");
    CHECKV(std::string_view(ui->get_editor_a().data()) == "7.074", "typing replaces the selected seed: '%s'", ui->get_editor_a().data());
    {
        Frame t = render(ui, 440, 600);
        const int ex = kA.x + digitX(0), ey = kA.y + 3, ew = digitX(9) + kW - digitX(0);
        CHECK(t.countNear(ex + 2, ey + 2, ex + ew - 2, ey + kH - 2, digit, 12) > 40);                  // the typed text is Theme.digit
        CHECK(t.countNear(ex + 2, ey + 2, ex + ew - 2, ey + kH - 2, accent, 12) < 20);                 // and no longer selected
    }
    typed(u8"5");                                                                                       // further typing appends
    CHECK(std::string_view(ui->get_editor_a().data()) == "7.0745");
    keyTap(win, slint::platform::key_codes::Return);
    CHECKV(std::string_view(ui->get_typed_text().data()) == "7.0745" && ui->get_cancels() == 0, "Enter: typed '%s', %d cancels", ui->get_typed_text().data(), ui->get_cancels());
    CHECK(!ui->get_editing_a());                                                                       // the host closed it
    settle(milliseconds(30));
    Frame closed = render(ui, 440, 600);
    CHECKV(!ui->get_focused_a(), "the editor gives the keyboard focus back when editing ends");
    CHECK(closed.countNear(kA.x + digitX(0), kA.y + 3, kA.x + digitX(9) + kW, kA.y + 3 + kH, nudge, 20) > 60);   // the figures are back
    CHECK(differing(f0, closed, kA.x, kA.y, kA.x + kPlateW, kA.y + kCaptionY) == 0);                              // exactly as before
    CHECK(ui->get_cancels() == 0);                                                                     // closing after Enter is not a cancel
    typed(u8"9");                                                                                       // no editor listens any more
    CHECK(std::string_view(ui->get_editor_a().data()) == "7.0745");

    // Esc cancels (and only cancels)
    reset();
    click(win, figX(kA, 8), rowY(kA, kH / 2.0f));
    settle(milliseconds(30)); render(ui, 440, 600);
    CHECK(ui->get_edit_requests() == 1 && ui->get_editing_a() && ui->get_focused_a());
    typed(u8"1"); typed(u8"2");
    CHECK(std::string_view(ui->get_editor_a().data()) == "12");                                        // the reopened seed "7.0745" was selected
    keyTap(win, slint::platform::key_codes::Escape);
    CHECKV(ui->get_cancels() == 1 && ui->get_typed_text().size() == 0, "Esc: %d cancels, typed '%s'", ui->get_cancels(), ui->get_typed_text().data());
    CHECK(!ui->get_editing_a());
    settle(milliseconds(30)); render(ui, 440, 600);
    CHECK(!ui->get_focused_a() && ui->get_cancels() == 1);                                             // the focus release is not a second cancel
    park();

    // counter B shows the editing state from the start; flipping `editing` hands it the focus, then keys type into it
    reset();
    CHECK(ui->get_editing_b() && !ui->get_focused_b());
    ui->set_editing_b(false);
    settle(milliseconds(30)); render(ui, 440, 600);
    CHECK(!ui->get_focused_b() && ui->get_cancels() == 0);
    ui->set_editing_b(true);
    settle(milliseconds(30));
    Frame bf = render(ui, 440, 600);
    CHECK(ui->get_focused_b());
    CHECK(bf.colNear(kB.x + digitX(0), kB.y + 3 + 8, kB.y + 3 + kH - 7, focus));
    typed(u8"1"); typed(u8"4"); typed(u8"6");
    CHECKV(std::string_view(ui->get_editor_b().data()) == "146", "editor B text '%s'", ui->get_editor_b().data());
    typed(u8"."); typed(u8"5");
    CHECK(std::string_view(ui->get_editor_b().data()) == "146.5");
    keyTap(win, slint::platform::key_codes::Return);
    CHECKV(std::string_view(ui->get_typed_text().data()) == "146.5", "Enter in B: '%s'", ui->get_typed_text().data());
    CHECK(ui->get_editing_b() && ui->get_cancels() == 0);                                              // B's host leaves it open
    keyTap(win, slint::platform::key_codes::Escape);
    CHECK(ui->get_cancels() == 1);
    // Backspace and the arrow keys are the TextInput's own
    keyTap(win, slint::platform::key_codes::Backspace);
    CHECK(std::string_view(ui->get_editor_b().data()) == "146.");
    ui->set_editor_b("145.500000");                                                                    // the host re-seeds
    CHECK(std::string_view(ui->get_editor_b().data()) == "145.500000");
    reset();

    // focus lost cancels: A's editor opening takes the focus from B, which was left open
    click(win, figX(kA, 1), rowY(kA, kH / 2.0f));
    settle(milliseconds(30)); render(ui, 440, 600);
    CHECKV(ui->get_focused_a() && !ui->get_focused_b() && ui->get_cancels() == 1, "focus moved to A: a %d, b %d, %d cancels", ui->get_focused_a(), ui->get_focused_b(), ui->get_cancels());
    keyTap(win, slint::platform::key_codes::Escape);
    CHECK(ui->get_cancels() == 2 && !ui->get_editing_a());
    ui->set_editing_b(false);                                                                          // close B for the final picture
    settle(milliseconds(30));
    park();

    // after all of that the sheet is back at rest: every part but counter B (now closed) is the first picture's, pixel for pixel
    const Frame last = render(ui, 440, 600);
    CHECKV(differing(f0, last, 0, 0, 440, kB.y - 12) == 0 && differing(f0, last, 0, kC.y - 12, 440, 600) == 0,
           "the sheet did not return to rest: %d + %d pixels differ", differing(f0, last, 0, 0, 440, kB.y - 12), differing(f0, last, 0, kC.y - 12, 440, 600));
    CHECK(last.countNear(kB.x + digitX(0), kB.y + 3, kB.x + digitX(9) + kW, kB.y + 3 + kH, nudge, 20) > 60);   // B shows its figures now

    return testSummary("test_slint_digits");
}
