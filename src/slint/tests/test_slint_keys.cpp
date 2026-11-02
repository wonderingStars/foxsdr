// The keys: Caption, Key, SegGroup / SegKey, RailKey and StopKey rendered from the specimen sheet and probed against
// the Workbench CSS (Workbench.dc.html, Workbench-PluginKit.dc.html): heights, widths, border and ground colours of
// every variant, the diagonal hazard ground, the dome, the rail key, the group. Then every callback is driven the way
// the OS delivers input: a click, a disabled click, a right-click, the keyboard, a hover and its tooltip.
// The typeface is a fallback on this machine, so text is asserted as ink in a region, never as glyph shapes.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "keys_specimen.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <string_view>

using namespace foxsdr::slinttest;
using std::chrono::milliseconds;

namespace {

constexpr int kW = 800;
constexpr int kH = 720;

// ---- the sheet values (the Workbench filling), as the CSS writes them ----
const Rgb kBg = rgb(0x15171b);          // body and rail ground
const Rgb kPanel = rgb(0x1d2026);
const Rgb kPanelAlt = rgb(0x1a1d22);    // .seg ground
const Rgb kMenu = rgb(0x262a32);        // menus, tooltips, the lit rail key
const Rgb kMenuHot = rgb(0x363b45);
const Rgb kKeyBg = rgb(0x262a32);
const Rgb kKeyLine = rgb(0x363b45);
const Rgb kKeyFg = rgb(0xe6e8ec);
const Rgb kDisBg = rgb(0x1d2026);       // .btn[disabled]
const Rgb kDisFg = rgb(0x8a93a0);
const Rgb kDisLine = over(kKeyLine, 0.5, kDisBg);   // its border: key-line at half alpha over that ground
const Rgb kAccent = rgb(0xf5a524);
const Rgb kAccentInk = rgb(0x1b1400);
const Rgb kAccentHover = rgb(0xffc66a);
const Rgb kDangerBg = rgb(0x3a1d1d);
const Rgb kDangerLine = rgb(0x6b2a2a);
const Rgb kDangerStrong = rgb(0x8a3434);
const Rgb kDangerFg = rgb(0xffd4d4);
const Rgb kDangerGlyph = rgb(0xff8a8a);
const Rgb kHazardLine = rgb(0x6b4d16);
const Rgb kHazardStripe = rgb(0x2e2a1c);
const Rgb kPlateLine = rgb(0xf5a524);
const Rgb kPlateFg = rgb(0xf5c66b);
const Rgb kLineStrong = rgb(0x363b45);
const Rgb kFgSoft = rgb(0xc9ced6);
const Rgb kMuted = rgb(0x9aa3ad);
const Rgb kFg = rgb(0xe6e8ec);
const Rgb kFaint = rgb(0x5c6370);
const Rgb kFocus = rgb(0x7ad7ff);
const Rgb kLampRing = rgb(0x434955);
const Rgb kLampAmber = rgb(0xf5a524);

// FOXSDR_KEYS_VERBOSE=1 prints the measurements the checks are made from.
bool verbose() {
    static const bool v = std::getenv("FOXSDR_KEYS_VERBOSE") != nullptr;
    return v;
}

// Names the group of checks a failure belongs to (a CHECK inside a helper only knows its own line).
struct Scope {
    const char* what;
    int before;
    explicit Scope(const char* w) : what(w), before(g_checksFailed.load()) {}
    ~Scope() {
        if (g_checksFailed.load() != before) { std::printf("  ^ failed in: %s\n", what); }
    }
};

struct Run {
    int first = -1;
    int last = -1;
    int len() const { return first < 0 ? 0 : last - first + 1; }
};

// The first and last pixel of colour `c` along row y in [x0, x1), or along column x in [y0, y1).
Run rowRun(const Frame& f, int y, int x0, int x1, Rgb c, int tol = 6) {
    Run r;
    for (int x = x0; x < x1; ++x) {
        if (f.near(x, y, c, tol)) {
            if (r.first < 0) { r.first = x; }
            r.last = x;
        }
    }
    return r;
}
Run colRun(const Frame& f, int x, int y0, int y1, Rgb c, int tol = 6) {
    Run r;
    for (int y = y0; y < y1; ++y) {
        if (f.near(x, y, c, tol)) {
            if (r.first < 0) { r.first = y; }
            r.last = y;
        }
    }
    return r;
}

// A key measured from the pixels: its border (or lit fill) colour marks the outline.
struct Box {
    int x = -1, y = -1, w = 0, h = 0;
    int midX() const { return x + w / 2; }
    int midY() const { return y + h / 2; }
};
Box measure(const Frame& f, int x, int y, int h, Rgb outline, int reach = 152) {
    Box b;
    const Run row = rowRun(f, y + h / 2, x - 3, x + reach, outline);
    if (row.first < 0) { return b; }
    b.x = row.first;
    b.w = row.len();
    const Run col = colRun(f, b.x + 12, y - 3, y + h + 3, outline);
    if (col.first < 0) { return b; }
    b.y = col.first;
    b.h = col.len();
    return b;
}

// What a key of the sheet must measure and paint.
struct Want {
    int x, y, h;      // where the sheet puts it and how tall it is
    int minW, maxW;   // its width window (a hugging key is as wide as its label)
    Rgb outline;      // border colour (or the lit fill, which is the border colour too)
    Rgb ground;       // fill inside, left of the label
    Rgb alt;          // a second colour that probe may land on (the hazard stripes alternate under it)
    int reach = 152;  // how far right of x the outline is searched: under the distance to the next key
    Want(int x_, int y_, int h_, int minW_, int maxW_, Rgb outline_, Rgb ground_)
        : x(x_), y(y_), h(h_), minW(minW_), maxW(maxW_), outline(outline_), ground(ground_), alt(ground_) {}
    Want& orStripe(Rgb a) { alt = a; return *this; }
    Want& narrow(int r) { reach = r; return *this; }
};
Box checkKey(const Frame& f, const char* what, const Want& w) {
    Scope scope(what);
    const Box b = measure(f, w.x, w.y, w.h, w.outline, w.reach);
    if (verbose()) { std::printf("  %-18s box x=%d y=%d w=%d h=%d\n", what, b.x, b.y, b.w, b.h); }
    CHECK(b.x == w.x);
    CHECK(b.y == w.y);
    CHECK(b.h == w.h);
    CHECK(b.w >= w.minW && b.w <= w.maxW);
    CHECK(f.near(b.x + 4, b.midY(), w.ground, 4) || f.near(b.x + 4, b.midY(), w.alt, 4));
    return b;
}

// Ink: pixels of the region that are not the ground (a label was drawn there), and pixels near one colour.
int ink(const Frame& f, const Box& b, Rgb ground) { return f.countInk(b.x + 3, b.y + 3, b.x + b.w - 3, b.y + b.h - 3, ground, 30); }
int tint(const Frame& f, const Box& b, Rgb colour, int tol = 24) { return f.countNear(b.x + 3, b.y + 3, b.x + b.w - 3, b.y + b.h - 3, colour, tol); }

// Wherever two colours alternate along a row: how many times the pixel flips between a and b.
int flips(const Frame& f, int y, int x0, int x1, Rgb a, Rgb b) {
    int n = 0;
    int last = 0;  // 1 = a, 2 = b
    for (int x = x0; x < x1; ++x) {
        const int now = f.near(x, y, a, 3) ? 1 : f.near(x, y, b, 3) ? 2 : 0;
        if (now != 0 && last != 0 && now != last) { ++n; }
        if (now != 0) { last = now; }
    }
    return n;
}

}  // namespace

