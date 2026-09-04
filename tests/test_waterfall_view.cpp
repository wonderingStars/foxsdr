// Tests for gui/waterfall_view.hpp — everything here is headless: only the
// colormap, the dB->pixel row conversion, the CPU ring, the seam/window uv
// math (uvRects), the two axis ladders (timeLabelStep, dbLabelStep) and the
// written-history count (filledRows) are exercised; the draw() overloads (the
// GL-touching members) are never called, so no GL context is needed. Expected colors are
// derived in-test from the documented contract (anchor RGBA constants, the
// normalization formula, the nearest-resampling rule
// floor((x + 0.5) * n / texWidth)) — never read back from internals — and
// the uv expectations come from the documented seam-split contract.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/waterfall_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "test_check.hpp"

using cascade::gui::mapLineToPixels;
using cascade::gui::WaterfallView;
using cascade::gui::waterfallColor;

namespace {

int chR(ImU32 c) { return static_cast<int>((c >> IM_COL32_R_SHIFT) & 0xFF); }
int chG(ImU32 c) { return static_cast<int>((c >> IM_COL32_G_SHIFT) & 0xFF); }
int chB(ImU32 c) { return static_cast<int>((c >> IM_COL32_B_SHIFT) & 0xFF); }
int chA(ImU32 c) { return static_cast<int>((c >> IM_COL32_A_SHIFT) & 0xFF); }

// Perceived brightness per the contract: 0.299R + 0.587G + 0.114B.
double lumaOf(ImU32 c) {
    return 0.299 * chR(c) + 0.587 * chG(c) + 0.114 * chB(c);
}

// --- waterfallColor -------------------------------------------------------

void testColorEndpoints() {
    // Documented contract anchors, exact RGBA.
    // THE ANCHORS MOVED WITH THE PALETTE, DELIBERATELY. The map was a
    // blue-to-red jet ramp and is now phosphor green to cream, because the
    // waterfall, the spectrum and the radar scope are meant to read as one
    // tube. What did NOT move is the property these anchors are pinned for:
    // the table is still brightness-monotonic, which the checks below still
    // prove across all 256 entries. Changing the hues is a design decision;
    // breaking monotonicity would be breaking the measurement.
    CHECK(waterfallColor(0.0f) == IM_COL32(6, 20, 10, 255));
    CHECK(waterfallColor(1.0f) == IM_COL32(240, 235, 180, 255));
}

void testColorClamping() {
    CHECK(waterfallColor(-0.001f) == waterfallColor(0.0f));
    CHECK(waterfallColor(-123.0f) == waterfallColor(0.0f));
    CHECK(waterfallColor(1.001f) == waterfallColor(1.0f));
    CHECK(waterfallColor(50.0f) == waterfallColor(1.0f));
}

void testColorMonotonicBrightness() {
    // Non-decreasing perceived brightness across all 256 steps, and every
    // entry fully opaque (the waterfall texture must never blend with the
    // panel behind it). Tiny epsilon absorbs double-vs-float luma rounding;
    // any real inversion (a jet-style yellow->red dip is tens of units) is
    // far outside it.
    double prev = lumaOf(waterfallColor(0.0f));
    for (int i = 0; i < 256; ++i) {
        const ImU32 c = waterfallColor(static_cast<float>(i) / 255.0f);
        const double luma = lumaOf(c);
        CHECK(luma >= prev - 1e-3);
        CHECK(chA(c) == 255);
        prev = luma;
    }
    // Hue milestones of the documented PHOSPHOR progression: quiet green ->
    // phosphor -> yellow-green -> cream (loose inequalities, not exact
    // mid-table constants).
    //
    // ALL FOUR WERE REWRITTEN when the palette changed, not just the one that
    // failed. Only the cold check went red on the new table; the other three
    // still passed, but by accident rather than because they described the
    // ramp - a green-to-cream map happens to satisfy "R > B at the warm end"
    // as readily as a yellow-to-red one did. A test that keeps passing for the
    // wrong reason is worse than one that fails, because nothing will ever
    // draw attention to it again.
    //
    // GREEN LEADS FOR THE WHOLE COLD HALF, which is the actual claim "this is
    // a phosphor ramp" makes, and it is the thing a future palette edit would
    // most plausibly break.
    const ImU32 cold = waterfallColor(0.0f);
    CHECK(chG(cold) > chR(cold) && chG(cold) > chB(cold));  // quiet green
    const ImU32 mid = waterfallColor(0.45f);
    CHECK(chG(mid) > chR(mid) && chG(mid) > chB(mid));      // phosphor
    const ImU32 warm = waterfallColor(0.75f);
    CHECK(chG(warm) > chB(warm) && chR(warm) > chB(warm));  // yellow-green
    const ImU32 hot = waterfallColor(1.0f);
    // Warm cream: red and green both well clear of blue, and - the part that
    // matters - it is the brightest entry, which the monotone sweep above has
    // already proved across all 256 entries.
    CHECK(chR(hot) > chB(hot) && chG(hot) > chB(hot));
    CHECK(lumaOf(hot) > lumaOf(warm));
}

// --- mapLineToPixels ------------------------------------------------------

// dB value whose normalized position in [dbMin, dbMax] is `norm`.
float dbFor(float norm, float dbMin, float dbMax) {
    return dbMin + norm * (dbMax - dbMin);
}

void testMapIdentity() {
    // n == texWidth must be an identity mapping: pixel x reads bin x
    // (floor((x + 0.5) * n / n) == x). Norms are multiples of 1/15 so the
    // LUT index (norm * 255 = 17k) sits far from a rounding boundary.
    constexpr int kN = 16;
    const float dbMin = -100.0f;
    const float dbMax = 0.0f;
    float bins[kN];
    for (int i = 0; i < kN; ++i) {
        bins[i] = dbFor(static_cast<float>(i) / 15.0f, dbMin, dbMax);
    }
    ImU32 dst[kN] = {};
    mapLineToPixels(bins, kN, dbMin, dbMax, dst, kN);
    for (int x = 0; x < kN; ++x) {
        CHECK(dst[x] == waterfallColor(static_cast<float>(x) / 15.0f));
    }
    // Out-of-range dB values clamp to the endpoints rather than wrapping.
    float extremes[2] = {dbMin - 40.0f, dbMax + 40.0f};
    ImU32 dst2[2] = {};
    mapLineToPixels(extremes, 2, dbMin, dbMax, dst2, 2);
    CHECK(dst2[0] == waterfallColor(0.0f));
    CHECK(dst2[1] == waterfallColor(1.0f));
}

void testMapUpsample() {
    // n = 4 bins onto texWidth = 8: source bin = floor((x + 0.5) * 4 / 8)
    // = floor((x + 0.5) / 2), i.e. the pattern 0,0,1,1,2,2,3,3.
    const float dbMin = -120.0f;
    const float dbMax = -20.0f;
    float bins[4];
    for (int i = 0; i < 4; ++i) {
        bins[i] = dbFor(static_cast<float>(i) / 3.0f, dbMin, dbMax);
    }
    ImU32 dst[8] = {};
    mapLineToPixels(bins, 4, dbMin, dbMax, dst, 8);
    const int expectedSrc[8] = {0, 0, 1, 1, 2, 2, 3, 3};
    for (int x = 0; x < 8; ++x) {
        CHECK(dst[x] == waterfallColor(static_cast<float>(expectedSrc[x]) / 3.0f));
    }
}

void testMapDownsample() {
    // n = 8 bins onto texWidth = 4: source bin = floor((x + 0.5) * 8 / 4)
    // = 2x + 1 — the documented pixel-center rule picks the odd bins, and
    // the even bins must not leak through.
    const float dbMin = -90.0f;
    const float dbMax = -10.0f;
    float bins[8];
    for (int i = 0; i < 8; ++i) {
        bins[i] = dbFor(static_cast<float>(i) / 7.0f, dbMin, dbMax);
    }
    ImU32 dst[4] = {};
    mapLineToPixels(bins, 8, dbMin, dbMax, dst, 4);
    for (int x = 0; x < 4; ++x) {
        CHECK(dst[x] == waterfallColor(static_cast<float>(2 * x + 1) / 7.0f));
    }
}

void testMapUniformInput() {
    // All-equal input maps to one uniform color at the right norm.
    const float dbMin = -100.0f;
    const float dbMax = 0.0f;
    const float level = dbFor(6.0f / 15.0f, dbMin, dbMax);  // -60 dB
    std::vector<float> bins(11, level);
    ImU32 dst[7] = {};
    mapLineToPixels(bins.data(), 11, dbMin, dbMax, dst, 7);
    const ImU32 expected = waterfallColor(6.0f / 15.0f);
    for (int x = 0; x < 7; ++x) {
        CHECK(dst[x] == expected);
    }
}

void testMapDegenerateRange() {
    // dbMin == dbMax must not divide by zero: the documented behavior is a
    // uniform floor-color row. Inverted bounds are the same degenerate case.
    float bins[5] = {-50.0f, -40.0f, -30.0f, -20.0f, -10.0f};
    ImU32 dst[5];
    for (int i = 0; i < 5; ++i) {
        dst[i] = 0xDEADBEEFu;
    }
    mapLineToPixels(bins, 5, -30.0f, -30.0f, dst, 5);
    for (int x = 0; x < 5; ++x) {
        CHECK(dst[x] == waterfallColor(0.0f));
    }
    mapLineToPixels(bins, 5, -10.0f, -90.0f, dst, 5);  // inverted range
    for (int x = 0; x < 5; ++x) {
        CHECK(dst[x] == waterfallColor(0.0f));
    }
    // n <= 0 / null bins: defined as a floor-color row, never a crash.
    mapLineToPixels(nullptr, 0, -100.0f, 0.0f, dst, 5);
    for (int x = 0; x < 5; ++x) {
        CHECK(dst[x] == waterfallColor(0.0f));
    }
}

// --- WaterfallView CPU ring -----------------------------------------------

// Pushes a line of uniform level `norm` (within [dbMin, dbMax]) into wf.
void addUniformLine(WaterfallView& wf, float norm) {
    const float dbMin = -100.0f;
    const float dbMax = 0.0f;
    std::vector<float> bins(static_cast<std::size_t>(wf.texWidth()),
                            dbFor(norm, dbMin, dbMax));
    wf.addLine(bins.data(), wf.texWidth(), dbMin, dbMax);
}

bool rowIsUniform(const WaterfallView& wf, int row, ImU32 expected) {
    const ImU32* px = wf.rowPixels(row);
    if (px == nullptr) {
        return false;
    }
    for (int x = 0; x < wf.texWidth(); ++x) {
        if (px[x] != expected) {
            return false;
        }
    }
    return true;
}

void testRingInitialState() {
    WaterfallView wf(8, 4);
    CHECK(wf.texWidth() == 8);
    CHECK(wf.texHeight() == 4);
    CHECK(wf.rowCursor() == 0);
    // Fresh ring is pre-filled with the coldest color (empty waterfall).
    for (int r = 0; r < 4; ++r) {
        CHECK(rowIsUniform(wf, r, waterfallColor(0.0f)));
    }
    CHECK(wf.rowPixels(-1) == nullptr);
    CHECK(wf.rowPixels(4) == nullptr);
}

void testRingCursorWrap() {
    // The cursor decrements (newest row walks up the texture, wrapped), so
    // after k lines it must sit at (H - k) mod H: 3, 2, 1, 0, then wrap to 3.
    WaterfallView wf(8, 4);
    addUniformLine(wf, 1.0f / 15.0f);
    CHECK(wf.rowCursor() == 3);
    addUniformLine(wf, 2.0f / 15.0f);
    CHECK(wf.rowCursor() == 2);
    addUniformLine(wf, 3.0f / 15.0f);
    CHECK(wf.rowCursor() == 1);
    addUniformLine(wf, 4.0f / 15.0f);
    CHECK(wf.rowCursor() == 0);  // exactly height lines -> back at the start
    addUniformLine(wf, 5.0f / 15.0f);
    CHECK(wf.rowCursor() == 3);  // wrapped

    // Line 5 must have overwritten line 1's row (the oldest), and lines
    // 2..4 must still be intact in their rows — the wrap recycles exactly
    // one row per line.
    CHECK(rowIsUniform(wf, 3, waterfallColor(5.0f / 15.0f)));
    CHECK(rowIsUniform(wf, 2, waterfallColor(2.0f / 15.0f)));
    CHECK(rowIsUniform(wf, 1, waterfallColor(3.0f / 15.0f)));
    CHECK(rowIsUniform(wf, 0, waterfallColor(4.0f / 15.0f)));

    // Newest-to-oldest must read cursor, cursor+1, ... (mod H): that order
    // is what the wrapped-v draw shows top-to-bottom.
    const int c = wf.rowCursor();
    const float ages[4] = {5.0f, 4.0f, 3.0f, 2.0f};  // newest .. oldest
    for (int k = 0; k < 4; ++k) {
        CHECK(rowIsUniform(wf, (c + k) % 4, waterfallColor(ages[k] / 15.0f)));
    }
}

void testRingResampledAndDegenerateLines() {
    // addLine resamples through the same documented rule: 16 bins onto an
    // 8-wide ring picks bins 2x+1.
    WaterfallView wf(8, 3);
    const float dbMin = -100.0f;
    const float dbMax = 0.0f;
    float bins[16];
    for (int i = 0; i < 16; ++i) {
        bins[i] = dbFor(static_cast<float>(i) / 15.0f, dbMin, dbMax);
    }
    wf.addLine(bins, 16, dbMin, dbMax);
    const ImU32* px = wf.rowPixels(wf.rowCursor());
    CHECK(px != nullptr);
    for (int x = 0; x < 8; ++x) {
        CHECK(px[x] == waterfallColor(static_cast<float>(2 * x + 1) / 15.0f));
    }
    // A null/empty line writes a floor-color row (and still advances the
    // ring — a dropout scrolls through instead of freezing the display).
    wf.addLine(nullptr, 0, dbMin, dbMax);
    CHECK(rowIsUniform(wf, wf.rowCursor(), waterfallColor(0.0f)));

    // Inert view: non-positive dimensions never crash and never advance.
    WaterfallView empty(0, 0);
    empty.addLine(bins, 16, dbMin, dbMax);
    CHECK(empty.rowCursor() == 0);
    CHECK(empty.rowPixels(0) == nullptr);
}

// --- uvRects (windowed-draw seam math) --------------------------------------

using UvRect = WaterfallView::UvRect;

void checkRect(const UvRect& r, float y0, float y1, float u0, float v0, float u1,
               float v1, double tol) {
    CHECK_NEAR(r.y0Frac, y0, tol);
    CHECK_NEAR(r.y1Frac, y1, tol);
    CHECK_NEAR(r.u0, u0, tol);
    CHECK_NEAR(r.v0, v0, tol);
    CHECK_NEAR(r.u1, u1, tol);
    CHECK_NEAR(r.v1, v1, tol);
}

void testUvNoSeam() {
    // Cursor 0: ring start coincides with the texture top — one quad, v
    // spans [0, 1], the u-window passes straight through. All the values
    // here are exact binary fractions, so tolerance 0.
    UvRect r[2];
    CHECK(WaterfallView::uvRects(0, 64, 0.25, 0.75, r) == 1);
    checkRect(r[0], 0.0f, 1.0f, 0.25f, 0.0f, 0.75f, 1.0f, 0.0);
    // Full window at cursor 0 is the identity rect of the full-span draw.
    CHECK(WaterfallView::uvRects(0, 64, 0.0, 1.0, r) == 1);
    checkRect(r[0], 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0);
}

void testUvSeam() {
    // Cursor 16 of 64: seam at v = 16/64 = 0.25. Per the contract, the
    // newest rows 16..63 (v in [0.25, 1]) render on top and must cover
    // 48/64 = 0.75 of the widget height; the oldest rows 0..15 fill the
    // rest. Expectations derived from the contract, never read back.
    UvRect r[2];
    CHECK(WaterfallView::uvRects(16, 64, 0.0, 1.0, r) == 2);
    checkRect(r[0], 0.0f, 0.75f, 0.0f, 0.25f, 1.0f, 1.0f, 0.0);
    checkRect(r[1], 0.75f, 1.0f, 0.0f, 0.0f, 1.0f, 0.25f, 0.0);

    // The u-window must pass through both quads unchanged — zooming
    // horizontally cannot move the seam or the vertical split.
    CHECK(WaterfallView::uvRects(16, 64, 0.25, 0.75, r) == 2);
    checkRect(r[0], 0.0f, 0.75f, 0.25f, 0.25f, 0.75f, 1.0f, 0.0);
    checkRect(r[1], 0.75f, 1.0f, 0.25f, 0.0f, 0.75f, 0.25f, 0.0);

    // Extreme cursors: 1 and H-1 still split into two exact quads (1/64 and
    // 63/64 are exact in float, so tolerance stays 0).
    CHECK(WaterfallView::uvRects(1, 64, 0.0, 1.0, r) == 2);
    checkRect(r[0], 0.0f, 63.0f / 64.0f, 0.0f, 1.0f / 64.0f, 1.0f, 1.0f, 0.0);
    checkRect(r[1], 63.0f / 64.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f / 64.0f, 0.0);
    CHECK(WaterfallView::uvRects(63, 64, 0.0, 1.0, r) == 2);
    checkRect(r[0], 0.0f, 1.0f / 64.0f, 0.0f, 63.0f / 64.0f, 1.0f, 1.0f, 0.0);
    checkRect(r[1], 1.0f / 64.0f, 1.0f, 0.0f, 0.0f, 1.0f, 63.0f / 64.0f, 0.0);

    // Out-of-range cursors wrap defensively: -1 must equal 3 (mod 4).
    UvRect a[2];
    UvRect b[2];
    CHECK(WaterfallView::uvRects(-1, 4, 0.0, 1.0, a) == 2);
    CHECK(WaterfallView::uvRects(3, 4, 0.0, 1.0, b) == 2);
    for (int i = 0; i < 2; ++i) {
        checkRect(a[i], b[i].y0Frac, b[i].y1Frac, b[i].u0, b[i].v0, b[i].u1, b[i].v1, 0.0);
    }
}

void testUvClampAndDegenerate() {
    UvRect r[2];
    // Window clamped into the texture: [-0.5, 1.5] reads as [0, 1].
    CHECK(WaterfallView::uvRects(0, 8, -0.5, 1.5, r) == 1);
    checkRect(r[0], 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0);
    // Degenerate windows draw nothing (0 quads) — never UB, never a
    // silently-full window: zero width, inverted, entirely outside on
    // either side, degenerate-after-clamping, NaN.
    CHECK(WaterfallView::uvRects(0, 8, 0.5, 0.5, r) == 0);
    CHECK(WaterfallView::uvRects(3, 8, 0.7, 0.3, r) == 0);
    CHECK(WaterfallView::uvRects(0, 8, -2.0, -1.0, r) == 0);
    CHECK(WaterfallView::uvRects(0, 8, 1.2, 3.0, r) == 0);
    CHECK(WaterfallView::uvRects(0, 8, -1.0, 0.0, r) == 0);  // clamps to [0, 0]
    CHECK(WaterfallView::uvRects(0, 8, 1.0, 2.0, r) == 0);   // clamps to [1, 1]
    const float qnan = std::nanf("");
    CHECK(WaterfallView::uvRects(0, 8, static_cast<double>(qnan), 1.0, r) == 0);
    CHECK(WaterfallView::uvRects(0, 8, 0.0, static_cast<double>(qnan), r) == 0);
    // Inert inputs: non-positive height, null output.
    CHECK(WaterfallView::uvRects(0, 0, 0.0, 1.0, r) == 0);
    CHECK(WaterfallView::uvRects(0, -3, 0.0, 1.0, r) == 0);
    CHECK(WaterfallView::uvRects(0, 8, 0.0, 1.0, nullptr) == 0);
}

void testUvInvariantsSweep() {
    // Property sweep with a fixed-seed LCG: for any cursor/height/window the
    // quads must tile the widget top-to-bottom with no gap, keep each quad's
    // screen share equal to its v share (uniform history-row thickness —
    // the invariant that makes the seam invisible), pass the u-window
    // through untouched, and cover v as [seam, 1] on top then [0, seam].
    std::uint32_t lcg = 0xBADC0DE5u;
    auto next01 = [&lcg]() {
        lcg = lcg * 1664525u + 1013904223u;
        return static_cast<double>(lcg >> 8) / 16777216.0;
    };
    const int heights[] = {1, 2, 7, 64, 480};
    for (int trial = 0; trial < 200; ++trial) {
        const int h = heights[trial % 5];
        const int cur = static_cast<int>(next01() * static_cast<double>(h));
        // Window construction guarantees 0 <= a < b <= 1, so no trial is
        // ever skipped (a silent skip would shrink the sweep unnoticed).
        const double a = 0.9 * next01();
        const double b = a + 0.05 + (1.0 - a - 0.05) * next01();
        UvRect r[2];
        const int rectCount = WaterfallView::uvRects(cur, h, a, b, r);
        CHECK(rectCount == (cur == 0 ? 1 : 2));
        if (rectCount < 1) { continue; }  // already failed above; avoid UB reads
        CHECK_NEAR(r[0].y0Frac, 0.0f, 0.0);
        CHECK_NEAR(r[rectCount - 1].y1Frac, 1.0f, 0.0);
        double vCovered = 0.0;
        for (int i = 0; i < rectCount; ++i) {
            CHECK_NEAR(r[i].u0, a, 1e-6);
            CHECK_NEAR(r[i].u1, b, 1e-6);
            CHECK_NEAR(r[i].y1Frac - r[i].y0Frac, r[i].v1 - r[i].v0, 1e-6);
            vCovered += static_cast<double>(r[i].v1) - static_cast<double>(r[i].v0);
        }
        CHECK_NEAR(vCovered, 1.0, 1e-6);
        if (rectCount == 2) {
            CHECK_NEAR(r[0].y1Frac, r[1].y0Frac, 0.0);  // contiguous, no gap
            CHECK_NEAR(r[0].v0, static_cast<double>(cur) / static_cast<double>(h), 1e-6);
            CHECK_NEAR(r[0].v1, 1.0, 0.0);
            CHECK_NEAR(r[1].v0, 0.0, 0.0);
            CHECK_NEAR(r[1].v1, static_cast<double>(cur) / static_cast<double>(h), 1e-6);
        } else {
            CHECK_NEAR(r[0].v0, 0.0, 0.0);
            CHECK_NEAR(r[0].v1, 1.0, 0.0);
        }
    }
}

// --- timeLabelStep (the elapsed-time strip's ladder) ------------------------
//
// THE LADDER IS RESTATED HERE, not reached for in the implementation. A test
// that reads the table it is checking cannot notice the table changing, and
// the table IS the contract: the header names these rungs one by one.

const double kTimeLadder[] = {1.0,  2.0,   5.0,   10.0,  15.0,  20.0,   30.0,
                              60.0, 120.0, 300.0, 600.0, 900.0, 1800.0, 3600.0};
constexpr int kTimeLadderN = 14;

bool onTimeLadder(double s) {
    for (int i = 0; i < kTimeLadderN; ++i) {
        if (s == kTimeLadder[i]) {
            return true;
        }
    }
    return false;
}

// Independent statement of the documented rule: the smallest rung at least
// span/maxLabels, whole hours past the top of the ladder, 0 where there is no
// answer.
double refTimeStep(double span, int maxLabels) {
    if (!(span > 0.0) || maxLabels < 1) {
        return 0.0;
    }
    const double want = span / static_cast<double>(maxLabels);
    for (int i = 0; i < kTimeLadderN; ++i) {
        if (kTimeLadder[i] >= want) {
            return kTimeLadder[i];
        }
    }
    return std::ceil(want / 3600.0) * 3600.0;
}

// How many figures drawTimeStrip's own loop would put down the edge for this
// step: k = 1, 2, ... while k * step <= span (waterfall_view.cpp's label
// loop). The cap exists so a runaway is REPORTED by the caller rather than
// hanging the suite — a silently truncated count would turn "hundreds of
// labels" into a passing test.
constexpr int kLabelCountCap = 4096;
int labelsDrawn(double span, double step) {
    if (!(step > 0.0)) {
        return 0;
    }
    int n = 0;
    while (n < kLabelCountCap && static_cast<double>(n + 1) * step <= span) {
        ++n;
    }
    return n;
}

void testTimeLabelStepLadderBoundaries() {
    // A step function is all boundaries. maxLabels == 1 makes want == span
    // exactly, so these land ON each transition rather than near it: the rung
    // is taken when want EQUALS it (>= not >), which is the off-by-one that
    // would otherwise hand every exact span the next rung up and halve the
    // number of figures on the axis.
    for (int i = 0; i < kTimeLadderN; ++i) {
        const double s = kTimeLadder[i];
        CHECK(WaterfallView::timeLabelStep(s, 1) == s);
        CHECK(WaterfallView::timeLabelStep(std::nextafter(s, 0.0), 1) == s);
        const double above = std::nextafter(s, 1.0e300);
        const double wantAbove = (i + 1 < kTimeLadderN) ? kTimeLadder[i + 1]
                                                        : std::ceil(above / 3600.0) * 3600.0;
        CHECK(WaterfallView::timeLabelStep(above, 1) == wantAbove);
    }
    // The same boundaries approached through maxLabels rather than span. Four
    // rungs' worth of history asked for in four labels is exactly that rung;
    // one ulp more is the next one up. (4 * s and its quarter are exact in
    // binary, so these sit on the boundary and not beside it.)
    for (int i = 0; i + 1 < kTimeLadderN; ++i) {
        const double s = kTimeLadder[i];
        CHECK(WaterfallView::timeLabelStep(4.0 * s, 4) == s);
        CHECK(WaterfallView::timeLabelStep(std::nextafter(4.0 * s, 1.0e300), 4) ==
              kTimeLadder[i + 1]);
    }
}

void testTimeLabelStepDegenerate() {
    const double qnan = std::nan("");
    // No span is no axis — and the NaN case is the one that matters, because
    // a NaN reaching the ladder compares false against every rung and would
    // otherwise fall out of the bottom as "1s" beside a picture of nothing.
    CHECK(WaterfallView::timeLabelStep(0.0, 4) == 0.0);
    CHECK(WaterfallView::timeLabelStep(-1.0, 4) == 0.0);
    CHECK(WaterfallView::timeLabelStep(-1.0e9, 4) == 0.0);
    CHECK(WaterfallView::timeLabelStep(qnan, 4) == 0.0);
    // maxLabels < 1: a strip with no room for a figure gets no step.
    CHECK(WaterfallView::timeLabelStep(60.0, 0) == 0.0);
    CHECK(WaterfallView::timeLabelStep(60.0, -3) == 0.0);
    CHECK(WaterfallView::timeLabelStep(60.0, -1000000) == 0.0);
    CHECK(WaterfallView::timeLabelStep(qnan, 0) == 0.0);

    // AN INFINITE SPAN is not in the documented contract and cannot arrive
    // through drawTimeStrip (its span is a finite row count over a rate it has
    // already proved positive and finite), so what is asserted here is only
    // the property that holds whichever way it is answered: the step is either
    // "no axis" or at least the spacing that was asked for. Pinning the value
    // would freeze an undocumented answer.
    //
    // MEASURED, for the record: it currently returns inf for every budget,
    // which is NOT the "0" the non-positive/NaN cases give. If a span of inf
    // could ever reach drawTimeStrip, its `step > span` guard would pass
    // (inf > inf is false) and its label loop would then never terminate,
    // since k * inf <= inf holds for every k.
    const double dinf = std::numeric_limits<double>::infinity();
    for (int maxLabels = 1; maxLabels <= 8; ++maxLabels) {
        const double step = WaterfallView::timeLabelStep(dinf, maxLabels);
        CHECK(step == 0.0 || !(step < dinf / static_cast<double>(maxLabels)));
    }
}

void testTimeLabelStepNarrowAndWide() {
    // A HISTORY UNDER A SECOND. The ladder stops at 1 s, so the step comes
    // back larger than the whole span and drawTimeStrip's `step > span` guard
    // then draws no strip at all. That pair — a floor on the ladder plus the
    // caller's guard — is what the header means by "no labels at all rather
    // than ones rounded to 1s", and it is only true of the pair.
    const double shortSpans[] = {0.001, 0.05, 0.25, 0.5, 0.999};
    for (const double span : shortSpans) {
        const double step = WaterfallView::timeLabelStep(span, 8);
        CHECK(step == 1.0);
        CHECK(step > span);
        CHECK(labelsDrawn(span, step) == 0);
    }
    // One second exactly is the first history that carries a figure.
    CHECK(WaterfallView::timeLabelStep(1.0, 8) == 1.0);
    CHECK(labelsDrawn(1.0, WaterfallView::timeLabelStep(1.0, 8)) == 1);

    // A HISTORY OF DAYS. The strip must still carry at most the number of
    // figures it has room for, never a column of hundreds: 8 labels is 8
    // labels whether the ring holds ten seconds or eleven days.
    const double longSpans[] = {3600.0, 86400.0, 86400.0 * 30.0, 1.0e9};
    for (const double span : longSpans) {
        // From two labels up — the fewest drawTimeStrip ever asks for.
        for (int maxLabels = 2; maxLabels <= 8; ++maxLabels) {
            const double step = WaterfallView::timeLabelStep(span, maxLabels);
            CHECK(step > 0.0);
            const int drawn = labelsDrawn(span, step);
            CHECK(drawn <= maxLabels);
            CHECK(drawn >= 1);
            CHECK(drawn < kLabelCountCap);  // the count was not truncated
        }
    }
    // ONE label is the budget where the whole-hour rounding can overshoot:
    // 1e9 s is 277777.8 hours, rounded up to 277778, which is longer than the
    // history itself — so the step no longer fits and drawTimeStrip drops the
    // strip. drawTimeStrip never asks for fewer than two labels (it clamps
    // with std::max(2, ...)), so this is pinned as behaviour, not relied on.
    CHECK(WaterfallView::timeLabelStep(1.0e9, 1) == 1000000800.0);
    CHECK(WaterfallView::timeLabelStep(1.0e9, 1) > 1.0e9);
    CHECK(labelsDrawn(1.0e9, WaterfallView::timeLabelStep(1.0e9, 1)) == 0);
    CHECK(labelsDrawn(1.0e9, WaterfallView::timeLabelStep(1.0e9, 2)) >= 1);
    // Past the top of the ladder the step is whole hours, and it is the
    // ROUNDED-UP hour, so the label count never exceeds what was asked for.
    CHECK(WaterfallView::timeLabelStep(7200.0, 1) == 7200.0);
    CHECK(WaterfallView::timeLabelStep(3601.0, 1) == 7200.0);
    CHECK(WaterfallView::timeLabelStep(9000.0, 1) == 10800.0);  // 2.5 h -> 3 h
    CHECK(WaterfallView::timeLabelStep(86400.0, 8) == 10800.0);  // 3 h x 8 = a day
}

void testTimeLabelStepSweep() {
    // Geometric sweep from a millisecond to thirty years against the
    // independent reference, checking the three properties the axis rests on
    // at every point: the answer is the reference's, it never puts more
    // figures on the strip than were asked for, and it is always a rung of
    // the ladder or a whole number of hours (never an arbitrary interval like
    // "37 s", which is what a mis-ordered ladder produces).
    int mismatches = 0;
    int tooManyLabels = 0;
    int offLadder = 0;
    int notMonotone = 0;
    int examined = 0;
    for (int maxLabels = 1; maxLabels <= 8; ++maxLabels) {
        double prevStep = 0.0;
        for (int i = 0; i <= 720; ++i) {
            const double span = std::pow(10.0, -3.0 + 0.0175 * static_cast<double>(i));
            const double step = WaterfallView::timeLabelStep(span, maxLabels);
            const double want = refTimeStep(span, maxLabels);
            ++examined;
            if (step != want) {
                if (mismatches == 0) {
                    std::printf("  timeLabelStep(%.6g, %d) = %.6g, want %.6g\n", span,
                                maxLabels, step, want);
                }
                ++mismatches;
            }
            if (labelsDrawn(span, step) > maxLabels) {
                ++tooManyLabels;
            }
            if (!onTimeLadder(step) && std::fmod(step, 3600.0) != 0.0) {
                ++offLadder;
            }
            if (step < prevStep) {  // a longer history can never get a finer step
                ++notMonotone;
            }
            prevStep = step;
        }
    }
    CHECK(examined == 8 * 721);
    CHECK(mismatches == 0);
    CHECK(tooManyLabels == 0);
    CHECK(offLadder == 0);
    CHECK(notMonotone == 0);
}

// --- dbLabelStep (the strength key's dB scale) ------------------------------

const double kDbLadderRef[] = {1.0, 2.0, 5.0, 10.0, 20.0, 25.0, 50.0, 100.0, 200.0};
constexpr int kDbLadderN = 9;

bool onDbLadder(double s) {
    for (int i = 0; i < kDbLadderN; ++i) {
        if (s == kDbLadderRef[i]) {
            return true;
        }
    }
    return false;
}

// HOW MANY FIGURES ACTUALLY REACH THE SCALE, asked the way the scale is drawn
// rather than derived from the span: the first tick sits on the first multiple
// of the step at or above the floor, and the last is kept if it is within
// 1e-6 dB of the ceiling (kDbTickEps in waterfall_view.cpp — the same
// tolerance layoutDbTicks uses, so "fits" and "drawn" cannot disagree).
constexpr int kDbTickCap = 4096;
int dbTicksInRange(double lo, double hi, double step) {
    if (!(step > 0.0) || !(hi > lo)) {
        return 0;
    }
    const double first = std::ceil(lo / step) * step;
    int n = 0;
    while (n < kDbTickCap && first + static_cast<double>(n) * step <= hi + 1.0e-6) {
        ++n;
    }
    return n;
}

// Independent statement of the documented rule, including the exception.
double refDbStep(double lo, double hi, int maxIntervals) {
    const double range = hi - lo;
    if (!(range > 0.0) || maxIntervals < 1) {
        return 0.0;
    }
    const double want = range / static_cast<double>(maxIntervals);
    double chosen = 0.0;
    for (int i = 0; i < kDbLadderN; ++i) {
        if (kDbLadderRef[i] >= want) {
            chosen = kDbLadderRef[i];
            break;
        }
    }
    if (!(chosen > 0.0)) {
        chosen = std::ceil(want / 500.0) * 500.0;
    }
    if (dbTicksInRange(lo, hi, chosen) >= 2) {
        return chosen;
    }
    double best = 0.0;
    for (int i = 0; i < kDbLadderN; ++i) {
        if (kDbLadderRef[i] < chosen && dbTicksInRange(lo, hi, kDbLadderRef[i]) >= 2) {
            best = kDbLadderRef[i];
        }
    }
    return best;
}

void testDbLabelStepDocumentedCases() {
    // THE HEADER'S OWN CASE, and the reason this function exists: -160 dB ..
    // -5 dB on the default panel width chose 100 and printed the single
    // figure "-100". The step is coarse enough to fit the range on paper and
    // still lands its first tick most of the way in, so the second one falls
    // past the ceiling. The exception must take 50 instead.
    CHECK(dbTicksInRange(-160.0, -5.0, 100.0) == 1);  // the defect, stated
    CHECK(WaterfallView::dbLabelStep(-160.0, -5.0, 2) == 50.0);
    CHECK(WaterfallView::dbLabelStep(-160.0, -5.0, 3) == 50.0);
    CHECK(dbTicksInRange(-160.0, -5.0, 50.0) == 3);  // -150, -100, -50
    // maxIntervals == 1 asks for the coarsest scale there is; the exception
    // still overrides it, because a scale finer than requested crowds the bar
    // while a lone number says nothing at all.
    CHECK(WaterfallView::dbLabelStep(-160.0, -5.0, 1) == 50.0);

    // The header's other example: the common ranges stay as fine as they
    // were — the default -110 .. 0 keeps -100 / -50 / 0.
    CHECK(WaterfallView::dbLabelStep(-110.0, 0.0, 3) == 50.0);
    CHECK(dbTicksInRange(-110.0, 0.0, 50.0) == 3);
    // A range whose width-driven rung fits: no exception, the rung stands.
    CHECK(WaterfallView::dbLabelStep(-100.0, 0.0, 4) == 25.0);
    CHECK(dbTicksInRange(-100.0, 0.0, 25.0) == 5);
}

void testDbLabelStepLadderBoundaries() {
    // Both sides of every rung. The floor is -1000, a multiple of every rung
    // AND of the 500 fallback, so the first tick always lands on it and the
    // two-figure exception cannot mask the transition being measured; four
    // intervals' worth of range makes the rung above still fit two figures,
    // so the change of rung is the only thing moving.
    //
    // The hair either side is a nanodecibel rather than one ulp: at the top
    // rung the ulp of the ceiling (2.8e-14 at -200 dB) is finer than the ulp
    // of the range it produces (1.1e-13 at 800 dB), so a one-ulp nudge is
    // rounded away and the boundary never moves. 1e-9 dB survives that and is
    // still nine orders below anything the Display section can express.
    const double lo = -1000.0;
    const double hair = 1.0e-9;
    for (int i = 0; i < kDbLadderN; ++i) {
        const double s = kDbLadderRef[i];
        const double hi = lo + 4.0 * s;
        CHECK(WaterfallView::dbLabelStep(lo, hi, 4) == s);
        CHECK(WaterfallView::dbLabelStep(lo, hi - hair, 4) == s);
        const double wantAbove = (i + 1 < kDbLadderN) ? kDbLadderRef[i + 1] : 500.0;
        CHECK(WaterfallView::dbLabelStep(lo, hi + hair, 4) == wantAbove);
    }
    // Past the top of the ladder the step is rounded up to whole 500s. Both
    // of these are unreachable through the Display section (its widest range
    // is 180 dB); they pin the branch, not a user-visible state.
    CHECK(WaterfallView::dbLabelStep(-4000.0, -3000.0, 2) == 500.0);
    CHECK(WaterfallView::dbLabelStep(-4000.0, -2998.0, 2) == 1000.0);  // 501 -> 1000
}

void testDbLabelStepNarrowAndDegenerate() {
    const double qnan = std::nan("");
    // A SPAN UNDER A DECIBEL. Nothing on the ladder puts two figures inside
    // it, and the documented answer is no scale at all — an unlabelled ramp
    // under a plate that still states the exact floor and ceiling.
    CHECK(WaterfallView::dbLabelStep(-100.5, -100.0, 4) == 0.0);
    CHECK(WaterfallView::dbLabelStep(-100.25, -100.0, 1) == 0.0);
    CHECK(WaterfallView::dbLabelStep(-100.0, std::nextafter(-100.0, 0.0), 8) == 0.0);
    // Exactly one decibel, phased onto the integers: two figures fit, so
    // there IS a scale.
    CHECK(WaterfallView::dbLabelStep(-100.0, -99.0, 1) == 1.0);
    CHECK(dbTicksInRange(-100.0, -99.0, 1.0) == 2);
    // The same one-decibel span phased off the integers: the first multiple
    // is almost the whole span in, so only one figure lands and the answer
    // goes back to none. Same width, opposite outcome — which is exactly why
    // the decision cannot be made from the span alone.
    CHECK(dbTicksInRange(-100.5, -99.5, 1.0) == 1);
    CHECK(WaterfallView::dbLabelStep(-100.5, -99.5, 1) == 0.0);

    // No range, inverted range, NaN either end, no intervals asked for.
    CHECK(WaterfallView::dbLabelStep(-100.0, -100.0, 4) == 0.0);
    CHECK(WaterfallView::dbLabelStep(0.0, -100.0, 4) == 0.0);
    CHECK(WaterfallView::dbLabelStep(qnan, 0.0, 4) == 0.0);
    CHECK(WaterfallView::dbLabelStep(-100.0, qnan, 4) == 0.0);
    CHECK(WaterfallView::dbLabelStep(qnan, qnan, 4) == 0.0);
    CHECK(WaterfallView::dbLabelStep(-100.0, 0.0, 0) == 0.0);
    CHECK(WaterfallView::dbLabelStep(-100.0, 0.0, -3) == 0.0);
    CHECK(WaterfallView::dbLabelStep(-100.0, 0.0, -1000000) == 0.0);

    // AN INFINITE BOUND, on the same terms as timeLabelStep's: unreachable
    // through the Display section (both sliders are bounded), undocumented,
    // and asserted only for the property that must hold either way — a step
    // that is returned at all is one that puts at least two figures on the
    // scale, which is the whole reason this function is not just a ladder
    // lookup.
    //
    // MEASURED, for the record: all three answer 200 dB — the width-driven
    // choice is infinite, fails the two-figure test, and the exception falls
    // back to the coarsest rung on the ladder.
    const double dinf = std::numeric_limits<double>::infinity();
    const double infCases[3][2] = {{-100.0, dinf}, {-dinf, 0.0}, {-dinf, dinf}};
    for (const auto& c : infCases) {
        const double step = WaterfallView::dbLabelStep(c[0], c[1], 4);
        CHECK(step == 0.0 || dbTicksInRange(c[0], c[1], step) >= 2);
    }
}

void testDbLabelStepReachableSweep() {
    // EVERY FLOOR/CEILING PAIR THE PRODUCT CAN REACH. The Display section's
    // Min dB slider spans [-160, -20] and Max dB [-100, 20], and the section
    // holds the two ends at least kMinDbSpan = 10 dB apart (app_window.cpp).
    // That domain is the 12,966 whole-decibel pairs the header counts, and
    // the count is asserted below so a slider range changing underneath this
    // test is visible rather than silently shrinking the sweep.
    //
    // Over all of them, and over every label budget a panel from a sliver to
    // a full window can produce: the answer matches the independent
    // reference, there is ALWAYS a scale (never the lone-figure case, never
    // none), the step is always a rung of the ladder, and the scale never
    // needs more ticks than the drawing loop's 64-entry cap can place.
    int pairs = 0;
    int mismatches = 0;
    int lonelyOrAbsent = 0;
    int offLadder = 0;
    int overTickCap = 0;
    int maxTicks = 0;
    for (int floorDb = -160; floorDb <= -20; ++floorDb) {
        for (int ceilDb = -100; ceilDb <= 20; ++ceilDb) {
            if (ceilDb - floorDb < 10) {
                continue;
            }
            ++pairs;
            const double lo = static_cast<double>(floorDb);
            const double hi = static_cast<double>(ceilDb);
            for (int maxIntervals = 1; maxIntervals <= 12; ++maxIntervals) {
                const double step = WaterfallView::dbLabelStep(lo, hi, maxIntervals);
                const double want = refDbStep(lo, hi, maxIntervals);
                if (step != want) {
                    if (mismatches == 0) {
                        std::printf("  dbLabelStep(%g, %g, %d) = %g, want %g\n", lo, hi,
                                    maxIntervals, step, want);
                    }
                    ++mismatches;
                }
                const int ticks = dbTicksInRange(lo, hi, step);
                if (ticks < 2) {
                    if (lonelyOrAbsent == 0) {
                        std::printf("  dbLabelStep(%g, %g, %d) = %g places %d figure(s)\n",
                                    lo, hi, maxIntervals, step, ticks);
                    }
                    ++lonelyOrAbsent;
                }
                if (!onDbLadder(step)) {
                    ++offLadder;
                }
                if (ticks > 64) {  // kMaxDbTicks, the drawing loop's cap
                    ++overTickCap;
                }
                maxTicks = std::max(maxTicks, ticks);
            }
        }
    }
    CHECK(pairs == 12966);
    CHECK(mismatches == 0);
    CHECK(lonelyOrAbsent == 0);
    CHECK(offLadder == 0);
    CHECK(overTickCap == 0);
    CHECK(maxTicks >= 2);
}

// --- filledRows (how much of the ring is a measurement) ---------------------

void testFilledRowsCountsWrittenLines() {
    WaterfallView wf(8, 4);
    // A view that has received nothing holds no history — the whole point:
    // the application's own line counter survives a GL rebuild and this does
    // not, so a restart must read as an empty ring and not a full one.
    CHECK(wf.filledRows() == 0);
    CHECK(wf.hasRange() == false);

    // One per line, then saturating at texHeight() and staying there: past
    // that the ring recycles a row per line and the written depth stops
    // growing.
    for (int i = 1; i <= 20; ++i) {
        addUniformLine(wf, static_cast<float>(i % 15) / 15.0f);
        CHECK(wf.filledRows() == std::min(i, wf.texHeight()));
        CHECK(wf.filledRows() <= wf.texHeight());
    }
}

void testFilledRowsMarksExactlyTheWrittenRows() {
    // THE PROPERTY THE TIME STRIP RESTS ON: rows cursor .. cursor+filled-1
    // (mod H) are the ones a line was actually written into, and every other
    // row is still the constructor's pre-fill. Labelling those with an age is
    // what put "5s" beside a band of nothing on a fresh receiver.
    WaterfallView wf(8, 8);
    const float levels[3] = {3.0f / 15.0f, 7.0f / 15.0f, 11.0f / 15.0f};
    for (int i = 0; i < 3; ++i) {
        addUniformLine(wf, levels[i]);
    }
    CHECK(wf.filledRows() == 3);
    const int c = wf.rowCursor();
    // Newest first, from the cursor.
    CHECK(rowIsUniform(wf, (c + 0) % 8, waterfallColor(levels[2])));
    CHECK(rowIsUniform(wf, (c + 1) % 8, waterfallColor(levels[1])));
    CHECK(rowIsUniform(wf, (c + 2) % 8, waterfallColor(levels[0])));
    // Everything past filledRows() is still the empty colour, and none of the
    // three written levels is the empty colour, so this cannot pass by
    // accident.
    for (int i = 0; i < 3; ++i) {
        CHECK(waterfallColor(levels[i]) != waterfallColor(0.0f));
    }
    for (int k = wf.filledRows(); k < 8; ++k) {
        CHECK(rowIsUniform(wf, (c + k) % 8, waterfallColor(0.0f)));
    }
}

void testFilledRowsCountsDegenerateLines() {
    // A dropout was still received. It is painted as the floor colour on
    // purpose so the gap scrolls through the picture like any other line, and
    // it must count: a strip that skipped them would measure the history as
    // shorter than the picture it labels.
    WaterfallView wf(8, 16);
    float bins[8];
    for (int i = 0; i < 8; ++i) {
        bins[i] = -100.0f + 10.0f * static_cast<float>(i);
    }
    wf.addLine(nullptr, 0, -100.0f, 0.0f);  // null line
    CHECK(wf.filledRows() == 1);
    wf.addLine(bins, 0, -100.0f, 0.0f);  // empty line
    CHECK(wf.filledRows() == 2);
    wf.addLine(bins, 8, -30.0f, -30.0f);  // degenerate range
    CHECK(wf.filledRows() == 3);
    CHECK(wf.hasRange() == false);  // ... and no range to name it with
    wf.addLine(bins, 8, 0.0f, -100.0f);  // inverted range
    CHECK(wf.filledRows() == 4);
    const float qnanf = std::nanf("");
    wf.addLine(bins, 8, qnanf, 0.0f);  // NaN bound
    CHECK(wf.filledRows() == 5);
    CHECK(wf.hasRange() == false);
    // All five were painted as the empty colour, and all five counted.
    const int c = wf.rowCursor();
    for (int k = 0; k < 5; ++k) {
        CHECK(rowIsUniform(wf, (c + k) % 16, waterfallColor(0.0f)));
    }
}

void testFilledRowsInertViews() {
    // No texture, no history — and never a count over a ring that has no
    // rows to hold it.
    float bins[4] = {-90.0f, -60.0f, -30.0f, 0.0f};
    const int dims[3][2] = {{0, 0}, {8, 0}, {-4, -4}};
    for (const auto& d : dims) {
        WaterfallView wf(d[0], d[1]);
        CHECK(wf.filledRows() == 0);
        for (int i = 0; i < 5; ++i) {
            wf.addLine(bins, 4, -100.0f, 0.0f);
        }
        CHECK(wf.filledRows() == 0);
        CHECK(wf.filledRows() <= wf.texHeight());
    }
}

}  // namespace

int main() {
    testColorEndpoints();
    testColorClamping();
    testColorMonotonicBrightness();
    testMapIdentity();
    testMapUpsample();
    testMapDownsample();
    testMapUniformInput();
    testMapDegenerateRange();
    testRingInitialState();
    testRingCursorWrap();
    testRingResampledAndDegenerateLines();
    testUvNoSeam();
    testUvSeam();
    testUvClampAndDegenerate();
    testUvInvariantsSweep();
    testTimeLabelStepLadderBoundaries();
    testTimeLabelStepDegenerate();
    testTimeLabelStepNarrowAndWide();
    testTimeLabelStepSweep();
    testDbLabelStepDocumentedCases();
    testDbLabelStepLadderBoundaries();
    testDbLabelStepNarrowAndDegenerate();
    testDbLabelStepReachableSweep();
    testFilledRowsCountsWrittenLines();
    testFilledRowsMarksExactlyTheWrittenRows();
    testFilledRowsCountsDegenerateLines();
    testFilledRowsInertViews();
    return testSummary("test_waterfall_view");
}
