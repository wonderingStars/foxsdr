// The CARDS family (docs 6.35): Card (the update, Store and team-message cards of the sections panel's foot, the warn and
// err tones, a download bar, bullets, a disabled primary) and Banner (the slim sound-muted bar), rendered headlessly from
// ui/specimens/cards_specimen.slint and probed against Workbench.dc.html's update card (margin 10, padding 10, radius 8,
// 6 px gaps, 26 px keys), Workbench-Menus.dc.html's foot cards, Workbench-PluginKit.dc.html's .card / .card.wa / .card.er and
// the contract in COMPONENTS.md.
//
// Text is in the embedded Inter (third_party/fonts, the same on every machine), so text checks assert INK in a region and its
// COLOUR, never a glyph's pixels or a width, and the pitch between lines of text is measured on baselines (the first row of a
// line's ink moves with the glyphs the line holds); the cards, keys, bars and borders are drawn geometry and are asserted to
// the pixel. Heights come from the specimen's out properties and every key, bar and line is found by its colour on the
// rendered sheet, so the checks follow whatever line height the font gives.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "cards_specimen.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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

// The bounding box and count of the ink (pixels differing from `bg`) in a rectangle.
struct Ink {
    int n = 0, x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
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

// The first row in [y0, y1) whose pixels [x0, x1) are all `want`: the top edge of a key or a bar. -1 when none.
int firstRunRow(const Frame& f, int y0, int y1, int x0, int x1, Rgb want, int tol = 3) {
    for (int y = y0; y < y1; ++y) { if (f.rowNear(y, x0, x1, want, tol)) { return y; } }
    return -1;
}
// The first run of at least `minLen` consecutive rows on column x, in [y0, y1), near `want`: where a key's or a box's
// ground runs down a column that holds no text. top / bot are -1 when there is none.
struct Run { int top = -1, bot = -1; };
Run colRun(const Frame& f, int x, int y0, int y1, Rgb want, int tol, int minLen) {
    for (int y = y0; y < y1; ++y) {
        if (!f.near(x, y, want, tol)) { continue; }
        int e = y;
        while (e + 1 < y1 && f.near(x, e + 1, want, tol)) { ++e; }
        if (e - y + 1 >= minLen) { return {y, e}; }
        y = e;
    }
    return {};
}
// The first and last row of each band of rows that hold ink (a pixel that is not `ground`) inside [x0, x1): the lines of a
// block of text.
struct RowSpan { int top = -1, bot = -1; };
std::vector<RowSpan> inkSpans(const Frame& f, int x0, int x1, int y0, int y1, Rgb ground, int tol = 24) {
    std::vector<RowSpan> spans;
    bool in = false;
    for (int y = y0; y < y1; ++y) {
        bool any = false;
        for (int x = x0; x < x1 && !any; ++x) { any = !f.near(x, y, ground, tol); }
        if (any && !in) { spans.push_back({y, y}); }
        if (any) { spans.back().bot = y; }
        in = any;
    }
    return spans;
}
// The baseline of the line of text that fills rows [s.top, s.bot]: the last row that still holds at least half the ink of the
// line's densest row. Lower-case text keeps most of its ink between the x-height and the baseline, so what a particular line
// happens to contain (an ascender, the dot of an i, a descender) cannot move it, as it moves the line's first or last row of
// ink: the pitch between two lines is a layout fact only when it is measured on baselines.
int baselineRow(const Frame& f, int x0, int x1, const RowSpan& s, Rgb ground, int tol = 24) {
    std::vector<int> n;
    int peak = 0;
    for (int y = s.top; y <= s.bot; ++y) {
        int c = 0;
        for (int x = x0; x < x1; ++x) { if (!f.near(x, y, ground, tol)) { ++c; } }
        n.push_back(c);
        peak = std::max(peak, c);
    }
    int base = s.top;
    for (int i = 0; i < static_cast<int>(n.size()); ++i) { if (2 * n[i] >= peak) { base = s.top + i; } }
    return base;
}
// The last row in [y0, y1) with a pixel near `want` inside [x0, x1); -1 when none.
int lastRowNear(const Frame& f, int x0, int x1, int y0, int y1, Rgb want, int tol) {
    for (int y = y1 - 1; y >= y0; --y) {
        for (int x = x0; x < x1; ++x) { if (f.near(x, y, want, tol)) { return y; } }
    }
    return -1;
}
// The download bar on a key row's middle row `mid`: found by the first column whose six rows are all fill or track (text
// cannot make that), walked left to the round cap's tip, then the fill run and the track run after it.
struct Bar { int x0 = -1, fillEnd = -1, end = -1; int fill() const { return fillEnd - x0 + 1; } int total() const { return end - x0 + 1; } };
Bar measureBar(const Frame& f, int mid, int xFrom, int xTo, Rgb fillCol, Rgb trackCol) {
    Bar b;
    const auto either = [&](int x, int y) { return f.near(x, y, fillCol, 8) || f.near(x, y, trackCol, 3); };
    const auto full = [&](int x) {
        for (int y = mid - 3; y < mid + 3; ++y) { if (!either(x, y)) { return false; } }
        return true;
    };
    int first = -1;
    for (int x = xFrom; x + 12 < xTo && first < 0; ++x) {
        bool run = true;
        for (int k = 0; k < 12 && run; ++k) { run = full(x + k); }
        if (run) { first = x; }
    }
    if (first < 0) { return b; }
    b.x0 = first;
    while (b.x0 > xFrom && either(b.x0 - 1, mid)) { --b.x0; }
    int x = b.x0;
    while (f.near(x, mid, fillCol, 8)) { ++x; }
    b.fillEnd = x - 1;
    for (int k = 0; k < 3 && !f.near(x, mid, trackCol, 4); ++k) { ++x; }          // the round end blends into the track for a pixel
    while (f.near(x, mid, trackCol, 4)) { ++x; }
    if (x - 1 > b.fillEnd + 1) { b.end = x - 1; } else { b.end = b.fillEnd; }       // no track after it: the bar is full
    return b;
}

// How many pixels of the w x h block at (x, y) are near `want`.
int blockNear(const Frame& f, int x, int y, int w, int h, Rgb want, int tol) { return f.countNear(x, y, x + w, y + h, want, tol); }

struct Box { int x, y, w, h; };
constexpr int kW = 640, kH = 764;
constexpr int kPad = 11;                                  // 1 border + 10 padding
constexpr int kKeyH = 26;                                 // the sheet's inline key height
constexpr int kGapPx = 6;                                 // a card's gap, and the gap between its two keys

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = CardsSpecimen::create();
    slint::Window& win = ui->window();

    // The tokens, read from the Workbench skin by name (the theme test pins their values).
    const Skin wb = ui->global<Workbench>().get_value();
    const Rgb bg = tok(wb.bg), card = tok(wb.card), lineStrong = tok(wb.line_strong), fg = tok(wb.fg), fgSoft = tok(wb.fg_soft), dim = tok(wb.dim);
    const Rgb accent = tok(wb.accent), accentInk = tok(wb.accent_ink), accentSoft = tok(wb.accent_soft);
    const Rgb cardWarn = tok(wb.card_warn), cardWarnLine = tok(wb.card_warn_line), cardErr = tok(wb.card_err), cardErrLine = tok(wb.card_err_line);
    const Rgb errFg = tok(wb.chip_err_fg), keyBg = tok(wb.key_bg), keyLine = tok(wb.key_line), keyFg = tok(wb.key_fg);
    const Rgb keyDisBg = tok(wb.key_disabled_bg), keyDisFg = tok(wb.key_disabled_fg), menu = tok(wb.menu), menuHot = tok(wb.menu_hot);
    const Rgb sliderTrack = tok(wb.slider_track), sliderFill = tok(wb.slider_fill), field = tok(wb.field);

