// The PRESETS family of the plugin kit (docs 6.37): PresetBar (the row form of the torn-off window, wrapped by `columns`, and the
// column form of the rail entry) and PlateKeys (the module page's command keys), rendered headlessly from
// ui/specimens/presets_specimen.slint and probed against the PluginKit sheet ("Torn-off plugin window": the preset bar, `.row.g6.fw`
// padding 6 12 with a 1 px rule under it and `.btn.sm` keys; "Sections-panel form": the wide keys, the 24 px x key, the green note and the
// wide plate save key; "Module page (tab) excerpt": the plate keys) and the contract in COMPONENTS.md ("The plugin kit").
//
// The typefaces are the embedded Inter and JetBrains Mono, but text checks still assert INK in a region and its COLOUR, never glyph
// positions: the boxes, borders and gaps are drawn geometry and are asserted to the pixel. Input goes through the harness's dispatched
// pointer and key events, so pressed / forgot / save-clicked are the real callbacks. A list of key spans is read through spanAt(), which
// answers an impossible span when the list is short, so a missing key fails every check made on it instead of skipping them.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "presets_specimen.h"

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
// The i-th span of a list, or an impossible one off the sheet when the list is shorter: every probe made on it then fails.
Span spanAt(const std::vector<Span>& v, std::size_t i) { return i < v.size() ? v[i] : Span{-1000, -990}; }

// The runs of pixels that are not `outside` along row y. A solid key is one run (border and ground are both "not outside"); a key with a
// transparent interior (ghost, plate) is its two borders and whatever its label draws, which callers handle themselves.
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

// The last x in [x0, x1) whose pixel on row y is within tol of `c`: the right border of a key with a transparent interior; -1 when none.
int lastNearOnRow(const Frame& f, int y, int x0, int x1, Rgb c, int tol = 3) {
    for (int x = x1 - 1; x >= x0; --x) { if (f.near(x, y, c, tol)) { return x; } }
    return -1;
}

// The bounding box and count of the ink (pixels differing from `bg`) in a rectangle.
struct Ink {
    int n = 0, x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    int w() const { return x1 - x0 + 1; }
    int h() const { return y1 - y0 + 1; }
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

// ---- the specimen's table (presets_specimen.slint) ----
constexpr int kW = 520, kH = 640;
constexpr int kBarX = 10, kBarW = 500;
constexpr int kAy = 24, kBy = 84, kCy = 174;                 // PresetBar A, B, C (row form)
constexpr int kPadX = 12, kPadY = 6, kKeyH = 24, kGap = 6;   // .row.g6.fw padding 6 12; .btn.sm 24 high; gap 6
constexpr int kDx = 20, kDy = 244, kDw = 296;                // PresetBar D (the rail form), its ground starts at (10, 234)
constexpr int kEy = 420, kFy = 490, kGx = 270;               // PlateKeys E, F, G

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = PresetsSpecimen::create();
    slint::Window& win = ui->window();

    // The tokens, read from the Workbench skin by name (the theme test pins their values).
    const Skin wb = ui->global<Workbench>().get_value();
    const Rgb bg = tok(wb.bg), panel = tok(wb.panel), panelAlt = tok(wb.panel_alt), line = tok(wb.line), keyBg = tok(wb.key_bg);
    const Rgb keyLine = tok(wb.key_line), keyFg = tok(wb.key_fg), accent = tok(wb.accent), accentInk = tok(wb.accent_ink);
    const Rgb accentHover = tok(wb.accent_hover), menuHot = tok(wb.menu_hot), menu = tok(wb.menu), knobRing = tok(wb.knob_ring);
    const Rgb plateLine = tok(wb.plate_line), plateFg = tok(wb.plate_fg), goodFg = tok(wb.chip_good_fg), fgSoft = tok(wb.fg_soft);
    const Rgb keyDisBg = tok(wb.key_disabled_bg), keyDisFg = tok(wb.key_disabled_fg);
    // the artboard's own values, once, so a drift of the Workbench filling is caught here too
    CHECK(nearColour(plateLine, rgb(0xf5a524), 0) && nearColour(plateFg, rgb(0xf5c66b), 0) && nearColour(goodFg, rgb(0x7fe6ad), 0));
    CHECK(nearColour(keyBg, rgb(0x262a32), 0) && nearColour(keyLine, rgb(0x363b45), 0) && nearColour(line, rgb(0x2b2f37), 0));

    const auto clk = [&](int x, int y) { click(win, static_cast<float>(x), static_cast<float>(y)); };
    const auto rclk = [&](int x, int y) { click(win, static_cast<float>(x), static_cast<float>(y), slint::PointerEventButton::Right); };
    const auto mv = [&](int x, int y) { moveTo(win, static_cast<float>(x), static_cast<float>(y)); };
    const auto reset = [&] {
        ui->set_pressed_count(0); ui->set_last_pressed(-1); ui->set_forgot_count(0); ui->set_last_forgot(-1);
        ui->set_save_count(0); ui->set_plate_count(0); ui->set_last_plate(-1); ui->set_last_source("");
    };
    // Parks the pointer on the sheet's empty corner: closes any tooltip and ends any hover.
    const auto park = [&] { mv(515, 636); settle(milliseconds(30)); };
    const auto sval = [&](const slint::SharedString& s) { return std::string(s); };

    // =========================================================================================================
    // 1. The sheet, before anything is touched
    // =========================================================================================================
    Frame f0 = render(ui, kW, kH);
    CHECK(f0.width == kW && f0.height == kH);
    CHECK(f0.near(518, 638, bg));
    f0.savePng(shotDir() + "presets-specimen.png");