namespace {

using Ui = slint::ComponentHandle<KeysSpecimen>;

// the first and last column (or row) in the region that holds any ink: how wide (tall) the drawn thing is
Run inkSpanX(const Frame& f, int x0, int y0, int x1, int y1, Rgb ground, int tol = 24) {
    Run r;
    for (int x = x0; x < x1; ++x) {
        if (f.countInk(x, y0, x + 1, y1, ground, tol) > 0) {
            if (r.first < 0) { r.first = x; }
            r.last = x;
        }
    }
    return r;
}

// The focus ring around a key at (x, y, w, h): 2 px of focus colour, 1 px of the ground clear of the key.
bool ringAround(const Frame& g, int x, int y, int w, int h) {
    return g.near(x - 3, y + h / 2, kFocus, 14) && g.near(x - 2, y + h / 2, kFocus, 14)
        && g.near(x + w + 1, y + h / 2, kFocus, 14) && g.near(x + w + 2, y + h / 2, kFocus, 14)
        && g.near(x + w / 2, y - 3, kFocus, 14) && g.near(x + w / 2, y - 2, kFocus, 14)
        && g.near(x + w / 2, y + h + 1, kFocus, 14) && g.near(x + w / 2, y + h + 2, kFocus, 14);
}

// Row 1 and 2: the plain key and its modifiers, 30 high.
void checkPlainKeys(const Frame& f) {
    Scope scope("plain keys");
    const Box shortKey = checkKey(f, "plain short", {16, 24, 30, 64, 64, kKeyLine, kKeyBg});
    CHECK(shortKey.w == 64);                                      // .btn min-width: 64 for a short label
    CHECK(ink(f, shortKey, kKeyBg) > 15);                         // "OK" is drawn
    CHECK(tint(f, shortKey, kKeyFg, 12) > 3);                     // in key-fg
    CHECK(!f.near(shortKey.x, shortKey.y, kKeyLine, 6));          // radius 6 cuts the corner pixel
    CHECK(f.near(shortKey.x - 1, shortKey.midY(), kBg));          // nothing outside the border
    CHECK(f.near(shortKey.x + shortKey.w, shortKey.midY(), kBg));
    CHECK(f.near(shortKey.midX(), shortKey.y - 1, kBg));
    CHECK(f.near(shortKey.midX(), shortKey.y + shortKey.h, kBg));

    const Box longKey = checkKey(f, "plain long", {170, 24, 30, 100, 150, kKeyLine, kKeyBg});
    CHECK(longKey.w > shortKey.w);                                // a longer label makes a wider key
    const Box latchKey = checkKey(f, "latched, off", {324, 24, 30, 64, 150, kKeyLine, kKeyBg});
    CHECK(latchKey.w >= 64);
    // ghost: the window ground shows through, the border stays
    const Box ghostKey = checkKey(f, "ghost", {478, 24, 30, 64, 150, kKeyLine, kBg});
    CHECK(f.near(ghostKey.midX(), ghostKey.y + 4, kBg, 3));
    CHECK(tint(f, ghostKey, kKeyFg, 12) > 3);
    // danger: danger-bg ground, danger-line border, danger-fg text
    const Box dangerKey = checkKey(f, "danger", {632, 24, 30, 64, 150, kDangerLine, kDangerBg});
    CHECK(tint(f, dangerKey, kDangerFg, 30) > 3);
    // plate: transparent, an accent outline, plate-fg text
    const Box plateKey = checkKey(f, "plate", {16, 78, 30, 64, 150, kPlateLine, kBg});
    CHECK(tint(f, plateKey, kPlateFg, 30) > 3);

    // disabled: key-disabled-bg ground, key-disabled-fg text, the border at half alpha over the ground
    const Rgb disLine = over(kKeyLine, 0.5, kDisBg);
    const Box disKey = checkKey(f, "disabled", {170, 78, 30, 64, 150, disLine, kDisBg});
    CHECK(tint(f, disKey, kDisFg, 30) > 3);
    CHECK(tint(f, disKey, kKeyFg, 12) == 0);                      // not the enabled text colour
    CHECK(!nearColour(disLine, kKeyLine, 6));                     // and the border really is dimmer
    const Box disOn = checkKey(f, "disabled, on", {324, 78, 30, 64, 150, over(kAccent, 0.5, kDisBg), kDisBg});
    CHECK(tint(f, disOn, kAccent, 12) == 0);                      // a disabled latched key is not lit
    checkKey(f, "disabled ghost", {478, 78, 30, 64, 150, disLine, kDisBg});   // the disabled ground wins over ghost
    checkKey(f, "right-click target", {632, 78, 30, 64, 150, kKeyLine, kKeyBg});
}

// Row 3: the hazard key (BIAS TEE): 36 high, min width 120, stripes of key-bg and hazard-stripe at 135 degrees.
void checkHazardKeys(const Frame& f) {
    Scope scope("hazard keys");
    const Box bias = checkKey(f, "hazard", Want{16, 132, 36, 120, 150, kHazardLine, kKeyBg}.orStripe(kHazardStripe));
    CHECK(bias.w == 120);                                         // the sheet min-width:120 (the label is shorter)

    // above the label and below the key top: only ground
    const int ax0 = bias.x + 4, ax1 = bias.x + bias.w - 4, ay0 = bias.y + 3, ay1 = bias.y + 10;
    const int nBg = f.countNear(ax0, ay0, ax1, ay1, kKeyBg, 3);
    const int nStripe = f.countNear(ax0, ay0, ax1, ay1, kHazardStripe, 3);
    if (verbose()) { std::printf("  hazard ground: key-bg %d, stripe %d of %d\n", nBg, nStripe, (ax1 - ax0) * (ay1 - ay0)); }
    CHECK(nBg > 100);                                             // both stripe colours are in the key
    CHECK(nStripe > 100);
    CHECK(nBg + nStripe > (ax1 - ax0) * (ay1 - ay0) * 9 / 10);    // and nothing but those two

    // the bands run along x + y = const (135 degrees): same colour along that line, different across it
    const auto cls = [&](int x, int y) { return f.near(x, y, kKeyBg, 3) ? 1 : f.near(x, y, kHazardStripe, 3) ? 2 : 0; };
    int along = 0, alongBad = 0, across = 0, acrossBad = 0, period = 0, periodBad = 0;
    for (int y = bias.y + 5; y <= bias.y + 9; ++y) {
        for (int x = bias.x + 4; x < bias.x + bias.w - 8; ++x) {
            const int a = cls(x, y), b = cls(x + 3, y - 3);
            if (a != 0 && b != 0) { ++along; if (a != b) { ++alongBad; } }
        }
    }
    for (int y = bias.y + 3; y <= bias.y + 6; ++y) {
        for (int x = bias.x + 4; x < bias.x + bias.w - 8; ++x) {
            const int a = cls(x, y), b = cls(x + 3, y + 3);
            if (a != 0 && b != 0) { ++across; if (a != b) { ++acrossBad; } }
        }
    }
    for (int y = bias.y + 3; y <= bias.y + 9; ++y) {
        for (int x = bias.x + 4; x < bias.x + bias.w - 4 - 23; ++x) {
            const int a = cls(x, y), b = cls(x + 23, y);
            if (a != 0 && b != 0) { ++period; if (a != b) { ++periodBad; } }
        }
    }
    if (verbose()) { std::printf("  hazard bands: along %d/%d differ, across %d/%d differ, period %d/%d differ\n", alongBad, along, acrossBad, across, periodBad, period); }
    CHECK(along > 300 && alongBad * 50 <= along);                 // <= 2 % of the pairs on one diagonal differ
    CHECK(across > 200 && acrossBad * 4 >= across);               // >= 25 % of the pairs across it differ
    CHECK(period > 300 && periodBad * 50 <= period);              // the pattern repeats every 23 px
    CHECK(flips(f, bias.y + 4, bias.x + 3, bias.x + bias.w - 3, kKeyBg, kHazardStripe) >= 6);   // 8 px bands: ~10 along 120 px

    // the lamp before the label: off = a ring in lamp-ring
    CHECK(f.countNear(bias.x + 10, bias.y + 10, bias.x + 38, bias.y + 26, kLampRing, 8) >= 8);
    CHECK(f.countNear(bias.x + 10, bias.y + 10, bias.x + 38, bias.y + 26, kLampAmber, 12) == 0);

    const Box lit = checkKey(f, "hazard, lamp lit", Want{170, 132, 36, 120, 150, kHazardLine, kKeyBg}.orStripe(kHazardStripe));
    CHECK(f.countNear(lit.x + 10, lit.y + 10, lit.x + 38, lit.y + 26, kLampAmber, 12) >= 30);   // a lit lamp: a filled amber dot

    // disabled: key-disabled-bg, no stripes at all, the hazard border at half alpha
    const Box dis = checkKey(f, "hazard disabled", {324, 132, 36, 120, 150, over(kHazardLine, 0.5, kDisBg), kDisBg});
    CHECK(f.countNear(dis.x + 4, dis.y + 3, dis.x + dis.w - 4, dis.y + 10, kDisBg, 3) == (dis.w - 8) * 7);   // flat ground, no stripes
}

// Rows 4 and 5: small (24, 11 px, padding 10, no min width) and xs (20, 10 px, padding 8).
void checkSmallKeys(const Frame& f) {
    Scope scope("small and xs keys");
    const Box sm = checkKey(f, "small", {16, 194, 24, 20, 60, kKeyLine, kKeyBg});
    CHECK(sm.w < 64);                                             // .btn.sm: min-width 0
    const Box smOn = checkKey(f, "small, on", {170, 194, 24, 60, 150, kAccent, kAccent});
    CHECK(tint(f, smOn, kAccentInk, 30) > 3);                     // accent-ink text on the accent ground
    checkKey(f, "small ghost", {324, 194, 24, 40, 150, kKeyLine, kBg});
    checkKey(f, "small plate", {478, 194, 24, 30, 150, kPlateLine, kBg});
    checkKey(f, "small danger", {632, 194, 24, 30, 150, kDangerLine, kDangerBg});

    const Box xs = checkKey(f, "xs", {16, 248, 20, 20, 60, kKeyLine, kKeyBg});
    CHECK(xs.w < 64);
    CHECK(xs.w < sm.w + 4);                                       // the same label in the smaller key: not wider
    const Box xsOn = checkKey(f, "xs, on", {170, 248, 20, 40, 150, kAccent, kAccent});
    CHECK(tint(f, xsOn, kAccentInk, 40) > 2);
    checkKey(f, "xs ghost", {324, 248, 20, 30, 150, kKeyLine, kBg});
    const Box xsAuto = checkKey(f, "xs AUTO", {478, 248, 20, 48, 60, kKeyLine, kKeyBg});
    CHECK(xsAuto.w >= 48);                                        // the AUTO key of the receiver strip: min-width 48
    checkKey(f, "xs disabled", {632, 248, 20, 30, 150, over(kKeyLine, 0.5, kDisBg), kDisBg});
}

// Row 6: icon-only keys (square), the flat window button, icon + label.
void checkIconKeys(const Frame& f) {
    Scope scope("icon keys");
    const Box close = checkKey(f, "icon-only ghost", Want{16, 298, 30, 30, 30, kKeyLine, kBg}.narrow(36));
    CHECK(close.w == 30);                                         // .btn.ico: square, 30 x 30
    CHECK(f.countNear(close.x + 6, close.y + 6, close.x + 24, close.y + 24, kFgSoft, 24) >= 6);   // the glyph in fg-soft
    CHECK(f.countNear(close.x + 6, close.y + 6, close.x + 24, close.y + 24, kKeyFg, 6) == 0);
    const Box bell = checkKey(f, "icon-only plain", Want{56, 298, 30, 30, 30, kKeyLine, kKeyBg}.narrow(36));
    CHECK(bell.w == 30);
    CHECK(f.countNear(bell.x + 6, bell.y + 6, bell.x + 24, bell.y + 24, kFgSoft, 24) >= 6);
    const Box on = checkKey(f, "icon-only on", Want{96, 298, 30, 30, 30, kAccent, kAccent}.narrow(36));
    CHECK(on.w == 30);
    CHECK(f.countNear(on.x + 6, on.y + 6, on.x + 24, on.y + 24, kAccentInk, 40) >= 6);            // the glyph in accent-ink
    const Box dis = checkKey(f, "icon-only disabled", Want{136, 298, 30, 30, 30, kDisLine, kDisBg}.narrow(36));
    CHECK(dis.w == 30);
    CHECK(f.countNear(dis.x + 6, dis.y + 6, dis.x + 24, dis.y + 24, kDisFg, 40) >= 4);
    const Box small = checkKey(f, "icon-only small", Want{176, 298, 24, 24, 24, kKeyLine, kKeyBg}.narrow(30));
    CHECK(small.w == 24);                                         // square at the small height
    CHECK(f.countNear(small.x + 4, small.y + 4, small.x + 20, small.y + 20, kFgSoft, 24) >= 4);

    // flat (the window chrome): 26 x 22, no border, no ground, glyph in fg-soft
    CHECK(f.rowNear(302, 218, 232, kBg, 3));                      // top edge: ground, no border
    CHECK(f.rowNear(323, 218, 232, kBg, 3));                      // bottom edge
    CHECK(f.colNear(212, 306, 320, kBg, 3));                      // left edge
    CHECK(f.colNear(237, 306, 320, kBg, 3));                      // right edge
    CHECK(f.countNear(214, 304, 236, 322, kFgSoft, 24) >= 6);

    const Box label = checkKey(f, "icon + label", {324, 298, 30, 110, 150, kKeyLine, kKeyBg});
    CHECK(f.countInk(label.x + 10, label.y + 5, label.x + 36, label.y + 25, kKeyBg, 30) >= 20);                       // the icon, left
    CHECK(f.countInk(label.x + 40, label.y + 5, label.x + label.w - 10, label.y + 25, kKeyBg, 30) >= 30);             // the label, right
    const Box labelOn = checkKey(f, "icon + label, on", {478, 298, 30, 80, 150, kAccent, kAccent});
    CHECK(f.countNear(labelOn.x + 10, labelOn.y + 5, labelOn.x + 36, labelOn.y + 25, kAccentInk, 40) >= 6);
    const Box focus = checkKey(f, "focus target", {632, 298, 30, 64, 150, kKeyLine, kKeyBg});
    CHECK(f.near(focus.x - 2, focus.midY(), kBg));                // no focus ring before it has the focus
}

// Row 7: wide keys in layouts. The plates are the parents of the layouts; the keys are measured from the pixels.
void checkWideKeys(const Frame& f, const Ui& ui) {
    Scope scope("wide keys");
    const Box column = measure(f, 20, 356, 30, kKeyLine, 220);
    CHECK(column.x == 20 && column.w == 192 && column.h == 30);   // fills the 200 px column less 4 + 4 of padding
    CHECK_NEAR(ui->get_wide_v_w(), 192.0, 0.01);
    const Box small = measure(f, 20, 390, 24, kPlateLine, 220);
    CHECK(small.x == 20 && small.w == 192 && small.h == 24);      // a wide small key fills it too

    // a wide on key and a ghost key in a row: the wide one takes everything the ghost key leaves
    CHECK_NEAR(ui->get_wide_h_w() + ui->get_ghost_h_w() + 4 + 8, 280.0, 0.01);
    CHECK(ui->get_ghost_h_w() >= 72);                             // .btn min-w 72
    CHECK(ui->get_wide_h_w() > 150);
    const Box on = measure(f, 236, 356, 26, kAccent, 280);
    CHECK(on.x == 236 && on.h == 26);
    CHECK_NEAR(on.w, ui->get_wide_h_w(), 0.51);
    CHECK_NEAR(ui->get_ghost_h_x(), ui->get_wide_h_x() + ui->get_wide_h_w() + 4, 0.01);
    CHECK(f.near(int(ui->get_ghost_h_x()) + 4, 356 + 13, kPanel, 3));   // the ghost key shows the plate through it

    // two wide keys share the row, neither under its label
    CHECK_NEAR(ui->get_wide2_a_w() + ui->get_wide2_b_w() + 4 + 8, 252.0, 0.01);
    CHECK(ui->get_wide2_a_w() > 64 && ui->get_wide2_b_w() > 64);
    const Box stop = measure(f, 532, 356, 30, kDangerLine, 252);
    CHECK(stop.x == 532 && stop.h == 30);
    CHECK_NEAR(stop.w, ui->get_wide2_a_w(), 0.51);
}

// Row 8: SegGroup and SegKey.
void checkSegGroups(const Frame& f, const Ui& ui) {
    Scope scope("SegGroup");
    // the group built from a model: 8 keys, the first lit
    const Box g = measure(f, 16, 444, 32, kLineStrong, 470);
    CHECK(g.x == 16 && g.y == 444 && g.h == 32);                  // 26 + 2 + 2 padding + 2 border
    CHECK(g.w > 300 && g.w < 470);
    CHECK(f.near(g.x + 1, g.midY(), kPanelAlt, 3));               // the padding shows the group ground
    CHECK(!f.near(g.x, g.y, kLineStrong, 6));                     // radius 7 cuts the corner
    const Run lit = rowRun(f, g.midY(), g.x + 1, g.x + g.w - 1, kAccent);
    CHECK(lit.first == g.x + 3);                                  // 1 px border + 2 px padding
    CHECK(lit.len() >= 52);                                       // .seg .btn min-width
    const Run height = colRun(f, lit.first + 8, g.y, g.y + g.h, kAccent);
    CHECK(height.first == g.y + 3 && height.len() == 26);         // .seg .btn: 26 px
    CHECK(f.countNear(lit.first + 3, g.y + 6, lit.last - 3, g.y + g.h - 6, kAccentInk, 40) > 2);   // accent-ink label
    CHECK(f.near(lit.last + 7, g.midY(), kPanelAlt, 3));          // the next key is bare: the group ground
    CHECK(f.countNear(lit.last + 4, g.y + 6, lit.last + 50, g.y + g.h - 6, kFgSoft, 30) > 2);     // its label: fg-soft

    // three explicit keys; the last is disabled
    const Box g2 = measure(f, 16, 482, 32, kLineStrong, 300);
    CHECK(g2.x == 16 && g2.y == 482 && g2.h == 32);
    const int skC = int(ui->get_sk_c_x() + 0.5f);
    CHECK(f.countNear(skC + 3, 482 + 6, skC + 40, 482 + 26, kDisFg, 30) > 2);
    CHECK(f.countNear(skC + 3, 482 + 6, skC + 40, 482 + 26, kFgSoft, 6) == 0);

    // the small group: 28 high, its keys 22
    const Box g3 = measure(f, 520, 444, 28, kLineStrong, 270);
    CHECK(g3.x == 520 && g3.y == 444 && g3.h == 28);              // 22 + 2 + 2 + 2
    const Run litS = rowRun(f, g3.midY(), g3.x + 1, g3.x + g3.w - 1, kAccent);
    CHECK(litS.first == int(ui->get_tr_b_x() + 0.5f));            // the pixel agrees with the layout: key two is lit
    const Run heightS = colRun(f, litS.first + 8, g3.y, g3.y + g3.h, kAccent);
    CHECK(heightS.first == g3.y + 3 && heightS.len() == 22);      // .seg.sm .btn: 22 px
    CHECK(litS.len() < 90);                                       // no min width on a small key

    // a group with a HorizontalLayout child: the same ground, border and padding
    const Box g4 = measure(f, 520, 482, 32, kLineStrong, 270);
    CHECK(g4.x == 520 && g4.y == 482 && g4.h == 32);
    const Run litN = rowRun(f, g4.midY(), g4.x + 1, g4.x + g4.w - 1, kAccent);
    CHECK(litN.first == g4.x + 3);
    CHECK(litN.last + 3 == int(ui->get_nest_b_x() + 0.5f));       // 2 px between the keys
}

// Row 9: RailKey, StopKey, Caption.
void checkRailStopCaption(const Frame& f) {
    Scope scope("rail, dome, caption");
    // lit rail key: 56 x 56, the menu ground, accent icon and caption
    {
        Scope inner("rail key, lit");
        const Run row = rowRun(f, 538 + 28, 8, 78, kMenu, 3);
        CHECK(row.first == 16 && row.len() == 56);
        const Run col = colRun(f, 16 + 28, 530, 600, kMenu, 3);
        CHECK(col.first == 538 && col.len() == 56);
        CHECK(!f.near(16, 538, kMenu, 3));                        // radius 8 cuts the corner
        CHECK(f.countNear(16 + 14, 538 + 3, 16 + 42, 538 + 25, kAccent, 40) >= 12);   // the icon
        CHECK(f.countNear(16 + 4, 538 + 24, 16 + 52, 538 + 40, kAccent, 60) >= 5);    // the caption
        CHECK(f.countInk(16 + 14, 538 + 40, 16 + 42, 538 + 54, kMenu, 24) >= 4);      // the F1 hint under it
        CHECK(f.countNear(16 + 14, 538 + 40, 16 + 42, 538 + 54, kFaint, 40) >= 2);    // in faint
    }
    {
        Scope inner("rail key, unlit");
        CHECK(f.near(80 + 4, 538 + 28, kBg, 3));                  // no ground
        CHECK(f.countNear(80 + 14, 538 + 3, 80 + 42, 538 + 25, kMuted, 40) >= 8);     // icon in muted
        CHECK(f.countNear(80 + 4, 538 + 3, 80 + 52, 538 + 54, kAccent, 40) == 0);     // nothing lit
        CHECK(f.countInk(80 + 4, 538 + 24, 80 + 52, 538 + 40, kBg, 24) >= 10);        // the caption
    }
    {
        Scope inner("rail key, no hint");
        // FOLD has no hint: the band where F1 sits on the lit key is empty here, the content is centred instead
        CHECK(f.countInk(144 + 6, 538 + 47, 144 + 50, 538 + 54, kBg, 24) == 0);
        CHECK(f.countInk(144 + 14, 538 + 8, 144 + 42, 538 + 46, kBg, 24) >= 15);
    }
    {
        Scope inner("rail key, disabled");
        CHECK(f.near(208 + 4, 538 + 28, kBg, 3));
        CHECK(f.countNear(208 + 14, 538 + 3, 208 + 42, 538 + 25, kDisFg, 40) >= 8);
        CHECK(f.countNear(208 + 4, 538 + 3, 208 + 52, 538 + 54, kAccent, 40) == 0);
    }

    // the dome: 92 x 64, danger-bg, a danger-strong-line border, a 12 px glyph in danger-glyph, the word in danger-fg
    int wordWidth[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        Scope inner(i == 0 ? "StopKey, running" : "StopKey, stopped");
        const int x = i == 0 ? 292 : 396;
        const Box b = measure(f, x, 534, 64, kDangerStrong, 100);
        if (verbose()) { std::printf("  dome %d: x=%d y=%d w=%d h=%d\n", i, b.x, b.y, b.w, b.h); }
        CHECK(b.x == x && b.y == 534 && b.w == 92 && b.h == 64);
        CHECK(f.near(b.x + 6, b.midY(), kDangerBg, 4));
        CHECK(!f.near(b.x, b.y, kDangerStrong, 6));               // radius 10 cuts the corner
        const Run glyphRow = rowRun(f, b.y + 23, b.x + 20, b.x + 72, kDangerGlyph, 8);
        const Run glyphCol = colRun(f, b.x + 46, b.y + 8, b.y + 38, kDangerGlyph, 8);
        if (verbose()) { std::printf("  glyph row %d..%d (%d), col %d..%d (%d)\n", glyphRow.first, glyphRow.last, glyphRow.len(), glyphCol.first, glyphCol.last, glyphCol.len()); }
        CHECK(glyphRow.len() == 12);                              // the 12 px rounded square
        CHECK(glyphRow.first == b.x + 40);                        // centred: (92 - 12) / 2
        CHECK(glyphCol.len() >= 11 && glyphCol.len() <= 13);
        CHECK(f.countNear(b.x + 30, b.y + 10, b.x + 62, b.y + 34, kDangerGlyph, 8) >= 120);
        CHECK(f.countNear(b.x + 14, b.y + 34, b.x + 78, b.y + 58, kDangerFg, 30) >= 12);   // the word
        const Run word = inkSpanX(f, b.x + 6, b.y + 34, b.x + 86, b.y + 58, kDangerBg);
        wordWidth[i] = word.len();
        if (verbose()) { std::printf("  word spans %d..%d (%d)\n", word.first, word.last, word.len()); }
        CHECK(std::abs((word.first + word.last) / 2 - b.midX()) <= 2);   // centred under the glyph
    }
    CHECK(wordWidth[1] >= wordWidth[0] + 2);                      // START is wider than STOP, in any proportional typeface

    // captions: 10 px / 600 / 0.8 px, upper-case, muted
    const Run upper = inkSpanX(f, 520, 538, 700, 554, kBg);
    const Run mixed = inkSpanX(f, 520, 558, 700, 574, kBg);
    if (verbose()) { std::printf("  caption widths: upper %d, mixed %d\n", upper.len(), mixed.len()); }
    CHECK(upper.len() > 60 && mixed.len() > 40);
    CHECK(upper.len() >= mixed.len() + 4);                        // upper-case letters are wider than the same words in lower case
    CHECK(f.countNear(520, 538, 700, 554, kMuted, 24) >= 5);      // the default colour is muted
    CHECK(f.countNear(520, 538, 700, 554, kFg, 12) == 0);
    CHECK(f.countNear(520, 578, 700, 594, kFg, 12) >= 3);         // colour overridden to fg
    CHECK(f.countNear(520, 578, 700, 594, kMuted, 6) == 0);
}

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = KeysSpecimen::create();
    slint::Window& win = ui->window();

