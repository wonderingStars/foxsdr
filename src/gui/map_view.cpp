// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/map_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "gui/basemap_cache.hpp"
#include "gui/coastline_data.hpp"
#include "gui/fonts.hpp"
#include "gui/scope_face.hpp"
#include "gui/theme.hpp"
#include "gui/track_detail_view.hpp"
#include "gui/track_info_cache.hpp"
#include "imgui.h"

namespace cascade::gui {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusKm = 6371.0;

// Per-kind colours. Chosen to stay distinguishable when several kinds share
// the map, which is the case this whole design exists to support.
ImU32 colourFor(std::uint32_t kind) {
    ImU32 c;
    switch (kind) {
        // Red for aircraft. The pale blue this used to be is legible over a
        // dark empty map and nearly invisible over street tiles, which are
        // pale and busy - and street tiles are now the normal case whenever a
        // basemap plugin is installed. Red is the one hue an OSM raster style
        // does not use for large areas (its reds are thin motorway lines), so
        // an aircraft stays findable against it.
        case CASCADE_TRACK_AIRCRAFT: c = IM_COL32(235, 40, 40, 255); break;
        case CASCADE_TRACK_VESSEL:   c = IM_COL32(120, 255, 170, 255); break;
        case CASCADE_TRACK_STATION:  c = IM_COL32(255, 210, 120, 255); break;
        case CASCADE_TRACK_SATELLITE:c = IM_COL32(230, 150, 255, 255); break;
        default:                     c = IM_COL32(200, 200, 200, 255); break;
    }
    return c;
}

// WHAT COLOUR A TARGET ACTUALLY DRAWS IN, in strict order of precedence:
//
//   1. EMERGENCY wins over everything. A squawk of 7500/7600/7700 - which is
//      what a plugin sets CASCADE_TRACK_FLAG_EMERGENCY for - is the one thing
//      on the map that must never be mistaken for anything else, so it takes
//      the one hue the altitude palette deliberately does not contain. (The
//      flag has been in the ABI since the beginning and nothing drew it until
//      now; an aircraft in trouble was the same red as the one beside it.)
//   2. ALTITUDE BAND, when the altitude is known. This is what makes an
//      approach and a cruise read differently at a glance, which is the whole
//      point of colouring by altitude at all.
//   3. THE KIND COLOUR, when it is not. altM is NaN by ABI contract when the
//      source does not report it, and a ship or an APRS station reporting no
//      altitude is not a thing at sea level - it is a thing whose altitude is
//      not part of what it transmits. Those keep the per-kind colour they have
//      always had, which is also what keeps several kinds on one map
//      distinguishable.
//
// Selection is not in this list because it is not a colour: a selected target
// gets a RINGED marker in whatever colour it already had (see the draw loop),
// so it stands out without hiding what its altitude was.

// WHICH LADDER A KIND IS READ ON, and this is the whole of the rule. The
// altitude ladder in track_metrics.hpp is an AVIATION ladder: its top band
// opens at 30 kft, and the ISS at 421 km is 1,381,000 ft, so every satellite
// this application has ever tracked fell in one band, drew in one colour, and
// sat under a legend reading "> 30 kft" - a unit nothing in orbit is anywhere
// near. The orbital ladder bands the same measurement in kilometres, where the
// eight targets a stock installation follows separate across four bands.
//
// BY KIND, NOT BY VALUE. Choosing the ladder from the number itself - "above
// 100 km, call it orbital" - would put a sounding rocket and a very low
// satellite on different ladders for the same reading, and would silently
// re-band an aircraft the moment a plugin reported a nonsense altitude. The
// ABI says what a thing IS; that is what decides how its altitude is read.
bool orbitalLadder(std::uint32_t kind) { return kind == CASCADE_TRACK_SATELLITE; }

int bandIndexFor(std::uint32_t kind, double altM) {
    return orbitalLadder(kind) ? orbitBandIndex(altM) : altitudeBandIndex(altM);
}

const AltBandStyle& bandStyleFor(std::uint32_t kind, int index) {
    return orbitalLadder(kind) ? orbitBandStyle(index) : altBandStyle(index);
}

int bandCountFor(std::uint32_t kind) {
    return orbitalLadder(kind) ? kOrbitBandCount : kAltBandCount;
}

ImU32 colourForTrack(const CascadeTrack& t) {
    if ((t.flags & CASCADE_TRACK_FLAG_EMERGENCY) != 0u) {
        return IM_COL32(255, 45, 45, 255);
    }
    const int band = bandIndexFor(t.kind, t.altM);
    if (band < 0) { return colourFor(t.kind); }
    const AltBandStyle& s = bandStyleFor(t.kind, band);
    return IM_COL32(s.r, s.g, s.b, 255);
}

// Applies a 0..1 fade to a colour's alpha. Faded rather than removed while the
// target is merely quiet: a target that stopped reporting a moment ago is
// still information, and making it vanish the instant it goes quiet loses the
// last known position exactly when it matters. Removal comes later, and from
// cascade::core::trackPresentation, which knows what cadence this KIND reports
// at - the one thing a single "older than a minute" test could never know.
ImU32 fadedColour(ImU32 c, float alpha) {
    const float a = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFFu) * alpha;
    unsigned int v = static_cast<unsigned int>(a + 0.5f);
    if (v > 255u) { v = 255u; }
    return (c & ~(0xFFu << IM_COL32_A_SHIFT)) | (v << IM_COL32_A_SHIFT);
}

// Aircraft draw as a plane silhouette rather than a dot, rotated to the
// reported course; the SELECTED aircraft is the same silhouette knocked out
// of a filled disc, so the one being watched reads at a glance. The table is
// the RIGHT half of the outline, nose up, in a unit box (y negative toward
// the nose); the left half is the mirror walked backwards, which keeps the
// two sides identical by construction. The shape is concave, hence
// AddConcavePolyFilled.
// Half-width of a ribbon trail, in screen pixels. Wide enough to read as a
// band rather than a fat line, narrow enough that several aircraft converging
// on an approach do not merge into one shape.
constexpr float kTrailRibbonHalfPx = 3.0f;

constexpr float kPlaneHalf[][2] = {
    {0.00f, -1.00f},  // nose
    {0.13f, -0.70f},  // cockpit taper
    {0.13f, -0.26f},  // wing root, leading edge
    {0.98f, 0.16f},   // wing tip, leading edge
    {0.98f, 0.38f},   // wing tip, trailing edge
    {0.13f, 0.20f},   // wing root, trailing edge
    {0.13f, 0.62f},   // fuselage ahead of the tail
    {0.48f, 0.88f},   // tailplane tip, leading edge
    {0.48f, 1.02f},   // tailplane tip, trailing edge
    {0.08f, 0.94f},   // tail root
    {0.00f, 0.96f},   // tail, on the centreline
};
constexpr int kPlaneHalfCount = static_cast<int>(sizeof(kPlaneHalf) / sizeof(kPlaneHalf[0]));

// `filled` false draws the same silhouette as an OUTLINE. It is how an aircraft
// with NO REPORTED ALTITUDE is told apart from one at sea level: those two are
// different facts, they must not look alike, and a hue comparison at nine
// pixels is not a reliable way to tell them apart - a hollow shape against a
// solid one is. Only aircraft get this cue, because altitude is a thing an
// aircraft is expected to report and a ship or a base station is not.
void addPlane(ImDrawList* dl, const ImVec2& c, double courseDeg, float scale, ImU32 col,
              bool filled = true) {
    // Course 0 is north, which on screen is straight up; an unknown course
    // (NaN by ABI contract) draws the plane pointing north rather than
    // inventing a heading line the way the tick for other kinds would.
    const double a = (std::isnan(courseDeg) ? 0.0 : courseDeg) * kPi / 180.0;
    const float ca = static_cast<float>(std::cos(a));
    const float sa = static_cast<float>(std::sin(a));
    ImVec2 pts[2 * kPlaneHalfCount - 2];
    int n = 0;
    const auto put = [&](float x, float y) {
        pts[n++] = ImVec2(c.x + (x * ca - y * sa) * scale, c.y + (x * sa + y * ca) * scale);
    };
    for (int i = 0; i < kPlaneHalfCount; ++i) { put(kPlaneHalf[i][0], kPlaneHalf[i][1]); }
    // Mirror, skipping both centreline points so no vertex repeats.
    for (int i = kPlaneHalfCount - 2; i >= 1; --i) {
        put(-kPlaneHalf[i][0], kPlaneHalf[i][1]);
    }
    // A BLACK RIM ON EVERY PLANE, fading with the marker. Requested after use
    // over real basemap tiles: a small red silhouette over urban tile colours
    // or another aircraft's trail loses its edge, and the rim is what keeps it
    // reading as a shape rather than a smudge. The rim takes its alpha FROM
    // the fill colour so an ageing target fades as one thing - a solid black
    // outline around a ghost would read as a different, newer object.
    const ImU32 rim = IM_COL32(0, 0, 0, (col >> IM_COL32_A_SHIFT) & 0xFFu);
    if (filled) {
        dl->AddConcavePolyFilled(pts, n, col);
        dl->AddPolyline(pts, n, rim, ImDrawFlags_Closed, 1.5f);
    } else {
        // The hollow variant is a CUE (no reported altitude - see above), so
        // the black cannot replace the coloured outline; it goes UNDER it,
        // wider, as a halo. The cue survives, the contrast arrives.
        dl->AddPolyline(pts, n, rim, ImDrawFlags_Closed, 3.25f);
        dl->AddPolyline(pts, n, col, ImDrawFlags_Closed, 1.5f);
    }
}

// EVERY WORD ON THIS MAP IS DRAWN OVER SOMEBODY ELSE'S PICTURE, so its
// contrast cannot come from the palette.
//
// This is the same argument as the black rim on a plane above, and it was
// learned the same way - by looking. The bench palette is built for dark
// enamel: cream letters a control, and cream is exactly right on the toolbar
// and on the rail. Put that cream over an OpenStreetMap raster of the English
// Midlands - pale green fields, near-white towns, blue sea - and the receiver's
// own "RX" legend disappears completely, along with the ring distances beside
// it. The tiles come from whichever basemap plugin the user installed and can
// be any brightness at all, so no single ink is safe on them.
//
// A halo is: a dark outline that does not depend on what is underneath. It
// takes its alpha from the text, so a target label fading with age fades as
// one thing rather than leaving a hard black ghost of itself behind.
//
// AND THE INK INSIDE IT HAS TO BE BRIGHT. The first attempt kept each label
// the tone its ROLE gives it - the graticule and the ring distances engraved
// in kInkMuted and kInkFaint, which is right for a legend cut into brass. Over
// tiles they came out as dark letters inside a dark outline, which is worse
// than the problem it was fixing: legible only because the halo made them
// thicker. So over-map captions take cream, and the engraved tones stay where
// there is actually brass behind them. Target labels keep their altitude
// colour, which is a measurement and must not be repainted for contrast.
// AND THE HALO IS HALF-STRENGTH, WHICH IS THE WHOLE CRAFT OF IT. The four
// offsets overlap wherever two of them cover the same pixel, so a halo drawn
// at the text's own alpha compounds to solid black and closes the counters of
// every small glyph: "10 km" and "RX" became legible blobs, which is a
// different failure from being invisible and not much of an improvement.
// Roughly half stacks to about three quarters where it doubles - enough
// separation from any tile, and the letterforms stay open.
void addMapLabel(ImDrawList* dl, const ImVec2& at, ImU32 col, const char* text) {
    if (dl == nullptr || text == nullptr || text[0] == '\0') { return; }
    const unsigned int a = (col >> IM_COL32_A_SHIFT) & 0xFFu;
    const ImU32 halo = IM_COL32(0, 0, 0, (a * 120u) / 255u);
    dl->AddText(ImVec2(at.x - 1.0f, at.y), halo, text);
    dl->AddText(ImVec2(at.x + 1.0f, at.y), halo, text);
    dl->AddText(ImVec2(at.x, at.y - 1.0f), halo, text);
    dl->AddText(ImVec2(at.x, at.y + 1.0f), halo, text);
    dl->AddText(at, col, text);
}

// The private great-circle and bearing helpers that used to live here are gone
// too: gui/track_metrics.hpp now holds the tested pair, and this view's hover
// readout, the track table's columns and the coverage accumulator all measure
// with the same arithmetic instead of with three copies of it. The old local
// bearing also answered 000 for a target directly overhead, where the tested
// one says explicitly that there is no such direction.

// The span a fit falls back to when there is exactly one target, which has
// no extent of its own to fit. Wide enough to place a marker in a country
// rather than in blank blue.
constexpr double kLoneTargetSpanDeg = 24.0;

// Graticule step that keeps roughly 4-10 lines across the view at any zoom.
double graticuleStep(double spanDeg) {
    static const double steps[] = {0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 45};
    for (double s : steps) {
        if (spanDeg / s <= 10.0) { return s; }
    }
    return 45.0;
}

// wholeWorldSpanDeg used to live here, in this anonymous namespace. It is now
// declared in map_view.hpp and defined below, unchanged, so the span a
// no-position satellite page opens at can be pinned by a test instead of only
// being looked at.

// --- LAND AS AREA, NOT AS AN OUTLINE -----------------------------------------
//
// The compiled-in Natural Earth data is the COASTLINE layer: 134 open and
// closed POLYLINES, not polygons. Stroked over the well it reads as a
// wireframe - which is not what the design has, and not what a chart looks
// like. It can be filled, but only after three things a line loop never had to
// care about:
//
//   THE RINGS HAVE TO BE ASSEMBLED. 120 of the 134 runs already close on
//   themselves: islands and small landmasses. The other 14 are FRAGMENTS the
//   source cut at the antimeridian - the Americas arrive in four pieces,
//   Eurasia-and-Africa in three, Fiji and Wrangel Island in two each, and
//   Antarctica as one run that leaves the world at -180 and re-enters at +180.
//   They are chained end to end below, treating -180 and +180 as the same
//   meridian, by a depth-first search over the fragments rather than a greedy
//   walk: a greedy walk takes the first endpoint that matches, and at the
//   Bering Strait that is a two-point stub which swallows the join Eurasia
//   actually needed and leaves the largest ring in the world unclosed. Two
//   short stubs join nothing and stay as open lines.
//
//   ANTARCTICA HAS NO SOUTHERN COAST in this data. Closing it directly draws a
//   straight line at its last coastal latitude and fills only down to there,
//   leaving the pole as sea and cutting off the ice shelves below it. A chain
//   whose two ends are a whole world apart gets two POLE points inserted.
//
//   THE CASPIAN IS IN THE COASTLINE LAYER, as a closed ring INSIDE the Eurasian
//   one, so filling every ring paints an inland sea as land - a false statement
//   about geography drawn at full size. Rings are classified by how many larger
//   rings contain them; an odd count is water, and those are painted back in
//   the map's own ground after the land.
//
// TRIANGULATED ONCE, NEVER PER FRAME. Ear clipping is O(n^2) and the Eurasian
// ring is 1,318 points, so doing it every frame would cost more than the rest
// of the map put together. Both projections drawn here are affine in longitude
// and strictly monotonic in latitude, which is a homeomorphism of the plane: a
// triangulation that is valid in degrees stays valid on screen. So the
// triangles are computed once, in degrees, and only their vertices are
// projected.
//
// AND THE GEOMETRY IS KEPT IN A CONTINUOUS LONGITUDE, un-wrapped, with each
// ring drawn once per world-copy that the view touches. Wrapping each vertex
// into +/-180 separately is what drew the basemap in mirror writing in 0.70.0;
// the rule that came out of it - wrap once, for the whole extent, never per
// point - is what this follows.
struct LandShape {
    // x is longitude in degrees and may run outside +/-180; y is latitude.
    std::vector<ImVec2> pts;
    // Three indices into pts per triangle. Empty for a fragment that never
    // closed, which is drawn as a line and cannot be filled.
    std::vector<std::uint16_t> tri;
    float minLon = 0.0f;
    float maxLon = 0.0f;
    float minLat = 0.0f;
    float maxLat = 0.0f;
    bool closed = false;
    // Odd nesting depth: an inland sea, painted in the map's ground rather
    // than in land.
    bool water = false;
};

double ringSignedArea(const std::vector<ImVec2>& p) {
    double s = 0.0;
    const std::size_t n = p.size();
    for (std::size_t i = 0; i < n; ++i) {
        const ImVec2& a = p[i];
        const ImVec2& b = p[(i + 1) % n];
        s += static_cast<double>(a.x) * static_cast<double>(b.y) -
             static_cast<double>(b.x) * static_cast<double>(a.y);
    }
    return s * 0.5;
}

double cross3(const ImVec2& a, const ImVec2& b, const ImVec2& c) {
    return (static_cast<double>(b.x) - a.x) * (static_cast<double>(c.y) - a.y) -
           (static_cast<double>(b.y) - a.y) * (static_cast<double>(c.x) - a.x);
}

bool pointInRing(const ImVec2& q, const std::vector<ImVec2>& p) {
    bool in = false;
    const std::size_t n = p.size();
    for (std::size_t i = 0; i < n; ++i) {
        const ImVec2& a = p[i];
        const ImVec2& b = p[(i + 1) % n];
        if ((a.y > q.y) != (b.y > q.y)) {
            const double t = static_cast<double>(q.y - a.y) / static_cast<double>(b.y - a.y);
            if (static_cast<double>(q.x) < a.x + t * (static_cast<double>(b.x) - a.x)) {
                in = !in;
            }
        }
    }
    return in;
}

// Ear clipping, run once per ring at start-up. The guard is not decoration: a
// ring with a repeated vertex or a zero-area spur has no ear at some step, and
// without the forced clip the loop would not terminate. Forcing one produces a
// sliver triangle - a defect measured in fractions of a pixel - rather than a
// hang, and the whole-ring area check that verified this data found none.
void earClipRing(const std::vector<ImVec2>& p, std::vector<std::uint16_t>& out) {
    out.clear();
    const int n = static_cast<int>(p.size());
    if (n < 3 || n > 65535) { return; }
    std::vector<int> nxt(static_cast<std::size_t>(n));
    std::vector<int> prv(static_cast<std::size_t>(n));
    const bool ccw = ringSignedArea(p) > 0.0;
    for (int i = 0; i < n; ++i) {
        const int fwd = ccw ? (i + 1) % n : (i + n - 1) % n;
        const int back = ccw ? (i + n - 1) % n : (i + 1) % n;
        nxt[static_cast<std::size_t>(i)] = fwd;
        prv[static_cast<std::size_t>(i)] = back;
    }
    out.reserve(static_cast<std::size_t>(n - 2) * 3u);
    int remaining = n;
    int cur = 0;
    int sinceEar = 0;
    while (remaining > 3) {
        const int a = prv[static_cast<std::size_t>(cur)];
        const int b = cur;
        const int c = nxt[static_cast<std::size_t>(cur)];
        bool ear = cross3(p[static_cast<std::size_t>(a)], p[static_cast<std::size_t>(b)],
                          p[static_cast<std::size_t>(c)]) > 0.0;
        if (ear) {
            for (int v = nxt[static_cast<std::size_t>(c)]; v != a;
                 v = nxt[static_cast<std::size_t>(v)]) {
                const ImVec2& q = p[static_cast<std::size_t>(v)];
                if (cross3(p[static_cast<std::size_t>(a)], p[static_cast<std::size_t>(b)], q) >=
                        0.0 &&
                    cross3(p[static_cast<std::size_t>(b)], p[static_cast<std::size_t>(c)], q) >=
                        0.0 &&
                    cross3(p[static_cast<std::size_t>(c)], p[static_cast<std::size_t>(a)], q) >=
                        0.0) {
                    ear = false;
                    break;
                }
            }
        }
        if (!ear && sinceEar <= n) {
            cur = nxt[static_cast<std::size_t>(cur)];
            ++sinceEar;
            continue;
        }
        out.push_back(static_cast<std::uint16_t>(a));
        out.push_back(static_cast<std::uint16_t>(b));
        out.push_back(static_cast<std::uint16_t>(c));
        nxt[static_cast<std::size_t>(a)] = c;
        prv[static_cast<std::size_t>(c)] = a;
        --remaining;
        cur = a;
        sinceEar = 0;
    }
    out.push_back(static_cast<std::uint16_t>(prv[static_cast<std::size_t>(cur)]));
    out.push_back(static_cast<std::uint16_t>(cur));
    out.push_back(static_cast<std::uint16_t>(nxt[static_cast<std::size_t>(cur)]));
}