    // =========================================================================================================
    // 2. PresetBar A, the row form: padding 6 12, keys 24 high 6 apart, a 1 px rule under it (37 px in all)
    // =========================================================================================================
    const int aKeyTop = kAy + kPadY, aMid = aKeyTop + kKeyH / 2;
    const std::vector<Span> keysA = runsOnRow(f0, aMid, kBarX + 1, kBarX + kBarW - 1, panel);       // three solid keys, then the ghost's borders and label
    {
        CHECK_NEAR(ui->get_a_height(), 37.0, 0.01);
        const int rule = kAy + kPadY + kKeyH + kPadY;
        CHECK(f0.near(kBarX + 4, kAy + 3, panel));                                    // the window's ground behind the bar
        CHECK(f0.rowNear(rule, kBarX + 2, kBarX + kBarW - 2, line));                   // the 1 px rule across the bar's foot ...
        CHECK(f0.rowNear(rule - 1, kBarX + 2, kBarX + 10, panel));                     // ... with the ground above it ...
        CHECK(f0.rowNear(rule + 1, kBarX + 2, kBarX + kBarW - 2, bg));                 // ... and the sheet's ground below: 37 px exactly
        CHECK(f0.near(kBarX + 5, aKeyTop - 1, panel));                                 // 6 px of padding above the keys
        CHECKV(keysA.size() >= 3, "%zu runs on row %d (wanted the three solid keys and the ghost key's borders)", keysA.size(), aMid);
        const Span k0 = spanAt(keysA, 0), k1 = spanAt(keysA, 1), k2 = spanAt(keysA, 2);
        CHECKV(k0.l == kBarX + kPadX, "first key starts at %d, wanted %d (12 px padding)", k0.l, kBarX + kPadX);
        CHECKV(k1.l - k0.r == kGap && k2.l - k1.r == kGap, "gaps %d and %d, wanted 6", k1.l - k0.r, k2.l - k1.r);
        for (const Span& s : {k0, k1, k2}) { CHECKV(s.w() >= 40 && s.w() <= 150, "key %d..%d is %d wide", s.l, s.r, s.w()); }
        // key 0: a plain .btn.sm - key-bg ground, key-line border, 24 px high, text in key-fg
        CHECK(f0.near(k0.l, aMid, keyLine));
        CHECK(f0.near(k0.r - 1, aMid, keyLine));
        CHECK(f0.near(k0.l + 8, aKeyTop, keyLine) && f0.near(k0.l + 8, aKeyTop + kKeyH - 1, keyLine));      // top and bottom borders: 24 px
        CHECK(f0.near(k0.l + 8, aKeyTop - 1, panel) && f0.near(k0.l + 8, aKeyTop + kKeyH, panel));
        CHECK(f0.near(k0.l + 8, aKeyTop + 1, keyBg) && f0.near(k0.l + 3, aMid, keyBg));
        CHECKV(f0.countInk(k0.l + 2, aKeyTop + 2, k0.r - 2, aKeyTop + kKeyH - 2, keyBg) >= 30, "key 0 label ink %d", f0.countInk(k0.l + 2, aKeyTop + 2, k0.r - 2, aKeyTop + kKeyH - 2, keyBg));
        CHECK(f0.countNear(k0.l + 2, aKeyTop + 2, k0.r - 2, aKeyTop + kKeyH - 2, keyFg, 40) >= 8);
        // key 1: LIT - the accent ground and border, accent-ink text
        CHECK(f0.near(k1.l, aMid, accent) && f0.near(k1.r - 1, aMid, accent));
        CHECK(f0.near(k1.l + 8, aKeyTop, accent) && f0.near(k1.l + 8, aKeyTop + kKeyH - 1, accent));
        CHECK(f0.near(k1.l + 3, aMid, accent) && f0.near(k1.l + 8, aKeyTop + 2, accent));
        CHECK(f0.countNear(k1.l + 2, aKeyTop + 2, k1.r - 2, aKeyTop + kKeyH - 2, accentInk, 40) >= 8);
        CHECK(f0.countNear(k1.l + 2, aKeyTop + 2, k1.r - 2, aKeyTop + kKeyH - 2, keyFg, 12) == 0);
        // key 2: the operator's own preset looks like any other (the forget gesture is invisible until used)
        CHECK(f0.near(k2.l, aMid, keyLine) && f0.near(k2.l + 3, aMid, keyBg));
        // the + Save key: ghost - the window's ground inside, a key-line border, 24 px high
        const int sl = k2.r + kGap, sr = lastNearOnRow(f0, aMid, sl + 20, kBarX + kBarW - 1, keyLine);
        CHECKV(f0.near(sl, aMid, keyLine) && sr > sl + 30, "save key borders at %d and %d", sl, sr);
        CHECK(f0.near(sl + 8, aKeyTop, keyLine) && f0.near(sl + 8, aKeyTop + kKeyH - 1, keyLine));
        CHECK(f0.near(sl + 8, aKeyTop + 2, panel) && f0.near(sl + 3, aMid, panel));  // ghost: no ground of its own
        CHECKV(f0.countInk(sl + 3, aKeyTop + 3, sr - 2, aKeyTop + kKeyH - 3, panel) >= 20, "the save key's label is drawn");
        CHECK(f0.countNear(sl + 3, aKeyTop + 3, sr - 2, aKeyTop + kKeyH - 3, keyFg, 40) >= 6);
        CHECKV(sr - sl + 1 >= 44 && sr - sl + 1 <= 110, "save key is %d wide", sr - sl + 1);
    }

