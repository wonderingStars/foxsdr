// The FORMS family of the plugin kit (docs 6.37): PluginSettingsForm (the fold line and its fields, two to a row or one), GrantKeys,
// ReceiverControlCard, StateChips (and the StateChip they are made of) and RunKeys, rendered headlessly from
// ui/specimens/forms_specimen.slint and probed against the PluginKit sheet ("Torn-off plugin window": the settings fold and its form;
// "Sections-panel form": the fold line, STOP / START, the mute checkbox and the Receiver control card; "Module page (tab) excerpt": the
// grant keys; "State chips and row examples": the chips) and the contract in COMPONENTS.md ("The plugin kit").
//
// The typefaces are the embedded Inter and JetBrains Mono, but text checks still assert INK in a region and its COLOUR, never glyph
// positions: boxes, borders, gaps and the pitch of the rows are drawn geometry and are asserted to the pixel. Typing, clicks and keys
// go through the harness's dispatched events, so committed / toggled / grant-toggled ... are the real callbacks. A list of runs is read
// through spanAt(), which answers an impossible span when the list is short, so a missing chip fails every check made on it.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "forms_specimen.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
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

// A run of non-ground pixels along a row: [l, r).
struct Span {
    int l = 0, r = 0;
    int w() const { return r - l; }
    int mid() const { return (l + r) / 2; }
};
Span spanAt(const std::vector<Span>& v, std::size_t i) { return i < v.size() ? v[i] : Span{-1000, -990}; }
std::vector<Span> runsOnRow(const Frame& f, int y, int x0, int x1, Rgb outside, int tol = 4) {
    std::vector<Span> out;
    bool in = false;
    int start = 0;
    for (int x = x0; x < x1; ++x) {
        const bool ground = f.near(x, y, outside, tol);
        if (!ground && !in) { in = true; start = x; }
        if (ground && in) { in = false; out.push_back({start, x}); }
    }
    if (in) { out.push_back({start, x1}); }
    return out;
}
int lastNearOnRow(const Frame& f, int y, int x0, int x1, Rgb c, int tol = 3) {
    for (int x = x1 - 1; x >= x0; --x) { if (f.near(x, y, c, tol)) { return x; } }
    return -1;
}

// The bounding box and count of the ink (pixels differing from `bg`) in a rectangle.
struct Ink {
    int n = 0, x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    int w() const { return x1 - x0 + 1; }
    int h() const { return y1 - y0 + 1; }
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

SettingField sf(int id, const char* key, const char* label, SettingKind kind, const char* value, bool checked, const char* placeholder,
                int maxLength, const char* error, bool pending) {
    return SettingField{id, key, label, kind, value, checked, placeholder, maxLength, error, pending};
}

// ---- the specimen's table (forms_specimen.slint) ----
constexpr int kW = 1160, kH = 840;
constexpr int kLx = 10, kFormW = 470, kRailW = 280;
constexpr int kF1y = 24, kF2y = 60, kF3y = 470;
constexpr int kFoldH = 24, kBodyTop = 4, kFieldH = 52, kRowGap = 8, kInputDy = 28, kIn = 24;
constexpr int kPad = 12, kColGap = 10, kHalf = 218, kFull = 446, kRailCell = 256;
constexpr int kGx = 500, kG1y = 24, kG2y = 330;
constexpr int kRx = 510, kRy = 372, kCardW = 296;
constexpr int kSx = 500, kS1y = 600, kS2y = 640, kChipH = 20, kChipGap = 6;
constexpr int kK1x = 500, kK2x = 820, kK1y = 710, kK3y = 770, kRunW = 280, kKeyH = 30;

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = FormsSpecimen::create();
    slint::Window& win = ui->window();

    // The tokens, read from the Workbench skin by name (the theme test pins their values).
    const Skin wb = ui->global<Workbench>().get_value();
    const Rgb bg = tok(wb.bg), panel = tok(wb.panel), panelAlt = tok(wb.panel_alt), field = tok(wb.field), lineStrong = tok(wb.line_strong);
    const Rgb lampRed = tok(wb.lamp_red), fg = tok(wb.fg), fgSoft = tok(wb.fg_soft), muted = tok(wb.muted), dim = tok(wb.dim);
    const Rgb accent = tok(wb.accent), accentInk = tok(wb.accent_ink), accentSoft = tok(wb.accent_soft), errFg = tok(wb.chip_err_fg);
    const Rgb keyBg = tok(wb.key_bg), keyLine = tok(wb.key_line), keyFg = tok(wb.key_fg), plateLine = tok(wb.plate_line), plateFg = tok(wb.plate_fg);
    const Rgb cardWarn = tok(wb.card_warn), cardWarnLine = tok(wb.card_warn_line);
    const Rgb dangerBg = tok(wb.danger_bg), dangerLine = tok(wb.danger_line), dangerFg = tok(wb.danger_fg);
    const Rgb keyDisBg = tok(wb.key_disabled_bg), keyDisFg = tok(wb.key_disabled_fg), menu = tok(wb.menu), knobRing = tok(wb.knob_ring), focus = tok(wb.focus);
    const Rgb chipBg = tok(wb.chip_bg), chipLine = tok(wb.chip_line), chipFg = tok(wb.chip_fg);
    const Rgb goodBg = tok(wb.chip_good_bg), goodLine = tok(wb.chip_good_line), goodFg = tok(wb.chip_good_fg);
    const Rgb warnBg = tok(wb.chip_warn_bg), warnFg = tok(wb.chip_warn_fg), errBg = tok(wb.chip_err_bg), errLine = tok(wb.chip_err_line);
    const Rgb lampGreen = tok(wb.lamp_green), lampAmber = tok(wb.lamp_amber);
    // the artboard's own values, once, so a drift of the Workbench filling is caught here too
    CHECK(nearColour(cardWarn, rgb(0x2a2312), 0) && nearColour(cardWarnLine, rgb(0x6b4d16), 0) && nearColour(accentSoft, rgb(0xf5c66b), 0));
    CHECK(nearColour(goodBg, rgb(0x163626), 0) && nearColour(goodFg, rgb(0x7fe6ad), 0) && nearColour(errFg, rgb(0xff9b9b), 0));
    CHECK(nearColour(dangerBg, rgb(0x3a1d1d), 0) && nearColour(dangerLine, rgb(0x6b2a2a), 0) && nearColour(lineStrong, rgb(0x363b45), 0));

    const auto clk = [&](int x, int y) { click(win, static_cast<float>(x), static_cast<float>(y)); };
    const auto mv = [&](int x, int y) { moveTo(win, static_cast<float>(x), static_cast<float>(y)); };
    const auto tap = [&](std::u8string_view k) { keyTap(win, k); };
    const auto reset = [&] {
        ui->set_commits(0); ui->set_last_id(-1); ui->set_last_text(""); ui->set_last_form(""); ui->set_toggles(0); ui->set_last_checked(false);
        ui->set_browses(0); ui->set_grant_events(0); ui->set_last_grant(""); ui->set_last_grant_value(false);
        ui->set_card_toggles(0); ui->set_last_card(-1); ui->set_last_card_value(false);
        ui->set_chip_toggles(0); ui->set_last_chip(-1); ui->set_last_chip_source("");
        ui->set_run_count(0); ui->set_last_run(""); ui->set_mute_count(0); ui->set_last_mute_source(""); ui->set_last_mute(false);
    };
    // Parks the pointer on the sheet's empty corner: closes any tooltip and ends any hover.
    const auto park = [&] { mv(1155, 835); settle(milliseconds(30)); };
    const auto sval = [&](const slint::SharedString& s) { return std::string(s); };

    // =========================================================================================================
    // 1. The sheet, before anything is touched
    // =========================================================================================================
    Frame f0 = render(ui, kW, kH);
    CHECK(f0.width == kW && f0.height == kH);
    CHECK(f0.near(1155, 835, bg));
    f0.savePng(shotDir() + "forms-specimen.png");

    // =========================================================================================================
    // 2. F1, the closed settings form: the fold line alone - a chevron, the title (12 px / 600, fg), 6 + 16 + 2 = 24 px
    // =========================================================================================================
    {
        CHECK_NEAR(ui->get_f1_height(), 24.0, 0.01);
        CHECK(!ui->get_f1_open());
        CHECK(f0.near(kLx + 4, kF1y + 3, panel) && f0.near(kLx + 4, kF1y + 23, panel) && f0.near(kLx + kFormW - 4, kF1y + 12, panel));
        CHECK(f0.rowNear(kF1y + kFoldH, kLx, kLx + kFormW, bg));                       // nothing below the fold line: 24 px exactly
        CHECK(f0.countInk(kLx, kF1y + kFoldH, kLx + kFormW, kF1y + kFoldH + 10, bg) == 0);
        // the chevron: a 14 px icon at the 12 px padding, in the muted ink; the title 6 px after it
        const Ink chev = inkBox(f0, kLx, kF1y, kLx + kPad + 14 + 2, kF1y + kFoldH, panel);
        CHECKV(chev.n >= 6 && chev.x0 >= kLx + kPad + 2 && chev.x1 <= kLx + kPad + 14, "chevron ink n=%d x %d..%d", chev.n, chev.x0, chev.x1);
        CHECK(f0.countNear(kLx + kPad, kF1y + 4, kLx + kPad + 14, kF1y + 22, muted, 40) >= 4);
        const Ink title = inkBox(f0, kLx + kPad + 14 + 3, kF1y + 3, kLx + 300, kF1y + kFoldH - 1, panel);
        CHECKV(title.n > 60 && title.x0 >= kLx + kPad + 14 + 5 && title.x0 <= kLx + kPad + 14 + 10, "title ink n=%d starts at x %d (chevron box ends %d)", title.n, title.x0, kLx + kPad + 14);
        CHECKV(f0.countNear(kLx + kPad + 14 + 3, kF1y + 3, kLx + 300, kF1y + kFoldH - 1, fg, 40) >= 25, "title in fg: %d", f0.countNear(kLx + kPad + 14 + 3, kF1y + 3, kLx + 300, kF1y + kFoldH - 1, fg, 40));
        CHECKV(std::abs(title.cy() - (kF1y + 6 + 8)) <= 3.5, "title centre %.1f vs the line's %d", title.cy(), kF1y + 14);
        CHECK(f0.countInk(kLx + 300, kF1y + 2, kLx + kFormW, kF1y + kFoldH - 1, panel) == 0);     // nothing at the right of the title
    }