    // ======================= the sheet at rest: every variant against the CSS =======================
    Frame f = render(ui, kW, kH);
    CHECK(f.width == kW && f.height == kH);
    CHECK(f.near(795, 5, kBg));                                   // Theme.bg, the rail ground
    f.savePng(shotDir() + "keys-specimen.png");

    checkPlainKeys(f);
    checkHazardKeys(f);
    checkSmallKeys(f);
    checkIconKeys(f);
    checkWideKeys(f, ui);
    checkSegGroups(f, ui);
    checkRailStopCaption(f);

    // ======================= interaction, delivered the way the OS delivers it =======================
    const auto last = [&]() { return std::string(std::string_view(ui->get_last_clicked())); };
    const auto away = [&]() { moveTo(win, 795.0f, 715.0f); settle(); };
    const auto at = [&](const Box& b, slint::PointerEventButton button = slint::PointerEventButton::Left) {
        click(win, float(b.midX()), float(b.midY()), button);
    };
    const auto shot = [&]() { return render(ui, kW, kH); };

    const Box shortKey = measure(f, 16, 24, 30, kKeyLine);
    const Box latchKey = measure(f, 324, 24, 30, kKeyLine);
    const Box ghostKey = measure(f, 478, 24, 30, kKeyLine);
    const Box dangerKey = measure(f, 632, 24, 30, kDangerLine);
    const Box rightKey = measure(f, 632, 78, 30, kKeyLine);
    const Box biasKey = measure(f, 16, 132, 36, kHazardLine);

