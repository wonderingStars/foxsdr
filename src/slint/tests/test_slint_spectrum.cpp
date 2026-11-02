// The spectrum family: SpectrumView reaches the pixels at the artboard geometry and colours (the SPECTRUM div of
// Workbench.dc.html, the plugin marks of Workbench-PluginKit.dc.html), the trace image fills the plot exactly,
// and every gesture fires the contract callback with the contract arguments.
// Text is in a fallback typeface on this machine: it is asserted as INK in a region (and its colour as the
// strongest blend of the token over the ground), never as glyph shapes or widths.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "spectrum_specimen.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace foxsdr::slinttest;
using std::chrono::milliseconds;

namespace {

// ---- the sheet colours: the artboard own hexes, not the theme names ----------------------------------------
const Rgb kWell = rgb(0x0d0f12);        // #0d0f12, the spectrum ground
const Rgb kGrid = rgb(0x1e2229);        // the graticule
const Rgb kBandplan = rgb(0x2a3b5a);    // the ribbon
const Rgb kBandplanFg = rgb(0x8fb3e0);  // its names
const Rgb kAccent = rgb(0xf5a524);      // the VFO edges, the bookmark ticks
const Rgb kMark = rgb(0xd4b060);        // plugin marks
const Rgb kFg = rgb(0xe6e8ec);          // user markers, the cursor line
const Rgb kFgSoft = rgb(0xc9ced6);      // the caption, chip text
const Rgb kDim = rgb(0x7d8694);         // caveat, axis labels, the age
const Rgb kMuted = rgb(0x9aa3ad);       // the peak caption
const Rgb kAccentSoft = rgb(0xf5c66b);  // the peak figure, bookmark names
const Rgb kChipBg = rgb(0x262a32);      // the SPAN chip
const Rgb kChipLine = rgb(0x363b45);
const Rgb kCursorChipLine = rgb(0x434955);
const Rgb kPeakHold = rgb(0xff9f43);    // the PEAK HOLD label
const Rgb kAverage = rgb(0xf1ead6);     // the AVERAGE label
const Rgb kPhosphor = rgb(0x7ad7ff);    // the live trace: airband channels
const Rgb kGreen = rgb(0x00ff00);       // the test trace

constexpr int kW = 1024, kH = 424;                         // the sheet
constexpr int kPlotX = 44, kPlotY = 76, kPlotW = 980, kPlotH = 330;
constexpr int kPlotBottom = kPlotY + kPlotH;               // 406
constexpr double kVfoFillAlpha = 34.0 / 255.0;             // #f5a52422, #d4b06022
constexpr double kCursorAlpha = 102.0 / 255.0;             // #e6e8ec66
constexpr double kChipAlpha = 221.0 / 255.0;               // #0d0f12dd

// a position given as a plot fraction, as the page x / y it lands on (the component snaps to whole pixels)
int pageX(double pos) { return kPlotX + static_cast<int>(std::lround(kPlotW * pos)); }

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        const long long checkEqA = static_cast<long long>(a), checkEqB = static_cast<long long>(b); \
        CHECK(checkEqA == checkEqB);                                                                \
        if (checkEqA != checkEqB) { std::printf("  -> %s is %lld, expected %lld\n", #a, checkEqA, checkEqB); } \
    } while (0)

#define CHECK_CLOSE(a, b, tol)                                                                      \
    do {                                                                                            \
        const double checkCloseA = static_cast<double>(a), checkCloseB = static_cast<double>(b);    \
        CHECK(std::fabs(checkCloseA - checkCloseB) <= (tol));                                       \
        if (!(std::fabs(checkCloseA - checkCloseB) <= (tol))) { std::printf("  -> %s is %.5f, expected %.5f +- %g\n", #a, checkCloseA, checkCloseB, static_cast<double>(tol)); } \
    } while (0)

// pixels of a column that are within tol of c
int inCol(const Frame& f, int x, int y0, int y1, Rgb c, int tol = 8) { return f.countNear(x, y0, x + 1, y1, c, tol); }

// How many separate runs of colour c the column holds: a dashed line is many, a solid one is one.
int runsDown(const Frame& f, int x, int y0, int y1, Rgb c, int tol = 8) {
    int runs = 0;
    bool previous = false;
    for (int y = y0; y < y1; ++y) {
        const bool on = f.near(x, y, c, tol);
        if (on && !previous) { ++runs; }
        previous = on;
    }
    return runs;
}

// A pixel of colour c in the columns x-spread .. x+spread at row y (a 1 px line may land one column off).
bool nearAround(const Frame& f, int x, int y, Rgb c, int tol = 8, int spread = 1) {
    for (int dx = -spread; dx <= spread; ++dx) { if (f.near(x + dx, y, c, tol)) { return true; } }
    return false;
}

// How strongly ink of colour `fg` shows in the rectangle over `bg`: the largest coverage t (0..1) among the pixels
// that lie on the line from bg to fg (an antialiased glyph edge is a blend of the two); 0 when none is that colour.
double inkCoverage(const Frame& f, int x0, int y0, int x1, int y1, Rgb bg, Rgb fg, double maxResidual = 16.0) {
    const double dr = fg.r - bg.r, dg = fg.g - bg.g, db = fg.b - bg.b;
    const double len2 = dr * dr + dg * dg + db * db;
    double best = 0.0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Rgb p = f.at(x, y);
            const double t = ((p.r - bg.r) * dr + (p.g - bg.g) * dg + (p.b - bg.b) * db) / len2;
            if (t <= 0.0) { continue; }
            const double c = std::min(t, 1.0);
            const double er = p.r - (bg.r + c * dr), eg = p.g - (bg.g + c * dg), eb = p.b - (bg.b + c * db);
            if (std::sqrt(er * er + eg * eg + eb * eb) <= maxResidual) { best = std::max(best, c); }
        }
    }
    return best;
}

// the brightest pixel (mean of r, g, b) of a rectangle: the strongest ink a run of text laid down
int peakLuma(const Frame& f, int x0, int y0, int x1, int y1) {
    int best = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Rgb p = f.at(x, y);
            best = std::max(best, (p.r + p.g + p.b) / 3);
        }
    }
    return best;
}

std::string text(const slint::SharedString& s) { return std::string(std::string_view(s)); }

// the clock the double click and the rest tip run on is the wall clock: let it pass
void wait(int ms) { settle(milliseconds(ms)); }

