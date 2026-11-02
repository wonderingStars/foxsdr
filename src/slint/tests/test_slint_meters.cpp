// The meter (one component, four faces) measured against the app's own faces (src/gui/meter_styles_math.hpp:
// twelve segments, the two top ones the alarm colour and the next three the caution colour; scope_view.cpp: nine ticks,
// the last two in the alarm colour, a filled alarm wedge on the analogue face, a peak-hold catch on ladder and bar) and the
// handover's inventory (126 x 56 face, caption above, value below, right-click menu, hover hint). Every position is the
// specimen's own table (ui/specimens/meters_specimen.slint): a meter at (x, y) has its face at (x, y + 17), 126 x 56.
//
// What is exact and what is not. The ladder segments and the bar are rectangles on whole pixels, so their runs, heights and
// colours are asserted exactly. The arc, the ticks and the needles are Path strokes, and the software renderer places a
// stroke with up to ~0.6 px of sub-pixel error and soft edges, so those are asserted by AREA and by window ("amber ink in
// this 8 x 7 window"), never by the exact pixel a hairline lands on.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "meters_specimen.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace foxsdr::slinttest;

namespace {

// The Workbench filling's colours.
constexpr Rgb kBg = rgb(0x15171b);
constexpr Rgb kFace = rgb(0x0f1114);        // meter-face
constexpr Rgb kBorder = rgb(0x363b45);      // line-strong
constexpr Rgb kInk = rgb(0xc9ced6);         // meter-ink
constexpr Rgb kNeedle = rgb(0xf5a524);      // meter-needle
constexpr Rgb kLow = rgb(0x3ddc84);         // meter-led-low
constexpr Rgb kMid = rgb(0xf5a524);         // meter-led-mid
constexpr Rgb kHigh = rgb(0xff5c5c);        // meter-led-high
constexpr Rgb kPeak = rgb(0xffd27a);        // meter-peak
const Rgb kBarOff = over(kInk, 0.18, kFace);   // meter-ink at 18 %: the bar's empty part, as unlit as an LED segment
constexpr Rgb kDim = rgb(0x7d8694);         // dim: the hub without a reading
constexpr Rgb kMuted = rgb(0x9aa3ad);       // muted: the caption
constexpr Rgb kFg = rgb(0xe6e8ec);          // fg: the value text
constexpr Rgb kTip = rgb(0x262a32);         // menu: the tooltip's ground

struct Pos { int x, y; };
constexpr Pos A1{16, 16}, A2{166, 16}, A3{316, 16}, A4{466, 16};
constexpr Pos B1{16, 124}, B2{166, 124}, B3{316, 124}, B4{466, 124};
constexpr Pos C1{16, 232}, C2{166, 232}, C3{316, 232}, C4{466, 232};
constexpr Pos D1{16, 340}, D2{166, 340}, D3{316, 340}, D4{466, 340};
constexpr Pos E1{16, 448}, E2{166, 448}, E3{316, 448}, E4{466, 448};
constexpr Pos F1{16, 556}, F2{166, 556}, F3{316, 556}, F4{466, 556};
constexpr Pos G1{640, 16}, G2{640, 140}, G3{640, 240}, G4{640, 348}, G5{640, 456};

// A meter's face rectangle, border included. The caption row is 14px and 3px of air sit above the face.
struct Face { int x, y, w, h; };
Face faceOf(Pos p, int w = 126, int h = 56) { return {p.x, p.y + 17, w, h}; }

// Pixels of colour c inside a face, the border excluded.
int inFace(const Frame& f, const Face& fc, Rgb c, int tol = 8) {
    return f.countNear(fc.x + 1, fc.y + 1, fc.x + fc.w - 1, fc.y + fc.h - 1, c, tol);
}
// Pixels where two same-size regions differ by more than tol in any channel.
int differing(const Frame& f, const Face& a, const Face& b, int tol = 16) {
    int n = 0;
    for (int y = 0; y < a.h; ++y) {
        for (int x = 0; x < a.w; ++x) {
            const Rgb p = f.at(a.x + x, a.y + y), q = f.at(b.x + x, b.y + y);
            if (std::abs(int(p.r) - int(q.r)) > tol || std::abs(int(p.g) - int(q.g)) > tol || std::abs(int(p.b) - int(q.b)) > tol) { ++n; }
        }
    }
    return n;
}
// How much needle colour a window holds, in pixels of area: each amber-ish pixel counts its share of the way from the face
// to full amber. A stroke 1.8px wide and 36px long holds ~65 whatever its anti-aliasing; grey ink, red, green and the
// wedge are not amber-ish (their green channel does not lead their blue one), so they add nothing.
double amberArea(const Frame& f, int x0, int y0, int x1, int y1) {
    double sum = 0.0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Rgb c = f.at(x, y);
            if (c.r > c.g + 8 && c.g > c.b + 10) { sum += std::min(1.0, std::max(0.0, (double(c.r) - 15.0) / 230.0)); }
        }
    }
    return sum;
}
// Pixels in a window that are RED-ish: red leads, green and blue are close together. (Amber has green far above blue and
// grey has no lead, so neither the pointer nor the grey ticks count - an alarm tick is told from both.)
int redPixels(const Frame& f, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Rgb c = f.at(x, y);
            if (c.r > c.g + 40 && std::abs(int(c.g) - int(c.b)) < 30) { ++n; }
        }
    }
    return n;
}
// Pixels satisfying `pred` in a sector of the ring r0 <= r < r1 round the point (cx, cy), centred on `angleDeg` (0 = straight
// up, positive clockwise) and `halfDeg` wide each side. A dial's ticks and arc are read this way: where a hairline lands to
// within a pixel does not matter, which ring and which angle it lies at does.
template <class Pred>
int sectorCount(const Frame& f, int cx, int cy, double r0, double r1, double angleDeg, double halfDeg, Pred pred) {
    const double pi = 3.14159265358979;
    int n = 0;
    for (int y = cy - 48; y <= cy + 6; ++y) {
        for (int x = cx - 48; x <= cx + 48; ++x) {
            const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;           // the pixel's centre, from the pivot (a pixel corner)
            const double r = std::sqrt(dx * dx + dy * dy);
            if (r < r0 || r >= r1) { continue; }
            if (std::abs(std::atan2(dx, -dy) * 180.0 / pi - angleDeg) > halfDeg) { continue; }
            if (pred(f.at(x, y))) { ++n; }
        }
    }
    return n;
}
struct Run { int start, len; };
// Runs of pixels that are not `background` along one row.
std::vector<Run> runsAcross(const Frame& f, int y, int x0, int x1, Rgb background, int tol) {
    std::vector<Run> out;
    int start = -1;
    for (int x = x0; x < x1; ++x) {
        const bool ink = !f.near(x, y, background, tol);
        if (ink && start < 0) { start = x; }
        if (!ink && start >= 0) { out.push_back({start, x - start}); start = -1; }
    }
    if (start >= 0) { out.push_back({start, x1 - start}); }
    return out;
}
int tipPixels(const Frame& f, int x0, int y0, int x1, int y1) { return f.countNear(x0, y0, x1, y1, kTip, 2); }

