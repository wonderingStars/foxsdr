// Scrolling waterfall widget — implementation.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/waterfall_view.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

#include <imgui.h>

// glfw3.h pulls in GL/gl.h on Windows. Everything used here is OpenGL 1.1
// (glGenTextures / glTexImage2D / glTexSubImage2D / ...), which links straight
// from opengl32.lib — no loader, no GLEW, and no conflict with the ImGui
// opengl3 backend because this file never includes that backend's header.
#include <GLFW/glfw3.h>

// The GL upload below hands IM_COL32-packed pixels to glTexImage2D as
// GL_RGBA / GL_UNSIGNED_BYTE, which is only correct while ImU32 stores bytes
// in R,G,B,A memory order (ImGui's default packing). GL 1.1 has no BGRA
// format to fall back on, so fail the build loudly rather than render with
// red and blue swapped.
#ifdef IMGUI_USE_BGRA_PACKED_COLOR
#error "waterfall_view.cpp assumes ImGui's default RGBA packing (IM_COL32_R_SHIFT == 0)"
#endif

namespace cascade::gui {

namespace {

// Perceived brightness (ITU-R BT.601 luma weights) — the quantity the
// colormap keeps monotonically non-decreasing so louder always reads
// brighter, even on the yellow -> red stretch.
float lumaOf(int r, int g, int b) {
    return 0.299f * static_cast<float>(r) + 0.587f * static_cast<float>(g) +
           0.114f * static_cast<float>(b);
}

// Colormap anchor stops - PHOSPHOR, and the same tube the spectrum and the
// radar scope are drawn on.
//
// THE PROPERTY THIS TABLE EXISTS FOR IS UNCHANGED and is not a matter of
// taste: the anchor lumas strictly increase (14.7 -> 48.1 -> 98.0 -> 169.1 ->
// 230.2), so channel-wise linear interpolation gives a monotone ramp before
// rounding, and the sweep below fixes up the rounding. A waterfall is a
// MEASUREMENT: a stronger signal must never render darker than a weaker one,
// whatever colours it is made of. The blue-to-red table this replaced had the
// same property (4.6 -> 41.7 -> 105.1 -> 141.9 -> 145.2) and it was kept.
//
// THE FLOOR IS DELIBERATELY LIFTED off the reference's own value. The 1960s
// design starts its ramp at #050A06, which is within a couple of luma units of
// the panel it is painted on - and a signal a few dB above the noise floor
// would simply not be visible. This table starts at luma 14.7 instead: still
// unmistakably "nothing there", still darker than any real signal, but far
// enough off the ground that the bottom of the range keeps its resolution.
// The old table's first segment spanned 4.6 -> 41.7 and this one spans 14.7 ->
// 48.1, so the usable contrast at the quiet end is preserved rather than
// merely claimed.
//
// The top stop is a warm cream rather than white: it has to be the brightest
// entry in the table, and a pure white would leave nothing above it for the
// eye to read a peak against.
struct Rgb {
    float r, g, b;
};
constexpr float kStopPos[5] = {0.0f, 0.20f, 0.45f, 0.75f, 1.0f};
constexpr Rgb kStopRgb[5] = {
    {6.0f, 20.0f, 10.0f},      // quiet phosphor (exact at norm 0)
    {10.0f, 70.0f, 35.0f},     // green
    {30.0f, 140.0f, 60.0f},    // phosphor
    {150.0f, 200.0f, 60.0f},   // yellow-green
    {240.0f, 235.0f, 180.0f},  // cream anchor (exact at norm 1)
};

const std::array<ImU32, 256>& colorLut() {
    // Magic static: built once, thread-safe, no static-init-order hazards.
    static const std::array<ImU32, 256> lut = [] {
        std::array<int, 256> r{};
        std::array<int, 256> g{};
        std::array<int, 256> b{};
        for (int i = 0; i < 256; ++i) {
            const float t = static_cast<float>(i) / 255.0f;
            int seg = 3;
            for (int s = 0; s < 4; ++s) {
                if (t <= kStopPos[s + 1]) {
                    seg = s;
                    break;
                }
            }
            const float u = (t - kStopPos[seg]) / (kStopPos[seg + 1] - kStopPos[seg]);
            const Rgb& a = kStopRgb[seg];
            const Rgb& c = kStopRgb[seg + 1];
            r[i] = static_cast<int>(a.r + (c.r - a.r) * u + 0.5f);
            g[i] = static_cast<int>(a.g + (c.g - a.g) * u + 0.5f);
            b[i] = static_cast<int>(a.b + (c.b - a.b) * u + 0.5f);
        }
        // Rounding each channel independently can dip luma by up to ~1 unit
        // between neighbors on shallow segments. Sweep backward from the
        // fixed red anchor, darkening any entry brighter than its hotter
        // neighbor. Green goes first: it has the largest luma weight (fewest
        // steps) and lowering G near the top is exactly the natural
        // green -> yellow -> cream hue motion. Entry 255 is never touched and
        // entry 0's luma sits well below entry 1's, so both contract anchors
        // survive the sweep byte-exact.
        for (int i = 254; i >= 0; --i) {
            while (lumaOf(r[i], g[i], b[i]) > lumaOf(r[i + 1], g[i + 1], b[i + 1])) {
                if (g[i] > 0) {
                    --g[i];
                } else if (r[i] > 0) {
                    --r[i];
                } else if (b[i] > 0) {
                    --b[i];
                } else {
                    break;  // all black cannot be too bright; defensive only
                }
            }
        }
        std::array<ImU32, 256> out{};
        for (int i = 0; i < 256; ++i) {
            out[i] = IM_COL32(r[i], g[i], b[i], 255);
        }
        return out;
    }();
    return lut;
}

}  // namespace

ImU32 waterfallColor(float norm01) {
    // !(x > 0) instead of (x <= 0): NaN fails every comparison, so a NaN
    // input falls into this branch and reads as the coldest color instead of
    // indexing the LUT with garbage.
    if (!(norm01 > 0.0f)) {
        norm01 = 0.0f;
    }
    if (norm01 > 1.0f) {
        norm01 = 1.0f;
    }
    return colorLut()[static_cast<int>(norm01 * 255.0f + 0.5f)];
}

void mapLineToPixels(const float* dbBins, int n, float dbMin, float dbMax,
                     ImU32* dst, int texWidth) {
    if (dst == nullptr || texWidth <= 0) {
        return;
    }
    // Degenerate range check is !(dbMax > dbMin) so a NaN bound also lands
    // here instead of producing NaN norms for every pixel.
    if (dbBins == nullptr || n <= 0 || !(dbMax > dbMin)) {
        const ImU32 floorColor = waterfallColor(0.0f);
        for (int x = 0; x < texWidth; ++x) {
            dst[x] = floorColor;
        }
        return;
    }
    const float invRange = 1.0f / (dbMax - dbMin);
    for (int x = 0; x < texWidth; ++x) {
        // Nearest resampling: the bin under the pixel's center. The clamp
        // guards float round-up at the right edge for extreme sizes.
        int src = static_cast<int>((static_cast<float>(x) + 0.5f) *
                                   static_cast<float>(n) / static_cast<float>(texWidth));
        if (src >= n) {
            src = n - 1;
        }
        dst[x] = waterfallColor((dbBins[src] - dbMin) * invRange);
    }
}

WaterfallView::WaterfallView(int width, int height)
    : width_(std::max(width, 0)), height_(std::max(height, 0)) {
    // Pre-fill with the coldest color so the first frame shows an empty
    // waterfall, not uninitialized texels.
    pixels_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_),
                   waterfallColor(0.0f));
}