    // =========================================================================================================
    // 3. F2, the open form, wide: the fold line (chevron down), then fields two to a row with 10 px between the columns
    // =========================================================================================================
    const int r1Top = kF2y + kFoldH + kBodyTop;                 // the first row of fields
    const int in1 = r1Top + kInputDy;                           // row 1's inputs
    const int in2 = r1Top + kFieldH + kRowGap + kInputDy;       // row 2's inputs
    int pathIn = -1, pwIn = -1, chanIn = -1;                // the tops of the inputs of rows 3, 4 and 5
    {
        const double h = ui->get_f2_height();
        CHECKV(h >= 372 && h <= 388, "F2 is %.1f high (fold 24 + 4 + 52 + 8 + 70 + 8 + 52 + 8 + 52 + 8 + 84 + 8)", h);
        CHECK(ui->get_f2_open());
        // the chevron points down: the picture differs from F1's right-pointing one, and it is in the same place
        CHECKV(differingShifted(f0, f0, kLx + kPad, kF1y + 4, kLx + kPad + 14, kF1y + 22, kF2y - kF1y) >= 8, "F1's and F2's chevrons are the same picture");
        CHECK(f0.countNear(kLx + kPad, kF2y + 4, kLx + kPad + 14, kF2y + 22, muted, 40) >= 4);
        // row 1: text | number, columns 218 wide at x 22 and 250 (12 px padding, 10 px between), inputs 24 high
        for (const int x : {kLx + kPad, kLx + kPad + kHalf + kColGap}) {
            CHECKV(f0.rowNear(in1, x + 8, x + kHalf - 8, lineStrong) && f0.rowNear(in1 + kIn - 1, x + 8, x + kHalf - 8, lineStrong), "column at %d: input borders (%s)", x, hex(f0.at(x + 20, in1)).c_str());
            CHECK(f0.near(x, in1 + 12, lineStrong) && f0.near(x + kHalf - 1, in1 + 12, lineStrong));
            CHECK(f0.near(x + 4, in1 + 12, field));
            CHECK(f0.near(x - 1, in1 + 12, panel) && f0.near(x + kHalf, in1 + 12, panel));
            CHECK(f0.near(x + 20, in1 - 1, panel) && f0.near(x + 20, in1 + kIn, panel));                // 4 px gap above, ground below
        }
        CHECK(f0.rowNear(in1 + 12, kLx + kPad + kHalf, kLx + kPad + kHalf + kColGap, panel));          // the 10 px between the columns
        // the label row: the label at the left in fg-soft, the kind as a cap at the right in muted
        CHECK(f0.countNear(kLx + kPad, r1Top, kLx + kPad + 120, r1Top + 24, fgSoft, 40) >= 12);
        const int capR = kLx + kPad + kHalf;
        const Ink cap = inkBox(f0, capR - 70, r1Top, capR, r1Top + 24, panel);
        CHECKV(cap.n > 15 && cap.x1 >= capR - 4, "TEXT cap ink n=%d right edge %d (cell ends %d)", cap.n, cap.x1, capR);
        CHECK(f0.countNear(capR - 70, r1Top, capR, r1Top + 24, muted, 40) >= 8);
        // the placeholder of the empty text field (e.g. M0ABC) and the figures of the number field, right-aligned
        CHECK(f0.countInk(kLx + kPad + 9, in1 + 3, kLx + kPad + kHalf - 9, in1 + kIn - 3, field) >= 20);
        const Ink num = inkBox(f0, kLx + kPad + kHalf + kColGap + 7, in1 + 2, kLx + kPad + kHalf + kColGap + kHalf - 7, in1 + kIn - 2, field);
        CHECKV(num.n > 8 && (kLx + kPad + kHalf + kColGap + kHalf - 1 - num.x1) < 16, "number ink n=%d ends %d px inside the right edge", num.n, (kLx + kPad + kHalf + kColGap + kHalf - 1 - num.x1));
        // row 2: the number field with its error (red border, the sentence under it in chip-err-fg) | the BOOL field
        CHECK(f0.rowNear(in2, kLx + kPad + 8, kLx + kPad + kHalf - 8, lampRed) && f0.rowNear(in2 + kIn - 1, kLx + kPad + 8, kLx + kPad + kHalf - 8, lampRed));
        CHECK(f0.near(kLx + kPad, in2 + 12, lampRed) && f0.near(kLx + kPad + kHalf - 1, in2 + 12, lampRed));
        CHECKV(f0.countNear(kLx + kPad, in2 + kIn + 2, kLx + kPad + kHalf, in2 + kIn + 24, errFg, 40) >= 25, "error sentence in chip-err-fg: %d", f0.countNear(kLx + kPad, in2 + kIn + 2, kLx + kPad + kHalf, in2 + kIn + 24, errFg, 40));
        const int bx = kLx + kPad + kHalf + kColGap, by = in2 + (kIn - 14) / 2;                       // the checkbox of the BOOL field
        CHECK(f0.near(bx + 3, by + 3, accent) && f0.near(bx, by + 7, accent));
        CHECK(f0.countNear(bx + 1, by + 1, bx + 13, by + 13, accentInk, 40) >= 6);
        CHECK(f0.countNear(bx + 22, in2 + 2, bx + 70, in2 + kIn - 2, fg, 90) >= 4);                      // the word "on"
        // row 3: the path field takes the whole row, its BROWSE FILES key at the right edge; its input starts one row-2 field + 8 lower
        pathIn = f0.firstRowNear(kLx + kPad + 20, in2 + 30, in2 + 120, lineStrong);
        CHECKV(pathIn >= in2 + 74 && pathIn <= in2 + 82, "path input top at %d (row 2's inputs at %d)", pathIn, in2);
        CHECK(f0.near(kLx + kPad, pathIn + 12, lineStrong));
        CHECKV(f0.near(kLx + kPad + kFull - 1, pathIn + 12, keyLine), "BROWSE FILES key's right border at %d: %s", kLx + kPad + kFull - 1, hex(f0.at(kLx + kPad + kFull - 1, pathIn + 12)).c_str());
        const int browseL = [&] { int x = kLx + kPad + kFull - 2; while (x > 0 && f0.near(x, pathIn + 6, keyBg, 3)) { --x; } return x; }();
        CHECKV(kLx + kPad + kFull - browseL >= 70 && kLx + kPad + kFull - browseL <= 150, "BROWSE FILES key is %d wide", kLx + kPad + kFull - browseL);
        CHECK(f0.near(browseL + 8, pathIn, keyLine) && f0.near(browseL + 8, pathIn + kIn - 1, keyLine));    // a small key: 24 high
        CHECK(f0.countInk(kLx + kPad + 9, pathIn + 3, browseL - 14, pathIn + kIn - 3, field) >= 30);        // the path text
        // row 4: the password field, full row too; its dots and the SHOW key
        pwIn = pathIn + kFieldH + kRowGap;
        CHECK(f0.near(kLx + kPad, pwIn + 12, lineStrong));
        CHECK(f0.near(kLx + kPad + kFull - 1, pwIn + 12, keyLine));
        CHECK(f0.countNear(kLx + kPad + 9, pwIn + 3, kLx + kPad + 200, pwIn + kIn - 3, fg, 40) >= 20);   // the dots
        // row 5: Channel | Station with a two-line error under it, the second column's border red
        chanIn = pwIn + kFieldH + kRowGap;
        CHECK(f0.rowNear(chanIn, kLx + kPad + 8, kLx + kPad + kHalf - 8, lineStrong));
        const int sx = kLx + kPad + kHalf + kColGap;
        CHECK(f0.rowNear(chanIn, sx + 8, sx + kHalf - 8, lampRed));
        const Ink err2 = inkBox(f0, sx, chanIn + kIn + 2, sx + kHalf, chanIn + kIn + 60, panel);
        CHECKV(err2.n > 60 && err2.h() >= 22 && err2.h() <= 32, "the Station error wraps to two lines: %d rows of ink", err2.h());
        CHECK(f0.countNear(sx, chanIn + kIn + 2, sx + kHalf, chanIn + kIn + 60, errFg, 40) >= 40);
        CHECKV(err2.x1 < sx + kHalf, "error ink reaches x %d (cell ends %d)", err2.x1, sx + kHalf);
        // the form's own height holds the wrapped error and 8 px of padding under it
        const double formBottom = kF2y + h;
        CHECKV(formBottom - err2.y1 >= 8 && formBottom - err2.y1 <= 16, "wrapped error ends %.1f px above the form's bottom", formBottom - err2.y1);
        CHECK(f0.countInk(kLx, static_cast<int>(formBottom) + 1, kLx + kFormW, static_cast<int>(formBottom) + 8, panel) == 0);   // the ground below is the specimen's
    }

    // =========================================================================================================
    // 4. F3, the rail's width: one field to a row, 256 px cells, rows 60 px apart
    // =========================================================================================================
    const int f3Row = kF3y + kFoldH + kBodyTop;               // 498
    {
        CHECK_NEAR(ui->get_f3_height(), 268.0, 0.6);
        CHECK(ui->get_f3_open());
        for (const int i : {0, 1, 3}) {                                                    // the text, number and path rows have an input box; the BOOL row a checkbox
            const int in = f3Row + i * (kFieldH + kRowGap) + kInputDy;
            const int x = kLx + kPad;
            CHECKV(f0.rowNear(in, x + 8, x + 100, lineStrong) && f0.rowNear(in + kIn - 1, x + 8, x + 100, lineStrong), "rail input %d borders at row %d (%s)", i, in, hex(f0.at(x + 20, in)).c_str());
            CHECK(f0.near(x, in + 12, lineStrong) && f0.near(x - 1, in + 12, panelAlt));
            CHECK(f0.near(x + 4, in + 12, field));
        }
        CHECK(f0.near(kLx + kPad + kRailCell - 1, f3Row + kInputDy + 12, lineStrong));               // the first row's input runs to the 12 px padding at the right
        CHECK(f0.countInk(kLx + kPad + kRailCell, f3Row, kLx + 296, kF3y + 268, panelAlt) == 0);      // nothing in the right half: one to a row
        // the third (BOOL) row: the checked box, the word "on"
        const int in3 = f3Row + 2 * (kFieldH + kRowGap) + kInputDy, by = in3 + (kIn - 14) / 2;
        CHECK(f0.near(kLx + kPad + 3, by + 3, accent) && f0.near(kLx + kPad, by + 7, accent));
        // the fourth (PATH) row: the key at the right edge of the cell
        const int in4 = f3Row + 3 * (kFieldH + kRowGap) + kInputDy;
        CHECKV(f0.near(kLx + kPad + kRailCell - 1, in4 + 12, keyLine), "rail path key's right border: %s", hex(f0.at(kLx + kPad + kRailCell - 1, in4 + 12)).c_str());
        CHECK(f0.near(kLx + kPad, in4 + 12, lineStrong));
    }

