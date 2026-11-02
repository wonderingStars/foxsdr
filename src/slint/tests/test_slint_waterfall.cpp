// The waterfall: the lower well of the Workbench sheet, drawn from its CSS and probed pixel by pixel - the picture's
// rectangle, the age strip, the strength key and its ramp, the foot, the VFO line, the plugin mark and the marker lines
// (dashed), the marker tabs and how they stack - then driven the way the operator drives it: hover, click, a press held
// 0.45 s (not a click), a pan after 4 px, double-click, right-click, the wheel, the marker under the pointer, the tips.
// Text is in a fallback typeface on this machine: it is asserted as INK in a region, never as glyph shapes or widths.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "waterfall_specimen.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace foxsdr::slinttest;
using std::chrono::milliseconds;

namespace {

constexpr int kW = 1024, kH = 270;
constexpr int kPicX = 44, kPicW = 980, kPicH = 238;
constexpr int kFootTop = kH - 28;

const Rgb kBlue = rgb(0x0000ff);
const Rgb kGreen = rgb(0x00ff00);
const Rgb kWhite = rgb(0xffffff);
// Theme.* (the Workbench filling), the same values the sheet's CSS has
const Rgb kWellDeep = rgb(0x06070a);
const Rgb kWell = rgb(0x0d0f12);
const Rgb kLine = rgb(0x2b2f37);
const Rgb kKnobRing = rgb(0x434955);
const Rgb kAccentSoft = rgb(0xf5c66b);
const Rgb kVfo = rgb(0xffd27a);
const Rgb kChipBg = rgb(0x262a32);
const Rgb kChipLine = rgb(0x363b45);
const Rgb kChipWarnBg = rgb(0x3a2a0e);
const Rgb kMenu = rgb(0x262a32);
const Rgb kRampFloor = rgb(0x0a1430), kRampMid = rgb(0x1b3a6a), kRampHigh = rgb(0x2e86c1), kRampHot = rgb(0xf5a524), kRampPeak = rgb(0xffffff);

// the strength key's plate ground: Theme.wf-key-bg (the well at 0xdd) over the blue test picture
const Rgb kPlateOverBlue = over(kWell, 221.0 / 255.0, kBlue);
// the marker line (ivory at 60 %), the plugin mark (gold at 60 %) and the cursor (ivory at 40 %), over the blue picture
const Rgb kMarkerLine = over(rgb(0xe6e8ec), 0.6, kBlue);
const Rgb kPluginLine = over(rgb(0xd4b060), 0.6, kBlue);
const Rgb kCursorLine = over(rgb(0xe6e8ec), 0.4, kBlue);

// The column in [x0, x1) holding the most pixels near `want` in rows [y0, y1); -1 when none.
int bestColumn(const Frame& f, int x0, int x1, int y0, int y1, Rgb want, int tol = 10) {
    int best = -1, bestN = 0;
    for (int x = x0; x < x1; ++x) {
        const int n = f.countNear(x, y0, x + 1, y1, want, tol);
        if (n > bestN) { bestN = n; best = x; }
    }
    return best;
}

// A 1 px vertical line drawn as dashes: `on` for the first 3 rows of every 6 (counted from the picture's top), `off` for the next 3.
bool isDashed(const Frame& f, int x, int y0, int y1, Rgb on, Rgb off, int tol = 10) {
    for (int y = y0; y < y1; ++y) {
        if (!f.near(x, y, (y % 6) < 3 ? on : off, tol)) { return false; }
    }
    return y1 > y0;
}

bool isSolid(const Frame& f, int x, int y0, int y1, Rgb want, int tol = 10) { return f.colNear(x, y0, y1, want, tol); }

// The horizontal extent [first, last] of the pixels in row y that are a chip's ground or its border; first = -1 when none.
struct Span { int first = -1, last = -1; };
Span chipSpan(const Frame& f, int y, int x0 = 0, int x1 = kW) {
    Span s;
    for (int x = x0; x < x1; ++x) {
        if (f.near(x, y, kChipBg, 4) || f.near(x, y, kChipLine, 4)) {
            if (s.first < 0) { s.first = x; }
            s.last = x;
        }
    }
    return s;
}

int chipPixels(const Frame& f, int x0, int y0, int x1, int y1) { return f.countNear(x0, y0, x1, y1, kChipBg, 4); }

// How wide the tip's face is: its ground (Theme.menu) along one row of its top padding, from x0 to the right edge of the frame.
int tipWidth(const Frame& f, int x0, int y) { return f.countNear(x0, y, kW, y + 1, kMenu, 4); }

slint::Image bluePicture() {
    slint::SharedPixelBuffer<slint::Rgba8Pixel> pic(kPicW, kPicH);
    for (int y = 0; y < kPicH; ++y) {
        for (int x = 0; x < kPicW; ++x) {
            slint::Rgba8Pixel* p = pic.begin() + (static_cast<std::size_t>(y) * kPicW + x);
            *p = (x == 490) ? slint::Rgba8Pixel{0, 255, 0, 255} : slint::Rgba8Pixel{0, 0, 255, 255};
        }
    }
    return slint::Image(pic);
}

UserMarker marker(float pos, const char* label) {
    UserMarker m;
    m.pos = pos;
    m.label = label;
    return m;
}

template <class Ui>
void setMarkers(Ui& ui, std::vector<UserMarker> list) {
    ui->set_markers(std::make_shared<slint::VectorModel<UserMarker>>(std::move(list)));
}

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = WaterfallSpecimen::create();
    slint::Window& win = ui->window();
    ui->set_picture(bluePicture());

    // =============================== the sheet, at rest ===============================
    Frame f = render(ui, kW, kH);
    CHECK(f.width == kW && f.height == kH);