    // =========================================================================================================
    // 3. PresetBar B: the keys wrap at `columns` - four in the first row, then two and the + Save key in the second; rows 6 apart
    // =========================================================================================================
    const int bTop1 = kBy + kPadY, bTop2 = bTop1 + kKeyH + kGap;
    const std::vector<Span> keysB1 = runsOnRow(f0, bTop1 + kKeyH / 2, kBarX + 1, kBarX + kBarW - 1, panel);
    const std::vector<Span> keysB2 = runsOnRow(f0, bTop2 + kKeyH / 2, kBarX + 1, kBarX + kBarW - 1, panel);
    {
        CHECK_NEAR(ui->get_b_height(), 67.0, 0.01);
        const int rule = kBy + 66;
        CHECK(f0.rowNear(rule, kBarX + 2, kBarX + kBarW - 2, line));
        CHECK(f0.rowNear(rule + 1, kBarX + 2, kBarX + kBarW - 2, bg));
        CHECKV(keysB1.size() == 4, "row one has %zu runs (wanted the four solid keys)", keysB1.size());
        CHECKV(keysB2.size() >= 3, "row two has %zu runs (wanted two solid keys and the ghost key's borders)", keysB2.size());
        CHECK(spanAt(keysB1, 0).l == kBarX + kPadX && spanAt(keysB2, 0).l == kBarX + kPadX);       // both rows start at the left padding
        for (std::size_t i = 1; i < 4; ++i) { CHECK(spanAt(keysB1, i).l - spanAt(keysB1, i - 1).r == kGap); }
        CHECK(spanAt(keysB2, 1).l - spanAt(keysB2, 0).r == kGap);
        CHECKV(bTop2 - (bTop1 + kKeyH) == kGap, "rows are %d px apart", bTop2 - (bTop1 + kKeyH));
        CHECK(f0.near(spanAt(keysB1, 0).l + 8, bTop1 + kKeyH, panel) && f0.near(spanAt(keysB1, 0).l + 8, bTop2 - 1, panel));     // the 6 px between the rows is ground
        // the third preset is lit (accent), the others are plain
        const Span lit = spanAt(keysB1, 2);
        CHECK(f0.near(lit.l + 3, bTop1 + kKeyH / 2, accent) && f0.near(lit.l, bTop1 + kKeyH / 2, accent));
        CHECK(f0.near(spanAt(keysB1, 3).l + 3, bTop1 + kKeyH / 2, keyBg));
        // the sixth preset is the operator's own and the second row's second key; the ghost + Save follows it
        const int sl = spanAt(keysB2, 1).r + kGap;
        CHECKV(f0.near(sl, bTop2 + kKeyH / 2, keyLine) && f0.near(sl + 8, bTop2, keyLine), "the + Save key starts the second row's tail at %d", sl);
        CHECK(f0.countInk(kBarX + 2, bTop2 + 3, kBarX + kBarW - 2, bTop2 + kKeyH - 3, panel) >= 90);   // the second row is drawn
    }

    // =========================================================================================================
    // 4. PresetBar C: disabled - the kit's disabled look on every key (the same keys as A, so the same cells; the disabled ground IS the
    //    window's ground, Theme.key-disabled-bg being Theme.panel, so the cells cannot be found by their ground)
    // =========================================================================================================
    {
        const int keyTop = kCy + kPadY, ym = keyTop + kKeyH / 2;
        const Rgb halfEdge = over(keyLine, 0.5, panel), halfLit = over(accent, 0.5, panel);
        for (std::size_t i = 0; i < 3; ++i) {
            const Span s = spanAt(keysA, i);
            const Rgb edge = i == 1 ? halfLit : halfEdge;                                // the lit key's border is the accent, at half alpha
            CHECKV(f0.near(s.l + 3, ym, keyDisBg), "disabled key %zu ground %s, wanted %s", i, hex(f0.at(s.l + 3, ym)).c_str(), hex(keyDisBg).c_str());
            CHECKV(f0.near(s.l + 8, keyTop, edge, 6) && f0.near(s.l + 8, keyTop + kKeyH - 1, edge, 6), "disabled key %zu border %s, wanted about %s", i, hex(f0.at(s.l + 8, keyTop)).c_str(), hex(edge).c_str());
            CHECK(f0.near(s.l, ym, edge, 6) && f0.near(s.r - 1, ym, edge, 6));
            CHECK(f0.countNear(s.l + 2, keyTop + 2, s.r - 2, keyTop + kKeyH - 2, keyDisFg, 24) >= 6);
            CHECK(f0.countNear(s.l + 2, keyTop + 2, s.r - 2, keyTop + kKeyH - 2, accent, 12) == 0);      // the ground stays dark: no accent inside
            CHECK(f0.countNear(s.l + 2, keyTop + 2, s.r - 2, keyTop + kKeyH - 2, keyFg, 12) == 0);
        }
        // the ghost + Save key is the same: only its half-alpha border and its disabled label
        const int sl = spanAt(keysA, 2).r + kGap;
        CHECK(f0.near(sl, ym, halfEdge, 6));
        CHECK(f0.countNear(sl + 3, keyTop + 3, sl + 40, keyTop + kKeyH - 3, keyDisFg, 24) >= 4);
    }