    // =========================================================================================================
    // 5. G1, the grant keys: GRANT is a plate key, REVOKE a lit one; the notes under the keys that have them; the transmit state line
    // =========================================================================================================
    int g1Transmit = -1;
    {
        const double h = ui->get_g1_height();
        const int gh = static_cast<int>(h);
        CHECKV(h >= 195 && h <= 230, "G1 is %.1f high", h);
        // key 1: GRANT RECEIVER CONTROL - plate: transparent inside, a plate-line border, plate-fg label, 24 high at the top
        CHECK(f0.near(kGx, kG1y + 12, plateLine));
        CHECK(f0.near(kGx + 8, kG1y, plateLine) && f0.near(kGx + 8, kG1y + 23, plateLine));
        CHECK(f0.near(kGx + 8, kG1y - 1, bg));                                                          // the ground behind the keys starts with them
        CHECK(f0.near(kGx + 4, kG1y + 12, panelAlt));
        const int r1 = lastNearOnRow(f0, kG1y + 12, kGx + 20, kGx + 480, plateLine);
        CHECKV(r1 - kGx + 1 >= 120 && r1 - kGx + 1 <= 230, "the first key is %d wide (it hugs its label)", r1 - kGx + 1);
        CHECK(f0.countNear(kGx + 8, kG1y + 3, r1 - 6, kG1y + 21, plateFg, 40) >= 25);
        CHECK(f0.countInk(r1 + 2, kG1y, kGx + 480, kG1y + 24, panelAlt) == 0);                          // hugging: nothing beyond it on the line
        // key 2: REVOKE RADIO SETTINGS - lit: the accent ground and border, accent-ink label; 6 px under the first
        const int y2 = kG1y + 24 + 6;
        CHECK(f0.near(kGx, y2 + 12, accent) && f0.near(kGx + 4, y2 + 12, accent) && f0.near(kGx + 8, y2 + 2, accent));
        CHECK(f0.near(kGx + 8, y2 - 1, panelAlt) && f0.near(kGx + 8, y2 + 24, panelAlt));
        CHECK(f0.countNear(kGx + 8, y2 + 3, kGx + 120, y2 + 21, accentInk, 40) >= 25);
        // key 3: GRANT TRANSMIT - plate again; found by its top border below the settings note
        const int noteTop = y2 + 24 + 6;
        g1Transmit = f0.firstRowNear(kGx + 8, noteTop + 20, noteTop + 100, plateLine);
        CHECKV(g1Transmit >= noteTop + 24 && g1Transmit <= noteTop + 60, "GRANT TRANSMIT's top border at row %d (the settings note starts at %d)", g1Transmit, noteTop);
        CHECK(f0.near(kGx, g1Transmit + 12, plateLine) && f0.near(kGx + 8, g1Transmit + 23, plateLine));
        CHECK(f0.near(kGx + 4, g1Transmit + 12, panelAlt));
        // the settings note: 11 px dim, wrapped at the 480 px width, starting 6 px under the key, 6 px above the next
        const Ink note = inkBox(f0, kGx, noteTop - 2, kGx + 480, g1Transmit - 2, panelAlt);
        CHECKV(note.n > 400 && note.y0 >= noteTop && note.y0 <= noteTop + 4, "settings note ink n=%d from row %d (the line box starts at %d)", note.n, note.y0, noteTop);
        CHECKV(note.h() >= 24 && note.h() <= 54, "the settings note is %d rows tall (2 or 3 lines)", note.h());
        CHECKV(g1Transmit - 2 - note.y1 >= 3 && g1Transmit - note.y1 <= 14, "the note ends %d px above GRANT TRANSMIT", g1Transmit - note.y1);
        CHECK(f0.countNear(kGx, noteTop, kGx + 480, g1Transmit - 2, dim, 40) >= 80);
        // the transmit note, then the transmit state line in accent-soft (the module is waiting for the arming)
        const int tn = g1Transmit + 24 + 6;
        const Ink tnote = inkBox(f0, kGx, tn - 2, kGx + 480, kG1y + gh + 4, panelAlt);
        CHECKV(tnote.n > 700 && tnote.y0 >= tn && tnote.y0 <= tn + 4, "transmit note + state ink n=%d from row %d (box starts at %d)", tnote.n, tnote.y0, tn);
        CHECKV(f0.countNear(kGx, tn, kGx + 480, kG1y + gh - 18, dim, 40) >= 150, "transmit note in dim");
        CHECKV(f0.countNear(kGx, kG1y + gh - 18, kGx + 480, kG1y + gh + 1, accentSoft, 40) >= 40, "the state line (the last line) in accent-soft: %d", f0.countNear(kGx, kG1y + gh - 18, kGx + 480, kG1y + gh + 1, accentSoft, 40));
        CHECKV(tnote.y1 <= kG1y + gh + 1 && tnote.y1 >= kG1y + gh - 6, "ink ends at row %d, the component's bottom is %d", tnote.y1, kG1y + gh);
        // G2: the receiver control key alone, lit: 24 px, nothing under it
        CHECK_NEAR(ui->get_g2_height(), 24.0, 0.01);
        CHECK(f0.near(kGx, kG2y + 12, accent) && f0.near(kGx + 4, kG2y + 12, accent));
        CHECK(f0.near(kGx + 8, kG2y + 24, bg));
    }

    // =========================================================================================================
    // 6. R1, the Receiver control card: the amber card, the refusal, one checkbox per stale grant with its sentence; R2 draws nothing
    // =========================================================================================================
    std::vector<int> cardBoxes;                                  // the tops of the two accent-filled 14 px boxes of R1
    {
        const double h = ui->get_r1_height();
        const int ih = static_cast<int>(std::lround(h));
        CHECKV(h >= 150 && h <= 215, "R1 is %.1f high", h);
        // the card: card-warn ground, a 1 px card-warn-line border, 8 px corners
        CHECK(f0.near(kRx + 4, kRy + 40, cardWarn) && f0.near(kRx + kCardW - 5, kRy + 40, cardWarn));
        CHECK(f0.rowNear(kRy, kRx + 12, kRx + kCardW - 12, cardWarnLine));
        const int bottom = [&] { int y = kRy + ih + 4; while (y > kRy && !f0.near(kRx + 100, y, cardWarnLine, 3)) { --y; } return y; }();
        CHECKV(std::abs(bottom - (kRy + h - 1)) <= 1.0, "the card's bottom border is on row %d, its height says %.1f", bottom, kRy + h - 1);
        CHECK(f0.colNear(kRx, kRy + 12, bottom - 11, cardWarnLine) && f0.colNear(kRx + kCardW - 1, kRy + 12, bottom - 11, cardWarnLine));
        CHECKV(!f0.near(kRx, kRy, cardWarnLine, 10), "the corner pixel %s is cut by the 8 px radius", hex(f0.at(kRx, kRy)).c_str());
        CHECK(f0.near(kRx - 1, kRy + 40, panelAlt) && f0.near(kRx + 40, bottom + 2, panelAlt));
        // the title (accent-soft, 12 px / 600), 9 px inside the card; the refusal sentence under it in accent-soft too, two lines
        const Ink title = inkBox(f0, kRx + 8, kRy + 8, kRx + kCardW - 8, kRy + 9 + 16, cardWarn);
        CHECKV(title.n > 50 && title.x0 >= kRx + 9 && title.x0 <= kRx + 12 && title.y0 >= kRy + 9, "title ink n=%d from (%d,%d)", title.n, title.x0, title.y0);
        CHECKV(f0.countNear(kRx + 8, kRy + 8, kRx + kCardW - 8, kRy + 9 + 16, accentSoft, 40) >= 25, "title in accent-soft");
        int firstRow = -1, lastRow = -1;
        for (int y = kRy + 9 + 17; y < kRy + 9 + 17 + 36; ++y) {
            if (f0.countNear(kRx + 8, y, kRx + kCardW - 8, y + 1, accentSoft, 30) > 0) { if (firstRow < 0) { firstRow = y; } lastRow = y; }
        }
        CHECKV(firstRow >= kRy + 9 + 17 && lastRow - firstRow >= 20, "refusal sentence: accent-soft ink on rows %d..%d (two lines)", firstRow, lastRow);
        CHECK(f0.countNear(kRx + 8, kRy + 9 + 17, kRx + kCardW - 8, kRy + 9 + 17 + 36, accentSoft, 30) >= 80);
        // the two checkboxes: found as the accent-filled 14 px boxes down the column the boxes sit in
        for (int y = kRy + 9 + 40; y < bottom - 8; ++y) {
            if (f0.near(kRx + 9 + 3, y, accent, 6) && f0.near(kRx + 9 + 3, y - 1, cardWarn, 6)) { cardBoxes.push_back(y); }       // an accent row under the card's ground: a box's top
        }
        CHECKV(cardBoxes.size() == 2, "found %zu accent boxes in the column", cardBoxes.size());
        for (const int y : cardBoxes) {
            CHECKV(f0.near(kRx + 9, y + 7, accent) && f0.near(kRx + 9 + 13, y + 7, accent) && f0.near(kRx + 9 + 7, y + 13, accent) && f0.near(kRx + 9 + 7, y, accent), "box at row %d is 14 px", y);
            CHECK(!f0.near(kRx + 9 + 7, y + 14, accent, 10) && f0.near(kRx + 9 + 7, y - 1, cardWarn));
            CHECK(f0.countNear(kRx + 9 + 1, y + 1, kRx + 9 + 13, y + 13, accentInk, 40) >= 6);          // the tick
        }
        // the file name beside the first box (fg, 11 px); the sentence under it, indented to the name, in dim
        const int y0 = cardBoxes.size() > 0 ? cardBoxes[0] : -1000, y1 = cardBoxes.size() > 1 ? cardBoxes[1] : -1000;
        const Ink name = inkBox(f0, kRx + 9 + 14 + 2, y0 - 2, kRx + kCardW - 9, y0 + 16, cardWarn);
        CHECKV(name.n > 40 && name.x0 >= kRx + 9 + 14 + 6 && name.x0 <= kRx + 9 + 14 + 10, "file name ink n=%d starts %d px after the box", name.n, name.x0 - (kRx + 9 + 14));
        CHECK(f0.countNear(kRx + 9 + 14 + 2, y0 - 2, kRx + kCardW - 9, y0 + 16, fg, 40) >= 15);
        const Ink sentence = inkBox(f0, kRx + 9, y0 + 16, kRx + kCardW - 9, y1 - 2, cardWarn);
        CHECKV(sentence.n > 150 && sentence.x0 >= kRx + 9 + 14 + 6 && sentence.h() >= 24, "sentence ink n=%d, %d rows, from x %d", sentence.n, sentence.h(), sentence.x0);
        CHECK(f0.countNear(kRx + 9, y0 + 16, kRx + kCardW - 9, y1 - 2, dim, 40) >= 60);
        CHECKV(sentence.x1 < kRx + kCardW - 9, "sentence ink reaches x %d (card interior ends %d)", sentence.x1, kRx + kCardW - 9);
        // R2: nothing to say, nothing drawn, no height
        CHECK_NEAR(ui->get_r2_height(), 0.0, 0.01);
        CHECKV(f0.countInk(840, 372, 1136, 420, panelAlt) == 0, "R2 drew %d pixels", f0.countInk(840, 372, 1136, 420, panelAlt));
    }