    // --- a click fires clicked, once ---
    {
        Scope scope("click");
        CHECK(ui->get_clicks() == 0);
        at(shortKey);
        CHECK(ui->get_clicks() == 1);
        CHECK(last() == "short");
        at(shortKey);
        CHECK(ui->get_clicks() == 2);
        at(ghostKey);
        CHECK(ui->get_clicks() == 3 && last() == "ghost");
        at(dangerKey);
        CHECK(ui->get_clicks() == 4 && last() == "danger");
        at(measure(f, 16, 78, 30, kPlateLine));
        CHECK(ui->get_clicks() == 5 && last() == "plate");
    }

    // --- a disabled key never fires (click, in any variant), and fires nothing else either ---
    {
        Scope scope("disabled keys do not act");
        const int before = ui->get_clicks();
        at(measure(f, 170, 78, 30, kDisLine));                              // disabled
        at(measure(f, 324, 78, 30, over(kAccent, 0.5, kDisBg)));            // disabled, on
        at(measure(f, 478, 78, 30, kDisLine));                              // disabled ghost
        at(measure(f, 324, 132, 36, over(kHazardLine, 0.5, kDisBg)));       // disabled hazard
        at(measure(f, 632, 248, 20, kDisLine));                             // disabled xs
        at(measure(f, 136, 298, 30, kDisLine));                             // disabled icon-only
        click(win, 208.0f + 28, 538.0f + 28);                               // disabled rail key
        click(win, ui->get_sk_c_x() + 12, 482.0f + 16);                     // disabled segmented key
        click(win, 532.0f + ui->get_wide2_a_w() + 4 + ui->get_wide2_b_w() / 2, 356.0f + 15);   // disabled wide key
        CHECK(ui->get_clicks() == before);
        CHECK(last() == "plate");
        CHECK(ui->get_mode() == 0);                                         // the disabled segmented key would have set 2
        CHECK(ui->get_rail() == 0);                                         // the disabled rail key would have set 3
        click(win, 292.0f + 46, 534.0f + 32);                               // a StopKey is enabled: clicks do reach this window
        CHECK(ui->get_clicks() == before + 1 && last() == "stop-run");
        click(win, 292.0f + 46, 534.0f + 32);                               // and back to running
        CHECK(ui->get_running());
    }
    const int base = ui->get_clicks();

