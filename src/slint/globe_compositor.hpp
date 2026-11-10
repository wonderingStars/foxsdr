// globe_compositor.hpp - THE GLOBE VIEWS' PICTURE (docs/engine-stage4.md 6.65): a GlobeDrawList (globe_draw.hpp) drawn by the 3D framework's CPU renderer (scene3d.hpp) into a
// caller-owned RGBA buffer (the well's alternating buffers, scene3d_slint.hpp SceneSurface, in the window), with the screen position of each label so the page can write its words over the picture. No Slint types.
//
// THE GLOBE IS TURNED, NOT THE CAMERA. The framework's orbit camera keeps its elevation between 4 and 89 degrees (a camera under a floor makes no sense for the box views),
// which could never face the south of a globe. So a globe's camera is FIXED over the world's +Z axis (elevation 89, north up the picture) and the globe is rotated under it: the
// point the view is centred on (globe_logic GlobeCamera: a longitude and a latitude) is turned to +Z, the north pole kept toward the top of the picture. Every vertex, line and
// marker takes the same rotation, so the depth test and the labels stay the renderer's. The orbits view has no globe: its camera is the framework's own (azimuth = the spin about
// the ecliptic pole, elevation = the tilt above the ecliptic plane).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "globe_draw.hpp"
#include "scene3d.hpp"

namespace foxsdr::globe {

// The turn that brings (centreLon, centreLat) to the world's +Z with north up: Rz(-90 - lon) then Rx(lat - 90). Row-major 3 x 3.
struct Rotation {
    double m[3][3];
};
Rotation rotationFor(const GlobeCamera& c);
Vec3 rotate(const Rotation& r, const Vec3& v);

// A colour as the framework wants it: ARGB (the draw list's) to its Rgba pixel and to the 0xRRGGBBAA of its labels.
scene3d::Rgba toRgba(std::uint32_t argb);
std::uint32_t toLabelRgba(std::uint32_t argb);

class GlobeCompositor {
public:
    // Paints `list` into `pixels` (width * height, top row first) and returns the labels that are in view. False when the size is unusable (under 16 x 16) or `pixels` is null.
    bool paint(const GlobeDrawList& list, scene3d::Rgba* pixels, int width, int height, std::vector<scene3d::Label3D>& labels);
    // For the tests: the scene the last compose drew with (projection of a point, the depth under a pixel).
    const scene3d::Scene& scene() const { return scene_; }

private:
    scene3d::Scene scene_;
    scene3d::Mesh sphere_;                       // the unit sphere, built once (96 x 48)
    std::vector<scene3d::Rgba> texPx_;
    std::uint64_t texKey_ = ~std::uint64_t{0}, texRevision_ = ~std::uint64_t{0};
    int texW_ = 0, texH_ = 0;
};

}  // namespace foxsdr::globe