// Longitude in hundredths of a degree folded into [-18000, 18000), so that the
// two spellings of the antimeridian compare equal and a chain carried past it
// still matches the fragment waiting on the other side.
int foldLon(int hundredths) {
    long v = (static_cast<long>(hundredths) + 18000L) % 36000L;
    if (v < 0) { v += 36000L; }
    return static_cast<int>(v - 18000L);
}

std::vector<LandShape> buildLandShapes() {
    std::vector<LandShape> shapes;
    const auto rawLon = [](std::uint32_t run, std::uint32_t i) {
        return static_cast<int>(
            coastline::kCoords[(static_cast<std::size_t>(coastline::kRuns[run].first) + i) * 2u]);
    };
    const auto rawLat = [](std::uint32_t run, std::uint32_t i) {
        return static_cast<int>(
            coastline::kCoords[(static_cast<std::size_t>(coastline::kRuns[run].first) + i) * 2u +
                               1u]);
    };

    std::vector<std::uint32_t> fragments;
    std::vector<std::pair<int, int>> chain;  // hundredths, longitude un-wrapped
    for (std::uint32_t r = 0; r < coastline::kRunCount; ++r) {
        const std::uint32_t last = coastline::kRuns[r].count - 1u;
        if (rawLon(r, 0) == rawLon(r, last) && rawLat(r, 0) == rawLat(r, last)) {
            chain.clear();
            for (std::uint32_t i = 0; i < last; ++i) {
                chain.emplace_back(rawLon(r, i), rawLat(r, i));
            }
            LandShape s;
            s.closed = true;
            for (const auto& q : chain) {
                s.pts.emplace_back(static_cast<float>(q.first) / 100.0f,
                                   static_cast<float>(q.second) / 100.0f);
            }
            shapes.push_back(std::move(s));
        } else {
            fragments.push_back(r);
        }
    }

    // Depth-first, with backtracking, over the fragments. See the note above:
    // the greedy version of this loses Eurasia to a two-point stub.
    std::vector<bool> consumed(fragments.size(), false);
    std::vector<std::size_t> path;
    const auto closes = [&](const std::vector<std::pair<int, int>>& c) {
        return c.size() >= 4u && foldLon(c.back().first) == foldLon(c.front().first) &&
               c.back().second == c.front().second;
    };
    auto extend = [&](auto&& self, std::vector<std::pair<int, int>>& c) -> bool {
        if (closes(c)) { return true; }
        for (std::size_t f = 0; f < fragments.size(); ++f) {
            if (consumed[f]) { continue; }
            if (std::find(path.begin(), path.end(), f) != path.end()) { continue; }
            const std::uint32_t run = fragments[f];
            const std::uint32_t cnt = coastline::kRuns[run].count;
            for (int reversed = 0; reversed < 2; ++reversed) {
                const std::uint32_t head = reversed ? (cnt - 1u) : 0u;
                if (foldLon(rawLon(run, head)) != foldLon(c.back().first) ||
                    rawLat(run, head) != c.back().second) {
                    continue;
                }
                const int off = c.back().first - rawLon(run, head);
                const std::size_t grew = c.size();
                for (std::uint32_t k = 1; k < cnt; ++k) {
                    const std::uint32_t i = reversed ? (cnt - 1u - k) : k;
                    c.emplace_back(rawLon(run, i) + off, rawLat(run, i));
                }
                path.push_back(f);
                if (self(self, c)) { return true; }
                path.pop_back();
                c.resize(grew);
            }
        }
        return false;
    };

    for (std::size_t f = 0; f < fragments.size(); ++f) {
        if (consumed[f]) { continue; }
        const std::uint32_t run = fragments[f];
        chain.clear();
        for (std::uint32_t i = 0; i < coastline::kRuns[run].count; ++i) {
            chain.emplace_back(rawLon(run, i), rawLat(run, i));
        }
        path.assign(1, f);
        LandShape s;
        if (extend(extend, chain)) {
            for (std::size_t idx : path) { consumed[idx] = true; }
            s.closed = true;
            const int startLon = chain.front().first;
            const int endLon = chain.back().first;
            const int wrapped = endLon - startLon;
            if (wrapped >= 35900 || wrapped <= -35900) {
                // A ring that went right round the world: Antarctica. Its ends
                // are the same meridian one world apart, so the run itself is
                // kept whole and taken to the pole and back.
                double meanLat = 0.0;
                for (const auto& q : chain) { meanLat += q.second; }
                meanLat /= static_cast<double>(chain.size());
                const int pole = (meanLat < 0.0) ? -9000 : 9000;
                chain.emplace_back(endLon, pole);
                chain.emplace_back(startLon, pole);
            } else {
                chain.pop_back();  // the duplicate of the first point
            }
        } else {
            consumed[f] = true;
        }
        for (const auto& q : chain) {
            s.pts.emplace_back(static_cast<float>(q.first) / 100.0f,
                               static_cast<float>(q.second) / 100.0f);
        }
        shapes.push_back(std::move(s));
    }

    // Bounds, nesting and triangles. The nesting test is O(rings x points) and
    // runs once; the rings it has to consider are only those with a LARGER
    // area, which is what makes "how many contain me" the right question.
    for (LandShape& s : shapes) {
        s.minLon = s.maxLon = s.pts.empty() ? 0.0f : s.pts.front().x;
        s.minLat = s.maxLat = s.pts.empty() ? 0.0f : s.pts.front().y;
        for (const ImVec2& q : s.pts) {
            s.minLon = std::min(s.minLon, q.x);
            s.maxLon = std::max(s.maxLon, q.x);
            s.minLat = std::min(s.minLat, q.y);
            s.maxLat = std::max(s.maxLat, q.y);
        }
    }
    std::vector<double> areas(shapes.size(), 0.0);
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        if (shapes[i].closed) { areas[i] = std::fabs(ringSignedArea(shapes[i].pts)); }
    }
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        if (!shapes[i].closed || shapes[i].pts.empty()) { continue; }
        int depth = 0;
        for (std::size_t j = 0; j < shapes.size(); ++j) {
            if (i == j || !shapes[j].closed || areas[j] <= areas[i]) { continue; }
            if (pointInRing(shapes[i].pts.front(), shapes[j].pts)) { ++depth; }
        }
        shapes[i].water = (depth % 2) == 1;
        earClipRing(shapes[i].pts, shapes[i].tri);
    }
    return shapes;
}

const std::vector<LandShape>& landShapes() {
    static const std::vector<LandShape> shapes = buildLandShapes();
    return shapes;
}

}  // namespace

// See map_view.hpp. Guarded against a zero height because it divides by one:
// draw() will not call it with a viewport that small, and a figure that can
// only be produced by a caller that does not exist is still a figure this must
// not return.
double wholeWorldSpanDeg(float widthPx, float heightPx) {
    if (!(heightPx > 0.0f) || !(widthPx > 0.0f)) { return 360.0; }
    const double byHeight =
        180.0 * static_cast<double>(widthPx) / static_cast<double>(heightPx);
    return (byHeight > 360.0) ? byHeight : 360.0;
}

void MapView::setHome(double latDeg, double lonDeg) {
    homeLat_ = latDeg;
    homeLon_ = lonDeg;
    hasHome_ = true;
}

namespace {

// Web Mercator, normalised so the whole world is 0..1 in both axes - which is
// the form the tile grid is defined in: at zoom z the world is 2^z tiles, so a
// tile index is simply the normalised coordinate times 2^z.
double mercY(double latDeg) {
    const double lat = std::clamp(latDeg, -85.05112878, 85.05112878);
    const double s = std::sin(lat * 3.14159265358979323846 / 180.0);
    return 0.5 - std::log((1.0 + s) / (1.0 - s)) / (4.0 * 3.14159265358979323846);
}

double mercLat(double y) {
    const double n = 3.14159265358979323846 * (1.0 - 2.0 * y);
    return std::atan(std::sinh(n)) * 180.0 / 3.14159265358979323846;
}

}  // namespace

void MapView::goTo(double latDeg, double lonDeg, double spanDeg) {
    centreLat_ = latDeg;
    centreLon_ = lonDeg;
    // Tightened only, never loosened - see the header for why.
    if (spanDeg_ > spanDeg) { spanDeg_ = spanDeg; }
    // A go-to is an explicit instruction about where to look, so it also
    // cancels the pending initial fit; otherwise the first target to arrive
    // would immediately drag the view somewhere else.
    fitRequested_ = false;
    fittedOnce_ = true;
}