    // ---- the picture's rectangle: x 44 .. width, y 0 .. height - 32 ----
    CHECK(f.near(kPicX, 0, kBlue));                       // top-left of the picture
    CHECK(f.near(kW - 1, kPicH - 1, kBlue));              // bottom-right of the picture
    CHECK(f.near(kPicX + 490, 100, kGreen));              // the green test column, at the picture's x 490
    CHECK(f.near(kPicX + 489, 100, kBlue) && f.near(kPicX + 491, 100, kBlue));
    CHECK(f.near(kPicX - 1, 100, kWellDeep));             // the gutter beside it is the well
    CHECK(f.near(500, 250, kWellDeep));                   // the foot band is the well too
    CHECK(f.near(300, kPicH + 1, kWellDeep) && f.near(300, kFootTop - 1, kWellDeep));   // 4 px of bare well under the picture

    // ---- the age strip: the cap "Age" at (6, 4), the ticks at x 8, tops 22 / 76 / 130 / 184 ----
    CHECK(f.countInk(6, 4, 40, 15, kWellDeep) > 15);
    for (int top : {22, 76, 130, 184}) { CHECK(f.countInk(8, top, 40, top + 13, kWellDeep) > 8); }
    CHECK(f.countInk(8, 40, 40, 70, kWellDeep) == 0);     // nothing between the ticks
    CHECK(f.countInk(8, 100, 40, 126, kWellDeep) == 0);
    CHECK(f.countInk(8, 0, 40, 3, kWellDeep) == 0);       // the cap starts at y 4
    {   // the gutter's ink is the dim of the sheet's .axis, not a bright colour
        int brightest = 0;
        for (int y = 22; y < 36; ++y) { for (int x = 8; x < 40; ++x) { brightest = std::max(brightest, int(f.at(x, y).b)); } }
        CHECK(brightest > 0x50 && brightest <= 0x94 + 8);   // Theme.dim is 0x7d8694: its blue is 0x94
    }

    // ---- the strength key plate: top right, right 14 / top 10, padding 8 x 10, on Theme.wf-key-bg ----
    const int kx = int(std::lround(ui->get_key_x())), ky = int(std::lround(ui->get_key_y()));
    const int kw = int(std::lround(ui->get_key_w())), kh = int(std::lround(ui->get_key_h()));
    CHECK(kw >= 180 + 22 && kw < 300);                    // the 180 px ramp plus padding 10 and the 1 px border either side
    CHECK(kh > 50 && kh < 120);
    CHECK(kx + kw == kW - 14);                            // right 14
    CHECK(ky == 10);                                      // top 10
    CHECK(f.near(kx + 4, ky + kh / 2, kPlateOverBlue));   // the ground: the well at 0xdd over the picture
    CHECK(f.near(kx + kw - 5, ky + kh / 2, kPlateOverBlue));
    CHECK(f.near(kx + kw / 2, ky, kLine));                // the 1 px border: top...
    CHECK(f.near(kx + kw / 2, ky + kh - 1, kLine) || f.near(kx + kw / 2, ky + kh - 2, kLine) || f.near(kx + kw / 2, ky + kh, kLine));   // ...bottom
    CHECK(f.near(kx, ky + kh / 2, kLine) && f.near(kx + kw - 1, ky + kh / 2, kLine));    // ...and the sides
    CHECK(f.near(kx + kw / 2, ky - 1, kBlue) && f.near(kx - 1, ky + kh / 2, kBlue));      // the picture just outside it
    // the 180 px ramp: floor, mid, high, hot, peak at 0, 25, 50, 75 and 100 %
    const int rampRight = kx + kw - 12;                   // the last pixel of the ramp
    const int rampTop = f.firstRowNear(rampRight - 2, ky, ky + kh, kWhite, 40);
    CHECK(rampTop > ky + 8 && rampTop < ky + 40);
    if (rampTop > 0) {
        const int rampLeft = kx + 11;
        const int rampW = rampRight - rampLeft + 1;
        CHECK(rampW >= 180);
        const int row = rampTop + 4;
        CHECK(f.near(rampLeft + 1, row, kRampFloor, 14));
        CHECK(f.near(rampLeft + rampW / 4, row, kRampMid, 16));
        CHECK(f.near(rampLeft + rampW / 2, row, kRampHigh, 16));
        CHECK(f.near(rampLeft + 3 * rampW / 4, row, kRampHot, 16));
        CHECK(f.near(rampRight - 1, row, kRampPeak, 24));
        CHECK(f.near(rampLeft + rampW / 2, rampTop - 1, kPlateOverBlue));         // the ramp is 8 px high
        CHECK(f.near(rampLeft + rampW / 2, rampTop + 7, kRampHigh, 16));
        CHECK(f.near(rampLeft + rampW / 2, rampTop + 8, kPlateOverBlue));
        // the dB labels under it and the FLOOR / CEILING line under those: ink on the plate
        CHECK(f.countInk(rampLeft, rampTop + 10, rampRight + 1, rampTop + 22, kPlateOverBlue) > 20);
        CHECK(f.countInk(rampLeft, rampTop + 22, rampRight + 1, ky + kh - 6, kPlateOverBlue) > 30);
        // the title above it
        CHECK(f.countInk(rampLeft, ky + 6, rampLeft + 70, rampTop - 2, kPlateOverBlue) > 15);
    }
    // MANUAL: absent, then present once the floor and ceiling are the user's own
    CHECK(f.countNear(kx, ky, kx + kw, ky + kh, kChipWarnBg, 4) == 0);
    const int plateH0 = kh;
    ui->set_key_manual(true);
    Frame manual = render(ui, kW, kH);
    CHECK(manual.countNear(kx, ky, kx + kw, ky + kh + 8, kChipWarnBg, 4) > 40);
    CHECK(int(std::lround(ui->get_key_h())) >= plateH0);
    ui->set_key_manual(false);
    // the coverage line: only when there is something to say, in the accent
    ui->set_key_coverage("TOP 128 LINES");
    Frame cover = render(ui, kW, kH);
    CHECK(int(std::lround(ui->get_key_h())) > plateH0 + 6);
    CHECK(cover.countNear(kx, ky + plateH0 - 6, kx + kw, ky + int(std::lround(ui->get_key_h())), kAccentSoft, 40) > 10);
    ui->set_key_coverage("");