    // pointer positions are whole pixels here; the harness takes floats
    const auto clk = [&](int x, int y) { click(win, static_cast<float>(x), static_cast<float>(y)); };
    const auto mv = [&](int x, int y) { moveTo(win, static_cast<float>(x), static_cast<float>(y)); };
    const auto reset = [&] {
        ui->set_primaries(0); ui->set_secondaries(0); ui->set_banner_clicks(0); ui->set_toggles(0); ui->set_last_clicked("");
    };
    // Parks the pointer on the sheet's empty corner: closes any tooltip and ends any hover.
    const auto park = [&] { mv(636, 760); settle(milliseconds(30)); };
    const auto sval = [&](const slint::SharedString& s) { return std::string(s); };

    // =========================================================================================================
    // 1. The sheet, before anything is touched
    // =========================================================================================================
    Frame f0 = render(ui, kW, kH);
    CHECK(f0.width == kW && f0.height == kH);
    CHECK(f0.near(638, 762, bg));                                                   // the sheet's ground
    f0.savePng(shotDir() + "cards-specimen.png");

    const Box upd{16, 16, 276, (int)ui->get_upd_h()}, notes{16, 112, 276, (int)ui->get_notes_h()}, warn{16, 296, 276, (int)ui->get_warn_h()},
        team{16, 480, 276, (int)ui->get_team_h()}, prog{348, 16, 276, (int)ui->get_prog_h()}, err{348, 168, 276, (int)ui->get_err_h()},
        dis{348, 332, 276, (int)ui->get_dis_h()}, store{348, 422, 276, (int)ui->get_store_h()}, longKey{348, 556, 276, (int)ui->get_long_h()};
    // a card sizes itself from its rows: more rows, more height; the host reads preferred-height to make room for it
    CHECKV(upd.h >= 2 * kPad + kKeyH + kGapPx + 10 && upd.h <= 2 * kPad + kKeyH + kGapPx + 50, "update card height %d", upd.h);
    CHECKV(notes.h > upd.h + 40 && warn.h > upd.h + 40 && team.h > upd.h + 20 && store.h > dis.h + 20, "heights: upd %d notes %d warn %d team %d dis %d store %d", upd.h, notes.h, warn.h, team.h, dis.h, store.h);
    CHECKV(ui->get_upd_preferred_h() == ui->get_upd_h() && ui->get_notes_preferred_h() == ui->get_notes_h(), "preferred-height %.1f vs height %.1f", ui->get_upd_preferred_h(), ui->get_upd_h());
    // and no card is cut short: nothing is drawn under any card's bottom edge, nor inside its bottom padding
    const auto checkBox = [&](const char* name, const Box& c, Rgb ground, Rgb border) {
        CHECKV(f0.rowNear(c.y, c.x + 10, c.x + c.w - 10, border), "%s: top border row %d is %s", name, c.y, hex(f0.at(c.x + 40, c.y)).c_str());
        CHECKV(f0.rowNear(c.y + c.h - 1, c.x + 10, c.x + c.w - 10, border), "%s: bottom border row %d is %s", name, c.y + c.h - 1, hex(f0.at(c.x + 40, c.y + c.h - 1)).c_str());
        CHECKV(f0.colNear(c.x, c.y + 10, c.y + c.h - 10, border), "%s: left border", name);
        CHECKV(f0.colNear(c.x + c.w - 1, c.y + 10, c.y + c.h - 10, border), "%s: right border", name);
        CHECKV(f0.rowNear(c.y - 1, c.x + 10, c.x + c.w - 10, bg) && f0.rowNear(c.y + c.h, c.x + 10, c.x + c.w - 10, bg), "%s: ground just outside, so exactly %d px high", name, c.h);
        CHECKV(f0.near(c.x + 5, c.y + c.h / 2, ground), "%s: ground inside is %s", name, hex(f0.at(c.x + 5, c.y + c.h / 2)).c_str());
        CHECKV(f0.near(c.x, c.y, bg, 14) && f0.near(c.x + c.w - 1, c.y + c.h - 1, bg, 14), "%s: 8 px corners are cut: %s / %s", name, hex(f0.at(c.x, c.y)).c_str(), hex(f0.at(c.x + c.w - 1, c.y + c.h - 1)).c_str());
        CHECKV(f0.countInk(c.x, c.y + c.h, c.x + c.w, c.y + c.h + 8, bg) == 0, "%s: ink under the card", name);
        CHECKV(f0.countInk(c.x + c.w, c.y, c.x + c.w + 8, c.y + c.h, bg) == 0, "%s: ink beside the card", name);
    };
    checkBox("update", upd, card, lineStrong);
    checkBox("notes", notes, card, lineStrong);
    checkBox("warn", warn, cardWarn, cardWarnLine);
    checkBox("team", team, card, lineStrong);
    checkBox("progress", prog, card, lineStrong);
    checkBox("err", err, cardErr, cardErrLine);
    checkBox("disabled", dis, card, lineStrong);
    checkBox("store", store, card, lineStrong);
    checkBox("long key", longKey, card, lineStrong);

    // =========================================================================================================
    // 2. The update card as the sheet draws it: title and subtitle, then the two keys on one row
    // =========================================================================================================
    int updKeyTop = 0, updPriW = 0, updSecLeft = 0, updSecW = 0;
    {
        const Box& c = upd;
        const int cx = c.x + kPad, inner = c.w - 2 * kPad;                            // the content box: 254 wide
        // the primary: an `on` key, 26 px high, on the accent, from the content edge; found down a column that holds no label
        const Run key = colRun(f0, cx + 8, c.y + kPad, c.y + c.h - 2, accent, 3, 10);
        updKeyTop = key.top;
        CHECKV(key.top > c.y + kPad + 8, "primary key found at row %d", key.top);
        CHECKV(key.bot - key.top + 1 == kKeyH, "primary key is %d px high", key.bot - key.top + 1);
        CHECKV(key.bot + kPad == c.y + c.h - 1, "10 px of padding under the key: key ends %d, card ends %d", key.bot, c.y + c.h - 1);
        CHECK(f0.near(cx - 1, key.top + 13, card) && f0.colNear(cx, key.top + 6, key.top + 20, accent));   // starts at the content edge
        CHECK(f0.countNear(cx + 4, key.top + 2, cx + 160, key.top + 24, accentInk, 40) >= 15);              // its label in accent-ink
        // the title is the cap in accent-soft, from the content's corner; it wraps in what the subtitle leaves
        const int titleEnd = lastRowNear(f0, cx, cx + 170, c.y + kPad, key.top - 1, accentSoft, 50);
        const Ink title = inkBox(f0, cx, c.y + kPad, cx + 170, titleEnd + 1, card);
        CHECKV(title.n > 60 && title.x0 >= cx && title.x0 <= cx + 2 && title.y0 >= c.y + kPad && title.y0 <= c.y + kPad + 3,
               "title ink n=%d from (%d,%d), content corner (%d,%d)", title.n, title.x0, title.y0, cx, c.y + kPad);
        CHECKV(f0.countNear(cx, c.y + kPad, cx + 170, titleEnd + 1, accentSoft, 40) >= 25, "title colour accent-soft: %d", f0.countNear(cx, c.y + kPad, cx + 170, titleEnd + 1, accentSoft, 40));
        CHECK(f0.countNear(cx, c.y + kPad, cx + 170, titleEnd + 1, errFg, 30) == 0);      // not critical: no red
        CHECKV(key.top - (title.y1 + 1) >= kGapPx - 1, "gap between the title and the keys: %d", key.top - (title.y1 + 1));
        // the subtitle: 10 px dim, flush to the right of the content box, level with the first line of the title
        const Ink sub = inkBox(f0, cx + inner - 110, c.y + kPad, cx + inner, c.y + kPad + 14, card);
        CHECKV(sub.n > 25 && sub.x1 >= cx + inner - 4 && sub.x1 <= cx + inner - 1, "subtitle ink n=%d right edge %d (content edge %d)", sub.n, sub.x1, cx + inner);
        CHECKV(f0.countNear(cx + inner - 110, c.y + kPad, cx + inner, c.y + kPad + 14, dim, 40) >= 8, "subtitle colour dim: %d", f0.countNear(cx + inner - 110, c.y + kPad, cx + inner, c.y + kPad + 14, dim, 40));
        CHECKV(std::abs(sub.y0 - title.y0) <= 3, "subtitle starts at row %d, title at %d", sub.y0, title.y0);
        // the primary fills the row up to the secondary
        // the secondary: ghost, 26 px high, at least 72 wide, ending at the content edge; 6 px between the keys
        int x = cx + inner - 2;
        while (f0.near(x, key.top + 6, card, 3)) { --x; }
        updSecLeft = x;
        updSecW = cx + inner - updSecLeft;
        x = updSecLeft - 1;
        while (f0.near(x, key.top + 6, card, 3)) { --x; }                               // across the gap, to the primary's last column
        updPriW = x - cx + 1;
        CHECKV(updSecLeft - 1 - x == kGapPx, "the gap between the keys is %d px", updSecLeft - 1 - x);
        CHECKV(updSecW >= 72, "secondary key is %d px wide", updSecW);
        CHECKV(updPriW + kGapPx + updSecW == inner, "primary %d + gap %d + secondary %d = %d, wanted %d", updPriW, kGapPx, updSecW, updPriW + kGapPx + updSecW, inner);
        CHECK(f0.rowNear(key.top + 4, cx + updPriW, cx + updPriW + kGapPx, card));      // the 6 px gap is the card's ground
        CHECK(f0.near(updSecLeft, key.top + 13, keyLine) && f0.near(cx + inner - 1, key.top + 13, keyLine));   // key-line borders
        CHECK(f0.rowNear(key.top, updSecLeft + 8, cx + inner - 8, keyLine) && f0.rowNear(key.top + kKeyH - 1, updSecLeft + 8, cx + inner - 8, keyLine));
        CHECK(f0.rowNear(key.top - 1, updSecLeft + 8, cx + inner - 8, card) && f0.rowNear(key.top + kKeyH, updSecLeft + 8, cx + inner - 8, card));   // 26, not 30
        CHECK(f0.near(updSecLeft + 8, key.top + 13, card));                             // ghost: the card shows through
        CHECKV(f0.countNear(updSecLeft + 2, key.top + 2, cx + inner - 2, key.top + kKeyH - 2, keyFg, 60) >= 12, "secondary label in key-fg");
        CHECK(f0.countInk(c.x + kPad, key.bot + 1, c.x + c.w - kPad, c.y + c.h - kPad, card) == 0);   // no note, no error: nothing under the keys
    }

