// The lamps family: Lamp, StatusChip and VLine reach the pixels at the artboard's geometry and colours
// (Workbench.dc.html .lamp / .sb, Workbench-PluginKit.dc.html .lamp.y), the glow is a skin switch, and the
// status chip elides, hovers, clicks, shows its tooltip and expands its lines upward.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "lamps_specimen.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace foxsdr::slinttest;

namespace {

// ---- the sheet's colours: the artboard's own hexes, not the theme's names ------------------------------------
const Rgb kBg = rgb(0x15171b);          // body
const Rgb kStrip = rgb(0x191c21);       // the status bar's ground
const Rgb kLine = rgb(0x2b2f37);        // every 1px divider (and lamp-off)
const Rgb kRing = rgb(0x434955);        // lamp-ring, knob-ring (the panel's and the tip's border)
const Rgb kMenu = rgb(0x262a32);        // menu ground (the lines panel, the tip)
const Rgb kMenuHot = rgb(0x363b45);     // a hovered chip
const Rgb kMuted = rgb(0x9aa3ad);       // the off lamp's caption
const Rgb kDim = rgb(0x7d8694);         // .sb .k and the detail
const Rgb kFg = rgb(0xe6e8ec);          // .sb .v by default
const Rgb kFgSoft = rgb(0xc9ced6);      // the lines in the panel
const Rgb kGood = rgb(0x7fe6ad);        // the DECODERS value (chip-good-fg)
const Rgb kGreen = rgb(0x3ddc84);       // lamp-green
const Rgb kDisabledBg = rgb(0x1d2026);  // .btn[disabled]
const Rgb kDisabledFg = rgb(0x8a93a0);

// A CHECK that says what it found when it fails.
#define CHECK_EQ(a, b)                                                                                      \
    do {                                                                                                    \
        const long long checkEqA = static_cast<long long>(a), checkEqB = static_cast<long long>(b);         \
        CHECK(checkEqA == checkEqB);                                                                        \
        if (checkEqA != checkEqB) { std::printf("  -> %s is %lld, expected %lld\n", #a, checkEqA, checkEqB); } \
    } while (0)

int whole(float v) { return static_cast<int>(std::lround(v)); }

// The pixel of the rectangle farthest from `background`: the strongest ink a run of text laid down. Antialiasing
// blends a glyph's edge towards the ground, so its core pixel is the one that carries the true colour.
Rgb strongestInk(const Frame& f, int x0, int y0, int x1, int y1, Rgb background) {
    Rgb best = background;
    int bestDistance = -1;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Rgb p = f.at(x, y);
            const int d = std::abs(int(p.r) - int(background.r)) + std::abs(int(p.g) - int(background.g)) + std::abs(int(p.b) - int(background.b));
            if (d > bestDistance) { bestDistance = d; best = p; }
        }
    }
    return best;
}

// The smallest per-channel distance from `want` over the rectangle: how close the nearest pixel comes.
int closestTo(const Frame& f, int x0, int y0, int x1, int y1, Rgb want) {
    int best = 255;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Rgb p = f.at(x, y);
            best = std::min(best, std::max({std::abs(int(p.r) - int(want.r)), std::abs(int(p.g) - int(want.g)), std::abs(int(p.b) - int(want.b))}));
        }
    }
    return best;
}

// The longest run of consecutive pixels within tol of `want` along row y (or column x).
int runAlongRow(const Frame& f, int y, int x0, int x1, Rgb want, int tol) {
    int best = 0, run = 0;
    for (int x = x0; x < x1; ++x) { run = f.near(x, y, want, tol) ? run + 1 : 0; best = std::max(best, run); }
    return best;
}
int runAlongCol(const Frame& f, int x, int y0, int y1, Rgb want, int tol) {
    int best = 0, run = 0;
    for (int y = y0; y < y1; ++y) { run = f.near(x, y, want, tol) ? run + 1 : 0; best = std::max(best, run); }
    return best;
}

// The first / last column (row) of the rectangle that holds ink (a pixel off the background); -1 when none.
int firstInkColumn(const Frame& f, int x0, int x1, int y0, int y1, Rgb background) {
    for (int x = x0; x < x1; ++x) { if (f.countInk(x, y0, x + 1, y1, background) > 0) { return x; } }
    return -1;
}
int lastInkColumn(const Frame& f, int x0, int x1, int y0, int y1, Rgb background) {
    for (int x = x1 - 1; x >= x0; --x) { if (f.countInk(x, y0, x + 1, y1, background) > 0) { return x; } }
    return -1;
}
int firstInkRow(const Frame& f, int x0, int x1, int y0, int y1, Rgb background) {
    for (int y = y0; y < y1; ++y) { if (f.countInk(x0, y, x1, y + 1, background) > 0) { return y; } }
    return -1;
}
int lastInkRow(const Frame& f, int x0, int x1, int y0, int y1, Rgb background) {
    for (int y = y1 - 1; y >= y0; --y) { if (f.countInk(x0, y, x1, y + 1, background) > 0) { return y; } }
    return -1;
}

// How far a pixel's channel (0 red, 1 green, 2 blue) sits above the body ground: the glow's strength in grey levels.
int lift(const Frame& f, int x, int y, int channel) {
    const Rgb p = f.at(x, y);
    const int v = channel == 0 ? p.r : channel == 1 ? p.g : p.b;
    const int b = channel == 0 ? kBg.r : channel == 1 ? kBg.g : kBg.b;
    return v - b;
}