    // =========================================================================================================
    // 7. S1 and S2, the state chips: pills 20 px high 6 px apart (a lamp, the word and the count), on in their tone, off neutral at half alpha
    // =========================================================================================================
    const int s1Mid = kS1y + kChipH / 2;
    const std::vector<Span> chips1 = runsOnRow(f0, s1Mid, kSx, kSx + 640, bg);
    {
        const double natural = ui->get_s1_natural();
        CHECKV(natural >= 480 && natural <= 640, "the chips' natural width is %.1f", natural);
        CHECK_NEAR(ui->get_s1_height(), 20.0, 0.01);
        CHECK_NEAR(ui->get_s2_height(), 46.0, 0.01);
        CHECKV(chips1.size() == 6, "%zu chips on the first row (wanted 6)", chips1.size());
        CHECKV(spanAt(chips1, 0).l == kSx, "first chip starts at %d", spanAt(chips1, 0).l);
        for (std::size_t i = 1; i < 6; ++i) { CHECKV(spanAt(chips1, i).l - spanAt(chips1, i - 1).r == kChipGap, "gap before chip %zu is %d", i, spanAt(chips1, i).l - spanAt(chips1, i - 1).r); }
        CHECKV(spanAt(chips1, 5).r <= kSx + 640 && spanAt(chips1, 5).r - kSx <= natural + 1.5 && spanAt(chips1, 5).r - kSx >= natural - 1.5, "the six chips end at %d, the natural width says %.1f", spanAt(chips1, 5).r - kSx, natural);
        struct Look { const char* name; Rgb ground, edge, text, lamp; bool lampLit; };
        const Rgb halfGround = over(chipBg, 0.5, bg), halfEdge = over(chipLine, 0.5, bg);
        const Look looks[6] = {
            {"FED 3", goodBg, goodLine, goodFg, lampGreen, true},
            {"IDLE 2", chipBg, chipLine, chipFg, tok(wb.lamp_off), false},
            {"NOT DECODING 1", warnBg, tok(wb.chip_warn_line), warnFg, lampAmber, true},
            {"TAKES NO SIGNAL 0", halfGround, halfEdge, over(chipFg, 0.5, halfGround), tok(wb.lamp_off), false},
            {"STOPPED 1", chipBg, chipLine, chipFg, tok(wb.lamp_off), false},
            {"REFUSED 1", errBg, errLine, errFg, lampRed, true},
        };
        const Rgb lampOff = tok(wb.lamp_off), lampRing = tok(wb.lamp_ring);
        for (std::size_t i = 0; i < 6; ++i) {
            const Span s = spanAt(chips1, i);
            const Look& k = looks[i];
            CHECKV(s.w() >= 50 && s.w() <= 175, "chip %s is %d wide", k.name, s.w());
            CHECKV(f0.near(s.l, s1Mid, k.edge, 14) && f0.near(s.r - 1, s1Mid, k.edge, 14), "%s ends %s / %s, wanted about %s", k.name, hex(f0.at(s.l, s1Mid)).c_str(), hex(f0.at(s.r - 1, s1Mid)).c_str(), hex(k.edge).c_str());
            CHECKV(f0.near(s.mid(), kS1y, k.edge, 5) && f0.near(s.mid(), kS1y + kChipH - 1, k.edge, 5), "%s top/bottom borders: 20 px high (%s, %s)", k.name, hex(f0.at(s.mid(), kS1y)).c_str(), hex(f0.at(s.mid(), kS1y + kChipH - 1)).c_str());
            CHECK(f0.near(s.mid(), kS1y - 1, bg) && f0.near(s.mid(), kS1y + kChipH, bg));
            CHECKV(f0.near(s.l + 3, s1Mid, k.ground, 5), "%s ground %s, wanted %s", k.name, hex(f0.at(s.l + 3, s1Mid)).c_str(), hex(k.ground).c_str());
            CHECK(f0.near(s.l, kS1y, bg));                                                         // a pill: the corner pixel is cut away
            // the lamp: an 8 px dot 8 px in from the edge, centred on the chip - lit in its colour, off a dark dot in a ring
            if (k.lampLit) {
                CHECKV(f0.near(s.l + 12, s1Mid, k.lamp, 8), "%s lamp %s, wanted %s", k.name, hex(f0.at(s.l + 12, s1Mid)).c_str(), hex(k.lamp).c_str());
                CHECK(f0.countInk(s.l + 8, kS1y + 5, s.l + 16, kS1y + 15, k.ground) >= 20);
            } else {
                CHECKV(f0.near(s.l + 12, s1Mid, lampOff, 10) && f0.near(s.l + 8, s1Mid, lampRing, 10), "%s lamp is off: centre %s, edge %s", k.name, hex(f0.at(s.l + 12, s1Mid)).c_str(), hex(f0.at(s.l + 8, s1Mid)).c_str());
            }
            // the words and the count, in the tone's ink (half alpha for the off chip)
            CHECKV(f0.countInk(s.l + 22, kS1y + 3, s.r - 6, kS1y + 17, k.ground) >= 25, "%s label ink", k.name);
            const int want = i == 3 ? 3 : 8;
            CHECKV(f0.countNear(s.l + 22, kS1y + 3, s.r - 6, kS1y + 17, k.text, i == 3 ? 24 : 40) >= want, "%s text colour %s: %d", k.name, hex(k.text).c_str(), f0.countNear(s.l + 22, kS1y + 3, s.r - 6, kS1y + 17, k.text, i == 3 ? 24 : 40));
            // the text sits 6 px after the dot and ends 8 px before the edge
            const Ink word = inkBox(f0, s.l + 17, kS1y + 5, s.r - 3, kS1y + 15, k.ground, 30);
            CHECKV(word.x0 >= s.l + 8 + 8 + 6 && word.x0 <= s.l + 8 + 8 + 6 + 3 && s.r - 1 - word.x1 >= 8 && s.r - 1 - word.x1 <= 12, "%s text spans x %d..%d in a chip %d..%d", k.name, word.x0, word.x1, s.l, s.r - 1);
        }
        // S2: 400 wide, so two rows of three, the second 26 px under the first
        const int s2Mid1 = kS2y + kChipH / 2, s2Mid2 = kS2y + kChipH + kChipGap + kChipH / 2;
        const std::vector<Span> a = runsOnRow(f0, s2Mid1, kSx, kSx + 400, bg), b = runsOnRow(f0, s2Mid2, kSx, kSx + 400, bg);
        CHECKV(a.size() == 3 && b.size() == 3, "S2 rows hold %zu and %zu chips", a.size(), b.size());
        CHECK(spanAt(a, 0).l == kSx && spanAt(b, 0).l == kSx);
        CHECKV(f0.near(spanAt(a, 0).mid(), kS2y, goodLine, 5) && f0.near(spanAt(b, 0).mid(), kS2y + kChipH + kChipGap, halfEdge, 5), "row tops: %s / %s", hex(f0.at(spanAt(a, 0).mid(), kS2y)).c_str(), hex(f0.at(spanAt(b, 0).mid(), kS2y + kChipH + kChipGap)).c_str());
        CHECK(f0.rowNear(kS2y + kChipH + 2, kSx, kSx + 400, bg));                                  // the 6 px between the rows
        CHECK(f0.near(spanAt(b, 2).l + 3, s2Mid2, errBg, 5));                                       // the sixth chip is the REFUSED one, last of row two
    }

    // =========================================================================================================
    // 8. K1..K4, the run keys: STOP (danger) / START, then the mute checkbox 6 px under; disabled wears the kit's disabled look
    // =========================================================================================================
    {
        CHECK_NEAR(ui->get_k1_height(), 50.0, 0.01);
        CHECK_NEAR(ui->get_k3_height(), 30.0, 0.01);
        // K1: STOP - the wide danger key, 30 px, edge to edge of the 280 px
        CHECKV(f0.near(kK1x, kK1y + 15, dangerLine) && f0.near(kK1x + kRunW - 1, kK1y + 15, dangerLine), "STOP borders %s / %s", hex(f0.at(kK1x, kK1y + 15)).c_str(), hex(f0.at(kK1x + kRunW - 1, kK1y + 15)).c_str());
        CHECK(f0.near(kK1x + 8, kK1y, dangerLine) && f0.near(kK1x + 8, kK1y + kKeyH - 1, dangerLine));
        CHECK(f0.near(kK1x + 8, kK1y - 1, bg) && f0.near(kK1x + 8, kK1y + kKeyH, bg));
        CHECK(f0.near(kK1x + 4, kK1y + 15, dangerBg));
        CHECKV(f0.countNear(kK1x + 100, kK1y + 4, kK1x + 180, kK1y + kKeyH - 4, dangerFg, 40) >= 15, "STOP label in danger-fg");
        // the mute checkbox under it: 14 px box at the left, checked (accent), the words 8 px after it
        const int cy = kK1y + kKeyH + 6;
        CHECKV(f0.near(kK1x + 3, cy + 3, accent) && f0.near(kK1x, cy + 7, accent) && f0.near(kK1x + 13, cy + 7, accent), "mute box %s", hex(f0.at(kK1x + 3, cy + 3)).c_str());
        CHECK(!f0.near(kK1x + 3, cy + 14, accent, 10) && !f0.near(kK1x + 3, cy - 1, accent, 10));
        const Ink words = inkBox(f0, kK1x + 14 + 2, cy - 2, kK1x + kRunW, cy + 16, bg);
        CHECKV(words.n > 80 && words.x0 >= kK1x + 14 + 6 && words.x0 <= kK1x + 14 + 10, "mute words ink n=%d, %d px after the box", words.n, words.x0 - (kK1x + 14));
        CHECK(f0.countNear(kK1x + 14 + 2, cy - 2, kK1x + kRunW, cy + 16, fg, 40) >= 30);
        // K2: START - a plain key, and the mute box unchecked (field ground, strong border)
        CHECK(f0.near(kK2x, kK1y + 15, keyLine) && f0.near(kK2x + kRunW - 1, kK1y + 15, keyLine) && f0.near(kK2x + 4, kK1y + 15, keyBg));
        CHECK(f0.countNear(kK2x + 100, kK1y + 4, kK2x + 180, kK1y + kKeyH - 4, keyFg, 40) >= 15);
        CHECK(f0.near(kK2x + 6, cy + 3, field) && f0.near(kK2x, cy + 7, lineStrong));
        // K3: START for an idle decoder, no mute line at all
        CHECK(f0.near(kK1x, kK3y + 15, keyLine) && f0.near(kK1x + 4, kK3y + 15, keyBg));
        CHECK(f0.countInk(kK1x, kK3y + kKeyH + 1, kK1x + kRunW, kK3y + kKeyH + 24, bg) == 0);
        // K4: disabled - the disabled ground, the disabled ink, the border at half alpha, and the mute box disabled too
        CHECKV(f0.near(kK2x + 4, kK3y + 15, keyDisBg) && f0.near(kK2x, kK3y + 15, over(dangerLine, 0.5, bg), 8), "disabled STOP: ground %s, border %s", hex(f0.at(kK2x + 4, kK3y + 15)).c_str(), hex(f0.at(kK2x, kK3y + 15)).c_str());
        CHECK(f0.countNear(kK2x + 100, kK3y + 4, kK2x + 180, kK3y + kKeyH - 4, keyDisFg, 24) >= 12);
        CHECK(f0.countNear(kK2x + 100, kK3y + 4, kK2x + 180, kK3y + kKeyH - 4, dangerFg, 12) == 0);
        const int cy4 = kK3y + kKeyH + 6;
        CHECK(f0.near(kK2x + 3, cy4 + 3, keyDisBg));
    }