void MapView::draw(float width, float height,
                   const std::vector<cascade::core::HostTrack>& tracks,
                   const std::vector<cascade::core::HostPath>& paths,
                   BasemapCache* tiles, TrackInfoCache* info) {
    if (width < 32.0f || height < 32.0f) { return; }
    // A PAGE THAT HAS ASKED FOR EQUIRECTANGULAR DOES NOT GET TILES, and that
    // is the whole of the projection choice: the tile loop below is already
    // gated on `mercator`, and with it false the built-in coastline draws
    // instead. See MapProjection for why the two travel together.
    const bool mercator = projection_ == MapProjection::Automatic && tiles != nullptr &&
                          tiles->active();

    // HOW FAR OUT THIS MAP WILL GO, and it is the same figure in both places it
    // is needed - the initial whole-world view and the wheel's zoom-out stop -
    // so that zooming out lands exactly on the view a page opens at instead of
    // stopping short of it or overshooting and snapping back. Mercator keeps
    // the flat 360: its vertical extent is unbounded at the poles, so "180
    // degrees of latitude in the height" is not a span that exists.
    const double zoomOutLimitDeg = mercator ? 360.0 : wholeWorldSpanDeg(width, height);

    // THE WHOLE PLANET, WHICH IS WHERE A SATELLITE PAGE OPENS. Applied here
    // rather than at the moment it is asked for, because "the whole world" is
    // a span that depends on the viewport's shape, and the viewport's shape is
    // not known until a frame is being drawn. It also cancels the pending fit:
    // fitting to one propagated satellite is exactly what opened this window
    // on a 24-degree patch of Indonesia.
    if (wholeWorldRequested_) {
        centreLat_ = 0.0;
        centreLon_ = 0.0;
        spanDeg_ = zoomOutLimitDeg;
        wholeWorldRequested_ = false;
        fitRequested_ = false;
        fittedOnce_ = true;
    }

    // Fit to content the first time there is any, so the map does not open on
    // empty ocean while every target is somewhere else. FITTING IGNORES WHAT
    // IT WILL NOT DRAW, and waits for something it will: a source that never
    // evicts keeps reporting a target last heard hours ago and a thousand
    // miles away, and fitting to it zooms the map out to a continent to
    // include a marker that is not on screen at all.
    if (fitRequested_ && !tracks.empty()) {
        double minLat = 90.0, maxLat = -90.0, minLon = 180.0, maxLon = -180.0;
        bool any = false;
        for (const auto& ht : tracks) {
            if (!cascade::core::trackPresentation(ht.t.ageMs, ht.t.kind).visible) {
                continue;
            }
            any = true;
            minLat = std::min(minLat, ht.t.latDeg);
            maxLat = std::max(maxLat, ht.t.latDeg);
            minLon = std::min(minLon, ht.t.lonDeg);
            maxLon = std::max(maxLon, ht.t.lonDeg);
        }
        if (any) {
            centreLat_ = 0.5 * (minLat + maxLat);
            centreLon_ = 0.5 * (minLon + maxLon);

            // What the content needs across, and what it needs DOWN expressed
            // as the longitude span that would show it. Two things were wrong
            // here and both are corrected:
            //
            //  - the vertical requirement used a hard-coded factor of two,
            //    which is the right answer only for a window exactly twice as
            //    wide as it is tall. Every other shape either cropped targets
            //    off the top and bottom or zoomed out further than it needed.
            //
            //  - it compared DEGREES of latitude against degrees of longitude
            //    even under tiles, where the vertical scale is Mercator. Those
            //    are not interchangeable anywhere but the equator, and the
            //    error grows without bound towards the poles - which is
            //    exactly where polar-orbiting satellites spend their time.
            const double aspect =
                static_cast<double>(width) / static_cast<double>(height);
            const double needFromLat =
                mercator ? (mercY(minLat) - mercY(maxLat)) * 360.0 * aspect
                         : (maxLat - minLat) * aspect;
            double need = std::max(maxLon - minLon, needFromLat);

            // A SINGLE TARGET HAS NO EXTENT, and the old arithmetic turned
            // that into need = 0 and a half-degree span: the map slammed to
            // roughly fifty kilometres across, showing one marker adrift in
            // blank tiles with no way to tell where on earth it was. One
            // target is the FIRST thing a fresh install sees, so this was the
            // common case, not the corner one. Give it a view with enough
            // context to recognise, and let the user zoom.
            if (need < 1.0e-6) { need = kLoneTargetSpanDeg; }

            spanDeg_ = std::clamp(need * 1.25, 0.5, zoomOutLimitDeg);
            fitRequested_ = false;
            fittedOnce_ = true;
        }
    }

    // FOLLOW, before the projection is set up so the whole frame uses the new
    // centre. A followed target that only re-centred on the NEXT frame would
    // visibly lag its own marker.
    //
    // A DROPPED TARGET IS NOT FOLLOWED, but the follow is not cancelled
    // either: the map simply stops moving where the marker no longer is. The
    // id is kept because re-acquisition is automatic everywhere else, and a
    // follow silently cleared by a gap in reception is a setting the user
    // would have to notice and restore by hand.
    if (!followId_.empty()) {
        for (const auto& ht : tracks) {
            if (followId_ == ht.t.id) {
                if (!cascade::core::trackPresentation(ht.t.ageMs, ht.t.kind).visible) {
                    break;
                }
                centreLat_ = ht.t.latDeg;
                centreLon_ = ht.t.lonDeg;
                break;
            }
        }
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##mapcanvas", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    // A CLICK THAT IS NOT A DRAG, decided on the frame the button is released.
    // The map's primary gesture is panning, and a pan begins with exactly the
    // same press a click does: reacting on the press would select a target -
    // or drop the receiver somewhere - every time the user reached for the
    // map to move it. MouseDragMaxDistanceSqr is reset when the button goes
    // DOWN and still holds the gesture's full travel on the release frame,
    // which is what makes this the honest test rather than a guess at intent.
    // 16 px squared: four pixels of travel, which is a steady hand's wobble on
    // a press and nothing like a pan. ImGui's own drag threshold (6 px) is the
    // point at which it STARTS panning, so anything at or under it has moved
    // the map by nothing and was meant as a click.
    const bool clickNoDrag = ImGui::IsItemDeactivated() &&
                             ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] <=
                                 16.0f;
    // ONE CLICK DOES ONE THING. The receiver pick disarms itself the moment it
    // fires, so testing pickHomeArmed_ again further down would find it clear
    // and let the SAME press also change the selection - the click that placed
    // the antenna would have selected whatever was under it as well.
    bool clickConsumed = false;
    if (hovered && pickHomeArmed_) {
        // The cursor says the map is armed. Without it the only sign is a lit
        // lamp on a button three panels away.
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);
    // THE MAP IS A WELL, not a panel: it is the recessed glass everything
    // received gets drawn onto, so it takes the bench's darkest ground rather
    // than the near-black blue this used to be. The same constant is the
    // knockout inside a selected aircraft and the altitude legend's plate,
    // which is what keeps those two reading as holes cut in the map instead of
    // as three slightly different darks.
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), theme::kWell);

    // Equirectangular, with latitude span derived from the aspect ratio so a
    // degree of latitude and a degree of longitude stay the same size on
    // screen. (They are not the same distance on the ground away from the
    // equator; that is the projection's known and accepted distortion.)
    const double lonSpan = spanDeg_;
    const double latSpan = spanDeg_ * static_cast<double>(height) / static_cast<double>(width);
    // In Mercator the vertical scale is derived from the SAME pixels-per-degree
    // -of-longitude as the horizontal one, which is what keeps the projection
    // conformal: a tile drawn square stays square. `pixPerWorld` is how many
    // screen pixels one whole world-width spans, and everything else follows.
    const double pixPerWorld = static_cast<double>(width) * 360.0 / lonSpan;
    const double centreMercY = mercY(centreLat_);

    // THE VERTICAL HALF OF THE PROJECTION, on its own, because two things need
    // it and only one of them may wrap longitude: a POINT takes the short way
    // round the antimeridian, an EXTENT must not (see the tile note below, and
    // the land layer). Latitude never wraps in either, so it is written once.
    const auto projY = [&](double lat) {
        const double y = mercator
                             ? (mercY(lat) - centreMercY) * pixPerWorld + height * 0.5
                             : (centreLat_ - lat) / latSpan * height + height * 0.5;
        return origin.y + static_cast<float>(y);
    };
    const auto toScreen = [&](double lat, double lon) {
        double dx = lon - centreLon_;
        // Take the short way round: a target at +179 with the view at -179 is
        // two degrees away, not three hundred and fifty-eight.
        if (dx > 180.0) { dx -= 360.0; }
        if (dx < -180.0) { dx += 360.0; }
        const double x = dx / lonSpan * width + width * 0.5;
        return ImVec2(origin.x + static_cast<float>(x), projY(lat));
    };
    // TILE EDGES ARE PLACED FROM A CONTINUOUS LONGITUDE, NEVER THE WRAPPED
    // ONE, and this is not a tidy-up - it is the fix for a map that rendered
    // MIRRORED.
    //
    // toScreen's "short way round" correction is right for a POINT: a target
    // at +179 seen from -179 is two degrees away, not three hundred and
    // fifty-eight. It is wrong for a RECTANGLE, because the two edges are
    // wrapped independently. A tile running from +178 to +182 has its west
    // edge left at +178 and its east edge folded to -178, so east lands to the
    // LEFT of west, ImDrawList::AddImage is handed p_min.x > p_max.x, and it
    // dutifully draws the texture back to front - continents and all their
    // labels in mirror writing.
    //
    // It only bites once the view is wide enough for a visible tile to sit
    // more than 180 degrees from the centre, which is why it hid until
    // something zoomed the map out near the whole globe: eight satellites
    // scattered around the planet do exactly that (reported 2026-08-30).
    // The tile loop already works in an unwrapped, continuous longitude - the
    // one toWorld returns and txRaw preserves - so the only thing needed is to
    // stop re-wrapping it on the way back out.
    const auto tileX = [&](double lon) {
        return origin.x +
               static_cast<float>((lon - centreLon_) / lonSpan * width + width * 0.5);
    };
    const auto tileY = [&](double lat) {
        return origin.y +
               static_cast<float>((mercY(lat) - centreMercY) * pixPerWorld + height * 0.5);
    };

    const auto toWorld = [&](const ImVec2& p) {
        const double lon =
            (static_cast<double>(p.x - origin.x) - width * 0.5) / width * lonSpan + centreLon_;
        const double lat =
            mercator
                ? mercLat(centreMercY +
                          (static_cast<double>(p.y - origin.y) - height * 0.5) / pixPerWorld)
                : centreLat_ -
                      (static_cast<double>(p.y - origin.y) - height * 0.5) / height * latSpan;
        return std::pair<double, double>(lat, lon);
    };

    // --- interaction ------------------------------------------------------
    if (hovered) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            // Zoom about the CURSOR, not the centre: zooming about the middle
            // means the thing being examined slides away as you zoom in.
            const auto before = toWorld(ImGui::GetIO().MousePos);
            spanDeg_ = std::clamp(spanDeg_ * std::pow(0.85, static_cast<double>(wheel)),
                                  0.02, zoomOutLimitDeg);
            const double lonSpan2 = spanDeg_;
            const double latSpan2 = spanDeg_ * static_cast<double>(height) / width;
            const ImVec2 m = ImGui::GetIO().MousePos;
            const double lonAfter =
                (static_cast<double>(m.x - origin.x) - width * 0.5) / width * lonSpan2 +
                centreLon_;
            centreLon_ += before.second - lonAfter;
            // The vertical correction must use the projection actually drawing
            // the frame: with tiles up the y axis is Mercator, and the linear
            // formula made the point under the cursor jump on every notch —
            // by ~1.7x at this latitude.
            if (mercator) {
                const double pixPerWorld2 = static_cast<double>(width) * 360.0 / lonSpan2;
                const double atY =
                    centreMercY +
                    (static_cast<double>(m.y - origin.y) - height * 0.5) / pixPerWorld2;
                centreLat_ = mercLat(mercY(centreLat_) + (mercY(before.first) - atY));
            } else {
                const double latAfter =
                    centreLat_ -
                    (static_cast<double>(m.y - origin.y) - height * 0.5) / height * latSpan2;
                centreLat_ += before.first - latAfter;
            }
        }
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (!followId_.empty()) {
            // FOLLOWING WINS THE FRAME, THE USER GETS ASKED. Applying the drag
            // here used to move the centre for one frame and the follow above
            // snapped it straight back on the next — the map visibly fought
            // the hand and the user could not look around. The drag is
            // therefore not applied at all while a target is followed; the
            // attempt is latched, and the caller shows a "stop following?"
            // prompt so the choice is the user's rather than a tug of war.
            followInterrupt_ = true;
        } else {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            centreLon_ -= static_cast<double>(d.x) / width * lonSpan;
            // Same projection rule as the zoom: under tiles a degree of
            // latitude is not a constant number of pixels, and the linear
            // version left the map sliding slower than the cursor vertically.
            if (mercator) {
                centreLat_ =
                    mercLat(mercY(centreLat_) - static_cast<double>(d.y) / pixPerWorld);
            } else {
                centreLat_ += static_cast<double>(d.y) / height * latSpan;
            }
        }
    }
    // SET FROM MAP CLICK. Armed by the deck's own button and disarmed by the
    // click that answers it, so the map is never silently in a mode that
    // reassigns the antenna's position - one press, one pick.
    //
    // THE VIEW DOES NOT APPLY IT. A receiver position reaches every map page,
    // the radar scope and the coverage accumulator, none of which this class
    // knows about; it raises a request and the owner acts on it.
    if (pickHomeArmed_ && hovered && clickNoDrag) {
        const auto picked = toWorld(ImGui::GetIO().MousePos);
        // Refused rather than clamped, by the same range test the config
        // sanitizer and the typed entry use. A click past the pole is a click
        // on empty chart, not an instruction to move the antenna there.
        if (picked.first >= -90.0 && picked.first <= 90.0) {
            homeRequestLat_ = picked.first;
            homeRequestLon_ = detail::normaliseLonDeg(picked.second);
            homeRequest_ = true;
        }
        pickHomeArmed_ = false;
        clickConsumed = true;
    }
    centreLat_ = std::clamp(centreLat_, -89.0, 89.0);
    if (centreLon_ > 180.0) { centreLon_ -= 360.0; }
    if (centreLon_ < -180.0) { centreLon_ += 360.0; }

    // --- basemap tiles, beneath everything ---------------------------------
    //
    // The zoom is chosen so one tile's pixels land at roughly their native
    // size: below that the map is blurry, above it the client fetches four
    // times the tiles for detail nobody can see. Then every visible tile at
    // that zoom is asked for and drawn where its own edges say it goes, which
    // is the whole of the placement logic - in Mercator the tile grid IS the
    // projection, so there is nothing to reproject.
    bool haveTiles = false;
    if (mercator) {
        const double worldPixels = pixPerWorld;  // pixels for the whole world
        double want = std::log2(worldPixels / static_cast<double>(tiles->tileSize()));
        int z = static_cast<int>(std::floor(want + 0.5));
        z = std::clamp(z, static_cast<int>(tiles->minZoom()),
                       static_cast<int>(tiles->maxZoom()));
        const double n = std::pow(2.0, static_cast<double>(z));

        // The visible world rectangle in normalised Mercator, from the corners.
        const auto tl = toWorld(origin);
        const auto br = toWorld(ImVec2(origin.x + width, origin.y + height));
        const double x0 = (tl.second + 180.0) / 360.0;
        const double x1 = (br.second + 180.0) / 360.0;
        const double y0 = mercY(tl.first);
        const double y1 = mercY(br.first);

        long tx0 = static_cast<long>(std::floor(x0 * n));
        long tx1 = static_cast<long>(std::floor(x1 * n));
        long ty0 = static_cast<long>(std::floor(y0 * n));
        long ty1 = static_cast<long>(std::floor(y1 * n));
        if (tx1 < tx0) { std::swap(tx0, tx1); }
        if (ty1 < ty0) { std::swap(ty0, ty1); }
        ty0 = std::max(ty0, 0L);
        ty1 = std::min(ty1, static_cast<long>(n) - 1);

        // A hard cap on tiles per frame. The zoom choice already keeps this
        // near the screen area divided by the tile size, but a degenerate view
        // must not be able to ask for thousands.
        const long maxSpan = 64;
        if (tx1 - tx0 > maxSpan) { tx1 = tx0 + maxSpan; }
        if (ty1 - ty0 > maxSpan) { ty1 = ty0 + maxSpan; }

        for (long ty = ty0; ty <= ty1; ++ty) {
            for (long txRaw = tx0; txRaw <= tx1; ++txRaw) {
                // Wrap in x so panning across the antimeridian keeps working;
                // y does not wrap, because the world ends at the poles.
                long tx = txRaw % static_cast<long>(n);
                if (tx < 0) { tx += static_cast<long>(n); }
                const unsigned int tex = tiles->texture(static_cast<std::uint32_t>(z),
                                                        static_cast<std::uint32_t>(tx),
                                                        static_cast<std::uint32_t>(ty));
                if (tex == 0u) { continue; }
                // Placed from the tile's OWN edges rather than from a computed
                // size, so rounding cannot leave hairline gaps between
                // neighbours.
                const double west = static_cast<double>(txRaw) / n * 360.0 - 180.0;
                const double east = static_cast<double>(txRaw + 1) / n * 360.0 - 180.0;
                const double north = mercLat(static_cast<double>(ty) / n);
                const double south = mercLat(static_cast<double>(ty + 1) / n);
                const ImVec2 a(tileX(west), tileY(north));
                const ImVec2 b(tileX(east), tileY(south));
                // a is the top-left and b the bottom-right BY CONSTRUCTION now
                // (east > west, north > south), which is what AddImage's
                // implicit 0,0-1,1 texture coordinates require.
                dl->AddImage(static_cast<ImTextureID>(static_cast<std::uintptr_t>(tex)), a,
                             b);
                haveTiles = true;
            }
        }
    }

    // --- coastline, beneath everything else -------------------------------
    // Natural Earth 1:110m, public domain, compiled in as int16 hundredths of
    // a degree (see coastline_data.hpp). 20 KB of coordinates, so there is no
    // data file to lose, no download, and no basemap that can be missing on a
    // fresh install.
    //
    // SKIPPED once tiles are on screen: a rendered map already draws its own
    // coastlines, and a 1:110m vector outline over street-level imagery is not
    // a second opinion, it is a wrong-coloured line a few hundred metres off
    // the real one.
    //
    // AND IT IS FILLED, not merely traced. The runs are assembled into closed
    // rings once at start-up and triangulated there (see landShapes()); what
    // happens per frame is a projection and a draw. Land takes the design's
    // own land tone, which is the bench's dark brass; the coast keeps the
    // lighter brass it has always been, so the edge still reads against the
    // fill. An inland sea - the Caspian is the only one at this scale - is
    // painted back in the map's own ground after the land, because filling
    // every ring in the coastline layer would draw it as a plateau.
    if (!haveTiles) {
        // Land is drawn in brass, because that is what it is: the plate the
        // readings sit on. The blue-grey it used to be was a chart convention
        // borrowed from nothing else in the product.
        const ImU32 landCol = theme::kBrassMid;
        const ImU32 landFill = theme::kBrassDark;
        // The same constant the map's own ground is filled with at the top of
        // this function, so an inland sea is the map showing through rather
        // than a second, nearly-matching dark.
        const ImU32 seaFill = theme::kWell;
        const double west = centreLon_ - lonSpan * 0.5;
        const double east = centreLon_ + lonSpan * 0.5;
        const double south = centreLat_ - latSpan * 0.5;
        const double north = centreLat_ + latSpan * 0.5;
        const ImVec2 whiteUv = ImGui::GetFontTexUvWhitePixel();
        // Reused across frames and across the three passes. Single-threaded by
        // construction: ImGui draws on one thread and this is only ever
        // reached from inside a frame.
        static std::vector<ImVec2> screen;

        // WHICH COPIES OF THE WORLD THE VIEW TOUCHES. The shapes are held in a
        // continuous longitude that runs outside +/-180 wherever a ring
        // crosses the antimeridian, so a ring is offered to the view once per
        // whole-world offset that could put it on screen - and each copy is
        // shifted as ONE extent rather than per vertex, which is the rule the
        // mirrored-basemap fault of 0.70.0 produced.
        const auto forEachCopy = [&](const LandShape& s, auto&& body) {
            const double loK = std::floor((west - static_cast<double>(s.maxLon)) / 360.0);
            const double hiK = std::ceil((east - static_cast<double>(s.minLon)) / 360.0);
            // Bounded so a degenerate viewport cannot ask for thousands of
            // copies, and bounded WIDE ENOUGH that the bound is never the
            // thing that decides what is drawn: +/-16 covers every span this
            // view will accept, the whole-world one included.
            const int lo = static_cast<int>(std::clamp(loK, -16.0, 16.0));
            const int hi = static_cast<int>(std::clamp(hiK, -16.0, 16.0));
            for (int k = lo; k <= hi; ++k) {
                const double off = 360.0 * static_cast<double>(k);
                if (static_cast<double>(s.maxLon) + off < west ||
                    static_cast<double>(s.minLon) + off > east) {
                    continue;
                }
                body(off);
            }
        };
        const auto project = [&](const LandShape& s, double off) {
            screen.clear();
            screen.reserve(s.pts.size());
            for (const ImVec2& q : s.pts) {
                screen.emplace_back(
                    origin.x + static_cast<float>((static_cast<double>(q.x) + off - centreLon_) /
                                                      lonSpan * width +
                                                  width * 0.5),
                    projY(static_cast<double>(q.y)));
            }
        };
        // Writes a ring's pre-computed triangles straight into the draw list.
        // The vertices are shared between the triangles, so the whole of
        // Eurasia costs 1,318 of them rather than three per triangle, and no
        // anti-aliased fringe is generated - the coastline stroke drawn over
        // it afterwards is the antialiased edge.
        const auto fillShape = [&](const LandShape& s, ImU32 col) {
            const int vtx = static_cast<int>(screen.size());
            const int idx = static_cast<int>(s.tri.size());
            if (vtx < 3 || idx < 3) { return; }
            dl->PrimReserve(idx, vtx);
            const unsigned int base = dl->_VtxCurrentIdx;
            for (const ImVec2& p : screen) { dl->PrimWriteVtx(p, whiteUv, col); }
            for (std::uint16_t i : s.tri) {
                dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + i));
            }
        };

        const auto& shapes = landShapes();
        for (const LandShape& s : shapes) {
            if (!s.closed || s.water || s.tri.empty()) { continue; }
            if (static_cast<double>(s.maxLat) < south || static_cast<double>(s.minLat) > north) {
                continue;
            }
            forEachCopy(s, [&](double off) {
                project(s, off);
                fillShape(s, landFill);
            });
        }
        for (const LandShape& s : shapes) {
            if (!s.closed || !s.water || s.tri.empty()) { continue; }
            if (static_cast<double>(s.maxLat) < south || static_cast<double>(s.minLat) > north) {
                continue;
            }
            forEachCopy(s, [&](double off) {
                project(s, off);
                fillShape(s, seaFill);
            });
        }
        // The coast itself, over the fill. Drawn from the SAME assembled rings
        // rather than from the raw runs, so the outline and the area it
        // encloses cannot disagree about where a coast is - and broken into
        // the stretches that are actually on screen, because a 1,318-point
        // polyline of which four points are visible is thousands of vertices
        // to say nothing.
        for (const LandShape& s : shapes) {
            if (s.pts.size() < 2u) { continue; }
            if (static_cast<double>(s.maxLat) < south - latSpan ||
                static_cast<double>(s.minLat) > north + latSpan) {
                continue;
            }
            forEachCopy(s, [&](double off) {
                project(s, off);
                const std::size_t n = s.pts.size();
                const std::size_t segs = s.closed ? n : (n - 1u);
                std::size_t runStart = 0;
                std::size_t runLen = 0;
                const auto flush = [&]() {
                    if (runLen >= 2u) {
                        dl->AddPolyline(&screen[runStart], static_cast<int>(runLen), landCol,
                                        0, 1.0f);
                    }
                    runLen = 0;
                };
                for (std::size_t i = 0; i < segs; ++i) {
                    const std::size_t j = (i + 1u) % n;
                    const double lonA = static_cast<double>(s.pts[i].x) + off;
                    const double lonB = static_cast<double>(s.pts[j].x) + off;
                    const double latA = static_cast<double>(s.pts[i].y);
                    const double latB = static_cast<double>(s.pts[j].y);
                    const bool visible = !(std::max(lonA, lonB) < west ||
                                           std::min(lonA, lonB) > east ||
                                           std::max(latA, latB) < south - latSpan ||
                                           std::min(latA, latB) > north + latSpan);
                    // A closed ring's last segment joins the end back to the
                    // start, which is not a contiguous stretch of the array;
                    // it is drawn on its own.
                    if (visible && j != 0u) {
                        if (runLen == 0u) {
                            runStart = i;
                            runLen = 2u;
                        } else {
                            ++runLen;
                        }
                    } else {
                        flush();
                        if (visible) {
                            const ImVec2 closing[2] = {screen[i], screen[j]};
                            dl->AddPolyline(closing, 2, landCol, 0, 1.0f);
                        }
                    }
                }
                flush();
            });
        }
    }

    // --- graticule --------------------------------------------------------
    const double step = graticuleStep(spanDeg_);
    // A graticule is engraved into the plate and its numbers are a caption on
    // it - never a reading, which is why neither of them is amber.
    const ImU32 gridCol = theme::kEngraved;
    // THE GRATICULE STOPS AT THE POLES, because that is where the world stops.
    // A whole-world view in a viewport taller than two to one has to leave
    // slack above and below - 360 degrees of longitude across a portrait pane
    // is more than 180 of latitude down it, and the alternative is either
    // cropping the world or scaling the two axes differently, which is not a
    // projection any more. What must NOT happen is the graticule running on
    // into that slack: the satellites window opened with parallels engraved at
    // 135 and 180 degrees of latitude, which are not places. The lines are
    // bounded here and the meridians are bounded to the same range, so the
    // engraved grid ends exactly where the planet does.
    const double gratSouth = std::max(-90.0, centreLat_ - latSpan * 0.5);
    const double gratNorth = std::min(90.0, centreLat_ + latSpan * 0.5);
    const double lat0 = std::floor(gratSouth / step) * step;
    for (double lat = lat0; lat <= gratNorth; lat += step) {
        if (lat < gratSouth) { continue; }
        const ImVec2 a = toScreen(lat, centreLon_ - lonSpan * 0.5);
        const ImVec2 b = toScreen(lat, centreLon_ + lonSpan * 0.5);
        dl->AddLine(a, b, gridCol);
        char buf[24];
        std::snprintf(buf, sizeof buf, "%.4g", lat);
        // NOT OVER THE SCALE BAR. The bar and its distance sit in the bottom
        // left corner, and an equirectangular view reaching -90 puts a
        // latitude label in exactly that spot - two numbers on top of each
        // other, neither readable. The line is still drawn; only its label
        // stands down, because the bar is the one that says what the map's
        // scale is.
        if (a.y < origin.y + height - 40.0f) {
            addMapLabel(dl, ImVec2(origin.x + 3.0f, a.y - 14.0f), theme::kCream, buf);
        }
    }
    const double lon0 = std::floor((centreLon_ - lonSpan * 0.5) / step) * step;
    for (double lon = lon0; lon <= centreLon_ + lonSpan * 0.5; lon += step) {
        const ImVec2 a = toScreen(gratNorth, lon);
        const ImVec2 b = toScreen(gratSouth, lon);
        dl->AddLine(a, b, gridCol);
        char buf[24];
        std::snprintf(buf, sizeof buf, "%.4g", lon);
        addMapLabel(dl, ImVec2(a.x + 3.0f, origin.y + height - 16.0f), theme::kCream, buf);
    }

    // --- range rings around the receiver ---------------------------------
    if (hasHome_) {
        const ImVec2 h = toScreen(homeLat_, homeLon_);
        // Ring spacing picked so a handful are visible at the current zoom.
        const double viewKm = latSpan * 111.32;
        static const double ringsKm[] = {10, 25, 50, 100, 200, 400, 800, 1600};
        for (double r : ringsKm) {
            if (r > viewKm) { break; }
            const double dLat = r / 111.32;
            const ImVec2 edge = toScreen(homeLat_ + dLat, homeLon_);
            const float px = std::fabs(edge.y - h.y);
            if (px < 8.0f) { continue; }
            // A RING IS A BRASS RAIL LAID ON THE PLATE, and its distance is a
            // caption on that rail. Neither is a reading the receiver is
            // making, so neither is amber; the rings sit a shade above the
            // engraved graticule so the two layers stay tellable apart.
            dl->AddCircle(h, px, theme::withAlpha(theme::kBrassDark, 200.0f / 255.0f), 64);
            char buf[24];
            std::snprintf(buf, sizeof buf, "%.0f km", r);
            addMapLabel(dl, ImVec2(h.x + 3.0f, h.y - px - 14.0f), theme::kCream, buf);
        }
        // THE RECEIVER'S OWN MARK IS LETTERING, not a reading: this is the one
        // position on the map a hand put there, so it takes the ivory the rest
        // of the product uses for things the user operates.
        dl->AddCircleFilled(h, 4.0f, theme::kIvory);
        addMapLabel(dl, ImVec2(h.x + 6.0f, h.y + 2.0f), theme::kCream, "RX");
    }

    // --- coverage overlay, over the rings and under the targets ------------
    //
    // The furthest anything has been PLOTTED in each five-degree sector, drawn
    // as one closed polygon around the receiver. It is the cheapest antenna
    // diagnostic there is: the nulls in a real pattern are visible as notches,
    // and a mast or a building in one direction shows up as a flat side.
    //
    // PLOTTED, NOT HEARD, and the difference is the reason the deck's note was
    // rewritten. The accumulator is fed every visible track from every plugin
    // (see AppWindow::drawPluginWindows), and a track's position may have been
    // computed by a propagator rather than received. On a decoding page those
    // are the same set; on a satellite page they are not, and the shape then
    // reports where the tracker put things rather than what the antenna
    // managed to hear.
    //
    // NO RECEIVER POSITION MEANS NO OVERLAY, silently. The whole shape is a set
    // of distances FROM somewhere, so without that somewhere there is nothing
    // to draw and nothing sensible to draw it around - the map window's own
    // controls say why, which is the right place for an explanation.
    //
    // A SECTOR WITH NOTHING IN IT COLLAPSES TO THE CENTRE rather than being
    // interpolated across from its neighbours. A notch in this picture is the
    // interesting part; smoothing it away would turn the one thing worth seeing
    // into a rounder blob.
    if (hasHome_ && coverage_ != nullptr && !coverage_->empty()) {
        const ImVec2 h = toScreen(homeLat_, homeLon_);
        // Each vertex sits at the MIDDLE of its sector's arc, so a lone
        // sighting draws a spike pointing the way it was actually heard rather
        // than a wedge whose edge is the bearing.
        //
        // THE VERTEX IS A GREAT-CIRCLE DESTINATION, not a flat offset. What
        // stood here was a plate-carree step - latitude plus km/111.32*cos(brg),
        // longitude plus km/(111.32*cos(lat))*sin(brg) - under a comment
        // claiming it was within a pixel of the real thing. It is not: from
        // 51.5 N that shortcut is 1.1 km out at 100 km, 9.9 km at 300, 27.4 km
        // at 500 and 110 km at 1000, and 300-500 km is ordinary reach for the
        // ADS-B and APRS sources this ships with, so the lobe was skewed by
        // tens of kilometres exactly where it gets looked at. destinationPoint
        // in track_metrics.hpp does it properly and is tested against an
        // external reference; see coverageVertex.
        //
        // AND THE LONGITUDE IS AN OFFSET, not a place. toScreen normalises each
        // longitude into +/-180 of the view centre, which is right for one
        // target and wrong for a CONNECTED shape: a receiver at 179.5 E has
        // vertices either side of the antimeridian, each normalising to the
        // opposite edge of the map, and both the polyline and the fill then
        // ran straight across the world. Placing every vertex as a CONTINUOUS
        // longitude offset from the receiver's own pixel removes the seam
        // entirely rather than special-casing it - x is a fixed scale of
        // longitude in both projections this view offers, so the offset in
        // pixels is exact - and it costs an ordinary receiver nothing, because
        // away from the antimeridian the offset IS the difference.
        //
        // The latitude still goes through toScreen, which is what keeps the
        // radius right under Mercator: a Mercator pixel is not a linear
        // function of kilometres.
        const double pxPerLonDeg = static_cast<double>(width) / lonSpan;
        ImVec2 pts[CoverageMap::kBuckets];
        for (int i = 0; i < CoverageMap::kBuckets; ++i) {
            const double km = coverage_->maxKm(i);
            if (km <= 0.0) {
                pts[i] = h;
                continue;
            }
            const double brg = (static_cast<double>(i) + 0.5) * CoverageMap::kBucketDeg;
            const CoverageVertex v = coverageVertex(homeLat_, homeLon_, brg, km);
            // Clamped off the poles because mercY is infinite there; the
            // longitude needs no such guard now that it is an offset.
            const ImVec2 p = toScreen(std::clamp(v.latDeg, -89.9, 89.9), homeLon_);
            pts[i] = ImVec2(h.x + static_cast<float>(v.dLonDeg * pxPerLonDeg), p.y);
        }
        // PHOSPHOR, the display tone, because on every page that decodes its
        // own targets this shape IS what the radio heard - the same rule that
        // colours a trace on the spectrum. Where a page's positions are
        // propagated instead, the shape is still built from the display's own
        // plotted output rather than from a control or a setting, and the deck
        // beside it now states which of the two it is. Faint fill,
        // brighter edge: the fill says where the coverage is and must not hide
        // the map or the targets inside it, and the edge is the line the eye
        // actually reads the shape from, so it gets the live phosphor and the
        // wash gets the dim one.
        // CONCAVE, not convex. A real coverage lobe has notches in it - that is
        // the whole reason to look at one - and AddConvexPolyFilled on a
        // concave outline fills the triangle fan rather than the shape,
        // painting over exactly the nulls the picture exists to show.
        dl->AddConcavePolyFilled(pts, CoverageMap::kBuckets,
                                 theme::withAlpha(theme::kPhosphorDim, 34.0f / 255.0f));
        dl->AddPolyline(pts, CoverageMap::kBuckets,
                        theme::withAlpha(theme::kPhosphor, 190.0f / 255.0f),
                        ImDrawFlags_Closed, 1.5f);
    }

    // Reset per frame: the path loop below sets these, the target loop reads
    // them.
    anyBandedTrail_ = false;
    anyOrbitTrail_ = false;

    // --- paths, under the targets ----------------------------------------
    //
    // THE WHOLE LAYER IS OPTIONAL, and that is one of the two switches the
    // trail request asked for: a user watching a busy approach with fifty
    // aircraft on screen may want the markers and not the fifty lines behind
    // them. It hides every path, not only the trails behind targets - a
    // footprint circle and a predicted ground track are drawn by this loop
    // too, and a checkbox that hid two of the three kinds of line on the map
    // would be a checkbox nobody could predict.
    if (drawTrails_) {
        for (const auto& p : paths) {
            if (p.points.size() < 2) { continue; }
            // THE TRAIL OBEYS THE SAME RULE AS ITS OWNER. A path carries no age
            // of its own, so the host looks the owner up; without this the
            // marker disappeared on schedule and the line under it did not,
            // leaving a trail that starts where the missing marker would have
            // been and runs off into empty space - a drawing that says
            // "something is here" about the one target the host has just
            // decided is not.
            const cascade::core::TrackPresentation pres =
                cascade::core::pathPresentation(p, tracks);
            if (!pres.visible) { continue; }
            // THE TRAIL TAKES ITS OWNER'S COLOUR, altitude included, so a
            // climbing aircraft's trail is the same colour as its marker
            // instead of the generic per-kind line that used to sit under a
            // banded marker.
            //
            // An UNOWNED path - a footprint circle, a predicted track, anything
            // not reporting as a target - has no altitude to take, and falls
            // back to the path's own kind colour exactly as before.
            ImU32 base = colourFor(p.kind);
            bool owned = false;
            bool emergency = false;
            // THE OWNER'S KIND, NOT THE PATH'S, decides which altitude ladder
            // the trail is banded on. A path carries a kind of its own, and a
            // plugin is free to leave it at zero; the owner is the thing whose
            // altitude is being read, so it is the thing whose unit applies.
            std::uint32_t ownerKind = p.kind;
            for (const auto& ht : tracks) {
                if (ht.plugin == p.plugin && p.id == ht.t.id) {
                    base = colourForTrack(ht.t);
                    owned = true;
                    ownerKind = ht.t.kind;
                    emergency = (ht.t.flags & CASCADE_TRACK_FLAG_EMERGENCY) != 0u;
                    break;
                }
            }
            const ImU32 col =
                fadedColour((base & 0x00FFFFFFu) | (120u << IM_COL32_A_SHIFT), pres.alpha);

            // COLOURED ALONG ITS LENGTH, which the comment that stood here
            // said was impossible - and it was, from the path alone. A
            // CascadePathPoint carries a latitude and a longitude and nothing
            // else (see the ABI, which does not change for this), so the
            // altitude at a vertex is not in what the plugin hands over. What
            // changed is that the host no longer has to INVENT it: it watches
            // every track's altitude beside its position on every poll, so it
            // can be ASKED what this aircraft's altitude was when it was here.
            // That is recorded observation, and a vertex the host never
            // observed answers "no altitude here" rather than inheriting a
            // neighbour's - see PluginUi::altitudeNear.
            //
            // AN EMERGENCY IS NOT OVERRULED BY IT. colourForTrack puts a 7500/
            // 7600/7700 squawk above the altitude palette precisely because it
            // must never be mistaken for anything else, and banding the trail
            // of an aircraft in trouble would take that back for the longest
            // mark it draws on the map.
            const bool banded =
                trailAltitudeColours_ && owned && !emergency && altitudeAt_;
            // A BANDED TRAIL IS BANDED COLOUR ON SCREEN, so it has to count
            // towards the legend exactly as a banded marker does. The legend
            // was gated on markers alone, which left the one case where the
            // colours most need explaining - a trail still drawn under a
            // marker that has just faded out, or scrolled off - painting six
            // hues with nothing on screen to say what they mean.
            // ...AND IT COUNTS TOWARDS ITS OWN LADDER'S LEGEND. A trail banded
            // in kilometres under an aviation legend would be a key that
            // actively misinforms, which is worse than no key at all.
            if (banded) {
                if (orbitalLadder(ownerKind)) {
                    anyOrbitTrail_ = true;
                } else {
                    anyBandedTrail_ = true;
                }
            }
            // The band colours, once per path rather than once per segment:
            // they depend only on this trail's fade, and a segment loop that
            // rebuilt them would do the same arithmetic a hundred times over.
            // Sized for the LONGER of the two ladders, so one array serves
            // both and neither can be written past.
            constexpr int kMaxLadderBands =
                kAltBandCount > kOrbitBandCount ? kAltBandCount : kOrbitBandCount;
            ImU32 bandCol[kMaxLadderBands] = {};
            const int ownerBands = bandCountFor(ownerKind);
            const std::size_t n = p.points.size();
            if (banded) {
                for (int b = 0; b < ownerBands; ++b) {
                    const AltBandStyle& bs = bandStyleFor(ownerKind, b);
                    bandCol[b] = fadedColour(IM_COL32(bs.r, bs.g, bs.b, 120), pres.alpha);
                }
                // PER VERTEX, not per segment endpoint: adjacent segments share
                // a vertex, so asking once per vertex halves the lookups. NaN
                // is "not observed", which is the same thing altM means when a
                // source does not report it.
                trailAltScratch_.assign(n, std::numeric_limits<double>::quiet_NaN());
                for (std::size_t i = 0; i < n; ++i) {
                    double a = 0.0;
                    if (altitudeAt_(p.plugin, p.id, p.points[i].latDeg, p.points[i].lonDeg,
                                    a)) {
                        trailAltScratch_[i] = a;
                    }
                }
            }

            // THE PATH'S FLAGS, honoured at last: both were declared in the ABI
            // from the start and the first plugin to set them (the satellite
            // tracker's predicted ground track) found the host ignoring them.
            // DASHED draws alternating segments - a predicted line should not
            // read with the same certainty as a recorded trail, which is the
            // entire reason the flag exists. CLOSED joins the last vertex back
            // to the first (a footprint circle), through the same antimeridian
            // rule as every other segment.
            const bool dashed = (p.flags & CASCADE_PATH_FLAG_DASHED) != 0u;
            const bool closed = (p.flags & CASCADE_PATH_FLAG_CLOSED) != 0u;
            const std::size_t segs = closed ? n : (n - 1);
            for (std::size_t s = 1; s <= segs; ++s) {
                if (dashed && (s % 2u == 0u)) { continue; }
                const std::size_t ia = s - 1;
                const std::size_t ib = (s == n) ? 0 : s;
                const double lonA = p.points[ia].lonDeg;
                const double lonB = p.points[ib].lonDeg;
                // Do not draw the segment that wraps the antimeridian: joining
                // +179 to -179 would streak a line straight across the map,
                // which is what a naive ground-track plot always gets wrong.
                if (std::fabs(lonB - lonA) > 180.0) { continue; }
                // THE SEGMENT'S OWN COLOUR, and the fade goes on every one of
                // them: a stale trail still has to fade as ONE thing, or the
                // altitude bands would read as six lines of different ages.
                //
                // BOTH ENDS MUST BE OBSERVED. The band comes from the mean of
                // the two altitudes, which is the altitude at the middle of the
                // segment under exactly the assumption the straight line
                // already makes - that the aircraft went directly from one
                // observation to the other. With one end unobserved there is no
                // such pair, and the segment takes the owner's single colour
                // rather than half an answer.
                ImU32 segCol = col;
                if (banded) {
                    const double altA = trailAltScratch_[ia];
                    const double altB = trailAltScratch_[ib];
                    if (std::isfinite(altA) && std::isfinite(altB)) {
                        const int band = bandIndexFor(ownerKind, 0.5 * (altA + altB));
                        if (band >= 0 && band < ownerBands) { segCol = bandCol[band]; }
                    }
                }
                const ImVec2 a = toScreen(p.points[ia].latDeg, lonA);
                const ImVec2 b = toScreen(p.points[ib].latDeg, lonB);
                if (trailStyle_ != 1) {
                    dl->AddLine(a, b, segCol, 1.5f);
                    continue;
                }
                // THE RIBBON. The same segment and the same colour, given
                // width and a translucent fill so a trail reads at a glance on
                // a busy basemap - which is what it is FOR, and a 1.5 px line
                // over street detail is exactly what it is not.
                //
                // WIDTH IN PIXELS, NEVER IN DEGREES. A ribbon sized in degrees
                // would swell into a blob at world zoom - the view this map
                // reaches by itself when a plugin scatters targets around the
                // planet - and shrink to nothing when zoomed in. Constant
                // screen width is legible at every scale.
                const float dx = b.x - a.x;
                const float dy = b.y - a.y;
                const float len = std::sqrt(dx * dx + dy * dy);
                // A degenerate segment has no direction to be perpendicular
                // to; normalising it would divide by zero and scatter NaN
                // vertices through the draw list.
                if (!(len > 0.001f)) { continue; }
                const float nx = -dy / len * kTrailRibbonHalfPx;
                const float ny = dx / len * kTrailRibbonHalfPx;
                const ImVec2 quad[4] = {ImVec2(a.x + nx, a.y + ny),
                                        ImVec2(b.x + nx, b.y + ny),
                                        ImVec2(b.x - nx, b.y - ny),
                                        ImVec2(a.x - nx, a.y - ny)};
                dl->AddConvexPolyFilled(quad, 4, segCol);
                // A ROUND JOIN AT THE SHARED VERTEX. Two quads meeting at an
                // angle leave a wedge of gap on the outside of every turn, and
                // a trail is mostly turns; a disc of the same radius fills it
                // without needing to solve the mitre.
                dl->AddCircleFilled(b, kTrailRibbonHalfPx, segCol, 8);
            }
        }
    }

    // --- targets ----------------------------------------------------------
    hoveredId_.clear();
    // Whether anything actually drawn has a reported altitude, which is what
    // decides whether the altitude legend below is information or clutter -
    // and it is asked once PER LADDER, because a map carrying both aircraft
    // and satellites needs both keys and a map carrying one needs one.
    bool anyBanded = anyBandedTrail_;
    bool anyOrbitBanded = anyOrbitTrail_;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    float bestDist = 14.0f;  // hit radius in pixels
    const cascade::core::HostTrack* best = nullptr;

    for (const auto& ht : tracks) {
        // THE HOST'S SINGLE STALENESS RULE, which the ABI says is the host's
        // job (see CascadeTrack::ageMs). What was here before was a bare
        // "older than a minute, draw it faint" applied to every kind alike -
        // so a ship reporting on its normal three-minute Class B schedule was
        // permanently dimmed, and an aircraft heard once an hour ago was drawn
        // at full confidence forever because nothing ever removed it.
        const cascade::core::TrackPresentation pres =
            cascade::core::trackPresentation(ht.t.ageMs, ht.t.kind);
        if (!pres.visible) { continue; }
        const ImVec2 s = toScreen(ht.t.latDeg, ht.t.lonDeg);
        if (s.x < origin.x - 40 || s.x > origin.x + width + 40 || s.y < origin.y - 40 ||
            s.y > origin.y + height + 40) {
            continue;
        }
        const ImU32 col = fadedColour(colourForTrack(ht.t), pres.alpha);
        // Selected or followed: either way this is the one the user singled
        // out, and it gets the ringed marker.
        const bool picked = (!selectedId_.empty() && selectedId_ == ht.t.id) ||
                            (!followId_.empty() && followId_ == ht.t.id);
        const bool altKnown = bandIndexFor(ht.t.kind, ht.t.altM) >= 0;
        if (altKnown) {
            if (orbitalLadder(ht.t.kind)) {
                anyOrbitBanded = true;
            } else {
                anyBanded = true;
            }
        }
        // An emergency also gets a ring of its own, so it survives being
        // selected (where the marker is knocked out of a disc and the emergency
        // hue is no longer the fill) and survives a colour-blind reading.
        if ((ht.t.flags & CASCADE_TRACK_FLAG_EMERGENCY) != 0u) {
            dl->AddCircle(s, 16.0f, fadedColour(IM_COL32(255, 45, 45, 255), pres.alpha), 0,
                          2.0f);
        }

        if (ht.t.kind == CASCADE_TRACK_AIRCRAFT) {
            // The silhouette IS the heading indicator, so no tick.
            // 9.45/8.4/13.65: the 9/8/13 set grown 5% together, requested
            // after field use - the silhouettes read slightly small against
            // the new rims, and growing all three keeps the selected
            // knockout's margins exactly as designed.
            if (picked) {
                dl->AddCircleFilled(s, 13.65f, col);
                dl->AddCircle(s, 13.65f, IM_COL32(0, 0, 0, (col >> IM_COL32_A_SHIFT) & 0xFFu),
                              0, 1.5f);
                // The silhouette is KNOCKED OUT of the disc, so it is drawn in
                // the map's own ground rather than in a dark of its own.
                addPlane(dl, s, ht.t.courseDeg, 8.4f, theme::kWell);
            } else {
                addPlane(dl, s, ht.t.courseDeg, 9.45f, col, altKnown);
            }
        } else {
            // A course, where known, is drawn as a heading tick. It is the
            // difference between a field of dots and a picture of where things
            // are going.
            if (!std::isnan(ht.t.courseDeg)) {
                const double a = ht.t.courseDeg * kPi / 180.0;
                const ImVec2 tip(s.x + static_cast<float>(std::sin(a) * 12.0),
                                 s.y - static_cast<float>(std::cos(a) * 12.0));
                dl->AddLine(s, tip, col, 1.5f);
            }
            dl->AddCircleFilled(s, 3.5f, col);
            if (picked) { dl->AddCircle(s, 9.0f, col, 0, 2.0f); }
        }

        const char* lbl = ht.t.label[0] != '\0' ? ht.t.label : ht.t.id;
        const float lblX = s.x + (picked ? 15.0f : 8.0f);
        addMapLabel(dl, ImVec2(lblX, s.y - 6.0f), col, lbl);

        if (hovered) {
            const float dx = mouse.x - s.x;
            const float dy = mouse.y - s.y;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d < bestDist) {
                bestDist = d;
                best = &ht;
            }
        }
    }

    // CLICKING A MARKER SELECTS IT, which is what makes the map and the
    // register beside it one instrument rather than two pictures of the same
    // targets. Selection is a HIGHLIGHT and nothing more - it does not move
    // the view - so a click that lands on a target the user was only reaching
    // past costs them a ring, not their place on the chart.
    //
    // A click on empty chart clears the selection, for the same reason a list
    // deselects when you click below the last row: "nothing is selected" has
    // to be reachable, and the detail card has to be able to say so.
    if (hovered && clickNoDrag && !clickConsumed) {
        selectedId_ = (best != nullptr) ? std::string(best->t.id) : std::string();
    }

    if (best != nullptr) {
        hoveredId_ = best->t.id;
        // THE SHARED BLOCK. This tooltip used to spell the whole thing out
        // here, and the flight list spelled a second, shorter copy out for
        // itself; the copies drifted within the week. Both now draw what
        // buildTrackDetailLines says, so a change to the units or to the
        // registry fields reaches every place a target is described.
        //
        // Asking the info cache is non-blocking and cached, so hovering is
        // also what starts the lookup for a target the per-frame sweep has not
        // reached yet - which happens inside makeTrackDetailInput.
        ImGui::BeginTooltip();
        drawTrackDetail(*best, info, hasHome_, homeLat_, homeLon_);
        ImGui::EndTooltip();
    }

    // --- altitude legend ---------------------------------------------------
    // WORTH ITS SPACE, and only just: six swatches and six labels down the
    // right edge, about twenty lines of drawing and no layout of its own.
    // Without it the colours are a code the user has to guess at, and guessing
    // "green is higher than orange" is exactly the sort of thing that is right
    // until the day it matters. It costs nothing when it would be noise,
    // because it is DRAWN ONLY WHEN SOMETHING ON SCREEN HAS AN ALTITUDE: a map
    // of ships and APRS stations, which report none, never shows it.
    //
    // AND THE LEGEND NAMES ITS UNIT, in a heading over the swatches. The band
    // labels carry "kft" and "km" of their own, but a reader scanning a column
    // of colours needs to know what is being measured before the numbers mean
    // anything - and with two ladders now possible on one map, a heading is
    // the only thing that says which of them a given block of colour belongs
    // to.
    {
        const float sw = kAltLegendSwatch;
        const float pad = kAltLegendPad;
        const float lh = ImGui::GetTextLineHeight();
        const float rowStep = lh + 2.0f;
        float legendY = origin.y + 8.0f;

        // One ladder's key. Returns the height it used so a second can be
        // stacked under it without either guessing at the other's size.
        const auto drawLadder = [&](const char* heading, int bandCount,
                                    const AltBandStyle& (*styleAt)(int), float widthPx) {
            const float headH = lh + 3.0f;
            const float boxH =
                headH + rowStep * static_cast<float>(bandCount) + 8.0f;
            // The heading may be wider than any band label; the plate has to
            // hold whichever is longer or it clips its own title.
            const float headW = ImGui::CalcTextSize(heading).x + pad * 2.0f;
            const float boxW = (widthPx > headW) ? widthPx : headW;
            const ImVec2 tl(origin.x + width - boxW - 8.0f, legendY);
            const ImVec2 br(tl.x + boxW, tl.y + boxH);
            // The legend is a plate laid on the map: the map's own ground as a
            // scrim so it darkens busy tiles without hiding them, edged in the
            // same brass as the range rings.
            dl->AddRectFilled(tl, br, theme::withAlpha(theme::kWell, 190.0f / 255.0f),
                              3.0f);
            dl->AddRect(tl, br, theme::withAlpha(theme::kBrassDark, 200.0f / 255.0f),
                        3.0f);
            // A caption on glass, so cream rather than the engraved tone - the
            // same rule every other over-map word here follows.
            dl->AddText(ImVec2(tl.x + pad, tl.y + 4.0f), theme::kCream, heading);
            for (int i = 0; i < bandCount; ++i) {
                // Highest band at the TOP, which is the way an altitude scale
                // is read everywhere else.
                const AltBandStyle& s = styleAt(bandCount - 1 - i);
                const float y = tl.y + 4.0f + headH + rowStep * static_cast<float>(i);
                // Laid out from the same padding the width was computed from,
                // so the two cannot disagree about where the label starts.
                dl->AddRectFilled(ImVec2(tl.x + pad, y + (lh - sw) * 0.5f),
                                  ImVec2(tl.x + pad + sw, y + (lh + sw) * 0.5f),
                                  IM_COL32(s.r, s.g, s.b, 255), 2.0f);
                // The band SWATCH keeps its measured colour (see altBandStyle);
                // the label beside it is a caption on glass, so it is cream.
                dl->AddText(ImVec2(tl.x + pad + sw + pad, y), theme::kCream, s.label);
            }
            legendY = br.y + 6.0f;
        };

        // MEASURED WITH THE FONT IN USE, not assumed. A constant here was 74 px
        // and clipped half the labels against the map's clip rect; the width
        // has to come from the same CalcTextSize that will draw them - and the
        // two ladders measure separately, because "850-1200 km" is longer than
        // any aviation label.
        if (anyOrbitBanded) {
            drawLadder("ORBITAL ALTITUDE - km", kOrbitBandCount, &orbitBandStyle,
                       orbitLegendWidth(
                           [](const char* s) { return ImGui::CalcTextSize(s).x; }));
        }
        if (anyBanded) {
            drawLadder("ALTITUDE - kft", kAltBandCount, &altBandStyle,
                       altLegendWidth(
                           [](const char* s) { return ImGui::CalcTextSize(s).x; }));
        }
    }

    // Scale bar: a map with no basemap and no scale is a scatter plot.
    //
    // MEASURED ALONG THE AXIS IT IS DRAWN ON, AND TRUE AT A STATED LATITUDE.
    // It used to be neither, and that made it the most serious thing on this
    // picture, because it is the one part of it that is a MEASUREMENT. The
    // figure came from latSpan * 111.32 / height - kilometres per pixel
    // VERTICALLY - and was then drawn as a HORIZONTAL bar. On a Mercator page
    // near the equator the two are close enough that nobody noticed; on a
    // whole-globe equirectangular page they diverge as 1/cos(lat), so the bar
    // was simply a false statement about distance over most of the map.
    //
    // Horizontally, one degree of longitude is 111.32*cos(lat) km, in BOTH
    // projections drawn here - x is linear in longitude in each - so the
    // horizontal scale depends on WHERE on the map it is read, and a scale bar
    // that does not say where is not a measurement. This one is computed at the
    // view's centre latitude and prints it. For a whole-world view centred on
    // the equator that prints the design's own "at the equator", which is the
    // reason that qualification was on the artboard and should never have been
    // dropped. Mercator is conformal, so at the stated latitude the same figure
    // is true vertically as well.
    {
        const double refLat = std::clamp(centreLat_, -89.0, 89.0);
        const double kmPerPx = 111.32 * std::cos(refLat * kPi / 180.0) * lonSpan /
                               static_cast<double>(width);
        // Within a whisker of a pole there is no honest horizontal figure to
        // print, so nothing is printed. A bar with no number, or a number
        // computed from a scale of zero, would both be worse than a gap.
        if (kmPerPx > 1.0e-9) {
            double barKm = 1.0;
            while (barKm / kmPerPx < 60.0) { barKm *= 2.0; }
            const float barPx = static_cast<float>(barKm / kmPerPx);
            const ImVec2 a(origin.x + 12.0f, origin.y + height - 26.0f);
            const ImVec2 b(a.x + barPx, a.y);
            // Printed on the chart in cream, one tone below the ivory the
            // receiver's own mark takes: the bar states the map's scale, it is
            // not a figure the receiver is reporting.
            const ImU32 barCol = theme::withAlpha(theme::kCream, 220.0f / 255.0f);
            dl->AddLine(a, b, barCol, 2.0f);
            // End ticks, so the bar's extent is unambiguous: a bare line has
            // no stated end and a scale read half a pixel long is a scale read
            // wrong.
            dl->AddLine(ImVec2(a.x, a.y - 4.0f), ImVec2(a.x, a.y + 4.0f), barCol, 2.0f);
            dl->AddLine(ImVec2(b.x, b.y - 4.0f), ImVec2(b.x, b.y + 4.0f), barCol, 2.0f);
            char buf[64];
            if (std::fabs(refLat) < 0.5) {
                std::snprintf(buf, sizeof buf, "%.0f km at the equator", barKm);
            } else {
                std::snprintf(buf, sizeof buf, "%.0f km at %.0f %c", barKm,
                              std::fabs(refLat), (refLat < 0.0) ? 'S' : 'N');
            }
            addMapLabel(dl, ImVec2(a.x, a.y - 15.0f), barCol, buf);
        }
    }

    dl->PopClipRect();
}