    // =========================================================================================================
    // 5. PresetBar D, the rail form: wide keys one a line, the mine row with its 24 px x key, the green note, the plate save key
    // =========================================================================================================
    const int dY1 = kDy, dY2 = dY1 + kKeyH + kGap, dY3 = dY2 + kKeyH + kGap, dNoteTop = dY3 + kKeyH + kGap;
    // the plate save key is the first row under the note whose left border is the accent
    const int dSaveTop = f0.firstRowNear(kDx + 8, dNoteTop, kDy + 200, plateLine);
    const int dSaveBottom = dSaveTop + kKeyH;
    {
        const double dh = ui->get_d_height();
        // 296 px is the rail's own width, and the sentence is wider than that in 11 px type: the note wraps to two lines (13 or 14 px each)
        CHECKV(dh >= 143 && dh <= 151, "rail form is %.1f high (4 x 24 + 4 x 6 + a two-line note)", dh);
        CHECKV(dSaveTop > dNoteTop + 8 && std::abs((kDy + dh) - dSaveBottom) <= 1.0, "save key spans rows %d..%d, the bar ends at %.1f", dSaveTop, dSaveBottom, kDy + dh);
        // two wide keys, edge to edge of the 296 px column
        for (const int top : {dY1, dY2}) {
            const int ym = top + kKeyH / 2;
            CHECK(f0.near(kDx, ym, keyLine) && f0.near(kDx + kDw - 1, ym, keyLine));
            CHECK(f0.near(kDx - 1, ym, panelAlt) && f0.near(kDx + kDw, ym, panelAlt));
            CHECK(f0.near(kDx + 8, top, keyLine) && f0.near(kDx + 8, top + kKeyH - 1, keyLine));
            CHECK(f0.near(kDx + 8, top - 1, panelAlt) && f0.near(kDx + 8, top + kKeyH, panelAlt));
            CHECK(f0.near(kDx + 4, ym, keyBg));
            CHECKV(f0.countInk(kDx + 40, top + 3, kDx + kDw - 40, top + kKeyH - 3, keyBg) >= 40, "wide key label ink");
        }
        // the mine row: the key takes the width the x key leaves, 4 px apart; the x key is a 24 px square ghost
        {
            const int ym = dY3 + kKeyH / 2, xl = kDx + kDw - kKeyH, mainR = xl - 4;
            CHECK(f0.near(kDx, ym, keyLine));
            CHECKV(f0.near(mainR - 1, ym, keyLine) && f0.near(mainR, ym, panelAlt), "main key's right edge: %s then %s", hex(f0.at(mainR - 1, ym)).c_str(), hex(f0.at(mainR, ym)).c_str());
            CHECK(f0.rowNear(ym, mainR, xl, panelAlt));                                // the 4 px gap
            CHECKV(f0.near(xl, ym, keyLine) && f0.near(kDx + kDw - 1, ym, keyLine), "x key borders: %s and %s", hex(f0.at(xl, ym)).c_str(), hex(f0.at(kDx + kDw - 1, ym)).c_str());
            CHECK(f0.near(xl + 8, dY3, keyLine) && f0.near(xl + 8, dY3 + kKeyH - 1, keyLine));    // 24 high ...
            CHECK(f0.near(xl + 8, dY3 - 1, panelAlt) && f0.near(xl + 8, dY3 + kKeyH, panelAlt));
            CHECK(f0.near(xl + 4, dY3 + 4, panelAlt));                                 // ... ghost: no ground of its own
            CHECKV(f0.countNear(xl + 4, dY3 + 4, xl + kKeyH - 4, dY3 + kKeyH - 4, fgSoft, 60) >= 6, "the x glyph is drawn in fg-soft");
            CHECK(f0.near(kDx + 4, ym, keyBg));
        }
        // the green result note: chip-good-fg, one line, the row after the mine row
        {
            const Ink n = inkBox(f0, kDx, dNoteTop - 2, kDx + kDw, dSaveTop - 2, panelAlt);
            CHECKV(n.n > 80 && n.y0 >= dNoteTop && n.y0 <= dNoteTop + 4, "note ink n=%d starts at row %d (the line box starts at %d)", n.n, n.y0, dNoteTop);
            CHECKV(f0.countNear(kDx, dNoteTop, kDx + kDw, dSaveTop - 2, goodFg, 40) >= 30, "note colour chip-good-fg: %d", f0.countNear(kDx, dNoteTop, kDx + kDw, dSaveTop - 2, goodFg, 40));
            CHECK(n.x0 <= kDx + 3);
            CHECKV(n.h() >= 22 && n.h() <= 32, "the note is two lines of 11 px type: %d rows of ink", n.h());
        }
        // the save key: a wide plate key - transparent inside, plate-line border, plate-fg label
        {
            const int ym = dSaveTop + kKeyH / 2;
            CHECKV(f0.near(kDx, ym, plateLine) && f0.near(kDx + kDw - 1, ym, plateLine), "save key borders %s / %s", hex(f0.at(kDx, ym)).c_str(), hex(f0.at(kDx + kDw - 1, ym)).c_str());
            CHECK(f0.near(kDx + 8, dSaveTop, plateLine) && f0.near(kDx + 8, dSaveBottom - 1, plateLine));
            CHECK(f0.near(kDx + 4, ym, panelAlt) && f0.near(kDx + 8, dSaveTop + 2, panelAlt));
            CHECKV(f0.countNear(kDx + 10, dSaveTop + 3, kDx + kDw - 10, dSaveBottom - 3, plateFg, 40) >= 25, "save label in plate-fg: %d", f0.countNear(kDx + 10, dSaveTop + 3, kDx + kDw - 10, dSaveBottom - 3, plateFg, 40));
            CHECK(f0.near(kDx + 8, dSaveTop - 1, panelAlt));                           // the gap above, below the note
        }
    }