    // =========================================================================================================
    // 9. The three tooltip sentences of the STOP / START key and the mute sentence: the ImGui window's own words
    // =========================================================================================================
    {
        CHECK(sval(ui->get_k1_key_tooltip()) == "Stop this decoder. It stays fitted and keeps its presets; nothing else is touched. Start it again with this key or with one of its presets.");
        CHECK(sval(ui->get_k2_key_tooltip()) == "Start this decoder again. It begins decoding as soon as the receiver is on a frequency it can use.");
        CHECK(sval(ui->get_k3_key_tooltip()) == "This decoder is idle: nothing is using it, so it is not running. It starts by itself when you open its window or the Decoder output window. START keeps it running all the time.");
        CHECK(sval(ui->get_k1_mute_tooltip()) == "Silence the speakers and the web audio stream while this plugin is actually decoding and the receiver is on one of its presets. A stopped or idle plugin silences nothing. The volume setting is not touched.");
    }

    // =========================================================================================================
    // 10. F2 interaction: committed(id, text) on Enter and on losing the focus, toggled(id, v), browse-clicked(id), max-length
    // =========================================================================================================
    {
        reset();
        const int callsignX = kLx + kPad + 100, numberX = kLx + kPad + kHalf + kColGap + 100;
        clk(callsignX, in1 + 12);
        tap(u8"a");
        tap(u8"b");
        CHECK(ui->get_commits() == 0);                                                      // typing alone commits nothing
        tap(slint::platform::key_codes::Return);
        CHECKV(ui->get_commits() == 1 && ui->get_last_id() == 1 && sval(ui->get_last_text()) == "ab" && sval(ui->get_last_form()) == "F2",
               "Enter: %d commits, id %d, text '%s', form %s", ui->get_commits(), ui->get_last_id(), sval(ui->get_last_text()).c_str(), sval(ui->get_last_form()).c_str());
        // typing, then clicking another input, commits on the focus leaving
        tap(u8"c");
        CHECK(ui->get_commits() == 1);
        clk(numberX, in1 + 12);
        CHECKV(ui->get_commits() == 2 && ui->get_last_id() == 1 && sval(ui->get_last_text()) == "abc", "focus lost: %d commits, id %d, text '%s'", ui->get_commits(), ui->get_last_id(), sval(ui->get_last_text()).c_str());
        // the number field is not a filter: a comma and a letter go in, so the host can say "that is not a number"
        tap(slint::platform::key_codes::End);
        tap(u8",");
        tap(u8"x");
        tap(slint::platform::key_codes::Return);
        CHECKV(ui->get_commits() == 3 && ui->get_last_id() == 2 && sval(ui->get_last_text()) == "3.5,x", "number: id %d, text '%s'", ui->get_last_id(), sval(ui->get_last_text()).c_str());
        // the channel field holds 4 characters: "7" + 5 keys = "7abc"
        reset();
        const int chanX = kLx + kPad + 100;
        clk(chanX, chanIn + 12);
        tap(slint::platform::key_codes::End);
        for (const std::u8string_view c : {u8"a", u8"b", u8"c", u8"d", u8"e"}) { tap(c); }
        tap(slint::platform::key_codes::Return);
        CHECKV(ui->get_commits() == 1 && ui->get_last_id() == 7 && sval(ui->get_last_text()) == "7abc", "max-length 4: id %d, text '%s'", ui->get_last_id(), sval(ui->get_last_text()).c_str());
        // BOOL: a click on the box toggles it and says which field it was
        reset();
        const int bx = kLx + kPad + kHalf + kColGap;
        clk(bx + 7, in2 + 12);
        CHECKV(ui->get_toggles() == 1 && ui->get_last_id() == 4 && !ui->get_last_checked() && sval(ui->get_last_form()) == "F2", "BOOL: %d toggles, id %d, checked %d", ui->get_toggles(), ui->get_last_id(), ui->get_last_checked());
        CHECK(ui->get_commits() == 0);                                                      // a bool is not a text commit
        clk(bx + 40, in2 + 12);
        CHECK(ui->get_toggles() == 2 && ui->get_last_checked());
        // PATH: the key says which field browses
        reset();
        clk(kLx + kPad + kFull - 30, pathIn + 12);
        CHECKV(ui->get_browses() == 1 && ui->get_last_id() == 5 && sval(ui->get_last_form()) == "F2", "BROWSE FILES: %d browses, id %d", ui->get_browses(), ui->get_last_id());
        // PASSWORD: SHOW is the field's own, and commits nothing
        clk(kLx + kPad + kFull - 30, pwIn + 12);
        CHECK(ui->get_commits() == 0 && ui->get_browses() == 1);
        Frame shown = render(ui, kW, kH);
        CHECKV(differing(f0, shown, kLx + kPad + 9, pwIn + 3, kLx + kPad + 160, pwIn + kIn - 3) >= 40, "SHOW reveals the text: %d pixels changed", differing(f0, shown, kLx + kPad + 9, pwIn + 3, kLx + kPad + 160, pwIn + kIn - 3));
        clk(kLx + kPad + kFull - 30, pwIn + 12);                                           // HIDE again
        park();
    }

    // =========================================================================================================
    // 11. The fold line: a click folds and unfolds the form (the height follows), and a disabled form stays as it is
    // =========================================================================================================
    {
        const double openH = ui->get_f2_height();
        clk(kLx + 150, kF2y + 14);
        Frame closed = render(ui, kW, kH);
        CHECKV(!ui->get_f2_open() && std::abs(ui->get_f2_height() - 24.0) < 0.01, "F2 after the click: open %d, height %.1f", ui->get_f2_open(), ui->get_f2_height());
        CHECK(closed.countInk(kLx, kF2y + kFoldH + 2, kLx + kFormW, kF2y + 300, panel) == 0);          // the fields are gone
        CHECK(differingShifted(f0, closed, kLx + kPad, kF1y + 4, kLx + kPad + 14, kF1y + 22, kF2y - kF1y) == 0);   // the chevron is F1's closed one
        clk(kLx + 150, kF2y + 14);
        Frame reopened = render(ui, kW, kH);
        CHECKV(ui->get_f2_open() && std::abs(ui->get_f2_height() - openH) < 0.01, "F2 after the second click: open %d, height %.1f (was %.1f)", ui->get_f2_open(), ui->get_f2_height(), openH);
        CHECK(reopened.rowNear(in1, kLx + kPad + 8, kLx + kPad + kHalf - 8, lineStrong));
        park();
        // F1 (it has a tooltip) opens by the keyboard too: Tab reaches its fold line, Space unfolds it, Space folds it again
        CHECK(!ui->get_f1_open());
        tap(slint::platform::key_codes::Tab);
        tap(u8" ");
        CHECKV(ui->get_f1_open(), "Tab, Space: F1 open %d", ui->get_f1_open());
        CHECK(ui->get_f1_height() > 200);
        Frame ring = render(ui, kW, kH);
        CHECKV(ring.countNear(kLx, kF1y, kLx + kFormW, kF1y + kFoldH, focus, 14) >= 20, "a focus ring round the fold line: %d pixels", ring.countNear(kLx, kF1y, kLx + kFormW, kF1y + kFoldH, focus, 14));
        tap(u8" ");
        CHECK(!ui->get_f1_open() && std::abs(ui->get_f1_height() - 24.0) < 0.01);
        // disabled: the click does nothing and the title wears the disabled ink
        ui->set_f1_enabled(false);
        Frame dis = render(ui, kW, kH);
        clk(kLx + 150, kF1y + 14);
        CHECK(!ui->get_f1_open());
        CHECKV(dis.countNear(kLx + kPad + 14 + 3, kF1y + 3, kLx + 300, kF1y + kFoldH - 1, keyDisFg, 30) >= 20 && dis.countNear(kLx + kPad + 14 + 3, kF1y + 3, kLx + 300, kF1y + kFoldH - 1, fg, 12) == 0, "disabled title ink");
        ui->set_f1_enabled(true);
        park();
    }

    // =========================================================================================================
    // 11b. A disabled form: its fields wear the disabled look and take no typing, no tick and no key; the fold line is dead too
    // =========================================================================================================
    {
        ui->set_f2_enabled(false);
        const Frame d = render(ui, kW, kH);
        reset();
        const int lx = kLx + kPad, rx = kLx + kPad + kHalf + kColGap;
        CHECKV(d.near(lx + 4, in1 + 12, keyDisBg) && d.near(rx + 4, in1 + 12, keyDisBg) && d.near(lx + 4, pathIn + 12, keyDisBg), "disabled inputs: %s / %s / %s", hex(d.at(lx + 4, in1 + 12)).c_str(), hex(d.at(rx + 4, in1 + 12)).c_str(), hex(d.at(lx + 4, pathIn + 12)).c_str());
        CHECK(d.near(rx + 3, in2 + (kIn - 14) / 2 + 3, keyDisBg));                                           // the BOOL box
        CHECK(d.countNear(kLx + kPad + 14 + 3, kF2y + 3, kLx + 300, kF2y + kFoldH - 1, keyDisFg, 30) >= 20);   // the title
        clk(lx + 100, in1 + 12);
        tap(u8"q");                                                                                         // (no Enter here: the keyboard focus is still on F1's fold line, where Enter unfolds it)
        clk(rx + 7, in2 + 12);
        clk(kLx + kPad + kFull - 30, pathIn + 12);
        clk(kLx + kPad + kFull - 30, pwIn + 12);
        CHECKV(ui->get_commits() == 0 && ui->get_toggles() == 0 && ui->get_browses() == 0, "a disabled form fired %d commits, %d toggles, %d browses", ui->get_commits(), ui->get_toggles(), ui->get_browses());
        clk(kLx + 150, kF2y + 14);
        CHECK(ui->get_f2_open());                                                                           // the fold line does not fold
        ui->set_f2_enabled(true);
        park();
    }

