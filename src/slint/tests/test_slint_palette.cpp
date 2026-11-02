// The command palette: the title bar's field (520 x 26: panel ground, line border, the search icon, the placeholder, the
// "Ctrl K" badge) and the sheet it opens - a 640 px panel with a knob-ring border, a 26 px query row on the field ground and
// 32 px result rows, the highlighted one on menu-hot - driven with the keyboard the way the operator drives it: typing tells the
// host, Up / Down move the highlight (and wrap), Enter activates it, Esc dismisses, the pointer highlights and a click activates.
// Text is in a fallback typeface on this machine: it is asserted as INK in a region, never as glyph shapes or widths.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "palette_specimen.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace foxsdr::slinttest;
using std::chrono::milliseconds;
namespace keys = slint::platform::key_codes;

namespace {

constexpr int kW = 700, kH = 300;

// Theme.* (the Workbench filling)
const Rgb kBg = rgb(0x15171b);
const Rgb kPanel = rgb(0x1d2026);
const Rgb kLine = rgb(0x2b2f37);
const Rgb kLineStrong = rgb(0x363b45);
const Rgb kKnobRing = rgb(0x434955);
const Rgb kField = rgb(0x0f1114);
const Rgb kMenuHot = rgb(0x363b45);
const Rgb kChipBg = rgb(0x262a32);
const Rgb kMenu = rgb(0x262a32);
const Rgb kFg = rgb(0xe6e8ec);
const Rgb kDim = rgb(0x7d8694);

// The field: (20, 20), 520 x 26. The sheet: its panel at (30, 60), 640 wide; query row (39, 69, 622, 26); rows from y 101.
constexpr int kFx = 20, kFy = 20, kFw = 520, kFh = 26;
constexpr int kPx = 30, kPy = 60, kPw = 640;
constexpr int kRowsY = 101;

PaletteResult result(const char* kind, const char* title, const char* detail, int id) {
    PaletteResult r;
    r.kind = kind;
    r.title = title;
    r.detail = detail;
    r.id = id;
    return r;
}

template <class Ui>
void setRows(Ui& ui, std::vector<PaletteResult> rows) {
    ui->set_rows(std::make_shared<slint::VectorModel<PaletteResult>>(std::move(rows)));
}

// Row i of a sheet that opens at y 60 is highlighted when its first and last pixels are menu-hot and the neighbours are not.
bool rowIsHot(const Frame& f, int row, int top = kRowsY) {
    const int y = top + 32 * row;
    return f.near(300, y + 2, kMenuHot) && f.near(300, y + 29, kMenuHot);
}
bool rowIsPlain(const Frame& f, int row, int top = kRowsY) {
    const int y = top + 32 * row;
    return f.near(300, y + 2, kPanel) && f.near(300, y + 29, kPanel);
}
// the sheet is open when its border is there, at row y
bool sheetOpen(const Frame& f, int y = kPy + 40) { return f.near(kPx, y, kKnobRing) && f.near(kPx + kPw - 1, y, kKnobRing); }

// The brightest blue channel in a rectangle: the ink's own colour, which its anti-aliased edges never exceed.
int brightestBlue(const Frame& f, int x0, int y0, int x1, int y1) {
    int best = 0;
    for (int y = y0; y < y1; ++y) { for (int x = x0; x < x1; ++x) { best = std::max(best, int(f.at(x, y).b)); } }
    return best;
}

// The first column in [x0, x1) with ink (a pixel unlike `bg`) in rows [y0, y1); -1 when none.
int firstInkX(const Frame& f, int x0, int x1, int y0, int y1, Rgb bg) {
    for (int x = x0; x < x1; ++x) {
        if (f.countInk(x, y0, x + 1, y1, bg, 40) > 0) { return x; }
    }
    return -1;
}

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = PaletteSpecimen::create();
    slint::Window& win = ui->window();

