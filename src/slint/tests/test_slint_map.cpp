// The map pane (src/slint/ui/map.slint): the picture the host composed with the targets laid over it as Slint items. Drawn from the contract and probed
// pixel by pixel on a 520 x 360 specimen - the base picture showing through, an icon of each kind with ink at its place, the accent rings of a selected and
// a followed target, the dashed ring of an estimated one, the red halo of an emergency (blinking: seen lit and dark), the label with its halo, the faded
// target, the legend at the top right, the scale bar and the attribution at the bottom, the two zoom keys - then driven the way the operator drives it: the
// wheel, a drag, a click on the map and on a target (a 20 px square), a double-click, the hover and its tip, the crosshair of the pick, a disabled pane,
// the pane's size reported when it changes. Text is in the embedded Inter (third_party/fonts): asserted as ink in a region, never as glyph shapes.
//
// ROTATION IS NOT HELD BY PIXELS HERE: the software renderer draws no rotation of any item (measured in the work on this file: an icon at 90 and at 45
// degrees came out unturned). The heading is held by what the icon is bound to (MapPane.icon-turn, read below); a desktop renderer is needed to see an
// aircraft turn. Also here: the legend, the scale and the targets follow their models (rows added, moved and removed), each kind has a glyph of its own,
// and a pane that is switched off acts on nothing.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "map_specimen.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace foxsdr::slinttest;
using std::chrono::milliseconds;