    // --- a latched key: a click flips it, it lights accent, a hover brightens it ---
    {
        Scope scope("latched key");
        at(latchKey);
        CHECK(ui->get_latched() && ui->get_clicks() == base + 1 && last() == "latch");
        away();
        Frame g = shot();
        CHECK(g.near(latchKey.x + 4, latchKey.midY(), kAccent, 4));         // accent ground
        CHECK(g.near(latchKey.x + 20, latchKey.y, kAccent, 4));             // accent border
        CHECK(tint(g, latchKey, kAccentInk, 30) > 3);                       // accent-ink text
        moveTo(win, float(latchKey.x + 8), float(latchKey.midY()));
        g = shot();
        CHECK(g.near(latchKey.x + 4, latchKey.midY(), kAccentHover, 4));    // hovered: accent-hover
        at(latchKey);
        CHECK(!ui->get_latched());
        away();
        g = shot();
        CHECK(g.near(latchKey.x + 4, latchKey.midY(), kKeyBg, 4));          // off again
    }

    // --- hover: menu-hot on a plain or ghost key, danger-line on a danger key, nothing on a disabled one ---
    {
        Scope scope("hover ground");
        away();
        moveTo(win, float(shortKey.x + 8), float(shortKey.midY()));
        Frame g = shot();
        CHECK(g.near(shortKey.x + 4, shortKey.midY(), kMenuHot, 3));
        CHECK(g.near(shortKey.x + 20, shortKey.y, kKeyLine, 6));            // the border does not move
        moveTo(win, float(ghostKey.x + 8), float(ghostKey.midY()));
        g = shot();
        CHECK(g.near(ghostKey.x + 4, ghostKey.midY(), kMenuHot, 3));
        CHECK(g.near(shortKey.x + 4, shortKey.midY(), kKeyBg, 3));          // the first key let go
        moveTo(win, float(dangerKey.x + 8), float(dangerKey.midY()));
        g = shot();
        CHECK(g.near(dangerKey.x + 4, dangerKey.midY(), kDangerLine, 4));
        moveTo(win, 170.0f + 8, 78.0f + 15);                                // the disabled key
        g = shot();
        CHECK(g.near(170 + 4, 78 + 15, kDisBg, 3));
        // the flat window button: 26 x 22 of menu-hot while hovered, nothing at rest
        moveTo(win, 212.0f + 13, 302.0f + 11);
        g = shot();
        const Run w = rowRun(g, 302 + 11, 204, 244, kMenuHot, 3);
        const Run h = colRun(g, 212 + 13, 296, 330, kMenuHot, 3);
        CHECK(w.first == 212 && w.len() == 26);
        CHECK(h.first == 302 && h.len() == 22);
        away();
    }