    // =========================================================================================================
    // 3. Release notes: evenly spaced bullets, the size note under the keys
    // =========================================================================================================
    {
        const Box& c = notes;
        const int cx = c.x + kPad, inner = c.w - 2 * kPad;
        const Run key = colRun(f0, cx + 8, c.y + kPad + 20, c.y + c.h - 2, accent, 3, 10);
        CHECKV(key.top > c.y + kPad + 40 && key.bot - key.top + 1 == kKeyH, "notes: primary key rows %d..%d", key.top, key.bot);
        // the bullets sit between the title and the keys: a dot at the left, the words 10 px in
        const int titleEnd = lastRowNear(f0, cx, cx + 150, c.y + kPad, key.top - 1, accentSoft, 50);
        const std::vector<RowSpan> dots = inkSpans(f0, cx, cx + 8, titleEnd + 3, key.top - 3, card);
        const std::vector<RowSpan> words = inkSpans(f0, cx + 10, cx + inner, titleEnd + 3, key.top - 3, card);
        CHECKV(dots.size() == 3 && words.size() == 3, "%zu bullet dots, %zu lines of words (three bullets, one line each)", dots.size(), words.size());
        if (dots.size() == 3) {
            CHECKV(dots[1].top - dots[0].top == dots[2].top - dots[1].top, "dot pitch %d then %d", dots[1].top - dots[0].top, dots[2].top - dots[1].top);
            CHECKV(dots[1].top - dots[0].top >= 14 && dots[1].top - dots[0].top <= 20, "line pitch %d: an 11 px line and 2 px between", dots[1].top - dots[0].top);
        }
        if (words.size() == 3) {
            // the pitch between lines is measured on baselines: the first row of a line's ink depends on whether it holds an f, an l or an i
            const auto base = [&](const RowSpan& s) { return baselineRow(f0, cx + 10, cx + inner, s, card); };
            CHECKV(std::abs((base(words[1]) - base(words[0])) - (base(words[2]) - base(words[1]))) <= 1, "text baseline pitch %d then %d", base(words[1]) - base(words[0]), base(words[2]) - base(words[1]));
            // the dot is a middle dot: it sits at mid x-height of its own line, so the line's rows must hold it (and the next line's must not)
            if (dots.size() == 3) {
                CHECKV(dots[0].top >= words[0].top && dots[0].bot <= words[0].bot && dots[0].bot < words[1].top,
                       "the first dot (rows %d..%d) rides the first line (rows %d..%d)", dots[0].top, dots[0].bot, words[0].top, words[0].bot);
            }
            CHECKV(words[0].top - titleEnd >= kGapPx - 1, "bullets start %d rows under the last row of the title", words[0].top - titleEnd);
        }
        const Ink textInk = inkBox(f0, cx + 10, titleEnd + 3, cx + inner, key.top - 3, card);
        CHECKV(textInk.x0 >= cx + 10 && textInk.x0 <= cx + 12, "the words start %d px in", textInk.x0 - cx);
        CHECKV(f0.countNear(cx + 10, titleEnd + 3, cx + inner, key.top - 3, fgSoft, 40) >= 40, "bullet words in fg-soft");
        // the size note: 10 px dim under the keys, wrapped inside the card and clear of its bottom padding
        const Ink note = inkBox(f0, cx, key.bot + 2, cx + inner, c.y + c.h - 1, card);
        CHECKV(note.n > 100 && note.y0 >= key.bot + kGapPx && note.y1 <= c.y + c.h - 1 - 10, "note ink n=%d rows %d..%d (key ends %d, card ends %d)", note.n, note.y0, note.y1, key.bot, c.y + c.h - 1);
        CHECKV(note.y1 - note.y0 >= 14, "the note wraps to two lines: %d rows of ink", note.y1 - note.y0 + 1);
        CHECKV(f0.countNear(cx, key.bot + 2, cx + inner, c.y + c.h - 1, dim, 40) >= 40, "note colour dim");
        CHECK(f0.countNear(cx, key.bot + 2, cx + inner, c.y + c.h - 1, errFg, 20) == 0);   // a note is not an error
    }