    // =============================== the field, at rest ===============================
    Frame f = render(ui, kW, kH);
    CHECK(f.width == kW && f.height == kH);
    CHECK(f.rowNear(kFy, kFx + 8, kFx + kFw - 8, kLine));                          // the 1 px line border: top...
    CHECK(f.rowNear(kFy + kFh - 1, kFx + 8, kFx + kFw - 8, kLine));                // ...bottom
    CHECK(f.colNear(kFx, kFy + 8, kFy + kFh - 8, kLine));                          // ...left
    CHECK(f.colNear(kFx + kFw - 1, kFy + 8, kFy + kFh - 8, kLine));                // ...right
    CHECK(f.near(kFx - 1, kFy + 13, kBg) && f.near(kFx + kFw, kFy + 13, kBg));     // exactly 520 wide
    CHECK(f.near(300, kFy - 1, kBg) && f.near(300, kFy + kFh, kBg));               // exactly 26 high
    CHECK(f.near(kFx + 1, kFy + 1, kBg, 10) && f.near(kFx + kFw - 2, kFy + kFh - 2, kBg, 10));   // rounded corners (radius 6)
    CHECK(f.near(kFx + 5, kFy + 13, kPanel) && f.near(300, kFy + 4, kPanel) && f.near(kFx + kFw - 5, kFy + 13, kPanel));   // the panel ground
    CHECK(f.countInk(kFx + 11, kFy + 6, kFx + 25, kFy + 20, kPanel) > 15);         // the search icon, 14 px at x 11
    CHECK(f.countNear(kFx + 11, kFy + 6, kFx + 25, kFy + 20, kDim, 24) > 5);       // in the dim ink...
    CHECK(brightestBlue(f, kFx + 11, kFy + 6, kFx + 25, kFy + 20) > 0x70 && brightestBlue(f, kFx + 11, kFy + 6, kFx + 25, kFy + 20) <= 0x94 + 8);   // ...and no brighter (Theme.dim is 0x7d8694)
    CHECK(f.countInk(kFx + 33, kFy + 4, kFx + 400, kFy + 22, kPanel) > 80);        // the placeholder
    CHECK(f.countNear(kFx + 33, kFy + 4, kFx + 400, kFy + 22, kDim, 24) > 10);     // in the dim ink
    CHECK(brightestBlue(f, kFx + 33, kFy + 4, kFx + 400, kFy + 22) > 0x60 && brightestBlue(f, kFx + 33, kFy + 4, kFx + 400, kFy + 22) <= 0x94 + 8);
    CHECK(f.countInk(kFx + 400, kFy + 4, kFx + 458, kFy + 22, kPanel) == 0);       // ...and it stops well short of the badge
    CHECK(f.countNear(kFx + 450, kFy + 2, kFx + kFw - 2, kFy + kFh - 2, kLineStrong, 4) > 50);     // the badge's 1 px line-strong box
    CHECK(f.countNear(kFx + 460, kFy + 6, kFx + kFw - 12, kFy + kFh - 6, kFg, 40) > 8);            // "Ctrl K" in the sheet's bright ink
    CHECK(f.near(kFx + kFw - 11, kFy + 13, kPanel) && f.near(kFx + kFw - 10, kFy + 13, kPanel));   // the badge keeps 10 px from the line
    CHECK(ui->get_field_clicks() == 0);

    // click: the field says so wherever it is pressed; a click beside it says nothing
    click(win, 100, 32);
    CHECK(ui->get_field_clicks() == 1);
    click(win, kFx + kFw - 25, 32);                                                // on the badge
    CHECK(ui->get_field_clicks() == 2);
    click(win, kFx + kFw + 20, 32);
    click(win, 100, 14);
    CHECK(ui->get_field_clicks() == 2);
    // disabled: no click
    ui->set_field_enabled(false);
    click(win, 100, 32);
    CHECK(ui->get_field_clicks() == 2);
    ui->set_field_enabled(true);
    // the tooltip: under the field after the pointer has rested, gone when it leaves
    ui->set_field_tooltip("Open the command palette");
    moveTo(win, 200, 32);
    settle(milliseconds(750));
    Frame tip = render(ui, kW, kH);
    CHECK(tip.countNear(kFx + 2, kFy + kFh + 6, kFx + 150, kFy + kFh + 18, kMenu, 4) > 300);
    moveTo(win, 650, 150);
    settle(milliseconds(60));
    Frame tipGone = render(ui, kW, kH);
    CHECK(tipGone.countNear(kFx + 2, kFy + kFh + 6, kFx + 150, kFy + kFh + 18, kMenu, 4) == 0);
    ui->set_field_tooltip("");

