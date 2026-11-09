// scope_compositor.hpp - THE SCOPES' PICTURES (docs/engine-stage4.md 6.57): the two glass faces rasterised on the CPU into a caller-owned RGBA buffer, the way the spectrum trace and
// the map are (main.cpp WellBuffers, map_compositor.hpp), because a Slint Path with thousands of segments re-tessellates on every update and a PERSIST figure is hundreds of
// thousands. The words on the glass, the keys and the targets are Slint items (ui/scope_modes.slint); this file draws only what is a picture: the demod tube's graticule, beam,
// spectrum, vector figure, ghosts and scan lines, and (radar_compositor, later in this file's family) the radar tube's ground.
//
// The painters are pure over their buffer: the same inputs paint the same bytes, which is what test_slint_host_scope pins. Colours are the classic tube's own (demod_scope_face.cpp
// 55-84), written as 8-bit values.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "host_logic.hpp"
#include "scope_logic.hpp"

namespace foxsdr::slintscope {

using foxsdr::slinthost::Rgba;

// A drawing surface over caller-owned memory (w * h Rgba, row-major, top row first). Everything clips to the surface.
class Canvas {
public:
    Canvas(Rgba* px, int w, int h) : px_(px), w_(w), h_(h) {}
    int width() const { return w_; }
    int height() const { return h_; }
    void fill(Rgba c);
    // Source-over a colour of alpha `a` (0..255) scaled by `cover` (0..1) onto one pixel.
    void blend(int x, int y, Rgba c, float cover);
    // An anti-aliased segment of `widthPx` (round-ish ends): coverage from the distance to the segment.
    void line(float x0, float y0, float x1, float y1, float widthPx, Rgba c);
    void rect(float x0, float y0, float x1, float y1, Rgba c);   // filled, pixel-aligned edges
    void disc(float cx, float cy, float r, Rgba c);
    void ring(float cx, float cy, float r, float widthPx, Rgba c);
    const Rgba& at(int x, int y) const { return px_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x)]; }

private:
    Rgba* px_;
    int w_, h_;
};

// What the painter keeps between frames: the column envelope's scratch and the PERSIST vector figure's phosphor (a decaying intensity buffer: the classic keeps up to 240 ghost figures
// of 1024 points and strokes them all every frame, about 245 000 segments, which a CPU raster cannot do at 30 Hz - docs 6.57 records the measured cost and this departure).
struct PaintScratch {
    std::vector<float> lo, hi;
    std::vector<float> phosphor;       // w * h intensities 0..1 of the vector figures still glowing
    std::vector<Rgba> deposit;         // the live figure's coverage, one frame
    int pw = 0, ph = 0;
    std::uint64_t key = 0;             // what the phosphor belongs to (signal, time base, size): a change clears it
    double ghostPeak = 0.0;            // the brightest pixel of the phosphor after the last frame (a test reads it)
};

// The demod tube's glass (w >= 120, h >= 100 else it only clears): graticule, the signal selected, scan lines. Reads and updates the scope's memory (PERSIST). Returns the number of line
// segments it drew (a cost the tests and the bounded run's summary report).
std::size_t paintDemodFace(Rgba* px, int w, int h, DemodScope& scope, PaintScratch& scratch);

constexpr int kDemodMinW = 120, kDemodMinH = 100;

}  // namespace foxsdr::slintscope