    // =========================================================================================================
    // 4. The download: a 6 px bar in the slider's track and fill replaces the primary key, the label beside it
    // =========================================================================================================
    int progKeyTop = 0, progBarTop = 0;
    Bar progBar;
    {
        const Box& c = prog;
        const int cx = c.x + kPad, inner = c.w - 2 * kPad;
        const int secLeft = cx + inner - updSecW;                                          // the same "Not now" key as the update card's
        progKeyTop = firstRunRow(f0, c.y + kPad + 10, c.y + c.h - 5, secLeft + 10, secLeft + 40, keyLine);
        CHECKV(progKeyTop > 0, "secondary key found at row %d", progKeyTop);
        const int mid = progKeyTop + kKeyH / 2;
        progBar = measureBar(f0, mid, cx + 20, secLeft - 2, sliderFill, sliderTrack);
        CHECKV(progBar.x0 > cx + 20 && progBar.fill() > 10 && progBar.total() > progBar.fill() + 20, "bar: from %d, fill %d of %d px", progBar.x0, progBar.fill(), progBar.total());
        const double width = progBar.total(), filled = progBar.fill();
        CHECKV(std::abs(filled / width - 0.4) <= 0.04, "the fill is %.3f of the bar (%.0f of %.0f px), wanted 0.4", filled / width, filled, width);
        CHECKV(progBar.end < secLeft - 2 && progBar.end > secLeft - 20, "the bar ends %d px before the Not now key", secLeft - 1 - progBar.end);
        // 6 px high, centred on the key row's 26, round ends
        const Run rows = colRun(f0, progBar.x0 + 20, progKeyTop, progKeyTop + kKeyH, sliderFill, 6, 3);
        progBarTop = rows.top;
        CHECKV(rows.top == progKeyTop + 10 && rows.bot - rows.top + 1 == 6, "the bar is %d px high from row %d (key row %d, wanted 6 high from %d)", rows.bot - rows.top + 1, rows.top, progKeyTop, progKeyTop + 10);
        CHECK(f0.near(progBar.end - 20, rows.top + 1, sliderTrack, 4) && f0.near(progBar.end - 20, rows.top + 4, sliderTrack, 4));   // the track beyond the fill
        CHECK(f0.near(progBar.end - 20, rows.top - 1, card) && f0.near(progBar.end - 20, rows.top + 6, card));
        CHECKV(!f0.near(progBar.x0, rows.top, sliderFill, 20) && !f0.near(progBar.end, rows.top, sliderTrack, 4), "the bar's ends are rounded: %s / %s", hex(f0.at(progBar.x0, rows.top)).c_str(), hex(f0.at(progBar.end, rows.top)).c_str());
        // the label is in front of it; and NO accent key: the tallest accent column in the key row is the bar's 6
        const Ink label = inkBox(f0, cx, progKeyTop, progBar.x0 - 3, progKeyTop + kKeyH, card);
        CHECKV(label.n > 40 && label.x0 >= cx && label.x0 <= cx + 2, "label ink n=%d from x %d", label.n, label.x0);
        CHECKV(f0.countNear(cx, progKeyTop, progBar.x0 - 3, progKeyTop + kKeyH, fgSoft, 50) >= 15, "label colour fg-soft");
        CHECKV(std::abs((label.y0 + label.y1 + 1) / 2.0 - (progKeyTop + 13.0)) <= 3.0, "label rows %d..%d beside a bar on row %d", label.y0, label.y1, mid);
        int tallest = 0;
        for (int x = cx; x < cx + inner; ++x) {
            int run = 0;
            for (int y = progKeyTop; y < progKeyTop + kKeyH; ++y) {
                if (f0.near(x, y, accent, 6)) { tallest = std::max(tallest, ++run); } else { run = 0; }
            }
        }
        CHECKV(tallest == 6, "tallest accent column in the key row is %d px, so there is no key", tallest);
        // the secondary key is still there, and the note under it
        CHECK(f0.near(cx + inner - 1, progKeyTop + 13, keyLine));
        CHECKV(f0.countNear(cx, progKeyTop + kKeyH + 2, cx + inner, c.y + c.h - 1, dim, 40) >= 15, "note under the key row");
    }

    // =========================================================================================================
    // 5. The tones: warn (the Receiver control card, with Checkbox children and a note) and err (critical, with its error)
    // =========================================================================================================
    std::vector<Run> warnBoxes;                                                            // the left border column of each child box
    {
        const Box& c = warn;
        const int cx = c.x + kPad, inner = c.w - 2 * kPad;
        const Ink title = inkBox(f0, cx, c.y + kPad, cx + inner, c.y + kPad + 14, cardWarn);
        CHECKV(title.n > 40 && title.x0 <= cx + 2, "warn title ink n=%d", title.n);
        CHECKV(f0.countNear(cx, c.y + kPad, cx + inner, c.y + kPad + 14, accentSoft, 40) >= 20, "warn title in accent-soft");
        CHECKV(f0.countNear(cx, c.y + kPad + 14, cx + inner, c.y + c.h - kPad, fgSoft, 40) >= 40, "its line in fg-soft");
        // two Checkbox children, each a 14 px accent box at the content edge, between the line and the note
        for (int y = c.y + kPad + 14; y < c.y + c.h - kPad;) {
            const Run r = colRun(f0, cx, y, c.y + c.h - kPad, accent, 6, 8);
            if (r.top < 0) { break; }
            warnBoxes.push_back(r);
            y = r.bot + 1;
        }
        CHECKV(warnBoxes.size() == 2, "%zu checkbox boxes found in the warn card (want 2)", warnBoxes.size());
        if (warnBoxes.size() == 2) {
            const int top0 = warnBoxes[0].top - 2, top1 = warnBoxes[1].top - 2;           // the border column starts 2 rows into the 2 px corner
            CHECKV(warnBoxes[0].bot - warnBoxes[0].top + 1 == 10 && warnBoxes[1].bot - warnBoxes[1].top + 1 == 10, "boxes are 14 px: borders span %d and %d rows", warnBoxes[0].bot - warnBoxes[0].top + 1, warnBoxes[1].bot - warnBoxes[1].top + 1);
            CHECK(f0.near(cx - 1, top0 + 6, cardWarn));                                   // at the content edge
            CHECKV(f0.countNear(cx + 22, top0 - 2, cx + inner, top0 + 36, fg, 60) >= 25, "the box's label in Theme.fg");
            CHECKV(top1 - top0 >= 30, "the second row is %d px under the first (a wrapped label between)", top1 - top0);
            const Ink note = inkBox(f0, cx, top1 + 20, cx + inner, c.y + c.h - 1, cardWarn);
            CHECKV(note.n > 30 && note.y1 <= c.y + c.h - 1 - 10, "note ink n=%d ends at %d (card %d)", note.n, note.y1, c.y + c.h - 1);
            CHECKV(top0 - lastRowNear(f0, cx, cx + inner, c.y + kPad + 14, top0 - 1, fgSoft, 40) >= 3, "the first box is clear of the line above it");
        }
        // a warn card with no key row: no key-line column anywhere inside it
        CHECK(f0.countNear(cx + 10, c.y + kPad, cx + inner - 10, c.y + c.h - kPad, keyLine, 3) < 6);
    }
    {
        const Box& c = err;
        const int cx = c.x + kPad, inner = c.w - 2 * kPad;
        // `critical`: the title is chip-err-fg, not accent-soft
        CHECKV(f0.countNear(cx, c.y + kPad, cx + 160, c.y + kPad + 30, errFg, 40) >= 25, "critical title in chip-err-fg: %d", f0.countNear(cx, c.y + kPad, cx + 160, c.y + kPad + 30, errFg, 40));
        CHECK(f0.countNear(cx, c.y + kPad, cx + 160, c.y + kPad + 30, accentSoft, 30) == 0);
        // the keys keep their look on the red ground; the ghost key shows the red through
        const Run key = colRun(f0, cx + 8, c.y + kPad + 20, c.y + c.h - 2, accent, 3, 10);
        CHECKV(key.top > 0 && key.bot - key.top + 1 == kKeyH, "err: primary key rows %d..%d", key.top, key.bot);
        CHECK(f0.near(cx + inner - 40, key.top + 8, cardErr));
        // the error line under the keys: 11 px in chip-err-fg, 6 px below, inside the padding
        const Ink e = inkBox(f0, cx, key.bot + 2, cx + inner, c.y + c.h - 1, cardErr);
        CHECKV(e.n > 40 && e.y0 >= key.bot + kGapPx && e.y1 <= c.y + c.h - 1 - 10, "error ink n=%d rows %d..%d (key ends %d, card ends %d)", e.n, e.y0, e.y1, key.bot, c.y + c.h - 1);
        CHECKV(f0.countNear(cx, key.bot + 2, cx + inner, c.y + c.h - 1, errFg, 40) >= 30, "error line colour chip-err-fg: %d", f0.countNear(cx, key.bot + 2, cx + inner, c.y + c.h - 1, errFg, 40));
        CHECKV(e.y1 - e.y0 <= 14, "the error is one line: %d rows", e.y1 - e.y0 + 1);
        // the line of text between the title and the keys is fg-soft, not red
        CHECK(f0.countNear(cx, c.y + kPad + 14, cx + inner, key.top - 2, fgSoft, 40) >= 30);
    }