    // --- a right-click fires right-clicked with the pointer position, and is not a click ---
    {
        Scope scope("right click");
        const int before = ui->get_clicks();
        at(rightKey, slint::PointerEventButton::Right);
        CHECK(ui->get_right_clicks() == 1);
        CHECK(ui->get_clicks() == before);                                  // not a left click
        CHECK_NEAR(ui->get_right_x(), rightKey.w / 2, 1.5);                 // position inside the key
        CHECK_NEAR(ui->get_right_y(), rightKey.h / 2, 1.5);
        at(rightKey);
        CHECK(ui->get_clicks() == before + 1 && last() == "right");
        CHECK(ui->get_right_clicks() == 1);                                 // a left click is not a right-click
    }

    // --- the BIAS TEE lamp follows the key ---
    {
        Scope scope("hazard lamp");
        const auto lamp = [&](const Frame& g) { return g.countNear(biasKey.x + 10, biasKey.y + 10, biasKey.x + 38, biasKey.y + 26, kLampAmber, 12); };
        CHECK(lamp(f) == 0);
        at(biasKey);
        CHECK(ui->get_bias() && last() == "bias");
        away();
        Frame g = shot();
        CHECK(lamp(g) >= 30);
        CHECK(g.near(biasKey.x + 20, biasKey.y, kHazardLine, 6));           // still the hazard border, not accent
        CHECK(tint(g, biasKey, kAccentInk, 6) == 0);                       // the label is not accent-ink: a hazard key ignores on
        at(biasKey);
        CHECK(!ui->get_bias());
        away();
        g = shot();
        CHECK(lamp(g) == 0);
    }

    // --- the segmented keys: a click selects, the key built from a model reports its index, a disabled one stays put ---
    {
        Scope scope("segmented keys");
        const Run litBefore = rowRun(f, 444 + 16, 17, 470, kAccent);       // the first key of the model-built group
        const int skB = int(ui->get_sk_b_x() + 0.5f);
        click(win, float(skB + 12), 482.0f + 16);
        CHECK(ui->get_mode() == 1 && last() == "sk-b");
        away();
        Frame g = shot();
        CHECK(g.near(skB + 12, 482 + 16, kAccent, 4));                      // key two of the explicit group is lit
        CHECK(g.near(16 + 3 + 8, 482 + 16, kPanelAlt, 3));                  // and key one let go
        const Run litNow = rowRun(g, 444 + 16, 17, 470, kAccent);          // the model-built group follows the same state
        CHECK(litNow.first == litBefore.last + 3);
        CHECK(litNow.len() >= 52);

        // the first and the third key of the model-built group: indices 0 and 2 arrive through the repeater
        const int third = litNow.last + 3;                                  // key two spans litNow, key three follows it
        click(win, float(litBefore.first + 12), 444.0f + 16);
        CHECK(ui->get_mode() == 0 && last() == "mode-0");
        click(win, float(third + 12), 444.0f + 16);
        CHECK(ui->get_mode() == 2 && last() == "mode-2");
        away();
        g = shot();
        const Run lit2 = rowRun(g, 444 + 16, 17, 470, kAccent);
        CHECK(lit2.first == third);                                         // key three is lit now
        CHECK(lit2.len() >= 52);

        // the small group: key one lit after its click
        click(win, 520.0f + 3 + 12, 444.0f + 14);
        CHECK(ui->get_trace() == 0 && last() == "tr-a");
        away();
        g = shot();
        const Run litS = rowRun(g, 444 + 14, 521, 700, kAccent);
        CHECK(litS.first == 520 + 3);
        // the nested layout group
        click(win, ui->get_nest_b_x() + 12, 482.0f + 16);
        CHECK(last() == "nest-b");
    }

    // --- the rail key: a click lights it and lets the other go; a hover takes the panel ground ---
    {
        Scope scope("rail key");
        click(win, 80.0f + 28, 538.0f + 28);
        CHECK(ui->get_rail() == 1 && last() == "rk-off");
        away();
        Frame g = shot();
        CHECK(g.near(80 + 4, 538 + 28, kMenu, 3));                          // the clicked key is lit
        CHECK(g.near(16 + 4, 538 + 28, kBg, 3));                            // the first one let go
        CHECK(g.countNear(80 + 14, 538 + 3, 80 + 42, 538 + 25, kAccent, 40) >= 12);
        CHECK(g.countNear(16 + 14, 538 + 3, 16 + 42, 538 + 25, kAccent, 40) == 0);
        moveTo(win, 144.0f + 28, 538.0f + 28);
        g = shot();
        CHECK(g.near(144 + 4, 538 + 28, kPanel, 3));                        // hover on an unlit key
        CHECK(g.near(144 + 1, 538 + 1, kBg, 3));                            // radius 8 cuts the corner of the hover ground too
        away();
    }