// A colour check that says what it saw when it fails.
#define CHECK_RGB(got, want, tol)                                                                             \
    do {                                                                                                      \
        const Rgb got_ = (got);                                                                               \
        const Rgb want_ = (want);                                                                             \
        const bool ok_ = nearColour(got_, want_, (tol));                                                      \
        if (!ok_) { std::printf("  %s: got %s, wanted %s (tol %d)\n", #got, hex(got_).c_str(), hex(want_).c_str(), (tol)); } \
        CHECK(ok_);                                                                                           \
    } while (0)

namespace {

constexpr int kW = 520, kH = 360;

// Theme.* (the Workbench filling)
const Rgb kBg = rgb(0x15171b);
const Rgb kWell = rgb(0x0d0f12);
const Rgb kAccent = rgb(0xf5a524);
const Rgb kLampRed = rgb(0xff5c5c);
const Rgb kTraceLive = rgb(0x7ad7ff);
const Rgb kLampGreen = rgb(0x3ddc84);
const Rgb kLampAmber = rgb(0xf5a524);
const Rgb kMint = rgb(0x7fe6ad);
const Rgb kFg = rgb(0xe6e8ec);
const Rgb kFgSoft = rgb(0xc9ced6);
const Rgb kMuted = rgb(0x9aa3ad);
const Rgb kDim = rgb(0x7d8694);
const Rgb kErrFg = rgb(0xff9b9b);
const Rgb kKeyBg = rgb(0x262a32);
const Rgb kKeyLine = rgb(0x363b45);
const Rgb kKeyDisabledBg = rgb(0x1d2026);
const Rgb kMenu = rgb(0x262a32);
const Rgb kLine = rgb(0x2b2f37);

// the synthetic base picture: a teal left half, an olive right half, a magenta column at x = 300 and a white row at y = 250 (the test's own colours)
const Rgb kBaseL = rgb(0x1a354a);
const Rgb kBaseR = rgb(0x3a422a);
const Rgb kStripe = rgb(0xff00ff);
const Rgb kRow = rgb(0xffffff);
Rgb baseAt(int x) { return x < 260 ? kBaseL : kBaseR; }

slint::Image makeBase(int w, int h) {
    slint::SharedPixelBuffer<slint::Rgba8Pixel> pic(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const Rgb c = (x == (w * 300) / 520) ? kStripe : (y == (h * 250) / 360) ? kRow : (x < w / 2 ? kBaseL : kBaseR);
            *(pic.begin() + (static_cast<std::size_t>(y) * w + x)) = slint::Rgba8Pixel{c.r, c.g, c.b, 255};
        }
    }
    return slint::Image(pic);
}

struct Pt {
    double x, y;
};
// the specimen's six targets, in pane pixels
const Pt kT[6] = {{78, 72}, {208, 72}, {325, 72}, {78, 180}, {208, 180}, {325, 180}};

// Counts the pixels at a distance in [r0, r1] from (cx, cy) that are within tol of `want`.
int ringNear(const Frame& f, double cx, double cy, double r0, double r1, Rgb want, int tol) {
    int n = 0;
    for (int y = static_cast<int>(cy - r1) - 1; y <= static_cast<int>(cy + r1) + 1; ++y) {
        for (int x = static_cast<int>(cx - r1) - 1; x <= static_cast<int>(cx + r1) + 1; ++x) {
            const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            if (d >= r0 && d <= r1 && f.near(x, y, want, tol)) { ++n; }
        }
    }
    return n;
}
// Counts the pixels at a distance in [r0, r1] that differ from the base picture's colour there by more than tol.
int ringInk(const Frame& f, double cx, double cy, double r0, double r1, int tol = 30) {
    int n = 0;
    for (int y = static_cast<int>(cy - r1) - 1; y <= static_cast<int>(cy + r1) + 1; ++y) {
        for (int x = static_cast<int>(cx - r1) - 1; x <= static_cast<int>(cx + r1) + 1; ++x) {
            const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            if (d >= r0 && d <= r1 && x != 300 && !nearColour(f.at(x, y), baseAt(x), tol)) { ++n; }
        }
    }
    return n;
}
// Ink in a region: pixels that are not the base picture's colour (its own magenta column and white row are skipped).
int regionInk(const Frame& f, int x0, int y0, int x1, int y1, int tol = 20) {
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (x != 300 && y != 250 && !nearColour(f.at(x, y), baseAt(x), tol)) { ++n; }
        }
    }
    return n;
}
// A region of a frame blown up by whole pixels, for a person to look at.
Frame zoomed(const Frame& f, int x0, int y0, int w, int h, int scale) {
    Frame z;
    z.width = static_cast<std::uint32_t>(w * scale);
    z.height = static_cast<std::uint32_t>(h * scale);
    z.px.assign(static_cast<std::size_t>(z.width) * z.height, slint::Rgb8Pixel{0, 0, 0});
    for (int y = 0; y < h * scale; ++y) {
        for (int x = 0; x < w * scale; ++x) {
            const Rgb c = f.at(x0 + x / scale, y0 + y / scale);
            z.px[static_cast<std::size_t>(y) * z.width + static_cast<std::size_t>(x)] = slint::Rgb8Pixel{c.r, c.g, c.b};
        }
    }
    return z;
}
// The brightest pixel (by channel sum) in a rectangle, the base picture's own column and row aside.
Rgb brightest(const Frame& f, int x0, int y0, int x1, int y1) {
    Rgb best{};
    int bestSum = -1;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (x == 300 || y == 250) { continue; }
            const Rgb p = f.at(x, y);
            if (int(p.r) + p.g + p.b > bestSum) { bestSum = int(p.r) + p.g + p.b; best = p; }
        }
    }
    return best;
}
// Ink (not the base picture's colour) in a box around a target
int inkAround(const Frame& f, Pt t, int half) {
    int n = 0;
    for (int y = static_cast<int>(t.y) - half; y < static_cast<int>(t.y) + half; ++y) {
        for (int x = static_cast<int>(t.x) - half; x < static_cast<int>(t.x) + half; ++x) {
            if (x != 300 && !nearColour(f.at(x, y), baseAt(x), 20)) { ++n; }
        }
    }
    return n;
}
// The bounding box of the pixels near `want` in a region; false when there are none.
struct Box {
    int x0 = 1 << 20, y0 = 1 << 20, x1 = -1, y1 = -1, n = 0;
};
Box boxOf(const Frame& f, int rx0, int ry0, int rx1, int ry1, Rgb want, int tol) {
    Box b;
    for (int y = ry0; y < ry1; ++y) {
        for (int x = rx0; x < rx1; ++x) {
            if (f.near(x, y, want, tol)) {
                b.x0 = std::min(b.x0, x);
                b.y0 = std::min(b.y0, y);
                b.x1 = std::max(b.x1, x);
                b.y1 = std::max(b.y1, y);
                ++b.n;
            }
        }
    }
    return b;
}

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = MapSpecimen::create();
    slint::Window& win = ui->window();
    ui->set_base(makeBase(kW, kH));

    // =============================== the sheet, at rest ===============================
    Frame f = render(ui, kW, kH);
    CHECK(f.width == kW && f.height == kH);
    f.savePng(shotDir() + "map-specimen.png");
    zoomed(f, 40, 36, 330, 190, 3).savePng(shotDir() + "map-specimen-targets.png");   // the six targets, three times life size

    // ---- the base picture shows through, filled to the pane ----
    CHECK(f.near(20, 270, kBaseL, 2) && f.near(480, 270, kBaseR, 2));
    CHECK(f.near(300, 150, kStripe, 2) && f.near(299, 150, kBaseR, 2) && f.near(301, 150, kBaseR, 2));   // the picture's own column, one pixel wide
    CHECK(f.near(150, 250, kRow, 2) && f.near(150, 249, kBaseL, 2) && f.near(150, 251, kBaseL, 2));
    CHECK(f.near(0, 0, kBaseL, 2) && f.near(kW - 1, 0, kBaseR, 2));                // the corners (the legend and the keys do not reach them)
    CHECK(f.near(0, kH - 1, kBaseL, 2) && f.near(kW - 1, kH - 1, kBaseR, 2));

    // ---- an icon of each kind has ink at its target's place, in the target's colour ----
    const Rgb colours[6] = {kTraceLive, kLampGreen, kErrFg, kFg, kMint, kMuted};
    for (int i = 0; i < 6; ++i) {
        const int ink = f.countInk(static_cast<int>(kT[i].x) - 9, static_cast<int>(kT[i].y) - 9, static_cast<int>(kT[i].x) + 9, static_cast<int>(kT[i].y) + 9, baseAt(static_cast<int>(kT[i].x)), 20);
        if (ink < 14) { std::printf("  target %d: ink %d\n", i, ink); }
        CHECK(ink >= 14);                                                           // the glyph, not a blank
        if (i != 5) {
            const int own = f.countNear(static_cast<int>(kT[i].x) - 9, static_cast<int>(kT[i].y) - 9, static_cast<int>(kT[i].x) + 9, static_cast<int>(kT[i].y) + 9, colours[i], 36);
            if (own < 4) { std::printf("  target %d: %d pixels of its own colour\n", i, own); }
            CHECK(own >= 4);
        }
    }
    // the plain station and the faded unknown have no ring: the annulus outside their 18 px box is bare picture
    CHECK(ringInk(f, kT[3].x, kT[3].y, 11.0, 13.5) == 0 && ringInk(f, kT[5].x, kT[5].y, 11.0, 13.5) == 0);
    CHECK(inkAround(f, kT[3], 9) > 14 && inkAround(f, kT[5], 9) > 10);
    // the heading is what the icon is bound to: 90 degrees is 90, none is 0 (a pixel cannot say it in the software renderer)
    CHECK(ui->get_turn_of_90() == 90.0f && ui->get_turn_of_none() == 0.0f);

    // ---- the selected ring: accent, 2 px, round, 32 px across ----
    CHECK(ringNear(f, kT[0].x, kT[0].y, 14.0, 17.5, kAccent, 40) >= 70);
    CHECK(ringNear(f, kT[0].x, kT[0].y, 12.0, 14.0, kAccent, 40) == 0 && ringNear(f, kT[0].x, kT[0].y, 18.5, 22.0, kAccent, 40) == 0);   // only at its radius
    CHECK(ringNear(f, kT[3].x, kT[3].y, 8.0, 24.0, kAccent, 40) == 0 && ringNear(f, kT[4].x, kT[4].y, 8.0, 24.0, kAccent, 40) == 0);       // the others have none
    // ... a round one: the four compass points of it, and the diagonals, all on the 2 px ring's centre line, 15 px from the centre
    for (int k = 0; k < 8; ++k) {
        const double a = k * 3.14159265358979 / 4.0;
        const double px = kT[0].x + 15.0 * std::sin(a), py = kT[0].y - 15.0 * std::cos(a);
        bool hit = false;
        for (int dy = -1; dy <= 1 && !hit; ++dy) { for (int dx = -1; dx <= 1 && !hit; ++dx) { if (f.near(static_cast<int>(px) + dx, static_cast<int>(py) + dy, kAccent, 40)) { hit = true; } } }
        CHECK(hit);
    }

    // ---- followed: two rings, 32 and 40 px across (the outer one fainter) ----
    CHECK(ringNear(f, kT[1].x, kT[1].y, 14.0, 17.5, kAccent, 40) >= 70);
    CHECK(ringInk(f, kT[1].x, kT[1].y, 18.3, 20.8, 25) >= 90);                     // the outer ring: 40 px across
    CHECK(ringInk(f, kT[1].x, kT[1].y, 16.8, 17.9, 12) == 0);                      // and clear of the inner one
    CHECK(ringNear(f, kT[1].x, kT[1].y, 18.3, 20.8, kAccent, 24) == 0);            // ... fainter than it (the outer ring is the accent at 65 %)

    // ---- estimated: a dashed ring of twelve arcs in the target's colour, 26 px across ----
    {
        int on = 0, transitions = 0;
        bool prev = false;
        for (int k = 0; k < 360; ++k) {
            const double a = k * 3.14159265358979 / 180.0;
            const int px = static_cast<int>(std::floor(kT[4].x + 13.0 * std::sin(a))), py = static_cast<int>(std::floor(kT[4].y - 13.0 * std::cos(a)));
            const bool ink = f.near(px, py, kMint, 60) || f.near(px, py - 1, kMint, 60) || f.near(px - 1, py, kMint, 60) || f.near(px - 1, py - 1, kMint, 60);
            if (ink) { ++on; }
            if (k > 0 && ink != prev) { ++transitions; }
            prev = ink;
        }
        if (on < 100 || on > 260 || transitions < 18) { std::printf("  dashed ring: %d of 360 samples inked, %d transitions\n", on, transitions); }
        CHECK(on >= 100 && on <= 260);                                              // about half the circle: dashes with gaps
        CHECK(transitions >= 18 && transitions <= 30);                              // twelve dashes: 24 edges
    }

    // ---- the label: beside the icon, clear of the ring it wears (14 px from the centre, 20 for a selected target, 24 followed, 29 emergency), in its colour, over a dark halo ----
    {
        const int lx0 = static_cast<int>(kT[0].x) + 20, lx1 = lx0 + 62, ly0 = static_cast<int>(kT[0].y) - 8, ly1 = static_cast<int>(kT[0].y) + 8;   // 20 px out: clear of its 16 px ring
        CHECK(f.countInk(lx0, ly0, lx1, ly1, kBaseL, 20) > 40);
        CHECK(f.countNear(lx0, ly0, lx1, ly1, kBg, 4) > 25);                           // the halo: Theme.bg
        const Rgb ink = brightest(f, lx0, ly0, lx1, ly1);
        CHECK_RGB(ink, kTraceLive, 40);                                             // in the target's colour
        CHECK(f.countInk(lx0, ly0 - 12, lx1, ly0 - 4, kBaseL, 20) == 0);            // and only there
        // the others stand as far from their rings: no label ink between a ring and its label, ink beyond the gap
        CHECK(regionInk(f, 90, 173, 91, 187) == 0 && regionInk(f, 93, 173, 120, 187) > 8);                   // plain: 14 px (RX)
        CHECK(regionInk(f, 229, 66, 231, 78) == 0 && regionInk(f, 233, 64, 290, 80) > 40);                   // followed: 24 px (SEA STAR)
        CHECK(regionInk(f, 351, 66, 353, 78) == 0 && regionInk(f, 355, 64, 400, 80) > 40);                   // emergency: 29 px (SQK7700)
        CHECK(regionInk(f, 223, 174, 225, 186) == 0 && regionInk(f, 226, 174, 245, 186) > 6);                // estimated: 18 px (ISS)
        // a label that would run off the right edge goes to the left of its target
        ui->set_targets(std::make_shared<slint::VectorModel<MapTarget>>(std::vector<MapTarget>{
            MapTarget{"EDGE", 0.97f, 0.40f, -1.0f, 3, "A LONG LABEL", 1.0f, false, false, false, false, slint::Color::from_rgb_uint8(0xe6, 0xe8, 0xec)}}));
        Frame edge = render(ui, kW, kH);
        const int ex = static_cast<int>(0.97 * kW), ey = static_cast<int>(0.40 * kH);
        CHECK(edge.countInk(ex - 90, ey - 8, ex - 14, ey + 8, baseAt(ex), 20) > 40);   // left of it
        CHECK(edge.countInk(ex + 14, ey - 8, kW - 1, ey + 8, baseAt(ex), 20) == 0);    // none off the right edge
        ui->set_targets(std::make_shared<slint::VectorModel<MapTarget>>(std::vector<MapTarget>{}));
        Frame none = render(ui, kW, kH);
        CHECK(regionInk(none, 0, 0, 376, 300) == 0);                                // no targets: the picture, the legend corner (x >= 380) and the bar below it are all there is
    }
    // (the six are put back for the rest)
    const auto sixTargets = [] {
        std::vector<MapTarget> v;
        const auto t = [&](const char* id, float x, float y, float heading, int kind, const char* label, float alpha, bool sel, bool fol, bool est, bool emg, slint::Color c) {
            v.push_back(MapTarget{id, x, y, heading, kind, label, alpha, sel, fol, est, emg, c});
        };
        t("A1", 0.15f, 0.20f, 90, 1, "BAW123", 1.0f, true, false, false, false, slint::Color::from_rgb_uint8(0x7a, 0xd7, 0xff));
        t("V2", 0.40f, 0.20f, 180, 2, "SEA STAR", 1.0f, false, true, false, false, slint::Color::from_rgb_uint8(0x3d, 0xdc, 0x84));
        t("A3", 0.625f, 0.20f, 0, 1, "SQK7700", 1.0f, false, false, false, true, slint::Color::from_rgb_uint8(0xff, 0x9b, 0x9b));
        t("S4", 0.15f, 0.50f, -1, 3, "RX", 1.0f, false, false, false, false, slint::Color::from_rgb_uint8(0xe6, 0xe8, 0xec));
        t("T5", 0.40f, 0.50f, -1, 4, "ISS", 1.0f, false, false, true, false, slint::Color::from_rgb_uint8(0x7f, 0xe6, 0xad));
        t("U6", 0.625f, 0.50f, -1, 0, "", 0.5f, false, false, false, false, slint::Color::from_rgb_uint8(0x9a, 0xa3, 0xad));
        return std::make_shared<slint::VectorModel<MapTarget>>(std::move(v));
    };
    ui->set_targets(sixTargets());
    f = render(ui, kW, kH);

    // ---- the faded target: half of its colour over the picture ----
    {
        const Rgb want = over(kMuted, 0.5, baseAt(325));
        const Rgb got = brightest(f, 316, 171, 334, 189);
        if (!nearColour(got, want, 14)) { std::printf("  faded target: brightest #%02x%02x%02x, wanted #%02x%02x%02x\n", got.r, got.g, got.b, want.r, want.g, want.b); }
        CHECK(nearColour(got, want, 14));
        CHECK(f.countInk(kW - 40, 120, kW, 128, kBaseR, 20) == 0);
    }

    // ---- the legend: top right, a 10 px swatch and a cap a row, on a plate ----
    {
        const Box amber = boxOf(f, 380, 0, kW, 120, kLampAmber, 6);
        const Box green = boxOf(f, 380, 0, kW, 120, kLampGreen, 6);
        const Box live = boxOf(f, 380, 0, kW, 120, kTraceLive, 6);
        CHECK(amber.n >= 80 && green.n >= 80 && live.n >= 80);
        CHECK(amber.x1 - amber.x0 + 1 == 10 && amber.y1 - amber.y0 + 1 == 10 && green.x1 - green.x0 + 1 == 10 && live.y1 - live.y0 + 1 == 10);
        CHECK(amber.x0 == green.x0 && green.x0 == live.x0);                         // one column of swatches
        CHECK(amber.y0 < green.y0 && green.y0 < live.y0 && green.y0 - amber.y0 >= 12 && green.y0 - amber.y0 <= 20);   // top to bottom, a row apart
        // the plate: wf-key-bg (the well at 0xdd) over the picture, a line round it, 10 px from the right edge and the top
        const Rgb plate = over(kWell, 221.0 / 255.0, kBaseR);
        CHECK(f.near(amber.x0 - 4, amber.y0 + 5, plate, 5));
        int right = -1, top = 999;
        for (int x = 380; x < kW; ++x) { if (f.near(x, 40, kLine, 6)) { right = std::max(right, x); } }
        for (int y = 0; y < 60; ++y) { if (f.near(430, y, kLine, 6)) { top = std::min(top, y); } }
        CHECK(right == kW - 11);                                                   // the plate's right edge: 10 px of picture beyond it
        CHECK(top == 10);
        CHECK(f.countInk(amber.x1 + 4, amber.y0 - 2, kW - 14, amber.y1 + 2, plate, 24) > 20);   // "0-5 kft" in the cap
        CHECK(f.near(kW - 1, 40, kBaseR, 2) && f.near(kW - 5, 40, kBaseR, 2));
    }

    // ---- the scale bar: bottom left, scale-frac of the width, with the text above it ----
    {
        const Box bar = boxOf(f, 0, 325, 200, 350, kFgSoft, 6);
        CHECK(bar.n > 90);
        CHECK(bar.x0 == 12);                                                       // left 12
        const int want = static_cast<int>(std::lround(0.2 * kW));
        CHECK(bar.x1 - bar.x0 + 1 >= want - 1 && bar.x1 - bar.x0 + 1 <= want + 1);    // 0.2 of 520: 104 px, the end ticks included
        CHECK(f.rowNear(336, 14, 12 + want - 3, kFgSoft, 6) && f.rowNear(337, 14, 12 + want - 3, kFgSoft, 6));   // the 2 px rule
        const Rgb underlay = over(kBg, 0.55, kBaseL);
        CHECK(f.near(60, 334, underlay, 6) && f.near(60, 335, underlay, 6) && f.near(60, 338, underlay, 6) && f.near(60, 340, underlay, 6));   // the rule has a dark bed under it
        CHECK(f.near(60, 332, kBaseL, 3) && f.near(60, 341, kBaseL, 3));            // ... 8 px high, and no more
        CHECK(f.near(12, 334, kFgSoft, 6) && f.near(12, 340, kFgSoft, 6) && f.near(12 + want - 1, 334, kFgSoft, 6) && f.near(12 + want - 1, 340, kFgSoft, 6));   // the end ticks
        CHECK(f.countInk(12, 316, 70, 332, kBaseL, 20) > 25);                      // "10 km" over the bar
        CHECK_RGB(brightest(f, 12, 316, 70, 332), kFgSoft, 30);
        // a longer bar is longer; none when scale-frac is 0
        ui->set_scale_frac(0.4f);
        Frame longer = render(ui, kW, kH);
        const Box bar2 = boxOf(longer, 0, 325, 300, 350, kFgSoft, 6);
        CHECK(bar2.x0 == 12 && bar2.x1 - bar2.x0 + 1 >= 207 && bar2.x1 - bar2.x0 + 1 <= 209);
        ui->set_scale_frac(0.0f);
        ui->set_scale_text("");
        Frame nobar = render(ui, kW, kH);
        CHECK(boxOf(nobar, 0, 300, 200, 350, kFgSoft, 6).n == 0);
        ui->set_scale_frac(0.2f);
        ui->set_scale_text("10 km");
        render(ui, kW, kH);
    }

    // ---- the attribution: bottom centre, dim ----
    {
        long sum = 0, n = 0;
        for (int y = 338; y < 358; ++y) {
            for (int x = 120; x < 400; ++x) {
                if (!nearColour(f.at(x, y), baseAt(x), 20) && !nearColour(f.at(x, y), kBg, 6)) { sum += x; ++n; }
            }
        }
        CHECK(n > 30);
        CHECK(n > 0 && std::abs(static_cast<double>(sum) / static_cast<double>(n) - 260.0) < 12.0);   // centred on the pane (the keys and the bar are elsewhere)
        CHECK_RGB(brightest(f, 120, 338, 400, 358), kDim, 40);
        ui->set_attribution("");
        Frame none = render(ui, kW, kH);
        CHECK(regionInk(none, 120, 338, 400, 358) == 0);
        ui->set_attribution("Map data - test");
        render(ui, kW, kH);
    }

    // ---- the zoom keys: bottom right, 26 px square, + above - ----
    {
        // + : x 485..510, y 295..320; - : x 485..510, y 325..350 (9 px from the right and the bottom, 4 px apart)
        CHECK(f.countNear(486, 296, 510, 320, kKeyBg, 4) > 300 && f.countNear(486, 326, 510, 350, kKeyBg, 4) > 300);
        CHECK(f.near(497, 295, kKeyLine, 6) && f.near(497, 320, kKeyLine, 6) && f.near(485, 308, kKeyLine, 6) && f.near(510, 308, kKeyLine, 6));   // the + key's border: 26 x 26
        CHECK(f.near(497, 325, kKeyLine, 6) && f.near(497, 350, kKeyLine, 6) && f.near(485, 338, kKeyLine, 6) && f.near(510, 338, kKeyLine, 6));
        CHECK(f.near(497, 293, baseAt(497), 4) && f.near(497, 322, baseAt(497), 4) && f.near(497, 352, baseAt(497), 4) && f.near(512, 308, baseAt(512), 4));   // nothing round them
        // the glyphs: + has ink in both directions, - in one
        CHECK(f.countInk(487, 297, 509, 319, kKeyBg, 40) > 20 && f.countInk(487, 327, 509, 349, kKeyBg, 40) > 10);
        CHECK(f.countInk(490, 296, 504, 302, kKeyBg, 40) < 3 && f.countInk(490, 316, 504, 320, kKeyBg, 40) < 3);      // clear of the top and bottom of the key (the + is centred)
        CHECK(f.countInk(487, 297, 509, 319, kKeyBg, 40) > f.countInk(487, 327, 509, 349, kKeyBg, 40));          // + has more ink than -
        // disabled: the key's ground and ink change
        ui->set_zoom_out_enabled(false);
        Frame off = render(ui, kW, kH);
        CHECK(off.countNear(486, 326, 510, 350, kKeyDisabledBg, 4) > 300 && off.countNear(486, 296, 510, 320, kKeyBg, 4) > 300);
        ui->set_zoom_out_enabled(true);
        render(ui, kW, kH);
    }
    f.savePng(shotDir() + "map-specimen.png");

    // =============================== the emergency blinks ===============================
    {
        // the halo is a red ring 50 px across round the target: sampled over a second and a half, it is seen lit and seen dark
        int lit = 0, dark = 0, litInk = 0;
        for (int s = 0; s < 8; ++s) {
            const Frame b = render(ui, kW, kH);
            const int red = ringNear(b, kT[2].x, kT[2].y, 23.0, 26.5, kLampRed, 24);
            if (red >= 100) { ++lit; litInk = std::max(litInk, red); } else if (red == 0) { ++dark; }
            settle(milliseconds(260));
        }
        if (lit < 2 || dark < 2) { std::printf("  blink: lit %d, dark %d of 8 samples\n", lit, dark); }
        CHECK(lit >= 2 && dark >= 2);
        // the others do not blink: no red ring round the selected aircraft or anything else
        const Frame b = render(ui, kW, kH);
        CHECK(ringNear(b, kT[0].x, kT[0].y, 22.0, 27.0, kLampRed, 24) == 0 && ringNear(b, kT[1].x, kT[1].y, 22.0, 27.0, kLampRed, 24) == 0);
        // lit, the halo is on the target (its own plate tinted) and the icon stays in its own colour either way
        CHECK(b.countNear(316, 63, 334, 81, kErrFg, 40) >= 4);
        b.savePng(shotDir() + "map-specimen-blink.png");
    }

    // =============================== the gestures ===============================
    auto reset = [&] {
        settle(milliseconds(520));
        moveTo(win, 250, 130);
        moveTo(win, 250, 131);
    };
    // the wheel: one `zoomed` per event, round(delta-y / 120) notches and at least one, UP positive, at the pointer's fractions
    reset();
    moveTo(win, 150, 250);
    scroll(win, 150, 250, 0, 120);
    CHECK(ui->get_zooms() == 1 && ui->get_zoom_notches() == 1);
    CHECK_NEAR(ui->get_zoom_fx(), 150.0 / kW, 0.001);
    CHECK_NEAR(ui->get_zoom_fy(), 250.0 / kH, 0.001);
    scroll(win, 400, 100, 0, -120);
    CHECK(ui->get_zooms() == 2 && ui->get_zoom_notches() == 0);                     // +1 then -1
    CHECK_NEAR(ui->get_zoom_fx(), 400.0 / kW, 0.001);
    CHECK_NEAR(ui->get_zoom_fy(), 100.0 / kH, 0.001);
    scroll(win, 400, 100, 0, 240);                                                  // a fast wheel: two notches in one event
    CHECK(ui->get_zooms() == 3 && ui->get_zoom_notches() == 2);
    scroll(win, 400, 100, 0, -60);                                                  // a 60 px click is one notch
    CHECK(ui->get_zooms() == 4 && ui->get_zoom_notches() == 1);
    scroll(win, 400, 100, 0, 5);                                                    // a touchpad trickle counts one per event
    CHECK(ui->get_zooms() == 5 && ui->get_zoom_notches() == 2);
    scroll(win, 400, 100, 0, 0);                                                    // no delta: not a wheel event
    scroll(win, 400, 100, 30, 0);                                                   // sideways: not a zoom
    CHECK(ui->get_zooms() == 5);
    // over a target the wheel still zooms (the target's area lets it through)
    scroll(win, 208, 72, 0, 120);
    CHECK(ui->get_zooms() == 6 && ui->get_zoom_notches() == 3);
    CHECK_NEAR(ui->get_zoom_fx(), 208.0 / kW, 0.001);
    CHECK(ui->get_clicks() == 0 && ui->get_target_clicks() == 0 && ui->get_pans() == 0);

    // a click on the map: `clicked` at the pointer's fractions, no pan, no target
    reset();
    click(win, 200, 250);
    CHECK(ui->get_clicks() == 1 && ui->get_pans() == 0 && ui->get_target_clicks() == 0);
    CHECK_NEAR(ui->get_click_x(), 200.0 / kW, 0.001);
    CHECK_NEAR(ui->get_click_y(), 250.0 / kH, 0.001);
    // a press that moves 3 px is still a click
    reset();
    press(win, 300, 250);
    moveTo(win, 302, 252);
    release(win, 302, 252);
    CHECK(ui->get_clicks() == 2 && ui->get_pans() == 0);
    CHECK_NEAR(ui->get_click_x(), 302.0 / kW, 0.001);

    // a drag: past 4 px of travel it is a pan, the pointer's step as a fraction of the pane per move, and not a click
    reset();
    press(win, 200, 250);
    moveTo(win, 203, 250);                                                          // 3 px: still a click in the making
    CHECK(ui->get_pans() == 0);
    moveTo(win, 230, 250);                                                          // 30 px in all
    CHECK(ui->get_pans() == 1);
    CHECK_NEAR(ui->get_pan_dx(), 27.0 / kW, 0.0005);                                // this move's step, 203 -> 230 (the 3 px inside the 4 px were not a pan)
    CHECK_NEAR(ui->get_pan_dy(), 0.0, 0.0005);
    moveTo(win, 220, 262);
    CHECK(ui->get_pans() == 2);
    CHECK_NEAR(ui->get_pan_dx(), -10.0 / kW, 0.0005);                               // leftwards is negative
    CHECK_NEAR(ui->get_pan_dy(), 12.0 / kH, 0.0005);                                // downwards is positive, a fraction of the HEIGHT
    CHECK(ui->get_grabbing());                                                      // the hand that holds the map
    release(win, 220, 262);
    CHECK(ui->get_clicks() == 2 && !ui->get_grabbing());                            // no click on release
    CHECK_NEAR(ui->get_pan_sum_x(), 17.0 / kW, 0.0005);                             // the steps add up: 27 - 10
    CHECK_NEAR(ui->get_pan_sum_y(), 12.0 / kH, 0.0005);
    // the contract's drag: a press and a 30 px move is one `panned` of 30 / 520
    reset();
    {
        const int p0 = ui->get_pans();
        press(win, 150, 200);
        moveTo(win, 180, 200);
        CHECK(ui->get_pans() == p0 + 1);
        CHECK_NEAR(ui->get_pan_dx(), 30.0 / kW, 0.0005);
        release(win, 180, 200);
    }
    // vertical travel alone pans too
    reset();
    const int pansBefore = ui->get_pans();
    press(win, 150, 250);
    moveTo(win, 150, 275);
    release(win, 150, 275);
    CHECK(ui->get_pans() == pansBefore + 1 && ui->get_clicks() == 2);
    CHECK_NEAR(ui->get_pan_dy(), 25.0 / kH, 0.0005);
    CHECK_NEAR(ui->get_pan_dx(), 0.0, 0.0005);

    // a click on a target: `target-clicked(index)`, and no click on the map; the hit area is a 20 px square
    reset();
    for (int i = 0; i < 6; ++i) {
        const int before = ui->get_target_clicks();
        click(win, static_cast<float>(kT[i].x), static_cast<float>(kT[i].y));
        CHECK(ui->get_target_clicks() == before + 1 && ui->get_last_target() == i);
        settle(milliseconds(520));
        moveTo(win, 250, 131);
    }
    CHECK(ui->get_clicks() == 2);                                                   // none of them was a click on the map
    // the corners of the square are in, one pixel past them is out
    reset();
    int tclicks = ui->get_target_clicks();
    click(win, static_cast<float>(kT[3].x - 9.5), static_cast<float>(kT[3].y - 9.5));
    CHECK(ui->get_target_clicks() == tclicks + 1 && ui->get_last_target() == 3);
    settle(milliseconds(520));
    click(win, static_cast<float>(kT[3].x + 9.5), static_cast<float>(kT[3].y + 9.5));
    CHECK(ui->get_target_clicks() == tclicks + 2 && ui->get_last_target() == 3);
    settle(milliseconds(520));
    const int mapClicks = ui->get_clicks();
    click(win, static_cast<float>(kT[3].x + 11.0), static_cast<float>(kT[3].y));
    click(win, static_cast<float>(kT[3].x), static_cast<float>(kT[3].y - 11.0));
    CHECK(ui->get_target_clicks() == tclicks + 2 && ui->get_clicks() == mapClicks + 2);     // outside the 20 px square it is the map's
    // a double-click on a target: the first press selects, the second is `target-double-clicked` and not a click
    reset();
    tclicks = ui->get_target_clicks();
    const int dbl0 = ui->get_target_dclicks();
    click(win, static_cast<float>(kT[0].x), static_cast<float>(kT[0].y));
    click(win, static_cast<float>(kT[0].x), static_cast<float>(kT[0].y));
    CHECK(ui->get_target_dclicks() == dbl0 + 1 && ui->get_last_dtarget() == 0);
    CHECK(ui->get_target_clicks() == tclicks + 1);                                  // the first press selected; the second was not a click
    // two clicks on two targets are two clicks, and two slow ones are two
    reset();
    tclicks = ui->get_target_clicks();
    click(win, static_cast<float>(kT[0].x), static_cast<float>(kT[0].y));
    click(win, static_cast<float>(kT[1].x), static_cast<float>(kT[1].y));
    CHECK(ui->get_target_clicks() == tclicks + 2 && ui->get_target_dclicks() == dbl0 + 1);
    reset();
    click(win, static_cast<float>(kT[0].x), static_cast<float>(kT[0].y));
    settle(milliseconds(650));
    click(win, static_cast<float>(kT[0].x), static_cast<float>(kT[0].y));
    CHECK(ui->get_target_clicks() == tclicks + 4 && ui->get_target_dclicks() == dbl0 + 1);
    // a drag that starts on a target pans the map, and selects nothing
    reset();
    tclicks = ui->get_target_clicks();
    const int pansNow = ui->get_pans();
    press(win, static_cast<float>(kT[4].x), static_cast<float>(kT[4].y));
    moveTo(win, static_cast<float>(kT[4].x + 30), static_cast<float>(kT[4].y));
    CHECK(ui->get_pans() == pansNow + 1);
    CHECK_NEAR(ui->get_pan_dx(), 30.0 / kW, 0.0005);
    release(win, static_cast<float>(kT[4].x + 30), static_cast<float>(kT[4].y));
    CHECK(ui->get_target_clicks() == tclicks);

    // the hover: `hovered(index)`, -1 on leaving; the host's text is the tip, drawn after 600 ms of rest
    reset();
    ui->set_hover_idx(-5);
    moveTo(win, static_cast<float>(kT[3].x), static_cast<float>(kT[3].y));
    CHECK(ui->get_hover_idx() == 3 && ui->get_pane_hover() == 3);
    moveTo(win, 250, 250);
    CHECK(ui->get_hover_idx() == -1);
    moveTo(win, static_cast<float>(kT[3].x), static_cast<float>(kT[3].y));
    moveTo(win, static_cast<float>(kT[4].x), static_cast<float>(kT[4].y));            // straight from one to the next
    CHECK(ui->get_hover_idx() == 4);
    {
        // the tip: nothing at 300 ms, the TooltipFace (Theme.menu) under the target at 650, gone when the pointer leaves
        settle(milliseconds(300));
        Frame wait = render(ui, kW, kH);
        CHECK(wait.countNear(static_cast<int>(kT[4].x) - 20, static_cast<int>(kT[4].y) + 22, static_cast<int>(kT[4].x) + 60, static_cast<int>(kT[4].y) + 44, kMenu, 3) == 0);
        CHECK(!ui->get_tip_shown());
        settle(milliseconds(600));
        Frame tip = render(ui, kW, kH);
        CHECK(ui->get_tip_shown());
        CHECK(tip.countNear(static_cast<int>(kT[4].x) - 20, static_cast<int>(kT[4].y) + 22, static_cast<int>(kT[4].x) + 60, static_cast<int>(kT[4].y) + 44, kMenu, 3) > 300);
        CHECK(tip.countInk(static_cast<int>(kT[4].x) - 8, static_cast<int>(kT[4].y) + 26, static_cast<int>(kT[4].x) + 50, static_cast<int>(kT[4].y) + 40, kMenu, 30) > 20);   // "Target 4"
        tip.savePng(shotDir() + "map-specimen-tip.png");
        // a click with the tip up is a click (a popup tip would have swallowed it)
        const int tc = ui->get_target_clicks();
        click(win, static_cast<float>(kT[4].x), static_cast<float>(kT[4].y));
        CHECK(ui->get_target_clicks() == tc + 1 && ui->get_last_target() == 4);
        CHECK(!ui->get_tip_shown());                                                // a press hides it
        // and a wheel with it up is a wheel
        const int z = ui->get_zooms();
        moveTo(win, 250, 250);
        moveTo(win, static_cast<float>(kT[4].x), static_cast<float>(kT[4].y));
        settle(milliseconds(700));
        CHECK(ui->get_tip_shown());
        scroll(win, static_cast<float>(kT[4].x), static_cast<float>(kT[4].y), 0, 120);
        CHECK(ui->get_zooms() == z + 1);
        moveTo(win, 250, 250);
        CHECK(ui->get_hover_idx() == -1 && !ui->get_tip_shown());
        CHECK(render(ui, kW, kH).countNear(static_cast<int>(kT[4].x) - 20, static_cast<int>(kT[4].y) + 22, static_cast<int>(kT[4].x) + 60, static_cast<int>(kT[4].y) + 44, kMenu, 3) == 0);
    }
    // no words from the host, no tip
    reset();
    moveTo(win, static_cast<float>(kT[3].x), static_cast<float>(kT[3].y));
    ui->set_hover_text("");
    settle(milliseconds(800));
    CHECK(!ui->get_tip_shown());
    moveTo(win, 250, 250);

    // the zoom keys: `zoom-key(+1)` and `zoom-key(-1)`; a disabled one does nothing
    reset();
    const int clicksBeforeKeys = ui->get_clicks();
    click(win, 497, 308);
    CHECK(ui->get_zoom_keys() == 1 && ui->get_last_zoom_key() == 1);
    click(win, 497, 338);
    CHECK(ui->get_zoom_keys() == 2 && ui->get_last_zoom_key() == -1);
    CHECK(ui->get_clicks() == clicksBeforeKeys);                                    // (a key press is not a click on the map)
    ui->set_zoom_in_enabled(false);
    click(win, 497, 308);
    CHECK(ui->get_zoom_keys() == 2);
    ui->set_zoom_in_enabled(true);
    ui->set_zoom_out_enabled(false);
    click(win, 497, 338);
    CHECK(ui->get_zoom_keys() == 2);
    ui->set_zoom_out_enabled(true);
    {   // a key is not a drag handle: a press on it and a drag off it pans nothing
        const int p0 = ui->get_pans(), c0 = ui->get_clicks();
        press(win, 497, 308);
        moveTo(win, 420, 308);
        release(win, 420, 308);
        CHECK(ui->get_pans() == p0 && ui->get_clicks() == c0);
    }

    // the pick: the crosshair, and the next click is a position (even over a target)
    reset();
    CHECK(!ui->get_crosshair());
    ui->set_pick_armed(true);
    CHECK(ui->get_crosshair());
    {
        const int c0 = ui->get_clicks(), t0 = ui->get_target_clicks();
        click(win, static_cast<float>(kT[3].x), static_cast<float>(kT[3].y));
        CHECK(ui->get_clicks() == c0 + 1 && ui->get_target_clicks() == t0);
        CHECK_NEAR(ui->get_click_x(), kT[3].x / kW, 0.001);
        CHECK(ui->get_crosshair());                                                 // the host disarms it
        ui->set_pick_armed(false);
        CHECK(!ui->get_crosshair());
        click(win, static_cast<float>(kT[3].x), static_cast<float>(kT[3].y));
        CHECK(ui->get_target_clicks() == t0 + 1 && ui->get_clicks() == c0 + 1);
    }

    // =============================== the size is reported ===============================
    {
        CHECK(std::lround(ui->get_pane_w()) == kW && std::lround(ui->get_pane_h()) == kH);   // the first layout is read, not announced
        const int r0 = ui->get_resizes();
        ui->set_view_w(400.0f);
        ui->set_view_h(300.0f);
        Frame small = render(ui, kW, kH);
        CHECK(ui->get_resizes() == r0 + 1);                                         // width and height both moved: one report, with both
        CHECK(std::lround(ui->get_resized_w()) == 400 && std::lround(ui->get_resized_h()) == 300);
        CHECK(std::lround(ui->get_pane_w()) == 400 && std::lround(ui->get_pane_h()) == 300);
        // the pane follows: the keys are at its new bottom right (x 365..390, y 235..291), the legend at its new top right (right edge 390)
        CHECK(small.countNear(366, 236, 390, 260, kKeyBg, 4) > 300 && small.countNear(366, 266, 390, 290, kKeyBg, 4) > 300);
        CHECK(small.near(450, 100, kBg, 2) && small.near(200, 330, kBg, 2));       // outside the pane: the window's own ground (the pane clips)
        int right = -1;
        for (int x = 250; x < 400; ++x) { if (small.near(x, 40, kLine, 6)) { right = std::max(right, x); } }
        CHECK(right == 400 - 11);
        small.savePng(shotDir() + "map-specimen-narrow.png");
        render(ui, kW, kH);
        CHECK(ui->get_resizes() == r0 + 1);                                         // the same size is not a change
        ui->set_view_w(450.0f);
        render(ui, kW, kH);
        CHECK(ui->get_resizes() == r0 + 2 && std::lround(ui->get_resized_w()) == 450 && std::lround(ui->get_resized_h()) == 300);
        ui->set_view_h(340.0f);
        render(ui, kW, kH);
        CHECK(ui->get_resizes() == r0 + 3 && std::lround(ui->get_resized_h()) == 340);
        ui->set_view_w(520.0f);
        ui->set_view_h(360.0f);
        render(ui, kW, kH);
        CHECK(ui->get_resizes() == r0 + 4 && std::lround(ui->get_resized_w()) == kW && std::lround(ui->get_resized_h()) == kH);
        CHECK(std::lround(ui->get_pane_w()) == kW && std::lround(ui->get_pane_h()) == kH);
    }

    // =============================== a disabled pane acts on nothing ===============================
    ui->set_pane_enabled(false);
    settle(milliseconds(520));
    {
        const int zooms = ui->get_zooms(), clicks = ui->get_clicks(), tcl = ui->get_target_clicks(), pans = ui->get_pans(), keys = ui->get_zoom_keys(), dcl = ui->get_target_dclicks();
        ui->set_hover_idx(-7);
        click(win, 200, 250);
        click(win, static_cast<float>(kT[0].x), static_cast<float>(kT[0].y));
        click(win, static_cast<float>(kT[0].x), static_cast<float>(kT[0].y));
        scroll(win, 200, 250, 0, 120);
        press(win, 200, 250);
        moveTo(win, 240, 250);
        release(win, 240, 250);
        click(win, 497, 308);
        click(win, 497, 338);
        moveTo(win, static_cast<float>(kT[3].x), static_cast<float>(kT[3].y));
        CHECK(ui->get_zooms() == zooms && ui->get_clicks() == clicks && ui->get_target_clicks() == tcl && ui->get_pans() == pans);
        if (ui->get_zoom_keys() != keys || ui->get_target_dclicks() != dcl || ui->get_hover_idx() != -7) {
            std::printf("  disabled: zoom keys %d (was %d), dclicks %d (was %d), hover %d\n", ui->get_zoom_keys(), keys, ui->get_target_dclicks(), dcl, ui->get_hover_idx());
        }
        CHECK(ui->get_zoom_keys() == keys && ui->get_target_dclicks() == dcl && ui->get_hover_idx() == -7);
        Frame off = render(ui, kW, kH);
        CHECK(off.countNear(486, 296, 510, 320, kKeyDisabledBg, 4) > 300);         // the keys look disabled
    }
    ui->set_pane_enabled(true);

    // =============================== the icons: one shape a kind ===============================
    {
        // five kinds and a second aircraft, all white, plain, at the same height: the glyphs differ by kind and agree for one kind
        const int kinds[6] = {1, 2, 3, 4, 0, 1};
        const double xs[6] = {60, 130, 200, 340, 410, 470};
        std::vector<MapTarget> v;
        for (int i = 0; i < 6; ++i) {
            v.push_back(MapTarget{"K", static_cast<float>(xs[i] / kW), 0.5f, -1.0f, kinds[i], "", 1.0f, false, false, false, false, slint::Color::from_rgb_uint8(255, 255, 255)});
        }
        ui->set_targets(std::make_shared<slint::VectorModel<MapTarget>>(std::move(v)));
        const Frame m = render(ui, kW, kH);
        // the ink of each, as a set of pixels in a 26 x 26 box about its centre
        std::vector<std::vector<bool>> mask(6, std::vector<bool>(26 * 26, false));
        int bbw[6], bbh[6];
        for (int i = 0; i < 6; ++i) {
            int x0 = 999, y0 = 999, x1 = -1, y1 = -1;
            for (int dy = -13; dy < 13; ++dy) {
                for (int dx = -13; dx < 13; ++dx) {
                    const int x = static_cast<int>(xs[i]) + dx, y = 180 + dy;
                    if (x == 300 || y == 250 || nearColour(m.at(x, y), baseAt(x), 40)) { continue; }
                    mask[static_cast<std::size_t>(i)][static_cast<std::size_t>((dy + 13) * 26 + dx + 13)] = true;
                    x0 = std::min(x0, x);
                    x1 = std::max(x1, x);
                    y0 = std::min(y0, y);
                    y1 = std::max(y1, y);
                }
            }
            bbw[i] = x1 - x0 + 1;
            bbh[i] = y1 - y0 + 1;
        }
        const auto jaccard = [&](int a, int b) {
            int both = 0, either = 0;
            for (std::size_t k = 0; k < 26 * 26; ++k) {
                if (mask[static_cast<std::size_t>(a)][k] && mask[static_cast<std::size_t>(b)][k]) { ++both; }
                if (mask[static_cast<std::size_t>(a)][k] || mask[static_cast<std::size_t>(b)][k]) { ++either; }
            }
            return either > 0 ? static_cast<double>(both) / either : 0.0;
        };
        CHECK(jaccard(0, 5) > 0.95);                                                // two aircraft: the same glyph
        for (int a = 0; a < 5; ++a) {
            for (int b = a + 1; b < 5; ++b) {
                if (jaccard(a, b) >= 0.75) { std::printf("  kinds %d and %d look alike (%.2f)\n", kinds[a], kinds[b], jaccard(a, b)); }
                CHECK(jaccard(a, b) < 0.75);                                        // every kind its own
            }
        }
        // and each is the shape its kind is: an aircraft is as wide as it is long, a vessel and a mast stand tall and narrow, a satellite is wide
        // and not tall (its panels), the unknown is a diamond of about 14 px; all of them within the 18 px box
        CHECK(bbw[0] >= 14 && bbw[0] <= 18 && bbh[0] >= 14 && bbh[0] <= 18);        // aircraft
        CHECK(bbw[1] >= 7 && bbw[1] <= 11 && bbh[1] >= 14 && bbh[1] <= 18);         // vessel
        CHECK(bbw[2] >= 8 && bbw[2] <= 12 && bbh[2] >= 13 && bbh[2] <= 18);         // station
        CHECK(bbw[3] >= 16 && bbw[3] <= 19 && bbh[3] >= 10 && bbh[3] <= 15);        // satellite
        CHECK(bbw[4] >= 12 && bbw[4] <= 16 && bbh[4] >= 12 && bbh[4] <= 16);        // unknown
        zoomed(m, 40, 160, 460, 40, 3).savePng(shotDir() + "map-specimen-kinds.png");
    }

    // =============================== the legend and the scale follow their models ===============================
    {
        const auto row = [](const char* label, std::uint8_t r, std::uint8_t g, std::uint8_t b) { return LegendRow{label, slint::Color::from_rgb_uint8(r, g, b)}; };
        ui->set_targets(sixTargets());
        ui->set_legend(std::make_shared<slint::VectorModel<LegendRow>>(std::vector<LegendRow>{row("ONE", 0xff, 0x20, 0x20)}));
        Frame one = render(ui, kW, kH);
        const Box s1 = boxOf(one, 380, 0, kW, 140, rgb(0xff2020), 6);
        CHECK(s1.n >= 80 && s1.n <= 100 && s1.x1 - s1.x0 == 9 && s1.y1 - s1.y0 == 9);            // one swatch, 10 x 10 (its corners are rounded)
        const Box plate1 = boxOf(one, 380, 0, kW, 140, rgb(0x2b2f37), 6);           // (the plate's own line)
        ui->set_legend(std::make_shared<slint::VectorModel<LegendRow>>(std::vector<LegendRow>{
            row("A", 0xff, 0x20, 0x20), row("B", 0x20, 0xff, 0x20), row("C", 0x20, 0x20, 0xff), row("D", 0xff, 0xff, 0x20), row("E", 0xff, 0x20, 0xff)}));
        Frame five = render(ui, kW, kH);
        CHECK(boxOf(five, 380, 0, kW, 140, rgb(0xff2020), 6).n >= 80 && boxOf(five, 380, 0, kW, 140, rgb(0x20ff20), 6).n >= 80);
        CHECK(boxOf(five, 380, 0, kW, 140, rgb(0xff20ff), 6).n >= 80);              // the fifth row is there
        int bottom1 = 0, bottom5 = 0;
        for (int y = 0; y < 140; ++y) {
            if (one.near(500, y, rgb(0x2b2f37), 6)) { bottom1 = std::max(bottom1, y); }
            if (five.near(500, y, rgb(0x2b2f37), 6)) { bottom5 = std::max(bottom5, y); }
        }
        CHECK(plate1.n > 0 && bottom1 > 20 && bottom5 > bottom1 + 3 * 14);          // the plate grows by a row each: four more rows, 16 px apiece
        ui->set_legend(std::make_shared<slint::VectorModel<LegendRow>>(std::vector<LegendRow>{}));
        Frame none = render(ui, kW, kH);
        CHECK(regionInk(none, 420, 0, kW, 140) == 0);                               // no rows: no plate (the emergency's label ends before 420)
        // the scale text alone: the words, and no bar
        ui->set_scale_frac(0.0f);
        ui->set_scale_text("5 nm");
        Frame words = render(ui, kW, kH);
        CHECK(words.countInk(12, 316, 60, 332, kBaseL, 20) > 15 && boxOf(words, 0, 333, 200, 342, kFgSoft, 6).n == 0);
        ui->set_scale_frac(0.2f);
        ui->set_scale_text("10 km");
        ui->set_legend(std::make_shared<slint::VectorModel<LegendRow>>(std::vector<LegendRow>{row("0-5 kft", 0xf5, 0xa5, 0x24), row("5-20 kft", 0x3d, 0xdc, 0x84), row("above 20 kft", 0x7a, 0xd7, 0xff)}));
        render(ui, kW, kH);
    }

    // =============================== the targets follow their model, row by row ===============================
    {
        auto model = sixTargets();
        ui->set_targets(model);
        Frame before = render(ui, kW, kH);
        CHECK(ringNear(before, kT[3].x, kT[3].y, 14.0, 17.5, kAccent, 40) == 0);
        MapTarget t = *model->row_data(3);
        t.selected = true;
        model->set_row_data(3, t);                                                  // the station is selected
        Frame after = render(ui, kW, kH);
        CHECK(ringNear(after, kT[3].x, kT[3].y, 14.0, 17.5, kAccent, 40) >= 70);
        t.x = 0.30f;
        model->set_row_data(3, t);                                                  // ... and moves
        Frame moved = render(ui, kW, kH);
        CHECK(ringNear(moved, kT[3].x, kT[3].y, 14.0, 17.5, kAccent, 40) == 0 && ringNear(moved, 0.30 * kW, kT[3].y, 14.0, 17.5, kAccent, 40) >= 70);
        click(win, static_cast<float>(0.30 * kW), static_cast<float>(kT[3].y));
        CHECK(ui->get_last_target() == 3);                                          // its hit area went with it
        const int before2 = ui->get_target_clicks();
        click(win, static_cast<float>(kT[3].x), static_cast<float>(kT[3].y));
        CHECK(ui->get_target_clicks() == before2);                                  // and left the old place
        model->push_back(MapTarget{"N7", 0.80f, 0.80f, -1.0f, 2, "", 1.0f, false, false, false, false, slint::Color::from_rgb_uint8(0x3d, 0xdc, 0x84)});
        Frame grown = render(ui, kW, kH);
        CHECK(inkAround(grown, Pt{0.80 * kW, 0.80 * kH}, 9) > 10);                  // a seventh appears
        model->erase(6);
        model->erase(0);
        Frame shrunk = render(ui, kW, kH);
        CHECK(inkAround(shrunk, Pt{0.80 * kW, 0.80 * kH}, 9) == 0);
        CHECK(ringNear(shrunk, kT[0].x, kT[0].y, 8.0, 24.0, kAccent, 40) == 0);     // the selected aircraft was the first row: gone
        ui->set_targets(sixTargets());
        render(ui, kW, kH);
    }

    // =============================== the odd inputs ===============================
    {
        // targets off the pane are not drawn (nor hit); a target at the very edge is; a transparent one is not; an unknown kind is the unknown icon
        std::vector<MapTarget> odd;
        const auto push = [&](float x, float y, int kind, float alpha, const char* label) {
            odd.push_back(MapTarget{"X", x, y, -1.0f, kind, label, alpha, false, false, false, false, slint::Color::from_rgb_uint8(0xff, 0xff, 0xff)});
        };
        push(1.30f, 0.50f, 1, 1.0f, "OUT");
        push(0.50f, -0.30f, 1, 1.0f, "UP");
        push(0.50f, 0.50f, 99, 1.0f, "");
        push(0.30f, 0.50f, 2, 0.0f, "GHOST");
        push(0.0f, 0.0f, 3, 1.0f, "");
        ui->set_targets(std::make_shared<slint::VectorModel<MapTarget>>(std::move(odd)));
        Frame o = render(ui, kW, kH);
        CHECK(o.countInk(kW - 16, 170, kW, 190, kBaseR, 20) == 0);                  // 1.3 across: off
        CHECK(inkAround(o, Pt{260, 180}, 9) > 12);                                  // kind 99: the unknown icon, drawn
        CHECK(o.countInk(140, 170, 175, 190, kBaseL, 20) == 0);                     // alpha 0: nothing
        CHECK(o.countInk(0, 0, 9, 9, kBaseL, 20) > 4);                              // at the corner: its quarter shows
        ui->set_targets(sixTargets());
        render(ui, kW, kH);
    }
    f = render(ui, kW, kH);
    f.savePng(shotDir() + "map-specimen-final.png");

    return testSummary("test_slint_map");
}