// ============================================================================
// THE SATELLITES MAP WINDOW
// ============================================================================
//
// One window that IS the satellite instrument: receiver position, overlays,
// trail style, coverage, the target register, the selected target's figures
// and the map. Nothing satellite-shaped is left scattered in the main window's
// rail; the rail row is a switch that opens this, and nothing more.
//
// THE ONE RULE THIS FILE WAS REWRITTEN UNDER. Every figure drawn here comes
// from something the application actually measures. The plugin ABI carries a
// track's id, label, position, altitude, course, speed and the age of the fix,
// and it carries polylines; that is the whole of what a satellite page can
// honestly show. INCLINATION, ORBITAL PERIOD, the age of the element set and
// the next passes are NOT in it - a previous draft of this window printed four
// such numbers and every one of them was invented. They are not drawn, and
// where their absence is the answer to a question the user is asking, this
// window says so in words rather than leaving a blank.
//
// AND THERE ARE TWO KINDS OF MISSING, drawn differently on purpose:
//
//   RECOVERABLE - distance and bearing, which need a receiver position. The
//   value is HATCHED, never zeroed, and a note beside it says what would fill
//   it in. Setting a position fills it in.
//
//   NOT REPORTED - inclination, period, element-set age, pass predictions.
//   Nothing the user can click will produce them, because the tracking plugin
//   does not publish them. Those are stated plainly as not reported, so a
//   hatch is never mistaken for "keep clicking".