    // =========================================================================================================
    // 6. The team message: bullets that wrap under themselves, a plain "Seen" key across the whole width
    // =========================================================================================================
    int teamKeyTop = 0;
    {
        const Box& c = team;
        const int cx = c.x + kPad, inner = c.w - 2 * kPad;
        teamKeyTop = c.y + c.h - kPad - kKeyH;                                           // the key row is the card's last row
        CHECKV(f0.rowNear(teamKeyTop, cx + 8, cx + inner - 8, keyLine) && f0.rowNear(teamKeyTop + kKeyH - 1, cx + 8, cx + inner - 8, keyLine),
               "Seen key's border rows %d and %d", teamKeyTop, teamKeyTop + kKeyH - 1);
        CHECK(f0.rowNear(teamKeyTop + 4, cx + 8, cx + inner - 8, keyBg));              // a plain key: the key ground, not the accent
        CHECK(f0.near(cx, teamKeyTop + 13, keyLine) && f0.near(cx + inner - 1, teamKeyTop + 13, keyLine));   // from the content edge to the other
        CHECK(f0.near(cx - 1, teamKeyTop + 13, card) && f0.near(cx + inner, teamKeyTop + 13, card));
        CHECKV(f0.countInk(cx + 8, teamKeyTop + 2, cx + inner - 8, teamKeyTop + kKeyH - 2, keyBg) >= 20, "the word on the key");
        CHECK(f0.countNear(cx, teamKeyTop, cx + inner, teamKeyTop + kKeyH, accent, 8) == 0);
        // the title and the date
        const int titleEnd = lastRowNear(f0, cx, cx + 190, c.y + kPad, teamKeyTop - 1, accentSoft, 50);
        CHECKV(f0.countNear(cx, c.y + kPad, cx + 190, titleEnd + 1, accentSoft, 40) >= 30, "team title in accent-soft");
        const Ink date = inkBox(f0, cx + inner - 70, c.y + kPad, cx + inner, c.y + kPad + 14, card);
        CHECKV(date.n > 15 && date.x1 >= cx + inner - 4, "date ink n=%d right edge %d", date.n, date.x1);
        // two bullets, the second wraps: two dots, three lines of words
        const std::vector<RowSpan> dots = inkSpans(f0, cx, cx + 8, titleEnd + 3, teamKeyTop - 3, card);
        const std::vector<RowSpan> words = inkSpans(f0, cx + 10, cx + inner, titleEnd + 3, teamKeyTop - 3, card);
        CHECKV(dots.size() == 2 && words.size() == 3, "%zu dots and %zu lines of words (two bullets, the second wraps)", dots.size(), words.size());
        if (dots.size() == 2 && words.size() == 3) {
            // pitches are measured on baselines (the first row of a line's ink moves with the glyphs the line holds: the line that
            // starts "using Request a feature" has an f and an i, so its ink begins a row higher than the lines above it)
            const auto base = [&](const RowSpan& s) { return baselineRow(f0, cx + 10, cx + inner, s, card); };
            const int bulletPitch = base(words[1]) - base(words[0]), linePitch = base(words[2]) - base(words[1]);
            CHECKV(std::abs(bulletPitch - linePitch - 2) <= 1, "bullets are 2 px further apart than the lines of one bullet: baselines %d, then %d", bulletPitch, linePitch);
            CHECKV(std::abs((dots[1].top - dots[0].top) - bulletPitch) <= 1, "second dot %d under the first, second bullet's baseline %d under the first's", dots[1].top - dots[0].top, bulletPitch);
            CHECKV(bulletPitch <= 20 && linePitch <= 20, "line pitch %d / %d: 2 px between lines, not 6", bulletPitch, linePitch);   // wrapped and bulleted lines alike
        }
    }

    // =========================================================================================================
    // 7. The disabled primary, the Store card's single full-width key
    // =========================================================================================================
    int disKeyTop = 0, storeKeyTop = 0;
    {
        const Box& c = dis;
        const int cx = c.x + kPad, inner = c.w - 2 * kPad;
        disKeyTop = c.y + c.h - kPad - kKeyH;
        CHECKV(f0.rowNear(disKeyTop + 5, cx + 8, cx + 100, keyDisBg), "disabled key ground %s", hex(f0.at(cx + 20, disKeyTop + 5)).c_str());    // key-disabled-bg
        CHECK(f0.countNear(cx + 4, disKeyTop + 2, cx + 160, disKeyTop + kKeyH - 2, keyDisFg, 24) >= 8);          // its label in key-disabled-fg
        CHECK(f0.countNear(cx + 4, disKeyTop + 2, cx + 160, disKeyTop + kKeyH - 2, accentInk, 12) == 0);
        CHECK(f0.countNear(cx, disKeyTop, cx + 160, disKeyTop + kKeyH, accent, 8) == 0);                         // no accent: it is off
        const Rgb half = over(accent, 0.5, keyDisBg);
        CHECKV(f0.rowNear(disKeyTop, cx + 8, cx + 100, half, 14), "disabled border is %s, about %s", hex(f0.at(cx + 20, disKeyTop)).c_str(), hex(half).c_str());   // half alpha, not dashed
        // no subtitle: the title has the row to itself, on one line
        const Ink t = inkBox(f0, cx, c.y + kPad, cx + inner, disKeyTop - 3, card);
        CHECKV(t.n > 40 && t.y1 - t.y0 <= 10, "title ink n=%d, %d rows high (one line)", t.n, t.y1 - t.y0 + 1);
        CHECK(t.x1 > cx + 150);                                                          // and it uses the width
        // the secondary's word is "No": the key is not as wide as its word and padding, it is the 72 px minimum
        int x = cx + inner - 2;
        while (f0.near(x, disKeyTop + 6, card, 3)) { --x; }
        const int secW = cx + inner - x;
        CHECKV(secW == 72, "a key labelled \"No\" is %d px wide, wanted the 72 px minimum", secW);
        CHECK(f0.rowNear(disKeyTop + 6, x - kGapPx, x, card));                           // the 6 px gap before it ...
        CHECK(!f0.near(x - kGapPx - 1, disKeyTop + 6, card, 3));                         // ... then the primary's last column,
        CHECK(!f0.near(cx, disKeyTop + 6, card, 3) && f0.near(cx - 1, disKeyTop + 6, card, 3));   // which runs back to the content edge: 176 wide
    }
    {
        const Box& c = store;
        const int cx = c.x + kPad, inner = c.w - 2 * kPad;
        const Run key = colRun(f0, cx + 8, c.y + kPad + 20, c.y + c.h - 2, accent, 3, 10);
        storeKeyTop = key.top;
        CHECKV(key.top > 0 && key.bot - key.top + 1 == kKeyH, "store key rows %d..%d", key.top, key.bot);
        CHECK(f0.colNear(cx, key.top + 6, key.top + 20, accent) && f0.colNear(cx + inner - 1, key.top + 6, key.top + 20, accent));   // all 254 px
        CHECK(f0.rowNear(key.top + 4, cx + 8, cx + inner - 8, accent));
        // a headline too long for one line wraps to three beside the subtitle, and the card grows to hold it
        const std::vector<RowSpan> lines = inkSpans(f0, cx, cx + inner - 90, c.y + kPad, key.top - 2, card);
        CHECKV(lines.size() >= 3, "the Store headline takes %zu lines", lines.size());
        const Ink note = inkBox(f0, cx, key.bot + 2, cx + inner, c.y + c.h - 1, card);
        CHECKV(note.n > 100 && note.y1 <= c.y + c.h - 1 - 10 && note.y1 - note.y0 >= 14, "store note ink n=%d rows %d..%d", note.n, note.y0, note.y1);
    }

    // =========================================================================================================
    // 7b. A primary word too long for what "Not now" leaves is elided: the keys still fill the row and stay inside the card
    // =========================================================================================================
    {
        const Box& c = longKey;
        const int cx = c.x + kPad, inner = c.w - 2 * kPad;
        const Run key = colRun(f0, cx + 8, c.y + kPad, c.y + c.h - 2, accent, 3, 10);
        CHECKV(key.top > 0 && key.bot - key.top + 1 == kKeyH, "long key rows %d..%d", key.top, key.bot);
        // the secondary is the same "Not now" key, flush to the content edge; the primary has everything else
        const int secLeft = cx + inner - updSecW;
        CHECKV(f0.near(secLeft, key.top + 13, keyLine) && f0.near(cx + inner - 1, key.top + 13, keyLine), "Not now key at %d..%d", secLeft, cx + inner - 1);
        CHECK(f0.rowNear(key.top + 6, secLeft - kGapPx, secLeft, card));                // the 6 px gap
        CHECKV(f0.colNear(secLeft - kGapPx - 1, key.top + 8, key.top + 18, accent), "the primary runs to the gap: %s", hex(f0.at(secLeft - kGapPx - 1, key.top + 12)).c_str());
        CHECK(f0.colNear(cx, key.top + 8, key.top + 18, accent) && f0.near(cx - 1, key.top + 12, card));
        // its word is longer than that, so the label is cut with an ellipsis: ink up to near the key's right end
        const int keyRight = secLeft - kGapPx - 1;
        const Ink label = inkBox(f0, cx + 4, key.top + 2, keyRight, key.top + kKeyH - 2, accent);
        CHECKV(label.n > 60 && label.x1 >= keyRight - 16 && label.x1 < keyRight, "label ink n=%d to x %d, key ends at %d", label.n, label.x1, keyRight);
        CHECK(f0.countNear(cx + 4, key.top + 2, keyRight, key.top + kKeyH - 2, accentInk, 40) >= 25);
    }

