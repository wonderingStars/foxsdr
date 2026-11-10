// globe_compositor.hpp - THE GLOBE VIEWS' PICTURE (docs/engine-stage4.md 6.65): a GlobeDrawList (globe_draw.hpp) drawn into an RGBA buffer by the scene renderer of the 3D
// framework (scene3d.hpp: orbit camera, z-buffer, triangles, lines, points, label anchors, the textured UV sphere), plus the screen position of each label so the page can
// write its words over the picture. No Slint here: the publisher copies the pixels into the window's alternating buffers (main.cpp line 7).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "globe_draw.hpp"

namespace foxsdr::globe {

struct ScreenLabel {
    float x = 0.0f, y = 0.0f;       // 0..1 of the picture across and down
    std::string text;
    std::uint32_t argb = 0;
};

class GlobeCompositor {
public:
    // Draws `list` at width x height into `rgba` (width * height * 4 bytes, R G B A) and returns the labels that are in view. False when the size is unusable.
    bool compose(const GlobeDrawList& list, int width, int height, std::vector<std::uint8_t>& rgba, std::vector<ScreenLabel>& labels);
};

}  // namespace foxsdr::globe