    // =========================================================================================================
    // 6. PlateKeys: a grid of equal cells 6 px apart, keys 24 high, transparent with a plate-line border and plate-fg labels
    // =========================================================================================================
    {
        CHECK_NEAR(ui->get_e_height(), 54.0, 0.01);
        CHECK_NEAR(ui->get_f_height(), 54.0, 0.01);
        // E: per-row 4, cells (498 - 3 x 6) / 4 = 120 wide
        const int cell = 120, pitch = cell + kGap;
        for (int i = 0; i < 4; ++i) {
            const int x = kBarX + i * pitch, ym = kEy + kKeyH / 2;
            CHECKV(f0.near(x, ym, plateLine) && f0.near(x + cell - 1, ym, plateLine), "E key %d borders at %d and %d: %s / %s", i, x, x + cell - 1, hex(f0.at(x, ym)).c_str(), hex(f0.at(x + cell - 1, ym)).c_str());
            CHECK(f0.near(x + 10, kEy, plateLine) && f0.near(x + 10, kEy + kKeyH - 1, plateLine));
            CHECK(f0.near(x + 10, kEy - 1, bg) && f0.near(x + 10, kEy + kKeyH, bg));
            CHECK(f0.near(x + 4, ym, bg));                                              // plate: nothing inside but the label
            if (i > 0) { CHECK(f0.rowNear(ym, x - kGap, x, bg)); }                      // 6 px between the cells
            CHECKV(f0.countNear(x + 8, kEy + 3, x + cell - 8, kEy + kKeyH - 3, plateFg, 40) >= 12, "E key %d label in plate-fg: %d", i, f0.countNear(x + 8, kEy + 3, x + cell - 8, kEy + kKeyH - 3, plateFg, 40));
        }
        // the fifth label wraps to the second row, first cell; the other three cells of that row are empty
        const int y2 = kEy + kKeyH + kGap;
        CHECK(f0.near(kBarX, y2 + kKeyH / 2, plateLine) && f0.near(kBarX + cell - 1, y2 + kKeyH / 2, plateLine));
        CHECK(f0.near(kBarX + 10, y2, plateLine) && f0.near(kBarX + 10, y2 + kKeyH - 1, plateLine));
        CHECKV(f0.countInk(kBarX + pitch, y2, kBarX + 498, y2 + kKeyH, bg) == 0, "the rest of row two is empty: %d ink", f0.countInk(kBarX + pitch, y2, kBarX + 498, y2 + kKeyH, bg));
        CHECK(f0.near(kBarX + 10, y2 - 1, bg));                                         // 6 px between the rows
        CHECK(f0.near(kBarX + 10, y2 - kGap, bg));
        // a label too long for its cell is cut inside the key, not spilled over its neighbour or the gap
        CHECKV(f0.countInk(kBarX + cell, y2 + 3, kBarX + cell + kGap, y2 + kKeyH - 3, bg) == 0, "long label spilled into the gap");
        // F: the default is two to a row; three labels leave the second row's right cell empty, the grid holds
        const int fcell = 117;
        for (int i = 0; i < 2; ++i) {
            const int x = kBarX + i * (fcell + kGap), ym = kFy + kKeyH / 2;
            CHECKV(f0.near(x, ym, plateLine) && f0.near(x + fcell - 1, ym, plateLine), "F key %d at %d", i, x);
        }
        const int fy2 = kFy + kKeyH + kGap;
        CHECK(f0.near(kBarX, fy2 + kKeyH / 2, plateLine) && f0.near(kBarX + fcell - 1, fy2 + kKeyH / 2, plateLine));    // the third key is a cell wide, not the row
        CHECKV(f0.countInk(kBarX + fcell + 1, fy2, kBarX + 240, fy2 + kKeyH, bg) == 0, "the empty cell stays empty");
        // G: disabled
        const Rgb halfPlate = over(plateLine, 0.5, keyDisBg);
        for (int i = 0; i < 2; ++i) {
            const int x = kGx + i * (fcell + kGap), ym = kFy + kKeyH / 2;
            CHECKV(f0.near(x + 4, ym, keyDisBg), "G key %d ground %s", i, hex(f0.at(x + 4, ym)).c_str());
            CHECKV(f0.near(x + 10, kFy, halfPlate, 8), "G key %d border %s, wanted about %s", i, hex(f0.at(x + 10, kFy)).c_str(), hex(halfPlate).c_str());
            CHECK(f0.countNear(x + 8, kFy + 3, x + fcell - 8, kFy + kKeyH - 3, keyDisFg, 24) >= 8);
            CHECK(f0.countNear(x + 8, kFy + 3, x + fcell - 8, kFy + kKeyH - 3, plateFg, 12) == 0);
        }
        CHECK(f0.countInk(kGx, kFy + kKeyH + 1, kGx + 240, kFy + 54, bg) == 0);          // two labels, one row
    }

    // =========================================================================================================
    // 7. Interaction: pressed(i), the lit key still presses, + Save is its own callback
    // =========================================================================================================
    {
        reset();
        clk(spanAt(keysA, 0).mid(), aMid);
        CHECKV(ui->get_pressed_count() == 1 && ui->get_last_pressed() == 0 && sval(ui->get_last_source()) == "A",
               "key 0: %d presses, last %d, source '%s'", ui->get_pressed_count(), ui->get_last_pressed(), sval(ui->get_last_source()).c_str());
        clk(spanAt(keysA, 1).mid(), aMid);
        CHECK(ui->get_pressed_count() == 2 && ui->get_last_pressed() == 1);              // the lit key presses too
        clk(spanAt(keysA, 2).mid(), aMid);
        CHECK(ui->get_pressed_count() == 3 && ui->get_last_pressed() == 2 && ui->get_forgot_count() == 0);    // a left click on a mine key is a press, not a forget
        CHECK(ui->get_save_count() == 0);
        clk(spanAt(keysA, 2).r + kGap + 25, aMid);
        CHECKV(ui->get_save_count() == 1 && sval(ui->get_last_source()) == "A" && ui->get_pressed_count() == 3, "+ Save: %d saves, %d presses", ui->get_save_count(), ui->get_pressed_count());
        clk(spanAt(keysA, 0).l - 6, aMid);                                               // the padding is not a key
        clk(spanAt(keysA, 0).r + 2, aMid);                                               // nor is the gap
        CHECK(ui->get_pressed_count() == 3 && ui->get_save_count() == 1);
        park();
        // wrapped bar B: second-row keys press with their model index, the + Save key after them
        reset();
        const int ym2 = bTop2 + kKeyH / 2;
        clk(spanAt(keysB2, 0).mid(), ym2);
        CHECKV(ui->get_pressed_count() == 1 && ui->get_last_pressed() == 4 && sval(ui->get_last_source()) == "B", "B row two key 0 is preset 4: %d (source %s)", ui->get_last_pressed(), sval(ui->get_last_source()).c_str());
        clk(spanAt(keysB2, 1).mid(), ym2);
        CHECK(ui->get_last_pressed() == 5 && ui->get_forgot_count() == 0);
        clk(spanAt(keysB2, 1).r + kGap + 25, ym2);
        CHECKV(ui->get_save_count() == 1 && sval(ui->get_last_source()) == "B", "B save: %d", ui->get_save_count());
        clk(spanAt(keysB1, 3).mid(), bTop1 + kKeyH / 2);
        CHECK(ui->get_last_pressed() == 3);                                              // the fourth key of the first row
        rclk(spanAt(keysB2, 1).mid(), ym2);                                              // B's sixth preset is a mine key
        CHECKV(ui->get_forgot_count() == 1 && ui->get_last_forgot() == 5, "B forget: %d (last %d)", ui->get_forgot_count(), ui->get_last_forgot());
        park();
    }

