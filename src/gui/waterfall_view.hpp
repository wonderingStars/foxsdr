// Scrolling waterfall widget: CPU-side pixel ring + lazily created GL texture.
//
// The CPU ring (per-row RGBA pixels + row cursor) is deliberately separate
// from the GL upload path so every piece of logic that can be wrong — the
// colormap, the dB->pixel conversion, nearest resampling, ring wrap, the
// seam/window uv math — is testable headless. The draw() overloads are the
// only members that touch OpenGL.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <vector>

#include <imgui.h>

namespace cascade::gui {

// Colormap LUT lookup. Input clamped to [0, 1] (NaN reads as 0). Progression
// deep blue -> cyan -> yellow -> red with monotonically non-decreasing
// perceived brightness (0.299R + 0.587G + 0.114B) across the whole table —
// enforced at LUT build time, because rounding independent channels of a
// piecewise-linear ramp can otherwise dip by a fraction of a luma unit.
//
// Contract constants a display (and the tests) may rely on:
//   waterfallColor(0.0f) == IM_COL32(  6,  20,  10, 255)   // quiet phosphor
//   waterfallColor(1.0f) == IM_COL32(240, 235, 180, 255)   // cream anchor
// The top anchor is a warm cream rather than white because it must be the
// brightest entry in the table and still leave the eye somewhere to read a
// peak; the bottom anchor is lifted off true black so a signal a few dB above
// the noise floor stays visible. All entries are fully opaque.
ImU32 waterfallColor(float norm01);

// Converts one spectrum line into one texture row: clamped normalization
// norm = (db - dbMin) / (dbMax - dbMin), nearest-neighbor resampling of n
// bins onto texWidth pixels, then waterfallColor lookup.
//
// Resampling rule (documented so tests can pin it): pixel x reads source bin
//   floor((x + 0.5) * n / texWidth), clamped to [0, n-1]
// i.e. the bin under the pixel's center. n == texWidth is an exact identity.
//
// Degenerate inputs are defined, not UB: dbBins == nullptr, n <= 0, or a
// non-positive/NaN dB span (dbMax <= dbMin) fill the row with the norm-0
// floor color — "no information" renders as an empty (coldest) line.
void mapLineToPixels(const float* dbBins, int n, float dbMin, float dbMax,
                     ImU32* dst, int texWidth);

// Scrolling waterfall: newest line at the top, history scrolling down.
//
// Threading: addLine() and draw() mutate shared state without locks; call
// both from the same (GUI) thread. The pipeline hands frames to the GUI
// thread anyway, so this costs nothing and keeps draw() allocation-free.
class WaterfallView {
public:
    // Texture dimensions: width = frequency bins across, height = history
    // lines. Non-positive dimensions yield an inert view (addLine/draw no-op).
    WaterfallView(int width, int height);

    // Frees the GL texture if draw() ever created one. Requires the GL
    // context that created it to still be current (same rule as any GL
    // resource); a never-drawn view is safely destroyed without a context.
    ~WaterfallView();

    // The destructor owns a GL texture handle, so copies would double-free.
    WaterfallView(const WaterfallView&) = delete;
    WaterfallView& operator=(const WaterfallView&) = delete;

    // Pushes one spectrum line into the CPU ring (newest at top, scrolls
    // down), resampling n bins to the texture width via mapLineToPixels.
    // Never touches GL, so it is headless-testable; the rows written since
    // the last draw() are uploaded lazily there.
    void addLine(const float* dbBins, int n, float dbMin, float dbMax);

    // Full-span draw: forwards to the windowed draw with the whole texture
    // width visible ([u0, u1] = [0, 1]). Kept so existing callers and the
    // unzoomed display need no window bookkeeping.
    void draw(float width, float height);

    // Windowed draw: renders only the horizontal texture window [u0, u1]
    // (0..1 across the bins) stretched to width x height. Lazily creates
    // the GL texture on first call (needs a live GL context), then uploads
    // only the rows addLine() wrote since the previous draw, as single-row
    // glTexSubImage2D updates. The window arrives as plain texture-u
    // numbers from the zoom controller — no frequency-scale dependency —
    // and addLine() always stores the full span, so zooming back out never
    // loses history.
    //
    // Seam handling: the ring seam (newest row at the widget's top edge,
    // ages increasing downward) is drawn via uvRects() below — at most two
    // quads split exactly at the seam row, each quad's screen share equal
    // to its v share. On-screen this is identical to the previous
    // wrapped-v single-Image approach, but it also works for any u-window
    // and keeps every uv inside [0, 1]. Filtering is GL_NEAREST so the
    // newest and oldest rows never blend into each other at the seam.
    // A degenerate window (u0 >= u1 after clamping to [0, 1], or NaN)
    // draws no texture but still occupies the widget rectangle, so a bad
    // zoom state cannot shift the surrounding layout.
    void draw(float width, float height, double u0, double u1);

    // One screen quad of a windowed draw: the vertical slice of the widget
    // it covers and the texture uv corners it samples. Plain floats, no GL
    // types, so the seam math is testable headless.
    struct UvRect {
        float y0Frac, y1Frac;  // widget-vertical span: 0 = top edge, 1 = bottom
        float u0, v0;          // uv of the quad's top-left corner
        float u1, v1;          // uv of the quad's bottom-right corner
    };

    // The uv computation behind draw(w, h, u0, u1), exposed static so tests
    // can pin seam handling without a GL context. Clamps [u0, u1] to
    // [0, 1], then writes the quads to issue for ring cursor `rowCursor` in
    // a texture of `height` rows:
    //   rowCursor == 0 -> 1 quad: v spans [0, 1], no seam inside the widget;
    //   rowCursor == c -> 2 quads: rows c..H-1 (v in [c/H, 1]) on top, then
    //                     rows 0..c-1 (v in [0, c/H]) below — newest row at
    //                     the top edge, seam exactly between the quads.
    // Invariant the tests rely on: each quad's screen span equals its v
    // span (y1Frac - y0Frac == v1 - v0), so every history row renders at
    // the same thickness. Returns the number of quads written (0, 1 or 2);
    // a degenerate window (u0 >= u1 after clamping, or NaN), height <= 0,
    // or a null `out` returns 0. An out-of-range rowCursor is wrapped into
    // [0, height) defensively.
    static int uvRects(int rowCursor, int height, double u0, double u1, UvRect out[2]);

    // --- Read-only introspection (headless-safe, never touches GL) ---

    int texWidth() const noexcept { return width_; }
    int texHeight() const noexcept { return height_; }

    // Texture row holding the newest line. Starts at 0; each addLine()
    // decrements it (wrapping), so rows cursor, cursor+1, ... cursor+H-1
    // (mod H) read newest-to-oldest — which is what the seam-split draw
    // (uvRects) shows top-to-bottom.
    int rowCursor() const noexcept { return cursor_; }

    // Pointer to texture row `row`'s texWidth() pixels, or nullptr when the
    // row is out of [0, texHeight()) or the view is empty.
    const ImU32* rowPixels(int row) const noexcept;

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<ImU32> pixels_;  // row-major width_ x height_ CPU ring
    int cursor_ = 0;             // ring row of the newest line
    int pendingRows_ = 0;        // rows written since last upload, capped at height_
    unsigned int texture_ = 0;   // GLuint; 0 = not created yet
};

}  // namespace cascade::gui