    // ---- the foot: x 44 .. width - 14, 28 px, a hairline on top ----
    CHECK(f.rowNear(kFootTop, kPicX, kW - 14, kLine));
    CHECK(f.near(kPicX - 1, kFootTop, kWellDeep) && f.near(kW - 14, kFootTop, kWellDeep));
    CHECK(f.near(500, kFootTop + 1, kWellDeep) && f.near(500, kFootTop - 1, kWellDeep));
    CHECK(f.countInk(kPicX, kFootTop + 2, 300, kH, kWellDeep) > 25);                      // foot-left, at the left
    CHECK(f.countInk(kPicX, kFootTop + 2, 300, kFootTop + 6, kWellDeep) == 0);            // vertically centred, not hugging the rule
    CHECK(f.countNear(700, kFootTop + 2, kW - 14, kH, kAccentSoft, 40) > 12);             // foot-tuned, in the accent
    CHECK(f.countNear(960, kFootTop + 2, kW - 14, kH, kAccentSoft, 30) == 0);             // foot-rest is the muted ink after it
    CHECK(f.countInk(960, kFootTop + 2, kW - 14, kH, kWellDeep) > 12);
    CHECK(f.countInk(kW - 13, kFootTop + 2, kW, kH, kWellDeep) == 0);                     // nothing past the right margin

    // ---- the VFO line: 1 px Theme.wf-vfo at 0.44 of the picture, down its whole height ----
    const int vfoX = bestColumn(f, 470, 481, 2, 236, kVfo);
    CHECK(vfoX >= 474 && vfoX <= 476);                                                    // 44 + 0.44 * 980 = 475.2
    CHECK(vfoX > 0 && isSolid(f, vfoX, 0, kPicH, kVfo));
    CHECK(f.near(vfoX - 2, 100, kBlue) && f.near(vfoX + 2, 100, kBlue));
    ui->set_show_vfo(false);
    Frame novfo = render(ui, kW, kH);
    CHECK(novfo.countNear(470, 2, 481, 236, kVfo) == 0);
    ui->set_show_vfo(true);

    // ---- the plugin mark at 0.10: a dashed line (columns of short rectangles), no label ----
    const int markX = bestColumn(f, 138, 147, 0, 190, kPluginLine);
    CHECK(markX >= 141 && markX <= 143);                                                  // 44 + 0.10 * 980 = 142
    CHECK(markX > 0 && isDashed(f, markX, 0, 190, kPluginLine, kBlue));
    CHECK(f.near(markX, 3, kBlue) && f.near(markX, 0, kPluginLine) && f.near(markX, 6, kPluginLine));
    CHECK(f.countInk(markX - 40, 4, markX - 3, 40, kBlue) == 0 && f.countInk(markX + 3, 4, markX + 40, 40, kBlue) == 0);   // no label

    // ---- the user markers M1 0.306, M2 0.312, M3 0.80: dashed ivory lines at 60 % ----
    const int m1x = bestColumn(f, 340, 348, 0, 190, kMarkerLine);
    const int m2x = bestColumn(f, 348, 356, 0, 190, kMarkerLine);
    const int m3x = bestColumn(f, 824, 832, 100, 190, kMarkerLine);
    CHECK(m1x >= 343 && m1x <= 345);                                                      // 44 + 0.306 * 980 = 343.9
    CHECK(m2x >= 349 && m2x <= 351);                                                      // 44 + 0.312 * 980 = 349.8
    CHECK(m3x >= 827 && m3x <= 829);                                                      // 44 + 0.80 * 980 = 828
    CHECK(m1x > 0 && isDashed(f, m1x, 0, 190, kMarkerLine, kBlue));
    CHECK(m2x > 0 && isDashed(f, m2x, 0, 190, kMarkerLine, kBlue));
    CHECK(m3x > 0 && isDashed(f, m3x, 100, 190, kMarkerLine, kBlue));
    CHECK(f.near(m1x, 1, kMarkerLine) && f.near(m1x, 4, kBlue));                          // alternating: on, on, on, off, off, off

    // ---- the marker tabs: M1 on the picture's foot (y 216..234), M2 touching it so one row up, M3 far away so on row 0 ----
    const Span t1 = chipSpan(f, 218, 200, 600);
    CHECK(t1.first > 0);
    CHECK(t1.last - t1.first + 1 >= 80 && t1.last - t1.first + 1 <= 150);                // "M1 145.212 MHz" in a mono chip
    CHECK(std::abs((t1.first + t1.last) / 2 - m1x) <= 3);                                 // centred on its line
    CHECK(f.near(335, 216, kChipLine) && f.near(335, 233, kChipLine));                    // 18 px high: border top and bottom...
    CHECK(f.near(335, 215, kBlue) && f.near(335, 234, kBlue));                            // ...and the picture just outside it
    CHECK(f.near(335, 217, kChipBg) && f.near(335, 232, kChipBg));
    CHECK(f.countInk(t1.first + 8, 219, t1.last - 8, 231, kChipBg, 30) > 40);             // the label's ink
    CHECK(f.near(m1x, 217, kChipBg));                                                     // the tab covers its own line
    // M2: one row up (rows are a tab and 2 px apart: y 196..214), the same width, centred on its own line
    const Span t2 = chipSpan(f, 198, 200, 600);
    CHECK(t2.first > 0 && std::abs((t2.last - t2.first) - (t1.last - t1.first)) <= 3);
    CHECK(std::abs((t2.first + t2.last) / 2 - m2x) <= 3);
    CHECK(f.near(335, 196, kChipLine) && f.near(335, 213, kChipLine) && f.near(335, 195, kBlue) && f.near(335, 214, kBlue));
    CHECK(f.countInk(t2.first + 8, 199, t2.last - 8, 211, kChipBg, 30) > 40);
    CHECK(t2.last > t1.last && t2.last - t1.last <= 10);                                  // only a few px right of M1's tab
    // M2's tab is not on M1's row: where it would stick out past M1's, the foot row holds no chip
    CHECK(chipPixels(f, t1.last + 6, 220, t1.last + 12, 230) == 0);
    CHECK(chipPixels(f, t1.first - 12, 220, t1.first - 6, 230) == 0);
    // M3 is alone: row 0, at the foot band, and nothing above it
    const Span t3 = chipSpan(f, 218, 700, 1000);
    CHECK(t3.first > 0 && std::abs((t3.first + t3.last) / 2 - m3x) <= 3);
    CHECK(f.near(m3x, 217, kChipBg));
    CHECK(chipPixels(f, 700, 176, 1000, 214) == 0);