    // =============================== the sheet, open ===============================
    CHECK(!sheetOpen(f));
    CHECK(f.near(kPx + 5, 140, kBg) && f.near(300, 150, kBg));                     // nothing there until it is opened
    ui->invoke_open_palette();
    Frame o = render(ui, kW, kH);
    CHECK(sheetOpen(o));
    // the panel: 640 px wide, a 1 px knob-ring border, the panel ground inside, radius-panel corners
    CHECK(o.rowNear(kPy, kPx + 12, kPx + kPw - 12, kKnobRing));                    // top border
    CHECK(o.colNear(kPx, kPy + 12, kPy + 178 - 12, kKnobRing));                    // left border
    CHECK(o.colNear(kPx + kPw - 1, kPy + 12, kPy + 178 - 12, kKnobRing));          // right border
    CHECK(o.rowNear(kPy + 177, kPx + 12, kPx + kPw - 12, kKnobRing));              // bottom border: 4 rows make it 178 high
    CHECK(o.near(kPx - 1, 150, kBg) && o.near(kPx + kPw, 150, kBg));               // exactly 640 wide
    CHECK(o.near(300, kPy - 1, kBg) && o.near(300, kPy + 178, kBg));
    CHECK(o.near(kPx + 4, 150, kPanel) && o.near(kPx + kPw - 5, 150, kPanel) && o.near(300, kPy + 4, kPanel));
    CHECK(o.near(kPx, kPy, kBg, 10) && o.near(kPx + 1, kPy + 1, kBg, 10));        // the corners are cut round
    // the query row: 622 x 26 on Theme.field, 9 px in
    CHECK(o.near(300, 69, kField) && o.near(300, 94, kField) && o.near(300, 68, kPanel) && o.near(300, 95, kPanel));
    CHECK(o.near(39, 82, kField) && o.near(38, 82, kPanel) && o.near(660, 82, kField) && o.near(661, 82, kPanel));
    CHECK(o.countInk(49, 71, 400, 93, kField, 30) > 60);                           // the placeholder (the query is empty)
    // four rows, 32 px apart, the first highlighted on menu-hot
    CHECK(rowIsHot(o, 0) && rowIsPlain(o, 1) && rowIsPlain(o, 2) && rowIsPlain(o, 3));
    CHECK(o.near(300, kRowsY - 1, kPanel) && o.near(300, kRowsY, kMenuHot));       // the list starts at y 101 (41 under the panel's top)
    CHECK(o.near(300, kRowsY + 31, kMenuHot) && o.near(300, kRowsY + 32, kPanel)); // a row is 32 px
    CHECK(o.near(39, kRowsY + 16, kMenuHot) && o.near(38, kRowsY + 16, kPanel) && o.near(660, kRowsY + 16, kMenuHot) && o.near(661, kRowsY + 16, kPanel));
    CHECK(o.near(300, kRowsY + 4 * 32 + 2, kPanel));                               // nothing under the fourth
    for (int row = 0; row < 4; ++row) {
        const int top = kRowsY + 32 * row;
        CHECK(o.countNear(49, top + 6, 49 + 70, top + 26, kChipBg, 4) > 150);      // the kind chip's ground, 20 px high, in the first column
        CHECK(o.countNear(55, top + 8, 100, top + 24, rgb(0xc9ced6), 40) > 8);     // and its word, in the chip's ink
        CHECK(o.countNear(155, top + 6, 400, top + 26, kFg, 40) > 12);             // the title in the bright ink
        CHECK(o.countNear(520, top + 6, 655, top + 26, kDim, 30) > 5);             // the detail, dim, at the right
    }
    // the titles line up in one column whatever the width of the kind chip before them (SETTING .. FREQUENCY)
    {
        const int x0 = firstInkX(o, 140, 400, kRowsY + 6, kRowsY + 26, kMenuHot);
        CHECK(x0 > 100);
        for (int row = 1; row < 4; ++row) {
            const int x = firstInkX(o, 140, 400, kRowsY + 32 * row + 6, kRowsY + 32 * row + 26, kPanel);
            CHECK(x > 0 && std::abs(x - x0) <= 4);
        }
    }
    o.savePng(shotDir() + "palette-specimen.png");