    // =========================================================================================================
    // 8. Forgetting: a right-click on a mine key fires forgot(i); on a plugin's own key it does nothing
    // =========================================================================================================
    {
        reset();
        rclk(spanAt(keysA, 2).mid(), aMid);
        CHECKV(ui->get_forgot_count() == 1 && ui->get_last_forgot() == 2 && sval(ui->get_last_source()) == "A", "right-click on the mine key: %d forgets, last %d", ui->get_forgot_count(), ui->get_last_forgot());
        CHECK(ui->get_pressed_count() == 0);                                             // a right-click presses nothing
        rclk(spanAt(keysA, 0).mid(), aMid);                                              // the plugin's own key is not the operator's to remove
        rclk(spanAt(keysA, 1).mid(), aMid);
        CHECKV(ui->get_forgot_count() == 1, "right-click on a plugin key forgot (%d)", ui->get_forgot_count());
        CHECK(ui->get_pressed_count() == 0 && ui->get_save_count() == 0);
        park();
    }

    // =========================================================================================================
    // 9. A disabled bar fires nothing
    // =========================================================================================================
    {
        reset();
        const int ym = kCy + kPadY + kKeyH / 2;
        for (const int x : {kBarX + kPadX + 30, kBarX + kPadX + 150, kBarX + kPadX + 260, kBarX + kPadX + 370}) { clk(x, ym); }
        rclk(kBarX + kPadX + 260, ym);
        CHECKV(ui->get_pressed_count() == 0 && ui->get_forgot_count() == 0 && ui->get_save_count() == 0, "disabled bar fired %d presses, %d forgets, %d saves", ui->get_pressed_count(), ui->get_forgot_count(), ui->get_save_count());
        park();
    }

    // =========================================================================================================
    // 10. The column form's callbacks: a wide key presses, the x key and a right-click forget, the plate key saves
    // =========================================================================================================
    {
        reset();
        const int ymain = dY1 + kKeyH / 2, y2 = dY2 + kKeyH / 2, y3 = dY3 + kKeyH / 2;
        clk(kDx + 100, ymain);
        CHECKV(ui->get_pressed_count() == 1 && ui->get_last_pressed() == 0 && sval(ui->get_last_source()) == "D", "D wide key 0: %d", ui->get_last_pressed());
        clk(kDx + 150, y2);
        CHECK(ui->get_last_pressed() == 1);
        clk(kDx + 100, y3);
        CHECKV(ui->get_last_pressed() == 2 && ui->get_pressed_count() == 3 && ui->get_forgot_count() == 0, "D mine key presses: %d presses, %d forgets", ui->get_pressed_count(), ui->get_forgot_count());
        clk(kDx + kDw - 12, y3);                                                           // the x key beside it
        CHECKV(ui->get_forgot_count() == 1 && ui->get_last_forgot() == 2 && ui->get_pressed_count() == 3, "x key: %d forgets (last %d), %d presses", ui->get_forgot_count(), ui->get_last_forgot(), ui->get_pressed_count());
        rclk(kDx + 100, y3);                                                               // a right-click on the mine key forgets as well
        CHECK(ui->get_forgot_count() == 2 && ui->get_last_forgot() == 2);
        rclk(kDx + 100, ymain);                                                            // but not on the plugin's own
        CHECK(ui->get_forgot_count() == 2);
        clk(kDx + kDw / 2, dSaveTop + kKeyH / 2);
        CHECKV(ui->get_save_count() == 1 && sval(ui->get_last_source()) == "D", "D plate save: %d", ui->get_save_count());
        clk(kDx + 100, dSaveTop - 3);                                                      // the gap above the save key
        CHECK(ui->get_save_count() == 1 && ui->get_pressed_count() == 3);
        park();
    }

    // =========================================================================================================
    // 11. PlateKeys: pressed(i) with the key's index, nothing from the empty cell or the disabled grid
    // =========================================================================================================
    {
        reset();
        const int cell = 120, pitch = cell + kGap, fcell = 117;
        clk(kBarX + 2 * pitch + 60, kEy + kKeyH / 2);
        CHECKV(ui->get_plate_count() == 1 && ui->get_last_plate() == 2 && sval(ui->get_last_source()) == "E", "E key 2: %d (source %s)", ui->get_last_plate(), sval(ui->get_last_source()).c_str());
        clk(kBarX + 60, kEy + kKeyH + kGap + kKeyH / 2);
        CHECK(ui->get_plate_count() == 2 && ui->get_last_plate() == 4);                    // the fifth label is index 4
        clk(kBarX + pitch + 60, kEy + kKeyH + kGap + kKeyH / 2);                           // an empty cell
        CHECK(ui->get_plate_count() == 2);
        clk(kBarX + fcell + kGap + 50, kFy + kKeyH / 2);
        CHECK(ui->get_plate_count() == 3 && ui->get_last_plate() == 1 && sval(ui->get_last_source()) == "F");
        clk(kBarX + 50, kFy + kKeyH + kGap + kKeyH / 2);
        CHECK(ui->get_plate_count() == 4 && ui->get_last_plate() == 2);
        clk(kBarX + fcell + kGap + 50, kFy + kKeyH + kGap + kKeyH / 2);                    // F's empty cell
        CHECK(ui->get_plate_count() == 4);
        clk(kGx + 50, kFy + kKeyH / 2);                                                    // G is disabled
        clk(kGx + fcell + kGap + 50, kFy + kKeyH / 2);
        CHECKV(ui->get_plate_count() == 4, "disabled plate keys fired (%d)", ui->get_plate_count());
        park();
    }