    f.savePng(shotDir() + "waterfall-specimen.png");

    // ---- tabs are a stack of at most 3 rows; a 4th touching marker keeps its line and loses its tab ----
    setMarkers(ui, {marker(0.300f, "M1 145.200 MHz"), marker(0.302f, "M2 145.202 MHz"), marker(0.304f, "M3 145.204 MHz"),
                    marker(0.306f, "M4 145.206 MHz"), marker(0.308f, "M5 145.208 MHz")});
    Frame crowd = render(ui, kW, kH);
    for (int row = 0; row < 3; ++row) {
        const int top = 216 - 20 * row;
        CHECK(crowd.near(335, top, kChipLine) && crowd.near(300, top + 2, kChipBg));
    }
    CHECK(chipPixels(crowd, 100, 100, 600, 175) == 0);                                    // no 4th or 5th tab above the third row
    CHECK(crowd.countNear(332, 0, 349, 150, kMarkerLine) > 300);                          // yet all five lines are drawn
    // markers outside the view are not drawn at all; a lone marker lands on row 0 even when it is the 3rd in the list
    setMarkers(ui, {marker(-0.10f, "M1 144.000 MHz"), marker(1.20f, "M2 147.000 MHz"), marker(0.50f, "M3 145.500 MHz")});
    Frame sparse = render(ui, kW, kH);
    const Span ts = chipSpan(sparse, 218, kPicX, kW);
    CHECK(ts.first > 0 && std::abs((ts.first + ts.last) / 2 - 534) <= 3);                 // 44 + 0.5 * 980
    CHECK(chipPixels(sparse, kPicX, 120, kW, 214) == 0);
    CHECK(sparse.countInk(kPicX, 0, kPicX + 4, 214, kBlue) == 0 && sparse.countInk(kW - 4, 100, kW, 214, kBlue) == 0);
    setMarkers(ui, {marker(0.306f, "M1 145.212 MHz"), marker(0.312f, "M2 145.230 MHz"), marker(0.80f, "M3 146.100 MHz")});

    // ---- plugin marks: a point in the plugin's own colour (alpha floored at 0x60), a span as a tinted band with an edge line each side ----
    {
        PluginMark point;
        point.pos = 0.20f; point.width = 0.0f; point.label = "APRS"; point.color = slint::Color::from_argb_uint8(0x20, 0x20, 0xff, 0x20); point.dashed = false;
        PluginMark span;
        span.pos = 0.55f; span.width = 0.10f; span.label = ""; span.color = slint::Color::from_argb_uint8(0xff, 0xff, 0x50, 0x50); span.dashed = false;
        PluginMark clipped;                                                                   // starts left of the picture: only the part in view is drawn
        clipped.pos = -0.05f; clipped.width = 0.10f; clipped.label = ""; clipped.color = slint::Color::from_argb_uint8(0, 0, 0, 0); clipped.dashed = false;
        PluginMark outside;
        outside.pos = 1.30f; outside.width = 0.0f; outside.label = ""; outside.color = slint::Color::from_argb_uint8(0xff, 0xff, 0x50, 0x50); outside.dashed = false;
        ui->set_marks(std::make_shared<slint::VectorModel<PluginMark>>(std::vector<PluginMark>{point, span, clipped, outside}));
    }
    Frame pm = render(ui, kW, kH);
    const Rgb flooredGreen = over(rgb(0x20ff20), 96.0 / 255.0, kBlue);                    // 0x20 of alpha is raised to 0x60
    const int pointX = bestColumn(pm, 236, 245, 0, 200, flooredGreen);
    CHECK(pointX >= 239 && pointX <= 241 && isSolid(pm, pointX, 0, 200, flooredGreen));    // 44 + 0.20 * 980 = 240, solid
    CHECK(pm.near(583, 100, rgb(0xff5050)) && pm.near(680, 100, rgb(0xff5050)));          // the span: an edge line each side (44 + 539, and 98 px on)
    CHECK(pm.near(582, 100, kBlue) && pm.near(681, 100, kBlue));
    CHECK(pm.near(630, 100, over(rgb(0xff5050), 0.14, kBlue), 6) && pm.near(584, 20, over(rgb(0xff5050), 0.14, kBlue), 6));   // between: the 14 % tint
    const Rgb themeTint = over(rgb(0xd4b060), 0x22 / 255.0, kBlue);                       // Theme.mark-fill over the picture
    CHECK(pm.near(60, 100, themeTint, 6));                                                // the clipped span: the theme's tint, from the picture's edge...
    const Rgb edgeOverTint = over(rgb(0xd4b060), 0.6, themeTint);                         // ...with the default mark's line (60 %) over it at each end
    CHECK(pm.near(44, 100, edgeOverTint) && pm.near(92, 100, edgeOverTint) && pm.near(93, 100, kBlue) && pm.near(94, 100, kBlue));   // ...to 0.05 of the picture
    CHECK(pm.countNear(690, 0, kW, kPicH, rgb(0xff5050), 6) == 0);                       // the mark right of the picture (pos 1.3) is not drawn
    CHECK(pm.countNear(138, 0, 147, kPicH, kPluginLine, 10) == 0);                // ...and the dashed default mark is gone with the old model
    {
        std::vector<PluginMark> back;
        PluginMark dashedMark;
        dashedMark.pos = 0.10f; dashedMark.width = 0.0f; dashedMark.label = ""; dashedMark.color = slint::Color::from_argb_uint8(0, 0, 0, 0); dashedMark.dashed = true;
        back.push_back(dashedMark);
        ui->set_marks(std::make_shared<slint::VectorModel<PluginMark>>(back));
    }
    render(ui, kW, kH);

