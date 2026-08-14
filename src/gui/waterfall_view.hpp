// Scrolling waterfall widget: CPU-side pixel ring + lazily created GL texture.
//
// The CPU ring (per-row RGBA pixels + row cursor) is deliberately separate
// from the GL upload path so every piece of logic that can be wrong — the
// colormap, the dB->pixel conversion, nearest resampling, ring wrap — is
// testable headless. draw() is the only member that touches OpenGL.
//
// SPDX-License-Identifier: MIT
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
//   waterfallColor(0.0f) == IM_COL32(  0,   0,  40, 255)   // deep blue anchor
//   waterfallColor(1.0f) == IM_COL32(255, 100,  90, 255)   // red anchor
// The red anchor carries some green/blue on purpose: a *pure* red (255,0,0)
// has lower perceived brightness than yellow, so a jet-style map ending there
// cannot be brightness-monotonic. All entries are fully opaque.
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

    // ImGui::Image of the ring texture stretched to width x height. Lazily
    // creates the GL texture on first call (needs a live GL context), then
    // uploads only the rows addLine() wrote since the previous draw, as
    // single-row glTexSubImage2D updates.
    //
    // Seam handling (deliberate choice): one Image call with wrapped v
    // coordinates — uv spans [cursor/H, cursor/H + 1] and GL_REPEAT carries
    // v past 1.0, so the ring seam lands exactly at the widget's top edge.
    // Filtering is GL_NEAREST because GL_LINEAR would blend the newest and
    // oldest rows across the seam; nearest sidesteps that without a second
    // Image call, and spectrum bins are typically ~1:1 with panel pixels.
    void draw(float width, float height);

    // --- Read-only introspection (headless-safe, never touches GL) ---

    int texWidth() const noexcept { return width_; }
    int texHeight() const noexcept { return height_; }

    // Texture row holding the newest line. Starts at 0; each addLine()
    // decrements it (wrapping), so rows cursor, cursor+1, ... cursor+H-1
    // (mod H) read newest-to-oldest — which is what the wrapped-v draw shows
    // top-to-bottom.
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