    // =========================================================================================================
    // 12. F3 (the rail): the host's answer. `pending` keeps what was typed; a refusal snaps the box back to the stored value and shows its
    //     sentence; an edit the user is in the middle of is not trampled by a refresh; a quiet refresh is adopted
    // =========================================================================================================
    {
        auto rail = std::make_shared<slint::VectorModel<SettingField>>(std::vector<SettingField>{
            sf(11, "callsign", "Callsign filter", SettingKind::Text, "M0ABC", false, "e.g. M0ABC", 0, "", false),
            sf(12, "offset", "Squelch offset", SettingKind::Number, "3.5", false, "", 0, "", false),
            sf(13, "log", "Log to file", SettingKind::Bool, "", true, "", 0, "", false),
            sf(14, "logfile", "Log file", SettingKind::Path, "C:\\logs\\aprs.txt", false, "", 0, "", false)});
        ui->set_fields_rail(rail);
        Frame start = render(ui, kW, kH);
        const int in = f3Row + kInputDy, x = kLx + kPad, mid = in + 12;
        CHECK(start.rowNear(in, x + 8, x + kRailCell - 8, lineStrong));
        reset();
        clk(x + 100, mid);
        tap(slint::platform::key_codes::End);
        tap(u8"X");
        tap(slint::platform::key_codes::Return);
        CHECKV(ui->get_commits() == 1 && ui->get_last_id() == 11 && sval(ui->get_last_text()) == "M0ABCX" && sval(ui->get_last_form()) == "F3",
               "typed X: %d commits, id %d, '%s', %s", ui->get_commits(), ui->get_last_id(), sval(ui->get_last_text()).c_str(), sval(ui->get_last_form()).c_str());
        Frame typed = render(ui, kW, kH);
        // (b) the host has the edit and has not answered: PENDING - the stored value is still the old one, the box keeps the typed text
        rail->set_row_data(0, sf(11, "callsign", "Callsign filter", SettingKind::Text, "M0ABC", false, "e.g. M0ABC", 0, "", true));
        Frame waiting = render(ui, kW, kH);
        CHECKV(differing(typed, waiting, x + 3, in + 3, x + kRailCell - 3, in + kIn - 3) == 0, "pending: the box changed in %d pixels", differing(typed, waiting, x + 3, in + 3, x + kRailCell - 3, in + kIn - 3));
        tap(slint::platform::key_codes::Return);
        CHECKV(ui->get_commits() == 2 && sval(ui->get_last_text()) == "M0ABCX", "pending: the box still holds '%s'", sval(ui->get_last_text()).c_str());
        // (c) the answer is a refusal: the box goes back to the stored value, its border turns red and the sentence appears under it
        rail->set_row_data(0, sf(11, "callsign", "Callsign filter", SettingKind::Text, "M0ABC", false, "e.g. M0ABC", 0, "Not saved: it holds a character a setting cannot.", false));
        Frame refused = render(ui, kW, kH);
        CHECKV(differing(typed, refused, x + 3, in + 3, x + kRailCell - 3, in + kIn - 3) >= 6, "refused: the box did not snap back (%d pixels differ)", differing(typed, refused, x + 3, in + 3, x + kRailCell - 3, in + kIn - 3));
        CHECK(refused.rowNear(in, x + 8, x + kRailCell - 8, lampRed));
        CHECKV(refused.countNear(x, in + kIn + 2, x + kRailCell, in + kIn + 40, errFg, 40) >= 40, "the refusal sentence is under the box");
        CHECKV(ui->get_f3_height() > 268.0 + 10.0, "the form grew by the sentence: %.1f", ui->get_f3_height());
        tap(slint::platform::key_codes::Return);
        CHECKV(ui->get_commits() == 3 && sval(ui->get_last_text()) == "M0ABC", "refused: the box holds '%s' (the stored value)", sval(ui->get_last_text()).c_str());
        // (d) the user types again and the host refreshes the row meanwhile (the error is cleared): the typing stays
        tap(slint::platform::key_codes::End);
        tap(u8"Z");
        rail->set_row_data(0, sf(11, "callsign", "Callsign filter", SettingKind::Text, "M0ABC", false, "e.g. M0ABC", 0, "", false));
        render(ui, kW, kH);
        CHECK_NEAR(ui->get_f3_height(), 268.0, 0.6);                                          // the error line left with the error
        tap(slint::platform::key_codes::Return);
        CHECKV(ui->get_commits() == 4 && sval(ui->get_last_text()) == "M0ABCZ", "mid-edit refresh: the box holds '%s'", sval(ui->get_last_text()).c_str());
        // (e) a refresh while nothing is being typed is adopted
        rail->set_row_data(0, sf(11, "callsign", "Callsign filter", SettingKind::Text, "NEWVAL", false, "e.g. M0ABC", 0, "", false));
        render(ui, kW, kH);
        tap(slint::platform::key_codes::Return);
        CHECKV(ui->get_commits() == 5 && sval(ui->get_last_text()) == "NEWVAL", "quiet refresh: the box holds '%s'", sval(ui->get_last_text()).c_str());
        // the bool and the path rows of the rail
        reset();
        const int in3 = f3Row + 2 * (kFieldH + kRowGap) + kInputDy;
        clk(x + 7, in3 + 12);
        CHECKV(ui->get_toggles() == 1 && ui->get_last_id() == 13 && !ui->get_last_checked() && sval(ui->get_last_form()) == "F3", "rail BOOL: id %d, checked %d", ui->get_last_id(), ui->get_last_checked());
        const int in4 = f3Row + 3 * (kFieldH + kRowGap) + kInputDy;
        clk(x + kRailCell - 30, in4 + 12);
        CHECKV(ui->get_browses() == 1 && ui->get_last_id() == 14 && sval(ui->get_last_form()) == "F3", "rail BROWSE: id %d", ui->get_last_id());
        park();
        ui->set_fields_rail(std::make_shared<slint::VectorModel<SettingField>>(std::vector<SettingField>{}));
        render(ui, kW, kH);
        CHECKV(std::abs(ui->get_f3_height() - 24.0) < 0.01, "an open form with no fields is the fold line alone: %.1f", ui->get_f3_height());
    }

    // =========================================================================================================
    // 13. G1 and G2: toggled callbacks carry the NEW state; GRANT turns into a lit REVOKE; the transmit line goes hot
    // =========================================================================================================
    {
        reset();
        clk(kGx + 60, kG1y + 12);                                                          // GRANT RECEIVER CONTROL
        CHECKV(ui->get_grant_events() == 1 && sval(ui->get_last_grant()) == "tune" && ui->get_last_grant_value(), "tune: %d events, '%s', %d", ui->get_grant_events(), sval(ui->get_last_grant()).c_str(), ui->get_last_grant_value());
        Frame hovered = render(ui, kW, kH);
        CHECKV(hovered.near(kGx + 4, kG1y + 12, tok(wb.accent_hover)), "the lit key under the pointer takes the hover accent: %s", hex(hovered.at(kGx + 4, kG1y + 12)).c_str());
        park();
        Frame lit = render(ui, kW, kH);
        CHECKV(lit.near(kGx, kG1y + 12, accent) && lit.near(kGx + 4, kG1y + 12, accent) && lit.near(kGx + 8, kG1y + 2, accent), "the key is lit after the grant: %s", hex(lit.at(kGx + 4, kG1y + 12)).c_str());
        CHECK(lit.countNear(kGx + 8, kG1y + 3, kGx + 150, kG1y + 21, accentInk, 40) >= 25);
        const int litW = lastNearOnRow(lit, kG1y + 12, kGx + 20, kGx + 480, accent) - kGx + 1;
        const int plateW = lastNearOnRow(f0, kG1y + 12, kGx + 20, kGx + 480, plateLine) - kGx + 1;
        CHECKV(litW > plateW, "REVOKE RECEIVER CONTROL (%d) is wider than GRANT RECEIVER CONTROL (%d)", litW, plateW);
        clk(kGx + 60, kG1y + 12);
        CHECKV(ui->get_grant_events() == 2 && !ui->get_last_grant_value(), "revoke: %d events, value %d", ui->get_grant_events(), ui->get_last_grant_value());
        CHECK(render(ui, kW, kH).near(kGx, kG1y + 12, plateLine));                         // back to a plate key
        clk(kGx + 60, kG1y + 24 + 6 + 12);                                                 // REVOKE RADIO SETTINGS
        CHECKV(ui->get_grant_events() == 3 && sval(ui->get_last_grant()) == "settings" && !ui->get_last_grant_value(), "settings: '%s' %d", sval(ui->get_last_grant()).c_str(), ui->get_last_grant_value());
        CHECK(render(ui, kW, kH).near(kGx, kG1y + 24 + 6 + 12, plateLine));                // REVOKE became GRANT: a plate key
        clk(kGx + 60, g1Transmit + 12);                                                    // GRANT TRANSMIT
        CHECKV(ui->get_grant_events() == 4 && sval(ui->get_last_grant()) == "transmit" && ui->get_last_grant_value(), "transmit: '%s' %d", sval(ui->get_last_grant()).c_str(), ui->get_last_grant_value());
        // the transmit state line turns hot (chip-err-fg) when the module is on the air
        ui->set_g1_tx_state("ON AIR: this module has the transmitter.");
        ui->set_g1_tx_hot(true);
        Frame hot = render(ui, kW, kH);
        const int gh = static_cast<int>(ui->get_g1_height());
        CHECKV(hot.countNear(kGx, kG1y + gh - 18, kGx + 480, kG1y + gh + 1, errFg, 40) >= 25 && hot.countNear(kGx, kG1y + gh - 18, kGx + 480, kG1y + gh + 1, accentSoft, 20) == 0, "ON AIR line in chip-err-fg");
        ui->set_g1_tx_hot(false);
        ui->set_g1_tx_state("");
        render(ui, kW, kH);
        CHECKV(ui->get_g1_height() < gh - 10, "no transmit state: the component is shorter (%.1f, was %d)", ui->get_g1_height(), gh);
        // G2: its one key, then nothing when it is not shown, and a disabled key is dead
        reset();
        clk(kGx + 60, kG2y + 12);
        CHECKV(ui->get_grant_events() == 1 && sval(ui->get_last_grant()) == "tune2" && !ui->get_last_grant_value(), "G2: '%s' %d", sval(ui->get_last_grant()).c_str(), ui->get_last_grant_value());
        ui->set_g2_enabled(false);
        Frame dis = render(ui, kW, kH);
        clk(kGx + 60, kG2y + 12);
        CHECK(ui->get_grant_events() == 1);
        CHECKV(dis.near(kGx + 4, kG2y + 12, keyDisBg) && dis.countNear(kGx + 8, kG2y + 3, kGx + 150, kG2y + 21, keyDisFg, 24) >= 12, "disabled grant key: ground %s", hex(dis.at(kGx + 4, kG2y + 12)).c_str());
        ui->set_g2_enabled(true);
        ui->set_g2_tune_on(true);
        ui->set_g2_tune_shown(false);
        Frame none = render(ui, kW, kH);
        CHECKV(std::abs(ui->get_g2_height()) < 0.01 && none.countInk(kGx, kG2y - 2, kGx + 480, kG2y + 30, bg) == 0, "nothing shown: height %.1f", ui->get_g2_height());
        ui->set_g2_tune_shown(true);
        park();
    }