    // ---- typing: the host is told, the placeholder gives way, the highlight goes back to the top ----
    CHECK(ui->get_query_changes() == 0);
    keyTap(win, u8"a");
    CHECK(ui->get_query_changes() == 1 && std::string(ui->get_last_query()) == "a" && std::string(ui->get_query()) == "a");
    Frame typed = render(ui, kW, kH);
    CHECK(typed.countInk(80, 71, 400, 93, kField, 30) == 0);                       // no placeholder any more
    CHECK(typed.countInk(47, 72, 62, 92, kField, 30) > 5);                         // "a" and the caret at the left of the row
    CHECK(sheetOpen(typed));
    keyTap(win, u8"b");
    CHECK(ui->get_query_changes() == 2 && std::string(ui->get_last_query()) == "ab");
    keyTap(win, keys::Backspace);
    CHECK(ui->get_query_changes() == 3 && std::string(ui->get_last_query()) == "a");

    // ---- Down moves the highlight to the second row, Up back; both wrap round the list ----
    CHECK(ui->get_highlight() == 0);
    keyTap(win, keys::DownArrow);
    CHECK(ui->get_highlight() == 1);
    Frame down = render(ui, kW, kH);
    CHECK(rowIsHot(down, 1) && rowIsPlain(down, 0) && rowIsPlain(down, 2) && rowIsPlain(down, 3));
    CHECK(down.near(300, kRowsY + 32, kMenuHot) && down.near(300, kRowsY + 31, kPanel));    // the highlight moved a whole row
    keyTap(win, keys::UpArrow);
    CHECK(ui->get_highlight() == 0);
    keyTap(win, keys::UpArrow);                                                    // above the first: the last
    CHECK(ui->get_highlight() == 3);
    Frame wrapUp = render(ui, kW, kH);
    CHECK(rowIsHot(wrapUp, 3) && rowIsPlain(wrapUp, 0));
    keyTap(win, keys::DownArrow);                                                  // below the last: the first
    CHECK(ui->get_highlight() == 0);
    // typing takes the highlight back to the top
    keyTap(win, keys::DownArrow);
    keyTap(win, keys::DownArrow);
    CHECK(ui->get_highlight() == 2);
    keyTap(win, u8"c");
    CHECK(ui->get_highlight() == 0 && std::string(ui->get_last_query()) == "ac");
    keyTap(win, keys::Backspace);
    CHECK(std::string(ui->get_last_query()) == "a");

    // ---- Enter activates the highlighted row's id and closes the sheet ----
    keyTap(win, keys::DownArrow);
    CHECK(ui->get_highlight() == 1);
    CHECK(ui->get_activations() == 0);
    keyTap(win, keys::Return);
    CHECK(ui->get_activations() == 1 && ui->get_last_id() == 202);                 // the second result's id
    CHECK(ui->get_dismissals() == 0);
    Frame closed = render(ui, kW, kH);
    CHECK(!sheetOpen(closed) && closed.near(300, 150, kBg) && closed.near(300, 82, kBg));
    CHECK(closed.rowNear(kFy, kFx + 8, kFx + kFw - 8, kLine));                     // the field is still there

