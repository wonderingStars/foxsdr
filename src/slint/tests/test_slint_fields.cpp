// The FIELDS family (docs 6.35): Field (text, number, bool, path, password; the error and disabled looks) and Checkbox,
// rendered headlessly from ui/specimens/fields_specimen.slint and probed against the PluginKit sheet's "Plugin settings"
// form (.rw, .lb, .in, .in.err, .in.num, .ck, .n.rd and the PASSWORD block) and the contract in COMPONENTS.md.
//
// Text is in a fallback typeface on this machine, so text checks assert INK in a region and its COLOUR, never a shape or a
// width; the boxes, borders, keys and ticks are drawn geometry and are asserted to the pixel. Typing goes through the
// harness's key events, so edited / committed are the real TextInput callbacks.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "fields_specimen.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

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
    double cy() const { return (y0 + y1 + 1) / 2.0; }   // centre of the box, in pixel-edge coordinates
    int w() const { return x1 - x0 + 1; }
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

// The rectangle of one frame compared with the same rectangle `dy` rows lower in another: how many pixels differ.
int differingShifted(const Frame& a, const Frame& b, int x0, int y0, int x1, int y1, int dy) {
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Rgb p = a.at(x, y), q = b.at(x, y + dy);
            if (p.r != q.r || p.g != q.g || p.b != q.b) { ++n; }
        }
    }
    return n;
}

// The left edge of a key that ends at the right of its row: scan the row leftwards from `right` while the pixel is the key
// ground. A key whose border is the ground colour (the lit one, accent on accent) is one pixel further left.
int keyLeft(const Frame& f, int y, int right, Rgb ground, bool borderIsGround) {
    int x = right;
    while (x > 0 && f.near(x, y, ground, 3)) { --x; }
    return borderIsGround ? x + 1 : x;
}