WaterfallView::~WaterfallView() {
    if (texture_ != 0) {
        const GLuint id = texture_;
        glDeleteTextures(1, &id);
    }
}

void WaterfallView::addLine(const float* dbBins, int n, float dbMin, float dbMax) {
    if (width_ <= 0 || height_ <= 0) {
        return;
    }
    // The cursor walks upward through the texture (decrement, wrapped):
    // rows cursor, cursor+1, ... then read newest-to-oldest, which the
    // wrapped-v draw maps straight to top-to-bottom on screen.
    cursor_ = (cursor_ + height_ - 1) % height_;
    mapLineToPixels(dbBins, n, dbMin, dbMax,
                    pixels_.data() + static_cast<std::size_t>(cursor_) * static_cast<std::size_t>(width_),
                    width_);
    // Cap at height_: once every row is rewritten between draws, uploading
    // the full texture is cheaper than height_ single-row uploads.
    pendingRows_ = std::min(pendingRows_ + 1, height_);
}

int WaterfallView::uvRects(int rowCursor, int height, double u0, double u1,
                           UvRect out[2]) {
    if (out == nullptr || height <= 0) {
        return 0;
    }
    // Clamp the window into the texture. Deliberately one-sided per bound
    // (`<` / `>` rather than !(...)-style) so a NaN survives the clamps and
    // is then caught by the !(b > a) degenerate exit below — a NaN window
    // must render as "nothing", never as a silently-full window.
    double a = u0;
    double b = u1;
    if (a < 0.0) { a = 0.0; }
    if (b > 1.0) { b = 1.0; }
    // Catches inverted, equal (zero-width), NaN, and windows entirely
    // outside [0, 1] (both bounds clamp to the same edge).
    if (!(b > a)) {
        return 0;
    }
    // rowCursor() already stays in [0, height); wrap defensively anyway so a
    // caller-supplied cursor can never index texels that do not exist.
    int c = rowCursor % height;
    if (c < 0) { c += height; }

    const float fu0 = static_cast<float>(a);
    const float fu1 = static_cast<float>(b);
    if (c == 0) {
        // Ring start coincides with the texture's top row: no seam inside
        // the widget, one quad covers everything.
        out[0] = UvRect{0.0f, 1.0f, fu0, 0.0f, fu1, 1.0f};
        return 1;
    }
    // Seam split. Newest rows c..H-1 render on top, oldest rows 0..c-1
    // below. Each quad's screen share equals its v share, so history rows
    // keep a uniform on-screen thickness across the seam.
    const float vSeam = static_cast<float>(c) / static_cast<float>(height);
    out[0] = UvRect{0.0f, 1.0f - vSeam, fu0, vSeam, fu1, 1.0f};
    out[1] = UvRect{1.0f - vSeam, 1.0f, fu0, 0.0f, fu1, vSeam};
    return 2;
}

