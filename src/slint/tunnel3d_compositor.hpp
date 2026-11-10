// tunnel3d_compositor.hpp - THE I/Q TUNNEL'S PICTURE (docs/engine-stage4.md 6.64, specs/THREED.md section 4): the demod scope's baseband samples as points along a time axis - I and Q across,
// time along - inside a box, with faint rings marking the tunnel's cross-section, painted by the shared CPU renderer (scene3d.hpp) into a caller-owned buffer, and a small 2D constellation
// of the newest samples in a corner of the same picture (the inset). The axis words are returned as Label3D rows for Slint to place. Scene units: time from -1.5 to 1.5, I and Q from -0.5 to 0.5.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "scene3d.hpp"

namespace foxsdr::slintview3d {

struct TunnelInput {
    const float* i = nullptr;            // the samples to draw, oldest first (the caller has already taken the newest `n` for the TIME DEPTH)
    const float* q = nullptr;
    std::size_t n = 0;
    double rateHz = 0.0;                 // for the time axis
    int colourBy = 0;                    // 0 Time (cyan at the oldest to magenta at the newest), 1 Level (the sample's distance from the middle)
    bool inset = true;                   // the small 2D constellation in the top left corner
    scene3d::Camera camera;
    std::string captionTime = "time (ms)", captionI = "I", captionQ = "Q";
};

struct TunnelResult {
    std::size_t points = 0;              // samples drawn
    double axis = 1.0;                   // the I and Q half range the axes were drawn for
    double spanMs = 0.0;                 // the time the samples span
    int insetX = 0, insetY = 0, insetSize = 0;   // the inset's square in pixels (size 0: none drawn)
    std::vector<scene3d::Label3D> labels;
};

// Paints into px (w x h) on a transparent background. Reuses `scene` between frames.
TunnelResult paintTunnel(scene3d::Scene& scene, scene3d::Rgba* px, int w, int h, const TunnelInput& in);

scene3d::Camera tunnelDefaultCamera();
const scene3d::Ramp& tunnelRamp(int colourBy);   // the ramp COLOUR BY paints with (the key's too)

}  // namespace foxsdr::slintview3d