namespace {

// --- the bench vocabulary this window adds -----------------------------------
//
// scope_face.hpp holds the shared primitives - bevels, screws, rails,
// dividers, plates, group captions, lamps, meters, the drum cells and the
// keys - and every one of them that fits is used below. What is added here is
// what that header does not have and this design needs: a recessed WELL, a
// LABELLED brass key (drawBenchKey is a square with no lettering), a two-
// position ROCKER, a segment of a three-way selector, and a HATCH. They live
// in this file rather than in scope_face.hpp because that header is not this
// agent's to change; if a second window ever wants them, that is the moment
// to promote them rather than to copy them.

float textW(ImFont* f, float px, const char* s) {
    return f->CalcTextSizeA(px, FLT_MAX, 0.0f, s).x;
}

float faceH(ImFont* f, float px) { return f->CalcTextSizeA(px, FLT_MAX, 0.0f, "Ag").y; }

// A figure is a figure only if it is made of figures. fonts.hpp is narrow
// about this for a measured reason - Nova Mono's capitals merge into blocks
// below about 20 px - so the monospaced face is asked for by TESTING the
// string rather than by a call site's opinion of what it holds.
bool allDigits(const char* s) {
    if (s == nullptr || s[0] == '\0') { return false; }
    for (const char* p = s; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') { return false; }
    }
    return true;
}

ImFont* faceForValue(const char* s) {
    return allDigits(s) ? cascade::gui::fonts::reading() : cascade::gui::fonts::ui();
}

// The recessed bay a group of controls sits in: dark enamel cut into the
// panel, a brass lip around it and the bevel lit from below, which is what
// makes it read as a hole rather than as a dark rectangle.
void addDeckWell(ImDrawList* dl, const ImVec2& tl, const ImVec2& br) {
    if (dl == nullptr || br.x - tl.x < 8.0f || br.y - tl.y < 8.0f) { return; }
    const float r = theme::kPanelRounding;
    dl->AddRectFilled(tl, br, theme::kEnamelDark, r);
    // AddRectFilledMultiColor cannot round its corners, so the shape is laid
    // flat first and the gradient inset by the radius - the same trick
    // addBenchPlate uses.
    if (br.x - tl.x > r * 2.0f) {
        dl->AddRectFilledMultiColor(ImVec2(tl.x + r, tl.y), ImVec2(br.x - r, br.y),
                                    theme::kEnamelDark, theme::kEnamelDark, theme::kWell,
                                    theme::kWell);
    }
    dl->AddRect(tl, br, theme::withAlpha(theme::kBrassBright, 0.75f), r, 0, 2.0f);
    addBenchBevel(dl, tl, br, r, false);
}

// A labelled brass key, one or two lines. Disabled draws it drained and
// refuses the click, which is what a control that cannot do its job should
// look like - the sentence saying WHY lives beside it, because a greyed key
// with no explanation is the fault this whole redesign exists to remove.
bool drawDeckKey(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, const char* line1,
                 const char* line2, bool enabled, const char* id) {
    if (dl == nullptr || br.x - tl.x < 8.0f || br.y - tl.y < 8.0f) { return false; }
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(tl);
    ImGui::BeginDisabled(!enabled);
    const bool pressed =
        ImGui::InvisibleButton("##key", ImVec2(br.x - tl.x, br.y - tl.y));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool focused = ImGui::IsItemFocused();
    ImGui::EndDisabled();
    ImGui::PopID();

    const float r = theme::kKeyRounding;
    if (!enabled) {
        dl->AddRectFilled(tl, br, theme::kWell, r);
        dl->AddRect(tl, br, theme::withAlpha(theme::kBrassDark, 0.80f), r, 0,
                    theme::kHairline);
    } else {
        if (!held) {
            // Proud metal casts a shadow; a pressed key does not. That one
            // difference is the state indication before any colour is used.
            dl->AddRectFilled(ImVec2(tl.x + 1.0f, tl.y + 2.0f),
                              ImVec2(br.x + 1.0f, br.y + 2.0f),
                              theme::withAlpha(theme::kVoid, 0.45f), r);
        }
        const ImU32 top = held ? theme::kBrassMid : (hovered ? theme::kIvory : theme::kCream);
        const ImU32 bot = held ? theme::kBrassDark : theme::kBrassBright;
        dl->AddRectFilled(tl, br, bot, r);
        if (br.x - tl.x > r * 2.0f) {
            dl->AddRectFilledMultiColor(ImVec2(tl.x + r, tl.y), ImVec2(br.x - r, br.y), top,
                                        top, bot, bot);
        }
        addBenchBevel(dl, tl, br, r, !held);
    }
    if (focused) {
        dl->AddRect(ImVec2(tl.x - 2.0f, tl.y - 2.0f), ImVec2(br.x + 2.0f, br.y + 2.0f),
                    theme::kBrassBright, r + 1.0f, 0, theme::kHairline);
    }

    // ENGRAVED INTO BRASS, which the design's own rule allows for a caption on
    // metal and forbids for a reading on glass. A dead key letters in the
    // faint ink instead, so it reads as unavailable rather than as unlabelled.
    ImFont* f = cascade::gui::fonts::ui();
    const float px = cascade::gui::fonts::kTinySize;
    const ImU32 ink = enabled ? theme::kEnamel : theme::kInkFaint;
    const float lh = faceH(f, px);
    const int lines = (line2 != nullptr && line2[0] != '\0') ? 2 : 1;
    float y = (tl.y + br.y) * 0.5f - lh * static_cast<float>(lines) * 0.5f +
              (held ? 1.0f : 0.0f);
    const float cx = (tl.x + br.x) * 0.5f;
    dl->AddText(f, px, ImVec2(cx - textW(f, px, line1) * 0.5f, y), ink, line1);
    if (lines == 2) {
        y += lh;
        dl->AddText(f, px, ImVec2(cx - textW(f, px, line2) * 0.5f, y), ink, line2);
    }
    return pressed;
}

// One rocker row: the switch, its label plate, a right-aligned hint and a lamp.
//
// THE HINT IS NOT DECORATION. On a switch that works it says what the switch
// does; on one that is BLOCKED it says what would unblock it, which is the
// design's best idea and the reason no control on this window is ever a bare
// box with a greyed-out look and no explanation.
bool drawRockerRow(ImDrawList* dl, const ImVec2& tl, float width, float rowH,
                   const char* label, const char* hint, bool on, bool blocked,
                   const char* id) {
    if (dl == nullptr || width < 60.0f || rowH < 10.0f) { return false; }
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(tl);
    ImGui::BeginDisabled(blocked);
    const bool pressed = ImGui::InvisibleButton("##rocker", ImVec2(width, rowH));
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    ImGui::EndDisabled();
    ImGui::PopID();

    const float dim = blocked ? 0.55f : 1.0f;
    // The rocker: a dark aperture with a paddle in it, up for on and down for
    // off. Position, not colour, is what says which way it is thrown - a
    // switch readable in a greyscale photograph is readable by everyone.
    const float rw = 18.0f;
    const ImVec2 rTL(tl.x, tl.y + 1.0f);
    const ImVec2 rBR(tl.x + rw, tl.y + rowH - 1.0f);
    dl->AddRectFilled(rTL, rBR, theme::kVoid, theme::kKeyRounding);
    dl->AddRect(rTL, rBR, theme::withAlpha(theme::kBrassMid, 0.9f * dim),
                theme::kKeyRounding, 0, theme::kHairline);
    const float ph = (rBR.y - rTL.y) * 0.42f;
    const ImVec2 pTL(rTL.x + 2.0f, on ? rTL.y + 2.0f : rBR.y - 2.0f - ph);
    const ImVec2 pBR(rBR.x - 2.0f, pTL.y + ph);
    dl->AddRectFilled(pTL, pBR,
                      theme::withAlpha(on ? theme::kCream : theme::kBrassMid, dim), 1.0f);
    addBenchBevel(dl, pTL, pBR, 1.0f, true);

    // The label plate: lit brass with ink lettering when on, an engraved
    // outline when off. Same object either way, so the eye compares one thing.
    ImFont* f = cascade::gui::fonts::ui();
    const float px = cascade::gui::fonts::kTinySize;
    const float lw = textW(f, px, label);
    const float lh = faceH(f, px);
    const ImVec2 lTL(tl.x + rw + 8.0f, tl.y + (rowH - lh - 6.0f) * 0.5f);
    const ImVec2 lBR(lTL.x + lw + 14.0f, lTL.y + lh + 6.0f);
    if (on) {
        dl->AddRectFilled(lTL, lBR, theme::withAlpha(theme::kBrassBright, dim),
                          theme::kKeyRounding);
        addBenchBevel(dl, lTL, lBR, theme::kKeyRounding, true);
    } else {
        dl->AddRect(lTL, lBR, theme::withAlpha(theme::kBrassDark, 0.9f * dim),
                    theme::kKeyRounding, 0, theme::kHairline);
    }
    dl->AddText(f, px, ImVec2(lTL.x + 7.0f, lTL.y + 3.0f),
                theme::withAlpha(on ? theme::kEnamel : theme::kCream, dim), label);
    if (hovered && !blocked) {
        dl->AddRect(lTL, lBR, theme::withAlpha(theme::kBrassBright, 0.7f),
                    theme::kKeyRounding, 0, theme::kHairline);
    }
    if (focused) {
        dl->AddRect(ImVec2(tl.x - 2.0f, tl.y - 1.0f), ImVec2(tl.x + width + 2.0f,
                                                             tl.y + rowH + 1.0f),
                    theme::kBrassBright, theme::kKeyRounding, 0, theme::kHairline);
    }

    // The lamp at the right end, and the hint tucked in before it.
    const float lampR = 4.5f;
    const ImVec2 lampC(tl.x + width - lampR - 1.0f, tl.y + rowH * 0.5f);
    // PHOSPHOR, because a lit lamp here says a layer is being drawn from what
    // the receiver's plugins reported. Unlit is the dark aperture itself.
    drawBenchLamp(dl, lampC, lampR, theme::kPhosphor, on && !blocked, nullptr);

    if (hint != nullptr && hint[0] != '\0') {
        ImFont* hf = cascade::gui::fonts::legend();
        const float hpx = cascade::gui::fonts::kTinySize;
        const float hw = textW(hf, hpx, hint);
        const float hx = tl.x + width - lampR * 2.0f - 8.0f - hw;
        if (hx > lBR.x + 8.0f) {
            // A blocked switch's hint is the REASON, and a reason is trouble
            // the user can clear - so it takes the amber a note takes, not the
            // engraved grey of an ordinary caption.
            dl->AddText(hf, hpx, ImVec2(hx, tl.y + (rowH - faceH(hf, hpx)) * 0.5f),
                        blocked ? theme::kGold : theme::kInkFaint, hint);
        }
    }
    return pressed && !blocked;
}

// One segment of a three-way selector. The selected one is a key PRESSED IN -
// the same idiom drawBenchKey uses for an engaged function key - rather than a
// coloured tab: rust in this palette means trouble, and a trail drawn as a
// line is not trouble.
bool drawSegment(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, const char* label,
                 bool selected, const char* id) {
    if (dl == nullptr || br.x - tl.x < 6.0f) { return false; }
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(tl);
    const bool pressed = ImGui::InvisibleButton("##seg", ImVec2(br.x - tl.x, br.y - tl.y));
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    ImGui::PopID();

    const float r = theme::kKeyRounding;
    if (selected) {
        dl->AddRectFilled(tl, br, theme::kBrassDark, r);
        dl->AddRectFilledMultiColor(tl, ImVec2(br.x, tl.y + (br.y - tl.y) * 0.45f),
                                    theme::withAlpha(theme::kVoid, 0.55f),
                                    theme::withAlpha(theme::kVoid, 0.55f),
                                    theme::withAlpha(theme::kVoid, 0.0f),
                                    theme::withAlpha(theme::kVoid, 0.0f));
    } else {
        dl->AddRectFilled(ImVec2(tl.x + 1.0f, tl.y + 2.0f), ImVec2(br.x + 1.0f, br.y + 2.0f),
                          theme::withAlpha(theme::kVoid, 0.40f), r);
        dl->AddRectFilled(tl, br, hovered ? theme::kBrassBright : theme::kBrassMid, r);
    }
    addBenchBevel(dl, tl, br, r, !selected);
    if (focused) {
        dl->AddRect(ImVec2(tl.x - 2.0f, tl.y - 2.0f), ImVec2(br.x + 2.0f, br.y + 2.0f),
                    theme::kBrassBright, r + 1.0f, 0, theme::kHairline);
    }
    ImFont* f = cascade::gui::fonts::ui();
    const float px = cascade::gui::fonts::kTinySize;
    dl->AddText(f, px,
                ImVec2((tl.x + br.x) * 0.5f - textW(f, px, label) * 0.5f,
                       (tl.y + br.y) * 0.5f - faceH(f, px) * 0.5f + (selected ? 1.0f : 0.0f)),
                selected ? theme::kCream : theme::kEnamel, label);
    return pressed;
}

// A HATCHED VALUE: this cannot be computed yet, and here is the space it will
// occupy when it can. Diagonal ruling rather than a zero, because "0 km" and
// "we cannot work it out" are opposite statements and this product has been
// bitten by exactly that conflation before.
void addHatch(ImDrawList* dl, const ImVec2& tl, const ImVec2& br) {
    if (dl == nullptr || br.x - tl.x < 4.0f || br.y - tl.y < 4.0f) { return; }
    dl->PushClipRect(tl, br, true);
    const float h = br.y - tl.y;
    const ImU32 col = theme::withAlpha(theme::kInkMuted, 0.28f);
    for (float x = tl.x - h; x < br.x; x += 6.0f) {
        dl->AddLine(ImVec2(x, br.y), ImVec2(x + h, tl.y), col, 2.0f);
    }
    dl->PopClipRect();
}

// A note: a coloured rule down the left, a wash behind it and the sentence
// itself. `accent` carries the meaning - phosphor for a state that is working,
// gold for something the user can fix, rust for something refused.
float noteHeight(float width, const char* text) {
    ImFont* f = cascade::gui::fonts::ui();
    const float px = cascade::gui::fonts::kTinySize;
    const float wrap = width - 10.0f;
    if (wrap < 20.0f) { return faceH(f, px) + 8.0f; }
    return f->CalcTextSizeA(px, FLT_MAX, wrap, text).y + 8.0f;
}

void drawNote(ImDrawList* dl, const ImVec2& tl, float width, ImU32 accent,
              const char* text) {
    if (dl == nullptr || width < 30.0f) { return; }
    ImFont* f = cascade::gui::fonts::ui();
    const float px = cascade::gui::fonts::kTinySize;
    const float h = noteHeight(width, text);
    dl->AddRectFilled(tl, ImVec2(tl.x + width, tl.y + h), theme::withAlpha(accent, 0.10f));
    dl->AddRectFilled(tl, ImVec2(tl.x + 2.0f, tl.y + h), accent);
    dl->AddText(f, px, ImVec2(tl.x + 8.0f, tl.y + 4.0f), accent, text, nullptr,
                width - 10.0f);
}

// --- the coordinate cells -----------------------------------------------------
//
// A latitude READS AS A LATITUDE, in a row of machined apertures, and never as
// "0.00000" in an unlabelled box. drawFreqDrumCell is exactly this control and
// already exists; all that is added here is the arithmetic that decides how
// wide each aperture is, since a sign and a decimal point do not need a
// digit's width.
constexpr float kCellW = 13.0f;
constexpr float kCellNarrowW = 8.0f;
constexpr float kCellH = 26.0f;
constexpr float kCellGap = 2.0f;

bool narrowCell(char c) { return c == '.' || c == '+' || c == '-'; }

// SCALED, because a third of a narrow window is not as wide as a third of a
// wide one and a latitude has a fixed number of apertures. Nine cells at full
// size need about 125 px and two coordinates plus their gap need 270; a 900 px
// window gives this well 268. Shrinking the drums together keeps the whole
// coordinate readable, where letting them overflow hid the last four digits
// behind the button beside them - which is a receiver position the user cannot
// check.
float coordCellsWidth(const char* text, float scale) {
    float w = 0.0f;
    for (const char* p = text; *p != '\0'; ++p) {
        w += (narrowCell(*p) ? kCellNarrowW : kCellW) * scale;
        w += kCellGap;
    }
    return (w > 0.0f) ? w - kCellGap : 0.0f;
}

// `known` is the receiver position's existence, and it does TWO things: it
// lights the drums, and it decides whether the apertures carry figures at all.
// Without a position the caller has nothing to format and hands this the
// counter's SHAPE with zeros in it; showing those zeros would put the receiver
// at 0N 0E on the one control whose whole job is to say where it is. See
// coordApertureGlyph - the shape (and therefore every width) is unchanged, so
// the well does not resize when a position arrives.
void drawCoordCells(ImDrawList* dl, const ImVec2& tl, const char* text, bool known,
                    float scale) {
    float x = tl.x;
    // Never below 9 px: a drum digit smaller than that is a smudge, and at
    // that point the window is too narrow for this control however it is laid
    // out. The clip on the well is what keeps the overflow tidy.
    const float px = std::max(9.0f, cascade::gui::fonts::kReadingSize * scale);
    for (const char* p = text; *p != '\0'; ++p) {
        const float w = (narrowCell(*p) ? kCellNarrowW : kCellW) * scale;
        drawFreqDrumCell(dl, ImVec2(x, tl.y), ImVec2(x + w, tl.y + kCellH * scale),
                         cascade::gui::coordApertureGlyph(*p, known), known, px);
        x += w + kCellGap;
    }
}

// "+51.50720" and "+002.10400": one sign cell, a fixed number of whole
// degrees and five decimal places, which is about a metre on the ground and is
// what the typed entry has always stored.
void formatLat(double deg, char* out, std::size_t n) {
    std::snprintf(out, n, "%+09.5f", deg);
}
void formatLon(double deg, char* out, std::size_t n) {
    std::snprintf(out, n, "%+010.5f", deg);
}

// --- the register's own formatting -------------------------------------------

// The age of a fix, in the unit that makes it readable: seconds while it is
// seconds, minutes once it is minutes. NOT a bare number - a column of ages
// with no unit is a column of nothing.
void formatAge(std::uint64_t ageMs, char* out, std::size_t n) {
    const double s = static_cast<double>(ageMs) / 1000.0;
    if (s < 60.0) {
        std::snprintf(out, n, "%.0f s", s);
    } else {
        std::snprintf(out, n, "%.1f min", s / 60.0);
    }
}

}  // namespace