void WaterfallView::draw(float width, float height) {
    draw(width, height, 0.0, 1.0);
}

void WaterfallView::draw(float width, float height, double u0, double u1) {
    if (width_ <= 0 || height_ <= 0 || width <= 0.0f || height <= 0.0f) {
        return;
    }
    if (texture_ == 0) {
        GLuint id = 0;
        glGenTextures(1, &id);
        texture_ = id;
        glBindTexture(GL_TEXTURE_2D, texture_);
        // NEAREST: see the seam rationale in the header. Wrap modes are no
        // longer exercised (uvRects keeps every uv inside [0, 1]); REPEAT
        // stays as the value the original wrapped-v draw used, and GL 1.1
        // has no CLAMP_TO_EDGE to switch to anyway.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width_, height_, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, pixels_.data());
        pendingRows_ = 0;
    } else if (pendingRows_ > 0) {
        glBindTexture(GL_TEXTURE_2D, texture_);
        if (pendingRows_ >= height_) {
            // Every row changed since the last draw; one bulk update.
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, GL_RGBA,
                            GL_UNSIGNED_BYTE, pixels_.data());
        } else {
            for (int k = 0; k < pendingRows_; ++k) {
                const int row = (cursor_ + k) % height_;
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, width_, 1, GL_RGBA,
                                GL_UNSIGNED_BYTE,
                                pixels_.data() +
                                    static_cast<std::size_t>(row) * static_cast<std::size_t>(width_));
            }
        }
        pendingRows_ = 0;
    }

    // At most two quads split at the ring seam (see uvRects). Emitted as
    // AddImage calls on the draw list rather than stacked ImGui::Image items
    // because items would insert ItemSpacing between the two quads; a single
    // Dummy then reserves the widget rectangle exactly once — even when a
    // degenerate window drew nothing, so a bad zoom state cannot shift the
    // surrounding layout.
    UvRect rects[2];
    const int rectCount = uvRects(cursor_, height_, u0, u1, rects);
    if (rectCount > 0) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        for (int i = 0; i < rectCount; ++i) {
            const UvRect& r = rects[i];
            drawList->AddImage(static_cast<ImTextureID>(texture_),
                               ImVec2(p0.x, p0.y + r.y0Frac * height),
                               ImVec2(p0.x + width, p0.y + r.y1Frac * height),
                               ImVec2(r.u0, r.v0), ImVec2(r.u1, r.v1));
        }
    }
    ImGui::Dummy(ImVec2(width, height));
}

const ImU32* WaterfallView::rowPixels(int row) const noexcept {
    if (row < 0 || row >= height_ || width_ <= 0) {
        return nullptr;
    }
    return pixels_.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width_);
}

}  // namespace cascade::gui