// ---- the specimen's table (fields_specimen.slint) ----
constexpr int kW = 500, kH = 830;
constexpr int kLeftX = 16, kRightX = 258, kColW = 226, kWideW = 468;                  // columns
constexpr int kLabelH = 24, kGap = 4, kIn = 24;                                         // label row, gap, input (Theme.field-h)
constexpr int kInputDy = kLabelH + kGap;                                                // the input starts 28 under the field's y
constexpr int kTextY = 24, kErrY = 104, kBoolY = 104, kPathY = 200, kPassY = 276, kPass2Y = 376;
constexpr int kDisY = 452, kDisPathY = 512, kFrPathY = 512, kFrBoolY = 572, kFrPassY = 572, kMaxY = 652, kTipY = 652;
constexpr int kCbOnY = 740, kCbDisY = 768, kCbWrapY = 768, kCbKeyY = 796;
constexpr int kBox = 14;                                                                // the checkbox box

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = FieldsSpecimen::create();
    slint::Window& win = ui->window();

    // The tokens, read from the Workbench skin by name (the theme test pins their values).
    const Skin wb = ui->global<Workbench>().get_value();
    const Rgb bg = tok(wb.bg), field = tok(wb.field), lineStrong = tok(wb.line_strong), fg = tok(wb.fg), fgSoft = tok(wb.fg_soft);
    const Rgb muted = tok(wb.muted), dim = tok(wb.dim), disabled = tok(wb.disabled), accent = tok(wb.accent), accentInk = tok(wb.accent_ink);
    const Rgb lampRed = tok(wb.lamp_red), errFg = tok(wb.chip_err_fg), keyBg = tok(wb.key_bg), keyLine = tok(wb.key_line);
    const Rgb accentHover = tok(wb.accent_hover), keyDisBg = tok(wb.key_disabled_bg), keyDisFg = tok(wb.key_disabled_fg), focus = tok(wb.focus), menu = tok(wb.menu);

    const auto reset = [&] {
        ui->set_edits(0); ui->set_commits(0); ui->set_toggles(0); ui->set_browses(0);
        ui->set_last_value(""); ui->set_last_checked(false);
    };
    // Parks the pointer on the sheet's empty corner: closes any tooltip and ends any hover.
    const auto park = [&] { moveTo(win, 495, 825); settle(milliseconds(30)); };
    const auto sval = [&](const slint::SharedString& s) { return std::string(s); };
    const auto tap = [&](std::u8string_view k) { keyTap(win, k); };

    // =========================================================================================================
    // 1. The sheet, before anything is touched
    // =========================================================================================================
    Frame f0 = render(ui, kW, kH);
    CHECK(f0.width == kW && f0.height == kH);
    CHECK(f0.near(498, 828, bg));                                                   // the sheet's ground
    f0.savePng(shotDir() + "fields-specimen.png");

    // Fields size themselves: label row 24 + gap 4 + control 24, and 4 + the line under it when there is one
    CHECK_NEAR(ui->get_text_h(), 52.0, 0.01);
    CHECK_NEAR(ui->get_path_h(), 52.0, 0.01);
    CHECKV(ui->get_err_h() >= 52 + kGap + 10 && ui->get_err_h() <= 52 + kGap + 20, "error field height %.1f", ui->get_err_h());
    CHECKV(ui->get_pass_h() >= 52 + kGap + 10 && ui->get_pass_h() <= 52 + kGap + 20, "password field height %.1f (one line of note)", ui->get_pass_h());
    CHECK_NEAR(ui->get_cb_on_h(), 14.0, 0.01);
    CHECKV(ui->get_cb_wrap_h() >= 3 * 11 && ui->get_cb_wrap_h() <= 3 * 17, "wrapping checkbox height %.1f", ui->get_cb_wrap_h());

    // the kind is a word in the cap at the right of the label row
    CHECK(sval(ui->get_text_kind_text()) == "TEXT");
    CHECK(sval(ui->get_number_kind_text()) == "NUMBER");
    CHECK(sval(ui->get_bool_kind_text()) == "BOOL");
    CHECK(sval(ui->get_path_kind_text()) == "PATH");
    CHECK(sval(ui->get_pass_kind_text()) == "PASSWORD");

    // =========================================================================================================
    // 2. TEXT: the label row, the 24 px input, the placeholder
    // =========================================================================================================
    {
        const int x = kLeftX, y = kTextY, r = kLeftX + kColW;                         // the field's box
        const int iy = y + kInputDy;                                                   // the input's top row
        // label row (.rw, 24 px): the .lb label at the left in fg-soft, the kind cap at the right in muted
        const Ink label = inkBox(f0, x, y, x + 140, y + kLabelH, bg);
        CHECKV(label.n > 30 && label.x0 <= x + 2, "label ink n=%d x0=%d", label.n, label.x0);
        CHECKV(f0.countNear(x, y, x + 140, y + kLabelH, fgSoft, 40) >= 10, "label colour fg-soft: %d", f0.countNear(x, y, x + 140, y + kLabelH, fgSoft, 40));
        CHECKV(std::abs(label.cy() - (y + kLabelH / 2.0)) <= 3.0, "label centre %.1f vs %.1f", label.cy(), y + kLabelH / 2.0);
        const Ink cap = inkBox(f0, r - 80, y, r, y + kLabelH, bg);
        CHECKV(cap.n > 20 && cap.x1 >= r - 4 && cap.x1 <= r - 1, "kind cap ink n=%d right edge %d (field right %d)", cap.n, cap.x1, r);
        CHECKV(f0.countNear(r - 80, y, r, y + kLabelH, muted, 40) >= 8, "kind cap colour muted: %d", f0.countNear(r - 80, y, r, y + kLabelH, muted, 40));
        CHECK(f0.countInk(x + 150, y, r - 70, y + kLabelH, bg) == 0);                  // nothing between the label and the cap
        // the input: 24 px, field ground, a 1 px line-strong border inside the 24, 6 px corners
        CHECK(f0.rowNear(iy, x + 8, r - 8, lineStrong));                              // top border
        CHECK(f0.rowNear(iy + kIn - 1, x + 8, r - 8, lineStrong));                    // bottom border
        CHECK(f0.rowNear(iy - 1, x + 8, r - 8, bg));                                  // the 4 px gap above ...
        CHECK(f0.rowNear(iy + kIn, x + 8, r - 8, bg));                                // ... and the ground below: exactly 24 px
        CHECK(f0.rowNear(iy + 1, x + 8, r - 8, field));
        CHECK(f0.rowNear(iy + kIn - 2, x + 8, r - 8, field));
        CHECK(f0.colNear(x, iy + 8, iy + kIn - 8, lineStrong));                       // left border
        CHECK(f0.colNear(r - 1, iy + 8, iy + kIn - 8, lineStrong));                   // right border
        CHECK(f0.near(x + 4, iy + 12, field));
        CHECKV(f0.near(x, iy, bg, 14), "top-left corner pixel is %s, not the ground", hex(f0.at(x, iy)).c_str());   // radius 6: the corner is cut
        CHECKV(f0.near(r - 1, iy + kIn - 1, bg, 14), "bottom-right corner pixel is %s", hex(f0.at(r - 1, iy + kIn - 1)).c_str());
        // the placeholder, in Theme.disabled, starts at the 9 px inset; no other ink in the box
        const Ink ph = inkBox(f0, x + 7, iy + 2, r - 7, iy + kIn - 2, field);
        CHECKV(ph.n > 20 && ph.x0 >= x + 9 && ph.x0 <= x + 14, "placeholder ink n=%d x0=%d (box %d)", ph.n, ph.x0, x);
        CHECKV(f0.countNear(x + 7, iy + 2, r - 7, iy + kIn - 2, disabled, 28) >= 8, "placeholder colour: %d", f0.countNear(x + 7, iy + 2, r - 7, iy + kIn - 2, disabled, 28));
        CHECK(f0.countNear(x + 7, iy + 2, r - 7, iy + kIn - 2, fg, 12) == 0);         // not the typed colour
        CHECK(f0.countNear(x, iy + kIn + 2, r, iy + kIn + 12, errFg, 20) == 0);        // no error line without an error
    }

    // =========================================================================================================
    // 3. NUMBER: mono, right-aligned; the same box
    // =========================================================================================================
    {
        const int x = kRightX, y = kTextY, r = kRightX + kColW, iy = y + kInputDy;
        CHECK(f0.rowNear(iy, x + 8, r - 8, lineStrong));
        CHECK(f0.rowNear(iy + kIn - 1, x + 8, r - 8, lineStrong));
        CHECK(f0.near(x + 4, iy + 12, field));
        const Ink v = inkBox(f0, x + 7, iy + 2, r - 7, iy + kIn - 2, field);
        const int leftGap = v.x0 - x, rightGap = r - 1 - v.x1;
        CHECKV(v.n > 8 && rightGap < leftGap / 4, "number ink n=%d: left gap %d, right gap %d", v.n, leftGap, rightGap);
        CHECKV(rightGap >= 8 && rightGap <= 14, "the figures end %d px inside the right edge (9 px inset)", rightGap);
        CHECKV(f0.countNear(x + 7, iy + 2, r - 7, iy + kIn - 2, fg, 30) >= 4, "figure colour fg: %d", f0.countNear(x + 7, iy + 2, r - 7, iy + kIn - 2, fg, 30));
        CHECK(f0.countNear(x, y, r, iy + kIn + 1, lampRed, 20) == 0);                  // no red anywhere on a clean field
        // a text input is left-aligned, for contrast: the Station field starts at the left inset
        const Ink s = inkBox(f0, kRightX + 7, kTipY + kInputDy + 2, kRightX + kColW - 7, kTipY + kInputDy + kIn - 2, field);
        CHECKV(s.n > 8 && s.x0 - kRightX >= 9 && s.x0 - kRightX <= 14, "text ink starts %d px in", s.x0 - kRightX);
    }

    // =========================================================================================================
    // 4. The error variant: a red border and the red sentence under the input
    // =========================================================================================================
    {
        const int x = kLeftX, y = kErrY, r = kLeftX + kColW, iy = y + kInputDy;
        CHECK(f0.rowNear(iy, x + 8, r - 8, lampRed));
        CHECK(f0.rowNear(iy + kIn - 1, x + 8, r - 8, lampRed));
        CHECK(f0.colNear(x, iy + 8, iy + kIn - 8, lampRed));
        CHECK(f0.colNear(r - 1, iy + 8, iy + kIn - 8, lampRed));
        CHECK(f0.near(x + 4, iy + 12, field));                                        // the ground is unchanged
        CHECK(f0.countNear(x + 8, iy, r - 8, iy + 1, lineStrong, 8) == 0 && f0.countNear(x + 8, iy + kIn - 1, r - 8, iy + kIn, lineStrong, 8) == 0);   // no grey border left on the box
        // "3,x" in the number face, right-aligned
        const Ink v = inkBox(f0, x + 7, iy + 2, r - 7, iy + kIn - 2, field);
        CHECKV(v.n > 8 && (r - 1 - v.x1) < (v.x0 - x) / 4, "error value ink n=%d x0=%d x1=%d", v.n, v.x0, v.x1);
        // the error line: Theme.chip-err-fg, 4 px under the input, one line, inside the field's own height
        const int ly = iy + kIn + kGap;
        const Ink line = inkBox(f0, x, iy + kIn + 1, r, ly + 20, bg);
        CHECKV(line.n > 40 && line.y0 >= ly && line.y0 <= ly + 3, "error line ink n=%d starts at row %d (input ends %d, line box %d)", line.n, line.y0, iy + kIn, ly);
        CHECKV(f0.countNear(x, ly, r, ly + 20, errFg, 40) >= 25, "error line colour chip-err-fg: %d", f0.countNear(x, ly, r, ly + 20, errFg, 40));
        CHECKV(line.y1 - line.y0 <= 14, "the sentence is one line: %d rows of ink", line.y1 - line.y0 + 1);
        CHECKV(line.y1 < y + (int)ui->get_err_h(), "error ink ends at row %d, the field's own bottom is %d", line.y1, y + (int)ui->get_err_h());
        CHECK(f0.countNear(x, y, r, iy - 1, errFg, 20) == 0);                          // none above the input
        CHECK(f0.countNear(kRightX, y, kRightX + kColW, y + 60, errFg, 20) == 0);      // and none beside it: the BOOL field is clean
    }

    // =========================================================================================================
    // 5. BOOL: a 24 px row with a 14 px box, the word "on", the kind cap
    // =========================================================================================================
    {
        const int x = kRightX, y = kBoolY, r = kRightX + kColW, ry = y + kInputDy;     // the control row
        const int by = ry + (kIn - kBox) / 2;                                          // the box is centred in the 24
        const Ink box = inkBox(f0, x - 4, ry - 3, x + 20, ry + kIn + 3, bg);
        CHECKV(box.x0 == x && box.x1 == x + kBox - 1 && box.y0 == by && box.y1 == by + kBox - 1,
               "checked box spans (%d,%d)-(%d,%d), wanted (%d,%d)-(%d,%d)", box.x0, box.y0, box.x1, box.y1, x, by, x + kBox - 1, by + kBox - 1);
        CHECKV(f0.countNear(x + 1, by + 1, x + kBox - 1, by + kBox - 1, accent, 8) >= 80, "accent fill: %d of 144", f0.countNear(x + 1, by + 1, x + kBox - 1, by + kBox - 1, accent, 8));
        CHECKV(f0.countNear(x + 1, by + 1, x + kBox - 1, by + kBox - 1, accentInk, 40) >= 6, "tick in accent-ink: %d", f0.countNear(x + 1, by + 1, x + kBox - 1, by + kBox - 1, accentInk, 40));
        CHECK(f0.colNear(x, by + 3, by + kBox - 3, accent));                           // the border is the accent too
        CHECK(!f0.near(x, by, accent, 30));                                            // 2 px corners: the corner pixel is only partly covered
        const Ink on = inkBox(f0, x + kBox + 2, ry, r - 60, ry + kIn, bg);
        CHECKV(on.n > 6 && on.x0 >= x + kBox + 6 && on.x0 <= x + kBox + 10, "the word 'on' starts %d px after the box (8 px gap)", on.x0 - (x + kBox));
        CHECK(f0.countNear(x + kBox + 2, ry, r - 60, ry + kIn, fg, 90) >= 4);          // in Theme.fg
        // the label row above carries "Log to file" and BOOL
        CHECK(f0.countNear(x, y, x + 120, y + kLabelH, fgSoft, 40) >= 8);
        const Ink cap = inkBox(f0, r - 60, y, r, y + kLabelH, bg);
        CHECKV(cap.n > 8 && cap.x1 >= r - 4, "BOOL cap right edge %d", cap.x1);
    }

    // =========================================================================================================
    // 6. PATH: the input and the small BROWSE FILES key beside it, 6 px apart
    // =========================================================================================================
    int browseLeft = 0;
    {
        const int x = kLeftX, y = kPathY, r = kLeftX + kWideW, iy = y + kInputDy;
        browseLeft = keyLeft(f0, iy + 6, r - 2, keyBg, false);                         // row 6: past the 6 px corner, above the label, so all ground
        const int keyW = r - browseLeft;
        CHECKV(keyW >= 70 && keyW <= 150, "BROWSE FILES key is %d px wide (x %d..%d)", keyW, browseLeft, r - 1);
        CHECK(f0.near(browseLeft, iy + 12, keyLine, 8));                               // its left border
        CHECK(f0.near(r - 1, iy + 12, keyLine, 8));
        CHECK(f0.colNear(browseLeft + 8, iy + 1, iy + kIn - 1, keyBg, 3));             // 24 px high (small key): ground edge to edge ...
        CHECK(f0.near(browseLeft + 8, iy - 1, bg));                                    // ... and ground just outside it
        CHECK(f0.near(browseLeft + 8, iy + kIn, bg));
        CHECKV(f0.countInk(browseLeft + 2, iy + 2, r - 2, iy + kIn - 2, keyBg) >= 30, "the key's label is drawn");
        CHECK(f0.rowNear(iy + 12, browseLeft - 6, browseLeft, bg));                    // the 6 px gap
        CHECK(f0.near(browseLeft - 7, iy + 12, lineStrong));                           // the input's right border
        CHECK(f0.rowNear(iy, x + 8, browseLeft - 14, lineStrong));                     // the input runs up to the gap
        CHECK(f0.near(x + 4, iy + 12, field));
        const Ink path = inkBox(f0, x + 7, iy + 2, browseLeft - 14, iy + kIn - 2, field);
        CHECKV(path.n > 30 && path.x0 >= x + 9 && path.x0 <= x + 14, "path ink n=%d starts %d px in", path.n, path.x0 - x);
        CHECK(sval(ui->get_path_key_text()) == "BROWSE FILES");
    }

    // =========================================================================================================
    // 7. PASSWORD: dots, the SHOW key, the note; and the revealed twin with its lit HIDE key
    // =========================================================================================================
    int passLeft = 0, pass2Left = 0;
    {
        const int x = kLeftX, y = kPassY, r = kLeftX + kWideW, iy = y + kInputDy;
        passLeft = keyLeft(f0, iy + 6, r - 2, keyBg, false);
        CHECKV(r - passLeft >= 36 && r - passLeft <= 90, "SHOW key is %d px wide", r - passLeft);
        CHECK(f0.near(passLeft + 8, iy + 12, keyBg));                                  // not lit
        CHECK(f0.colNear(passLeft + 8, iy + 1, iy + kIn - 1, keyBg, 3));
        CHECK(sval(ui->get_pw_key_text()) == "SHOW");
        CHECK(ui->get_pw_revealed() == false);
        // the dots: ink in the figure colour across the input, none beyond the 14 characters' width
        const Ink dots = inkBox(f0, x + 7, iy + 2, passLeft - 14, iy + kIn - 2, field);
        CHECKV(dots.n >= 30 && dots.x0 >= x + 9 && dots.x0 <= x + 16, "dots ink n=%d from x %d", dots.n, dots.x0);
        CHECKV(f0.countNear(x + 7, iy + 2, passLeft - 14, iy + kIn - 2, fg, 40) >= 20, "dots colour: %d", f0.countNear(x + 7, iy + 2, passLeft - 14, iy + kIn - 2, fg, 40));
        CHECKV(dots.x1 < x + 9 + 14 * 12, "14 dots end at %d", dots.x1);                 // 14 characters, each a dot less than 12 px wide
        // the note: 11 px dim under the input, one line
        const int ny = iy + kIn + kGap;
        const Ink note = inkBox(f0, x, iy + kIn + 1, r, ny + 20, bg);
        CHECKV(note.n > 100 && note.y0 >= ny && note.y0 <= ny + 3, "note ink n=%d from row %d (line box %d)", note.n, note.y0, ny);
        CHECKV(f0.countNear(x, ny, r, ny + 20, dim, 40) >= 40, "note colour dim: %d", f0.countNear(x, ny, r, ny + 20, dim, 40));
        CHECK(f0.countNear(x, ny, r, ny + 20, errFg, 20) == 0);                        // a note is not an error

        // the same field, starting revealed: the text instead of dots, and the HIDE key lit
        const int iy2 = kPass2Y + kInputDy;
        pass2Left = keyLeft(f0, iy2 + 6, r - 2, accent, true);
        CHECKV(r - pass2Left >= 36 && r - pass2Left <= 90, "HIDE key is %d px wide", r - pass2Left);
        CHECK(f0.near(pass2Left + 8, iy2 + 12, accent));                               // lit: accent ground
        CHECK(f0.colNear(pass2Left + 8, iy2 + 1, iy2 + kIn - 1, accent, 3));
        CHECKV(f0.countNear(pass2Left + 2, iy2 + 2, r - 2, iy2 + kIn - 2, accentInk, 40) >= 8, "HIDE label in accent-ink");
        const Ink text = inkBox(f0, x + 7, iy2 + 2, pass2Left - 14, iy2 + kIn - 2, field);
        CHECKV(text.n >= 30 && text.x0 >= x + 9 && text.x0 <= x + 14, "revealed text ink n=%d from x %d", text.n, text.x0);
        // dots and text are different pictures of the same 14 characters
        const int d = differingShifted(f0, f0, x + 9, iy + 2, x + 9 + 100, iy + kIn - 2, kPass2Y - kPassY);
        CHECKV(d >= 40, "hidden and revealed inputs differ in %d pixels", d);
    }

    // =========================================================================================================
    // 8. DISABLED: the kit's disabled look on a text, a bool and a path field
    // =========================================================================================================
    {
        const int x = kLeftX, y = kDisY, r = kLeftX + kColW, iy = y + kInputDy;
        CHECK(f0.near(x + 4, iy + 12, keyDisBg));                                      // Theme.key-disabled-bg ground
        const Rgb halfEdge = over(lineStrong, 0.5, keyDisBg);
        CHECKV(f0.rowNear(iy, x + 8, r - 8, halfEdge, 6), "disabled border row is %s, wanted about %s", hex(f0.at(x + 20, iy)).c_str(), hex(halfEdge).c_str());
        CHECK(f0.countNear(x + 7, iy + 2, r - 7, iy + kIn - 2, keyDisFg, 24) >= 8);     // its text in key-disabled-fg
        CHECK(f0.countNear(x + 7, iy + 2, r - 7, iy + kIn - 2, fg, 12) == 0);
        // the label dims too
        CHECKV(f0.countNear(x, y, x + 120, y + kLabelH, keyDisFg, 30) >= 8, "disabled label in key-disabled-fg");
        CHECK(f0.countNear(x, y, x + 120, y + kLabelH, fgSoft, 12) == 0);
        // the bool: ground, tick and half-alpha border
        const int bx = kRightX, by = kDisY + kInputDy + (kIn - kBox) / 2;
        CHECK(f0.near(bx + 3, by + 3, keyDisBg));
        CHECKV(f0.countNear(bx + 1, by + 1, bx + kBox - 1, by + kBox - 1, keyDisFg, 30) >= 4, "disabled tick in key-disabled-fg");
        CHECK(f0.countNear(bx + 1, by + 1, bx + kBox - 1, by + kBox - 1, accent, 20) == 0);
        CHECKV(f0.colNear(bx, by + 3, by + kBox - 3, halfEdge, 6), "disabled box border is %s", hex(f0.at(bx, by + 6)).c_str());
        // the path: its key wears the disabled look
        const int piy = kDisPathY + kInputDy, pr = kLeftX + kColW;
        const int kl = keyLeft(f0, piy + 6, pr - 2, keyDisBg, false);
        CHECKV(pr - kl >= 70 && pr - kl <= 150, "disabled BROWSE FILES key is %d px wide", pr - kl);
        CHECK(f0.near(kl + 8, piy + 12, keyDisBg));
        CHECK(f0.countNear(kl + 2, piy + 2, pr - 2, piy + kIn - 2, keyDisFg, 24) >= 8);
        CHECK(pr - kl == (kLeftX + kWideW) - browseLeft);                              // the same label, the same width
    }

    // =========================================================================================================
    // 9. The words a host hands in: the kind cap, the BROWSE / SHOW / HIDE words and the BOOL's "on"
    // =========================================================================================================
    {
        CHECK(sval(ui->get_fr_path_kind_text()) == "CHEMIN");
        CHECK(sval(ui->get_fr_path_key_text()) == "PARCOURIR");
        CHECK(sval(ui->get_fr_pass_kind_text()) == "MOT DE PASSE");
        CHECK(sval(ui->get_fr_pass_key_text()) == "AFFICHER");
        // the shorter word makes the key narrower than BROWSE FILES (both end at their field's right edge)
        const int iy = kFrPathY + kInputDy, r = kRightX + kColW;
        const int frLeft = keyLeft(f0, iy + 6, r - 2, keyBg, false);
        const int enW = (kLeftX + kWideW) - browseLeft, frW = r - frLeft;
        CHECKV(frW >= 50 && frW < enW - 10, "PARCOURIR key is %d px, BROWSE FILES is %d", frW, enW);
        // the cap is the host's word: BOOLEEN is wider than BOOL, CHEMIN than PATH
        const Ink capBool = inkBox(f0, kRightX + kColW - 100, kBoolY, kRightX + kColW, kBoolY + kLabelH, bg);
        const Ink capFrBool = inkBox(f0, kLeftX + kColW - 100, kFrBoolY, kLeftX + kColW, kFrBoolY + kLabelH, bg);
        CHECKV(capFrBool.w() >= capBool.w() + 8, "BOOLEEN cap is %d px wide, BOOL %d", capFrBool.w(), capBool.w());
        CHECK(capFrBool.x1 >= kLeftX + kColW - 4);                                     // still flush right
        // "oui" is a longer word than "on"
        const Ink on = inkBox(f0, kRightX + kBox + 4, kBoolY + kInputDy, kRightX + kColW - 60, kBoolY + kInputDy + kIn, bg);
        const Ink oui = inkBox(f0, kLeftX + kBox + 4, kFrBoolY + kInputDy, kLeftX + kColW - 60, kFrBoolY + kInputDy + kIn, bg);
        CHECKV(on.n > 6 && oui.n > on.n && oui.w() > on.w(), "'oui' ink %d px wide, 'on' %d", oui.w(), on.w());
        // the French password's key opens as AFFICHER and lights as MASQUER
        const int piy = kFrPassY + kInputDy, pr = kRightX + kColW;
        const int showLeft = keyLeft(f0, piy + 6, pr - 2, keyBg, false);
        CHECKV(pr - showLeft >= 40 && f0.near(showLeft + 8, piy + 12, keyBg), "AFFICHER key at %d", showLeft);
        reset();
        click(win, showLeft + 20, piy + kIn / 2);
        CHECKV(ui->get_fr_pass_revealed() && sval(ui->get_fr_pass_key_text()) == "MASQUER", "after AFFICHER: revealed %d, key '%s'", ui->get_fr_pass_revealed(), sval(ui->get_fr_pass_key_text()).c_str());
        park();
        Frame lit = render(ui, kW, kH);
        const int hideLeft = keyLeft(lit, piy + 6, pr - 2, accent, true);
        CHECKV(pr - hideLeft >= 40 && lit.near(hideLeft + 8, piy + 12, accent), "MASQUER key lit at %d", hideLeft);
        click(win, hideLeft + 20, piy + kIn / 2);
        CHECK(!ui->get_fr_pass_revealed() && sval(ui->get_fr_pass_key_text()) == "AFFICHER");
        park();
    }

    // =========================================================================================================
    // 10. The Checkbox: 14 px box, accent when checked, field ground when not, label 11 px in fg
    // =========================================================================================================
    {
        // checked
        const int x = kLeftX, y = kCbOnY;
        const Ink box = inkBox(f0, x - 4, y - 3, x + kBox + 6, y + kBox + 3, bg);
        CHECKV(box.x0 == x && box.x1 == x + kBox - 1 && box.y0 == y && box.y1 == y + kBox - 1, "checked box (%d,%d)-(%d,%d)", box.x0, box.y0, box.x1, box.y1);
        CHECKV(f0.countNear(x + 1, y + 1, x + kBox - 1, y + kBox - 1, accent, 8) >= 80, "accent fill %d", f0.countNear(x + 1, y + 1, x + kBox - 1, y + kBox - 1, accent, 8));
        CHECKV(f0.countNear(x + 1, y + 1, x + kBox - 1, y + kBox - 1, accentInk, 40) >= 6, "tick in accent-ink %d", f0.countNear(x + 1, y + 1, x + kBox - 1, y + kBox - 1, accentInk, 40));
        const Ink word = inkBox(f0, x + kBox + 2, y - 2, x + kColW, y + kBox + 2, bg);
        CHECKV(word.n > 60 && word.x0 >= x + kBox + 7 && word.x0 <= x + kBox + 10, "label ink n=%d starts %d px after the box", word.n, word.x0 - (x + kBox));
        CHECKV(f0.countNear(x + kBox + 2, y - 2, x + kColW, y + kBox + 2, fg, 30) >= 25, "label colour fg %d", f0.countNear(x + kBox + 2, y - 2, x + kColW, y + kBox + 2, fg, 30));
        CHECKV(std::abs(word.cy() - (y + kBox / 2.0)) <= 3.0, "label centre %.1f vs the box's %.1f", word.cy(), y + kBox / 2.0);
        // unchecked: the field ground and a line-strong border, no tick
        const int ux = kRightX;
        CHECK(f0.near(ux + 6, y + 3, field));
        CHECK(f0.colNear(ux, y + 3, y + kBox - 3, lineStrong));
        CHECK(f0.colNear(ux + kBox - 1, y + 3, y + kBox - 3, lineStrong));
        CHECK(f0.rowNear(y, ux + 3, ux + kBox - 3, lineStrong));
        CHECK(f0.rowNear(y + kBox - 1, ux + 3, ux + kBox - 3, lineStrong));
        CHECK(f0.countNear(ux + 1, y + 1, ux + kBox - 1, y + kBox - 1, accent, 20) == 0);
        CHECK(f0.countNear(ux + 1, y + 1, ux + kBox - 1, y + kBox - 1, accentInk, 5) == 0);
        CHECK(f0.countInk(ux + kBox, y, ux + kBox + 8, y + kBox, bg) == 0);             // the 8 px gap before the label
        // the wrapping one: the box stays beside the FIRST line, the label wraps inside its width
        const Ink wbox = inkBox(f0, kRightX - 4, kCbWrapY - 3, kRightX + kBox + 4, kCbWrapY + 20, bg);
        CHECKV(wbox.y0 == kCbWrapY + 1 && wbox.x0 == kRightX, "wrapped label's box at (%d,%d), wanted (%d,%d)", wbox.x0, wbox.y0, kRightX, kCbWrapY + 1);
        const Ink wrap = inkBox(f0, kRightX + kBox + 2, kCbWrapY - 2, kRightX + kColW + 10, kCbWrapY + 60, bg);
        CHECKV(wrap.n > 150 && wrap.x1 < kRightX + kColW, "wrapped label ink n=%d right edge %d (column ends %d)", wrap.n, wrap.x1, kRightX + kColW);
        CHECKV(wrap.y1 - wrap.y0 >= 30, "wrapped label spans %d rows of ink", wrap.y1 - wrap.y0 + 1);
        // the disabled one
        CHECK(f0.near(kLeftX + 3, kCbDisY + 3, keyDisBg));
        CHECK(f0.countNear(kLeftX + 1, kCbDisY + 1, kLeftX + kBox - 1, kCbDisY + kBox - 1, keyDisFg, 30) >= 4);
        CHECK(f0.countNear(kLeftX + kBox + 2, kCbDisY - 2, kLeftX + kColW, kCbDisY + kBox + 2, keyDisFg, 30) >= 20);   // label dimmed
        CHECK(f0.countNear(kLeftX + kBox + 2, kCbDisY - 2, kLeftX + kColW, kCbDisY + kBox + 2, fg, 12) == 0);
    }

    // =========================================================================================================
    // 11. Typing: edited per change, committed on Enter and on losing the focus, once per edit
    // =========================================================================================================
    {
        reset();
        const int ix = kLeftX + 100, iy = kTextY + kInputDy + kIn / 2;                  // the middle of the Callsign filter input
        const int nx = kRightX + 100;                                                    // the number field's input
        click(win, ix, iy);
        tap(u8"a");
        tap(u8"b");
        CHECKV(ui->get_edits() == 2, "edited fired %d times for two keys", ui->get_edits());
        CHECK(sval(ui->get_last_value()) == "ab");
        CHECK(sval(ui->get_typed()) == "ab");
        CHECK(ui->get_commits() == 0);                                                  // typing alone commits nothing
        Frame t = render(ui, kW, kH);
        CHECKV(t.near(kLeftX - 2, iy, focus, 14), "focus ring at the left: %s", hex(t.at(kLeftX - 2, iy)).c_str());      // the 2 px ring, 1 px clear
        CHECK(t.near(kLeftX - 3, iy, focus, 14));
        CHECK(t.near(kLeftX - 1, iy, bg, 14));
        {   // the placeholder gives way to the typed text: "ab" is a narrower picture than "e.g. M0ABC" and in the typed colour
            const int tx0 = kLeftX + 7, tx1 = kLeftX + kColW - 7, ty0 = kTextY + kInputDy + 3, ty1 = kTextY + kInputDy + kIn - 3;
            const Ink before = inkBox(f0, tx0, ty0, tx1, ty1, field), typedInk = inkBox(t, tx0, ty0, tx1, ty1, field);
            CHECKV(typedInk.n > 4 && typedInk.w() < before.w() / 2, "typed ink %d px wide, the placeholder's %d", typedInk.w(), before.w());
            CHECK(t.countNear(tx0, ty0, tx1, ty1, fg, 90) >= 4);
        }
        tap(slint::platform::key_codes::Return);
        CHECKV(ui->get_commits() == 1 && sval(ui->get_last_value()) == "ab", "Enter: %d commits, last '%s'", ui->get_commits(), sval(ui->get_last_value()).c_str());
        CHECK(ui->get_edits() == 2);
        // a click away after Enter commits nothing more: nothing was edited since
        click(win, nx, iy);
        CHECKV(ui->get_commits() == 1, "Enter then a click away: %d commits", ui->get_commits());
        Frame left = render(ui, kW, kH);
        CHECKV(left.near(kLeftX - 2, iy, bg, 14), "the ring leaves with the focus: %s", hex(left.at(kLeftX - 2, iy)).c_str());
        CHECK(left.near(kRightX - 2, iy, focus, 14));                                    // and shows on the field that took it
        // typing then a click on another input commits on the focus leaving
        click(win, ix, iy);
        tap(u8"c");
        CHECK(ui->get_commits() == 1);
        click(win, nx, iy);
        CHECKV(ui->get_commits() == 2 && sval(ui->get_last_value()) == "abc", "focus lost: %d commits, last '%s'", ui->get_commits(), sval(ui->get_last_value()).c_str());
        CHECK(sval(ui->get_typed()) == "abc");
        // Backspace is an edit
        click(win, ix, iy);
        tap(slint::platform::key_codes::Backspace);
        CHECKV(sval(ui->get_typed()) == "ab" && sval(ui->get_last_value()) == "ab", "Backspace left '%s'", sval(ui->get_typed()).c_str());
        // the padding around the text is part of the input: a click there focuses it
        click(win, nx, iy);
        click(win, kLeftX + 4, iy);
        tap(u8"z");
        CHECKV(sval(ui->get_typed()) == "abz", "typing after a click on the padding gave '%s'", sval(ui->get_typed()).c_str());
        // Enter commits the whole text
        reset();
        tap(slint::platform::key_codes::Return);
        CHECK(ui->get_commits() == 1 && sval(ui->get_last_value()) == "abz");
        // NUMBER is not a filter: letters and commas go in, so the host can answer "that is not a number"
        click(win, nx, iy);
        tap(slint::platform::key_codes::End);
        reset();
        tap(u8"x");
        tap(u8",");
        CHECKV(ui->get_edits() == 2 && sval(ui->get_last_value()) == "3.5x,", "number field took '%s' in %d edits", sval(ui->get_last_value()).c_str(), ui->get_edits());
        tap(slint::platform::key_codes::Return);
        CHECK(ui->get_commits() == 1);
        park();
    }

    // =========================================================================================================
    // 12. max-length: typing stops at the limit and says nothing about the refused keys
    // =========================================================================================================
    {
        reset();
        click(win, kLeftX + 100, kMaxY + kInputDy + kIn / 2);
        for (const std::u8string_view c : {u8"a", u8"b", u8"c", u8"d", u8"e", u8"f"}) { tap(c); }
        CHECKV(sval(ui->get_limited()) == "abcd", "limited field holds '%s'", sval(ui->get_limited()).c_str());
        CHECKV(ui->get_edits() == 4 && sval(ui->get_last_value()) == "abcd", "%d edits reported, last '%s'", ui->get_edits(), sval(ui->get_last_value()).c_str());
        tap(slint::platform::key_codes::Backspace);
        CHECK(sval(ui->get_limited()) == "abc" && ui->get_edits() == 5);
        tap(u8"z");
        CHECKV(sval(ui->get_limited()) == "abcz" && ui->get_edits() == 6, "after Backspace and z: '%s' in %d edits", sval(ui->get_limited()).c_str(), ui->get_edits());
        Frame m = render(ui, kW, kH);
        CHECK(m.countInk(kLeftX + 9, kMaxY + kInputDy + 4, kLeftX + 60, kMaxY + kInputDy + kIn - 4, field) >= 20);   // the four characters are drawn
        park();
    }

    // =========================================================================================================
    // 13. A disabled field takes no typing, no focus, and its key does nothing
    // =========================================================================================================
    {
        ui->invoke_focus_checkbox();                                                     // the focus is on a Checkbox, not on a text input
        settle(milliseconds(20));                                                        // (a dirty field commits as the focus leaves it, a turn later)
        reset();
        click(win, kLeftX + 100, kDisY + kInputDy + kIn / 2);
        tap(u8"x");
        tap(slint::platform::key_codes::Return);
        CHECK(ui->get_key_checkbox_focused());                                           // the click did not take the focus
        CHECKV(ui->get_edits() == 0 && ui->get_commits() == 0, "disabled text field fired %d edits, %d commits", ui->get_edits(), ui->get_commits());
        CHECKV(sval(ui->get_dis_value()) == "M0ABC", "disabled text now '%s'", sval(ui->get_dis_value()).c_str());
        Frame d = render(ui, kW, kH);
        CHECK(d.countNear(kLeftX - 4, kDisY + kInputDy - 4, kLeftX + kColW + 4, kDisY + kInputDy + kIn + 4, focus, 20) == 0);   // no focus ring
        // the disabled path: its key (the same label as the enabled path's, so the same width, ending at its own right edge)
        const int piy = kDisPathY + kInputDy, pr = kLeftX + kColW, keyW = (kLeftX + kWideW) - browseLeft;
        click(win, pr - keyW / 2, piy + kIn / 2);
        click(win, kLeftX + 40, piy + kIn / 2);
        tap(u8"y");
        CHECKV(ui->get_browses() == 0 && ui->get_edits() == 0, "disabled path fired %d browses, %d edits", ui->get_browses(), ui->get_edits());
        CHECK(sval(ui->get_dis_path_value()) == "C:\\logs\\aprs.txt");
        // the disabled bool
        const int by = kDisY + kInputDy + kIn / 2;
        click(win, kRightX + kBox / 2, by);
        click(win, kRightX + 80, by);
        CHECKV(ui->get_toggles() == 0 && ui->get_dis_bool_checked(), "disabled bool toggled %d times", ui->get_toggles());
        // and a disabled Checkbox
        click(win, kLeftX + kBox / 2, kCbDisY + kBox / 2);
        click(win, kLeftX + 80, kCbDisY + kBox / 2);
        CHECKV(ui->get_toggles() == 0 && ui->get_cb_dis_checked(), "disabled checkbox toggled %d times", ui->get_toggles());
        park();
    }

    // =========================================================================================================
    // 14. BOOL: a click on the box or the row toggles; the callback and the property agree
    // =========================================================================================================
    {
        reset();
        const int fy = kBoolY + kInputDy + kIn / 2;                                     // the BOOL row's middle
        click(win, kRightX + kBox / 2, fy);
        CHECKV(ui->get_toggles() == 1 && !ui->get_last_checked() && !ui->get_bool_checked(), "BOOL click: %d toggles, last %d, checked %d", ui->get_toggles(), ui->get_last_checked(), ui->get_bool_checked());
        CHECK(ui->get_edits() == 0 && ui->get_commits() == 0);                           // a bool is not an edit
        Frame a = render(ui, kW, kH);
        const int by = kBoolY + kInputDy + (kIn - kBox) / 2;
        CHECK(a.near(kRightX + 6, by + 3, field));                                      // now unchecked: the field ground ...
        CHECK(a.colNear(kRightX, by + 3, by + kBox - 3, lineStrong));                  // ... and the strong border
        CHECK(a.countNear(kRightX + 1, by + 1, kRightX + kBox - 1, by + kBox - 1, accent, 20) == 0);
        click(win, kRightX + 40, fy);                                                    // the word "on" and the row beside it also toggle
        CHECKV(ui->get_toggles() == 2 && ui->get_last_checked() && ui->get_bool_checked(), "second BOOL click: %d toggles, last %d", ui->get_toggles(), ui->get_last_checked());
        park();
        Frame b = render(ui, kW, kH);
        CHECK(b.near(kRightX + 3, by + 3, accent));
        park();
    }

    // =========================================================================================================
    // 15. Checkbox: the box or the label toggles; toggled(bool) carries the new state
    // =========================================================================================================
    {
        reset();
        click(win, kRightX + kBox / 2, kCbOnY + kBox / 2);                              // the unchecked one, at the right
        CHECKV(ui->get_toggles() == 1 && ui->get_last_checked() && ui->get_cb_off_checked(), "Checkbox click: %d toggles, last %d", ui->get_toggles(), ui->get_last_checked());
        Frame hot = render(ui, kW, kH);
        CHECKV(hot.near(kRightX + 3, kCbOnY + 3, accentHover), "a checked box under the pointer lights: %s", hex(hot.at(kRightX + 3, kCbOnY + 3)).c_str());
        park();
        Frame a = render(ui, kW, kH);
        CHECK(a.near(kRightX + 3, kCbOnY + 3, accent));
        CHECK(a.countNear(kRightX + 1, kCbOnY + 1, kRightX + kBox - 1, kCbOnY + kBox - 1, accentInk, 40) >= 6);
        click(win, kRightX + 120, kCbOnY + kBox / 2);                                    // the label toggles it back
        CHECKV(ui->get_toggles() == 2 && !ui->get_last_checked() && !ui->get_cb_off_checked(), "label click: %d toggles, last %d", ui->get_toggles(), ui->get_last_checked());
        Frame b = render(ui, kW, kH);
        CHECK(b.near(kRightX + 6, kCbOnY + 3, field));
        reset();
        click(win, kLeftX + kBox / 2, kCbOnY + kBox / 2);                                // the checked one turns off
        CHECK(ui->get_toggles() == 1 && !ui->get_last_checked() && !ui->get_cb_on_checked());
        CHECK(render(ui, kW, kH).near(kLeftX + 6, kCbOnY + 3, field));
        park();
    }

    // =========================================================================================================
    // 16. Keyboard: Space toggles the focused Checkbox and a 2 px ring shows round its box
    // =========================================================================================================
    {
        click(win, kLeftX + 100, kTextY + kInputDy + kIn / 2);                           // the focus is in a text input, so the Checkbox has none
        settle(milliseconds(20));
        reset();
        Frame before = render(ui, kW, kH);
        CHECK(before.countNear(kLeftX - 5, kCbKeyY - 5, kLeftX + kBox + 5, kCbKeyY + kBox + 5, focus, 20) == 0);
        CHECK(!ui->get_key_checkbox_focused());
        ui->invoke_focus_checkbox();
        tap(u8" ");
        CHECKV(ui->get_toggles() == 1 && ui->get_last_checked() && ui->get_cb_key_checked(), "Space: %d toggles", ui->get_toggles());
        CHECK(ui->get_key_checkbox_focused());
        Frame a = render(ui, kW, kH);
        CHECKV(a.near(kLeftX - 2, kCbKeyY + kBox / 2, focus, 14), "ring left of the box: %s", hex(a.at(kLeftX - 2, kCbKeyY + kBox / 2)).c_str());
        CHECK(a.near(kLeftX - 1, kCbKeyY + kBox / 2, bg, 14));                           // 1 px clear
        CHECK(a.near(kLeftX + kBox / 2, kCbKeyY - 3, focus, 14));                        // above
        CHECK(a.near(kLeftX + kBox / 2, kCbKeyY + kBox + 2, focus, 14));                 // below
        CHECK(a.near(kLeftX + 3, kCbKeyY + 3, accent));                                 // and the box is lit
        tap(u8" ");
        CHECK(ui->get_toggles() == 2 && !ui->get_cb_key_checked());
        tap(u8"a");                                                                      // other keys do nothing
        CHECK(ui->get_toggles() == 2);
        park();
    }

    // =========================================================================================================
    // 17. PATH: the key fires browse-clicked
    // =========================================================================================================
    {
        reset();
        const int iy = kPathY + kInputDy;
        click(win, browseLeft + 30, iy + kIn / 2);
        CHECKV(ui->get_browses() == 1 && ui->get_edits() == 0 && ui->get_toggles() == 0, "BROWSE FILES: %d browses", ui->get_browses());
        click(win, browseLeft + 1, iy + 1);                                              // its corner still counts
        CHECK(ui->get_browses() == 2);
        click(win, browseLeft - 10, iy + kIn / 2);                                       // the input is not the key
        CHECK(ui->get_browses() == 2);
        // the host's word for the key is the key, too
        click(win, kRightX + kColW - 20, kFrPathY + kInputDy + kIn / 2);
        CHECK(ui->get_browses() == 3);
        park();
    }

    // =========================================================================================================
    // 18. PASSWORD: a click on SHOW reveals the text and lights the key as HIDE; a second click hides it again
    // =========================================================================================================
    {
        reset();
        const int iy = kPassY + kInputDy, r = kLeftX + kWideW;
        Frame hidden = render(ui, kW, kH);
        click(win, passLeft + 20, iy + kIn / 2);
        CHECKV(ui->get_pw_revealed() && sval(ui->get_pw_key_text()) == "HIDE", "after SHOW: revealed %d, key '%s'", ui->get_pw_revealed(), sval(ui->get_pw_key_text()).c_str());
        CHECK(ui->get_toggles() == 0 && ui->get_browses() == 0 && ui->get_edits() == 0);
        park();
        Frame shown = render(ui, kW, kH);
        const int hideLeft = keyLeft(shown, iy + 6, r - 2, accent, true);
        CHECKV(r - hideLeft >= 36 && r - hideLeft <= 90 && shown.near(hideLeft + 8, iy + 12, accent), "HIDE key lit: left %d, ground %s", hideLeft, hex(shown.at(hideLeft + 8, iy + 12)).c_str());
        CHECK(shown.colNear(hideLeft + 8, iy + 1, iy + kIn - 1, accent, 3));
        CHECK(shown.countNear(hideLeft + 2, iy + 2, r - 2, iy + kIn - 2, accentInk, 40) >= 8);
        const int changed = differing(hidden, shown, kLeftX + 9, iy + 3, kLeftX + 9 + 100, iy + kIn - 3);
        CHECKV(changed >= 40, "the input shows different pixels once revealed: %d", changed);
        CHECK(hideLeft == pass2Left);                                                    // the lit key matches the revealed twin's
        // and back
        click(win, hideLeft + 20, iy + kIn / 2);
        CHECKV(!ui->get_pw_revealed() && sval(ui->get_pw_key_text()) == "SHOW", "after HIDE: revealed %d, key '%s'", ui->get_pw_revealed(), sval(ui->get_pw_key_text()).c_str());
        park();
        Frame again = render(ui, kW, kH);
        const int dd = differing(hidden, again, kLeftX, iy - 4, r, iy + kIn + 4);
        CHECKV(dd == 0, "the hidden picture returns exactly: %d pixels differ", dd);
        // pressing SHOW while typing leaves the caret where it was (a key never steals the focus)
        reset();
        click(win, kLeftX + 150, iy + kIn / 2);                                           // focus the password input
        tap(u8"Q");
        CHECK(ui->get_edits() == 1);
        click(win, passLeft + 20, iy + kIn / 2);                                          // SHOW
        tap(u8"R");
        CHECKV(ui->get_edits() == 2 && ui->get_pw_revealed(), "typing after pressing SHOW: %d edits", ui->get_edits());
        click(win, hideLeft + 20, iy + kIn / 2);                                          // HIDE again
        CHECK(!ui->get_pw_revealed());
        park();
    }

    // =========================================================================================================
    // 19. Tooltips: the input's, through TipTimer, 6 px under the box, whether the pointer is over the text or the padding
    // =========================================================================================================
    {
        const int iy = kTipY + kInputDy;                                                 // the Station input
        Frame none = render(ui, kW, kH);
        CHECK(none.near(kRightX + 5, iy + kIn + 12, bg));                                // nothing under it yet
        moveTo(win, kRightX + 100, iy + kIn / 2);                                        // over the text
        settle(milliseconds(750));
        Frame tip = render(ui, kW, kH);
        CHECKV(tip.near(kRightX + 5, iy + kIn + 12, menu), "tooltip ground under the input: %s", hex(tip.at(kRightX + 5, iy + kIn + 12)).c_str());
        CHECK(tip.near(kRightX + 5, iy + kIn + 3, bg));                                  // the 6 px between the box and the tip
        park();
        Frame gone = render(ui, kW, kH);
        CHECK(gone.near(kRightX + 5, iy + kIn + 12, bg));
        moveTo(win, kRightX + 3, iy + kIn / 2);                                          // over the padding
        settle(milliseconds(750));
        CHECK(render(ui, kW, kH).near(kRightX + 5, iy + kIn + 12, menu));
        park();
        // a field with no tooltip opens none
        moveTo(win, kRightX + 100, kTextY + kInputDy + kIn / 2);
        settle(milliseconds(750));
        CHECK(render(ui, kW, kH).near(kRightX + 5, kTextY + kInputDy + kIn + 12, bg));
        park();
        // a key inside a field carries its own tooltip: SHOW says "Show", 6 px under the key
        moveTo(win, passLeft + 20, kPassY + kInputDy + kIn / 2);
        settle(milliseconds(750));
        Frame k = render(ui, kW, kH);
        CHECKV(k.near(passLeft + 5, kPassY + kInputDy + kIn + 12, menu), "SHOW's tooltip ground: %s", hex(k.at(passLeft + 5, kPassY + kInputDy + kIn + 12)).c_str());
        park();
        // a BOOL field hands its tooltip to the Checkbox row: 6 px under the 24 px row
        const int ry = kBoolY + kInputDy;
        CHECK(render(ui, kW, kH).countNear(kRightX + 1, ry + kIn + 6 + 4, kRightX + 7, ry + kIn + 6 + 18, menu, 2) <= 16);
        moveTo(win, kRightX + 60, ry + kIn / 2);
        settle(milliseconds(750));
        Frame b = render(ui, kW, kH);
        CHECKV(b.countNear(kRightX + 1, ry + kIn + 6 + 4, kRightX + 7, ry + kIn + 6 + 18, menu, 2) >= 80, "BOOL field tooltip: %d of 84 pixels are tip ground", b.countNear(kRightX + 1, ry + kIn + 6 + 4, kRightX + 7, ry + kIn + 6 + 18, menu, 2));
        park();
        // a standalone Checkbox: 6 px under its own 14 px
        moveTo(win, kLeftX + 80, kCbOnY + kBox / 2);
        settle(milliseconds(750));
        Frame c = render(ui, kW, kH);
        CHECKV(c.countNear(kLeftX + 1, kCbOnY + kBox + 6 + 4, kLeftX + 7, kCbOnY + kBox + 6 + 18, menu, 2) >= 80, "Checkbox tooltip: %d of 84 pixels are tip ground", c.countNear(kLeftX + 1, kCbOnY + kBox + 6 + 4, kLeftX + 7, kCbOnY + kBox + 6 + 18, menu, 2));
        park();
        CHECK(render(ui, kW, kH).countNear(kLeftX + 1, kCbOnY + kBox + 6 + 4, kLeftX + 7, kCbOnY + kBox + 6 + 18, menu, 2) <= 16);
    }

    // =========================================================================================================
    // 20. The skin: a second filling reaches the fields' pixels with no component change
    // =========================================================================================================
    {
        Frame before = render(ui, kW, kH);
        Skin probe = ui->global<Workbench>().get_value();
        probe.field = slint::Color::from_rgb_uint8(0x20, 0x30, 0x10);
        probe.line_strong = slint::Color::from_rgb_uint8(0xff, 0x00, 0xff);
        probe.lamp_red = slint::Color::from_rgb_uint8(0x00, 0xff, 0xff);
        probe.accent = slint::Color::from_rgb_uint8(0x00, 0xc0, 0x40);
        probe.chip_err_fg = slint::Color::from_rgb_uint8(0xff, 0xff, 0x00);
        ui->global<Theme>().set_skin(probe);
        Frame g = render(ui, kW, kH);
        const int errIy = kErrY + kInputDy, numIy = kTextY + kInputDy, boolBy = kBoolY + kInputDy + (kIn - kBox) / 2;
        CHECK(g.near(kLeftX + 4, errIy + 12, rgb(0x203010)));                            // the input ground
        CHECK(g.rowNear(errIy, kLeftX + 8, kLeftX + kColW - 8, rgb(0x00ffff)));          // the error border
        CHECK(g.rowNear(numIy, kRightX + 8, kRightX + kColW - 8, rgb(0xff00ff)));        // a clean input's border
        CHECK(g.countNear(kLeftX, errIy + kIn + kGap, kLeftX + kColW, errIy + kIn + kGap + 20, rgb(0xffff00), 40) >= 25);   // the error sentence
        CHECK(g.near(kRightX + 3, boolBy + 3, rgb(0x00c040)));                           // the checked box
        ui->global<Theme>().set_skin(ui->global<Workbench>().get_value());
        Frame h = render(ui, kW, kH);
        const int dd = differing(before, h, 0, 100, kW, 180);
        CHECKV(dd == 0, "the Workbench filling comes back: %d pixels differ", dd);
    }

    return testSummary("test_slint_fields");
}
