// patch_faces.hpp - THE PATCH PAGE'S TWO LIVE FACES (docs/multi-radio-records/patch-faces.md; ledger rows 1924, 1925, 1940): the Spectrum part's trace and waterfall, and the Map part's
// chart. patch_logic.hpp says WHAT a part shows (the slice of the band, the markers, the two sentences); this file holds what draws it, and nothing here is a Slint type or an engine
// call (the publisher, patch_publish.cpp, reads the engine and copies the pixels into the window's images), so the pictures are tested without a window.
//
// THE SPECTRUM WELL reuses the main receiver's painters: the trace is host_logic.hpp's paintTrace and the waterfall its scrollWaterfallRow (the same colour map, the same scroll), given a
// picture of the part's own size and the columns of the part's own slice. The slice is cut out of the radio's wideband frame by gui/patch_scope_math.hpp's binsToColumns (the loudest bin
// of each column, so a narrow carrier survives a small part) and scaled by its autoRange: the ImGui window's own arithmetic for the same part.
//
// THE MAP FACE keeps one view and one compositor per Map part. The compositor is the map pages' (map_compositor.hpp: the basemap's tiles or the coastline, the graticule, the rings), the view
// moves as the map pages' does (a pan, a wheel notch, the zoom keys), and the targets are those of the decoders wired in, projected by that compositor into the items MapPane places.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "host_logic.hpp"
#include "map_compositor.hpp"
#include "patch_logic.hpp"

namespace foxsdr::slintpatch {

// ---- the Spectrum well ----
// The size a part's pictures are painted at, from the node's own size (world pixels at zoom 1): the face is the well less 22 px across and 40 px down; the trace takes the upper 38 %, the
// waterfall the rest less a 2 px gap (the ImGui window's own proportions). Painted at one pixel a world pixel and scaled to the zoom by the window; capped, so a part dragged huge costs a
// bounded picture.
struct WellSize {
    std::uint32_t w = 0, traceH = 0, wfH = 0;
    bool usable() const { return w >= 16 && traceH >= 4 && wfH >= 4; }
    bool operator==(const WellSize& o) const { return w == o.w && traceH == o.traceH && wfH == o.wfH; }
    bool operator!=(const WellSize& o) const { return !(*this == o); }
};
constexpr std::uint32_t kWellMaxW = 640, kWellMaxTraceH = 120, kWellMaxWfH = 200;
WellSize wellSizeOf(float nodeW, float nodeH);

class SpectrumWell {
public:
    // Reshapes to `size` for a slice [lo, hi] of a capture at `rate`; anything that differs from the last call starts the picture afresh (rows of another span or width cannot be stacked).
    // True when it did.
    bool shape(const WellSize& size, double loHz, double hiHz, double rateHz);
    // Takes one frame (dBFS per bin, fftshifted, bin 0 at -rate/2) as the newest row and repaints both pictures. False and nothing painted when the frame cannot be cut to the slice.
    bool push(const std::vector<float>& frame);
    void clear();

    const std::vector<foxsdr::slinthost::Rgba>& trace() const { return trace_; }       // w * traceH, transparent ground
    const std::vector<foxsdr::slinthost::Rgba>& waterfall() const { return picture_; } // w * wfH, newest row at the top
    const WellSize& size() const { return size_; }
    std::uint64_t frames() const { return frames_; }                                    // frames pushed since the last shape
    float rangeLo() const { return rangeLo_; }
    float rangeHi() const { return rangeHi_; }
    // What the last frame's columns held: the column of the loudest one (0..w-1, -1 none) and its level; the median column's level (the floor). Measured from the columns the pictures were
    // painted from, so a drive script and a test read the same figures the eye does.
    int peakColumn() const { return peakColumn_; }
    float peakDb() const { return peakDb_; }
    float floorDb() const { return floorDb_; }
    // A fingerprint of the two pictures' pixels, for "the well changed between two looks".
    std::uint64_t traceHash() const;
    std::uint64_t waterfallHash() const;
    int litWaterfallRows() const;                                                       // rows of the waterfall that are not the cleared ground

private:
    WellSize size_;
    double lo_ = 0.0, hi_ = 0.0, rate_ = 0.0;
    std::vector<foxsdr::slinthost::Rgba> trace_, picture_;
    foxsdr::slinthost::TraceState traceState_;
    std::vector<float> cols_;
    std::uint64_t frames_ = 0;
    float rangeLo_ = 0.0f, rangeHi_ = 0.0f;
    int peakColumn_ = -1;
    float peakDb_ = 0.0f, floorDb_ = 0.0f;
};

// ---- the Map face ----
// The view of one Map part, the map pages' (kit_logic's MapPage: a centre and the longitude span across the width).
struct MapFaceView {
    double centreLat = 0.0, centreLon = 0.0, spanDeg = 360.0;
    bool fitted = false;       // fitted to the targets once (the first time there are any); the Whole world key and any pan or zoom keep it from refitting under the hand
};
// The span the whole planet needs across `w` x `h` pixels (map_view.cpp's wholeWorldSpanDeg: 180 degrees of latitude in the height, never less than 360 across).
double wholeWorldSpan(double w, double h);
// WHOLE WORLD: the centre of the planet and the span above (a Mercator basemap keeps the flat 360), and the view counts as fitted so the first targets do not pull it away.
void wholeWorld(MapFaceView& v, double w, double h, bool mercator);
// FIT TO TARGETS: the map pages' fitView over the targets that have a position and that `present` says are still shown (the core's trackPresentation: a target gone stale is not fitted
// to); false and the view untouched when none is. `present` is passed in so this unit needs none of the plugin runtime the rule lives with (the publisher hands in the core's own).
using PresentFn = cascade::core::TrackPresentation (*)(const CascadeTrack&);
bool fitToTargets(MapFaceView& v, const std::vector<cascade::core::HostTrack>& tracks, double w, double h, PresentFn present);

// What MapPane places for one target (ui/map.slint's MapTarget before the window's types): a position as fractions of the picture, the heading, kind, label, fade and flags.
struct MapFaceTarget {
    std::string id, label;
    float x = 0.0f, y = 0.0f, heading = -1.0f, alpha = 1.0f;
    int kind = 0;
    bool estimated = false, emergency = false;
    float size = 0.0f;
};
// The targets of `tracks` (only those a Map part draws: visible, with a position, on the picture), at most kMaxFaceTargets, projected by `comp`.
constexpr std::size_t kMaxFaceTargets = 300;
std::vector<MapFaceTarget> projectTargets(const std::vector<cascade::core::HostTrack>& tracks, const foxsdr::slintmap::TileCompositor& comp, const foxsdr::slintmap::MapLook& look, PresentFn present);
// The tracks one Map part draws: those of the modules wired into it (pc::mapSources, by module name), whatever radio each one hangs off.
std::vector<cascade::core::HostTrack> tracksOfSources(const std::vector<cascade::core::HostTrack>& all, const std::vector<std::string>& sources);

// Pan: the view moves so the picture follows the hand by (dx, dy), fractions of the picture (the map pages' arithmetic over the compositor's own unproject).
void panView(MapFaceView& v, const foxsdr::slintmap::TileCompositor& comp, float dx, float dy);
// Zoom by `factor` (below 1 in) about the point (fx, fy) of the picture, which stays where it is.
void zoomView(MapFaceView& v, const foxsdr::slintmap::TileCompositor& comp, double factor, float fx, float fy);

}  // namespace foxsdr::slintpatch