    // =============================== the states the sheet does not show at rest ===============================
    // ---- the key hides when it would eat the picture (box wider than 46 % of it) and with show-key false ----
    ui->set_show_key(false);
    Frame nokey = render(ui, kW, kH);
    CHECK(nokey.near(kx + kw / 2, ky + kh / 2, kBlue) && ui->get_key_w() == 0.0f);
    ui->set_show_key(true);
    ui->set_view_w(400.0f);                                                               // a view 400 px wide: the 202 px plate is over 46 % of the picture
    Frame narrow = render(ui, kW, kH);
    CHECK(ui->get_key_w() == 0.0f);
    CHECK(narrow.near(400 - 14 - 100, 40, kWellDeep) || narrow.near(400 - 14 - 100, 40, kBlue));
    ui->set_view_w(1024.0f);
    Frame again = render(ui, kW, kH);
    CHECK(int(std::lround(ui->get_key_w())) == kw);

    // ---- the range-changed rail: a hairline in the accent across the picture, its plate under it at the right; hidden behind the key ----
    ui->set_rail_pos(150.0f / 238.0f);
    Frame rail = render(ui, kW, kH);
    CHECK(rail.rowNear(150, kPicX, 470, kAccentSoft) && rail.rowNear(150, 480, kW, kAccentSoft));   // across the picture (the VFO line crosses it)
    CHECK(rail.near(kPicX, 150, kAccentSoft) && rail.near(kW - 1, 150, kAccentSoft));    // all the way to both edges
    CHECK(rail.near(kPicX - 1, 150, kWellDeep) && rail.near(500, 149, kBlue) && rail.near(500, 151, kBlue));   // 1 px, and not into the gutter
    CHECK(rail.countNear(kW - 100, 152, kW - 1, 164, kPlateOverBlue) > 150);              // the label's plate, under the rail, at the right
    CHECK(rail.countInk(kW - 85, 153, kW - 10, 163, kPlateOverBlue, 40) > 20);            // "RANGE CHANGED" on it
    ui->set_rail_pos(40.0f / 238.0f);                                                     // the rail would run under the key plate: not drawn
    Frame railHidden = render(ui, kW, kH);
    CHECK(railHidden.countNear(kPicX, 40, 780, 41, kAccentSoft) == 0);
    ui->set_show_key(false);
    Frame railFree = render(ui, kW, kH);                                                  // with no key there is nothing to clear
    CHECK(railFree.rowNear(40, kPicX, 470, kAccentSoft));
    ui->set_show_key(true);
    ui->set_rail_pos(-1);

    // ---- the reception-gap break: a rail across the strip with an age either side ----
    ui->set_gap_pos(105.0f / 238.0f);
    Frame gap = render(ui, kW, kH);
    CHECK(gap.rowNear(105, 0, kPicX, kKnobRing));                                         // the rail spans the strip...
    CHECK(gap.near(kPicX + 2, 105, kKnobRing) && gap.near(kPicX + 4, 105, kKnobRing));    // ...and overhangs the picture by 5 px
    CHECK(gap.near(kPicX + 8, 105, kBlue) && gap.near(20, 104, kWellDeep) && gap.near(20, 106, kWellDeep));
    CHECK(gap.countInk(8, 91, 44, 104, kWellDeep) > 8);                                   // the age of the row above the break
    CHECK(gap.countInk(8, 107, 44, 121, kWellDeep) > 10);                                 // ...and of the row below it
    CHECK(gap.countInk(8, 0, 44, 3, kWellDeep) == 0);
    ui->set_gap_pos(-1);

    // ---- the cursor: a 1 px line at the pointer with the readout chip 6 px to its right ----
    ui->set_cursor_visible(true);
    moveTo(win, 600, 100);
    Frame cur = render(ui, kW, kH);
    const int curX = bestColumn(cur, 596, 605, 2, 70, kCursorLine);
    CHECK(curX >= 599 && curX <= 601);
    CHECK(curX > 0 && isSolid(cur, curX, 0, kPicH - 30, kCursorLine, 12));
    CHECK(cur.countNear(curX + 6, 74, curX + 130, 92, kPlateOverBlue, 10) > 200);         // the chip's ground (cursor-chip-bg = the well at 0xdd)
    CHECK(cur.countNear(curX + 6, 74, curX + 130, 76, kKnobRing, 8) > 20);                // its border
    CHECK(cur.countInk(curX + 12, 76, curX + 110, 92, kPlateOverBlue, 40) > 25);          // the readout's ink
    CHECK(cur.near(curX + 3, 83, kBlue));                                                 // the 6 px gap between line and chip
    const int chipLeft = cur.firstColNear(83, curX + 2, curX + 40, kKnobRing, 16);        // the chip's left border, at its mid-height
    CHECK(chipLeft == curX + 6);
    moveTo(win, 20, 100);                                                                 // off the picture: no line of its own...
    Frame curOff = render(ui, kW, kH);
    CHECK(curOff.countNear(500, 2, 700, 200, kCursorLine, 12) == 0);
    ui->set_cursor_pos(0.25f);                                                            // ...unless the host mirrors the other plot's
    Frame curMirror = render(ui, kW, kH);
    const int mirX = bestColumn(curMirror, 285, 294, 100, 190, kCursorLine);
    CHECK(mirX >= 288 && mirX <= 290);                                                    // 44 + 0.25 * 980
    ui->set_cursor_pos(-1);
    ui->set_cursor_visible(false);
    moveTo(win, 600, 100);
    Frame curHidden = render(ui, kW, kH);
    CHECK(curHidden.countNear(590, 2, 610, 200, kCursorLine, 12) == 0);
    moveTo(win, 20, 100);