    // =========================================================================================================
    // 14. R1: a click on a box says which grant and what it became; the card follows its model
    // =========================================================================================================
    {
        reset();
        const int b0 = cardBoxes.size() > 0 ? cardBoxes[0] : -1000, b1 = cardBoxes.size() > 1 ? cardBoxes[1] : -1000;
        clk(kRx + 9 + 7, b0 + 7);
        CHECKV(ui->get_card_toggles() == 1 && ui->get_last_card() == 0 && !ui->get_last_card_value(), "first box: %d toggles, index %d, value %d", ui->get_card_toggles(), ui->get_last_card(), ui->get_last_card_value());
        park();
        Frame c1 = render(ui, kW, kH);
        CHECKV(c1.near(kRx + 9 + 6, b0 + 3, field) && c1.near(kRx + 9 + 3, b1 + 3, accent), "the first box is unticked (%s), the second still ticked (%s)", hex(c1.at(kRx + 9 + 6, b0 + 3)).c_str(), hex(c1.at(kRx + 9 + 3, b1 + 3)).c_str());
        clk(kRx + 9 + 14 + 40, b1 + 7);                                                    // the file name toggles too
        CHECKV(ui->get_card_toggles() == 2 && ui->get_last_card() == 1 && !ui->get_last_card_value(), "second row (by its name): index %d", ui->get_last_card());
        // the model is live: no refusal -> the card is shorter; no stale grants -> only the refusal; neither -> nothing at all
        const double full = ui->get_r1_height();
        const auto stale2 = [] {
            return std::make_shared<slint::VectorModel<StaleGrant>>(std::vector<StaleGrant>{StaleGrant{"aprs-decoder-1.0.1.dll", true, true}, StaleGrant{"old-module.dll", false, true}});
        };
        ui->set_stale_r1(stale2());
        ui->set_r1_refusal("");
        render(ui, kW, kH);
        CHECKV(ui->get_r1_height() < full - 20 && ui->get_r1_height() > 100, "without the refusal: %.1f (was %.1f)", ui->get_r1_height(), full);
        ui->set_r1_refusal("\"APRS / AX.25 Packet Decoder\" asked to tune the receiver and was refused.");
        ui->set_stale_r1(std::make_shared<slint::VectorModel<StaleGrant>>(std::vector<StaleGrant>{}));
        Frame only = render(ui, kW, kH);
        CHECKV(ui->get_r1_height() > 50 && ui->get_r1_height() < 90, "the refusal alone: %.1f", ui->get_r1_height());
        CHECK(only.countNear(kRx + 9, kRy + 9, kRx + kCardW - 9, kRy + static_cast<int>(ui->get_r1_height()) - 9, accent, 8) == 0);   // no boxes
        ui->set_r1_refusal("");
        Frame empty = render(ui, kW, kH);
        CHECKV(std::abs(ui->get_r1_height()) < 0.01 && empty.countInk(kRx - 2, kRy - 2, kRx + kCardW + 2, kRy + 60, panelAlt) == 0, "nothing to say: height %.1f, %d pixels", ui->get_r1_height(), empty.countInk(kRx - 2, kRy - 2, kRx + kCardW + 2, kRy + 60, panelAlt));
        // restore, and a disabled card is dead
        ui->set_r1_refusal("\"APRS / AX.25 Packet Decoder\" asked to tune the receiver and was refused.");
        ui->set_stale_r1(stale2());
        ui->set_r1_enabled(false);
        Frame dead = render(ui, kW, kH);
        reset();
        clk(kRx + 9 + 7, b0 + 7);
        clk(kRx + 9 + 7, b1 + 7);
        CHECKV(ui->get_card_toggles() == 0, "a disabled card fired %d toggles", ui->get_card_toggles());
        CHECK(dead.countNear(kRx + 9, kRy + 9, kRx + kCardW - 9, kRy + static_cast<int>(ui->get_r1_height()) - 9, accent, 8) == 0);   // no accent boxes: disabled boxes are dark
        CHECKV(dead.near(kRx + 9 + 6, b0 + 3, keyDisBg), "disabled box %s", hex(dead.at(kRx + 9 + 6, b0 + 3)).c_str());
        ui->set_r1_enabled(true);
        park();
    }

    // =========================================================================================================
    // 15. The chips: toggled(i) from either row; the model drives the look; a disabled set is dead
    // =========================================================================================================
    {
        reset();
        clk(spanAt(chips1, 3).mid(), s1Mid);                                               // the OFF chip toggles too
        CHECKV(ui->get_chip_toggles() == 1 && ui->get_last_chip() == 3 && sval(ui->get_last_chip_source()) == "S1", "S1 chip 3: %d toggles, index %d, source %s", ui->get_chip_toggles(), ui->get_last_chip(), sval(ui->get_last_chip_source()).c_str());
        clk(spanAt(chips1, 0).mid(), s1Mid);
        CHECK(ui->get_last_chip() == 0 && ui->get_chip_toggles() == 2);
        const int s2Mid2 = kS2y + kChipH + kChipGap + kChipH / 2;
        const std::vector<Span> row2 = runsOnRow(f0, s2Mid2, kSx, kSx + 400, bg);
        clk(spanAt(row2, 1).mid(), s2Mid2);                                                 // the fifth chip, second row of S2
        CHECKV(ui->get_last_chip() == 4 && sval(ui->get_last_chip_source()) == "S2", "S2 row two, second chip: index %d, source %s", ui->get_last_chip(), sval(ui->get_last_chip_source()).c_str());
        clk(spanAt(chips1, 1).r + 2, s1Mid);                                                // the gap is not a chip
        CHECK(ui->get_chip_toggles() == 3);
        // hover: a chip under the pointer is lighter than at rest
        mv(spanAt(chips1, 1).mid(), s1Mid);
        Frame hov = render(ui, kW, kH);
        CHECKV(differing(f0, hov, spanAt(chips1, 1).l + 2, kS1y + 2, spanAt(chips1, 1).r - 2, kS1y + kChipH - 2) >= 20, "a hovered chip changes (%d pixels)", differing(f0, hov, spanAt(chips1, 1).l + 2, kS1y + 2, spanAt(chips1, 1).r - 2, kS1y + kChipH - 2));
        CHECK(differing(f0, hov, spanAt(chips1, 0).l, kS1y, spanAt(chips1, 0).r, kS1y + kChipH) == 0);   // only that one
        park();
        // the model: switching the off chip on and the FED chip off swaps their looks
        std::vector<StateChipModel> m = {
            StateChipModel{"FED", 3, ChipTone::Good, LampState::Green, false}, StateChipModel{"IDLE", 2, ChipTone::Neutral, LampState::Off, true},
            StateChipModel{"NOT DECODING", 1, ChipTone::Warn, LampState::Amber, true}, StateChipModel{"TAKES NO SIGNAL", 0, ChipTone::Neutral, LampState::Off, true},
            StateChipModel{"STOPPED", 1, ChipTone::Neutral, LampState::Off, true}, StateChipModel{"REFUSED", 1, ChipTone::Err, LampState::Red, true}};
        ui->set_chips_s1(std::make_shared<slint::VectorModel<StateChipModel>>(m));
        Frame sw = render(ui, kW, kH);
        const std::vector<Span> run2 = runsOnRow(sw, s1Mid, kSx, kSx + 640, bg);
        CHECKV(run2.size() == 6, "%zu chips after the model changed", run2.size());
        CHECKV(sw.near(spanAt(run2, 0).l + 3, s1Mid, over(chipBg, 0.5, bg), 5), "FED off: neutral ground at half alpha %s", hex(sw.at(spanAt(run2, 0).l + 3, s1Mid)).c_str());
        CHECK(sw.near(spanAt(run2, 3).l + 3, s1Mid, chipBg, 5));                             // TAKES NO SIGNAL is on: the neutral chip at full strength
        // a zero count is still drawn ("TAKES NO SIGNAL 0"), and a chip with count -1 shows its word alone
        m[1] = StateChipModel{"IDLE", -1, ChipTone::Neutral, LampState::Off, true};
        ui->set_chips_s1(std::make_shared<slint::VectorModel<StateChipModel>>(m));
        const Frame bare = render(ui, kW, kH);
        const std::vector<Span> run3 = runsOnRow(bare, s1Mid, kSx, kSx + 640, bg);
        CHECKV(run3.size() == 6 && spanAt(run3, 1).w() < spanAt(run2, 1).w(), "a chip without a count is narrower: %d vs %d", spanAt(run3, 1).w(), spanAt(run2, 1).w());
        // disabled: no toggling, the kit's disabled ground
        ui->set_chips_s1(std::make_shared<slint::VectorModel<StateChipModel>>(std::vector<StateChipModel>{
            StateChipModel{"FED", 3, ChipTone::Good, LampState::Green, true}, StateChipModel{"IDLE", 2, ChipTone::Neutral, LampState::Off, true},
            StateChipModel{"NOT DECODING", 1, ChipTone::Warn, LampState::Amber, true}, StateChipModel{"TAKES NO SIGNAL", 0, ChipTone::Neutral, LampState::Off, false},
            StateChipModel{"STOPPED", 1, ChipTone::Neutral, LampState::Off, true}, StateChipModel{"REFUSED", 1, ChipTone::Err, LampState::Red, true}}));
        ui->set_s2_enabled(false);
        const Frame offf = render(ui, kW, kH);
        reset();
        clk(spanAt(row2, 1).mid(), s2Mid2);
        clk(spanAt(runsOnRow(f0, kS2y + 10, kSx, kSx + 400, bg), 0).mid(), kS2y + 10);
        CHECKV(ui->get_chip_toggles() == 0, "a disabled chip toggled (%d)", ui->get_chip_toggles());
        CHECKV(offf.near(spanAt(runsOnRow(offf, kS2y + 10, kSx, kSx + 400, bg), 0).l + 3, kS2y + 10, keyDisBg, 5), "disabled chip ground %s", hex(offf.at(spanAt(runsOnRow(offf, kS2y + 10, kSx, kSx + 400, bg), 0).l + 3, kS2y + 10)).c_str());
        ui->set_s2_enabled(true);
        park();
    }

    // =========================================================================================================
    // 15b. The chips by keyboard: Tab reaches them one by one, a 2 px ring shows on the one that holds the focus, Space and Enter press it
    // =========================================================================================================
    {
        reset();
        const auto ringed = [&](const Frame& fr, const Span& c) { return fr.near(c.l - 2, s1Mid, focus, 14) && fr.near(c.r + 1, s1Mid, focus, 14); };
        int tabs = 0, blind = 0;
        Frame fr = render(ui, kW, kH);
        while (tabs < 150 && !ringed(fr, spanAt(chips1, 0))) {
            tap(slint::platform::key_codes::Tab);
            fr = render(ui, kW, kH);
            if (fr.countNear(0, 0, kW, kH, focus, 3) == 0) { ++blind; }                                    // a tab stop that draws no ring is a dead one
            ++tabs;
        }
        CHECKV(tabs < 150 && ringed(fr, spanAt(chips1, 0)), "Tab never reached the first chip (%d presses): %s", tabs, hex(fr.at(spanAt(chips1, 0).l - 2, s1Mid)).c_str());
        CHECKV(blind == 0, "%d of the %d tab stops before the first chip showed no focus ring", blind, tabs);
        CHECK(fr.near(spanAt(chips1, 0).l - 1, s1Mid, bg, 14) && fr.near(spanAt(chips1, 0).mid(), kS1y - 3, focus, 14) && fr.near(spanAt(chips1, 0).mid(), kS1y + kChipH + 2, focus, 14));     // 1 px clear, 2 px ring
        CHECK(!ringed(fr, spanAt(chips1, 1)));
        tap(u8" ");
        CHECKV(ui->get_chip_toggles() == 1 && ui->get_last_chip() == 0 && sval(ui->get_last_chip_source()) == "S1", "Space on the first chip: %d toggles, index %d, source %s", ui->get_chip_toggles(), ui->get_last_chip(), sval(ui->get_last_chip_source()).c_str());
        tap(slint::platform::key_codes::Tab);
        fr = render(ui, kW, kH);
        CHECK(ringed(fr, spanAt(chips1, 1)) && !ringed(fr, spanAt(chips1, 0)));
        tap(slint::platform::key_codes::Return);
        CHECKV(ui->get_chip_toggles() == 2 && ui->get_last_chip() == 1, "Enter on the second chip: %d toggles, index %d", ui->get_chip_toggles(), ui->get_last_chip());
        tap(u8"a");                                                                                       // other keys do nothing
        CHECK(ui->get_chip_toggles() == 2);
        park();
    }