    // =========================================================================================================
    // 8. Clicks: the primary and the secondary report themselves, only when enabled
    // =========================================================================================================
    {
        reset();
        const Box& c = upd;
        const int cx = c.x + kPad;
        clk(cx + updPriW / 2, updKeyTop + 13);
        CHECKV(ui->get_primaries() == 1 && ui->get_secondaries() == 0 && sval(ui->get_last_clicked()) == "upd", "primary: %d / %d, last '%s'", ui->get_primaries(), ui->get_secondaries(), sval(ui->get_last_clicked()).c_str());
        Frame hot = render(ui, kW, kH);
        CHECKV(!hot.near(cx + 8, updKeyTop + 13, accent, 6), "the key under the pointer lights: still %s", hex(hot.at(cx + 8, updKeyTop + 13)).c_str());   // (how is the Key family's: the card's key is one)
        clk(updSecLeft + updSecW / 2, updKeyTop + 13);
        CHECKV(ui->get_primaries() == 1 && ui->get_secondaries() == 1 && sval(ui->get_last_clicked()) == "upd", "secondary: %d / %d", ui->get_primaries(), ui->get_secondaries());
        Frame hot2 = render(ui, kW, kH);
        CHECKV(hot2.near(updSecLeft + 8, updKeyTop + 13, menuHot), "the ghost key under the pointer lights: %s", hex(hot2.at(updSecLeft + 8, updKeyTop + 13)).c_str());
        clk(cx + updPriW + 3, updKeyTop + 13);                                    // the gap between the keys is neither
        CHECK(ui->get_primaries() == 1 && ui->get_secondaries() == 1);
        clk(cx + 30, c.y + kPad + 4);                                             // nor is the title
        CHECK(ui->get_primaries() == 1 && ui->get_secondaries() == 1);
        park();
        Frame cool = render(ui, kW, kH);
        CHECK(differing(f0, cool, upd.x, upd.y, upd.x + upd.w, upd.y + upd.h) == 0);    // the card is as it was once the pointer leaves
        // the Seen key: the whole width is the key
        reset();
        clk(team.x + kPad + 4, teamKeyTop + 13);
        clk(team.x + team.w - kPad - 5, teamKeyTop + 13);
        clk(team.x + team.w / 2, teamKeyTop + 13);
        CHECKV(ui->get_primaries() == 3 && sval(ui->get_last_clicked()) == "team", "Seen: %d clicks, last '%s'", ui->get_primaries(), sval(ui->get_last_clicked()).c_str());
        CHECK(ui->get_secondaries() == 0);
        clk(team.x + 5, teamKeyTop + 13);                                         // the padding beside the key is not
        CHECK(ui->get_primaries() == 3);
        // the Store card's only key
        reset();
        clk(store.x + store.w / 2, storeKeyTop + 13);
        CHECKV(ui->get_primaries() == 1 && sval(ui->get_last_clicked()) == "store", "Store key: %d clicks, last '%s'", ui->get_primaries(), sval(ui->get_last_clicked()).c_str());
        // the err card's two keys
        reset();
        const Run ek = colRun(f0, err.x + kPad + 8, err.y + kPad + 20, err.y + err.h - 2, accent, 3, 10);
        clk(err.x + kPad + 40, ek.top + 13);
        clk(err.x + err.w - kPad - 20, ek.top + 13);
        CHECK(ui->get_primaries() == 1 && ui->get_secondaries() == 1 && sval(ui->get_last_clicked()) == "err");
        // a download in progress has no primary key: a click on the bar does nothing, the secondary still answers
        reset();
        clk((progBar.x0 + progBar.end) / 2, progKeyTop + 13);
        clk(prog.x + kPad + 5, progKeyTop + 13);
        CHECKV(ui->get_primaries() == 0 && ui->get_secondaries() == 0, "bar and label: %d primaries, %d secondaries", ui->get_primaries(), ui->get_secondaries());
        clk(prog.x + prog.w - kPad - 30, progKeyTop + 13);
        CHECKV(ui->get_primaries() == 0 && ui->get_secondaries() == 1 && sval(ui->get_last_clicked()) == "prog", "Not now beside the bar: %d / %d", ui->get_primaries(), ui->get_secondaries());
        // a disabled primary never acts; the secondary beside it does
        reset();
        clk(dis.x + kPad + 40, disKeyTop + 13);
        clk(dis.x + kPad + 5, disKeyTop + 13);
        CHECKV(ui->get_primaries() == 0, "disabled primary fired %d times", ui->get_primaries());
        clk(dis.x + dis.w - kPad - 20, disKeyTop + 13);
        CHECK(ui->get_secondaries() == 1 && sval(ui->get_last_clicked()) == "dis");
        park();
    }

    // =========================================================================================================
    // 9. A card's children are live: the warn card's Checkbox rows toggle
    // =========================================================================================================
    if (warnBoxes.size() == 2) {
        reset();
        const int cx = warn.x + kPad, top = warnBoxes[0].top - 2;
        clk(cx + 7, top + 7);
        CHECKV(ui->get_toggles() == 1 && ui->get_primaries() == 0, "child checkbox: %d toggles", ui->get_toggles());
        park();
        Frame off = render(ui, kW, kH);
        CHECKV(off.near(cx + 7, top + 2, field) && off.near(cx + 2, top + 7, field), "unchecked: the field ground, %s", hex(off.at(cx + 7, top + 2)).c_str());
        clk(cx + 7, top + 7);
        park();
        CHECK(render(ui, kW, kH).near(cx + 3, top + 3, accent));
        CHECK(ui->get_toggles() == 2);
        clk(cx + 7, warnBoxes[1].top - 2 + 7);                                     // the second row's box is its own
        CHECK(ui->get_toggles() == 3);
        clk(cx + 7, warnBoxes[1].top - 2 + 7);
        park();
    }