    // the sheet with every state at once, for a person to look at
    ui->set_key_manual(true);
    ui->set_key_coverage("TOP 128 LINES");
    ui->set_rail_pos(150.0f / 238.0f);
    ui->set_gap_pos(105.0f / 238.0f);
    ui->set_cursor_visible(true);
    ui->set_cursor_pos(0.62f);
    Frame all = render(ui, kW, kH);
    all.savePng(shotDir() + "waterfall-specimen-states.png");
    ui->set_key_manual(false);
    ui->set_key_coverage("");
    ui->set_rail_pos(-1);
    ui->set_gap_pos(-1);
    ui->set_cursor_visible(false);
    ui->set_cursor_pos(-1);
    render(ui, kW, kH);

    // =============================== the gestures ===============================
    // hover: the fractions of the picture, (-1, -1) when the pointer leaves it
    moveTo(win, 600, 100);
    CHECK_NEAR(ui->get_hover_x(), (600.0 - kPicX) / kPicW, 0.0005);
    CHECK_NEAR(ui->get_hover_y(), 100.0 / kPicH, 0.0005);
    moveTo(win, 800, 200);
    CHECK_NEAR(ui->get_hover_x(), (800.0 - kPicX) / kPicW, 0.0005);
    CHECK_NEAR(ui->get_hover_y(), 200.0 / kPicH, 0.0005);
    moveTo(win, 20, 100);                                                                 // into the age strip
    CHECK(ui->get_hover_x() == -1.0f && ui->get_hover_y() == -1.0f);
    moveTo(win, 600, 100);
    moveTo(win, 600, 250);                                                                // into the foot band
    CHECK(ui->get_hover_x() == -1.0f && ui->get_hover_y() == -1.0f);
    CHECK(ui->get_marker_hover() == -1);

    // a quick click: one `clicked`, at the pointer's fraction, no pan
    settle(milliseconds(520));
    click(win, 600, 100);
    CHECK(ui->get_clicks() == 1 && ui->get_pans() == 0 && ui->get_dclicks() == 0);
    CHECK_NEAR(ui->get_click_x(), (600.0 - kPicX) / kPicW, 0.0005);
    CHECK(!ui->get_click_shift());

    // a press held 0.45 s is not a click (the long press is the marker menu on touch)
    settle(milliseconds(520));
    moveTo(win, 600, 100);
    press(win, 600, 100);
    settle(milliseconds(600));
    release(win, 600, 100);
    CHECK(ui->get_clicks() == 1 && ui->get_pans() == 0);
    // ...but one released at 0.3 s is
    settle(milliseconds(520));
    press(win, 640, 100);
    settle(milliseconds(300));
    release(win, 640, 100);
    CHECK(ui->get_clicks() == 2);
    CHECK_NEAR(ui->get_click_x(), (640.0 - kPicX) / kPicW, 0.0005);

    // a press that travels past 4 px is a pan, not a click: panned(step) per move, the step the pointer's own
    settle(milliseconds(520));
    moveTo(win, 600, 100);
    press(win, 600, 100);
    moveTo(win, 603, 100);                                                                // 3 px: still a click in the making
    CHECK(ui->get_pans() == 0);
    moveTo(win, 612, 100);
    CHECK(ui->get_pans() >= 1 && ui->get_pan_dx() > 0.0f);
    CHECK_NEAR(ui->get_pan_dx(), 9.0 / kPicW, 0.0005);                                    // this move's step: 603 -> 612
    const int pansAfterFirst = ui->get_pans();
    moveTo(win, 590, 100);
    CHECK(ui->get_pans() == pansAfterFirst + 1);
    CHECK_NEAR(ui->get_pan_dx(), -22.0 / kPicW, 0.0005);                                  // leftwards is negative
    release(win, 590, 100);
    CHECK(ui->get_clicks() == 2);                                                         // no tune on release
    // vertical travel alone does not pan (the waterfall pans horizontally): it is still a click
    settle(milliseconds(520));
    const int pansBefore = ui->get_pans();
    press(win, 600, 100);
    moveTo(win, 601, 140);
    release(win, 601, 140);
    CHECK(ui->get_pans() == pansBefore && ui->get_clicks() == 3);
    // a press that moves 3 px is a click
    settle(milliseconds(520));
    press(win, 700, 100);
    moveTo(win, 703, 100);
    release(win, 703, 100);
    CHECK(ui->get_clicks() == 4 && ui->get_pans() == pansBefore);

    // Shift + click: the tune is off the raster
    settle(milliseconds(520));
    keyDown(win, slint::platform::key_codes::Shift);
    click(win, 600, 100);
    keyUp(win, slint::platform::key_codes::Shift);
    CHECK(ui->get_clicks() == 5 && ui->get_click_shift());

    // double-click: the second press is neither a tune nor a pan, and one `double-clicked`
    settle(milliseconds(520));
    const int clicksBefore = ui->get_clicks();
    click(win, 700, 100);
    click(win, 700, 100);
    CHECK(ui->get_dclicks() == 1);
    CHECK(ui->get_clicks() == clicksBefore + 1);                                          // the first press already tuned
    CHECK(ui->get_pans() == pansBefore);
    // ...also with a pause between the two clicks, so long as it is under the double-click time (500 ms)
    settle(milliseconds(520));
    click(win, 700, 100);
    settle(milliseconds(250));
    click(win, 700, 100);
    CHECK(ui->get_dclicks() == 2 && ui->get_clicks() == clicksBefore + 2);
    // two slow clicks (past the double-click time) are two clicks
    settle(milliseconds(520));
    click(win, 700, 100);
    settle(milliseconds(650));
    click(win, 700, 100);
    CHECK(ui->get_dclicks() == 2 && ui->get_clicks() == clicksBefore + 4);
    // two clicks far apart are not a double-click either
    settle(milliseconds(520));
    click(win, 300, 100);
    click(win, 800, 100);
    CHECK(ui->get_dclicks() == 2 && ui->get_clicks() == clicksBefore + 6);