    // =========================================================================================================
    // 11b. PlateKeys holds at most sixteen: a seventeenth label is not drawn, and has no key to press
    // =========================================================================================================
    {
        reset();
        std::vector<slint::SharedString> names;
        for (int i = 0; i < 18; ++i) { names.emplace_back("KEY " + std::to_string(i)); }
        ui->set_labels_e(std::make_shared<slint::VectorModel<slint::SharedString>>(names));
        const Frame many = render(ui, kW, kH);
        CHECKV(std::abs(ui->get_e_height() - (4 * kKeyH + 3 * kGap)) < 0.01, "18 labels make a grid %.1f high (four rows of 24, 6 apart: %d)", ui->get_e_height(), 4 * kKeyH + 3 * kGap);
        const int cell = 120, pitch = cell + kGap, lastRow = kEy + 3 * (kKeyH + kGap);
        CHECK(many.near(kBarX + 3 * pitch, lastRow + 12, plateLine) && many.near(kBarX + 3 * pitch + cell - 1, lastRow + 12, plateLine));       // the sixteenth key is the last of the fourth row
        CHECKV(many.countInk(kBarX, kEy + 4 * (kKeyH + kGap) + 5, kBarX + 498, kEy + 4 * (kKeyH + kGap) + 24, bg) == 0, "a fifth row was drawn");     // (under the F and G keys, which end 4 rows down)
        clk(kBarX + 3 * pitch + 60, lastRow + 12);
        CHECKV(ui->get_plate_count() == 1 && ui->get_last_plate() == 15, "the sixteenth key: index %d", ui->get_last_plate());
        clk(kBarX + 60, kEy + 4 * (kKeyH + kGap) + 12);                                                    // where a seventeenth would be
        CHECK(ui->get_plate_count() == 1);
        ui->set_labels_e(std::make_shared<slint::VectorModel<slint::SharedString>>(std::vector<slint::SharedString>{"RESET", "CLEAR LOG", "SEND BEACON", "SAVE", "LONG RUNNING COMMAND"}));
        render(ui, kW, kH);
        CHECK_NEAR(ui->get_e_height(), 54.0, 0.01);
        park();
    }

    // =========================================================================================================
    // 12. The keyboard: Tab reaches the first key of the sheet and Space presses it
    // =========================================================================================================
    {
        reset();
        keyTap(win, slint::platform::key_codes::Tab);
        keyTap(win, u8" ");
        CHECKV(ui->get_pressed_count() == 1 && ui->get_last_pressed() == 0 && sval(ui->get_last_source()) == "A", "Tab, Space: %d presses, last %d, source '%s'", ui->get_pressed_count(), ui->get_last_pressed(), sval(ui->get_last_source()).c_str());
        Frame fr = render(ui, kW, kH);
        CHECKV(fr.near(spanAt(keysA, 0).l - 2, aMid, tok(wb.focus), 14), "focus ring round the first key: %s", hex(fr.at(spanAt(keysA, 0).l - 2, aMid)).c_str());
    }

    // =========================================================================================================
    // 13. Hover: a key under the pointer takes the hot ground; a lit key goes to the hover accent
    // =========================================================================================================
    {
        mv(spanAt(keysA, 0).mid(), aMid);
        Frame h0 = render(ui, kW, kH);
        CHECKV(h0.near(spanAt(keysA, 0).l + 4, aKeyTop + 3, menuHot), "hovered key ground %s", hex(h0.at(spanAt(keysA, 0).l + 4, aKeyTop + 3)).c_str());
        CHECK(h0.near(spanAt(keysA, 2).l + 4, aKeyTop + 3, keyBg));                        // the others stay put
        mv(spanAt(keysA, 1).mid(), aMid);
        Frame h1 = render(ui, kW, kH);
        CHECKV(h1.near(spanAt(keysA, 1).l + 4, aKeyTop + 3, accentHover), "hovered lit key ground %s", hex(h1.at(spanAt(keysA, 1).l + 4, aKeyTop + 3)).c_str());
        // plate keys too: a hovered plate key takes the hot ground
        mv(kBarX + 60, kEy + kKeyH / 2);
        Frame hp = render(ui, kW, kH);
        CHECK(hp.near(kBarX + 4, kEy + 3, menuHot));
        park();
    }