    // ---- open again: the highlight starts at the top; Esc dismisses ----
    ui->invoke_open_palette();
    Frame again = render(ui, kW, kH);
    CHECK(sheetOpen(again) && ui->get_highlight() == 0 && rowIsHot(again, 0));
    CHECK(again.countInk(47, 72, 62, 92, kField, 30) > 5);                         // the query ("a") is the host's: still there
    ui->set_query("");                                                             // ...and the host clears it when it wants an empty sheet
    Frame empty = render(ui, kW, kH);
    CHECK(empty.countInk(49, 71, 400, 93, kField, 30) > 60);
    keyTap(win, keys::Escape);
    CHECK(ui->get_dismissals() == 1 && ui->get_activations() == 1);
    Frame esc = render(ui, kW, kH);
    CHECK(!sheetOpen(esc) && esc.near(300, 150, kBg));

    // ---- the pointer: moving over a row highlights it, a click activates it ----
    ui->invoke_open_palette();
    render(ui, kW, kH);
    moveTo(win, 300, kRowsY + 2 * 32 + 16);
    CHECK(ui->get_highlight() == 2);
    Frame hover = render(ui, kW, kH);
    CHECK(rowIsHot(hover, 2) && rowIsPlain(hover, 0));
    moveTo(win, 300, kRowsY + 3 * 32 + 16);
    CHECK(ui->get_highlight() == 3);
    moveTo(win, 300, kRowsY + 2 * 32 + 16);
    click(win, 300, kRowsY + 2 * 32 + 16);
    CHECK(ui->get_activations() == 2 && ui->get_last_id() == 303);                 // the third result
    CHECK(!sheetOpen(render(ui, kW, kH)));
    CHECK(ui->get_dismissals() == 1);

    // ---- a click outside the sheet closes it, silently (a popup has no "closed" callback); a click on the sheet's blank part does not ----
    ui->invoke_open_palette();
    render(ui, kW, kH);
    click(win, 34, 150);                                                           // the sheet's own padding
    CHECK(sheetOpen(render(ui, kW, kH)));
    click(win, 300, kPy + 4);                                                      // its top padding
    CHECK(sheetOpen(render(ui, kW, kH)));
    click(win, 680, 280);                                                          // outside it
    CHECK(!sheetOpen(render(ui, kW, kH)));
    CHECK(ui->get_dismissals() == 1 && ui->get_activations() == 2);
    ui->invoke_open_palette();
    render(ui, kW, kH);
    click(win, 20, 150);                                                           // in the margin the popup keeps for its shadow
    CHECK(!sheetOpen(render(ui, kW, kH)));
    CHECK(ui->get_activations() == 2);

    // the public close() closes it too
    ui->invoke_open_palette();
    CHECK(sheetOpen(render(ui, kW, kH)));
    ui->invoke_close_palette();
    CHECK(!sheetOpen(render(ui, kW, kH)));

    // ---- the host answers a keystroke with fewer rows than the highlight was on: it stays on the list ----
    ui->invoke_open_palette();
    render(ui, kW, kH);
    for (int i = 0; i < 3; ++i) { keyTap(win, keys::DownArrow); }
    CHECK(ui->get_highlight() == 3);
    setRows(ui, {result("SETTING", "Tuner gain", "Signal", 101), result("PLUGIN", "ADS-B aircraft", "Decode", 202)});
    Frame fewer = render(ui, kW, kH);
    CHECK(ui->get_highlight() == 1 && rowIsHot(fewer, 1) && rowIsPlain(fewer, 0));
    CHECK(fewer.near(300, kPy + 41 + 64 + 9 - 1, kKnobRing) && fewer.near(300, kPy + 41 + 64 + 9, kBg));   // 41 + 2 * 32 + 9 = 114 high
    setRows(ui, {});
    Frame emptied = render(ui, kW, kH);
    CHECK(ui->get_highlight() == 0 && emptied.near(300, kPy + 43, kKnobRing));
    ui->invoke_close_palette();
    setRows(ui, {result("SETTING", "Tuner gain", "Signal", 101), result("PLUGIN", "ADS-B aircraft", "Decode", 202),
                 result("FREQUENCY", "145.500 MHz", "NFM", 303), result("BOOKMARK", "Local repeater", "145.725", 404)});

