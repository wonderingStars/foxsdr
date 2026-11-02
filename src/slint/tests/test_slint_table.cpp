// The Table family (ui/table.slint): the frozen heading row, the row pitch, the band / selected / tone colours, the scrolling body,
// clicks, right-clicks, the empty state and the themed scroll bar - rendered headlessly from ui/specimens/table_specimen.slint and
// probed against the plugin kit's `.tbl` CSS (Workbench-PluginKit.dc.html).
//
// The sheet: Table A at (10, 10) 420 x 160, heading 22 high (rows y 10..31, its border on y 31), body rows 24 high from y 32 whose last
// pixel row is the hairline; columns Time 78 | Source 70 | Message (the rest, 216) | Level 56. Table B (empty) at (10, 180) 300 x 80.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "table_specimen.h"

#include <chrono>
#include <initializer_list>
#include <vector>

using namespace foxsdr::slinttest;

namespace {

Rgb tok(slint::Color c) { return {c.red(), c.green(), c.blue()}; }

// Pixels of the rectangle that differ at all between two frames.
int differing(const Frame& a, const Frame& b, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Rgb p = a.at(x, y);
            const Rgb q = b.at(x, y);
            if (p.r != q.r || p.g != q.g || p.b != q.b) { ++n; }
        }
    }
    return n;
}

// The leftmost column that carries ink in the rectangle, and the lowest row that does; -1 when none.
int firstInkX(const Frame& f, int x0, int y0, int x1, int y1, Rgb bg) {
    for (int x = x0; x < x1; ++x) { if (f.countInk(x, y0, x + 1, y1, bg) > 0) { return x; } }
    return -1;
}
int lastInkY(const Frame& f, int x0, int y0, int x1, int y1, Rgb bg) {
    for (int y = y1 - 1; y >= y0; --y) { if (f.countInk(x0, y, x1, y + 1, bg) > 0) { return y; } }
    return -1;
}

constexpr int kW = 440, kH = 280;
constexpr int kTx = 10, kTy = 10, kTw = 420;            // Table A
constexpr int kHeadH = 22, kRowH = 24;
constexpr int kBodyTop = kTy + kHeadH;                    // 32
constexpr int kBodyBottom = kTy + 160;                    // 170
constexpr int kTbx = 10, kTby = 180, kTbw = 300;          // Table B, its height 80
constexpr int kColTime = kTx, kColSource = kTx + 78, kColMessage = kTx + 78 + 70, kColLevel = kTx + 78 + 70 + 216;

int rowTop(int row, int scrolled = 0) { return kBodyTop + kRowH * row - scrolled; }

