// scene3d_slint.hpp - the thin Slint adapter of the 3D scene framework (docs/engine-stage4.md 6.64): the only file that knows both scene3d.hpp's pure renderer and Slint's image type.
//
// SceneSurface follows the window's two-alternating-buffers rule (main.cpp, line 7): the buffer the window still shows is never written, so begin() never detaches (copies) a shared
// buffer and nothing is allocated per frame once the size is steady. `detaches` counts the times it did anyway (the run's exit line reports it, as it does for the wells).
// A new size reallocates both buffers (a window resized, a tab torn off); the picture is then painted at the new size on the very next frame.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

#include <slint.h>

#include "scene3d.hpp"

namespace foxsdr::scene3d {

static_assert(sizeof(Rgba) == sizeof(slint::Rgba8Pixel), "the renderer's pixel is Slint's");

class SceneSurface {
public:
    // The memory to paint this frame (w x h Rgba, top row first). A size under 8 x 8 is raised to it (Slint refuses an empty image).
    Rgba* begin(int w, int h) {
        w = std::max(8, w);
        h = std::max(8, h);
        if (w != w_ || h != h_) {
            for (auto& b : buf_) { b = slint::SharedPixelBuffer<slint::Rgba8Pixel>(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)); }
            ptr_ = {nullptr, nullptr};
            w_ = w;
            h_ = h;
        }
        auto* p = buf_[static_cast<std::size_t>(which_)].begin();
        if (ptr_[static_cast<std::size_t>(which_)] != nullptr && ptr_[static_cast<std::size_t>(which_)] != p) { ++detaches_; }
        ptr_[static_cast<std::size_t>(which_)] = p;
        return reinterpret_cast<Rgba*>(p);
    }
    int width() const { return w_; }
    int height() const { return h_; }
    // The picture just painted. Call it after painting, then flip().
    slint::Image image() const { return slint::Image(buf_[static_cast<std::size_t>(which_)]); }
    void flip() { which_ ^= 1; ++frames_; }
    int detaches() const { return detaches_; }
    int frames() const { return frames_; }

private:
    std::array<slint::SharedPixelBuffer<slint::Rgba8Pixel>, 2> buf_{slint::SharedPixelBuffer<slint::Rgba8Pixel>(8, 8), slint::SharedPixelBuffer<slint::Rgba8Pixel>(8, 8)};
    std::array<const void*, 2> ptr_{nullptr, nullptr};
    int which_ = 0, w_ = 8, h_ = 8, detaches_ = 0, frames_ = 0;
};

// The picture of a colour key: a ramp as an image, `n` pixels long and one wide (vertical, the top is the ramp's end) or one high (horizontal, the left is its start).
inline slint::Image rampImage(const Ramp& r, bool vertical, int n = 64) {
    n = std::max(2, n);
    slint::SharedPixelBuffer<slint::Rgba8Pixel> b(vertical ? 1u : static_cast<std::uint32_t>(n), vertical ? static_cast<std::uint32_t>(n) : 1u);
    auto* p = b.begin();
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
        const Rgba c = r.at(vertical ? 1.0f - t : t);
        p[i] = slint::Rgba8Pixel{c.r, c.g, c.b, 255};
    }
    return slint::Image(b);
}

}  // namespace foxsdr::scene3d