    // ---- more results than fit: `max-rows` rows show, the highlight keeps itself in view as it moves and wraps ----
    {
        std::vector<PaletteResult> many;
        for (int i = 0; i < 12; ++i) { many.push_back(result("PLUGIN", ("Result " + std::to_string(i + 1)).c_str(), "detail", 1000 + i)); }
        setRows(ui, many);
    }
    ui->set_max_rows(3);
    ui->invoke_open_palette();
    Frame list = render(ui, kW, kH);
    CHECK(sheetOpen(list) && rowIsHot(list, 0) && rowIsPlain(list, 1) && rowIsPlain(list, 2));
    CHECK(list.near(300, kRowsY + 3 * 32 + 1, kPanel));                            // the list ends after the third row: no 4th
    CHECK(list.near(300, kPy + 146 - 1, kKnobRing) && list.near(300, kPy + 146, kBg));      // 41 + 3 * 32 + 9 = 146 high
    for (int i = 0; i < 3; ++i) { keyTap(win, keys::DownArrow); }
    CHECK(ui->get_highlight() == 3);
    Frame scrolled = render(ui, kW, kH);
    CHECK(rowIsHot(scrolled, 2) && rowIsPlain(scrolled, 0) && rowIsPlain(scrolled, 1));    // the 4th is in the last slot: the list moved
    for (int i = 0; i < 3; ++i) { keyTap(win, keys::UpArrow); }
    CHECK(ui->get_highlight() == 0);
    Frame back = render(ui, kW, kH);
    CHECK(rowIsHot(back, 0) && rowIsPlain(back, 2));
    keyTap(win, keys::UpArrow);                                                    // wraps to the last of the 12
    CHECK(ui->get_highlight() == 11);
    Frame last = render(ui, kW, kH);
    CHECK(rowIsHot(last, 2) && rowIsPlain(last, 0));
    keyTap(win, keys::Return);
    CHECK(ui->get_last_id() == 1011 && ui->get_activations() == 3);
    ui->set_max_rows(8);

    // ---- no results: the sheet is the query row alone, or one row with the host's words ----
    setRows(ui, {});
    ui->invoke_open_palette();
    Frame none = render(ui, kW, kH);
    CHECK(sheetOpen(none, kPy + 20) && none.near(300, kPy + 43, kKnobRing) && none.near(300, kPy + 44, kBg) && none.near(300, kPy + 42, kPanel));   // 44 px high
    keyTap(win, keys::Return);                                                     // nothing to activate: nothing happens
    keyTap(win, keys::DownArrow);
    keyTap(win, keys::UpArrow);
    CHECK(ui->get_activations() == 3);
    CHECK(sheetOpen(render(ui, kW, kH), kPy + 20));
    ui->invoke_close_palette();
    ui->set_empty_text("No matches");
    ui->invoke_open_palette();
    Frame words = render(ui, kW, kH);
    CHECK(words.near(300, kPy + 81, kKnobRing) && words.near(300, kPy + 82, kBg));                               // 82 px high
    CHECK(words.countInk(49, kPy + 43, 300, kPy + 73, kPanel, 30) > 25);
    CHECK(words.countNear(49, kPy + 43, 300, kPy + 73, kDim, 30) > 5);
    ui->invoke_close_palette();
    ui->set_empty_text("");
    setRows(ui, {result("SETTING", "Tuner gain", "Signal", 101), result("PLUGIN", "ADS-B aircraft", "Decode", 202),
                 result("FREQUENCY", "145.500 MHz", "NFM", 303), result("BOOKMARK", "Local repeater", "145.725", 404)});

    return testSummary("test_slint_palette");
}
