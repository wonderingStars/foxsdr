// globe_draw.hpp - WHAT EACH GLOBE VIEW DRAWS, as a renderer-neutral list (docs/engine-stage4.md 6.65). The five builders turn the logic's scenes (globe_logic.hpp) into
// a GlobeDrawList - a textured sphere, 3D polylines, markers, labels - and know nothing of pixels; globe_compositor.* hands the list to the scene renderer (scene3d.*,
// lane A's framework) and gets a picture. The split keeps every decision about WHAT to draw (the layers, the colours, the lifts, the empty states) testable without a
// renderer, and the renderer's own tests about HOW.
//
// THE EARTH TEXTURE. The globe's surface is one equirectangular RGBA picture baked here from the coastline's land mask, the sun's position (night side, greyline band) or the
// GOES picture; the renderer samples it on the sphere. A bake costs about 10 ms at 1024 x 512 and is redone only when its inputs change (the sun moves a degree in four
// minutes; the bake key says so).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "globe_logic.hpp"

namespace foxsdr::globe {

// ---- the palette: the artboards' dark navy globe (every theme frames it in a dark well, SATVIEW.md) ----
struct Palette {
    std::uint32_t oceanDay = 0xFF244a82u, oceanNight = 0xFF101e3eu;
    std::uint32_t landDay = 0xFF4b7a91u, landNight = 0xFF1b3447u;
    std::uint32_t graticule = 0x55aebfdfu;       // translucent: ARGB
    std::uint32_t equator = 0x88aebfdfu;
    std::uint32_t limb = 0xFFF5A524u;            // the GOES disk edge, dashed orange
    std::uint32_t text = 0xFFE6ECF5u;
    std::uint32_t textDim = 0xFF98a6bfu;
    std::uint32_t white = 0xFFFFFFFFu;
    std::uint32_t sun = 0xFFFFD27Au;
    std::uint32_t orbitRing = 0x66aab4c8u;
    std::uint32_t greyline = 0xFFF5C26Bu;        // the warm tint of the twilight band
};
const Palette& palette();

// ---- the surface ----
struct Texture {
    int width = 0, height = 0;                   // equirectangular: column 0 is the -180 meridian, row 0 the north edge
    std::vector<std::uint32_t> argb;
    std::uint64_t key = 0;                       // what it was baked from (for the caller's cache)
    bool empty() const { return argb.empty(); }
};
// The land and sea, shaded by the Sun when `sun` is given: day colours where the Sun is up, night colours where it is not, a smooth ramp across +/- 6 degrees of solar
// elevation, and (greyline) a warm tint along the terminator. `sun` null: everything lit.
void bakeEarth(Texture& out, const LandMask& land, const SunPosition* sun, bool greyline, int width = 1024, int height = 512);
// The cache key of a bake: the sun's position rounded to a quarter of a degree (about a minute of the Earth's turn), the greyline flag, the size.
std::uint64_t earthBakeKey(const SunPosition* sun, bool greyline, int width, int height);
// The GOES picture draped: the equirectangular texture of what a geostationary satellite over satLonDeg saw, sampled from `pixels` (gray8 or rgb24, w x h) through the
// fixed-grid navigation; ground behind the limb or outside the frame shows the plain ocean, and so does a picture pixel that is exactly black (nothing received there).
// The picture's grey is tinted toward white so clouds read against the ocean.
void bakeGoes(Texture& out, const std::vector<std::uint8_t>& pixels, int w, int h, bool rgb, double satLonDeg, int width = 1024, int height = 512);

// ---- the list ----
struct DrawLine {
    std::vector<Vec3> points;                    // on or above the unit sphere (or in the orbits view's own units)
    std::uint32_t argb = 0;
    float widthPx = 1.5f;
    bool dashed = false;
    bool closed = false;
    bool surface = true;                         // true: hidden when behind the globe; false: always drawn (an orbit ring seen through the Sun)
};
enum class MarkerShape { Dot = 0, Triangle, Square, Ring, Star };
struct DrawMarker {
    Vec3 at;
    MarkerShape shape = MarkerShape::Dot;
    std::uint32_t argb = 0;
    float sizePx = 5.0f;
    std::string label;                           // drawn beside the marker when not empty
    bool hiddenBehind = true;                    // hidden when the globe is in front of it
};
struct DrawLabel {
    Vec3 anchor;
    std::string text;
    std::uint32_t argb = 0;
    bool hiddenBehind = true;
};
struct GlobeDrawList {
    enum class Space { Globe, Orbits } space = Space::Globe;
    GlobeCamera camera;                          // Globe: the point looked at and the zoom. Orbits: centreLon = spin, centreLat = tilt above the ecliptic, zoom
    bool sphere = true;
    double sphereRadius = 1.0;
    Texture texture;                             // the surface (Globe)
    std::vector<DrawLine> lines;
    std::vector<DrawMarker> markers;
    std::vector<DrawLabel> labels;
    std::string emptyNote;                       // the honest empty state's words ("" = there is something to draw), drawn over the well by the page
};

// The lift of an altitude above the sphere, as a fraction of the radius, drawn at true scale up to 0.25 (1 600 km); higher satellites sit at that height ("not to scale").
double altitudeLift(double altKm);

// ---- the builders: each takes what the page's logic already computed ----
GlobeDrawList drawPropagation(const PropagationScene& scene, const ViewState& view, const Texture& earth);
GlobeDrawList drawFt8(const std::vector<Ft8Spot>& spots, bool rxKnown, const LatLon& rx, const ViewState& view, const Texture& earth);
// goesLookValid false hides the RX marker (no receiver position).
GlobeDrawList drawGoes(const ViewState& view, double satLonDeg, bool rxKnown, const LatLon& rx, const Texture& surface);
GlobeDrawList drawOrbits(const OrbitScene& scene, const ViewState& view);
struct SatSceneIn {
    std::vector<SatIn> sats;
    std::vector<SatPathIn> paths;
    std::string selectedId;
    bool rxKnown = false;
    LatLon rx;
    bool footprints = true;
    bool groundTracks = true;
};
GlobeDrawList drawSatellites(const SatSceneIn& in, const ViewState& view, const Texture& earth);

// The camera a view starts at when the user has not touched it: the globe looks at the home / receiver / satellite where that is known.
GlobeCamera initialCamera(ViewKind k, bool homeKnown, const LatLon& home);

}  // namespace foxsdr::globe