    // right-click: the marker menu for the frequency under the pointer, with the pointer's place in the view
    settle(milliseconds(520));
    press(win, 700, 120, slint::PointerEventButton::Right);
    CHECK(ui->get_rclicks() == 0);                                                        // nothing at the press: the menu the host opens must not be handed it
    release(win, 700, 120, slint::PointerEventButton::Right);
    CHECK(ui->get_rclicks() == 1);
    CHECK_NEAR(ui->get_rclick_x(), (700.0 - kPicX) / kPicW, 0.0005);
    CHECK_NEAR(ui->get_rclick_px(), 700.0, 0.5);
    CHECK_NEAR(ui->get_rclick_py(), 120.0, 0.5);
    CHECK(ui->get_clicks() == clicksBefore + 6);                                          // the right button tunes nothing
    // ...but not while the left button is panning
    press(win, 600, 100);
    moveTo(win, 620, 100);
    click(win, 620, 100, slint::PointerEventButton::Right);
    CHECK(ui->get_rclicks() == 1);
    release(win, 620, 100);
    CHECK(ui->get_clicks() == clicksBefore + 6);

    // wheel: one `wheel` per event, round(delta-y / 120) notches and at least one, a wheel turned UP positive (as on the spectrum);
    // Shift is reported
    settle(milliseconds(520));
    moveTo(win, 600, 100);
    scroll(win, 600, 100, 0, 120);
    CHECK(ui->get_wheels() == 1 && ui->get_wheel_notches() == 1);
    CHECK_NEAR(ui->get_wheel_x(), (600.0 - kPicX) / kPicW, 0.0005);
    CHECK(!ui->get_wheel_shift());
    scroll(win, 600, 100, 0, -120);
    CHECK(ui->get_wheels() == 2 && ui->get_wheel_notches() == 0);                         // +1, then -1
    scroll(win, 600, 100, 0, 240);                                                        // a fast wheel: two notches in one event
    CHECK(ui->get_wheels() == 3 && ui->get_wheel_notches() == 2);
    scroll(win, 600, 100, 0, -60);                                                        // a 60 px click (the winit backend's) is one notch too
    CHECK(ui->get_wheels() == 4 && ui->get_wheel_notches() == 1);
    scroll(win, 600, 100, 0, 5);                                                          // a touchpad trickle counts one per event
    scroll(win, 600, 100, 0, 5);
    CHECK(ui->get_wheels() == 6 && ui->get_wheel_notches() == 3);
    scroll(win, 600, 100, 0, -5);
    CHECK(ui->get_wheels() == 7 && ui->get_wheel_notches() == 2);
    scroll(win, 600, 100, 0, 0);                                                          // no delta: not a wheel event
    scroll(win, 600, 100, 30, 0);                                                         // sideways: not a zoom
    CHECK(ui->get_wheels() == 7);
    keyDown(win, slint::platform::key_codes::Shift);
    scroll(win, 600, 100, 0, 120);
    keyUp(win, slint::platform::key_codes::Shift);
    CHECK(ui->get_wheels() == 8 && ui->get_wheel_notches() == 3 && ui->get_wheel_shift());
    // the Shift slide is not for a hand that is panning; a plain wheel still zooms
    press(win, 600, 100);
    moveTo(win, 620, 100);
    keyDown(win, slint::platform::key_codes::Shift);
    scroll(win, 620, 100, 0, 120);
    keyUp(win, slint::platform::key_codes::Shift);
    CHECK(ui->get_wheels() == 8);
    scroll(win, 620, 100, 0, 120);
    CHECK(ui->get_wheels() == 9);
    release(win, 620, 100);
    // the wheel is the picture's: not the age strip's nor the foot's
    scroll(win, 20, 100, 0, 120);
    scroll(win, 600, 255, 0, 120);
    CHECK(ui->get_wheels() == 9);

    // the marker under the pointer: within 4 px of its line, or over its tab; -1 elsewhere
    moveTo(win, 700, 100);
    CHECK(ui->get_marker_hover() == -1);
    moveTo(win, 341, 100);                                                                // M1's line is at 343.9: 3 px away
    CHECK(ui->get_marker_hover() == 0);
    moveTo(win, 700, 100);
    CHECK(ui->get_marker_hover() == -1);
    moveTo(win, 340, 100);                                                                // 3.9 px from M1's line: still on it
    CHECK(ui->get_marker_hover() == 0);
    moveTo(win, 700, 100);
    moveTo(win, 339, 100);                                                                // 4.9 px: off it
    CHECK(ui->get_marker_hover() == -1);
    moveTo(win, 352, 100);                                                                // 2 px from M2's line (349.8), 8 px from M1's
    CHECK(ui->get_marker_hover() == 1);
    // where two zones overlap the NEAREST line wins, whichever way the pointer came
    moveTo(win, 346, 100);                                                                // 2.1 px from M1's line, 3.8 px from M2's
    CHECK(ui->get_marker_hover() == 0);
    moveTo(win, 347, 100);                                                                // 3.1 px from M1's, 2.8 px from M2's
    CHECK(ui->get_marker_hover() == 1);
    moveTo(win, 346, 101);                                                                // and back, one pixel down: M1 again, not "none"
    CHECK(ui->get_marker_hover() == 0);
    moveTo(win, 700, 100);
    moveTo(win, 330, 205);                                                                // over M2's tab (row 1), away from every line
    CHECK(ui->get_marker_hover() == 1);
    moveTo(win, 330, 225);                                                                // over M1's tab (row 0)
    CHECK(ui->get_marker_hover() == 0);
    moveTo(win, 828, 225);                                                                // over M3's tab
    CHECK(ui->get_marker_hover() == 2);
    moveTo(win, 828, 100);                                                                // on M3's line
    CHECK(ui->get_marker_hover() == 2);
    moveTo(win, 20, 100);                                                                 // off the picture
    CHECK(ui->get_marker_hover() == -1);
    moveTo(win, 300, 180);                                                                // above the tabs, between lines
    CHECK(ui->get_marker_hover() == -1);

