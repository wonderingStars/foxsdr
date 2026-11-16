// radar3d_compositor.hpp - THE 3D AIRSPACE'S PICTURE (docs/engine-stage4.md 6.64, specs/THREED.md section 2): the radar's plan-position scene stood up in a box - range rings and bearing
// spokes on the ground, the antenna's cross at the middle, each plotted aircraft at its altitude with a drop line to the ground, a short vector along its course and (when the scope's TRAILS
// are on) its trail through the air - painted by the shared CPU renderer (scene3d.hpp) into a caller-owned buffer; the words (axis ticks, the aircraft's callsigns and flight levels) are
// returned as Label3D rows for Slint to place. The scene is in units where the range is 1: east and north from -1 to 1, the height from 0 to kRadar3dHeightAtX1 * the ALTITUDE SCALE factor.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "radar3d_logic.hpp"
#include "scene3d.hpp"

namespace foxsdr::slintview3d {

struct Radar3dInput {
    double rangeNm = 100.0;                         // the scope's range: the ground's half width
    std::vector<Radar3dAircraft> aircraft;          // the plotted ones
    int scaleIndex = 0;                             // ALTITUDE SCALE: 0 x1, 1 x2, 2 x4
    int trails = 1;                                 // the scope's TRAILS option: 0 off, 1 line, 2 ribbon
    scene3d::Camera camera;                         // azimuth, elevation (TILT) and zoom are the user's
    std::string captionEast = "NM E", captionNorth = "NM N", captionLevel = "FL", noAlt = "NO ALT";
    std::size_t maxLabels = 8;
};

// WHERE EACH PLOTTED AIRCRAFT'S MARKER LANDED, as fractions of the picture (window gaps lane 4, row 3042): what a click on the airspace is held against.
struct Radar3dPick {
    std::string id;
    float x = 0.0f, y = 0.0f;
};

struct Radar3dResult {
    std::vector<Radar3dPick> picks;                 // one per aircraft whose marker is in front of the eye
    std::size_t aircraft = 0;                       // drawn
    std::size_t onGround = 0;                       // of them, without an altitude
    std::size_t trailSegments = 0;
    std::vector<scene3d::Label3D> labels;
};

// Paints into px (w x h) on a transparent background (the well's own ground shows through). Reuses `scene` between frames.
Radar3dResult paintRadar3d(scene3d::Scene& scene, scene3d::Rgba* px, int w, int h, const Radar3dInput& in);

// The aircraft a click at (fx, fy) (fractions of the w x h picture) names: the nearest marker within `maxPx` pixels, "" when none is that close (the first of two equally near wins).
std::string pickRadar3d(const std::vector<Radar3dPick>& picks, float fx, float fy, int w, int h, float maxPx = 16.0f);

scene3d::Camera radar3dDefaultCamera();             // TILT 26 degrees, as the design's
const scene3d::Ramp& radar3dRamp();                 // the altitude key's ramp: FL0 indigo to FL400 amber
// The scene's height at ALTITUDE SCALE index `scaleIndex` (a box's top, in units of the range).
double radar3dBoxHeight(int scaleIndex);

}  // namespace foxsdr::slintview3d