// Lets time pass until `done()` holds, at most `timeoutMs`; true when it did. A busy machine only makes it slower.
template <class Done>
bool waitUntil(Done done, int timeoutMs) {
    const auto until = std::chrono::steady_clock::now() + milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < until) {
        settle(milliseconds(20));
        if (done()) { return true; }
    }
    return done();
}

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = SpectrumSpecimen::create();
    slint::Window& win = ui->window();

    // The trace the host would paint: transparent, but for one row of pure green at row 100 of the 980 x 330 plot.
    slint::SharedPixelBuffer<slint::Rgba8Pixel> traceBuffer(kPlotW, kPlotH);
    for (slint::Rgba8Pixel* p = traceBuffer.begin(); p != traceBuffer.end(); ++p) { *p = slint::Rgba8Pixel{0, 0, 0, 0}; }
    for (int x = 0; x < kPlotW; ++x) { traceBuffer.begin()[100 * kPlotW + x] = slint::Rgba8Pixel{0, 255, 0, 255}; }
    ui->set_trace(slint::Image(traceBuffer));

    // ======================================================================================================
    // THE SHEET: the SPECTRUM div at 1024 x 424
    Frame f = render(ui, kW, kH);
    CHECK_EQ(f.width, kW);
    CHECK_EQ(f.height, kH);
    CHECK(f.near(3, 180, kWell));           // the gutter between two ladder labels: the well
    CHECK(f.near(620, 300, kWell));         // an empty spot of the plot: the well

    // --- graticule: a hairline at every frequency tick (x = 44 + 98k) and every dB tick (y = 76 + 66k) -----
    for (int k = 1; k <= 9; ++k) {
        const int x = kPlotX + 98 * k;
        if (k == 1) {
            // the plugin mark sits on this column (dashed 4 on, 3 off): the gaps show the graticule
            CHECK(inCol(f, x, kPlotY + 4, kPlotBottom - 4, kGrid) >= 100);
        } else if (k == 3) {
            // the span mark left edge sits on this column (solid): the mark colour, not the graticule
            CHECK(inCol(f, x, kPlotY + 4, kPlotBottom - 8, kMark) >= 300);
        } else if (k == 8) {
            // the favourite bookmark guide (a 35 % hairline) sits on this column
            CHECK(inCol(f, x, kPlotY + 4, kPlotBottom - 10, over(kAccent, 0.35, kGrid), 10) >= 300);
        } else {
            CHECK(inCol(f, x, kPlotY + 4, kPlotBottom - 4, kGrid) >= 290);
        }
    }
    CHECK(f.near(kPlotX, 300, kGrid));                    // the tick at 0: the plot left edge
    CHECK(f.near(kPlotX - 1, 300, kWell));                // and nothing in the gutter
    for (int k = 0; k <= 4; ++k) {
        CHECK(f.rowNear(kPlotY + 66 * k, 600, 650, kGrid));     // 0, -20, -40, -60, -80 dB
        CHECK(f.near(620, kPlotY + 66 * k + 3, kWell));         // the line is one pixel thick
    }
    CHECK(f.near(620, kPlotBottom, kWell));               // the tick at 1.0 is the plot foot: clipped, as on the sheet

    // --- dB ladder: 10 px mono, right-aligned in the 44 px gutter, centred on each tick --------------------
    for (int k = 0; k <= 5; ++k) {
        const int ty = kPlotY + 66 * k;
        CHECK(f.countInk(0, ty - 7, kPlotX, ty + 7, kWell) >= 6);                  // a label is centred on the tick
        if (k < 5) { CHECK_EQ(f.countInk(0, ty + 9, kPlotX, ty + 57, kWell), 0); }   // none between two ticks
    }
    CHECK(inkCoverage(f, 0, kPlotY + 128, kPlotX, kPlotY + 138, kWell, kDim) > 0.55);   // axis-fg
    CHECK_EQ(f.countInk(0, kPlotY - 7, 26, kPlotY + 7, kWell), 0);                  // the single 0 hugs the right end,
    CHECK(f.countInk(28, kPlotY - 7, 39, kPlotY + 7, kWell) >= 3);                   // 6 px short of the plot
    CHECK_EQ(f.countInk(kPlotX - 5, 60, kPlotX, 404, kWell), 0);                    // right-aligned, 6 px to spare

    // --- band-plan ribbon: left 44, right 0, top 0, 8 px; its name 10 px under it -----------------------------
    CHECK(f.near(300, 4, kBandplan));
    CHECK(f.near(300, 7, kBandplan));
    CHECK(f.near(300, 8, kWell));                          // 8 px high
    CHECK(f.near(kPlotX, 4, kBandplan));                   // from the plot left edge...
    CHECK(f.near(kPlotX - 1, 4, kWell));
    CHECK(f.near(kW - 1, 4, kBandplan));                   // ...to the right edge
    CHECK(f.countInk(46, 28, 200, 44, kWell) >= 25);       // "2 m amateur": it would sit under the header, so it steps below it
    CHECK(inkCoverage(f, 46, 28, 200, 44, kWell, kBandplanFg) > 0.55);
    CHECK_EQ(f.countInk(kPlotX, 28, 47, 44, kWell), 0);    // 4 px inside its segment
    CHECK_EQ(f.countInk(kPlotX, 8, kW, 13, kWell), 0);     // nothing between the ribbon and the header row

    // --- header at (52, 14): caption, caveat, PEAK HOLD in the trace-peak colour ---------------------------
    CHECK(f.countInk(52, 14, 400, 27, kWell) >= 60);
    CHECK_EQ(f.countInk(kPlotX, 14, 51, 27, kWell), 0);    // starts at x 52
    CHECK(inkCoverage(f, 52, 14, 250, 27, kWell, kFgSoft) > 0.6);          // the caption in fg-soft
    CHECK(peakLuma(f, 380, 14, 470, 27) < peakLuma(f, 52, 14, 250, 27) - 25);   // the caveat is dimmer than the caption
    CHECK(peakLuma(f, 380, 14, 470, 27) > 90);
    CHECK(inkCoverage(f, 440, 14, 760, 28, kWell, kPeakHold) > 0.6);       // PEAK HOLD

    // --- peak block: right 16, top 12, right-aligned: cap, the figure in accent-soft, its age ---------------
    CHECK(f.countInk(860, 12, 1010, 24, kWell) >= 10);
    CHECK(inkCoverage(f, 860, 12, 1010, 24, kWell, kMuted) > 0.5);         // "Peak in passband"
    CHECK(inkCoverage(f, 860, 24, 1010, 46, kWell, kAccentSoft) > 0.75);   // "-41.2 dBFS"
    CHECK_EQ(f.countInk(1010, 12, kW, 46, kWell), 0);                      // 16 px in from the right edge

    // --- SPAN chip: right 16, top 58, 20 px pill in the chip colours ----------------------------------------
    CHECK(f.near(1003, 68, kChipBg));
    CHECK(f.near(kW - 17, 68, kChipLine, 10));             // its right border: the chip ends at 1024 - 16
    CHECK(f.near(990, 58, kChipLine, 10));                 // top border
    CHECK(f.near(990, 77, kChipLine, 10));                 // bottom border: 20 px high
    CHECK(f.near(990, 57, kWell));
    CHECK(f.near(1011, 68, kWell));
    CHECK(inkCoverage(f, 920, 62, 1000, 74, kChipBg, kFgSoft) > 0.55);     // "SPAN 2.400 MHz"

    // --- VFO band: fill between two 1 px edges, a dashed centre line (2 on, 3 off) ------------------------------
    const int vfoL = pageX(0.456122 - 0.015306);           // 476: the sheet rect x 432
    const int vfoR = pageX(0.456122 + 0.015306);           // 506: 432 + 30 + 44
    const int vfoC = (vfoL + vfoR) / 2;                    // 491
    CHECK_EQ(vfoL, 476);
    CHECK_EQ(vfoR, 506);
    const Rgb vfoFill = over(kAccent, kVfoFillAlpha, kWell);
    CHECK(f.near(484, 300, vfoFill, 10));                  // the fill: accent at #22 over the well
    CHECK(f.near(500, 250, vfoFill, 10));
    CHECK(f.near(vfoL, 300, kAccent, 12));                 // the left edge: one accent column
    CHECK(f.near(vfoL - 1, 300, kWell));
    CHECK(f.near(vfoL + 1, 300, vfoFill, 10));
    CHECK(f.near(vfoR, 300, kAccent, 12));                 // the right edge
    CHECK(f.near(vfoR + 1, 300, kWell));
    CHECK(f.near(vfoR - 1, 300, vfoFill, 10));
    CHECK(f.near(470, 300, kWell));
    CHECK(f.near(512, 300, kWell));
    {
        const int on = inCol(f, vfoC, kPlotY, kPlotBottom, kAccent, 12);
        CHECK(on >= kPlotH * 0.2 && on <= kPlotH * 0.8);   // dashed: neither empty nor solid
        CHECK(runsDown(f, vfoC, kPlotY, kPlotBottom, kAccent, 12) >= 50);
        CHECK(f.near(vfoC, kPlotY + 1, kAccent, 12));      // 2 on...
        CHECK(f.near(vfoC, kPlotY + 2, vfoFill, 10));      // ...3 off, showing the fill
        CHECK(f.near(vfoC, kPlotY + 3, vfoFill, 10));
        CHECK(f.near(vfoC, kPlotY + 4, vfoFill, 10));
        CHECK(f.near(vfoC, kPlotY + 5, kAccent, 12));
    }

    // --- plugin marks: a dashed point (4 on, 3 off) with its name at y 60, a span as a tint between edges ------
    const int markX = pageX(0.10);                         // 142: the sheet path M98
    {
        const int on = inCol(f, markX, kPlotY, kPlotBottom, kMark);
        CHECK(on >= kPlotH * 0.2 && on <= kPlotH * 0.8);
        CHECK(runsDown(f, markX, kPlotY, kPlotBottom, kMark) >= 40);
        CHECK(f.near(markX, kPlotY + 1, kMark));           // 4 on...
        CHECK(f.near(markX, kPlotY + 3, kMark));
        CHECK(f.near(markX, kPlotY + 4, kGrid));           // ...3 off (the graticule shows)
        CHECK(f.near(markX, kPlotY + 5, kGrid));
        CHECK(f.near(markX, kPlotY + 6, kGrid));
        CHECK(f.near(markX, kPlotY + 7, kMark));
        CHECK(f.near(markX - 1, 300, kWell) && f.near(markX + 1, 300, kWell));   // one pixel thick
    }
    CHECK(f.countInk(markX, 58, markX + 110, 74, kWell) >= 20);               // APRS 144.800 at the line x, y 60
    CHECK(inkCoverage(f, markX, 58, markX + 110, 74, kWell, kMark) > 0.55);
    CHECK_EQ(f.countInk(markX - 12, 58, markX - 2, 74, kWell), 0);
    const int spanL = pageX(0.30), spanR = pageX(0.35);    // 338, 387
    CHECK_EQ(spanL, 338);
    CHECK_EQ(spanR, 387);
    CHECK(f.near(362, 150, over(kMark, kVfoFillAlpha, kWell), 10));           // the tint
    CHECK(f.near(330, 150, kWell));
    CHECK(f.near(395, 150, kWell));
    CHECK(inCol(f, spanL, kPlotY, kPlotBottom - 8, kMark) >= 320);            // both edges solid, full height
    CHECK(inCol(f, spanR, kPlotY, kPlotBottom - 8, kMark) >= 320);
    CHECK(f.countInk(spanL, 58, spanL + 100, 74, kWell) >= 20);               // Packet band
    CHECK(inkCoverage(f, spanL, 58, spanL + 100, 74, kWell, kMark) > 0.55);

    // --- user marker M1: 1 px, dashed 3 on 4 off, 35 % alpha -----------------------------------------------------
    const int markerX = pageX(0.30612);                    // 344: the sheet path M300 (plot x)
    {
        const Rgb tint = over(kMark, kVfoFillAlpha, kWell);                    // it crosses the span tint
        const Rgb faint = over(kFg, 0.35, tint);
        const int on = inCol(f, markerX, kPlotY, kPlotBottom, faint, 10);
        CHECK(on >= kPlotH * 0.2 && on <= kPlotH * 0.8);
        CHECK(runsDown(f, markerX, kPlotY, kPlotBottom, faint, 10) >= 30);
        CHECK(f.near(markerX, kPlotY + 1, faint, 10));     // 3 on...
        CHECK(f.near(markerX, kPlotY + 2, faint, 10));
        CHECK(f.near(markerX, kPlotY + 3, tint, 10));      // ...4 off
        CHECK(f.near(markerX, kPlotY + 4, tint, 10));
        CHECK(f.near(markerX, kPlotY + 6, tint, 10));
        CHECK(f.near(markerX, kPlotY + 7, faint, 10));
    }

    // --- bookmarks: 2 x 8 ticks at the plot foot, names at y 60 and 8 px left of the tick, a favourite guide ---
    const int gb3 = pageX(0.63673), iss = pageX(0.8);      // 668, 828: the sheet ticks
    CHECK_EQ(gb3, 668);
    CHECK_EQ(iss, 828);
    CHECK_EQ(f.countNear(gb3, kPlotBottom - 8, gb3 + 2, kPlotBottom, kAccent, 10), 16);
    CHECK_EQ(f.countNear(iss, kPlotBottom - 8, iss + 2, kPlotBottom, kAccent, 10), 16);
    CHECK(!f.near(gb3 - 1, 402, kAccent, 30) && !f.near(gb3 + 2, 402, kAccent, 30));   // exactly 2 px wide
    CHECK(f.near(gb3, kPlotBottom - 9, kWell));            // exactly 8 px high
    CHECK(f.near(gb3, 200, kWell));                        // GB3xx is not a favourite: no guide
    CHECK(f.near(iss, 200, over(kAccent, 0.35, kGrid), 10));     // ISS APRS is: a hairline guide up the plot
    CHECK(f.near(iss, kPlotY + 2, over(kAccent, 0.35, kGrid), 12));
    CHECK(f.countInk(656, 58, 720, 74, kWell) >= 15);                              // GB3xx, left edge x 660
    CHECK(inkCoverage(f, 656, 58, 720, 74, kWell, kAccentSoft) > 0.55);
    CHECK_EQ(f.countInk(640, 58, 658, 74, kWell), 0);
    CHECK(f.countInk(816, 58, 880, 74, kWell) >= 20);                              // ISS APRS, left edge x 820
    CHECK(inkCoverage(f, 816, 58, 880, 74, kWell, kAccentSoft) > 0.55);

    // --- cursor: a 1 px line at the pointer, the readout chip 6 px to its right ---------------------------------
    const int cursorX = pageX(0.5265);                     // 560: the sheet left 560
    CHECK_EQ(cursorX, 560);
    const Rgb cursorLine = over(kFg, kCursorAlpha, kWell);
    CHECK(f.near(cursorX, 300, cursorLine, 10));
    CHECK(inCol(f, cursorX, kPlotY + 2, kPlotBottom - 2, cursorLine, 12) >= 300);
    CHECK(f.near(cursorX - 1, 300, kWell) && f.near(cursorX + 1, 300, kWell));
    CHECK(f.near(cursorX, kPlotY - 1, kWell));             // the line starts at the plot top...
    CHECK(f.near(cursorX, kPlotY + 1, cursorLine, 12));
    CHECK(f.near(cursorX, kPlotBottom - 1, cursorLine, 12));  // ...and runs the 330 px
    CHECK(f.rowNear(120, 580, 620, kCursorChipLine, 14));  // the chip top border, at y 120
    CHECK(f.near(600, 119, kWell));
    CHECK(nearAround(f, cursorX + 6, 130, kCursorChipLine, 14, 1));           // its left border, 6 px right of the line
    CHECK(f.near(cursorX + 3, 130, kWell));
    CHECK(f.near(632, 137, over(kWell, kChipAlpha, kGrid), 5));               // its ground (87 %) dims the gridline behind it
    CHECK(f.near(632, 118, kGrid));                        // while above the chip that line is the plain graticule
    CHECK(inkCoverage(f, 575, 123, 700, 138, kWell, kFgSoft) > 0.55);         // 145.212 MHz, -78.4 dB

    // --- frequency axis: 10 px mono centred under each tick; the unit at the right ------------------------------
    for (int k = 0; k <= 9; ++k) {
        const int tx = kPlotX + 98 * k;
        CHECK(f.countInk(tx - 15, 408, tx + 16, kH, kWell) >= 8);
        CHECK(f.countInk(tx + 3, 408, tx + 16, kH, kWell) >= 4);       // centred: ink on both sides of the tick
        CHECK(f.countInk(tx - 16, 408, tx - 3, kH, kWell) >= 4);
        if (k < 9) { CHECK_EQ(f.countInk(tx + 22, 408, tx + 76, kH, kWell), 0); }   // none between two labels
    }
    CHECK(inkCoverage(f, kPlotX + 98 * 4 - 15, 408, kPlotX + 98 * 4 + 16, kH, kWell, kDim) > 0.5);
    CHECK_EQ(f.countInk(50, 406, 1000, 410, kWell), 0);                       // the labels sit in the band under the plot
    CHECK(f.countInk(990, 408, 1022, kH, kWell) >= 6);                         // the unit MHz
    CHECK_EQ(f.countInk(1022, 408, kW, kH, kWell), 0);

    // --- the trace image fills the plot exactly: row 100 of the picture is page row 176 --------------------------
    CHECK(f.near(kPlotX + 500, kPlotY + 100, kGreen, 4));
    CHECK(f.near(kPlotX + 500, kPlotY + 99, kWell));
    CHECK(f.near(kPlotX + 500, kPlotY + 101, kWell));
    CHECK(!f.near(kPlotX + 500, kPlotY + 90, kGreen, 60));
    CHECK(f.near(kPlotX, kPlotY + 100, kGreen, 4));        // the picture starts at the plot left edge
    CHECK(f.near(kW - 1, kPlotY + 100, kGreen, 4));        // and ends at the right one
    CHECK(!f.near(kPlotX - 1, kPlotY + 100, kGreen, 60));  // nothing in the gutter
    CHECK(!f.near(10, kPlotY + 100, kGreen, 60));

    f.savePng(shotDir() + "spectrum-specimen.png");

    // --- the trace fills the plot whatever size the host painted it at: a half-size picture is stretched ----------
    {
        slint::SharedPixelBuffer<slint::Rgba8Pixel> narrow(kPlotW / 2, kPlotH);
        for (slint::Rgba8Pixel* p = narrow.begin(); p != narrow.end(); ++p) { *p = slint::Rgba8Pixel{0, 0, 0, 0}; }
        for (int x = 0; x < kPlotW / 2; ++x) { narrow.begin()[100 * (kPlotW / 2) + x] = slint::Rgba8Pixel{0, 255, 0, 255}; }
        ui->set_trace(slint::Image(narrow));
        Frame h = render(ui, kW, kH);
        CHECK(h.near(kPlotX + 3, kPlotY + 100, kGreen, 60));          // half as wide as the plot, yet it runs the whole width
        CHECK(h.near(kPlotX + 490, kPlotY + 100, kGreen, 60));
        CHECK(h.near(kW - 3, kPlotY + 100, kGreen, 60));
        CHECK(h.near(kW - 3, kPlotY + 90, kWell));
        slint::SharedPixelBuffer<slint::Rgba8Pixel> flat(kPlotW, kPlotH / 2);
        for (slint::Rgba8Pixel* p = flat.begin(); p != flat.end(); ++p) { *p = slint::Rgba8Pixel{0, 0, 0, 0}; }
        for (int x = 0; x < kPlotW; ++x) { flat.begin()[50 * kPlotW + x] = slint::Rgba8Pixel{0, 255, 0, 255}; }
        ui->set_trace(slint::Image(flat));                            // and half as high: row 50 of 165 is row 100 of 330
        Frame g2 = render(ui, kW, kH);
        int bestRow = -1, bestGreen = 0;
        for (int y = kPlotY; y < kPlotBottom; ++y) {
            if (g2.at(544, y).g > bestGreen) { bestGreen = g2.at(544, y).g; bestRow = y; }
        }
        CHECK(bestGreen >= 100);
        CHECK(bestRow >= kPlotY + 99 && bestRow <= kPlotY + 102);
        ui->set_trace(slint::Image(traceBuffer));
    }

    // --- the cursor readout chip moves to the left of the line where it would leave the well --------------------
    {
        ui->set_cursor_pos(0.95f);
        Frame fl = render(ui, kW, kH);
        const int lineX = pageX(0.95);
        CHECK(fl.near(lineX, 300, cursorLine, 12));
        CHECK(nearAround(fl, lineX - 7, 130, kCursorChipLine, 14, 1));          // the chip right border, 6 px left of the line
        CHECK_EQ(fl.countInk(lineX + 2, 118, kW, 142, kWell), 0);               // nothing right of the line at the chip row
        CHECK(fl.countInk(lineX - 100, 123, lineX - 8, 138, kWell) > 40);       // the words are on its left
        ui->set_cursor_pos(0.5265f);
    }

    // ======================================================================================================
    // HOVER: the pointer over the plot reports its position as fractions of the plot; anywhere else, (-1, -1)
    CHECK_EQ(ui->get_hovers(), 0);
    moveTo(win, 600, 200);
    CHECK(ui->get_hovers() >= 1);
    CHECK_CLOSE(ui->get_hover_x(), (600.0 - kPlotX) / kPlotW, 0.002);
    CHECK_CLOSE(ui->get_hover_y(), (200.0 - kPlotY) / kPlotH, 0.003);
    moveTo(win, 700, 300);
    CHECK_CLOSE(ui->get_hover_x(), (700.0 - kPlotX) / kPlotW, 0.002);
    CHECK_CLOSE(ui->get_hover_y(), (300.0 - kPlotY) / kPlotH, 0.003);
    {
        const int h0 = ui->get_hovers();
        moveTo(win, 701, 303);
        CHECK_EQ(ui->get_hovers(), h0 + 1);                // one report per pointer move, not one per axis
        moveTo(win, 701, 303);
        CHECK_EQ(ui->get_hovers(), h0 + 1);                // a move to the same spot reports nothing new
    }
    moveTo(win, kPlotX, kPlotY);                           // the very corner of the plot is inside it
    CHECK_CLOSE(ui->get_hover_x(), 0.0, 0.002);
    CHECK_CLOSE(ui->get_hover_y(), 0.0, 0.003);
    struct Spot { float x, y; };
    const Spot outside[] = {{20, 200}, {600, 40}, {600, 412}, {43, 200}, {600, 8}, {2000, 2000}};   // gutter, header, axis band, 1 px left of the plot, ribbon, off the window
    for (const Spot& p : outside) {
        moveTo(win, 600, 200);
        CHECK(ui->get_hover_x() > 0.0f);
        moveTo(win, p.x, p.y);
        CHECK(ui->get_hover_x() == -1.0f);
        CHECK(ui->get_hover_y() == -1.0f);
    }

    // CLICK: reported at the press position as a fraction; Shift read from the modifiers, else from shift-held
    wait(600);                                             // the double click interval runs on the wall clock (500 ms)
    click(win, 600, 200);
    CHECK_EQ(ui->get_clicks(), 1);
    CHECK_CLOSE(ui->get_click_x(), (600.0 - kPlotX) / kPlotW, 0.002);
    CHECK(!ui->get_click_shift());
    CHECK_EQ(ui->get_dclicks(), 0);
    wait(600);
    keyDown(win, slint::platform::key_codes::Shift);
    click(win, 700, 250);
    keyUp(win, slint::platform::key_codes::Shift);
    CHECK_EQ(ui->get_clicks(), 2);
    CHECK(ui->get_click_shift());
    CHECK_CLOSE(ui->get_click_x(), (700.0 - kPlotX) / kPlotW, 0.002);
    wait(600);
    ui->set_shift_held(true);                              // a platform whose pointer events carry no modifiers
    click(win, 600, 200);
    ui->set_shift_held(false);
    CHECK_EQ(ui->get_clicks(), 3);
    CHECK(ui->get_click_shift());
    wait(600);
    click(win, 600, 200);
    CHECK_EQ(ui->get_clicks(), 4);
    CHECK(!ui->get_click_shift());
    CHECK_EQ(ui->get_dclicks(), 0);
    // DOUBLE CLICK: two clicks at the same point within the interval; the first is still a click
    wait(600);
    click(win, 800, 300);
    click(win, 800, 300);
    CHECK_EQ(ui->get_dclicks(), 1);
    CHECK_CLOSE(ui->get_dclick_x(), (800.0 - kPlotX) / kPlotW, 0.002);
    CHECK_EQ(ui->get_clicks(), 6);
    // RIGHT CLICK: the position as a fraction and in the component; it is not a click
    click(win, 600, 200, slint::PointerEventButton::Right);
    CHECK_EQ(ui->get_rclicks(), 1);
    CHECK_CLOSE(ui->get_rclick_x(), (600.0 - kPlotX) / kPlotW, 0.002);
    CHECK_CLOSE(ui->get_rclick_px(), 600.0, 0.5);
    CHECK_CLOSE(ui->get_rclick_py(), 200.0, 0.5);
    CHECK_EQ(ui->get_clicks(), 6);
    CHECK_EQ(ui->get_dclicks(), 1);
    click(win, 491, 120, slint::PointerEventButton::Right);   // on the band too: the trace menu opens anywhere in the plot
    CHECK_EQ(ui->get_rclicks(), 2);
    CHECK_CLOSE(ui->get_rclick_py(), 120.0, 0.5);
    click(win, 20, 200, slint::PointerEventButton::Right);    // but not outside the plot
    CHECK_EQ(ui->get_rclicks(), 2);

    // WHEEL: notches = round(delta-y / 120), at least one per event, wheel up = +1, reported at the pointer.
    // Slint delta-y is POSITIVE for a wheel turned up (its SpinBox steps up on delta-y > 0, and the winit backend
    // passes LineDelta on times 60 with the sign unchanged), so scroll(.., -120) is a wheel turned DOWN: -1.
    moveTo(win, 600, 200);
    scroll(win, 600, 200, 0, 120);
    CHECK_EQ(ui->get_wheels(), 1);
    CHECK_EQ(ui->get_wheel_notches(), 1);
    CHECK_CLOSE(ui->get_wheel_x(), (600.0 - kPlotX) / kPlotW, 0.002);
    CHECK(!ui->get_wheel_shift());
    scroll(win, 600, 200, 0, -120);
    CHECK_EQ(ui->get_wheel_notches(), -1);
    scroll(win, 600, 200, 0, 240);
    CHECK_EQ(ui->get_wheel_notches(), 2);
    scroll(win, 600, 200, 0, 60);                          // what the winit backend sends for one notch of a wheel
    CHECK_EQ(ui->get_wheel_notches(), 1);
    scroll(win, 600, 200, 0, -60);
    CHECK_EQ(ui->get_wheel_notches(), -1);
    scroll(win, 600, 200, 0, 5);                           // a touchpad trickle still counts one per event
    CHECK_EQ(ui->get_wheel_notches(), 1);
    scroll(win, 600, 200, 0, -5);
    CHECK_EQ(ui->get_wheel_notches(), -1);
    CHECK_EQ(ui->get_wheels(), 7);
    scroll(win, 600, 200, 0, 0);                           // no delta: not a wheel event
    scroll(win, 600, 200, 30, 0);                          // sideways: not a zoom
    CHECK_EQ(ui->get_wheels(), 7);
    keyDown(win, slint::platform::key_codes::Shift);
    scroll(win, 600, 200, 0, 120);
    keyUp(win, slint::platform::key_codes::Shift);
    CHECK_EQ(ui->get_wheels(), 8);
    CHECK(ui->get_wheel_shift());
    scroll(win, 600, 200, 0, 120);
    CHECK(!ui->get_wheel_shift());
    moveTo(win, 20, 200);
    scroll(win, 20, 200, 0, 120);                          // over the gutter: not the plot
    CHECK_EQ(ui->get_wheels(), 9);                         // (the one just above)
    moveTo(win, 700, 300);
    scroll(win, 700, 300, 0, 120);
    CHECK_CLOSE(ui->get_wheel_x(), (700.0 - kPlotX) / kPlotW, 0.002);

    // ZONES: move over the band, ew-resize within 6 px of an edge (inside or outside it), default elsewhere
    struct Zone { float x; int zone; };
    const Zone zones[] = {
        {static_cast<float>(vfoC), 1}, {static_cast<float>(vfoL + 7), 1}, {static_cast<float>(vfoR - 7), 1},
        {static_cast<float>(vfoL), 2}, {static_cast<float>(vfoR), 2},
        {static_cast<float>(vfoL + 6), 2}, {static_cast<float>(vfoR - 6), 2},      // 6 px inside an edge: still the edge
        {static_cast<float>(vfoL - 6), 2}, {static_cast<float>(vfoR + 6), 2},      // 6 px outside it: still the edge
        {static_cast<float>(vfoL - 7), 0}, {static_cast<float>(vfoR + 7), 0}, {600, 0}, {200, 0},
    };
    for (const Zone& z : zones) {
        moveTo(win, z.x, 200);
        CHECK_EQ(ui->get_zone(), z.zone);
        CHECK_EQ(ui->get_shape(), z.zone);                 // 1 = move, 2 = ew-resize, 0 = default
    }
    moveTo(win, 20, 200);                                  // out of the plot: nothing
    CHECK_EQ(ui->get_zone(), 0);

    // BAND DRAG: a press inside the band moves it; band-dragged carries the travel per pointer move
    const float vc = static_cast<float>(vfoC), vl = static_cast<float>(vfoL), vr = static_cast<float>(vfoR);
    wait(600);
    int clicksBase = ui->get_clicks();
    ui->set_band_dx(0);
    ui->set_band_moves(0);
    moveTo(win, vc, 200);
    press(win, vc, 200);
    CHECK_EQ(ui->get_band_moves(), 0);                     // a press alone moves nothing
    moveTo(win, vc + 20, 200);
    CHECK_EQ(ui->get_band_moves(), 1);
    CHECK_CLOSE(ui->get_band_dx(), 20.0 / kPlotW, 0.0005);
    Frame held = render(ui, kW, kH);                       // while it is held the fill is brighter
    CHECK(held.near(484, 300, over(kAccent, 0.27, kWell), 10));
    release(win, vc + 20, 200);
    CHECK_CLOSE(ui->get_band_dx(), 20.0 / kPlotW, 0.0005);
    CHECK_EQ(ui->get_clicks(), clicksBase);                // a drag of the band is not a click
    CHECK_EQ(ui->get_edge_moves(), 0);
    Frame rest = render(ui, kW, kH);
    CHECK(rest.near(484, 300, vfoFill, 10));               // and the fill is back to normal
    ui->set_band_dx(0);
    ui->set_band_moves(0);
    moveTo(win, vc, 200);
    press(win, vc, 200);
    for (int i = 1; i <= 4; ++i) { moveTo(win, vc + 5.0f * i, 200); }
    moveTo(win, vc + 10, 200);                             // and back by 10
    release(win, vc + 10, 200);
    CHECK_EQ(ui->get_band_moves(), 5);
    CHECK_CLOSE(ui->get_band_dx(), 10.0 / kPlotW, 0.0005); // the moves add up to the net travel
    ui->set_band_dx(0);
    moveTo(win, vc, 200);
    press(win, vc, 200);
    moveTo(win, vc - 30, 200);
    release(win, vc - 30, 200);
    CHECK_CLOSE(ui->get_band_dx(), -30.0 / kPlotW, 0.0005);
    ui->set_band_dx(0);
    moveTo(win, 600, 200);                                 // a press off the band is no band drag
    press(win, 600, 200);
    moveTo(win, 640, 200);
    release(win, 640, 200);
    CHECK_EQ(ui->get_band_dx(), 0.0f);
    CHECK_EQ(ui->get_edge_moves(), 0);
    CHECK_EQ(ui->get_clicks(), clicksBase + 1);            // it ends as a click, at the press position
    CHECK_CLOSE(ui->get_click_x(), (600.0 - kPlotX) / kPlotW, 0.002);
    clicksBase += 1;

    // A DRAG HOLDS THE BAND when the pointer leaves the plot and even the window, and ends cleanly on release there
    ui->set_band_dx(0);
    moveTo(win, vc, 200);
    press(win, vc, 200);
    moveTo(win, 1500, 200);
    CHECK_CLOSE(ui->get_band_dx(), (1500.0 - vc) / kPlotW, 0.001);
    CHECK_EQ(ui->get_zone(), 1);                           // the grab holds while the button is down
    release(win, 1500, 200);
    CHECK_EQ(ui->get_zone(), 0);
    const float dxAfter = ui->get_band_dx();
    moveTo(win, 600, 200);                                 // coming back: nothing is still dragging
    moveTo(win, 700, 220);
    CHECK_EQ(ui->get_band_dx(), dxAfter);

    // A DOUBLE CLICK ON THE BAND: not a click, not a double click (it unzooms only empty spectrum)
    wait(600);
    {
        const int c0 = ui->get_clicks(), d0 = ui->get_dclicks();
        click(win, vc, 200);
        click(win, vc, 200);
        CHECK_EQ(ui->get_clicks(), c0);
        CHECK_EQ(ui->get_dclicks(), d0);
    }

    // EDGE DRAG: a press within 6 px of an edge resizes the band about its centre; the edge follows the pointer
    ui->set_edge_half(-2);
    ui->set_edge_moves(0);
    ui->set_band_moves(0);
    moveTo(win, vr, 200);
    press(win, vr, 200);
    moveTo(win, vr + 10, 200);
    CHECK_EQ(ui->get_edge_moves(), 1);
    CHECK_CLOSE(ui->get_edge_half(), 25.0 / kPlotW, 0.0006);     // 15 px grew to 25
    moveTo(win, vr - 10, 200);
    CHECK_CLOSE(ui->get_edge_half(), 5.0 / kPlotW, 0.0006);      // 15 px shrank to 5
    moveTo(win, vr - 40, 200);
    CHECK_EQ(ui->get_edge_half(), 0.0f);                         // never below zero
    release(win, vr - 40, 200);
    CHECK_EQ(ui->get_band_moves(), 0);
    moveTo(win, vl, 200);                                  // the left edge: moving left widens
    press(win, vl, 200);
    moveTo(win, vl - 10, 200);
    CHECK_CLOSE(ui->get_edge_half(), 25.0 / kPlotW, 0.0006);
    release(win, vl - 10, 200);
    moveTo(win, vr - 4, 200);                              // grabbed 4 px inside the edge: no jump to the pointer
    press(win, vr - 4, 200);
    moveTo(win, vr - 4 + 10, 200);
    CHECK_CLOSE(ui->get_edge_half(), 25.0 / kPlotW, 0.0006);
    release(win, vr - 4 + 10, 200);
    CHECK_EQ(ui->get_clicks(), clicksBase);                // an edge drag is not a click either

    // TOOLTIPS: the string follows where the pointer is; the tip appears 600 ms after it came to rest, where it rests
    const std::string kBandTip = "Drag to retune, Shift for free tuning";
    const std::string kEdgeTip = "Drag to change the bandwidth";
    const std::string kOffTip = "Click to tune, double-click to see the full span";
    moveTo(win, vc, 200);
    CHECK(text(ui->get_tip()) == kBandTip);
    moveTo(win, vr, 200);
    CHECK(text(ui->get_tip()) == kEdgeTip);
    moveTo(win, vl, 200);
    CHECK(text(ui->get_tip()) == kEdgeTip);
    const auto restBegan = std::chrono::steady_clock::now();
    moveTo(win, 700, 300);
    CHECK(text(ui->get_tip()) == kOffTip);
    CHECK(!ui->get_tip_shown());                           // not yet: the pointer has only just arrived
    CHECK(waitUntil([&] { return ui->get_tip_shown(); }, 8000));
    const auto restMs = std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - restBegan).count();
    CHECK(restMs >= 550);                                  // it waited for the 600 ms of rest (a busy machine only adds)
    moveTo(win, 703, 302);                                 // a few pixels of jitter keep the tip
    CHECK(ui->get_tip_shown());
    Frame tipped = render(ui, kW, kH);                     // the tip is drawn below the pointer, in the tooltip colours
    CHECK(tipped.near(712, 326, rgb(0x262a32), 8));
    CHECK(tipped.countInk(700, 322, 900, 347, rgb(0x262a32), 24) > 40);   // and carries the words
    {   // the tip is not a popup that takes the pointer: the first click after it appeared still lands, and hides it
        const int before = ui->get_clicks();
        click(win, 703, 302);
        CHECK_EQ(ui->get_clicks(), before + 1);
        CHECK(!ui->get_tip_shown());
    }
    moveTo(win, vc, 200);                                  // moving on closes it, over the band it says the band string
    CHECK(!ui->get_tip_shown());
    CHECK(text(ui->get_tip()) == kBandTip);
    CHECK(waitUntil([&] { return ui->get_tip_shown(); }, 8000));
    press(win, vc, 200);                                   // a press hides it
    CHECK(!ui->get_tip_shown());
    release(win, vc, 200);
    moveTo(win, vr, 200);
    CHECK(waitUntil([&] { return ui->get_tip_shown(); }, 8000));
    CHECK(text(ui->get_tip()) == kEdgeTip);
    moveTo(win, 20, 200);                                  // leaving the plot closes it
    CHECK(!ui->get_tip_shown());

    // near the foot of the well the tip opens above the pointer, so it stays inside
    moveTo(win, 700, 395);
    CHECK(waitUntil([&] { return ui->get_tip_shown(); }, 8000));
    {
        Frame flipped = render(ui, kW, kH);
        CHECK(inCol(flipped, 706, 340, 395, rgb(0x262a32), 8) >= 20);        // the tip face, above the pointer
        CHECK_EQ(inCol(flipped, 706, 396, 406, rgb(0x262a32), 8), 0);        // nothing under it
    }
    moveTo(win, 20, 200);

    // DISABLED: nothing is reported, nothing is grabbed
    ui->set_enabled(false);
    const int clicksNow = ui->get_clicks(), rclicksNow = ui->get_rclicks(), wheelsNow = ui->get_wheels();
    ui->set_band_dx(0);
    wait(600);
    moveTo(win, vc, 200);
    CHECK_EQ(ui->get_zone(), 0);
    press(win, vc, 200);
    moveTo(win, vc + 20, 200);
    release(win, vc + 20, 200);
    click(win, 600, 200);
    click(win, 600, 200, slint::PointerEventButton::Right);
    scroll(win, 600, 200, 0, 120);
    CHECK_EQ(ui->get_band_dx(), 0.0f);
    CHECK_EQ(ui->get_clicks(), clicksNow);
    CHECK_EQ(ui->get_rclicks(), rclicksNow);
    CHECK_EQ(ui->get_wheels(), wheelsNow);
    ui->set_enabled(true);
    wait(600);
    click(win, 600, 200);
    CHECK_EQ(ui->get_clicks(), clicksNow + 1);             // and back

    // SHOW-VFO off (the Airband monitor listens): no band, no grabs, a click on its old place tunes
    ui->set_show_vfo(false);
    Frame novfo = render(ui, kW, kH);
    CHECK(novfo.near(484, 300, kWell));
    CHECK_EQ(inCol(novfo, vfoL, kPlotY, kPlotBottom, kAccent, 12), 0);
    CHECK_EQ(inCol(novfo, vfoR, kPlotY, kPlotBottom, kAccent, 12), 0);
    CHECK_EQ(inCol(novfo, vfoC, kPlotY, kPlotBottom, kAccent, 12), 0);
    moveTo(win, vc, 200);
    CHECK_EQ(ui->get_zone(), 0);
    CHECK(text(ui->get_tip()) == kOffTip);
    ui->set_band_dx(0);
    ui->set_edge_moves(0);
    press(win, vc, 200);
    moveTo(win, vc + 20, 200);
    release(win, vc + 20, 200);
    CHECK_EQ(ui->get_band_dx(), 0.0f);
    CHECK_EQ(ui->get_edge_moves(), 0);
    wait(600);
    click(win, vc, 200);
    CHECK_EQ(ui->get_clicks(), clicksNow + 3);             // the dragged press ended as a click too, then this one
    CHECK_CLOSE(ui->get_click_x(), (vc - kPlotX) / kPlotW, 0.002);
    ui->set_show_vfo(true);

    // SHOW-PEAK off: the peak block goes, the SPAN chip stays
    ui->set_show_peak(false);
    Frame nopeak = render(ui, kW, kH);
    CHECK_EQ(nopeak.countInk(860, 12, kW, 46, kWell), 0);
    CHECK(nopeak.near(1003, 68, kChipBg));
    ui->set_show_peak(true);

    // RIBBON SIZE: 8 / 13 / 20 px, none at 0
    const int ribbonRows[] = {0, 8, 13, 20};
    for (int size = 0; size <= 3; ++size) {
        ui->set_ribbon_size(size);
        Frame r = render(ui, kW, kH);
        const int rows = ribbonRows[size];
        CHECK_EQ(inCol(r, 800, 0, 40, kBandplan), rows);
        if (rows > 0) { CHECK(r.near(800, rows - 1, kBandplan)); }
        CHECK(r.near(800, rows, kWell));
    }
    ui->set_ribbon_size(1);

    // TRACE MODE: the label in the header is orange for peak hold, ivory for average, absent for normal
    const int headerInkPeak = f.countInk(440, 14, 760, 28, kWell);
    ui->set_trace_mode(TraceMode::Average);
    Frame avg = render(ui, kW, kH);
    CHECK(inkCoverage(avg, 440, 14, 760, 28, kWell, kAverage) > 0.6);
    CHECK(inkCoverage(avg, 440, 14, 760, 28, kWell, kPeakHold) < 0.3);
    ui->set_trace_mode(TraceMode::Normal);
    Frame normal = render(ui, kW, kH);
    CHECK(normal.countInk(440, 14, 760, 28, kWell) < headerInkPeak - 20);
    CHECK(inkCoverage(normal, 440, 14, 760, 28, kWell, kPeakHold) < 0.3);
    ui->set_trace_mode(TraceMode::PeakHold);

    // BAND-DRAGGING from the host: the brighter fill
    ui->set_band_dragging(true);
    Frame dragging = render(ui, kW, kH);
    CHECK(dragging.near(484, 300, over(kAccent, 0.27, kWell), 10));
    CHECK(!dragging.near(484, 300, vfoFill, 10));
    ui->set_band_dragging(false);

    // CURSOR: hidden when the host says so
    ui->set_cursor_visible(false);
    Frame nocursor = render(ui, kW, kH);
    CHECK(nocursor.near(560, 300, kWell));
    CHECK_EQ(nocursor.countInk(570, 120, 700, 140, kWell), 0);
    ui->set_cursor_visible(true);

    // BAND SEGMENTS: the plan own colour or the theme ribbon colour when transparent; a name steps below the header
    // only where it would sit under it; a segment too narrow for a name draws its colour alone
    {
        auto bands = std::make_shared<slint::VectorModel<BandSegment>>();
        bands->push_back(BandSegment{0.0f, 0.2f, slint::SharedString("Beacons"), slint::Color()});
        bands->push_back(BandSegment{0.5f, 0.52f, slint::SharedString("X"), slint::Color::from_rgb_uint8(0x8a, 0x2a, 0x3a)});
        bands->push_back(BandSegment{0.7f, 1.0f, slint::SharedString("Packet"), slint::Color::from_rgb_uint8(0x2a, 0x5a, 0x3a)});
        ui->set_bands(bands);
        Frame b = render(ui, kW, kH);
        CHECK(b.near(100, 4, kBandplan));                  // transparent: the theme colour
        CHECK(b.near(300, 4, kWell));                      // a gap in the plan stays dark
        CHECK(b.near(pageX(0.51), 4, rgb(0x8a2a3a)));      // an own colour
        CHECK(b.near(pageX(0.85), 4, rgb(0x2a5a3a)));
        CHECK(b.near(pageX(0.52) + 2, 4, kWell));
        CHECK(b.countInk(46, 28, 140, 44, kWell) >= 15);   // Beacons: under the header, so one line down
        CHECK_EQ(b.countInk(kPlotX, 8, 240, 13, kWell), 0);
        CHECK(b.countInk(pageX(0.7) + 2, 9, pageX(0.7) + 60, 24, kWell) >= 10);   // Packet: clear of the header, right under the ribbon
        CHECK(inkCoverage(b, pageX(0.7) + 2, 9, pageX(0.7) + 60, 24, kWell, kBandplanFg) > 0.55);
        CHECK_EQ(b.countInk(pageX(0.7) + 2, 29, pageX(0.7) + 60, 44, kWell), 0);
        CHECK_EQ(b.countInk(pageX(0.5) - 2, 9, pageX(0.52) + 6, 44, kWell), 0);    // X: too narrow for a name
        ui->set_bands(std::make_shared<slint::VectorModel<BandSegment>>(std::vector<BandSegment>{
            BandSegment{0.0f, 1.0f, slint::SharedString("2 m amateur"), slint::Color()}}));
    }

    // AIRBAND: the block caption and what is heard under the header, the channels as bands (bright open, faint shut)
    ui->set_airband_block("Airband · block 2");
    ui->set_airband_hearing("hearing 118.700");
    {
        auto channels = std::make_shared<slint::VectorModel<AirbandChannel>>();
        channels->push_back(AirbandChannel{0.60f, 0.64f, true});
        channels->push_back(AirbandChannel{0.70f, 0.74f, false});
        ui->set_airband_channels(channels);
        ui->set_show_vfo(false);
        ui->set_show_peak(false);
        Frame a = render(ui, kW, kH);
        CHECK(a.countInk(52, 44, 400, 58, kWell) >= 25);
        CHECK(inkCoverage(a, 52, 44, 400, 58, kWell, kMuted) > 0.5);              // the block name
        CHECK(inkCoverage(a, 52, 44, 500, 58, kWell, kAccentSoft) > 0.5);         // what is heard, in amber
        CHECK(a.near(pageX(0.62), 300, over(kPhosphor, 0.28, kWell), 10));        // open: a bright band...
        CHECK(a.near(pageX(0.60), 300, kPhosphor, 14));                           // ...with its edge lines
        CHECK(a.near(pageX(0.64) - 1, 300, kPhosphor, 14));
        CHECK(a.near(pageX(0.72), 300, over(kPhosphor, 0.07, kWell), 8));         // shut: a faint one
        CHECK(a.near(pageX(0.66), 300, kWell));                                   // between them nothing
        CHECK_EQ(a.countInk(860, 12, kW, 46, kWell), 0);                          // the peak block is gone while it listens
        CHECK_EQ(inCol(a, vfoC, kPlotY, kPlotBottom, kAccent, 12), 0);            // so is the band
        a.savePng(shotDir() + "spectrum-specimen-airband.png");
        ui->set_airband_block("");
        ui->set_airband_hearing("");
        ui->set_airband_channels(std::make_shared<slint::VectorModel<AirbandChannel>>());
        ui->set_show_vfo(true);
        ui->set_show_peak(true);
    }

    // RESIZE: the plot is x 44 .. the right edge and y 76 .. height - 18, whatever the size of the well; every
    // position stays a fraction of it and a picture painted at another size is stretched over it
    {
        auto rs = SpectrumResizeSpecimen::create();
        rs->set_trace(slint::Image(traceBuffer));
        constexpr int w2 = 800, h2 = 300;
        const int pw2 = w2 - kPlotX, ph2 = h2 - kPlotY - 18;                      // 756 x 206
        Frame r = render(rs, w2, h2);
        CHECK_EQ(r.width, w2);
        CHECK_EQ(r.height, h2);
        CHECK(r.near(w2 - 1, 4, kBandplan));                                        // the ribbon runs to the new right edge
        CHECK(r.near(kPlotX - 1, 4, kWell));
        CHECK_EQ(r.countInk(w2 - 15, 12, w2, 46, kWell), 0);                        // the peak block stays 16 px in
        CHECK(r.countInk(640, 12, w2 - 16, 46, kWell) > 40);
        CHECK(r.near(w2 - 17, 68, kChipLine, 10));                                  // and so does the SPAN chip
        for (int k = 1; k <= 4; ++k) {
            CHECK(r.rowNear(kPlotY + static_cast<int>(std::lround(ph2 * 0.2 * k)), 560, 660, kGrid));   // dB ticks: fractions of the new height
        }
        CHECK(r.near(600, kPlotY + ph2, kWell));                                    // the foot line is still the clipped last tick
        for (int k : {2, 4, 6, 7, 9}) {
            CHECK(inCol(r, kPlotX + static_cast<int>(std::lround(pw2 * 0.1 * k)), 150, 200, kGrid) >= 45);   // frequency ticks: of the new width
        }
        for (int k : {2, 4, 5}) {
            const int x = kPlotX + static_cast<int>(std::lround(pw2 * 0.1 * k));
            CHECK(r.countInk(x - 15, h2 - 16, x + 16, h2, kWell) >= 8);             // the axis labels follow the foot
        }
        CHECK(r.countInk(w2 - 34, h2 - 16, w2 - 2, h2, kWell) >= 6);                // the unit follows the right edge
        CHECK(r.countInk(0, kPlotY + 82 - 7, kPlotX, kPlotY + 82 + 7, kWell) >= 6);     // the -40 label sits on the 0.4 tick
        const int bandL = kPlotX + static_cast<int>(std::lround(pw2 * 0.28)), bandR = kPlotX + static_cast<int>(std::lround(pw2 * 0.32));
        CHECK(r.near(bandL, 250, kAccent, 12));                                     // the VFO band: a fraction of the new width
        CHECK(r.near(bandR, 250, kAccent, 12));
        CHECK(r.near(bandL + 6, 250, over(kAccent, kVfoFillAlpha, kWell), 10));
        int greenRow = -1;
        for (int y = kPlotY; y < kPlotY + ph2; ++y) { if (r.near(700, y, kGreen, 80)) { greenRow = y; break; } }
        CHECK(greenRow >= kPlotY + 61 && greenRow <= kPlotY + 64);                  // row 100 of 330 lands near row 62 of 206
        Frame s2 = render(rs, kW, kH);                                              // and at the sheet size it is the sheet geometry
        for (int k = 1; k <= 4; ++k) { CHECK(s2.rowNear(kPlotY + 66 * k, 560, 660, kGrid)); }
        CHECK(s2.near(kPlotX + 500, kPlotY + 100, kGreen, 4));
        Frame again = render(rs, w2, h2);
        int differing = 0;
        for (std::size_t i = 0; i < again.px.size(); ++i) {
            if (again.px[i].r != r.px[i].r || again.px[i].g != r.px[i].g || again.px[i].b != r.px[i].b) { ++differing; }
        }
        CHECK_EQ(differing, 0);                                                     // resizing leaves nothing behind
    }

    // THE SKIN: the same view in another filling follows the theme tokens, it draws no colour of its own
    {
        Skin probe = ui->global<Workbench>().get_value();
        probe.well = slint::Color::from_rgb_uint8(0x10, 0x20, 0x30);
        probe.graticule = slint::Color::from_rgb_uint8(0x40, 0x50, 0x60);
        probe.bandplan = slint::Color::from_rgb_uint8(0x70, 0x10, 0x10);
        probe.vfo = slint::Color::from_rgb_uint8(0x10, 0xd0, 0x20);
        probe.bookmark_tick = slint::Color::from_rgb_uint8(0xd0, 0x10, 0xd0);
        probe.mark = slint::Color::from_rgb_uint8(0x20, 0xd0, 0xd0);
        probe.trace_peak = slint::Color::from_rgb_uint8(0xd0, 0xd0, 0x10);
        ui->global<Theme>().set_skin(probe);
        Frame sk = render(ui, kW, kH);
        CHECK(sk.near(620, 300, rgb(0x102030)));                                    // the ground
        CHECK(sk.near(kPlotX + 98 * 5, 300, rgb(0x405060)));                        // a graticule line
        CHECK(sk.near(300, 4, rgb(0x701010)));                                      // the ribbon
        CHECK(sk.near(vfoL, 300, rgb(0x10d020), 12));                               // the VFO edge
        CHECK_EQ(sk.countNear(gb3, kPlotBottom - 8, gb3 + 2, kPlotBottom, rgb(0xd010d0), 10), 16);   // a bookmark tick
        CHECK(sk.near(markX, kPlotY + 1, rgb(0x20d0d0)));                           // a plugin mark
        CHECK(inkCoverage(sk, 440, 14, 760, 28, rgb(0x102030), rgb(0xd0d010)) > 0.6);   // the PEAK HOLD label
        ui->global<Theme>().set_skin(ui->global<Workbench>().get_value());
    }

    {
        Frame back = render(ui, kW, kH);                   // everything restored: the first picture again
        int differing = 0;
        for (std::size_t i = 0; i < back.px.size(); ++i) {
            if (back.px[i].r != f.px[i].r || back.px[i].g != f.px[i].g || back.px[i].b != f.px[i].b) { ++differing; }
        }
        CHECK_EQ(differing, 0);
    }

    return testSummary("test_slint_spectrum");
}