    // the tip: the gesture help, drawn inline 22 px under the point where the pointer came to rest, 600 ms after it did; over a
    // marker, that marker's words. (Drawn inline: while a PopupWindow tip was open Slint handed it the pointer's events.)
    moveTo(win, 600, 100);
    settle(milliseconds(300));
    Frame tipWait = render(ui, kW, kH);
    CHECK(tipWait.countNear(600, 122, 760, 144, kMenu, 4) == 0);                          // not yet
    settle(milliseconds(600));
    Frame tip = render(ui, kW, kH);
    CHECK(tip.countNear(600, 122, 760, 144, kMenu, 4) > 400);                             // TooltipFace
    CHECK(tip.countNear(600, 122 - 8, 760, 122 - 1, kMenu, 4) == 0 && tip.countNear(600, 150, 760, 175, kMenu, 4) == 0);   // just that face
    const int gestureTipW = tipWidth(tip, 596, 125);                                      // "Click to tune | wheel: zoom"
    CHECK(gestureTipW >= 120 && gestureTipW <= 260);
    CHECK(ui->get_hover_x() > 0.0f);                                                      // the view never saw the pointer leave
    // it stays while the pointer moves less than 6 px, goes when it moves further, and comes back 600 ms after the new rest
    moveTo(win, 603, 102);
    CHECK(render(ui, kW, kH).countNear(600, 122, 760, 144, kMenu, 4) > 400);
    moveTo(win, 620, 100);
    CHECK(render(ui, kW, kH).countNear(620, 122, 780, 144, kMenu, 4) == 0);
    CHECK_NEAR(ui->get_hover_x(), (620.0 - kPicX) / kPicW, 0.0005);                       // ...and the first move after it was seen
    settle(milliseconds(900));
    CHECK(render(ui, kW, kH).countNear(620, 122, 780, 144, kMenu, 4) > 400);
    // a click with the tip up is a click (it was lost while the tip was a popup), and a button going down hides it
    const int clicksTip = ui->get_clicks();
    press(win, 620, 100);
    CHECK(render(ui, kW, kH).countNear(620, 122, 780, 144, kMenu, 4) == 0);
    release(win, 620, 100);
    CHECK(ui->get_clicks() == clicksTip + 1);
    // ...and with a button held there is no tip however long the pointer rests (the press is no click either: it was held 0.75 s)
    settle(milliseconds(520));
    press(win, 640, 100);
    settle(milliseconds(900));
    CHECK(render(ui, kW, kH).countNear(640, 122, 800, 144, kMenu, 4) == 0);
    release(win, 640, 100);
    CHECK(ui->get_clicks() == clicksTip + 1);
    // a wheel with the tip up is a wheel
    settle(milliseconds(520));
    moveTo(win, 600, 100);
    settle(milliseconds(900));
    CHECK(render(ui, kW, kH).countNear(600, 122, 760, 144, kMenu, 4) > 400);
    const int wheelsTip = ui->get_wheels();
    scroll(win, 600, 100, 0, 120);
    CHECK(ui->get_wheels() == wheelsTip + 1);
    CHECK(render(ui, kW, kH).countNear(600, 122, 760, 144, kMenu, 4) > 400);             // the wheel does not hide it...
    moveTo(win, 20, 100);                                                                 // ...the pointer leaving the picture does
    CHECK(render(ui, kW, kH).countNear(600, 122, 760, 144, kMenu, 4) == 0);
    // onto a marker: its own words (longer than the gesture help, so a wider face), 600 ms after the pointer came onto it
    moveTo(win, 345, 100);
    CHECK(ui->get_marker_hover() == 0);
    settle(milliseconds(300));
    CHECK(render(ui, kW, kH).countNear(345, 122, 560, 144, kMenu, 4) == 0);
    settle(milliseconds(600));
    Frame tipM = render(ui, kW, kH);
    CHECK(tipM.countNear(345, 122, 560, 144, kMenu, 4) > 400);
    CHECK(tipWidth(tipM, 341, 125) > gestureTipW + 60);
    // off the picture: gone
    moveTo(win, 20, 100);
    CHECK(render(ui, kW, kH).countNear(300, 122, 760, 144, kMenu, 4) == 0);

    // a disabled view acts on nothing
    ui->set_view_enabled(false);
    settle(milliseconds(520));
    const int clicksNow = ui->get_clicks(), wheelsNow = ui->get_wheels(), rNow = ui->get_rclicks(), panNow = ui->get_pans();
    ui->set_hover_x(-2.0f);
    click(win, 600, 100);
    click(win, 600, 100, slint::PointerEventButton::Right);
    scroll(win, 600, 100, 0, -120);
    press(win, 600, 100);
    moveTo(win, 640, 100);
    release(win, 640, 100);
    CHECK(ui->get_clicks() == clicksNow && ui->get_wheels() == wheelsNow && ui->get_rclicks() == rNow && ui->get_pans() == panNow);
    CHECK(ui->get_hover_x() == -2.0f && ui->get_marker_hover() == -1);
    moveTo(win, 600, 100);
    settle(milliseconds(900));
    CHECK(render(ui, kW, kH).countNear(600, 122, 760, 144, kMenu, 4) == 0);               // and no tip
    ui->set_view_enabled(true);

    return testSummary("test_slint_waterfall");
}