// Pixels where the w x h rectangles at (ax, ay) and (bx, by) differ by more than tol in any channel.
int differingRects(const Frame& f, int ax, int ay, int bx, int by, int w, int h, int tol = 8) {
    int n = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const Rgb p = f.at(ax + x, ay + y), q = f.at(bx + x, by + y);
            if (std::abs(int(p.r) - int(q.r)) > tol || std::abs(int(p.g) - int(q.g)) > tol || std::abs(int(p.b) - int(q.b)) > tol) { ++n; }
        }
    }
    return n;
}

slint::Color col(unsigned r, unsigned g, unsigned b) {
    return slint::Color::from_rgb_uint8(static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b));
}

// The ladder's segment i at value v (0..1) with peak p (-1 none): what colour its middle pixel must be.
Rgb ladderColour(int i, double v, double p, bool hasReading = true) {
    const Rgb zone = i >= 10 ? kHigh : i >= 7 ? kMid : kLow;
    const int lit = hasReading ? static_cast<int>(std::lround(v * 12)) : 0;
    const int catchSeg = p >= 0 ? std::clamp(static_cast<int>(std::lround(p * 12)) - 1, 0, 11) : -1;
    if (i == catchSeg) { return kPeak; }
    return i < lit ? zone : over(zone, 0.18, kFace);
}
// Checks all twelve segments of the ladder on a face: twelve runs of 8px 2px apart from x + 4, each in its colour.
void checkLadder(const Frame& f, Pos p, double v, double peak, bool hasReading = true) {
    const Face fc = faceOf(p);
    const int mid = fc.y + 28;
    const std::vector<Run> runs = runsAcross(f, mid, fc.x + 1, fc.x + fc.w - 1, kFace, 4);
    CHECK(runs.size() == 12);
    for (std::size_t i = 0; i < runs.size() && i < 12; ++i) {
        CHECK(runs[i].start == fc.x + 4 + 10 * static_cast<int>(i) && runs[i].len == 8);
        CHECK(f.near(runs[i].start + 4, mid, ladderColour(static_cast<int>(i), v, peak, hasReading), 6));
    }
}

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = MetersSpecimen::create();
    slint::Window& w = ui->window();

    Frame f = render(ui, 880, 670);
    CHECK(f.width == 880 && f.height == 670);
    CHECK(f.near(870, 5, kBg));
    f.savePng(shotDir() + "meters-specimen.png");
    // the enum reaches C++ through the specimen's re-export, in the order the right-click menu offers it
    CHECK(static_cast<int>(MeterStyle::Classic) == 0 && static_cast<int>(MeterStyle::Analogue) == 1);
    CHECK(static_cast<int>(MeterStyle::LedLadder) == 2 && static_cast<int>(MeterStyle::Peak) == 3);

    // ===== the face: 126 x 56 on every style, border line-strong, ground meter-face ============================
    const std::array<Pos, 22> enabledOnes = {A1, A2, A3, A4, B1, B2, B3, B4, C1, C2, C3, C4, D1, D2, D3, D4, E1, E3, F1, F2, F3, F4};
    for (const Pos p : enabledOnes) {
        const Face fc = faceOf(p);
        const int mid = fc.y + 28;
        CHECK(f.near(fc.x, mid, kBorder) && f.near(fc.x + 125, mid, kBorder));            // left and right edge: 126 wide
        CHECK(f.near(fc.x - 1, mid, kBg) && f.near(fc.x + 126, mid, kBg));
        CHECK(f.near(fc.x + 20, fc.y, kBorder) && f.near(fc.x + 20, fc.y + 55, kBorder));  // top and bottom edge: 56 high
        CHECK(f.near(fc.x + 20, fc.y - 1, kBg) && f.near(fc.x + 20, fc.y + 56, kBg));
        CHECK(f.near(fc.x + 2, mid, kFace));                                              // the ground inside
    }
    // the caption above (cap look, muted) and the value below (mono 600, fg), with the sheet's air between them and the face
    for (const Pos p : {A1, A2, A3, A4, B1, C2, D1, D2, D3, D4, F1, F3}) {
        const Face fc = faceOf(p);
        CHECK(f.countInk(p.x, p.y, p.x + 126, p.y + 14, kBg) > 30);                      // caption
        CHECK(f.countInk(p.x, p.y + 14, p.x + 126, p.y + 17, kBg) == 0);                 // 3px of air
        CHECK(f.countInk(p.x, fc.y + 56, p.x + 126, fc.y + 60, kBg) == 0);               // 4px of air
        CHECK(f.countInk(p.x, fc.y + 60, p.x + 126, fc.y + 74, kBg) > 12);               // value
    }
    CHECK(f.countNear(A1.x, A1.y, A1.x + 126, A1.y + 14, kMuted, 30) > 3);              // the caption's colour
    CHECK(f.countNear(A1.x, A1.y + 77, A1.x + 126, A1.y + 91, kFg, 30) > 3);              // the value's colour
    CHECK(f.countInk(E1.x + 30, E1.y + 77, E1.x + 96, E1.y + 91, kBg) > 6);              // "--": no reading is still a line
    // the text is centred on the 126px column
    {
        int lo = 999, hi = -1;
        for (int y = A1.y + 77; y < A1.y + 91; ++y) {
            for (int x = A1.x; x < A1.x + 126; ++x) { if (!f.near(x, y, kBg, 24)) { lo = std::min(lo, x); hi = std::max(hi, x); } }
        }
        CHECK(hi > lo && std::abs((lo + hi) / 2.0 - (A1.x + 62.5)) < 3.0);
    }

    // ===== the four styles are four different pictures of the SAME reading =====================================
    const Face a1 = faceOf(A1), a2 = faceOf(A2), a3 = faceOf(A3), a4 = faceOf(A4);
    const int needleClassic = inFace(f, a1, kNeedle, 60), needleAnalogue = inFace(f, a2, kNeedle, 60);
    const int lowClassic = inFace(f, a1, kLow), lowAnalogue = inFace(f, a2, kLow), lowLadder = inFace(f, a3, kLow), lowBar = inFace(f, a4, kLow);
    const int peakClassic = inFace(f, a1, kPeak), peakAnalogue = inFace(f, a2, kPeak), peakLadder = inFace(f, a3, kPeak), peakBar = inFace(f, a4, kPeak);
    CHECK(needleClassic > 20 && needleClassic < 140);                       // a thin pointer
    CHECK(needleAnalogue > needleClassic * 1.3);                            // a bolder one (3px against 1.8px)
    CHECK(lowClassic == 0 && lowAnalogue == 0);                             // the arc styles hold no segment colour
    CHECK(lowLadder >= 1900 && lowLadder <= 7 * 288);                       // 7 lit segments of 8 x 36 (a 1px radius trims the corners)
    CHECK(lowBar >= 1500 && lowBar <= 1700);                                // the bar's green zone: 7/12 of 118px x 24
    CHECK(peakClassic == 0 && peakAnalogue == 0);                           // no catch on an arc
    CHECK(peakLadder >= 250 && peakLadder <= 288);                          // one segment: 8 x 36
    CHECK(peakBar >= 50 && peakBar <= 60);                                  // one line: 2 x 30
    const Face faces[4] = {a1, a2, a3, a4};
    for (int i = 0; i < 4; ++i) { for (int j = i + 1; j < 4; ++j) { CHECK(differing(f, faces[i], faces[j]) > 300); } }
    // both arcs and the two bars follow the reading; a style is never a second opinion: B-row (no peak) = A-row for the arcs
    CHECK(differing(f, faceOf(A1), faceOf(B1)) == 0);                       // the peak is ignored by the classic face ...
    CHECK(differing(f, faceOf(A2), faceOf(B2)) == 0);                       // ... and by the analogue one
    CHECK(differing(f, faceOf(A3), faceOf(B3)) > 200);                      // but the ladder and the bar show it
    CHECK(differing(f, faceOf(A4), faceOf(B4)) > 40);

    // ===== classic: a 180 degree arc scale, nine ticks (the last two the alarm colour), a thin pointer on a hub ===
    {
        // The scale is read in polar terms round the pivot, on E1 (no reading, so no pointer to be mistaken for a tick): the
        // nine ticks cross the ring r 40.5..43.5 every 22.5 degrees from -90 to +90, the arc lies just outside it (r 45),
        // and half way between two ticks the ring is empty.
        const Face e1 = faceOf(E1);
        const int epx = e1.x + 63, epy = e1.y + 51;
        const auto ink = [](Rgb c) { return !nearColour(c, kFace, 40); };
        const auto red = [](Rgb c) { return c.r > c.g + 40 && std::abs(int(c.g) - int(c.b)) < 30; };
        for (int i = 0; i < 9; ++i) {
            const double a = (i / 8.0 - 0.5) * 180.0;
            CHECK(sectorCount(f, epx, epy, 40.5, 43.5, a, 4.0, ink) >= 2);                  // a tick here
            const int reds = sectorCount(f, epx, epy, 40.5, 43.5, a, 4.0, red);
            if (i >= 7) { CHECK(reds >= 1); } else { CHECK(reds == 0); }                    // the last two in the alarm colour
        }
        for (int i = 0; i < 8; ++i) { CHECK(sectorCount(f, epx, epy, 40.5, 43.5, ((i + 0.5) / 8.0 - 0.5) * 180.0, 4.0, ink) == 0); }   // none between two
        // the arc: unbroken every 11.25 degrees from the left end to the right end, and not a pixel beyond a half circle
        for (int i = 0; i < 17; ++i) { CHECK(sectorCount(f, epx, epy, 43.5, 46.5, (i / 16.0 - 0.5) * 180.0, 3.0, ink) >= 3); }
        CHECK(sectorCount(f, epx, epy, 40.0, 47.0, -105.0, 8.0, ink) == 0 && sectorCount(f, epx, epy, 40.0, 47.0, 105.0, 8.0, ink) == 0);
        CHECK(sectorCount(f, epx, epy, 47.0, 52.0, 0.0, 85.0, ink) == 0);                   // and none outside it
        // the same unbroken arc on the analogue face; no ticks there (the pointer's own sector, 22.5 degrees, and the wedge's,
        // 67.5 and 90, are left out of the "no ticks" half)
        const int a2px = a2.x + 63, a2py = a2.y + 51;
        for (int i = 0; i < 17; ++i) { CHECK(sectorCount(f, a2px, a2py, 43.5, 46.5, (i / 16.0 - 0.5) * 180.0, 3.0, ink) >= 3); }
        // (a ring a little inside the classic ticks' own: the analogue arc is 1.4px and its soft edge reaches further in)
        for (const int i : {0, 1, 2, 3, 4, 6}) { CHECK(sectorCount(f, a2px, a2py, 40.0, 42.5, (i / 8.0 - 0.5) * 180.0, 4.0, ink) == 0); }
        // ... while the same ring on the classic face does hold a tick at each of those angles
        for (const int i : {0, 1, 2, 3, 4, 6}) { CHECK(sectorCount(f, epx, epy, 40.0, 42.5, (i / 8.0 - 0.5) * 180.0, 4.0, ink) >= 2); }
        // a ladder and a bar have no scale at all
        CHECK(inFace(f, a3, kInk, 30) == 0 && inFace(f, a4, kInk, 30) == 0);
        // the pointer, at 0.62 of 180 degrees = 21.6 degrees right of vertical, through (+7.5, -19) of the pivot
        const int px = a1.x + 63, py = a1.y + 51;
        CHECK(amberArea(f, a1.x + 67, a1.y + 29, a1.x + 75, a1.y + 36) > 6.0);
        CHECK(amberArea(f, a1.x + 20, a1.y + 8, a1.x + 60, a1.y + 46) < 1.0);          // none on the other side
        // the hub: meter-ink, 6px, on the pivot
        CHECK(f.near(px, py, kInk) && f.near(px - 2, py - 2, kInk) && f.near(px + 1, py + 1, kInk));
        CHECK(!f.near(px - 4, py, kInk, 20) && !f.near(px + 3, py, kInk, 20) && !f.near(px, py - 4, kInk, 20));
    }
    // the pointer's direction across the whole scale: left at 0, up at 0.5, right at 1
    {
        const Face c1 = faceOf(C1), c2 = faceOf(C2), c3 = faceOf(C3), c4 = faceOf(C4);
        const double leftArea = amberArea(f, c1.x + 26, c1.y + 46, c1.x + 58, c1.y + 56);
        const double upArea = amberArea(f, c2.x + 55, c2.y + 8, c2.x + 72, c2.y + 48);
        const double rightArea = amberArea(f, c3.x + 68, c3.y + 46, c3.x + 100, c3.y + 56);
        CHECK(leftArea > 40.0 && leftArea < 80.0);                                  // 32px of a 1.8px stroke
        CHECK(upArea > 45.0 && upArea < 85.0);                                      // ~36px of the same
        CHECK(rightArea > 40.0 && rightArea < 80.0);
        CHECK(amberArea(f, c1.x + 55, c1.y + 8, c1.x + 72, c1.y + 44) < 1.0);       // value 0: nothing pointing up
        CHECK(amberArea(f, c1.x + 68, c1.y + 44, c1.x + 110, c1.y + 56) < 1.0);     // ... nor to the right
        CHECK(amberArea(f, c2.x + 20, c2.y + 8, c2.x + 54, c2.y + 56) < 1.0);       // value 0.5: nothing to the left
        CHECK(amberArea(f, c2.x + 73, c2.y + 8, c2.x + 110, c2.y + 56) < 1.0);      // ... nor to the right
        CHECK(amberArea(f, c3.x + 20, c3.y + 44, c3.x + 58, c3.y + 56) < 1.0);      // value 1: nothing to the left
        CHECK(amberArea(f, c3.x + 55, c3.y + 8, c3.x + 72, c3.y + 44) < 1.0);       // ... nor up
        // the analogue pointer is bolder: about 3px against 1.8
        const double boldUp = amberArea(f, c4.x + 55, c4.y + 8, c4.x + 72, c4.y + 47);
        CHECK(boldUp > upArea * 1.4 && boldUp < upArea * 2.1);
        CHECK(boldUp > 85.0 && boldUp < 140.0);
    }

    // ===== analogue: continuous scale, filled alarm wedge over the top 18 %, bolder pointer on a bigger hub ====
    {
        const int px = a2.x + 63, py = a2.y + 51;
        const Rgb wedge = over(kHigh, 0.55, kFace);
        // inside the wedge (75 degrees, radius 40.5) the fill is the alarm colour at 55 %; at 30 degrees there is none
        CHECK(f.near(px + static_cast<int>(std::lround(40.5 * std::sin(1.309))), py - static_cast<int>(std::lround(40.5 * std::cos(1.309))), wedge, 24));
        CHECK(f.near(px + 20, py - 33, kFace, 12));
        CHECK(f.countNear(a2.x + 80, a2.y + 20, a2.x + 112, a2.y + 51, wedge, 24) > 120);   // the sector is ~220px2
        CHECK(f.countNear(a2.x + 1, a2.y + 1, a2.x + 90, a2.y + 55, wedge, 24) == 0);       // and only over the top 18 % of the travel
        CHECK(f.countNear(a1.x + 80, a1.y + 20, a1.x + 112, a1.y + 51, wedge, 24) < 30);   // the classic face has no wedge (two alarm ticks only)
        CHECK(f.near(px, py, kInk) && f.near(px - 2, py - 2, kInk) && f.near(px + 2, py + 1, kInk));   // an 8px hub ...
        CHECK(!f.near(px - 5, py, kInk, 20) && !f.near(px + 4, py, kInk, 20));                        // ... and no wider
    }

    // ===== led-ladder: twelve segments, low / mid / high by position, the catch segment in meter-peak ==========
    checkLadder(f, B3, 0.62, -1);          // 7 lit green, then the caution and alarm segments at 18 %
    checkLadder(f, A3, 0.62, 0.8);         // the same with the catch on segment 9 (round(0.8 x 12) - 1)
    checkLadder(f, F1, 1.0, -1);           // all lit: 7 low, 3 mid, 2 high
    checkLadder(f, F2, 0.0, 0.0);          // nothing lit, the catch on the first segment even at peak 0
    checkLadder(f, E3, 0.62, 0.5, false);  // no reading: nothing lit, the catch alone (segment 5)
    checkLadder(f, D3, 0.5, 0.75);         // with a scale: 6 lit, the catch on segment 8
    {
        const Face b3 = faceOf(B3), f1 = faceOf(F1), f2 = faceOf(F2), d3 = faceOf(D3), e3 = faceOf(E3);
        // a segment is 36px high (24 with the scale's band under it), from y 10
        CHECK(f.countNear(b3.x + 20, b3.y + 1, b3.x + 21, b3.y + 55, kLow) == 36 && f.firstRowNear(b3.x + 20, b3.y + 1, b3.y + 55, kLow) == b3.y + 10);
        CHECK(f.countNear(d3.x + 20, d3.y + 1, d3.x + 21, d3.y + 55, kLow) == 24 && f.firstRowNear(d3.x + 20, d3.y + 1, d3.y + 55, kLow) == d3.y + 10);
        // exact colour budgets
        CHECK(inFace(f, b3, kPeak) == 0);                                           // peak -1: no catch anywhere
        CHECK(inFace(f, faceOf(B3), kLow) >= 1900 && inFace(f, faceOf(B3), kLow) <= 7 * 288);
        CHECK(inFace(f, f1, kMid) >= 3 * 280 && inFace(f, f1, kMid) <= 3 * 288);    // 3 caution segments
        CHECK(inFace(f, f1, kHigh) >= 2 * 280 && inFace(f, f1, kHigh) <= 2 * 288);  // 2 alarm segments
        CHECK(inFace(f, f2, kLow) == 0 && inFace(f, f2, kPeak) >= 250);             // value 0: nothing lit; the catch is there
        CHECK(inFace(f, e3, kLow) == 0 && inFace(f, e3, kMid) == 0 && inFace(f, e3, kPeak) >= 250);   // no reading
        CHECK(f.near(f2.x + 8, f2.y + 28, kPeak) && !f.near(f2.x + 18, f2.y + 28, kPeak, 20));        // and it is the FIRST one
    }

    // ===== peak: a bar on meter-ink at 18 %, zones at 7/12 and 10/12 of its length, a 2px line at the peak ======
    {
        const Face b4 = faceOf(B4), a4f = faceOf(A4), f3 = faceOf(F3), f4 = faceOf(F4);
        const int mid = b4.y + 28;
        // the bar: from x 4 to 122 (118px), 24px high from y 16
        CHECK(f.near(b4.x + 3, mid, kFace) && f.near(b4.x + 4, mid, kLow) && f.near(b4.x + 121, mid, kBarOff) && f.near(b4.x + 122, mid, kFace));
        CHECK(f.countNear(b4.x + 20, b4.y + 1, b4.x + 21, b4.y + 55, kLow) == 24 && f.firstRowNear(b4.x + 20, b4.y + 1, b4.y + 55, kLow) == b4.y + 16);
        CHECK(f.near(b4.x + 100, mid, kBarOff) && f.near(b4.x + 100, b4.y + 16, kBarOff) && f.near(b4.x + 100, b4.y + 39, kBarOff));
        CHECK(f.near(b4.x + 100, b4.y + 15, kFace) && f.near(b4.x + 100, b4.y + 40, kFace));
        // 0.62: green to 7/12 (x 72.8), caution after it up to 0.62 x 118 + 4 (x 77.2), then the empty track
        CHECK(f.near(b4.x + 40, mid, kLow) && f.near(b4.x + 70, mid, kLow));
        CHECK(f.near(b4.x + 74, mid, kMid) && f.near(b4.x + 76, mid, kMid));
        CHECK(f.near(b4.x + 80, mid, kBarOff) && f.near(b4.x + 90, mid, kBarOff));
        CHECK(inFace(f, b4, kHigh) == 0 && inFace(f, b4, kPeak) == 0);              // no alarm yet, and no peak line at -1
        // the peak line at 0.8: x 4 + 0.8 x 118 = 98.4 -> 97..98, 30px tall (the bar and 3px over and under)
        CHECK(f.near(a4f.x + 97, a4f.y + 28, kPeak) && f.near(a4f.x + 98, a4f.y + 28, kPeak) && !f.near(a4f.x + 96, a4f.y + 28, kPeak, 20) && !f.near(a4f.x + 99, a4f.y + 28, kPeak, 20));
        CHECK(f.countNear(a4f.x + 97, a4f.y + 1, a4f.x + 98, a4f.y + 55, kPeak) == 30 && f.firstRowNear(a4f.x + 97, a4f.y + 1, a4f.y + 55, kPeak) == a4f.y + 13);
        // the full bar: green, caution from 72.8, alarm from 102.3, the line at the right end
        CHECK(f.near(f3.x + 20, f3.y + 28, kLow) && f.near(f3.x + 70, f3.y + 28, kLow));
        CHECK(f.near(f3.x + 76, f3.y + 28, kMid) && f.near(f3.x + 98, f3.y + 28, kMid));
        CHECK(f.near(f3.x + 106, f3.y + 28, kHigh) && f.near(f3.x + 118, f3.y + 28, kHigh));
        CHECK(f.near(f3.x + 121, f3.y + 28, kPeak) && f.near(f3.x + 122, f3.y + 28, kPeak));
        CHECK(f.countNear(f3.x + 121, f3.y + 1, f3.x + 122, f3.y + 55, kPeak) == 30);
        // the empty bar: all track, the line at the left end (x 3..4)
        CHECK(inFace(f, f4, kLow) == 0 && f.near(f4.x + 50, f4.y + 28, kBarOff) && f.near(f4.x + 100, f4.y + 28, kBarOff));
        CHECK(f.near(f4.x + 3, f4.y + 28, kPeak) && f.near(f4.x + 4, f4.y + 28, kPeak) && f.near(f4.x + 3, f4.y + 14, kPeak));
    }

    // ===== a scale under the face, a unit in its corner ========================================================
    {
        const Face d1 = faceOf(D1), d2 = faceOf(D2), d3 = faceOf(D3), d4 = faceOf(D4), a3f = faceOf(A3);
        // classic 0 / 5 / 10: under the arc's two ends and its hub, the band y 43..55
        CHECK(f.countInk(d1.x + 22, d1.y + 43, d1.x + 38, d1.y + 55, kFace) > 5);            // "0" at x 30
        CHECK(f.countInk(d1.x + 55, d1.y + 43, d1.x + 71, d1.y + 55, kFace) > 5);            // "5" at 63
        CHECK(f.countInk(d1.x + 88, d1.y + 43, d1.x + 106, d1.y + 55, kFace) > 8);           // "10" at 96
        CHECK(f.countInk(d1.x + 40, d1.y + 43, d1.x + 53, d1.y + 55, kFace) == 0);           // nothing between them
        CHECK(f.countInk(d1.x + 73, d1.y + 43, d1.x + 86, d1.y + 55, kFace) == 0);
        CHECK(f.countNear(d1.x + 22, d1.y + 43, d1.x + 106, d1.y + 55, kDim, 60) > 6);       // in the dim colour
        // analogue -60 / -30 / 0
        CHECK(f.countInk(d2.x + 18, d2.y + 43, d2.x + 42, d2.y + 55, kFace) > 8);
        CHECK(f.countInk(d2.x + 52, d2.y + 43, d2.x + 74, d2.y + 55, kFace) > 8);
        CHECK(f.countInk(d2.x + 88, d2.y + 43, d2.x + 106, d2.y + 55, kFace) > 4);
        // a ladder and a bar: flush under their ends, the middle one centred
        for (const Face& fc : {d3, d4}) {
            CHECK(f.countInk(fc.x + 2, fc.y + 43, fc.x + 24, fc.y + 55, kFace) > 8);         // "-60" flush left at x 4
            CHECK(f.countInk(fc.x + 52, fc.y + 43, fc.x + 74, fc.y + 55, kFace) > 8);        // "-30" centred
            CHECK(f.countInk(fc.x + 108, fc.y + 43, fc.x + 124, fc.y + 55, kFace) > 4);      // "0" flush right at x 122
            CHECK(f.countInk(fc.x + 26, fc.y + 43, fc.x + 50, fc.y + 55, kFace) == 0);
            CHECK(f.countInk(fc.x + 76, fc.y + 43, fc.x + 106, fc.y + 55, kFace) == 0);
        }
        CHECK(f.countInk(a3f.x + 8, a3f.y + 46, a3f.x + 118, a3f.y + 55, kFace) == 0);       // no labels given: no band (the corners round the border in)
        // the unit, top right, in every style that has one
        CHECK(f.countInk(d1.x + 90, d1.y + 3, d1.x + 122, d1.y + 15, kFace) > 8);            // "MS/s"
        CHECK(f.countInk(d2.x + 98, d2.y + 3, d2.x + 122, d2.y + 15, kFace) > 4);            // "dB"
        CHECK(f.countInk(d3.x + 100, d3.y + 3, d3.x + 122, d3.y + 10, kFace) > 4);           // above the segments
        CHECK(f.countInk(d4.x + 100, d4.y + 3, d4.x + 122, d4.y + 12, kFace) > 4);
        CHECK(f.countInk(a3f.x + 100, a3f.y + 3, a3f.x + 122, a3f.y + 10, kFace) == 0);      // none given: none drawn
        // with a scale the dial is smaller (radius 33, pivot 39 down) and the band below it is clear of the hub
        CHECK(f.near(d1.x + 63, d1.y + 39, kInk) && f.near(d2.x + 63, d2.y + 39, kInk));
    }

    // ===== no reading, and disabled =============================================================================
    {
        const Face e1 = faceOf(E1), e2 = faceOf(E2), e3 = faceOf(E3), e4 = faceOf(E4);
        CHECK(amberArea(f, e1.x + 1, e1.y + 1, e1.x + 125, e1.y + 55) < 1.0);                // no needle at rest: a needle at zero would be a reading
        CHECK(f.near(e1.x + 63, e1.y + 51, kDim) && !f.near(e1.x + 63, e1.y + 51, kInk, 20));  // the hub, dim
        CHECK(f.countInk(e1.x + 10, e1.y + 4, e1.x + 116, e1.y + 40, kFace, 40) > 30);       // the scale is still there
        CHECK(differing(f, e1, faceOf(A1)) > 40);
        // disabled: everything at 45 %, so no colour of the face is at full strength, and nothing answers
        CHECK(f.near(e2.x, e2.y + 28, over(kBorder, 0.45, kBg), 8) && !f.near(e2.x, e2.y + 28, kBorder, 8));
        CHECK(inFace(f, e2, kNeedle, 60) == 0 && inFace(f, e2, kInk, 40) == 0);              // a lit needle or hub at full colour: none
        CHECK(f.countInk(e2.x + 10, e2.y + 4, e2.x + 116, e2.y + 52, kFace, 12) > 40);       // but it is drawn
        CHECK(inFace(f, e4, kLow) == 0 && inFace(f, e4, over(kLow, 0.45, kFace), 30) > 1000);   // the bar's green at 45 %
        CHECK(f.countInk(E2.x, E2.y + 77, E2.x + 126, E2.y + 91, kBg, 12) > 6);
        (void)e3;
    }

    // ===== faces of another size: face-w / face-h =====================================================================
    {
        const Face g1 = faceOf(G1, 180, 70), g2 = faceOf(G2, 200, 40);
        const int m1 = g1.y + 35, m2 = g2.y + 20;
        CHECK(f.near(g1.x, m1, kBorder) && f.near(g1.x + 179, m1, kBorder) && f.near(g1.x - 1, m1, kBg) && f.near(g1.x + 180, m1, kBg));
        CHECK(f.near(g1.x + 20, g1.y, kBorder) && f.near(g1.x + 20, g1.y + 69, kBorder) && f.near(g1.x + 20, g1.y - 1, kBg) && f.near(g1.x + 20, g1.y + 70, kBg));
        CHECK(f.near(g2.x, m2, kBorder) && f.near(g2.x + 199, m2, kBorder) && f.near(g2.x - 1, m2, kBg) && f.near(g2.x + 200, m2, kBg));
        CHECK(f.near(g2.x + 20, g2.y, kBorder) && f.near(g2.x + 20, g2.y + 39, kBorder) && f.near(g2.x + 20, g2.y - 1, kBg) && f.near(g2.x + 20, g2.y + 40, kBg));
        // the caption and the value follow the face: centred on its width, the value 4px under it
        CHECK(f.countInk(G1.x + 40, G1.y, G1.x + 140, G1.y + 14, kBg) > 20);
        CHECK(f.countInk(G1.x + 70, g1.y + 74, G1.x + 110, g1.y + 88, kBg) > 12);
        CHECK(f.countInk(G1.x, g1.y + 70, G1.x + 180, g1.y + 74, kBg) == 0);
        // the dial is built on the new size: pivot (90, 65), radius 59, so the pointer at 0.5 rises 52px
        CHECK(f.near(g1.x + 90, g1.y + 65, kInk));
        CHECK(amberArea(f, g1.x + 87, g1.y + 8, g1.x + 94, g1.y + 60) > 70.0 && amberArea(f, g1.x + 87, g1.y + 8, g1.x + 94, g1.y + 60) < 110.0);
        CHECK(amberArea(f, g1.x + 20, g1.y + 8, g1.x + 85, g1.y + 66) < 1.0 && amberArea(f, g1.x + 96, g1.y + 8, g1.x + 170, g1.y + 66) < 1.0);
        // the bar is built on it too: 192px from x 4, 17px high from y 12; full: green to 112, caution to 160, alarm to the end
        CHECK(f.near(g2.x + 3, m2, kFace) && f.near(g2.x + 4, m2, kLow) && f.near(g2.x + 195, m2, kHigh) && f.near(g2.x + 196, m2, kFace));
        CHECK(f.near(g2.x + 60, m2, kLow) && f.near(g2.x + 130, m2, kMid) && f.near(g2.x + 175, m2, kHigh));
        CHECK(f.countNear(g2.x + 20, g2.y + 1, g2.x + 21, g2.y + 39, kLow) == 17 && f.firstRowNear(g2.x + 20, g2.y + 1, g2.y + 39, kLow) == g2.y + 12);
        CHECK(f.near(g2.x + 99, m2, kPeak) && f.near(g2.x + 100, m2, kPeak));                // the line at 0.5: x 4 + 96 -> 99..100
    }

    // ===== 34 languages: the caption's case is the component's, and words too long are elided, not spilled =========
    {
        // "xxxx xxxx" and "XXXX XXXX" are one caption once upper-cased: the same 126 x 14 pixels, whatever typeface this is
        CHECK(differingRects(f, G3.x, G3.y, G4.x, G4.y, 126, 14) == 0);
        CHECK(f.countInk(G3.x, G3.y, G3.x + 126, G3.y + 14, kBg) > 30);               // (and they are ink, not two empty rows)
        // the long ones keep inside the meter's own 126px column, above the face and below it
        const Face g5 = faceOf(G5);
        CHECK(f.countInk(G5.x, G5.y, G5.x + 126, G5.y + 14, kBg) > 60);                // the caption fills its column ...
        CHECK(f.countInk(G5.x - 24, G5.y, G5.x, G5.y + 14, kBg) == 0 && f.countInk(G5.x + 126, G5.y, G5.x + 150, G5.y + 14, kBg) == 0);   // ... and no further
        CHECK(f.countInk(G5.x, g5.y + 60, G5.x + 126, g5.y + 74, kBg) > 60);           // the value likewise
        CHECK(f.countInk(G5.x - 24, g5.y + 60, G5.x, g5.y + 74, kBg) == 0 && f.countInk(G5.x + 126, g5.y + 60, G5.x + 150, g5.y + 74, kBg) == 0);
    }

    // ===== right-click asks for the style menu; nothing else does =============================================
    {
        moveTo(w, 800, 600);
        int n = ui->get_menu_requests();
        const Face fc = faceOf(A1);
        click(w, static_cast<float>(fc.x + 30), static_cast<float>(fc.y + 20), slint::PointerEventButton::Right);
        CHECK(ui->get_menu_requests() == n + 1 && ui->get_last_from() == 1);
        CHECK_NEAR(ui->get_last_menu_x(), 30.0, 1.0);                                       // the pointer, in the meter's own coordinates
        CHECK_NEAR(ui->get_last_menu_y(), 37.0, 1.0);                                       // (the face starts 17px down)
        n = ui->get_menu_requests();
        click(w, static_cast<float>(fc.x + 30), static_cast<float>(fc.y + 20));             // a left click is nothing
        click(w, static_cast<float>(fc.x + 30), static_cast<float>(fc.y + 20), slint::PointerEventButton::Middle);
        CHECK(ui->get_menu_requests() == n);
        press(w, static_cast<float>(fc.x + 50), static_cast<float>(fc.y + 10), slint::PointerEventButton::Right);
        CHECK(ui->get_menu_requests() == n);                                                // asked on release, so a press that becomes a drag is not a menu
        release(w, static_cast<float>(fc.x + 50), static_cast<float>(fc.y + 10), slint::PointerEventButton::Right);
        CHECK(ui->get_menu_requests() == n + 1);
        CHECK_NEAR(ui->get_last_menu_x(), 50.0, 1.0);
        CHECK_NEAR(ui->get_last_menu_y(), 27.0, 1.0);
        // anywhere on the meter: the caption, the value text
        n = ui->get_menu_requests();
        click(w, static_cast<float>(A1.x + 60), static_cast<float>(A1.y + 5), slint::PointerEventButton::Right);
        CHECK(ui->get_menu_requests() == n + 1);
        CHECK_NEAR(ui->get_last_menu_y(), 5.0, 1.0);
        click(w, static_cast<float>(A1.x + 60), static_cast<float>(A1.y + 84), slint::PointerEventButton::Right);
        CHECK(ui->get_menu_requests() == n + 2);
        CHECK_NEAR(ui->get_last_menu_y(), 84.0, 1.0);
        // every meter answers for itself: which one asked, and where in ITSELF
        const std::array<std::pair<Pos, int>, 5> others = {{{A4, 4}, {C2, 10}, {D3, 15}, {F1, 21}, {G1, 25}}};
        for (const auto& [p, id] : others) {
            n = ui->get_menu_requests();
            click(w, static_cast<float>(p.x + 40), static_cast<float>(p.y + 50), slint::PointerEventButton::Right);
            CHECK(ui->get_menu_requests() == n + 1 && ui->get_last_from() == id);
            CHECK_NEAR(ui->get_last_menu_x(), 40.0, 1.0);
            CHECK_NEAR(ui->get_last_menu_y(), 50.0, 1.0);
        }
        // a disabled meter does not ask
        n = ui->get_menu_requests();
        click(w, static_cast<float>(E2.x + 40), static_cast<float>(E2.y + 50), slint::PointerEventButton::Right);
        click(w, static_cast<float>(E4.x + 40), static_cast<float>(E4.y + 50), slint::PointerEventButton::Right);
        CHECK(ui->get_menu_requests() == n);
        // and right-clicking beside a meter (the gap between two) is nobody's
        click(w, static_cast<float>(A1.x + 126 + 12), static_cast<float>(A1.y + 40), slint::PointerEventButton::Right);
        CHECK(ui->get_menu_requests() == n);
        moveTo(w, 800, 600);
    }

    // ===== the hover hint: "Right-click to change the meter style", after the pointer rests; none when disabled ====
    {
        Frame before = render(ui, 880, 670);
        CHECK(tipPixels(before, A1.x, A1.y + 97, A1.x + 280, A1.y + 125) < 40);
        const auto resting = std::chrono::steady_clock::now();
        moveTo(w, static_cast<float>(A1.x + 60), static_cast<float>(A1.y + 40));
        settle(std::chrono::milliseconds(150));
        Frame early = render(ui, 880, 670);
        // it waits 600 ms. (Should this machine have stalled for most of that, the claim is not made - but the check is
        // still counted, never skipped.)
        CHECK(std::chrono::steady_clock::now() - resting >= std::chrono::milliseconds(500) || tipPixels(early, A1.x, A1.y + 97, A1.x + 280, A1.y + 125) < 40);
        settle(std::chrono::milliseconds(700));
        Frame tip = render(ui, 880, 670);
        CHECK(tipPixels(tip, A1.x, A1.y + 97, A1.x + 280, A1.y + 125) > 2500);             // 6px under the meter, from its left edge
        CHECK(tip.near(A1.x + 1, A1.y + 105, kTip, 2) && tip.near(A1.x - 3, A1.y + 105, kBg));
        CHECK(tip.countInk(A1.x + 20, A1.y + 101, A1.x + 230, A1.y + 121, kTip, 40) > 200);   // the words
        tip.savePng(shotDir() + "meters-specimen-tooltip.png");
        moveTo(w, 800, 600);
        settle(std::chrono::milliseconds(50));
        Frame gone = render(ui, 880, 670);
        CHECK(tipPixels(gone, A1.x, A1.y + 97, A1.x + 280, A1.y + 125) < 40);
        // the enabled meter in the bottom row shows it; the disabled one beside it does not
        moveTo(w, static_cast<float>(E1.x + 60), static_cast<float>(E1.y + 40));
        settle(std::chrono::milliseconds(750));
        Frame e1tip = render(ui, 880, 670);
        CHECK(tipPixels(e1tip, E1.x, E1.y + 97, E1.x + 280, E1.y + 125) > 2500);
        moveTo(w, static_cast<float>(E2.x + 60), static_cast<float>(E2.y + 40));
        settle(std::chrono::milliseconds(750));
        Frame e2tip = render(ui, 880, 670);
        CHECK(tipPixels(e2tip, E2.x, E2.y + 97, E2.x + 280, E2.y + 125) < 40);
        moveTo(w, 800, 600);
        settle(std::chrono::milliseconds(50));
    }

    // ===== a second filling reaches every pixel, and no component borrows another's token ====================
    // Workbench's needle IS its caution segment colour (both #f5a524), its face IS the field colour: the checks above cannot
    // tell a token from its look-alike. Here every token the meter reads gets a colour of its own, in one assignment.
    {
        Skin probe = ui->global<Workbench>().get_value();
        probe.meter_face = col(0x20, 0x00, 0x20);
        probe.meter_ink = col(0x00, 0xff, 0xff);
        probe.meter_needle = col(0xff, 0x00, 0x00);
        probe.meter_led_low = col(0x00, 0x00, 0xff);
        probe.meter_led_mid = col(0xff, 0xff, 0x00);
        probe.meter_led_high = col(0xff, 0x00, 0xff);
        probe.meter_peak = col(0xff, 0xff, 0xff);
        probe.line_strong = col(0x80, 0x80, 0x80);
        probe.dim = col(0x00, 0xaa, 0x00);
        ui->global<Theme>().set_skin(probe);
        moveTo(w, 800, 600);
        Frame s = render(ui, 880, 670);
        const Rgb pFace = rgb(0x200020), pInk = rgb(0x00ffff), pNeedle = rgb(0xff0000), pLow = rgb(0x0000ff), pMid = rgb(0xffff00);
        const Rgb pHigh = rgb(0xff00ff), pPeak = rgb(0xffffff), pBorder = rgb(0x808080), pDim = rgb(0x00aa00);
        const Face b3 = faceOf(B3), f1 = faceOf(F1), b4 = faceOf(B4), e1 = faceOf(E1);       // (a1..a4 are the faces above)
        CHECK(s.near(a1.x + 2, a1.y + 28, pFace, 4) && s.near(a1.x, a1.y + 28, pBorder, 4));                 // ground and border
        // the ladder: lit low, the unlit caution and alarm segments at 18 % of their own colours, the catch in meter-peak
        CHECK(s.near(b3.x + 8, b3.y + 28, pLow, 6) && s.near(b3.x + 88, b3.y + 28, over(pMid, 0.18, pFace), 6) && s.near(b3.x + 118, b3.y + 28, over(pHigh, 0.18, pFace), 6));
        CHECK(s.near(f1.x + 88, f1.y + 28, pMid, 6) && s.near(f1.x + 118, f1.y + 28, pHigh, 6));            // all lit: caution, alarm
        CHECK(s.near(a3.x + 98, a3.y + 28, pPeak, 6));                                                         // the catch
        // the bar: low, caution, its empty part (meter-ink at 18 %), the peak line
        CHECK(s.near(b4.x + 40, b4.y + 28, pLow, 6) && s.near(b4.x + 75, b4.y + 28, pMid, 6) && s.near(b4.x + 100, b4.y + 28, over(pInk, 0.18, pFace), 6));
        CHECK(s.near(a4.x + 97, a4.y + 28, pPeak, 6));
        // the dials: pointer, hub, the alarm wedge, the hub without a reading
        CHECK(inFace(s, a1, pNeedle, 60) > 20 && inFace(s, a1, kNeedle, 60) == 0);                            // meter-needle, not the caution colour
        CHECK(s.near(a1.x + 63, a1.y + 51, pInk, 6) && s.near(e1.x + 63, e1.y + 51, pDim, 6));
        CHECK(s.near(a2.x + 63 + 39, a2.y + 51 - 10, over(pHigh, 0.55, pFace), 24));
        s.savePng(shotDir() + "meters-specimen-probe-filling.png");
    }

    return testSummary("test_slint_meters");
}