    // =========================================================================================================
    // 10. The bar follows `progress`: 0 is an empty track, 1 a full one, past it is clamped, below 0 gives the key back
    // =========================================================================================================
    {
        const int cx = prog.x + kPad, inner = prog.w - 2 * kPad;
        const int secLeft = cx + inner - updSecW, mid = progKeyTop + kKeyH / 2;
        const auto bar = [&] { return measureBar(render(ui, kW, kH), mid, cx + 20, secLeft - 2, sliderFill, sliderTrack); };
        ui->set_prog_progress(0.0f);
        Bar b = bar();
        CHECKV(b.x0 > 0 && b.fill() == 0 && std::abs(b.total() - progBar.total()) <= 3, "progress 0: fill %d of %d px (the bar is %d at 0.4)", b.fill(), b.total(), progBar.total());
        ui->set_prog_progress(1.0f);
        b = bar();
        CHECKV(b.total() - b.fill() <= 3 && std::abs(b.total() - progBar.total()) <= 3, "progress 1: fill %d of %d", b.fill(), b.total());   // (the round end's blended pixels count as track)
        const int fullFill = b.fill();
        ui->set_prog_progress(2.5f);
        b = bar();
        CHECKV(b.fill() == fullFill && b.total() - b.fill() <= 3, "progress 2.5 is clamped to full: fill %d of %d, full was %d", b.fill(), b.total(), fullFill);
        ui->set_prog_progress(0.75f);
        b = bar();
        CHECKV(std::abs(b.fill() / (double)b.total() - 0.75) <= 0.04, "progress 0.75: fill %d of %d", b.fill(), b.total());
        ui->set_prog_progress(0.4f);
        b = bar();
        CHECKV(b.x0 == progBar.x0 && b.fill() == progBar.fill() && b.total() == progBar.total(), "back at 0.4: fill %d of %d, as at first (%d of %d)", b.fill(), b.total(), progBar.fill(), progBar.total());
        // below zero there is no bar: the primary key is back and fills the row beside the secondary
        ui->set_prog_progress(-1.0f);
        Frame back = render(ui, kW, kH);
        const Run key = colRun(back, cx + 8, prog.y + kPad + 20, prog.y + prog.h - 2, accent, 3, 10);
        CHECKV(key.top > 0 && key.bot - key.top + 1 == kKeyH, "progress -1: the key returns (rows %d..%d)", key.top, key.bot);
        reset();
        clk(cx + 40, key.top + 13);
        CHECKV(ui->get_primaries() == 1 && sval(ui->get_last_clicked()) == "prog", "the returned key fires primary-clicked: %d", ui->get_primaries());
        park();
        ui->set_prog_progress(0.4f);
        Frame again = render(ui, kW, kH);
        const int dd = differing(f0, again, prog.x, prog.y, prog.x + prog.w, prog.y + prog.h);
        CHECKV(dd == 0, "back at 0.4 the card is the sheet's again: %d pixels differ", dd);
    }

    // =========================================================================================================
    // 11. Tooltips: the primary's, shown even while it is disabled, 6 px under the key
    // =========================================================================================================
    {
        // an enabled key with a tooltip
        const Run key = colRun(f0, notes.x + kPad + 8, notes.y + kPad + 20, notes.y + notes.h - 2, accent, 3, 10);
        const int tipX = notes.x + kPad + 1, tipY = key.bot + 1 + kGapPx + 4;           // inside the tip's left padding, clear of its words
        Frame none = render(ui, kW, kH);
        CHECKV(blockNear(none, tipX, tipY, 6, 14, menu, 2) <= 16, "no tip before the pointer rests: %d of 84 pixels are tip ground", blockNear(none, tipX, tipY, 6, 14, menu, 2));
        mv(notes.x + kPad + 40, key.top + 13);
        settle(milliseconds(750));
        Frame tip = render(ui, kW, kH);
        CHECKV(blockNear(tip, tipX, tipY, 6, 14, menu, 2) >= 80, "tooltip ground under the key: %d of 84 pixels", blockNear(tip, tipX, tipY, 6, 14, menu, 2));
        park();
        CHECK(blockNear(render(ui, kW, kH), tipX, tipY, 6, 14, menu, 2) <= 16);
        // the disabled primary says why
        mv(dis.x + kPad + 40, disKeyTop + 13);
        settle(milliseconds(750));
        Frame d = render(ui, kW, kH);
        const int dx = dis.x + kPad + 1, dy = disKeyTop + kKeyH + kGapPx + 4;
        CHECKV(blockNear(d, dx, dy, 6, 14, menu, 2) >= 80, "the disabled key's tooltip: %d of 84 pixels", blockNear(d, dx, dy, 6, 14, menu, 2));
        park();
        // a key without a tooltip opens none
        mv(upd.x + kPad + 40, updKeyTop + 13);
        settle(milliseconds(750));
        CHECK(blockNear(render(ui, kW, kH), upd.x + kPad + 1, updKeyTop + kKeyH + kGapPx + 4, 6, 14, menu, 2) <= 16);
        park();
    }