    // --- the dome: a click flips running; stopped it is drawn exactly like the second dome ---
    {
        Scope scope("StopKey");
        CHECK(ui->get_running());
        const auto domeDiffers = [&](const Frame& g) {
            int n = 0;
            for (int y = 0; y < 64; ++y) {
                for (int x = 0; x < 92; ++x) {
                    const Rgb a = g.at(292 + x, 534 + y), b = g.at(396 + x, 534 + y);
                    if (a.r != b.r || a.g != b.g || a.b != b.b) { ++n; }
                }
            }
            return n;
        };
        away();
        Frame g = shot();
        CHECK(domeDiffers(g) > 40);                                         // STOP and START differ
        click(win, 292.0f + 46, 534.0f + 32);
        CHECK(!ui->get_running() && last() == "stop-run");
        away();
        g = shot();
        CHECK(domeDiffers(g) == 0);                                         // both say START now
        moveTo(win, 292.0f + 46, 534.0f + 32);
        g = shot();
        CHECK(g.near(292 + 6, 534 + 32, kDangerLine, 4));                   // hovered: the brighter red ground
        click(win, 292.0f + 46, 534.0f + 32);
        CHECK(ui->get_running());
        away();
        g = shot();
        CHECK(domeDiffers(g) > 40);
    }

    // --- the keyboard: the focus ring, Space and Enter press the key, a click elsewhere does not take the focus ---
    {
        Scope scope("keyboard");
        const Box fk = measure(f, 632, 298, 30, kKeyLine);
        const int before = ui->get_clicks();
        ui->invoke_focus_target();
        settle();
        Frame g = shot();
        CHECK(g.near(fk.x - 3, fk.midY(), kFocus, 12) && g.near(fk.x - 2, fk.midY(), kFocus, 12));          // 2 px ring, left
        CHECK(g.near(fk.x + fk.w + 1, fk.midY(), kFocus, 12) && g.near(fk.x + fk.w + 2, fk.midY(), kFocus, 12));   // right
        CHECK(g.near(fk.midX(), fk.y - 3, kFocus, 12) && g.near(fk.midX(), fk.y - 2, kFocus, 12));          // top
        CHECK(g.near(fk.midX(), fk.y + fk.h + 1, kFocus, 12) && g.near(fk.midX(), fk.y + fk.h + 2, kFocus, 12));   // bottom
        CHECK(g.near(fk.x - 1, fk.midY(), kBg, 3) && g.near(fk.x + fk.w, fk.midY(), kBg, 3));               // 1 px clear of the key
        CHECK(g.near(fk.x - 4, fk.midY(), kBg, 3));
        keyTap(win, u8" ");
        CHECK(ui->get_clicks() == before + 1 && last() == "focus");         // Space presses the key
        keyTap(win, slint::platform::key_codes::Return);
        CHECK(ui->get_clicks() == before + 2);                              // so does Enter
        keyTap(win, u8"x");
        CHECK(ui->get_clicks() == before + 2);                              // any other key is not its business
        at(shortKey);                                                       // clicking another key does not move the focus
        CHECK(ui->get_clicks() == before + 3 && last() == "short");
        away();
        g = shot();
        CHECK(g.near(fk.x - 3, fk.midY(), kFocus, 12));                     // the ring is still there
        g.savePng(shotDir() + "keys-focus.png");
    }

