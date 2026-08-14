// Tests for gui/waterfall_view.hpp — everything here is headless: only the
// colormap, the dB->pixel row conversion and the CPU ring are exercised;
// draw() (the sole GL-touching member) is never called, so no GL context is
// needed. Expected colors are derived in-test from the documented contract
// (anchor RGBA constants, the normalization formula, the nearest-resampling
// rule floor((x + 0.5) * n / texWidth)) — never read back from internals.
//
// SPDX-License-Identifier: MIT
#include "gui/waterfall_view.hpp"

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
    CHECK(waterfallColor(0.0f) == IM_COL32(0, 0, 40, 255));
    CHECK(waterfallColor(1.0f) == IM_COL32(255, 100, 90, 255));
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
    // Hue milestones of the documented blue -> cyan -> yellow -> red
    // progression (loose inequalities, not exact mid-table constants).
    const ImU32 cold = waterfallColor(0.0f);
    CHECK(chB(cold) > chR(cold) && chB(cold) > chG(cold));  // blue end
    const ImU32 mid = waterfallColor(0.45f);
    CHECK(chG(mid) > chR(mid) && chB(mid) > chR(mid));      // cyan-ish
    const ImU32 warm = waterfallColor(0.75f);
    CHECK(chR(warm) > chB(warm) && chG(warm) > chB(warm));  // yellow-ish
    const ImU32 hot = waterfallColor(1.0f);
    CHECK(chR(hot) > chG(hot) && chR(hot) > chB(hot));      // red end
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
    return testSummary("test_waterfall_view");
}