TableRow makeRow(std::initializer_list<const char*> cells, RowTone tone = RowTone::Normal, bool band = false) {
    std::vector<slint::SharedString> text;
    for (const char* c : cells) { text.emplace_back(c); }
    return TableRow{std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(text)), tone, band};
}

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = TableSpecimen::create();
    slint::Window& win = ui->window();

    // The tokens in force, read from the Theme itself: the table is asserted against whatever filling is installed.
    const Rgb bg = tok(ui->global<Theme>().get_bg());
    const Rgb strip = tok(ui->global<Theme>().get_strip());
    const Rgb panel = tok(ui->global<Theme>().get_panel());
    const Rgb card = tok(ui->global<Theme>().get_card());
    const Rgb line = tok(ui->global<Theme>().get_line());
    const Rgb lineStrong = tok(ui->global<Theme>().get_line_strong());
    const Rgb knobRing = tok(ui->global<Theme>().get_knob_ring());
    const Rgb selectedGround = tok(ui->global<Theme>().get_table_selected());
    const Rgb fg = tok(ui->global<Theme>().get_fg());
    const Rgb muted = tok(ui->global<Theme>().get_muted());
    const Rgb dim = tok(ui->global<Theme>().get_dim());
    const Rgb accentSoft = tok(ui->global<Theme>().get_accent_soft());
    const Rgb warnFg = tok(ui->global<Theme>().get_chip_warn_fg());
    const Rgb goodFg = tok(ui->global<Theme>().get_chip_good_fg());
    // The artboard's own values, once, so a drift of the Workbench filling is caught here too.
    CHECK(nearColour(strip, rgb(0x191c21), 0) && nearColour(card, rgb(0x20242b), 0) && nearColour(selectedGround, rgb(0x2a2414), 0));
    CHECK(nearColour(lineStrong, rgb(0x363b45), 0) && nearColour(line, rgb(0x2b2f37), 0) && nearColour(accentSoft, rgb(0xf5c66b), 0));

    const auto away = [&] { moveTo(win, 436.0f, 276.0f); };   // outside both tables: nothing hovered
    const auto shot = [&] { return render(ui, kW, kH); };

    // A wheel notch: a pointer move first (the OS always sends one before a notch; a wheel event on its own is dropped every second
    // time), then the notch, then wait until the body's scroll animation has stopped: two frames in a row alike, after at least 250 ms.
    const auto wheel = [&](float dy) {
        moveTo(win, 200.0f, 100.0f);
        scroll(win, 200.0f, 100.0f, 0.0f, dy);
        const auto start = std::chrono::steady_clock::now();
        Frame prev = shot();
        int still = 0;
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(4)) {
            settle(std::chrono::milliseconds(60));
            Frame cur = shot();
            still = differing(prev, cur, 0, 0, kW, kH) == 0 ? still + 1 : 0;
            prev = std::move(cur);
            if (still >= 3 && std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(250)) { break; }
        }
    };

    // ===== the pristine sheet ============================================================================
    away();
    const Frame f0 = shot();
    CHECK(f0.width == kW && f0.height == kH);
    CHECK(f0.savePng(shotDir() + "table-specimen.png"));

    // --- the heading row: Theme.strip ground, 22 high, its bottom border in Theme.line-strong --------------------
    CHECK(f0.near(kTx + 2, kTy + 2, strip, 0));                                  // top-left of the heading
    CHECK(f0.near(kTx + 410, kTy + 2, strip, 0));
    CHECK(f0.near(kTx + 2, kTy + 20, strip, 0));                                 // last text row above the rule
    CHECK(f0.rowNear(kTy + kHeadH - 1, kTx, kTx + kTw, lineStrong, 0));          // y 31: the rule, full width
    CHECK(f0.near(kTx + 2, kTy + kHeadH - 2, strip, 0));                         // y 30: still heading ground
    CHECK(f0.near(kTx + 2, kTy + kHeadH, panel, 0));                             // y 32: the first body row (the panel behind it)
    CHECK(f0.near(kTx - 1, kTy + 5, bg, 0) && f0.near(kTx + kTw, kTy + 5, bg, 0));  // the window ground just outside the table: 420 wide
    CHECK(f0.near(kTx + 5, kTy - 1, bg, 0));
    CHECK(f0.countNear(kTx + 8, kTy + 3, kTx + 70, kTy + 19, muted, 6) >= 6);                 // TIME in Theme.muted
    CHECK(f0.countNear(kColMessage + 8, kTy + 3, kColMessage + 70, kTy + 19, muted, 6) >= 6); // MESSAGE
    CHECK(f0.countInk(kColSource + 8, kTy + 3, kColSource + 70, kTy + 19, strip) > 15);       // SOURCE
    CHECK(f0.countInk(kColLevel + 8, kTy + 3, kColLevel + 50, kTy + 19, strip) > 15);         // LEVEL
    // 8px of padding before every heading cell, in every column
    for (const int col : {kColTime, kColSource, kColMessage, kColLevel}) {
        CHECK(f0.countInk(col + 1, kTy + 1, col + 7, kTy + 20, strip) == 0);
        CHECK(firstInkX(f0, col + 7, kTy + 3, col + 40, kTy + 19, strip) >= col + 8);
        CHECK(firstInkX(f0, col + 7, kTy + 3, col + 40, kTy + 19, strip) <= col + 9);
    }
    // upper-case (to-uppercase): "Message" would hang a descender below the baseline of "Time" if it were left in mixed case
    const int baselineTime = lastInkY(f0, kColTime + 8, kTy + 3, kColTime + 70, kTy + 20, strip);
    const int baselineMessage = lastInkY(f0, kColMessage + 8, kTy + 3, kColMessage + 80, kTy + 20, strip);
    CHECK(baselineTime > 0 && baselineMessage == baselineTime);

    // --- the row pitch: the hairline is the LAST pixel row of each 24px row ---------------------------------
    int hairlines[8] = {};
    int found = 0;
    for (int y = kBodyTop; y < kBodyBottom && found < 8; ++y) {
        if (f0.near(kTx + 4, y, line, 0)) { hairlines[found++] = y; }
    }
    CHECK(found == 5);                                                         // rows 0..4 end inside the 138px body
    for (int i = 0; i < found; ++i) { CHECK(hairlines[i] == rowTop(i) + kRowH - 1); }
    for (int i = 1; i < found; ++i) { CHECK(hairlines[i] - hairlines[i - 1] == kRowH); }
    CHECK(kRowH == ui->global<Theme>().get_table_row_h());                     // ...and 24 is Theme.table-row-h
    CHECK(f0.rowNear(hairlines[0], kTx + 1, kTx + kTw - 12, line, 0));            // the hairline runs the full row width

    // --- grounds: normal rows show the panel, band rows Theme.card -----------------------------------------
    CHECK(f0.near(kTx + 4, rowTop(0) + 10, panel, 0));       // group row
    CHECK(f0.near(kTx + 4, rowTop(1) + 10, panel, 0));       // normal
    CHECK(f0.near(kTx + 4, rowTop(2) + 10, card, 0));        // warn, band
    CHECK(f0.near(kTx + 300, rowTop(2) + 3, card, 0));
    CHECK(f0.near(kTx + 4, rowTop(3) + 10, panel, 0));       // good
    CHECK(f0.near(kTx + 4, rowTop(4) + 10, card, 0));        // muted, band
    // the band row's ground covers the row above its hairline, and the hairline stays Theme.line over it
    CHECK(f0.near(kTx + 4, rowTop(2) + kRowH - 2, card, 0));
    CHECK(f0.near(kTx + 4, rowTop(2) + kRowH - 1, line, 0));

    // --- tone ink: the row's text takes the tone's colour --------------------------------------------------
    const auto toneInk = [&](const Frame& f, int top, Rgb want) { return f.countNear(kTx + 8, top + 3, kTx + kTw - 12, top + 20, want, 6); };
    CHECK(toneInk(f0, rowTop(1), fg) >= 12);                                    // normal: Theme.fg
    CHECK(toneInk(f0, rowTop(1), warnFg) == 0 && toneInk(f0, rowTop(1), goodFg) == 0);
    CHECK(toneInk(f0, rowTop(2), warnFg) >= 12);                                // warn: Theme.chip-warn-fg
    CHECK(toneInk(f0, rowTop(2), fg) == 0 && toneInk(f0, rowTop(2), goodFg) == 0);
    CHECK(toneInk(f0, rowTop(3), goodFg) >= 12);                                // good: Theme.chip-good-fg
    CHECK(toneInk(f0, rowTop(3), fg) == 0 && toneInk(f0, rowTop(3), warnFg) == 0);
    CHECK(toneInk(f0, rowTop(4), dim) >= 12);                                   // muted: Theme.dim
    CHECK(toneInk(f0, rowTop(4), fg) == 0 && toneInk(f0, rowTop(4), muted) == 0);
    // the columns: the same row carries ink in all four of them, each starting after its 8px of padding
    for (const int col : {kColTime, kColSource, kColMessage, kColLevel}) {
        CHECK(f0.countInk(col + 1, rowTop(1) + 2, col + 7, rowTop(1) + 21, panel) == 0);
        const int bodyStart = firstInkX(f0, col + 7, rowTop(1) + 3, col + 40, rowTop(1) + 20, panel);
        const int headStart = firstInkX(f0, col + 7, kTy + 3, col + 40, kTy + 19, strip);
        CHECK(bodyStart >= col + 8 && bodyStart <= col + 10);
        CHECK(bodyStart - headStart >= -2 && bodyStart - headStart <= 2);       // body and heading columns line up
    }

    // --- the group row: ONE cell spans every column, 10px letter-spaced, in the row's tone -----------------
    CHECK(f0.countNear(kTx + 8, rowTop(0) + 3, kTx + kTw - 12, rowTop(0) + 20, dim, 6) >= 20);
    CHECK(f0.countInk(kColSource + 20, rowTop(0) + 3, kColMessage + 60, rowTop(0) + 20, panel) > 40);   // ink beyond the first column
    CHECK(toneInk(f0, rowTop(0), fg) == 0 && toneInk(f0, rowTop(0), warnFg) == 0);                       // ...and in no other tone's colour
    const int groupStart = firstInkX(f0, kTx + 1, rowTop(0) + 3, kTx + 60, rowTop(0) + 20, panel);
    CHECK(groupStart >= kTx + 8 && groupStart <= kTx + 10);                                              // 8px of padding, like a cell

    // --- the text sits in the middle of its row, whatever the face: clear of the top and of the rule by at least 5px ----------
    const auto centred = [&](const Frame& f, int top, int ruleY, int x0, int x1, Rgb ground) {
        int first = -1, last = -1;
        for (int y = top; y < ruleY; ++y) {
            if (f.countInk(x0, y, x1, y + 1, ground) > 0) { if (first < 0) { first = y; } last = y; }
        }
        return first >= top + 5 && ruleY - last >= 5 && first - top <= 9 && ruleY - last <= 12;
    };
    CHECK(centred(f0, kTy, kTy + kHeadH - 1, kTx + 8, kTx + 400, strip));                                // the heading: y 10..30
    CHECK(centred(f0, rowTop(1), rowTop(1) + kRowH - 1, kTx + 8, kTx + 400, panel));                     // a body row
    CHECK(centred(f0, rowTop(0), rowTop(0) + kRowH - 1, kTx + 8, kTx + 400, panel));                     // the group row

    // --- the scroll bar: a themed 6px thumb over the right edge, present because the rows overflow -----------
    const int thumbX = kTx + kTw - 6;                                           // its centre column (421..426)
    CHECK(f0.countNear(thumbX - 3, kBodyTop, thumbX + 3, kBodyBottom, knobRing, 2) > 100);
    const int thumbTop0 = f0.firstRowNear(thumbX, kBodyTop, kBodyBottom, knobRing, 2);
    CHECK(thumbTop0 >= kBodyTop && thumbTop0 <= kBodyTop + 2);                  // at rest it sits at the top
    const int thumbLen = f0.countNear(thumbX, kBodyTop, thumbX + 1, kBodyBottom, knobRing, 2);
    CHECK(thumbLen >= 84 && thumbLen <= 90);                                    // 138 * 138 / 216 = 88px of the 138px track
    CHECK(f0.countNear(thumbX - 3, kTy, thumbX + 3, kTy + kHeadH, knobRing, 2) == 0);           // never over the heading

    // ===== clicks ==========================================================================================
    CHECK(ui->get_clicks() == 0 && ui->get_right_clicks() == 0 && ui->get_last_row() == -1);
    click(win, 200.0f, rowTop(1) + 12.0f);
    CHECK(ui->get_clicks() == 1 && ui->get_last_row() == 1);
    click(win, 60.0f, rowTop(4) + 12.0f);
    CHECK(ui->get_clicks() == 2 && ui->get_last_row() == 4);
    CHECK(ui->get_right_clicks() == 0);                                         // a left click is not a right click
    click(win, 120.0f, rowTop(2) + 7.0f, slint::PointerEventButton::Right);
    CHECK(ui->get_right_clicks() == 1 && ui->get_last_right_row() == 2);
    CHECK(ui->get_clicks() == 2);                                               // a right click is not a click
    CHECK(ui->get_last_right_x() >= 120 - kTx - 1 && ui->get_last_right_x() <= 120 - kTx + 1);                 // position in the table
    CHECK(ui->get_last_right_y() >= rowTop(2) + 7 - kTy - 1 && ui->get_last_right_y() <= rowTop(2) + 7 - kTy + 1);
    click(win, 380.0f, rowTop(0) + 5.0f);                                       // the group row is a row too
    CHECK(ui->get_clicks() == 3 && ui->get_last_row() == 0);
    click(win, 100.0f, kTy + 10.0f);                                            // a click on the heading is not a row click
    CHECK(ui->get_clicks() == 3);

    // ===== hover: the row under the pointer takes the key tone ========================================
    moveTo(win, 200.0f, rowTop(1) + 12.0f);
    const Frame hov = shot();
    CHECK(hov.near(kTx + 4, rowTop(1) + 10, selectedGround, 0));
    CHECK(hov.near(kTx + 300, rowTop(1) + 3, selectedGround, 0));
    CHECK(toneInk(hov, rowTop(1), accentSoft) >= 12);                           // ink goes to Theme.accent-soft
    CHECK(toneInk(hov, rowTop(1), fg) == 0);
    CHECK(hov.near(kTx + 4, rowTop(1) + kRowH - 1, line, 0));                      // the hairline survives
    CHECK(hov.near(kTx + 4, rowTop(3) + 10, panel, 0));                            // other rows unchanged
    CHECK(differing(f0, hov, kTx, kTy, kTx + kTw - 12, kTy + kHeadH) == 0);     // the heading does not react
    away();
    const Frame rest = shot();
    CHECK(rest.near(kTx + 4, rowTop(1) + 10, panel, 0));                           // pointer gone: the ground returns
    CHECK(toneInk(rest, rowTop(1), fg) >= 12);

    // ===== the selected row and the key tone (both below the fold: scroll three rows) ========================
    wheel(-72.0f);
    away();
    const Frame s1 = shot();
    // rows 3..8 are now at y 32, 56, ...: selected (5) at 80, key tone (6) at 104, band (7) at 128
    CHECK(s1.near(kTx + 4, rowTop(5, 72) + 10, selectedGround, 0));               // the selected row: Theme.table-selected
    CHECK(toneInk(s1, rowTop(5, 72), accentSoft) >= 12);                        // ...in Theme.accent-soft
    CHECK(toneInk(s1, rowTop(5, 72), fg) == 0);
    CHECK(s1.near(kTx + 4, rowTop(6, 72) + 10, selectedGround, 0));               // the key tone draws the same, with no selection
    CHECK(toneInk(s1, rowTop(6, 72), accentSoft) >= 12);
    CHECK(s1.near(kTx + 4, rowTop(7, 72) + 10, card, 0));                          // a band row after them
    CHECK(s1.near(kTx + 4, rowTop(3, 72) + 10, panel, 0));                         // good
    CHECK(toneInk(s1, rowTop(3, 72), goodFg) >= 12);
    CHECK(s1.near(kTx + 4, kBodyBottom - 1, panel, 0) && s1.near(kTx + 4, kBodyBottom, bg, 0));   // the last row is cut by the table's bottom edge

    // --- the frozen heading: the body scrolled by exactly three rows, the heading did not move a pixel -------------
    CHECK(differing(f0, s1, kTx, kTy, kTx + kTw, kTy + kHeadH) == 0);
    CHECK(differing(f0, s1, kTx, kBodyTop, kTx + kTw - 12, kBodyTop + kRowH) > 100);   // the first body row's content changed
    CHECK(s1.near(kTx + 4, kBodyTop + 10, panel, 0));                                      // row 3 (good) now leads
    CHECK(toneInk(s1, kBodyTop, goodFg) >= 12);
    CHECK(toneInk(s1, kBodyTop, dim) == 0);                                             // the group row is gone from the top
    // the pitch is unchanged, three rows further on: hairlines at y 55, 79, ... again (72 is a whole number of rows)
    CHECK(s1.near(kTx + 4, rowTop(0) + kRowH - 1, line, 0));
    CHECK(s1.near(kTx + 4, rowTop(1) + kRowH - 1, line, 0));
    // the thumb moved down with the content: 72 of 78 px scrolled
    const int thumbTop1 = s1.firstRowNear(thumbX, kBodyTop, kBodyBottom, knobRing, 2);
    CHECK(thumbTop1 > thumbTop0 + 30);

    // a click now lands on the row that is under the pointer, not on the row at the same y before the scroll
    click(win, 200.0f, kBodyTop + 12.0f);
    CHECK(ui->get_last_row() == 3);
    click(win, 200.0f, rowTop(6, 72) + 12.0f);
    CHECK(ui->get_last_row() == 6);
    // ...and a right click reports its position in the table, scrolled or not
    click(win, 150.0f, rowTop(5, 72) + 9.0f, slint::PointerEventButton::Right);
    CHECK(ui->get_last_right_row() == 5 && ui->get_right_clicks() == 2);
    CHECK(ui->get_last_right_x() >= 150 - kTx - 1 && ui->get_last_right_x() <= 150 - kTx + 1);
    CHECK(ui->get_last_right_y() >= rowTop(5, 72) + 9 - kTy - 1 && ui->get_last_right_y() <= rowTop(5, 72) + 9 - kTy + 1);
    away();

    // --- the wheel the other way: back to the top, the whole sheet is as it was -------------------------------------
    wheel(960.0f);
    away();
    const Frame back = shot();
    CHECK(differing(f0, back, kTx, kTy, kTx + kTw - 12, kBodyBottom) == 0);
    wheel(-480.0f);                                                                        // past the end: stops at the last row
    away();
    const Frame end = shot();
    CHECK(differing(f0, end, kTx, kTy, kTx + kTw, kTy + kHeadH) == 0);
    CHECK(end.near(kTx + 4, kBodyBottom - 1, line, 0));                                    // the last row's hairline is the table's last pixel row
    CHECK(end.near(kTx + 4, rowTop(8, 78) + 10, panel, 0));                                   // the last row is row 8: normal ground
    CHECK(end.near(thumbX, kBodyBottom - 2, knobRing, 4));                                  // the thumb ends at the foot
    CHECK(end.near(thumbX, kBodyTop + 2, panel, 0) || end.near(thumbX, kBodyTop + 2, card, 0) || end.near(thumbX, kBodyTop + 2, selectedGround, 0));   // ...and left the top
    wheel(960.0f);
    away();

    // ===== the thumb drags the body ========================================================================
    {
        const Frame t0 = shot();
        const int grabY = thumbTop0 + 30;
        press(win, static_cast<float>(thumbX), static_cast<float>(grabY));
        moveTo(win, static_cast<float>(thumbX), static_cast<float>(grabY + 10));
        moveTo(win, static_cast<float>(thumbX), static_cast<float>(grabY + 20));
        release(win, static_cast<float>(thumbX), static_cast<float>(grabY + 20));
        away();
        const Frame t1 = shot();
        const int movedBy = t1.firstRowNear(thumbX, kBodyTop, kBodyBottom, knobRing, 2) - t0.firstRowNear(thumbX, kBodyTop, kBodyBottom, knobRing, 2);
        CHECK(movedBy >= 19 && movedBy <= 21);                                              // the thumb followed the pointer: 20px
        CHECK(differing(t0, t1, kTx, kBodyTop, kTx + kTw - 12, kBodyBottom) > 500);         // and the rows moved with it
        CHECK(differing(t0, t1, kTx, kTy, kTx + kTw, kTy + kHeadH) == 0);
        CHECK(ui->get_clicks() == 5);                                                        // dragging the thumb clicked no row
        wheel(960.0f);
        away();
    }

    // ===== selection is the host's: -1 none, any row ===========================================================
    wheel(-72.0f);
    ui->set_selected(-1);
    away();
    const Frame none = shot();
    CHECK(none.near(kTx + 4, rowTop(5, 72) + 10, panel, 0));                       // no selection: row 5 is an ordinary row again
    CHECK(none.near(kTx + 4, rowTop(6, 72) + 10, selectedGround, 0));              // the key-tone row still draws as key
    ui->set_selected(7);
    const Frame seven = shot();
    CHECK(seven.near(kTx + 4, rowTop(7, 72) + 10, selectedGround, 0));             // ...and a band row, selected, shows the key ground
    CHECK(toneInk(seven, rowTop(7, 72), accentSoft) >= 12);
    ui->set_selected(5);
    wheel(960.0f);
    away();

    // ===== disabled: it never acts ==============================================================================
    ui->set_table_enabled(false);
    click(win, 200.0f, rowTop(1) + 12.0f);
    click(win, 200.0f, rowTop(2) + 12.0f, slint::PointerEventButton::Right);
    CHECK(ui->get_clicks() == 5 && ui->get_right_clicks() == 2);
    moveTo(win, 200.0f, rowTop(1) + 12.0f);
    const Frame off = shot();
    CHECK(off.near(kTx + 4, rowTop(1) + 10, panel, 0));                            // no hover highlight either
    ui->set_table_enabled(true);
    away();

    // ===== the columns: widths given, missing, or too wide ====================================================
    {
        // one width given: Time is 100 wide, the other three share the 320 that are left (106.67 each)
        ui->set_widths(std::make_shared<slint::VectorModel<float>>(std::vector<float>{100.0f}));
        const Frame w1 = shot();
        const double cols1[] = {kTx, kTx + 100.0, kTx + 100.0 + 320.0 / 3, kTx + 100.0 + 640.0 / 3};
        for (const double col : cols1) {
            const int ink = firstInkX(w1, static_cast<int>(col) + 6, kTy + 3, static_cast<int>(col) + 40, kTy + 19, strip);
            CHECK(ink >= col + 7.5 && ink <= col + 10.5);
        }
        // none given: four equal columns of 105
        ui->set_widths(std::make_shared<slint::VectorModel<float>>(std::vector<float>{}));
        const Frame w0 = shot();
        for (const int col : {kTx, kTx + 105, kTx + 210, kTx + 315}) {
            const int ink = firstInkX(w0, col + 6, kTy + 3, col + 40, kTy + 19, strip);
            CHECK(ink >= col + 8 && ink <= col + 10);
            CHECK(w0.countInk(col + 1, kTy + 1, col + 7, kTy + 20, strip) == 0);
        }
        // fixed means fixed, and no column grows to fill the table: four columns of 60 leave 180px empty at the right
        ui->set_widths(std::make_shared<slint::VectorModel<float>>(std::vector<float>{60.0f, 60.0f, 60.0f, 60.0f}));
        const Frame w60 = shot();
        for (const int col : {kTx, kTx + 60, kTx + 120, kTx + 180}) {
            const int ink = firstInkX(w60, col + 6, kTy + 3, col + 40, kTy + 19, strip);
            CHECK(ink >= col + 8 && ink <= col + 9);
        }
        CHECK(w60.countInk(kTx + 252, kTy + 1, kTx + kTw - 12, kTy + 20, strip) == 0);
        CHECK(w60.countInk(kTx + 252, rowTop(1) + 1, kTx + kTw - 12, rowTop(1) + 22, panel) == 0);
        CHECK(w60.rowNear(rowTop(1) + kRowH - 1, kTx + 1, kTx + kTw - 12, line, 0));                // the rows still run the full width
        // the columns together wider than the table: the second starts 395px along, its text is cut at the table's edge
        ui->set_widths(std::make_shared<slint::VectorModel<float>>(std::vector<float>{395.0f, 300.0f, 300.0f, 300.0f}));
        const Frame wide = shot();
        CHECK(wide.countInk(kTx + kTw, kTy, kW, kTy + 160, bg) == 0);                                  // nothing right of the table
        CHECK(wide.countInk(kTx, kTy - 6, kTx + kTw, kTy, bg) == 0 && wide.countInk(kTx, kBodyBottom, kTx + kTw, kBodyBottom + 6, bg) == 0);
        CHECK(wide.countInk(kTx + 8, rowTop(1) + 3, kTx + 90, rowTop(1) + 20, panel) > 40);            // the first column's text is still there...
        CHECK(wide.countInk(kTx + 100, rowTop(1) + 3, kTx + 395, rowTop(1) + 20, panel) == 0);         // ...nothing else in its 395px
        CHECK(wide.countInk(kTx + 395 + 1, kTy + 1, kTx + 395 + 7, kTy + 20, strip) == 0);              // 8px of padding before the second heading
        CHECK(wide.countInk(kTx + 395 + 8, kTy + 3, kTx + kTw, kTy + 19, strip) > 5);                   // and its first letters are inside the table
        ui->set_widths(std::make_shared<slint::VectorModel<float>>(std::vector<float>{78.0f, 70.0f, 0.0f, 56.0f}));
        CHECK(differing(f0, shot(), kTx, kTy, kTx + kTw - 12, kBodyBottom) == 0);                      // and back
    }

    // ===== the rows are the host's: short rows, long rows, none =================================================
    {
        // rows with fewer cells than columns draw the cells they have; the rest of the row stays empty; 3 rows fit: no scroll bar
        ui->set_rows(std::make_shared<slint::VectorModel<TableRow>>(std::vector<TableRow>{
            makeRow({"09:00:00", "A1"}), makeRow({"09:00:01", "B2"}, RowTone::Warn, true), makeRow({"09:00:02", "C3"}, RowTone::Good)}));
        away();
        const Frame r3 = shot();
        CHECK(r3.countInk(kColTime + 8, rowTop(0) + 3, kColSource - 2, rowTop(0) + 20, panel) > 30);
        CHECK(r3.countInk(kColSource + 8, rowTop(0) + 3, kColMessage - 2, rowTop(0) + 20, panel) > 10);
        CHECK(r3.countInk(kColMessage, rowTop(0) + 1, kTx + kTw, rowTop(0) + 22, panel) == 0);          // cells 3 and 4 do not exist
        CHECK(r3.near(kTx + 4, rowTop(1) + 10, card, 0) && r3.countNear(kTx + 8, rowTop(1) + 3, kTx + 200, rowTop(1) + 20, warnFg, 40) >= 8);
        CHECK(r3.near(kTx + 4, rowTop(2) + 10, panel, 0) && r3.countNear(kTx + 8, rowTop(2) + 3, kTx + 200, rowTop(2) + 20, goodFg, 40) >= 8);
        CHECK(r3.near(kTx + 4, rowTop(3) + 10, panel, 0) && r3.rowNear(rowTop(2) + kRowH - 1, kTx + 1, kTx + kTw - 1, line, 0));  // nothing under row 2
        CHECK(r3.countInk(kTx + 1, rowTop(3), kTx + kTw - 1, kBodyBottom, panel) == 0);
        CHECK(r3.countNear(thumbX - 3, kBodyTop, thumbX + 3, kBodyBottom, knobRing, 2) == 0);           // 72 < 138: nothing to scroll, no thumb
        CHECK(r3.countInk(kTx + 8, kBodyTop + kRowH * 3 + 2, kTx + 200, kBodyBottom - 2, panel) == 0);  // and no empty text while there are rows
        // rows with more cells than columns: the extra cells are not drawn anywhere
        ui->set_rows(std::make_shared<slint::VectorModel<TableRow>>(std::vector<TableRow>{
            makeRow({"09:00:00", "A1", "message", "INFO", "five", "six"})}));
        const Frame r6 = shot();
        CHECK(r6.countInk(kColLevel + 8, rowTop(0) + 3, kColLevel + 50, rowTop(0) + 20, panel) > 15);
        CHECK(r6.countInk(kTx + kTw, kTy, kW, kTy + 160, bg) == 0);
        // none at all: the empty text under the heading, no hairline, no scroll bar
        ui->set_rows(std::make_shared<slint::VectorModel<TableRow>>(std::vector<TableRow>{}));
        const Frame r0 = shot();
        CHECK(r0.rowNear(kTy + kHeadH - 1, kTx, kTx + kTw, lineStrong, 0));
        CHECK(r0.countNear(kTx + 12, kTy + kHeadH + 6, kTx + 200, kTy + kHeadH + 22, dim, 6) >= 10);
        CHECK(r0.countNear(kTx + 1, kTy + kHeadH + 22, kTx + kTw - 1, kBodyBottom, line, 2) == 0);
        CHECK(r0.countNear(thumbX - 3, kBodyTop, thumbX + 3, kBodyBottom, knobRing, 2) == 0);
        // and the original rows bring the original sheet back, pixel for pixel
        ui->set_rows(std::make_shared<slint::VectorModel<TableRow>>(std::vector<TableRow>{
            makeRow({"TODAY - MONDAY 2 NOVEMBER 2026"}, RowTone::Muted),
            makeRow({"14:32:08", "M0ABC", "Position report 51.50N 000.07W", "INFO"}),
            makeRow({"14:32:41", "G4XYZ", "Bad checksum, frame dropped", "WARN"}, RowTone::Warn, true),
            makeRow({"14:33:02", "M0ABC", "Message acknowledged", "GOOD"}, RowTone::Good),
            makeRow({"14:33:40", "ISS", "Digipeated, 2 hops", "MUTED"}, RowTone::Muted, true),
            makeRow({"14:34:11", "2E0FOX", "Whole row selectable (hovered)", "INFO"}),
            makeRow({"14:34:52", "G0KEY", "A row in the key tone", "INFO"}, RowTone::Key),
            makeRow({"14:35:30", "M0ABC", "Position report 51.51N 000.08W", "INFO"}, RowTone::Normal, true),
            makeRow({"14:36:02", "G4XYZ", "Position report 51.52N 000.09W", "INFO"})}));
        CHECK(differing(f0, shot(), kTx, kTy, kTx + kTw, kBodyBottom) == 0);
    }

    // ===== the empty table ======================================================================================
    {
        // heading as above, then the empty text in Theme.dim under it, and nothing else
        CHECK(f0.near(kTbx + 2, kTby + 2, strip, 0));
        CHECK(f0.rowNear(kTby + kHeadH - 1, kTbx, kTbx + kTbw, lineStrong, 0));
        CHECK(f0.near(kTbx + 2, kTby + kHeadH, panel, 0));
        CHECK(f0.countNear(kTbx + 12, kTby + kHeadH + 6, kTbx + 200, kTby + kHeadH + 22, dim, 6) >= 10);   // "Nothing to show yet."
        CHECK(f0.countNear(kTbx + 12, kTby + kHeadH + 6, kTbx + 200, kTby + kHeadH + 22, fg, 6) == 0);     // ...in Theme.dim, not Theme.fg
        CHECK(f0.countInk(kTbx + 12, kTby + kHeadH + 6, kTbx + 200, kTby + kHeadH + 22, panel) > 40);
        CHECK(f0.countInk(kTbx + 12, kTby + kHeadH + 6, kTbx + 200, kTby + kHeadH + 22, panel) < 700);     // a line of text, not a block
        CHECK(f0.countNear(kTbx + 1, kTby + kHeadH + 22, kTbx + kTbw - 1, kTby + 80, line, 2) == 0);       // no row hairlines (the first would be on y 225)
        CHECK(f0.countInk(kTbx + 1, kTby + kHeadH + 26, kTbx + kTbw - 1, kTby + 79, panel) == 0);           // nothing below the text
        CHECK(f0.countNear(kTbx + kTbw - 10, kTby + kHeadH, kTbx + kTbw, kTby + 80, knobRing, 2) == 0);     // no scroll bar: nothing to scroll
        CHECK(f0.near(kTbx + kTbw + 4, kTby + 40, bg, 0));                                                     // the table is 300 wide
        // the text starts 12px in: padding 6px 12px of the sheet's `.n`
        const int textX = firstInkX(f0, kTbx + 1, kTby + kHeadH + 6, kTbx + 100, kTby + kHeadH + 22, panel);
        CHECK(textX >= kTbx + 12 && textX <= kTbx + 14);
    }

    // ===== the tooltip: a rested pointer shows it below the table ===================================================
    {
        // the tip sits 6px under Table A (y 176): the menu ground in a knob-ring border, over Table B's heading
        const auto tipShown = [&] { return shot().countNear(kTx + 8, kTy + 160 + 6, kTx + 100, kTy + 160 + 7, knobRing, 2) > 20; };
        moveTo(win, 200.0f, rowTop(1) + 12.0f);
        bool shown = false;
        for (int i = 0; i < 50 && !shown; ++i) { settle(std::chrono::milliseconds(100)); shown = tipShown(); }
        CHECK(shown);                                                                                       // after about 600 ms of rest
        away();
        CHECK(!tipShown());                                                                                 // and it closes when the pointer leaves
    }

    return testSummary("test_slint_table");
}