TrackSortKey satelliteSortKey(int index) {
    switch (index) {
        case 0: return TrackSortKey::Label;
        case 1: return TrackSortKey::Id;
        case 2: return TrackSortKey::Altitude;
        case 3: return TrackSortKey::Age;
        default: return TrackSortKey::Label;
    }
}

const char* satelliteSortKeyLabel(int index) {
    switch (index) {
        case 0: return "CALLSIGN";
        case 1: return "NORAD";
        case 2: return "ALT";
        case 3: return "AGE";
        default: return "CALLSIGN";
    }
}

// See map_view.hpp. A figure becomes a dash, a sign becomes nothing, and
// everything else - the decimal point - is left where it is so the counter
// still reads as a coordinate rather than as a broken display.
char coordApertureGlyph(char shape, bool known) {
    if (known) { return shape; }
    if (shape >= '0' && shape <= '9') { return '-'; }
    if (shape == '+' || shape == '-') { return ' '; }
    return shape;
}

// See map_view.hpp. NEITHER NOTE WITHOUT A TARGET, which is the correction:
// both of them are sentences about a selected target's figures, and the card
// with nothing selected draws neither - so reserving space for them left a
// tall empty box on the first screen a new install shows, and left it a
// different height depending on a receiver position it was not mentioning.
SatelliteCardNotes satelliteCardNotes(bool haveTarget, bool haveReceiverPosition) {
    SatelliteCardNotes n;
    n.noReceiver = haveTarget && !haveReceiverPosition;
    n.notReported = haveTarget;
    return n;
}

