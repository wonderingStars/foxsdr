// waterfall3d_compositor.hpp - THE 3D WATERFALL SURFACE'S PICTURE (docs/engine-stage4.md 6.64, specs/THREED.md section 1): the history grid as a lit height field in the axis box,
// with the VFO line and the markers' lines down it, painted by the shared CPU renderer (scene3d.hpp) into a caller-owned buffer, and the labels that go over it as Slint text at their
// projected positions. The renders in redesign-handover are the LOOK (a dark ground, blue ridges with amber peaks, a light axis box); the surface here is the live data.
//
// WHAT IS PAINTED: the walls and floor behind, the surface, the VFO line (amber, along the surface at the tuned frequency, and on the floor), a dashed white line per user marker, the box's
// front edges. WHAT IS NOT: the words (tick values, the axis captions, the marker tabs), which are returned as Label3D rows for the well to place, and the colour key, which is a Slint plate.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "scene3d.hpp"

namespace foxsdr::slintview3d {

struct Wf3dMarker {
    double hz = 0.0;
    std::string label;        // "M1 145.212 MHz", as the 2D waterfall's tab says it
};

struct Wf3dInput {
    const float* grid = nullptr;       // rows x cols levels (dB), row 0 the newest; NaN = a hole (WaterfallHistory::grid)
    int rows = 0, cols = 0;
    double centreHz = 0.0, spanHz = 0.0;   // the window the columns cover
    double tunedHz = 0.0;
    double dbMin = -110.0, dbMax = 0.0;    // the display range: the surface's floor and ceiling
    double depthS = 60.0;
    bool showVfo = true;
    std::vector<Wf3dMarker> markers;
    int shading = 0;                   // 0 Height (one hue, lighter with level), 1 Colour ramp (the strength key's colours)
    scene3d::Camera camera;
    std::string captionFreq = "MHz", captionAge = "age (s)", captionDb = "dB";   // the host passes the translated caption
};

struct Wf3dResult {
    bool hasData = false;              // at least one row of the grid is real
    std::size_t triangles = 0;
    std::vector<scene3d::Label3D> labels;
};

// Paints the scene into px (w x h). The background is transparent: the well's own ground shows through. Reuses `scene` (its depth buffer) between frames.
Wf3dResult paintWaterfall3d(scene3d::Scene& scene, scene3d::Rgba* px, int w, int h, const Wf3dInput& in);

// The camera's defaults for this view and the box's proportions (tests and the host read them).
scene3d::Camera waterfall3dDefaultCamera();
constexpr float kWf3dBoxX = 1.0f, kWf3dBoxY = 0.8f, kWf3dBoxZ = 0.7f;   // half widths in x and y, the height in z

// The strength key under SHADING: the ramp the surface is coloured by.
const scene3d::Ramp& waterfall3dRamp(int shading);

}  // namespace foxsdr::slintview3d