    // =========================================================================================================
    // 15c. Tooltips of the grant keys, the card's boxes and the chips: each carries the host's sentence, 6 px under its own height
    // =========================================================================================================
    {
        mv(kGx + 60, kG1y + 12);                                                                           // GRANT RECEIVER CONTROL has a sentence of its own
        settle(milliseconds(750));
        const Frame a = render(ui, kW, kH);
        CHECKV(a.near(kGx + 12, kG1y + 24 + 6, knobRing, 3), "grant key tooltip border %s", hex(a.at(kGx + 12, kG1y + 24 + 6)).c_str());
        park();
        mv(kGx + 60, kG1y + 24 + 6 + 12);                                                                  // REVOKE RADIO SETTINGS has none
        settle(milliseconds(750));
        CHECK(!render(ui, kW, kH).near(kGx + 12, kG1y + 24 + 6 + 24 + 6, knobRing, 3));
        park();
        const int b0 = cardBoxes.size() > 0 ? cardBoxes[0] : -1000;
        mv(kRx + 9 + 40, b0 + 7);                                                                          // the card's rows carry the card's sentence
        settle(milliseconds(750));
        const Frame c = render(ui, kW, kH);
        CHECKV(c.near(kRx + 9 + 12, b0 + 14 + 6, knobRing, 3), "card checkbox tooltip border %s", hex(c.at(kRx + 9 + 12, b0 + 14 + 6)).c_str());
        park();
        const int s2Mid1 = kS2y + kChipH / 2;
        mv(spanAt(runsOnRow(f0, s2Mid1, kSx, kSx + 400, bg), 1).mid(), s2Mid1);                            // a chip of S2 carries the set's sentence
        settle(milliseconds(750));
        const Frame d = render(ui, kW, kH);
        const Span s2c = spanAt(runsOnRow(f0, s2Mid1, kSx, kSx + 400, bg), 1);
        CHECKV(d.near(s2c.l + 12, kS2y + kChipH + 6, knobRing, 3), "chip tooltip border %s", hex(d.at(s2c.l + 12, kS2y + kChipH + 6)).c_str());
        park();
        mv(spanAt(chips1, 1).mid(), s1Mid);                                                                // S1 has no sentence
        settle(milliseconds(750));
        CHECK(!render(ui, kW, kH).near(spanAt(chips1, 1).l + 12, kS1y + kChipH + 6, knobRing, 3));
        park();
    }

    // =========================================================================================================
    // 16. The run keys: run-clicked, mute-toggled(bool) with the new state; a disabled pair is dead; the key follows offers-stop
    // =========================================================================================================
    {
        reset();
        clk(kK1x + 140, kK1y + 15);
        CHECKV(ui->get_run_count() == 1 && sval(ui->get_last_run()) == "K1", "STOP: %d runs, '%s'", ui->get_run_count(), sval(ui->get_last_run()).c_str());
        clk(kK1x + 7, kK1y + kKeyH + 6 + 7);                                                // the mute box of K1 (checked) unticks
        CHECKV(ui->get_mute_count() == 1 && sval(ui->get_last_mute_source()) == "K1" && !ui->get_last_mute(), "K1 mute: %d, %s, %d", ui->get_mute_count(), sval(ui->get_last_mute_source()).c_str(), ui->get_last_mute());
        clk(kK1x + 120, kK1y + kKeyH + 6 + 7);                                              // its words toggle it back
        CHECK(ui->get_mute_count() == 2 && ui->get_last_mute());
        CHECK(ui->get_run_count() == 1);                                                    // the checkbox is not the key
        clk(kK2x + 140, kK1y + 15);
        CHECKV(ui->get_run_count() == 2 && sval(ui->get_last_run()) == "K2", "START: '%s'", sval(ui->get_last_run()).c_str());
        clk(kK2x + 7, kK1y + kKeyH + 6 + 7);                                                // K2's mute (unticked) ticks
        CHECKV(ui->get_mute_count() == 3 && sval(ui->get_last_mute_source()) == "K2" && ui->get_last_mute(), "K2 mute: %d %s %d", ui->get_mute_count(), sval(ui->get_last_mute_source()).c_str(), ui->get_last_mute());
        clk(kK1x + 140, kK3y + 15);
        CHECK(ui->get_run_count() == 3 && sval(ui->get_last_run()) == "K3");
        clk(kK2x + 140, kK3y + 15);                                                         // K4 is disabled
        clk(kK2x + 7, kK3y + kKeyH + 6 + 7);
        CHECKV(ui->get_run_count() == 3 && ui->get_mute_count() == 3, "disabled run keys fired: %d runs, %d mutes", ui->get_run_count(), ui->get_mute_count());
        // the key follows the model: STOP becomes START (a plain key), the label and the tooltip change
        ui->set_k1_offers_stop(false);
        Frame start = render(ui, kW, kH);
        CHECKV(start.near(kK1x, kK1y + 15, keyLine) && start.near(kK1x + 4, kK1y + 15, keyBg), "START ground %s", hex(start.at(kK1x + 4, kK1y + 15)).c_str());
        CHECK(sval(ui->get_k1_key_tooltip()).rfind("Start this decoder again", 0) == 0);
        CHECK(start.countNear(kK1x + 100, kK1y + 4, kK1x + 180, kK1y + kKeyH - 4, dangerFg, 12) == 0);
        ui->set_k1_offers_stop(true);
        park();
        // the tooltips show: the key's sentence opens 6 px under the 30 px key
        mv(kK1x + 140, kK1y + 15);
        settle(milliseconds(750));
        const Frame tip = render(ui, kW, kH);
        CHECKV(tip.near(kK1x + 12, kK1y + kKeyH + 6, knobRing, 3) && tip.near(kK1x + 5, kK1y + kKeyH + 6 + 8, menu), "STOP tooltip: border %s, ground %s", hex(tip.at(kK1x + 12, kK1y + kKeyH + 6)).c_str(), hex(tip.at(kK1x + 5, kK1y + kKeyH + 6 + 8)).c_str());
        park();
        mv(kK1x + 120, kK1y + kKeyH + 6 + 7);                                               // the mute words
        settle(milliseconds(750));
        const Frame tip2 = render(ui, kW, kH);
        CHECKV(tip2.near(kK1x + 12, kK1y + kKeyH + 6 + 14 + 6, knobRing, 3), "mute tooltip border %s", hex(tip2.at(kK1x + 12, kK1y + kKeyH + 6 + 14 + 6)).c_str());
        park();
        // F1's own tooltip: the fold line, 6 px under the 24 px line
        mv(kLx + 150, kF1y + 12);
        settle(milliseconds(750));
        const Frame tip3 = render(ui, kW, kH);
        CHECKV(tip3.near(kLx + 12, kF1y + kFoldH + 6, knobRing, 3) && tip3.near(kLx + 5, kF1y + kFoldH + 6 + 8, menu), "fold line tooltip: %s / %s", hex(tip3.at(kLx + 12, kF1y + kFoldH + 6)).c_str(), hex(tip3.at(kLx + 5, kF1y + kFoldH + 6 + 8)).c_str());
        park();
    }

    // =========================================================================================================
    // 17. The skin: a second filling reaches every form with no component change
    // =========================================================================================================
    {
        Skin probe = ui->global<Workbench>().get_value();
        probe.accent = slint::Color::from_rgb_uint8(0x00, 0xc0, 0x40);
        probe.plate_line = slint::Color::from_rgb_uint8(0x00, 0xff, 0xff);
        probe.card_warn = slint::Color::from_rgb_uint8(0x10, 0x30, 0x10);
        probe.card_warn_line = slint::Color::from_rgb_uint8(0xff, 0x00, 0xff);
        probe.chip_good_bg = slint::Color::from_rgb_uint8(0x30, 0x00, 0x30);
        probe.danger_bg = slint::Color::from_rgb_uint8(0x00, 0x20, 0x40);
        probe.field = slint::Color::from_rgb_uint8(0x20, 0x30, 0x10);
        probe.line_strong = slint::Color::from_rgb_uint8(0xff, 0xff, 0x00);
        ui->global<Theme>().set_skin(probe);
        ui->set_chips_s1(std::make_shared<slint::VectorModel<StateChipModel>>(std::vector<StateChipModel>{
            StateChipModel{"FED", 3, ChipTone::Good, LampState::Green, true}, StateChipModel{"IDLE", 2, ChipTone::Neutral, LampState::Off, true},
            StateChipModel{"NOT DECODING", 1, ChipTone::Warn, LampState::Amber, true}, StateChipModel{"TAKES NO SIGNAL", 0, ChipTone::Neutral, LampState::Off, false},
            StateChipModel{"STOPPED", 1, ChipTone::Neutral, LampState::Off, true}, StateChipModel{"REFUSED", 1, ChipTone::Err, LampState::Red, true}}));
        Frame g = render(ui, kW, kH);
        CHECK(g.near(kGx + 8, kG1y, rgb(0x00ffff)));                                         // the plate key's border
        CHECK(g.near(kRx + 4, kRy + 40, rgb(0x103010)) && g.near(kRx, kRy + 40, rgb(0xff00ff)));    // the card
        CHECK(g.near(kSx + 3, s1Mid, rgb(0x300030)));                                         // the FED chip
        CHECK(g.near(kK1x + 4, kK1y + 15, rgb(0x002040)));                                    // the STOP key
        CHECK(g.near(kLx + kPad + 4, in1 + 12, rgb(0x203010)) && g.rowNear(in1, kLx + kPad + 8, kLx + kPad + kHalf - 8, rgb(0xffff00)));   // the form's inputs
        CHECK(g.near(kLx + kPad + kHalf + kColGap + 3, in2 + (kIn - 14) / 2 + 3, rgb(0x00c040)));   // the BOOL box
        ui->global<Theme>().set_skin(ui->global<Workbench>().get_value());
        ui->set_chips_s1(std::make_shared<slint::VectorModel<StateChipModel>>(std::vector<StateChipModel>{
            StateChipModel{"FED", 3, ChipTone::Good, LampState::Green, true}, StateChipModel{"IDLE", 2, ChipTone::Neutral, LampState::Off, true},
            StateChipModel{"NOT DECODING", 1, ChipTone::Warn, LampState::Amber, true}, StateChipModel{"TAKES NO SIGNAL", 0, ChipTone::Neutral, LampState::Off, false},
            StateChipModel{"STOPPED", 1, ChipTone::Neutral, LampState::Off, true}, StateChipModel{"REFUSED", 1, ChipTone::Err, LampState::Red, true}}));
        Frame h = render(ui, kW, kH);
        CHECKV(differing(f0, h, kSx, kS1y, kSx + 640, kS1y + kChipH) == 0, "the Workbench filling comes back: %d chip pixels differ", differing(f0, h, kSx, kS1y, kSx + 640, kS1y + kChipH));
        CHECKV(differing(f0, h, kGx, kG2y, kGx + 480, kG2y + 24) == 0, "...and the grant key");
    }

    return testSummary("test_slint_forms");
}