    // --- the tooltip: it is the shared TipTimer, so it opens under the key after the hover delay and closes on leave ---
    {
        Scope scope("tooltip");
        const Box tipKey = measure(f, 16, 648, 30, kKeyLine);
        const Box tipDis = measure(f, 170, 648, 30, kDisLine);
        const Box noTip = measure(f, 324, 648, 30, kKeyLine);
        const auto band = [&](const Frame& g, const Box& k) { return g.countNear(k.x - 4, k.y + k.h + 4, k.x + 260, k.y + k.h + 44, kMenu, 3); };
        away();
        Frame g = shot();
        CHECK(band(g, tipKey) == 0 && band(g, tipDis) == 0 && band(g, noTip) == 0);   // the ground below the keys is bare

        moveTo(win, float(tipKey.midX()), float(tipKey.midY()));
        settle(milliseconds(150));
        g = shot();
        CHECK(band(g, tipKey) == 0);                                        // 150 ms: the 600 ms delay has not run out
        settle(milliseconds(600));
        g = shot();
        const int tip = band(g, tipKey);
        if (verbose()) { std::printf("  tooltip face: %d menu-coloured pixels\n", tip); }
        CHECK(tip > 500);                                                   // the face, in the menu colour
        CHECK(g.countInk(tipKey.x + 10, tipKey.y + tipKey.h + 8, tipKey.x + 200, tipKey.y + tipKey.h + 28, kMenu, 40) > 40);   // and its words
        if (verbose()) {
            int x0 = 9999, y0 = 9999, x1 = -1, y1 = -1;
            for (int y = tipKey.y + tipKey.h + 1; y < kH; ++y) {
                for (int x = 0; x < 330; ++x) {
                    if (!g.near(x, y, kBg, 3)) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); }
                }
            }
            std::printf("  tooltip ink box x %d..%d y %d..%d, pixel at x+20 first rows:", x0, x1, y0, y1);
            for (int y = tipKey.y + tipKey.h; y < tipKey.y + tipKey.h + 14; ++y) { std::printf(" %s", hex(g.at(tipKey.x + 20, y)).c_str()); }
            std::printf("\n");
        }
        int firstRow = -1;                                                  // the tip starts 6 px under the key
        for (int y = tipKey.y + tipKey.h; y < tipKey.y + tipKey.h + 40 && firstRow < 0; ++y) {
            if (!g.near(tipKey.x + 20, y, kBg, 3)) { firstRow = y; }
        }
        if (verbose()) { std::printf("  tooltip starts at row %d, key bottom %d\n", firstRow, tipKey.y + tipKey.h); }
        CHECK(firstRow == tipKey.y + tipKey.h + 6);
        g.savePng(shotDir() + "keys-tooltip.png");
        away();
        settle(milliseconds(30));
        g = shot();
        CHECK(band(g, tipKey) == 0);                                        // the pointer left: the tip closed

        moveTo(win, float(tipDis.midX()), float(tipDis.midY()));            // a disabled key explains itself
        settle(milliseconds(750));
        g = shot();
        CHECK(band(g, tipDis) > 300);
        away();
        settle(milliseconds(30));
        moveTo(win, float(noTip.midX()), float(noTip.midY()));              // no text, no tip
        settle(milliseconds(750));
        g = shot();
        CHECK(band(g, noTip) == 0);
        away();
    }

    // --- the hazard key under the pointer: both stripe colours take a tint of menu-hot, the bands stay ---
    {
        Scope scope("hazard hover");
        moveTo(win, float(biasKey.x + 8), float(biasKey.midY()));
        const Frame g = shot();
        const int restBg = g.countNear(biasKey.x + 4, biasKey.y + 3, biasKey.x + biasKey.w - 4, biasKey.y + 10, kKeyBg, 3);
        const int restStripe = g.countNear(biasKey.x + 4, biasKey.y + 3, biasKey.x + biasKey.w - 4, biasKey.y + 10, kHazardStripe, 3);
        CHECK(restBg == 0 && restStripe == 0);                                // every pixel of the band moved off the rest colours
        int edges = 0;                                                        // and the bands are still there: the colour still alternates
        for (int x = biasKey.x + 5; x < biasKey.x + biasKey.w - 4; ++x) {
            if (!g.near(x, biasKey.y + 4, g.at(x - 1, biasKey.y + 4), 2)) { ++edges; }
        }
        CHECK(edges >= 6);
        away();
    }

    // --- the segmented key, the rail key and the dome hold the keyboard focus the same way ---
    {
        Scope scope("keyboard on the other keys");
        const int skB = int(ui->get_sk_b_x() + 0.5f);
        const int skW = int(ui->get_sk_c_x() + 0.5f) - skB - 2;              // key two ends 2 px before key three
        const int before = ui->get_clicks();
        ui->invoke_focus_seg();
        Frame g = shot();
        CHECK(g.near(skB + skW / 2, 482, kFocus, 14) && g.near(skB + skW / 2, 483, kFocus, 14));     // 2 px ring above
        CHECK(g.near(skB + skW / 2, 484, kPanelAlt, 3));                     // 1 px clear, on the group ground
        CHECK(g.near(skB + skW / 2, 512, kFocus, 14) && g.near(skB + skW / 2, 513, kFocus, 14));     // and below
        keyTap(win, u8" ");
        CHECK(ui->get_clicks() == before + 1 && last() == "sk-b" && ui->get_mode() == 1);

        ui->invoke_focus_rail();
        g = shot();
        CHECK(ringAround(g, 16, 538, 56, 56));
        CHECK(g.near(16 - 1, 538 + 28, kBg, 3) && g.near(16 + 56, 538 + 28, kBg, 3));   // 1 px clear
        CHECK(!ringAround(g, 80, 538, 56, 56));
        keyTap(win, slint::platform::key_codes::Return);
        CHECK(ui->get_rail() == 0 && last() == "rk-on");

        ui->invoke_focus_stop();
        g = shot();
        CHECK(ringAround(g, 292, 534, 92, 64));
        CHECK(g.near(292 - 1, 534 + 32, kBg, 3) && g.near(292 + 92, 534 + 32, kBg, 3));
        const bool wasRunning = ui->get_running();
        keyTap(win, u8" ");
        CHECK(ui->get_running() != wasRunning && last() == "stop-run");
        keyTap(win, slint::platform::key_codes::Return);
        CHECK(ui->get_running() == wasRunning);
    }

    // --- Tab moves the focus along the keys in the order of the sheet, skips the disabled ones, and Shift+Tab goes back ---
    {
        Scope scope("tab navigation");
        ui->invoke_focus_target();
        keyTap(win, u8"\t");                                                // FOCUS -> the first key of the wide column
        Frame g = shot();
        CHECK(ringAround(g, 20, 356, 192, 30));
        CHECK(g.near(632 - 3, 298 + 15, kBg, 3));                           // the key that had it let go
        keyTap(win, u8"\t");
        g = shot();
        CHECK(ringAround(g, 20, 390, 192, 24));                             // the second key of the column
        CHECK(!g.near(20 - 3, 356 + 15, kFocus, 14));
        keyTap(win, slint::platform::key_codes::Backtab);
        g = shot();
        CHECK(ringAround(g, 20, 356, 192, 30));                             // and back
        ui->invoke_focus_plate();                                           // PLATE is followed by three disabled keys
        keyTap(win, u8"\t");
        g = shot();
        CHECK(ringAround(g, rightKey.x, rightKey.y, rightKey.w, rightKey.h));
        CHECK(!g.near(170 - 3, 78 + 15, kFocus, 14));                       // the disabled key was passed over
    }

    // --- a second filling: every colour and metric of the keys is read from Theme, none is baked in ---
    {
        Scope scope("second skin");
        away();
        const Frame rest = shot();
        Skin probe = ui->global<Workbench>().get_value();
        const auto col = [](unsigned v) { return slint::Color::from_rgb_uint8(std::uint8_t((v >> 16) & 0xff), std::uint8_t((v >> 8) & 0xff), std::uint8_t(v & 0xff)); };
        probe.key_bg = col(0x203040);
        probe.key_line = col(0x607080);
        probe.accent = col(0x00c040);
        probe.accent_ink = col(0x001000);
        probe.danger_bg = col(0x300030);
        probe.danger_strong_line = col(0xff00ff);
        probe.danger_glyph = col(0x00ffff);
        probe.hazard_stripe = col(0x004040);
        probe.panel_alt = col(0x101030);
        probe.line_strong = col(0xffff00);
        probe.menu = col(0x202060);
        probe.hairline = 2.0f;
        probe.radius = 12.0f;
        ui->global<Theme>().set_skin(probe);
        const Frame s = shot();
        // a plain key: new ground, a 2 px border in the new colour, the corner cut at radius 12
        CHECK(s.near(16 + 5, 24 + 15, rgb(0x203040), 3));
        CHECK(s.near(16 + 20, 24, rgb(0x607080), 3) && s.near(16 + 20, 25, rgb(0x607080), 3) && s.near(16 + 20, 26, rgb(0x203040), 3));
        CHECK(s.near(16 + 2, 24 + 2, kBg, 3));
        // a lit key and a danger key
        CHECK(s.near(170 + 5, 194 + 12, rgb(0x00c040), 3));
        CHECK(s.countNear(170 + 5, 194 + 4, 170 + 70, 194 + 20, rgb(0x001000), 30) > 3);
        CHECK(s.near(632 + 5, 24 + 15, rgb(0x300030), 3));
        // the hazard stripes: the new key ground and the new stripe colour, the old stripe gone
        CHECK(s.countNear(16 + 5, 132 + 4, 16 + 115, 132 + 11, rgb(0x203040), 3) > 100);
        CHECK(s.countNear(16 + 5, 132 + 4, 16 + 115, 132 + 11, rgb(0x004040), 3) > 100);
        CHECK(s.countNear(16 + 5, 132 + 4, 16 + 115, 132 + 11, kHazardStripe, 3) == 0);
        // the group: a 2 px border, its ground, the lit key in the new accent
        CHECK(s.near(16, 444 + 16, rgb(0xffff00), 3) && s.near(17, 444 + 16, rgb(0xffff00), 3));
        CHECK(s.near(16 + 3, 444 + 16, rgb(0x101030), 3));
        CHECK(s.countNear(16 + 4, 444 + 4, 16 + 440, 444 + 28, rgb(0x00c040), 3) > 1000);   // whichever key is lit
        // the rail key that is lit takes the new menu ground
        CHECK(s.near(16 + 4, 538 + 28, rgb(0x202060), 3));
        // the dome: new ground, a 2 px border, a glyph in the new colour
        CHECK(s.near(292 + 6, 534 + 32, rgb(0x300030), 3));
        CHECK(s.near(292 + 20, 534, rgb(0xff00ff), 3) && s.near(292 + 20, 535, rgb(0xff00ff), 3));
        CHECK(s.countNear(292 + 30, 534 + 10, 292 + 62, 534 + 34, rgb(0x00ffff), 8) >= 100);
        s.savePng(shotDir() + "keys-second-skin.png");
        // and back: the Workbench value restores the first picture exactly
        ui->global<Theme>().set_skin(ui->global<Workbench>().get_value());
        const Frame back = shot();
        int differing = 0;
        for (std::size_t i = 0; i < back.px.size(); ++i) {
            if (back.px[i].r != rest.px[i].r || back.px[i].g != rest.px[i].g || back.px[i].b != rest.px[i].b) { ++differing; }
        }
        CHECK(back.px.size() == rest.px.size() && differing == 0);
    }

    return testSummary("test_slint_keys");
}