    // =========================================================================================================
    // 12. The Banner: 28 px, the tone's ground and line, the words elided, a small key at the right
    // =========================================================================================================
    const Box bWarn{16, 652, 608, 28}, bLong{16, 688, 400, 28}, bErr{428, 688, 196, 28}, bPlain{16, 724, 300, 28};
    int warnKeyLeft = 0, plainKeyLeft = 0, longKeyLeft = 0;
    {
        const auto frame = [&](const char* name, const Box& b, Rgb ground, Rgb line) {
            CHECKV(f0.rowNear(b.y, b.x + 2, b.x + b.w - 2, line) && f0.rowNear(b.y + b.h - 1, b.x + 2, b.x + b.w - 2, line), "%s: border rows", name);
            CHECKV(f0.colNear(b.x, b.y, b.y + b.h, line) && f0.colNear(b.x + b.w - 1, b.y, b.y + b.h, line), "%s: border columns", name);
            CHECKV(f0.rowNear(b.y - 1, b.x, b.x + b.w, bg) && f0.rowNear(b.y + b.h, b.x, b.x + b.w, bg), "%s: exactly 28 px (ground just outside)", name);
            CHECKV(f0.near(b.x + 5, b.y + 14, ground) && f0.near(b.x + b.w - 5, b.y + 3, ground), "%s: ground %s", name, hex(f0.at(b.x + 5, b.y + 14)).c_str());
            CHECK(f0.near(b.x, b.y, line));                                              // square corners: a bar, not a card
        };
        frame("warn banner", bWarn, cardWarn, cardWarnLine);
        frame("long banner", bLong, cardWarn, cardWarnLine);
        frame("err banner", bErr, cardErr, cardErrLine);
        frame("plain banner", bPlain, card, lineStrong);
        // the words: 11 px fg, inset 11 px, centred on the 28
        const Ink words = inkBox(f0, bWarn.x + 2, bWarn.y + 2, bWarn.x + 300, bWarn.y + 26, cardWarn);
        CHECKV(words.n > 30 && words.x0 >= bWarn.x + 10 && words.x0 <= bWarn.x + 13, "banner words ink n=%d from x %d", words.n, words.x0 - bWarn.x);
        CHECKV(std::abs((words.y0 + words.y1 + 1) / 2.0 - (bWarn.y + 14)) <= 3.0, "banner words centred: rows %d..%d in a bar at %d", words.y0, words.y1, bWarn.y);
        CHECKV(f0.countNear(bWarn.x + 2, bWarn.y + 2, bWarn.x + 300, bWarn.y + 26, fg, 60) >= 20, "banner words in Theme.fg");
        // the key: small (24 high), key ground and border, 6 px from the right edge (7 with the bar's border)
        const auto keyOf = [&](const Box& b, Rgb ground) {
            int x = b.x + b.w - 1 - 7 - 1;                                                 // the pixel left of the key's right border
            while (f0.near(x, b.y + 8, ground, 3)) { --x; }
            return x;                                                                      // the key's left border
        };
        warnKeyLeft = keyOf(bWarn, keyBg);
        const int kw = bWarn.x + bWarn.w - 7 - warnKeyLeft;
        CHECKV(kw >= 50 && kw <= 120, "Stop plugin key is %d px wide", kw);
        CHECK(f0.near(warnKeyLeft, bWarn.y + 14, keyLine) && f0.near(bWarn.x + bWarn.w - 8, bWarn.y + 14, keyLine));
        CHECKV(f0.colNear(warnKeyLeft + 8, bWarn.y + 3, bWarn.y + 25, keyBg, 3), "key is 24 high: rows %d..%d", bWarn.y + 2, bWarn.y + 25);
        CHECK(f0.near(warnKeyLeft + 8, bWarn.y + 1, cardWarn) && f0.near(warnKeyLeft + 8, bWarn.y + 26, cardWarn));   // the bar's ground above and below it
        CHECKV(f0.near(warnKeyLeft + 8, bWarn.y + 2, keyLine, 14) || f0.near(warnKeyLeft + 8, bWarn.y + 2, keyBg, 14), "key's top row");
        CHECK(f0.countInk(warnKeyLeft + 2, bWarn.y + 4, bWarn.x + bWarn.w - 9, bWarn.y + 24, keyBg) >= 25);   // "Stop plugin"
        CHECKV(f0.rowNear(bWarn.y + 14, bWarn.x + bWarn.w - 7, bWarn.x + bWarn.w - 1, cardWarn), "6 px of the bar's ground at the right");
        // the neutral one
        plainKeyLeft = keyOf(bPlain, keyBg);
        CHECKV(bPlain.x + bPlain.w - 7 - plainKeyLeft == kw, "same key, same width: %d vs %d", bPlain.x + bPlain.w - 7 - plainKeyLeft, kw);
        // no key label, no key: the err banner has only its words
        CHECK(f0.countNear(bErr.x + 2, bErr.y + 2, bErr.x + bErr.w - 2, bErr.y + 26, keyBg, 3) == 0);
        CHECK(f0.countInk(bErr.x + 2, bErr.y + 2, bErr.x + bErr.w - 2, bErr.y + 26, cardErr) > 30);
        // a sentence too long for the bar is cut with an ellipsis before the key, and the whole sentence is its tooltip
        longKeyLeft = keyOf(bLong, keyBg);
        const Ink longWords = inkBox(f0, bLong.x + 2, bLong.y + 2, longKeyLeft - 1, bLong.y + 26, cardWarn);
        CHECKV(longWords.x1 >= longKeyLeft - 8 - 12 && longWords.x1 < longKeyLeft - 3, "long text fills up to the key: ink to x %d, key at %d", longWords.x1, longKeyLeft);
        CHECKV(longWords.x1 > words.x1 + 100, "the short sentence ends at x %d, the long one at %d", words.x1, longWords.x1);
    }
    {
        reset();
        clk(warnKeyLeft + 25, bWarn.y + 14);
        CHECKV(ui->get_banner_clicks() == 1 && sval(ui->get_last_clicked()) == "ban-warn", "Stop plugin: %d clicks, last '%s'", ui->get_banner_clicks(), sval(ui->get_last_clicked()).c_str());
        Frame hot = render(ui, kW, kH);
        CHECKV(hot.near(warnKeyLeft + 8, bWarn.y + 14, menuHot), "the key under the pointer lights: %s", hex(hot.at(warnKeyLeft + 8, bWarn.y + 14)).c_str());
        clk(bWarn.x + 60, bWarn.y + 14);                                          // the words are not the key
        CHECK(ui->get_banner_clicks() == 1);
        clk(bLong.x + 90, bLong.y + 14);
        CHECK(ui->get_banner_clicks() == 1);
        clk(longKeyLeft + 25, bLong.y + 14);
        CHECK(ui->get_banner_clicks() == 2 && sval(ui->get_last_clicked()) == "ban-long");
        clk(plainKeyLeft + 25, bPlain.y + 14);
        CHECK(ui->get_banner_clicks() == 3 && sval(ui->get_last_clicked()) == "ban-plain");
        // a disabled bar's key does nothing and looks it
        ui->set_ban_enabled(false);
        park();
        Frame d = render(ui, kW, kH);
        CHECKV(d.near(plainKeyLeft + 8, bPlain.y + 14, keyDisBg), "disabled key ground %s", hex(d.at(plainKeyLeft + 8, bPlain.y + 14)).c_str());
        clk(plainKeyLeft + 25, bPlain.y + 14);
        CHECKV(ui->get_banner_clicks() == 3, "disabled banner key fired: %d", ui->get_banner_clicks());
        ui->set_ban_enabled(true);
        park();
        CHECK(render(ui, kW, kH).near(plainKeyLeft + 8, bPlain.y + 14, keyBg));
        // the tooltip: only a bar that shortened its sentence has one, and it is the whole sentence, 6 px under the bar
        reset();
        mv(bLong.x + 90, bLong.y + 14);
        settle(milliseconds(750));
        Frame tip = render(ui, kW, kH);
        CHECKV(blockNear(tip, bLong.x + 1, bLong.y + bLong.h + 6 + 4, 6, 14, menu, 2) >= 80, "the long banner's tooltip ground: %d of 84 pixels", blockNear(tip, bLong.x + 1, bLong.y + bLong.h + 6 + 4, 6, 14, menu, 2));
        CHECK(tip.near(bLong.x + 5, bLong.y + bLong.h + 3, bg) || tip.near(bLong.x + 5, bLong.y + bLong.h + 3, card));   // the 6 px between
        park();
        mv(bWarn.x + 60, bWarn.y + 14);
        settle(milliseconds(750));
        Frame none = render(ui, kW, kH);
        CHECK(blockNear(none, bWarn.x + 1, bWarn.y + bWarn.h + 6 + 4, 6, 14, menu, 2) <= 16);
        park();
        // the key's own tooltip, 6 px under the 24 px key (which sits 2 px into the 28 px bar). The bar is near the window's right
        // edge, so the popup is shifted left to fit: probe the three rows of padding above its words, where it is all ground
        const int tipTop = bWarn.y + 2 + 24 + 6 + 2;
        CHECK(blockNear(render(ui, kW, kH), 545, tipTop, 80, 3, menu, 2) <= 24);
        mv(warnKeyLeft + 25, bWarn.y + 14);
        settle(milliseconds(750));
        Frame kt = render(ui, kW, kH);
        CHECKV(blockNear(kt, 545, tipTop, 80, 3, menu, 2) >= 216, "the banner key's tooltip: %d of 240 pixels are tip ground", blockNear(kt, 545, tipTop, 80, 3, menu, 2));
        park();
        CHECK(blockNear(render(ui, kW, kH), 545, tipTop, 80, 3, menu, 2) <= 24);
    }

    // =========================================================================================================
    // 13. The skin: a second filling reaches the cards' pixels (ground, line, corner, fill) with no component change
    // =========================================================================================================
    {
        Skin probe = ui->global<Workbench>().get_value();
        probe.card = slint::Color::from_rgb_uint8(0x30, 0x10, 0x10);
        probe.card_warn = slint::Color::from_rgb_uint8(0x10, 0x30, 0x10);
        probe.card_err = slint::Color::from_rgb_uint8(0x10, 0x10, 0x30);
        probe.line_strong = slint::Color::from_rgb_uint8(0xff, 0x00, 0xff);
        probe.card_warn_line = slint::Color::from_rgb_uint8(0x00, 0xff, 0xff);
        probe.accent = slint::Color::from_rgb_uint8(0x00, 0xc0, 0x40);
        probe.slider_fill = slint::Color::from_rgb_uint8(0xff, 0xff, 0x00);
        probe.radius_panel = 0.0f;
        ui->global<Theme>().set_skin(probe);
        Frame g = render(ui, kW, kH);
        CHECK(g.near(upd.x + 5, upd.y + upd.h / 2, rgb(0x301010)));                      // the neutral ground
        CHECK(g.rowNear(upd.y, upd.x + 10, upd.x + upd.w - 10, rgb(0xff00ff)));          // its border
        CHECKV(g.near(upd.x, upd.y, rgb(0xff00ff)), "radius 0: the corner is square: %s", hex(g.at(upd.x, upd.y)).c_str());
        CHECK(g.near(warn.x + 5, warn.y + warn.h / 2, rgb(0x103010)));                   // the warn ground
        CHECK(g.rowNear(warn.y, warn.x + 10, warn.x + warn.w - 10, rgb(0x00ffff)));      // its line
        CHECK(g.near(err.x + 5, err.y + err.h / 2, rgb(0x101030)));                      // the err ground
        CHECK(g.near(bWarn.x + 5, bWarn.y + 14, rgb(0x103010)));                         // the banner shares the warn ground
        CHECK(g.rowNear(bWarn.y, bWarn.x + 10, bWarn.x + bWarn.w - 10, rgb(0x00ffff)));
        CHECK(g.near(upd.x + kPad + 8, updKeyTop + 13, rgb(0x00c040)));                  // the primary key on the new accent
        CHECK(g.countNear(progBar.x0, progBarTop, progBar.end, progBarTop + 6, rgb(0xffff00), 6) > 60);   // the bar's fill on its own token
        ui->global<Theme>().set_skin(ui->global<Workbench>().get_value());
        Frame h = render(ui, kW, kH);
        const int dd = differing(f0, h, 0, 0, kW, kH);
        CHECKV(dd == 0, "the Workbench filling comes back: %d pixels differ", dd);
    }

    return testSummary("test_slint_cards");
}
