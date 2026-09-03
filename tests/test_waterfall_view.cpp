// Tests for gui/waterfall_view.hpp — everything here is headless: only the
// colormap, the dB->pixel row conversion, the CPU ring and the seam/window
// uv math (uvRects) are exercised; the draw() overloads (the GL-touching
// members) are never called, so no GL context is needed. Expected colors are
// derived in-test from the documented contract (anchor RGBA constants, the
// normalization formula, the nearest-resampling rule
// floor((x + 0.5) * n / texWidth)) — never read back from internals — and
// the uv expectations come from the documented seam-split contract.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/waterfall_view.hpp"

#include <cstdint>
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
    return testSummary("test_waterfall_view");
}