struct LampCase {
    const char* name;
    int x;            // the lamp's left edge on the sheet; its dot is x .. x + 8, rows 18 .. 26 (the lamp is 13 high at y 16)
    Rgb dot;          // the lit colour (lamp-off for the off lamp)
    Rgb cap;          // the caption colour
    bool lit;
    int glowChannel;  // the channel the glow lifts most: 0 red, 1 green
};
const LampCase kLamps[] = {
    {"off MUTE", 16, rgb(0x2b2f37), rgb(0x9aa3ad), false, 0},
    {"green RUN", 96, rgb(0x3ddc84), rgb(0xcfead9), true, 1},
    {"amber RCVR", 176, rgb(0xf5a524), rgb(0xf3dcae), true, 0},
    {"red FAIL", 256, rgb(0xff5c5c), rgb(0xffc9c9), true, 0},
    {"yellow NEW", 336, rgb(0xffd27a), rgb(0xf3e2b8), true, 0},
};

constexpr int kDotTop = 18, kDotCentreRow = 22, kDotBottom = 26;   // lamps sit at y 16, 13 high, the 9px dot centred in them

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = LampsSpecimen::create();
    slint::Window& win = ui->window();
    const int W = 840, H = 372;

    // The two skins the glow is checked with: the Workbench filling, and a copy of it with the glow switched off.
    const Skin workbench = ui->global<Workbench>().get_value();
    Skin noGlow = workbench;
    noGlow.lamp_glow = false;

    moveTo(win, 2, 338);                                             // the pointer rests where nothing listens
    Frame on = render(ui, W, H);
    CHECK(on.width == 840 && on.height == 372);
    on.savePng(shotDir() + "lamps-specimen.png");

    ui->global<Theme>().set_skin(noGlow);
    Frame off = render(ui, W, H);
    ui->global<Theme>().set_skin(workbench);
    off.savePng(shotDir() + "lamps-specimen-no-glow.png");

    // ======================================================================================================
    // Lamp: the dot, the 6px gap, the caption
    // ======================================================================================================
    CHECK_EQ(whole(ui->get_lamp_h()), 13);                           // 10px type at line-height 1.3
    CHECK_EQ(whole(ui->get_lamp_bare_w()), 9);                       // an empty caption draws the dot alone
    CHECK_EQ(whole(ui->get_lamp_bare_off_w()), 9);
    CHECK(whole(ui->get_lamp_green_w()) > 15 + 10);                  // dot + gap + a caption with some width

    for (const LampCase& c : kLamps) {
        std::printf("%s\n", c.name);
        const int cx = c.x + 4;

        // the dot is 9px across, centred in the 13px lamp: measured on the glow-less sheet, where a lit dot has no halo
        CHECK(off.near(cx, kDotCentreRow, c.dot, 8));
        CHECK_EQ(firstInkColumn(off, c.x - 4, c.x + 14, kDotTop, kDotBottom + 1, kBg), c.x);
        CHECK_EQ(lastInkColumn(off, c.x, c.x + 14, kDotTop, kDotBottom + 1, kBg), c.x + 8);
        CHECK_EQ(firstInkRow(off, cx, cx + 1, 10, 30, kBg), kDotTop);
        CHECK_EQ(lastInkRow(off, cx, cx + 1, 10, 30, kBg), kDotBottom);
        if (c.lit) {
            CHECK_EQ(runAlongRow(off, kDotCentreRow, c.x - 4, c.x + 14, c.dot, 8), 9);   // the lit colour fills it edge to edge
            CHECK_EQ(runAlongCol(off, cx, 10, 30, c.dot, 8), 9);
        } else {
            CHECK(off.near(c.x, kDotCentreRow, kRing, 8));                  // the off dot: a lamp-ring border ...
            CHECK(off.near(c.x + 8, kDotCentreRow, kRing, 8));
            CHECK(off.near(cx, kDotTop, kRing, 8));                         // ... top and bottom too
            CHECK(off.near(cx, kDotBottom, kRing, 8));
            CHECK_EQ(runAlongRow(off, kDotCentreRow, c.x - 4, c.x + 14, kLine, 4), 7);   // ... round a lamp-off middle
        }

        // the caption starts 6px after the dot (first ink column; a glyph's own side bearing may add a pixel or two)
        CHECK_EQ(off.countInk(c.x + 9, 16, c.x + 15, 29, kBg), 0);          // nothing in the 6px gap
        const int captionStart = firstInkColumn(off, c.x + 9, c.x + 60, 16, 29, kBg);
        std::printf("  caption starts %d px after the dot's left edge (9 + 6 = 15)\n", captionStart - c.x);
        CHECK(captionStart >= c.x + 15 && captionStart <= c.x + 17);

        // caption colour: lamp-<colour>-cap while lit, muted while off
        const Rgb ink = strongestInk(on, c.x + 15, 16, c.x + 55, 29, kBg);
        std::printf("  strongest caption ink %s, wanted %s\n", hex(ink).c_str(), hex(c.cap).c_str());
        CHECK(nearColour(ink, c.cap, 40));
        if (c.lit) { CHECK(!nearColour(ink, kMuted, 12)); }
    }

    // a captionless lamp is the dot alone: nothing to its right
    CHECK_EQ(off.countInk(425, 10, 446, 34, kBg), 0);
    CHECK_EQ(off.countInk(465, 10, 490, 34, kBg), 0);
    CHECK(off.near(420, kDotCentreRow, kGreen, 8));                  // the bare lit lamp
    CHECK(off.near(456, kDotCentreRow, kRing, 8));                   // the bare off lamp: ring
    CHECK(off.near(460, kDotCentreRow, kLine, 4));                   // ... and lamp-off

    // the caption IS the artboard's .lamp text laid 15px (9 + 6) from the dot's left edge: a bare Text set from the
    // sheet's CSS and placed at y 84 gives the very same pixels as the lamp's caption does at the lamp's y 16, so the
    // gap, the size, the weight, the letter spacing and the colour are all pinned without a glyph shape in sight
    {
        const int rulerX[5] = {140, 200, 260, 320, 380};
        for (int i = 0; i < 5; ++i) {
            const LampCase& c = kLamps[i];
            int differing = 0;
            for (int dy = 0; dy < 13; ++dy) {
                for (int dx = 0; dx < 34; ++dx) { if (!off.near(c.x + 15 + dx, 16 + dy, off.at(rulerX[i] + dx, 84 + dy), 0)) { ++differing; } }
            }
            std::printf("%s: caption differs from its ruler in %d pixels\n", c.name, differing);
            CHECK(off.countInk(rulerX[i], 84, rulerX[i] + 34, 97, kBg) > 20);   // the ruler is not blank
            CHECK_EQ(differing, 0);
        }
    }

    // ======================================================================================================
    // Glow: a skin switch. CSS `box-shadow: 0 0 6px #3ddc8488` on the 9px dot is, in green-channel grey levels
    // over the ground, about +32 / +22 / +14 / +8 at 1 / 2 / 3 / 4 px beyond the dot's edge.
    // ======================================================================================================
    {
        const int gx = 416, gcx = 420;                               // the bare green lamp: columns 416 .. 424, rows 18 .. 26
        const int expect[4] = {32, 22, 14, 8};
        for (int d = 1; d <= 4; ++d) {
            const int tol = d <= 2 ? 8 : 5;
            CHECK(std::abs(lift(on, gx - d, kDotCentreRow, 1) - expect[d - 1]) <= tol);
            CHECK(std::abs(lift(on, gx + 8 + d, kDotCentreRow, 1) - expect[d - 1]) <= tol);
            CHECK(std::abs(lift(on, gcx, kDotTop - d, 1) - expect[d - 1]) <= tol);
            CHECK(std::abs(lift(on, gcx, kDotBottom + d, 1) - expect[d - 1]) <= tol);
        }
        CHECK(!on.near(gx - 1, kDotCentreRow, kBg, 8));              // "differs from the ground", plainly
        CHECK(!on.near(gx - 2, kDotCentreRow, kBg, 8));
        CHECK(on.near(gx - 9, kDotCentreRow, kBg, 2));               // the glow is 7px deep, no more
        CHECK(on.near(gx + 8 + 9, kDotCentreRow, kBg, 2));
        CHECK(on.near(gcx, kDotTop - 9, kBg, 2));
        // it is the lamp's own colour: green lifts the green channel most
        CHECK(lift(on, gx - 1, kDotCentreRow, 1) > lift(on, gx - 1, kDotCentreRow, 2));
        CHECK(lift(on, gx - 1, kDotCentreRow, 2) > lift(on, gx - 1, kDotCentreRow, 0));
        // amber / red / yellow glows lift red most
        for (const LampCase& c : kLamps) {
            if (!c.lit) { continue; }
            const int lr = lift(on, c.x - 1, kDotCentreRow, 0), lg = lift(on, c.x - 1, kDotCentreRow, 1), lb = lift(on, c.x - 1, kDotCentreRow, 2);
            if (c.glowChannel == 0) { CHECK(lr > lg && lr > lb); } else { CHECK(lg > lr && lg > lb); }
        }
        // a lamp that is off casts none
        CHECK(on.near(455, kDotCentreRow, kBg, 2));
        CHECK(on.near(465, kDotCentreRow, kBg, 2));
        CHECK(on.near(460, kDotTop - 1, kBg, 2));
        CHECK(on.near(15, kDotCentreRow, kBg, 2));                   // the captioned off lamp, left of it

        // switched off by the skin (a copy of Workbench with lamp_glow = false): no halo, and the dot itself is unchanged
        CHECK(off.near(gx - 1, kDotCentreRow, kBg, 2));
        CHECK(off.near(gx - 2, kDotCentreRow, kBg, 2));
        CHECK(off.near(gx + 9, kDotCentreRow, kBg, 2));
        CHECK(off.near(gcx, kDotTop - 1, kBg, 2));
        CHECK(off.near(gcx, kDotBottom + 1, kBg, 2));
        CHECK(off.near(gcx, kDotCentreRow, kGreen, 8));
        CHECK_EQ(runAlongRow(off, kDotCentreRow, gx - 4, gx + 14, kGreen, 8), 9);

        // ... and back: the Workbench value restores the halo, pixel for pixel
        Frame back = render(ui, W, H);
        int differing = 0;
        for (int y = 0; y < H; ++y) { for (int x = 0; x < W; ++x) { if (!back.near(x, y, on.at(x, y), 0)) { ++differing; } } }
        CHECK_EQ(differing, 0);
        CHECK(!back.near(gx - 1, kDotCentreRow, kBg, 8));
    }

    // ======================================================================================================
    // VLine: 1 x 72, line colour
    // ======================================================================================================
    {
        Frame f = render(ui, W, H);
        CHECK(f.colNear(500, 8, 80, kLine, 2));
        CHECK(f.near(500, 7, kBg, 2));
        CHECK(f.near(500, 80, kBg, 2));
        CHECK(f.near(499, 40, kBg, 2));
        CHECK(f.near(501, 40, kBg, 2));
        CHECK_EQ(runAlongCol(f, 500, 0, H, kLine, 2), 72);
        CHECK_EQ(runAlongRow(f, 40, 490, 515, kLine, 2), 1);
        // line-h is a property: 32px here
        CHECK(f.colNear(520, 8, 40, kLine, 2));
        CHECK(f.near(520, 40, kBg, 2));
        CHECK_EQ(runAlongCol(f, 520, 0, 100, kLine, 2), 32);
    }

    // ======================================================================================================
    // A bare lamp over a key's TouchArea (BIAS TEE) lets the click through
    // ======================================================================================================
    {
        Frame f = render(ui, W, H);
        CHECK(f.near(604, 30, rgb(0xf5a524), 8));                    // the amber dot sits in the key's middle
        CHECK_EQ(ui->get_key_clicks(), 0);
        click(win, 604, 30);
        CHECK_EQ(ui->get_key_clicks(), 1);
        click(win, 575, 40);
        CHECK_EQ(ui->get_key_clicks(), 2);
        moveTo(win, 2, 338);
    }

    // ======================================================================================================
    // BIAS TEE as the main screen builds it: a bare lamp and the label in a HorizontalLayout inside a 36px key.
    // The lamp must stretch to the key's height and centre its dot, not sit against the top edge.
    // ======================================================================================================
    {
        Frame f = render(ui, W, H);
        const Rgb amber = rgb(0xf5a524);
        const int keyLeft = 680, keyRight = 819, keyTop = 16, keyBottom = 51;       // 140 x 36
        const int dotX = f.firstColNear(33, keyLeft + 4, keyRight - 4, amber, 12);
        std::printf("BIAS TEE: the dot starts at x=%d\n", dotX);
        CHECK(dotX > keyLeft);
        CHECK_EQ(runAlongRow(f, 33, dotX - 2, dotX + 12, amber, 12), 9);
        CHECK_EQ(runAlongCol(f, dotX + 4, keyTop + 2, keyBottom - 1, amber, 12), 9);
        CHECK_EQ(f.firstRowNear(dotX + 4, keyTop + 2, keyBottom - 1, amber, 12), keyTop + (36 - 9) / 2);   // centred: 13 below the top
        // the label follows 6px after the dot, vertically centred like it
        const Rgb keyGround = rgb(0x262a32);
        const int textStart = firstInkColumn(f, dotX + 13, keyRight - 2, keyTop + 4, keyBottom - 3, keyGround);
        const int textEnd = lastInkColumn(f, dotX + 13, keyRight - 2, keyTop + 4, keyBottom - 3, keyGround);
        const int textTop = firstInkRow(f, dotX + 13, keyRight - 2, keyTop + 4, keyBottom - 3, keyGround);
        const int textBottom = lastInkRow(f, dotX + 13, keyRight - 2, keyTop + 4, keyBottom - 3, keyGround);
        std::printf("  label ink columns %d .. %d, rows %d .. %d\n", textStart, textEnd, textTop, textBottom);
        CHECK(textStart >= dotX + 15 && textStart <= dotX + 17);
        CHECK(std::abs((textTop + textBottom) - (keyTop + keyBottom)) <= 3);
        // lamp + gap + label are centred in the key (padding 14 each side, alignment: center)
        CHECK(std::abs((dotX - keyLeft) - (keyRight - textEnd)) <= 3);
    }

    // ======================================================================================================
    // The MASTER block: four lamps in a GridLayout, 14px between the columns and 6px between the rows
    // ======================================================================================================
    {
        Frame f = render(ui, W, H);
        CHECK_EQ(whole(ui->get_master_h()), 32);                     // 13 + 6 + 13
        const int col0 = std::max(whole(ui->get_lamp_green_w()), whole(ui->get_lamp_mute_w()));
        const int col1x = 16 + col0 + 14;
        std::printf("MASTER: column 1 at x=%d, block %gx%g\n", col1x, ui->get_master_w(), ui->get_master_h());
        CHECK(f.near(20, 54, kGreen, 8));                            // RUN, row 0: lamp top 48, dot 50 .. 58
        CHECK(f.near(col1x + 4, 54, kGreen, 8));                     // DEC
        CHECK(f.near(20, 73, kLine, 4));                             // MUTE, row 1: lamp top 67, dot 69 .. 77
        CHECK(f.near(col1x + 4, 73, kLine, 4));                      // FAIL
        CHECK_EQ(runAlongCol(f, 20, 44, 82, kGreen, 8), 9);
        CHECK(f.near(16, 73, kRing, 8));                             // the off dot's ring starts at the cell's left edge
        CHECK_EQ(runAlongRow(f, 54, col1x - 3, col1x + 12, kGreen, 8), 9);
    }

    // ======================================================================================================
    // StatusChip: 28px high, padding 0 10, 6px gaps, a 1px right border in the line colour
    // ======================================================================================================
    moveTo(win, 2, 338);
    Frame f = render(ui, W, H);
    struct ChipCase { const char* name; int y; int w; };
    const ChipCase decoders{"DECODERS", 104, whole(ui->get_w_decoders())};
    const ChipCase sinkElided{"SINK elided", 136, whole(ui->get_w_sink_elided())};
    const ChipCase sinkFull{"SINK full", 168, whole(ui->get_w_sink_full())};
    const ChipCase sinkBare{"SINK bare", 200, whole(ui->get_w_sink_bare())};
    const ChipCase disabled{"RECORDER disabled", 232, whole(ui->get_w_disabled())};
    const ChipCase receiver{"RECEIVER", 296, whole(ui->get_w_receiver())};
    std::printf("widths: decoders %d, sink elided %d, full %d, bare %d, disabled %d, receiver %d\n",
                decoders.w, sinkElided.w, sinkFull.w, sinkBare.w, disabled.w, receiver.w);
    CHECK_EQ(whole(ui->get_h_receiver()), 28);

    for (const ChipCase& c : {decoders, sinkElided, sinkFull, sinkBare, receiver}) {
        std::printf("%s\n", c.name);
        const int bx = 4 + c.w - 1;                                  // the border column: the chip's right edge - 1
        CHECK(f.colNear(bx, c.y, c.y + 28, kLine, 3));               // 28 rows of line colour ...
        CHECK(f.near(bx, c.y - 1, kBg, 3));                          // ... and no more
        CHECK(f.near(bx, c.y + 28, kBg, 3));
        CHECK(f.near(bx + 1, c.y + 14, kStrip, 2));                  // the chip ends at the border
        CHECK(f.near(bx - 1, c.y + 14, kStrip, 2));                  // transparent ground: the bar shows through
        CHECK_EQ(f.countInk(bx - 9, c.y + 1, bx, c.y + 27, kStrip), 0);   // 10px of right padding stay empty
        // the words are vertically centred in the 28px
        const int first = firstInkRow(f, 4 + 10, bx - 10, c.y + 1, c.y + 27, kStrip);
        const int last = lastInkRow(f, 4 + 10, bx - 10, c.y + 1, c.y + 27, kStrip);
        std::printf("  ink rows %d .. %d of the chip's 0 .. 27\n", first - c.y, last - c.y);
        CHECK(first >= c.y + 6 && last <= c.y + 22);
        CHECK(std::abs((first + last) - (2 * c.y + 27)) <= 3);
    }
    // padding 0 10: the first word (or the lamp) starts 10px in
    CHECK_EQ(f.countInk(4, 105, 4 + 9, 131, kStrip), 0);
    CHECK_EQ(f.countInk(4, 137, 4 + 9, 163, kStrip), 0);
    CHECK_EQ(f.countInk(4, 297, 4 + 5, 323, kStrip), 0);
    CHECK(f.countInk(4 + 10, 105, 4 + 14, 131, kStrip) > 0);         // DECODERS: the key's first glyph
    CHECK(f.countInk(4 + 10, 297, 4 + 19, 323, kStrip) > 0);         // RECEIVER: the lamp

    // value ink in the requested colour; the key and detail in dim
    CHECK(closestTo(f, 4, 105, 4 + decoders.w - 1, 131, kGood) <= 16);
    CHECK(closestTo(f, 4, 105, 4 + decoders.w - 1, 131, kFg) > 16);
    CHECK(closestTo(f, 4, 137, 4 + sinkElided.w - 1, 163, kGood) <= 16);
    CHECK(closestTo(f, 4, 297, 4 + receiver.w - 1, 323, kFg) <= 16);          // default value colour: fg
    CHECK(closestTo(f, 4, 297, 4 + receiver.w - 1, 323, kGood) > 16);
    CHECK(closestTo(f, 4, 201, 4 + sinkBare.w - 1, 227, kDim) <= 24);         // SINK: the key, with no detail at all
    const int dimBare = f.countNear(4, 201, 4 + sinkBare.w - 1, 227, kDim, 24);
    const int dimFull = f.countNear(4, 169, 4 + sinkFull.w - 1, 195, kDim, 24);
    std::printf("dim pixels: bare %d, full %d\n", dimBare, dimFull);
    CHECK(dimFull > dimBare + 60);                                            // the detail adds its ink

    // the receiver chip's lamp: a lit green dot at x 14 .. 22, the dot centred in the 28px (rows 305 .. 313)
    CHECK(f.near(18, 309, kGreen, 8));
    CHECK_EQ(runAlongRow(f, 309, 10, 26, kGreen, 8), 9);
    CHECK_EQ(runAlongCol(f, 18, 296, 324, kGreen, 8), 9);

    // ---- the detail is elided to max-detail-w ----
    CHECK_EQ(sinkElided.w, sinkBare.w + 6 + 160);                    // key + value + gap + a detail box of exactly 160px
    CHECK(sinkFull.w > sinkElided.w);                                // unelided, the same words are wider
    CHECK(sinkFull.w - sinkBare.w - 6 > 200);                        // ... by a good margin: the text really is long
    {
        // the default max-detail-w is 320: the RECEIVER chip's long detail is elided to a 320px box (its bare twin has none)
        const int wBare = whole(ui->get_w_receiver_bare());
        CHECK_EQ(receiver.w, wBare + 6 + 320);
        CHECK(f.colNear(220 + wBare - 1, 232, 260, kLine, 3));       // the twin's own border, 28 rows
        CHECK(f.near(220 + wBare, 246, kStrip, 2));
        CHECK(f.near(220 + 14, 245, kGreen, 8));                     // its lamp: the dot is x 230 .. 238, rows 241 .. 249 (centre 245)
    }
    {
        const int bxE = 4 + sinkElided.w - 1, bxF = 4 + sinkFull.w - 1;
        const int lastE = lastInkColumn(f, bxE - 40, bxE, 137, 163, kStrip);
        const int lastF = lastInkColumn(f, bxF - 40, bxF, 169, 195, kStrip);
        std::printf("  last ink: elided %d (its box ends %d), full %d (its box ends %d)\n", lastE, bxE - 10, lastF, bxF - 10);
        CHECK(lastE <= bxE - 10 + 1 && lastE >= bxE - 10 - 14);     // the ellipsis ends the box ...
        CHECK(lastF <= bxF - 10 + 1 && lastF >= bxF - 10 - 4);      // ... the full text ends it too
        // the detail box starts 6px after the value and is 160 wide: ink right at its start
        CHECK(f.countInk(bxE - 10 - 160, 137, bxE - 10 - 160 + 24, 163, kStrip) > 15);
        // it is ELIDED, not clipped: both chips start the same words at the same x, so the first 120px of the detail
        // are pixel for pixel the same; the tail of the 160px box is not (an ellipsis replaces the words that did not fit)
        const int boxX = bxE - 10 - 160;                              // where the elided detail starts (and the full one)
        int headDiffering = 0, tailDiffering = 0;
        for (int dy = 1; dy < 27; ++dy) {
            for (int dx = 0; dx < 120; ++dx) { if (!f.near(boxX + dx, 136 + dy, f.at(boxX + dx, 168 + dy), 6)) { ++headDiffering; } }
            for (int dx = 148; dx < 160; ++dx) { if (!f.near(boxX + dx, 136 + dy, f.at(boxX + dx, 168 + dy), 24)) { ++tailDiffering; } }
        }
        std::printf("  elided vs full: %d differing pixels in the first 120px of the detail, %d in the last 12px of the box\n", headDiffering, tailDiffering);
        CHECK_EQ(headDiffering, 0);
        CHECK(tailDiffering > 6);
    }

    // ---- single-part chips: the padding (10) and the gap (6) from the widths alone, exactly ----
    const int wKey = whole(ui->get_w_key_only()), wValue = whole(ui->get_w_value_only()), wDetail = whole(ui->get_w_detail_only());
    const int textK = wKey - 21, textV = wValue - 21, textD = wDetail - 21;      // 10 + word + 10 + the 1px border
    std::printf("single-part chips: key %d, value %d, detail %d wide (words %d, %d, %d)\n", wKey, wValue, wDetail, textK, textV, textD);
    CHECK(textK > 5 && textV > 5 && textD > 100);
    CHECK_EQ(sinkBare.w, textK + textV + 27);                        // 10 + key + 6 + value + 10 + 1
    CHECK_EQ(sinkFull.w, sinkBare.w + 6 + textD);                    // ... + 6 + detail

    // ---- rulers: each chip word equals the artboard's text (bare Text set from the CSS, on a strip row of its own) ----
    {
        auto differs = [&](int originX, int chipY, int rulerX, int words) {
            int differing = 0;
            for (int dy = 1; dy < 27; ++dy) {
                for (int dx = 0; dx < words; ++dx) { if (!f.near(originX + dx, chipY + dy, f.at(rulerX + dx, 336 + dy), 0)) { ++differing; } }
            }
            return differing;
        };
        // the rulers are not blank
        CHECK(f.countInk(20, 337, 20 + textK, 363, kStrip) > 20);
        CHECK(f.countInk(120, 337, 120 + textV, 363, kStrip) > 40);
        CHECK(f.countInk(220, 337, 220 + textD, 363, kStrip) > 200);
        // the single-part chips: the word starts 10px in
        CHECK_EQ(differs(110 + 10, 200, 20, textK), 0);              // .sb .k  9px / 600 / .08em, dim
        CHECK_EQ(differs(200 + 10, 200, 120, textV), 0);             // .sb .v  11px / 600, in the colour it is given
        CHECK_EQ(differs(300 + 10, 200, 220, textD), 0);             // the detail  12px / 500, dim
        // the same words inside a chip with the others: key, then value 6px on, then detail 6px on
        const int keyX = 4 + 10, valueX = keyX + textK + 6, detailX = valueX + textV + 6;
        CHECK_EQ(differs(keyX, 200, 20, textK), 0);
        CHECK_EQ(differs(valueX, 200, 120, textV), 0);
        CHECK_EQ(differs(keyX, 168, 20, textK), 0);
        CHECK_EQ(differs(valueX, 168, 120, textV), 0);
        CHECK_EQ(differs(detailX, 168, 220, textD), 0);
        // the elided detail starts at the same place and is the same words for its first 120px
        CHECK_EQ(differs(detailX, 136, 220, 120), 0);
        // and the strongest ink of each single word is its own colour
        CHECK(nearColour(strongestInk(f, 110 + 10, 201, 110 + 10 + textK, 227, kStrip), kDim, 14));
        CHECK(nearColour(strongestInk(f, 200 + 10, 201, 200 + 10 + textV, 227, kStrip), kGood, 14));
        CHECK(nearColour(strongestInk(f, 300 + 10, 201, 300 + 10 + textD, 227, kStrip), kDim, 14));
        CHECK(!nearColour(strongestInk(f, 110 + 10, 201, 110 + 10 + textK, 227, kStrip), kMuted, 12));
    }

    // ---- disabled: ground, text, border at half alpha ----
    {
        const int bx = 4 + disabled.w - 1;
        CHECK(f.near(7, 246, kDisabledBg, 2));
        CHECK(nearColour(strongestInk(f, 4, 233, bx, 259, kDisabledBg), kDisabledFg, 24));
        CHECK(f.colNear(bx, 232, 260, over(kLine, 0.5, kDisabledBg), 3));
        CHECK(f.near(bx + 1, 246, kStrip, 2));
    }

    // ---- three chips laid by a HorizontalLayout from a model, abutting at their own widths ----
    {
        const int b1 = 4 + receiver.w - 1, b2 = b1 + decoders.w, b3 = b2 + sinkBare.w;
        std::printf("assembled bar: borders at %d, %d, %d\n", b1, b2, b3);
        CHECK(f.colNear(b1, 264, 292, kLine, 3));
        CHECK(f.colNear(b2, 264, 292, kLine, 3));
        CHECK(f.colNear(b3, 264, 292, kLine, 3));
        CHECK(f.near(b3 + 1, 278, kStrip, 2));                       // nothing stretched: the chips keep their own widths
        CHECK(f.near(b3 + 40, 278, kStrip, 2));
        CHECK_EQ(runAlongRow(f, 266, 0, W, kLine, 3), 1);
        CHECK(closestTo(f, b1 + 1, 265, b2, 291, kGood) <= 16);      // the second chip's value is good-green
        CHECK(f.near(18, 277, kGreen, 8));                           // the first chip's lamp
    }

    // ======================================================================================================
    // StatusChip: hover, tooltip, click
    // ======================================================================================================
    const int decodersCx = 4 + decoders.w / 2, sinkCx = 4 + sinkElided.w / 2, receiverCx = 4 + receiver.w / 2;
    {
        // a chip lights its ground under the pointer (the kit's hover) and only that chip
        moveTo(win, decodersCx, 118);
        Frame h = render(ui, W, H);
        CHECK(h.near(7, 118, kMenuHot, 3));
        CHECK(h.near(7, 150, kStrip, 2));
        CHECK(h.near(4 + decoders.w - 1, 118, kLine, 3));            // the right border stays drawn over the hover ground
        // the tip opens under the chip after the rest: 6px below its bottom edge, on the menu ground with a knob-ring border
        settle(std::chrono::milliseconds(750));
        Frame t = render(ui, W, H);
        CHECK(t.near(7, 148, kMenu, 3));
        CHECK_EQ(t.firstRowNear(40, 133, 175, kRing, 6), 138);       // 132 + 6
        CHECK(t.near(4, 148, kRing, 6));                             // the face starts at the chip's left edge (x = 4)
        CHECK(t.near(3, 148, kStrip, 3));
        moveTo(win, 2, 338);
        settle(std::chrono::milliseconds(50));
        Frame gone = render(ui, W, H);
        CHECK(gone.near(7, 148, kStrip, 2));                         // the tip closes when the pointer leaves ...
        CHECK(gone.near(7, 118, kStrip, 2));                         // ... and so does the hover ground
    }
    {
        // a lamp with a tooltip shows it the same way: 6px under the lamp's 13px
        moveTo(win, 100, 22);
        settle(std::chrono::milliseconds(750));
        Frame t = render(ui, W, H);
        CHECK(t.near(99, 46, kMenu, 3));
        CHECK_EQ(t.firstRowNear(110, 30, 60, kRing, 6), 35);         // 16 + 13 + 6
        CHECK(t.near(96, 46, kRing, 6));                             // the face starts at the lamp's left edge (x = 96)
        moveTo(win, 2, 338);
        settle(std::chrono::milliseconds(50));
        Frame gone = render(ui, W, H);
        CHECK(gone.near(99, 46, kBg, 3));
        // a lamp without one does not: the amber lamp
        moveTo(win, 180, 22);
        settle(std::chrono::milliseconds(750));
        Frame none = render(ui, W, H);
        CHECK_EQ(none.countNear(170, 30, 300, 70, kMenu, 3), 0);
        moveTo(win, 2, 338);
    }
    {
        // clicks: the whole chip is the key, the border column included, one pixel beyond it is not
        CHECK_EQ(ui->get_clicks(), 0);
        click(win, decodersCx, 118);
        CHECK_EQ(ui->get_clicks(), 1);
        CHECK_EQ(ui->get_last_clicked(), 1);
        click(win, sinkCx, 150);
        CHECK_EQ(ui->get_clicks(), 2);
        CHECK_EQ(ui->get_last_clicked(), 2);
        click(win, 5, 106);                                          // the chip's top-left corner pixels
        CHECK_EQ(ui->get_clicks(), 3);
        CHECK_EQ(ui->get_last_clicked(), 1);
        click(win, 4 + decoders.w - 1, 130);                         // its border column, bottom row
        CHECK_EQ(ui->get_clicks(), 4);
        click(win, 4 + decoders.w, 118);                             // the pixel after the chip
        CHECK_EQ(ui->get_clicks(), 4);
        click(win, 3, 118);                                          // the pixel before it
        CHECK_EQ(ui->get_clicks(), 4);
        click(win, decodersCx, 132);                                 // the gap row under it
        CHECK_EQ(ui->get_clicks(), 4);
        // a chip without lines opens no panel
        Frame f2 = render(ui, W, H);
        CHECK(f2.near(24, 90, kBg, 3));                              // where an empty panel (16px high) would sit
        CHECK(f2.near(100, 90, kBg, 3));
        moveTo(win, 2, 338);
    }
    {
        // disabled: a click is not acted on (no callback, no panel), no hover ground; the tip still explains why
        const int dx = 4 + disabled.w / 2;
        const int clicksBefore = ui->get_clicks(), lastBefore = ui->get_last_clicked();
        click(win, dx, 246);
        CHECK_EQ(ui->get_clicks(), clicksBefore);
        CHECK_EQ(ui->get_last_clicked(), lastBefore);
        Frame d = render(ui, W, H);
        CHECK(d.near(7, 246, kDisabledBg, 2));                       // no hover ground
        CHECK(d.near(98, 212, kStrip, 2));                           // no lines panel above it (it would be 30px high, 198 .. 227)
        CHECK(d.near(165, 212, kStrip, 2));
        settle(std::chrono::milliseconds(750));
        Frame t = render(ui, W, H);
        CHECK(t.near(7, 276, kMenu, 3));                             // the tip: 260 + 6 = 266 down
        CHECK_EQ(t.firstRowNear(40, 261, 300, kRing, 6), 266);
        moveTo(win, 2, 338);
        settle(std::chrono::milliseconds(50));
    }

    // ======================================================================================================
    // StatusChip: the lines panel opens above the chip
    // ======================================================================================================
    {
        const int clicksBefore = ui->get_clicks();
        const int rx0 = 4, rx1 = 4 + receiver.w - 1;                 // the chip's left edge and right edge (border column)
        const int panelH = whole(ui->get_panel_h());
        CHECK_EQ(panelH, 5 * 14 + 4 * 3 + 2 * 8);                    // 5 lines of 14, 3 between, 8 above and below = 98

        Frame closed = render(ui, W, H);
        CHECK_EQ(runAlongCol(closed, 420, 150, 296, kMenu, 3), 0);   // nothing open yet (x = 420 crosses no text)

        click(win, receiverCx, 310);
        CHECK_EQ(ui->get_clicks(), clicksBefore + 1);
        CHECK_EQ(ui->get_last_clicked(), 7);
        Frame p = render(ui, W, H);
        p.savePng(shotDir() + "lamps-specimen-lines-open.png");
        // vertical extent: a knob-ring border row at the top (296 - 4 - 98 = 194) and at the bottom (291), menu ground between
        int top = -1, bottom = -1;
        for (int y = 150; y < 296; ++y) { if (p.near(420, y, kRing, 4)) { if (top < 0) { top = y; } bottom = y; } }
        std::printf("lines panel rows %d .. %d\n", top, bottom);
        CHECK_EQ(top, 296 - 4 - panelH);
        CHECK_EQ(bottom, 296 - 4 - 1);                               // 4px above the chip
        CHECK_EQ(runAlongCol(p, 420, top + 1, bottom, kMenu, 3), panelH - 2);
        CHECK(p.near(420, 293, kBg, 3));                             // the 4px gap shows the sheet, not the panel
        CHECK(p.near(7, 310, kStrip, 2));                            // nothing of it over the chip itself
        CHECK(p.near(420, 310, kStrip, 2));
        // horizontal extent: flush with the chip (the lines are short, so the chip's width wins)
        int left = -1, right = -1;
        for (int x = 0; x < W; ++x) { if (p.near(x, 240, kRing, 4)) { if (left < 0) { left = x; } right = x; } }
        CHECK_EQ(left, rx0);
        CHECK_EQ(right, rx1);
        // the lines are drawn, one band each, 14px high every 17px, 12px in from the panel's left edge
        const int bandTop0 = top + 8;
        for (int i = 0; i < 5; ++i) {
            const int by = bandTop0 + i * 17;
            CHECK(p.countInk(rx0 + 12, by, rx0 + 12 + 150, by + 14, kMenu) > 25);
            CHECK_EQ(p.countInk(rx0 + 12, by + 14, rx0 + 12 + 150, by + 17, kMenu), 0);   // the 3px between lines is empty
        }
        CHECK(nearColour(strongestInk(p, rx0 + 12, bandTop0, rx0 + 12 + 150, bandTop0 + 14, kMenu), kFgSoft, 24));
        const int firstCol = firstInkColumn(p, rx0 + 2, rx0 + 60, bandTop0, bandTop0 + 14, kMenu);
        CHECK(firstCol >= rx0 + 12 && firstCol <= rx0 + 14);
        CHECK_EQ(p.countInk(rx0 + 1, top + 9, rx0 + 12, bottom - 8, kMenu), 0);   // the 12px of left padding is empty (below the corner arcs)

        // a second click on the chip closes the panel and is not a new click (the press closes the popup first)
        click(win, receiverCx, 310);
        Frame c1 = render(ui, W, H);
        CHECK_EQ(runAlongCol(c1, 420, 150, 296, kMenu, 3), 0);
        CHECK_EQ(ui->get_clicks(), clicksBefore + 1);

        // Escape closes it
        click(win, receiverCx, 310);
        CHECK_EQ(ui->get_clicks(), clicksBefore + 2);
        Frame o2 = render(ui, W, H);
        CHECK_EQ(runAlongCol(o2, 420, 150, 296, kMenu, 3), panelH - 2);
        keyTap(win, slint::platform::key_codes::Escape);
        Frame c2 = render(ui, W, H);
        CHECK_EQ(runAlongCol(c2, 420, 150, 296, kMenu, 3), 0);

        // a click anywhere else closes it, and does nothing else (it lands on empty sheet)
        click(win, receiverCx, 310);
        CHECK_EQ(ui->get_clicks(), clicksBefore + 3);
        Frame o3 = render(ui, W, H);
        CHECK_EQ(runAlongCol(o3, 420, 150, 296, kMenu, 3), panelH - 2);
        click(win, 780, 20);
        Frame c3 = render(ui, W, H);
        CHECK_EQ(runAlongCol(c3, 420, 150, 296, kMenu, 3), 0);
        CHECK_EQ(ui->get_clicks(), clicksBefore + 3);
        moveTo(win, 2, 338);
    }

    // ======================================================================================================
    // A second filling: the colours AND the divider thickness are tokens (hairline 3, a yellow line)
    // ======================================================================================================
    {
        Skin probe = workbench;
        probe.lamp_green = slint::Color::from_rgb_uint8(0x20, 0x60, 0xff);
        probe.lamp_green_cap = slint::Color::from_rgb_uint8(0xff, 0x00, 0xff);
        probe.lamp_glow = false;
        probe.line = slint::Color::from_rgb_uint8(0xff, 0xff, 0x00);
        probe.hairline = 3.0f;
        ui->global<Theme>().set_skin(probe);
        Frame g = render(ui, W, H);
        const Rgb yellow = rgb(0xffff00);
        CHECK(g.near(100, kDotCentreRow, rgb(0x2060ff), 8));                              // the dot follows lamp-green
        CHECK(nearColour(strongestInk(g, 96 + 15, 16, 96 + 55, 29, kBg), rgb(0xff00ff), 40));   // and the caption lamp-green-cap
        CHECK_EQ(runAlongRow(g, 40, 490, 515, yellow, 2), 3);                             // the VLine is hairline wide
        CHECK_EQ(runAlongCol(g, 501, 0, H, yellow, 2), 72);
        CHECK_EQ(whole(ui->get_w_sink_bare()), sinkBare.w + 2);                           // the chip's border is hairline wide too
        const int bx = 4 + whole(ui->get_w_sink_bare()) - 1;
        CHECK(g.colNear(bx, 201, 227, yellow, 2));
        CHECK(g.colNear(bx - 2, 201, 227, yellow, 2));
        CHECK(g.near(bx - 3, 214, kStrip, 2));
        CHECK(g.near(bx + 1, 214, kStrip, 2));
        // the dot stays 9px across (the border is inside it): a 3px lamp-ring, a 3px lamp-off middle, a 3px ring
        CHECK_EQ(runAlongRow(g, kDotCentreRow, 12, 30, kLine, 4), 3);
        CHECK_EQ(runAlongRow(g, kDotCentreRow, 12, 30, kRing, 4), 3);
        CHECK_EQ(g.countInk(16, kDotCentreRow, 25, kDotCentreRow + 1, kBg), 9);
        ui->global<Theme>().set_skin(workbench);
        Frame back = render(ui, W, H);
        CHECK_EQ(whole(ui->get_w_sink_bare()), sinkBare.w);
        CHECK(back.colNear(500, 8, 80, kLine, 2));
    }

    return testSummary("test_slint_lamps");
}
