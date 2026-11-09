// radar_compositor.hpp - THE RADAR TUBE'S GROUND (docs/engine-stage4.md 6.57): the dark glass, the compiled-in coastline, the phosphor wash, the crosshair, the trails, the scan lines, the four
// range rings and the twelve bearing ticks, rasterised on the CPU into a caller-owned RGBA buffer with the demod scope's Canvas (scope_compositor.hpp), and the sweep fan as a picture the
// view turns. What is NOT painted is a Slint item: the targets and their labels, the ring and bearing words, the corner readouts, the sweep's rotation, the antenna and view-centre marks.
//
// The painters are pure over their buffer: the same inputs paint the same bytes. The tube is a disc on the glass: everything outside it (the corners of the square) is transparent.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <vector>

#include "radar_logic.hpp"
#include "scope_compositor.hpp"

namespace foxsdr::slintscope {

// What the painter keeps between frames: the coastline's projected points (so a frame allocates nothing once it has grown).
struct RadarPaintScratch {
    std::vector<float> x, y;
    std::vector<unsigned char> inside;
};

// Paints the tube's ground into px (w x h, row-major, top row first; the tube is the square of the shorter side, centred, as RadarScope::setTubeSize was told). The frame the scope last
// built (RadarScope::update) decides what is drawn: without a receiver position only the dark ground, under 40 px nothing at all. Returns the number of strokes drawn (a cost the tests report).
std::size_t paintRadarGround(Rgba* px, int w, int h, const RadarScope& scope, RadarPaintScratch& scratch);

// The sweep fan: a 55 degree wedge of the phosphor green with its LEADING EDGE straight up from the centre of a size x size picture and its tail 55 degrees anticlockwise of it (the sweep turns
// clockwise), 44 steps, alpha 77 at the edge fading to nothing at the tail (scope_view.cpp addSweepFan), a disc of radius size / 2, transparent elsewhere. The view rotates it clockwise by
// radarSweepDegrees(now).
void paintSweepFan(Rgba* px, int size);

constexpr int kRadarMinTubePx = 40;

}  // namespace foxsdr::slintscope