void MapView::drawSatellitePanel(SatelliteDeck& deck,
                                 const std::vector<cascade::core::HostTrack>& tracks,
                                 const std::vector<cascade::core::HostPath>& paths,
                                 const CoverageMap* coverage, TrackInfoCache* info) {
    ImGui::PushID("satpanel");

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 60.0f || avail.y < 60.0f) {
        ImGui::PopID();
        return;
    }

    ImFont* uiF = cascade::gui::fonts::ui();
    ImFont* lgF = cascade::gui::fonts::legend();
    const float tinyPx = cascade::gui::fonts::kTinySize;
    const float uiPx = cascade::gui::fonts::kUiSize;
    const float legPx = cascade::gui::fonts::kLegendSize;
    const float tinyH = faceH(lgF, tinyPx);
    const float smallH = faceH(uiF, tinyPx);
    const float uiH = faceH(uiF, uiPx);

    constexpr float kPad = 10.0f;
    constexpr float kGap = 10.0f;
    constexpr float kRockerH = 22.0f;
    constexpr float kSegH = 26.0f;
    constexpr float kKeyH = 30.0f;

    // Whether the position that everything else here is measured FROM exists.
    // Its absence is the single fact that blocks the most controls on this
    // window, which is why the note under it is the longest one on the deck.
    const bool haveRx = hasHome_;

    // ======================= THE CONTROL DECK =============================
    const float wellW = (avail.x - kGap * 2.0f) / 3.0f;
    const float noteW = wellW - kPad * 2.0f;

    char latText[24];
    char lonText[24];
    formatLat(haveRx ? homeLat_ : 0.0, latText, sizeof latText);
    formatLon(haveRx ? homeLon_ : 0.0, lonText, sizeof lonText);

    const char* rxNote =
        haveRx ? "Position set. Distance and bearing are computed for every target, and "
                 "coverage is measured from here."
               : "No position set. Distance, bearing and the coverage overlay stay blank "
                 "until one is - none of them can be computed without it.";
    // WHAT THE ACCUMULATOR ACTUALLY HOLDS, decided here rather than at the
    // point it is drawn, because the deck's height is measured from the
    // sentence that will be in it - and a note sized from a different string
    // to the one drawn is a note that clips itself.
    //
    // AND IT SAYS WHAT IT MEASURES, which is not what it used to claim. The
    // ring read "nothing HEARD yet" and "the ring fills as targets are
    // DECODED", and neither is what the accumulator is fed: it takes every
    // visible track from every plugin, and a track's position may have been
    // COMPUTED rather than received - which for satellites is the normal case,
    // since a propagator reports a position for a satellite on the far side of
    // the planet that this receiver never heard at all. Restricting the
    // accumulator to genuine receptions is not this view's to do (it is fed in
    // the window that owns it, from a track vector that does not distinguish
    // the two), so the words are corrected to the measurement that exists: how
    // far out, on each of 72 bearings, anything has been PLOTTED.
    char covMeasured[200];
    const char* covNote;
    if (!haveRx) {
        covNote =
            "Coverage measures distances from the receiver, so it needs a position "
            "first. Set one on the left.";
    } else if (coverage == nullptr || coverage->empty()) {
        covNote =
            "Nothing plotted yet. The ring records how far out a target has been PLOTTED "
            "on each of 72 bearings - from every plugin, and from computed positions as "
            "well as received ones.";
    } else {
        std::snprintf(covMeasured, sizeof covMeasured,
                      "%d of %d bearings reached, furthest %.0f km. Plotted positions, "
                      "computed or received. RESET clears the ring and starts again.",
                      coverage->filledBuckets(), CoverageMap::kBuckets,
                      coverage->peakKm());
        covNote = covMeasured;
    }
    const bool coordInvalid =
        coordEditing_ >= 0 &&
        !(deck.latInput >= -90.0 && deck.latInput <= 90.0 && deck.lonInput >= -180.0 &&
          deck.lonInput <= 180.0);
    const char* rxShown =
        coordInvalid ? "Refused: latitude must be -90 to 90 and longitude -180 to 180."
                     : rxNote;

    // HOW THE RECEIVER WELL ARRANGES ITSELF, decided from what it is actually
    // given rather than from the artboard's width. Three states, in order of
    // preference: cells and key side by side; the key on its own line beneath
    // them; and, only when even the cells alone will not fit, the cells shrunk
    // together until they do.
    const float rxInnerW = wellW - kPad * 2.0f;
    const float coordsFull =
        coordCellsWidth(latText, 1.0f) + coordCellsWidth(lonText, 1.0f) + 12.0f + 12.0f;
    const bool keyBeside = (coordsFull + 74.0f) <= rxInnerW;
    const float coordScale =
        (coordsFull <= rxInnerW) ? 1.0f
                                 : std::clamp(rxInnerW / coordsFull, 0.55f, 1.0f);
    const float latW = coordCellsWidth(latText, coordScale);
    const float lonW = coordCellsWidth(lonText, coordScale);
    const float cellRowH = kCellH * coordScale + 6.0f;

    const float deckA = kPad + tinyH + 8.0f + smallH + 3.0f + cellRowH +
                        (keyBeside ? 0.0f : (6.0f + kKeyH)) + 10.0f +
                        noteHeight(noteW, rxShown) + kPad;
    const float deckB = kPad + tinyH + 8.0f + kRockerH * 3.0f + 12.0f + kPad;
    const float deckC = kPad + tinyH + 8.0f + kSegH + 12.0f + tinyH + 8.0f +
                        std::max(kKeyH, noteHeight(noteW - 74.0f, covNote)) + kPad;
    const float deckH = std::max(deckA, std::max(deckB, deckC));

    ImGui::Dummy(ImVec2(avail.x, deckH));
    const ImVec2 afterDeck = ImGui::GetCursorScreenPos();

    // ---- RECEIVER POSITION ------------------------------------------------
    {
        const ImVec2 tl(origin.x, origin.y);
        const ImVec2 br(tl.x + wellW, tl.y + deckH);
        addDeckWell(dl, tl, br);
        dl->PushClipRect(ImVec2(tl.x + 2.0f, tl.y + 2.0f), ImVec2(br.x - 2.0f, br.y - 2.0f),
                         true);
        float y = tl.y + kPad;
        addBenchGroupCaption(dl, ImVec2(tl.x + kPad, y), wellW - kPad * 2.0f,
                             "RECEIVER POSITION");
        y += tinyH + 8.0f;

        // Wide enough for two lines of lettering, and CAPPED: on a wide window
        // the leftover after two coordinate wells is most of the panel, and a
        // brass key four hundred pixels across stops reading as a key.
        const float keyW =
            keyBeside ? std::clamp(rxInnerW - coordsFull, 74.0f, 190.0f)
                      : std::clamp(rxInnerW, 74.0f, 190.0f);

        // EVERY CONTROL IS CAPTIONED. Two fields showing 0.00000 with nothing
        // to say which was which is the exact defect this window was redrawn
        // for; the words are part of the control, not an optional label.
        dl->AddText(uiF, tinyPx, ImVec2(tl.x + kPad, y), theme::kInkMuted, "LATITUDE");
        dl->AddText(uiF, tinyPx, ImVec2(tl.x + kPad + latW + 6.0f + 12.0f, y),
                    theme::kInkMuted, "LONGITUDE");
        y += smallH + 3.0f;

        // The two wells, and the click-to-type they carry. A machined aperture
        // is not a text box, so it becomes one only while it is being edited -
        // which keeps the typed entry the toolbar always had without putting a
        // form field in the middle of an instrument.
        const auto cellField = [&](int which, const char* text, float cellsW, float x) {
            const ImVec2 wTL(x, y);
            const ImVec2 wBR(x + cellsW + 6.0f, y + cellRowH);
            drawFreqDrumWell(dl, wTL, wBR);
            if (coordEditing_ == which) {
                ImGui::SetCursorScreenPos(ImVec2(wTL.x + 1.0f, wTL.y + 1.0f));
                ImGui::SetNextItemWidth(wBR.x - wTL.x - 2.0f);
                if (coordEditFocus_) {
                    ImGui::SetKeyboardFocusHere();
                    coordEditFocus_ = false;
                }
                double* v = (which == 0) ? &deck.latInput : &deck.lonInput;
                const bool entered =
                    ImGui::InputDouble(which == 0 ? "##latedit" : "##lonedit", v, 0.0, 0.0,
                                       "%.5f", ImGuiInputTextFlags_EnterReturnsTrue);
                if (entered) {
                    // REFUSED RATHER THAN CLAMPED, by the same positive range
                    // test the config sanitizer uses: a typo must not be able
                    // to install a receiver at the pole and quietly make every
                    // distance on the window wrong.
                    if (deck.latInput >= -90.0 && deck.latInput <= 90.0 &&
                        deck.lonInput >= -180.0 && deck.lonInput <= 180.0) {
                        homeRequestLat_ = deck.latInput;
                        homeRequestLon_ = deck.lonInput;
                        homeRequest_ = true;
                        coordEditing_ = -1;
                    }
                } else if (ImGui::IsItemDeactivated()) {
                    // Clicked away without pressing Enter: nothing is applied,
                    // exactly as the toolbar's fields behaved before the
                    // button was pressed.
                    coordEditing_ = -1;
                }
                return;
            }
            drawCoordCells(dl, ImVec2(wTL.x + 3.0f, wTL.y + 3.0f), text, haveRx, coordScale);
            ImGui::PushID(which);
            ImGui::SetCursorScreenPos(wTL);
            if (ImGui::InvisibleButton("##coordcell", ImVec2(wBR.x - wTL.x, wBR.y - wTL.y))) {
                deck.latInput = haveRx ? homeLat_ : 0.0;
                deck.lonInput = haveRx ? homeLon_ : 0.0;
                coordEditing_ = which;
                coordEditFocus_ = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Click to type this coordinate, then press Enter.");
            }
            ImGui::PopID();
        };
        cellField(0, latText, latW, tl.x + kPad);
        cellField(1, lonText, lonW, tl.x + kPad + latW + 6.0f + 12.0f);

        // ARMED SAYS SO ON THE KEY ITSELF. A latch whose only sign is a
        // changed cursor over a map two panels away is a mode the user cannot
        // see they are in.
        const ImVec2 keyTL(keyBeside ? (br.x - kPad - keyW) : (tl.x + kPad),
                           keyBeside ? y : (y + cellRowH + 6.0f));
        const float keyH = keyBeside ? cellRowH : kKeyH;
        if (drawDeckKey(dl, keyTL, ImVec2(keyTL.x + keyW, keyTL.y + keyH),
                        pickHomeArmed_ ? "CLICK THE" : "SET FROM",
                        pickHomeArmed_ ? "MAP NOW" : "MAP CLICK", true, "setrx")) {
            pickHomeArmed_ = !pickHomeArmed_;
        }
        y += cellRowH + (keyBeside ? 0.0f : (6.0f + kKeyH)) + 10.0f;

        drawNote(dl, ImVec2(tl.x + kPad, y), noteW,
                 coordInvalid ? theme::kAlarm : (haveRx ? theme::kPhosphor : theme::kGold),
                 rxShown);
        dl->PopClipRect();
    }

    // ---- MAP OVERLAYS -----------------------------------------------------
    {
        const ImVec2 tl(origin.x + wellW + kGap, origin.y);
        const ImVec2 br(tl.x + wellW, tl.y + deckH);
        addDeckWell(dl, tl, br);
        dl->PushClipRect(ImVec2(tl.x + 2.0f, tl.y + 2.0f), ImVec2(br.x - 2.0f, br.y - 2.0f),
                         true);
        float y = tl.y + kPad;
        addBenchGroupCaption(dl, ImVec2(tl.x + kPad, y), wellW - kPad * 2.0f, "MAP OVERLAYS");
        y += tinyH + 8.0f;
        const float rowW = wellW - kPad * 2.0f;

        // COVERAGE is blocked with its reason attached, never merely greyed.
        if (drawRockerRow(dl, ImVec2(tl.x + kPad, y), rowW, kRockerH, "COVERAGE",
                          haveRx ? "furthest plotted, by bearing"
                                 : "needs a receiver position",
                          deck.coverage && haveRx, !haveRx, "ovcov")) {
            deck.coverage = !deck.coverage;
        }
        y += kRockerH;
        if (drawRockerRow(dl, ImVec2(tl.x + kPad, y), rowW, kRockerH, "GROUND TRACKS",
                          "the paths the tracker publishes", deck.groundTracks, false,
                          "ovtrk")) {
            deck.groundTracks = !deck.groundTracks;
        }
        y += kRockerH;
        if (drawRockerRow(dl, ImVec2(tl.x + kPad, y), rowW, kRockerH, "ALTITUDE COLOURS",
                          "keyed on the map", deck.altitudeColours, false, "ovalt")) {
            deck.altitudeColours = !deck.altitudeColours;
        }
        dl->PopClipRect();
    }

    // ---- TRAIL STYLE and COVERAGE -----------------------------------------
    {
        const ImVec2 tl(origin.x + (wellW + kGap) * 2.0f, origin.y);
        const ImVec2 br(tl.x + wellW, tl.y + deckH);
        addDeckWell(dl, tl, br);
        dl->PushClipRect(ImVec2(tl.x + 2.0f, tl.y + 2.0f), ImVec2(br.x - 2.0f, br.y - 2.0f),
                         true);
        float y = tl.y + kPad;
        addBenchGroupCaption(dl, ImVec2(tl.x + kPad, y), wellW - kPad * 2.0f, "TRAIL STYLE");
        y += tinyH + 8.0f;

        // THREE BUTTONS, NOT A MENU: the whole option set visible at once. The
        // third is OFF, which is the same flag the GROUND TRACKS rocker throws
        // - one piece of state with two controls on it, exactly as the design
        // has it, rather than two settings that can contradict each other.
        //
        // AND THE MIDDLE ONE IS RIBBON, NOT DOTS. This renderer draws a trail
        // as a line or as a translucent ribbon; there is no dotted style, and
        // lettering a button DOTS that produced a ribbon would be a control
        // that lies about what it does.
        const float segW = (wellW - kPad * 2.0f - 8.0f) / 3.0f;
        const char* segs[3] = {"LINE", "RIBBON", "OFF"};
        const int current = !deck.groundTracks ? 2 : (deck.trailStyle == 1 ? 1 : 0);
        for (int i = 0; i < 3; ++i) {
            const ImVec2 sTL(tl.x + kPad + (segW + 4.0f) * static_cast<float>(i), y);
            char segId[8];
            std::snprintf(segId, sizeof segId, "seg%d", i);
            if (drawSegment(dl, sTL, ImVec2(sTL.x + segW, sTL.y + kSegH), segs[i],
                            current == i, segId)) {
                if (i == 2) {
                    deck.groundTracks = false;
                } else {
                    deck.groundTracks = true;
                    deck.trailStyle = i;
                }
            }
        }
        y += kSegH + 12.0f;

        addBenchGroupCaption(dl, ImVec2(tl.x + kPad, y), wellW - kPad * 2.0f, "COVERAGE");
        y += tinyH + 8.0f;
        const bool canReset = coverage != nullptr && !coverage->empty();
        if (drawDeckKey(dl, ImVec2(tl.x + kPad, y), ImVec2(tl.x + kPad + 64.0f, y + kKeyH),
                        "RESET", nullptr, canReset, "covreset")) {
            coverageResetRequest_ = true;
        }
        drawNote(dl, ImVec2(tl.x + kPad + 74.0f, y), noteW - 74.0f,
                 haveRx ? theme::kPhosphor : theme::kGold, covNote);
        dl->PopClipRect();
    }

    // ======================= THE BODY =====================================
    ImGui::SetCursorScreenPos(ImVec2(origin.x, afterDeck.y + kGap));
    const ImVec2 bodyTL = ImGui::GetCursorScreenPos();
    const float bodyH = std::max(80.0f, origin.y + avail.y - bodyTL.y);
    const float regW = std::clamp(avail.x * 0.30f, 260.0f, 430.0f);
    const float mapW = avail.x - regW - kGap;

    // ---- the register -----------------------------------------------------
    {
        const ImVec2 tl = bodyTL;
        const ImVec2 br(tl.x + regW, tl.y + bodyH);
        addDeckWell(dl, tl, br);
        dl->PushClipRect(ImVec2(tl.x + 2.0f, tl.y + 2.0f), ImVec2(br.x - 2.0f, br.y - 2.0f),
                         true);
        const float innerL = tl.x + kPad;
        const float innerR = br.x - kPad;
        const float innerW = innerR - innerL;
        float y = tl.y + kPad;

        // --- header: what it is, and how many of them there are ------------
        dl->AddText(lgF, legPx, ImVec2(innerL, y), theme::kIvory, "TARGETS");
        const std::size_t shown = cascade::core::visibleTrackCount(tracks);
        char countText[32];
        std::snprintf(countText, sizeof countText, "%d TRACKED", static_cast<int>(shown));
        dl->AddText(uiF, tinyPx,
                    ImVec2(innerR - textW(uiF, tinyPx, countText),
                           y + faceH(lgF, legPx) - smallH),
                    theme::kPhosphor, countText);
        y += faceH(lgF, legPx) + 6.0f;
        addBenchRail(dl, innerL, innerR, y);
        y += 8.0f;

        // --- sort: four visible keys and a direction ------------------------
        if (deck.sortKey < 0 || deck.sortKey >= kSatelliteSortKeyCount) { deck.sortKey = 0; }
        const float sortCapW = textW(lgF, tinyPx, "SORT") + 8.0f;
        dl->AddText(lgF, tinyPx, ImVec2(innerL, y + 4.0f), theme::kInkFaint, "SORT");
        const float dirW = 42.0f;
        const float keysW = innerW - sortCapW - dirW - 6.0f;
        const float keyW = (keysW - 3.0f * 3.0f) / static_cast<float>(kSatelliteSortKeyCount);
        for (int i = 0; i < kSatelliteSortKeyCount; ++i) {
            const ImVec2 kTL(innerL + sortCapW + (keyW + 3.0f) * static_cast<float>(i), y);
            char keyId[8];
            std::snprintf(keyId, sizeof keyId, "sk%d", i);
            if (drawSegment(dl, kTL, ImVec2(kTL.x + keyW, kTL.y + kRockerH),
                            satelliteSortKeyLabel(i), deck.sortKey == i, keyId)) {
                deck.sortKey = i;
            }
        }
        {
            const ImVec2 dTL(innerR - dirW, y);
            if (drawDeckKey(dl, dTL, ImVec2(dTL.x + dirW, dTL.y + kRockerH),
                            deck.sortAscending ? "ASC" : "DESC", nullptr, true, "sortdir")) {
                deck.sortAscending = !deck.sortAscending;
            }
        }
        y += kRockerH + 8.0f;

        // --- the rows, reduced to numbers first -----------------------------
        std::vector<TrackRow> rows;
        rows.reserve(shown);
        bool allOrbital = true;
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            const cascade::core::HostTrack& ht = tracks[i];
            if (!cascade::core::trackPresentation(ht.t.ageMs, ht.t.kind).visible) {
                continue;
            }
            TrackRow r;
            r.label = (ht.t.label[0] != '\0') ? ht.t.label : ht.t.id;
            r.id = ht.t.id;
            r.altM = ht.t.altM;
            r.speedMps = ht.t.speedMps;
            r.courseDeg = ht.t.courseDeg;
            if (hasHome_) {
                r.distanceKm = greatCircleKm(homeLat_, homeLon_, ht.t.latDeg, ht.t.lonDeg);
                r.bearingDeg =
                    initialBearingDeg(homeLat_, homeLon_, ht.t.latDeg, ht.t.lonDeg);
            }
            r.ageMs = ht.t.ageMs;
            r.source = i;
            if (!orbitalLadder(ht.t.kind)) { allOrbital = false; }
            rows.push_back(std::move(r));
        }
        sortTrackRows(rows, satelliteSortKey(deck.sortKey), deck.sortAscending);

        // THE ALTITUDE COLUMN NAMES ITS OWN UNIT, and which unit that is
        // depends on what is in the register. With satellites only, the head
        // carries "km" and the cells are bare figures in the counter face -
        // which is what makes a column of altitudes scan. The moment anything
        // that is not a satellite appears, one head cannot name both units, so
        // every cell carries its own.
        const char* altHead = allOrbital ? "ALT km" : "ALT";

        // --- what has to fit UNDER the rows, measured before they are sized --
        const cascade::core::HostTrack* sel = nullptr;
        for (const auto& ht : tracks) {
            if (!selectedId_.empty() && selectedId_ == ht.t.id &&
                cascade::core::trackPresentation(ht.t.ageMs, ht.t.kind).visible) {
                sel = &ht;
                break;
            }
        }
        // MEASURED AT THE NARROW WIDTH, always. Whether the column scrolls is
        // not known until the cards' heights are, and a note measured without
        // the scrollbar and then drawn with one wraps to an extra line inside
        // a box reserved for one fewer. Measuring at the narrower of the two
        // can only ever over-reserve, which shows as a few spare pixels.
        const float measureW = innerW - ImGui::GetStyle().ScrollbarSize;
        const char* notReported =
            "INCLINATION, ORBITAL PERIOD and the age of the element set are not part of "
            "what a track source reports, so nothing here can show them - and no receiver "
            "position will change that.";
        const char* passNote =
            "A pass prediction needs the orbit itself. A track source reports a position, "
            "not the elements it came from, so these cannot be computed here - a receiver "
            "position alone would not be enough.";
        const char* noRxNote =
            "DISTANCE and BEARING are hatched because no receiver position is set. Set "
            "one on the deck above and both fill in.";
        const char* noSelPrompt = "Click a row above, or a marker on the map.";
        const float detailRowH = smallH + faceH(uiF, tinyPx) + 9.0f;
        // BOTH NOTES, WHEN BOTH ARE TRUE, and they very often are. What stood
        // here picked one or the other: the "not reported" note appeared only
        // once a receiver position had been set. But the two are different
        // KINDS of missing and they are true at the same time - a position
        // would unblock distance and bearing, and nothing will ever unblock
        // inclination or period, because the plugin does not send them. Shown
        // one at a time, a user without a position was told only about the
        // blank a click can fill and left to assume the rest would follow.
        //
        // AND NEITHER OF THEM WITHOUT A TARGET, which is what this was getting
        // wrong on the one screen a new install actually shows: nothing
        // selected and no position set drew the words NO TARGET SELECTED into
        // a box reserved for six figure cells and two paragraphs of note, and
        // made that box a different height depending on a receiver position it
        // was not mentioning. Both the reservation and the drawing now read
        // one answer - satelliteCardNotes - so the box can never be reserved
        // for a sentence that is not drawn.
        const SatelliteCardNotes cardNotes = satelliteCardNotes(sel != nullptr, hasHome_);
        const float detailNotesH =
            (cardNotes.noReceiver ? (noteHeight(measureW - 16.0f, noRxNote) + 6.0f) : 0.0f) +
            (cardNotes.notReported ? noteHeight(measureW - 16.0f, notReported) : 0.0f);
        const float detailH =
            (sel != nullptr)
                ? (8.0f + uiH + 8.0f + detailRowH * 3.0f + 6.0f + detailNotesH + 8.0f)
                : (8.0f + uiH + 4.0f +
                   uiF->CalcTextSizeA(tinyPx, FLT_MAX, measureW - 16.0f, noSelPrompt).y +
                   8.0f);
        const float passH =
            8.0f + tinyH + 6.0f + noteHeight(measureW - 16.0f, passNote) + 8.0f;
        const float footH = tinyH + 4.0f;
        const float rowH = std::max(20.0f, smallH + 8.0f);

        // THE ROWS AND THE TWO CARDS ARE ONE SCROLLING COLUMN, and that is
        // what makes this survive a small window rather than merely look
        // right in a large one.
        //
        // The first arrangement gave the rows the remainder and drew the cards
        // beneath them at absolute positions. It has two failure modes and
        // both were reproduced: eight rows in a tall panel left two thirds of
        // the register empty with the target's figures on the floor - the dead
        // space this window was redrawn to remove - and a SHORT window pushed
        // the passes card straight through the bottom of the well and over the
        // footnote. One column fixes both. The cards follow the last row
        // immediately whatever the row count, slack falls at the bottom where
        // it costs nothing, and a window too short for all of it scrolls
        // instead of overlapping.
        const float rowsContent =
            rows.empty() ? rowH * 2.0f : static_cast<float>(rows.size()) * rowH;
        const float contentNeed = rowsContent + 8.0f + detailH + 8.0f + passH;
        float listH = br.y - kPad - footH - 6.0f - y;
        if (listH < rowH * 3.0f) { listH = rowH * 3.0f; }

        // THE HEAD AND THE ROWS SHARE ONE WIDTH, and it is the narrower one
        // whenever the column scrolls. A scrollbar takes its pixels out of the
        // rows and not out of the heading drawn above them, so a head laid out
        // at the full width sits a scrollbar's worth to the right of the
        // column it names - and the last column, AGE, loses its figures under
        // the bar. Knowing whether it will scroll BEFORE the head is drawn is
        // the only way the two can agree.
        const bool listScroll = contentNeed > listH;
        const float tableW =
            innerW - (listScroll ? ImGui::GetStyle().ScrollbarSize : 0.0f);
        const float cardW = tableW;
        const float noradW =
            std::max(textW(lgF, tinyPx, "NORAD"), textW(uiF, tinyPx, "00000")) + 6.0f;
        const float altW =
            std::max(textW(lgF, tinyPx, altHead), textW(uiF, tinyPx, "000000 ft")) + 6.0f;
        const float ageW =
            std::max(textW(lgF, tinyPx, "AGE"), textW(uiF, tinyPx, "00.0 min")) + 6.0f;
        const float dotW = 16.0f;
        const float callW = std::max(30.0f, tableW - dotW - noradW - altW - ageW - 12.0f);

        // The column head, on its own strip of glass.
        dl->AddRectFilled(ImVec2(innerL, y), ImVec2(innerR, y + tinyH + 8.0f), theme::kWell);
        {
            float cx = innerL + 6.0f;
            dl->AddText(lgF, tinyPx, ImVec2(cx, y + 4.0f), theme::kInkFaint, "CALLSIGN");
            cx = innerL + dotW + callW + 6.0f;
            dl->AddText(lgF, tinyPx,
                        ImVec2(cx + noradW - textW(lgF, tinyPx, "NORAD") - 6.0f, y + 4.0f),
                        theme::kInkFaint, "NORAD");
            cx += noradW;
            dl->AddText(lgF, tinyPx,
                        ImVec2(cx + altW - textW(lgF, tinyPx, altHead) - 6.0f, y + 4.0f),
                        theme::kInkFaint, altHead);
            cx += altW;
            dl->AddText(lgF, tinyPx,
                        ImVec2(cx + ageW - textW(lgF, tinyPx, "AGE") - 6.0f, y + 4.0f),
                        theme::kInkFaint, "AGE");
        }
        y += tinyH + 8.0f;

        // --- the rows -------------------------------------------------------
        // NO WINDOW PADDING AND NO ITEM SPACING ON THE CHILD, and both matter
        // for the same reason: the list is sized from `rowH` and every pixel
        // ImGui adds on its own is a pixel the arithmetic does not know about.
        // Padding put each row right of the heading it belongs under; spacing
        // made every row taller than rowH, so a list measured to hold eight
        // rows exactly held six and grew a scrollbar - which then ate the AGE
        // column. With both at zero the row pitch IS rowH, and the hairline at
        // its foot is the divider.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGui::SetCursorScreenPos(ImVec2(innerL, y));
        if (ImGui::BeginChild("##satlist", ImVec2(innerW, listH), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoBackground)) {
            ImDrawList* rdl = ImGui::GetWindowDrawList();
            if (rows.empty()) {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                rdl->AddText(uiF, tinyPx, ImVec2(at.x + 6.0f, at.y + 4.0f), theme::kInkFaint,
                             "No targets. Decoded and propagated tracks appear here.",
                             nullptr, tableW - 12.0f);
                // Reserved so the cards below start where the rows would have
                // ended, rather than riding up under the column head.
                ImGui::Dummy(ImVec2(tableW, rowH * 2.0f));
            }
            for (std::size_t i = 0; i < rows.size(); ++i) {
                const TrackRow& r = rows[i];
                const cascade::core::HostTrack& ht = tracks[r.source];
                const cascade::core::TrackPresentation pres =
                    cascade::core::trackPresentation(ht.t.ageMs, ht.t.kind);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                ImGui::PushID(static_cast<int>(i));
                const bool clicked = ImGui::InvisibleButton("##row", ImVec2(tableW, rowH));
                const bool hovered = ImGui::IsItemHovered();
                const bool dbl = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                ImGui::PopID();
                if (clicked) { selectedId_ = r.id; }
                if (dbl) {
                    // GO TO IT WITHOUT ZOOMING. Centring is what a user asks
                    // for by double-clicking a target; tightening the span as
                    // the flight list does would slam a whole-globe satellite
                    // view down to a couple of degrees and lose every other
                    // target on the map.
                    selectedId_ = r.id;
                    goTo(ht.t.latDeg, ht.t.lonDeg, spanDeg_);
                }
                const bool selected = (selectedId_ == r.id);
                if (selected) {
                    rdl->AddRectFilled(at, ImVec2(at.x + tableW, at.y + rowH),
                                       theme::kBrassDark);
                    rdl->AddRectFilled(at, ImVec2(at.x + 2.0f, at.y + rowH), theme::kAlarm);
                } else if (hovered) {
                    rdl->AddRectFilled(at, ImVec2(at.x + tableW, at.y + rowH),
                                       theme::withAlpha(theme::kBrassDark, 0.45f));
                }
                rdl->AddLine(ImVec2(at.x, at.y + rowH - 1.0f),
                             ImVec2(at.x + tableW, at.y + rowH - 1.0f),
                             theme::withAlpha(theme::kVoid, 0.55f));

                // The dot is the marker's own colour, so a row and the thing
                // it names cannot be a different colour from each other.
                const ImU32 dot = fadedColour(colourForTrack(ht.t), pres.alpha);
                rdl->AddCircleFilled(ImVec2(at.x + 9.0f, at.y + rowH * 0.5f), 4.5f, dot);

                const float ty = at.y + (rowH - smallH) * 0.5f;
                rdl->PushClipRect(ImVec2(at.x + dotW, at.y),
                                  ImVec2(at.x + dotW + callW, at.y + rowH), true);
                rdl->AddText(uiF, tinyPx, ImVec2(at.x + dotW, ty),
                             selected ? theme::kIvory : theme::kCream, r.label.c_str());
                rdl->PopClipRect();

                float cx = at.x + dotW + callW + 6.0f;
                // The identifier is figures, so it takes the counter face.
                ImFont* idF = faceForValue(r.id.c_str());
                rdl->AddText(idF, tinyPx,
                             ImVec2(cx + noradW - textW(idF, tinyPx, r.id.c_str()) - 6.0f, ty),
                             theme::kInkMuted, r.id.c_str());
                cx += noradW;

                char altText[24];
                if (!std::isfinite(r.altM)) {
                    std::snprintf(altText, sizeof altText, "--");
                } else if (allOrbital) {
                    std::snprintf(altText, sizeof altText, "%.0f", r.altM / 1000.0);
                } else if (orbitalLadder(ht.t.kind)) {
                    std::snprintf(altText, sizeof altText, "%.0f km", r.altM / 1000.0);
                } else {
                    std::snprintf(altText, sizeof altText, "%.0f ft", r.altM * 3.28084);
                }
                ImFont* altF = faceForValue(altText);
                rdl->AddText(altF, tinyPx,
                             ImVec2(cx + altW - textW(altF, tinyPx, altText) - 6.0f, ty),
                             std::isfinite(r.altM) ? theme::kAmber : theme::kInkFaint,
                             altText);
                cx += altW;

                char ageText[24];
                formatAge(r.ageMs, ageText, sizeof ageText);
                // A FADED ROW IS A TARGET THE MAP IS ABOUT TO DROP, and its
                // age is the reason. Fresh is phosphor - a fix the source
                // stands behind this instant; stale is the amber that says
                // look at this.
                const ImU32 ageCol = (pres.alpha < 1.0f) ? theme::kGold : theme::kPhosphor;
                rdl->AddText(uiF, tinyPx,
                             ImVec2(cx + ageW - textW(uiF, tinyPx, ageText) - 6.0f, ty),
                             ageCol, ageText);
            }

        // --- the selected target's card, immediately under the last row -----
        // Reserved with a Dummy so the column's scroll extent counts it, then
        // drawn into the CHILD's list at the position that Dummy took.
        ImGui::Dummy(ImVec2(tableW, 8.0f));
        {
            const ImVec2 cTL = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(tableW, detailH));
            const ImVec2 cBR(cTL.x + cardW, cTL.y + detailH);
            rdl->AddRectFilled(cTL, cBR, theme::kWell, theme::kKeyRounding);
            rdl->AddRect(cTL, cBR, theme::withAlpha(theme::kBrassDark, 0.9f),
                         theme::kKeyRounding, 0, theme::kHairline);
            rdl->PushClipRect(ImVec2(cTL.x + 1.0f, cTL.y + 1.0f),
                              ImVec2(cBR.x - 1.0f, cBR.y - 1.0f), true);
            float cy = cTL.y + 8.0f;
            if (sel == nullptr) {
                rdl->AddText(uiF, uiPx, ImVec2(cTL.x + 8.0f, cy), theme::kInkFaint,
                            "NO TARGET SELECTED");
                cy += uiH + 4.0f;
                // The SAME sentence the height above was measured from, and it
                // is a named constant precisely so the two cannot drift.
                rdl->AddText(uiF, tinyPx, ImVec2(cTL.x + 8.0f, cy), theme::kInkFaint,
                            noSelPrompt, nullptr, cardW - 16.0f);
            } else {
                const char* name =
                    (sel->t.label[0] != '\0') ? sel->t.label : sel->t.id;
                rdl->AddText(uiF, uiPx, ImVec2(cTL.x + 8.0f, cy), theme::kIvory, name);
                char idLine[48];
                std::snprintf(idLine, sizeof idLine, "%s %s",
                              orbitalLadder(sel->t.kind) ? "NORAD" : "ID", sel->t.id);
                rdl->AddText(uiF, tinyPx,
                            ImVec2(cBR.x - 8.0f - textW(uiF, tinyPx, idLine),
                                   cy + uiH - smallH),
                            theme::kInkMuted, idLine);
                cy += uiH + 8.0f;

                // Six cells in two columns: what the source reports, then what
                // this application computes from it and the receiver position.
                struct Cell {
                    const char* key;
                    char value[40];
                    // False draws the HATCH. It carries both kinds of blank -
                    // "the source does not report this" and "this needs a
                    // receiver position" - because on the card they look the
                    // same; the note underneath is what tells them apart, and
                    // it is chosen from whether a position is set.
                    bool known;
                };
                Cell cells[6];
                cells[0].value[0] = '\0';
                cells[0].key = "ALTITUDE";
                cells[0].known = std::isfinite(sel->t.altM);
                if (cells[0].known) {
                    if (orbitalLadder(sel->t.kind)) {
                        std::snprintf(cells[0].value, sizeof cells[0].value, "%.0f km",
                                      sel->t.altM / 1000.0);
                    } else {
                        std::snprintf(cells[0].value, sizeof cells[0].value, "%.0f ft",
                                      sel->t.altM * 3.28084);
                    }
                }
                cells[1].key = "SUB-POINT";
                cells[1].known = true;
                std::snprintf(cells[1].value, sizeof cells[1].value, "%.2f %c  %.2f %c",
                              std::fabs(sel->t.latDeg), sel->t.latDeg >= 0.0 ? 'N' : 'S',
                              std::fabs(sel->t.lonDeg), sel->t.lonDeg >= 0.0 ? 'E' : 'W');
                cells[2].key = "VELOCITY";
                cells[2].known = std::isfinite(sel->t.speedMps);
                cells[2].value[0] = '\0';
                if (cells[2].known) {
                    // km/s for an orbital speed, because 7660 m/s is a figure
                    // nobody recognises and 7.66 km/s is the one every source
                    // on the subject prints.
                    if (orbitalLadder(sel->t.kind)) {
                        std::snprintf(cells[2].value, sizeof cells[2].value, "%.2f km/s",
                                      sel->t.speedMps / 1000.0);
                    } else {
                        std::snprintf(cells[2].value, sizeof cells[2].value, "%.0f kt",
                                      sel->t.speedMps * 1.943844);
                    }
                }
                cells[3].key = "FIX AGE";
                cells[3].known = true;
                formatAge(sel->t.ageMs, cells[3].value, sizeof cells[3].value);
                cells[4].key = "DISTANCE";
                cells[4].value[0] = '\0';
                cells[4].known = false;
                cells[5].key = "BEARING";
                cells[5].value[0] = '\0';
                cells[5].known = false;
                if (hasHome_) {
                    const double km = greatCircleKm(homeLat_, homeLon_, sel->t.latDeg,
                                                    sel->t.lonDeg);
                    const double brg = initialBearingDeg(homeLat_, homeLon_, sel->t.latDeg,
                                                         sel->t.lonDeg);
                    if (std::isfinite(km)) {
                        std::snprintf(cells[4].value, sizeof cells[4].value, "%.0f km", km);
                        cells[4].known = true;
                    }
                    // NaN for a target directly overhead, which is a real
                    // answer: there is no direction to it. Hatched rather than
                    // printed as 000, which a user would read as north.
                    if (std::isfinite(brg)) {
                        std::snprintf(cells[5].value, sizeof cells[5].value, "%.0f deg",
                                      brg);
                        cells[5].known = true;
                    }
                }

                const float colW = (cardW - 16.0f) * 0.5f;
                for (int i = 0; i < 6; ++i) {
                    const float px2 = cTL.x + 8.0f + colW * static_cast<float>(i % 2);
                    const float py = cy + detailRowH * static_cast<float>(i / 2);
                    rdl->AddText(lgF, tinyPx, ImVec2(px2, py), theme::kInkFaint,
                                cells[i].key);
                    const float vy = py + smallH + 1.0f;
                    if (cells[i].known) {
                        ImFont* vf = faceForValue(cells[i].value);
                        rdl->AddText(vf, tinyPx, ImVec2(px2, vy), theme::kAmber,
                                    cells[i].value);
                    } else {
                        // HATCHED, NOT ZEROED. The two blanks here are not the
                        // same fact: DISTANCE and BEARING are hatched because
                        // no receiver position is set and the note below says
                        // so, while an altitude or a speed the source does not
                        // report is hatched because that source does not
                        // report it. Neither is ever drawn as a number.
                        addHatch(rdl, ImVec2(px2, vy),
                                 ImVec2(px2 + colW - 14.0f, vy + faceH(uiF, tinyPx)));
                    }
                }
                cy += detailRowH * 3.0f + 6.0f;

                // THE RECOVERABLE ONE FIRST, and only while it is true: it is
                // the one with a way out, it is drawn in gold because it is a
                // prompt to act, and it goes at the top because acting on it is
                // the next thing the user can do. Asked of the same
                // satelliteCardNotes the height was reserved from.
                if (cardNotes.noReceiver) {
                    drawNote(rdl, ImVec2(cTL.x + 8.0f, cy), cardW - 16.0f, theme::kGold,
                             noRxNote);
                    cy += noteHeight(cardW - 16.0f, noRxNote) + 6.0f;
                }
                // THE SECOND KIND OF MISSING, ALWAYS, and it does not look like
                // the first. No amount of clicking produces an inclination: the
                // ABI a track source reports through has no field for one, so
                // this is a statement of fact rather than a blocked control
                // with a way out - muted rather than gold, and present whether
                // or not a receiver position has been set. Read from the same
                // answer as the reservation, which inside this arm is simply
                // true - written out so a future change to one has to change
                // the other.
                if (cardNotes.notReported) {
                    drawNote(rdl, ImVec2(cTL.x + 8.0f, cy), cardW - 16.0f, theme::kInkMuted,
                             notReported);
                }
            }
            rdl->PopClipRect();
        }

        // --- next passes, and why the list is empty -------------------------
        ImGui::Dummy(ImVec2(tableW, 8.0f));
        {
            const ImVec2 cTL = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(tableW, passH));
            const ImVec2 cBR(cTL.x + cardW, cTL.y + passH);
            rdl->AddRectFilled(cTL, cBR, theme::kWell, theme::kKeyRounding);
            rdl->AddRect(cTL, cBR, theme::withAlpha(theme::kBrassDark, 0.9f),
                        theme::kKeyRounding, 0, theme::kHairline);
            rdl->PushClipRect(ImVec2(cTL.x + 1.0f, cTL.y + 1.0f),
                             ImVec2(cBR.x - 1.0f, cBR.y - 1.0f), true);
            addBenchGroupCaption(rdl, ImVec2(cTL.x + 8.0f, cTL.y + 8.0f), cardW - 16.0f,
                                 "NEXT PASSES - THIS TARGET");
            drawNote(rdl, ImVec2(cTL.x + 8.0f, cTL.y + 8.0f + tinyH + 6.0f), cardW - 16.0f,
                     theme::kInkMuted, passNote);
            rdl->PopClipRect();
        }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);

        // The one cue on this window that is not self-explanatory, on the
        // floor of the register where a maker's plate goes.
        dl->AddText(lgF, tinyPx, ImVec2(innerL, br.y - kPad - tinyH), theme::kInkFaint,
                    "HATCHED VALUE - CANNOT BE COMPUTED");
        dl->PopClipRect();
    }

    // ---- the map, in its glass ---------------------------------------------
    {
        const ImVec2 gTL(bodyTL.x + regW + kGap, bodyTL.y);
        const ImVec2 gBR(gTL.x + mapW, gTL.y + bodyH);
        if (mapW > 60.0f) {
            // The chart sits in a brass surround with an ink lip, which is
            // what the design has and what stops the map reading as a hole in
            // the panel.
            dl->AddRectFilled(gTL, gBR, theme::kBrassMid, theme::kPanelRounding);
            addBenchBevel(dl, gTL, gBR, theme::kPanelRounding, true);
            const ImVec2 mTL(gTL.x + 8.0f, gTL.y + 8.0f);
            const ImVec2 mBR(gBR.x - 8.0f, gBR.y - 8.0f);
            dl->AddRect(ImVec2(mTL.x - 3.0f, mTL.y - 3.0f), ImVec2(mBR.x + 3.0f, mBR.y + 3.0f),
                        theme::kEnamel, 0.0f, 0, 5.0f);

            // EQUIRECTANGULAR, AND NO TILES. See MapProjection: the poles are
            // on screen and a ground track reads as the sinusoid it is, at the
            // price of a Mercator raster this page therefore cannot draw.
            setProjection(MapProjection::Equirectangular);
            setTrailOptions(deck.groundTracks, deck.altitudeColours);
            setTrailStyle(deck.trailStyle);
            // Borrowed for the call, and null when the overlay is off - which
            // is how the map is told to skip it. The accumulator keeps filling
            // either way, which is why the deck's note can still report it.
            setCoverage((deck.coverage && hasHome_) ? coverage : nullptr);

            ImGui::SetCursorScreenPos(mTL);
            draw(mBR.x - mTL.x, mBR.y - mTL.y, tracks, paths, nullptr, info);

            // --- the chart's own captions, over the map it describes --------
            dl->PushClipRect(mTL, mBR, true);
            const auto overlay = [&](const ImVec2& at, const char* const* lines, int count,
                                     bool rightAlign) {
                float w = 0.0f;
                for (int i = 0; i < count; ++i) {
                    w = std::max(w, textW(lgF, tinyPx, lines[i]));
                }
                const float h = static_cast<float>(count) * (tinyH + 3.0f) + 9.0f;
                const ImVec2 tl(rightAlign ? at.x - w - 20.0f : at.x, at.y);
                const ImVec2 br(tl.x + w + 20.0f, tl.y + h);
                dl->AddRectFilled(tl, br, theme::withAlpha(theme::kWell, 0.92f), 2.0f);
                dl->AddRect(tl, br, theme::withAlpha(theme::kBrassDark, 0.85f), 2.0f);
                for (int i = 0; i < count; ++i) {
                    dl->AddText(lgF, tinyPx,
                                ImVec2(tl.x + 10.0f,
                                       tl.y + 5.0f + static_cast<float>(i) * (tinyH + 3.0f)),
                                theme::kCream, lines[i]);
                }
                return h;
            };
            // WHAT IS ACTUALLY DRAWN, named. The projection is the one this
            // page forced; the geography is the compiled-in Natural Earth
            // outline, because an equirectangular page draws no tiles. The
            // design's second line also carried the age of the element set,
            // which nothing here measures - so it is not there.
            const char* chart[2] = {"EQUIRECTANGULAR - WGS 84",
                                    "NATURAL EARTH 1:110m COASTLINE"};
            overlay(ImVec2(mTL.x + 10.0f, mTL.y + 10.0f), chart, 2, false);

            // The two marks the altitude legend cannot explain, and only when
            // they are on screen: a key to something not drawn is clutter.
            bool anyDashed = false;
            for (const auto& p : paths) {
                if ((p.flags & CASCADE_PATH_FLAG_DASHED) != 0u) {
                    anyDashed = true;
                    break;
                }
            }
            const char* keyLines[2];
            int keyCount = 0;
            if (!selectedId_.empty()) { keyLines[keyCount++] = "RINGED MARK - SELECTED"; }
            if (deck.groundTracks && !paths.empty()) {
                keyLines[keyCount++] =
                    anyDashed ? "DASHED LINE - PREDICTED TRACK" : "LINE - GROUND TRACK";
            }
            if (keyCount > 0) {
                float kh = static_cast<float>(keyCount) * (tinyH + 3.0f) + 9.0f;
                overlay(ImVec2(mBR.x - 10.0f, mBR.y - 10.0f - kh), keyLines, keyCount, true);
            }
            dl->PopClipRect();
        }
    }

    // The cursor is left under the panel, and an EMPTY ITEM is submitted after
    // moving it. Everything above is positioned absolutely, which ImGui treats
    // as extending the window's boundaries by hand - and it says so, loudly,
    // in its error hook unless an item follows to close the extent off.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + avail.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopID();
}

}  // namespace cascade::gui