    // =========================================================================================================
    // 14. Tooltips: each preset key carries its own sentence, 6 px under the key; the + Save key carries the bar's
    // =========================================================================================================
    {
        const int tipY = aKeyTop + kKeyH + kGap + 4;
        Frame none = render(ui, kW, kH);
        CHECK(none.near(spanAt(keysA, 1).l + 5, tipY, bg));                                // nothing under it yet
        mv(spanAt(keysA, 1).mid(), aMid);
        settle(milliseconds(750));
        Frame tip = render(ui, kW, kH);
        CHECKV(tip.near(spanAt(keysA, 1).l + 5, tipY + 2, menu), "tooltip ground under the lit key: %s", hex(tip.at(spanAt(keysA, 1).l + 5, tipY + 2)).c_str());
        CHECK(tip.near(spanAt(keysA, 1).l + 5, aKeyTop + kKeyH + kGap, knobRing));         // the tip's border, 6 px under the key
        park();
        Frame gone = render(ui, kW, kH);
        CHECK(gone.near(spanAt(keysA, 1).l + 5, tipY + 2, bg));
        // the + Save key of the row form
        const int sl = spanAt(keysA, 2).r + kGap;
        mv(sl + 20, aMid);
        settle(milliseconds(750));
        CHECKV(render(ui, kW, kH).near(sl + 5, tipY + 2, menu), "save key tooltip");
        park();
        // the rail form's save key
        mv(kDx + 100, dSaveTop + kKeyH / 2);
        settle(milliseconds(750));
        const Frame rail = render(ui, kW, kH);
        CHECKV(rail.near(kDx + 5, dSaveBottom + kGap + 3, menu), "rail save key tooltip: %s", hex(rail.at(kDx + 5, dSaveBottom + kGap + 3)).c_str());
        park();
        // a key with no sentence opens none (the second wide key has an empty tooltip): where a tip's border (knob-ring) would lie,
        // 6 px under the key, the mine key's own border (key-line) is
        mv(kDx + 100, dY2 + kKeyH / 2);
        settle(milliseconds(750));
        const Frame quiet = render(ui, kW, kH);
        CHECKV(quiet.near(kDx + 20, dY2 + kKeyH + kGap, keyLine, 3) && !quiet.near(kDx + 20, dY2 + kKeyH + kGap, knobRing, 3), "no tip: %s", hex(quiet.at(kDx + 20, dY2 + kKeyH + kGap)).c_str());
        park();
        // and one that has a sentence opens it: the first wide key's tip border lies where the second key's top border would
        mv(kDx + 100, dY1 + kKeyH / 2);
        settle(milliseconds(750));
        const Frame loud = render(ui, kW, kH);
        CHECKV(loud.near(kDx + 20, dY1 + kKeyH + kGap, knobRing, 3), "tip border under the first wide key: %s", hex(loud.at(kDx + 20, dY1 + kKeyH + kGap)).c_str());
        park();
        // the x key explains itself
        mv(kDx + kDw - 12, dY3 + kKeyH / 2);
        settle(milliseconds(750));
        const Frame xt = render(ui, kW, kH);
        CHECKV(xt.near(kDx + kDw - 20, dY3 + kKeyH + kGap + 3, menu), "x key tooltip: %s", hex(xt.at(kDx + kDw - 20, dY3 + kKeyH + kGap + 3)).c_str());
        park();
    }

    // =========================================================================================================
    // 15. The model is live: a shorter list moves the + Save key up, an empty one leaves + Save alone, and the note comes and goes
    // =========================================================================================================
    {
        ui->set_keys_a(std::make_shared<slint::VectorModel<PresetKeyModel>>(std::vector<PresetKeyModel>{
            PresetKeyModel{"145.800 APRS", false, false, ""}}));
        Frame a = render(ui, kW, kH);
        const std::vector<Span> runs = runsOnRow(a, aMid, kBarX + 1, kBarX + kBarW - 1, panel);
        CHECKV(!runs.empty() && a.near(spanAt(runs, 0).r + kGap, aMid, keyLine), "one preset: the save key follows at %d", spanAt(runs, 0).r + kGap);
        ui->set_keys_a(std::make_shared<slint::VectorModel<PresetKeyModel>>(std::vector<PresetKeyModel>{}));
        Frame e = render(ui, kW, kH);
        CHECK(e.near(kBarX + kPadX, aMid, keyLine));                                        // the + Save key alone, at the left padding
        CHECK(e.countInk(kBarX + kPadX + 130, kAy + 2, kBarX + kBarW - 2, kAy + 36, panel) == 0);
        CHECK_NEAR(ui->get_a_height(), 37.0, 0.01);
        // the rail form: no note, no line for it - the save key moves up by the note's height and the gap
        const double withNote = ui->get_d_height();
        ui->set_note_d("");
        render(ui, kW, kH);
        CHECKV(ui->get_d_height() < withNote - 12, "without the note the rail form is %.1f high (was %.1f)", ui->get_d_height(), withNote);
        ui->set_note_d("Saved 145.5000 MHz as a preset for APRS / AX.25 Packet Decoder");
        render(ui, kW, kH);
        CHECK_NEAR(ui->get_d_height(), withNote, 0.01);
    }

    // =========================================================================================================
    // 16. The skin: a second filling reaches the keys with no component change
    // =========================================================================================================
    {
        ui->set_keys_a(std::make_shared<slint::VectorModel<PresetKeyModel>>(std::vector<PresetKeyModel>{
            PresetKeyModel{"145.800 APRS", false, false, ""}, PresetKeyModel{"144.390 NA", true, false, ""}}));
        Skin probe = ui->global<Workbench>().get_value();
        probe.accent = slint::Color::from_rgb_uint8(0x00, 0xc0, 0x40);
        probe.key_bg = slint::Color::from_rgb_uint8(0x20, 0x30, 0x10);
        probe.key_line = slint::Color::from_rgb_uint8(0xff, 0x00, 0xff);
        probe.panel = slint::Color::from_rgb_uint8(0x10, 0x10, 0x40);
        probe.plate_line = slint::Color::from_rgb_uint8(0x00, 0xff, 0xff);
        probe.chip_good_fg = slint::Color::from_rgb_uint8(0xff, 0xff, 0x00);
        ui->global<Theme>().set_skin(probe);
        Frame g = render(ui, kW, kH);
        CHECK(g.near(kBarX + 4, aKeyTop + 3, rgb(0x101040)));                                // the window's ground
        CHECK(g.near(kBarX + kPadX + 3, aMid, rgb(0x203010)));                              // the plain key's ground
        CHECK(g.near(kBarX + kPadX + 8, aKeyTop, rgb(0xff00ff)));                           // its border
        const std::vector<Span> runs = runsOnRow(g, aMid, kBarX + 1, kBarX + kBarW - 1, rgb(0x101040));
        CHECKV(runs.size() >= 2 && g.near(spanAt(runs, 1).l + 3, aMid, rgb(0x00c040)), "lit key takes the skin's accent");
        CHECK(g.near(kBarX + 10, kEy, rgb(0x00ffff)));                                      // the plate keys' border
        CHECK(g.countNear(kDx, dNoteTop, kDx + kDw, dNoteTop + 22, rgb(0xffff00), 40) >= 30);       // the note
        ui->global<Theme>().set_skin(ui->global<Workbench>().get_value());
        Frame h = render(ui, kW, kH);
        CHECK(h.near(kBarX + kPadX + 3, aMid, keyBg));                                      // and the Workbench filling comes back
    }

    return testSummary("test_slint_presets");
}
