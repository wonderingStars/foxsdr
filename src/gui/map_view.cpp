// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/map_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "gui/basemap_cache.hpp"
#include "gui/coastline_data.hpp"
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
ImU32 colourForTrack(const CascadeTrack& t) {
    if ((t.flags & CASCADE_TRACK_FLAG_EMERGENCY) != 0u) {
        return IM_COL32(255, 45, 45, 255);
    }
    const int band = altitudeBandIndex(t.altM);
    if (band < 0) { return colourFor(t.kind); }
    const AltBandStyle& s = altBandStyle(band);
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
    if (filled) {
        dl->AddConcavePolyFilled(pts, n, col);
    } else {
        dl->AddPolyline(pts, n, col, ImDrawFlags_Closed, 1.5f);
    }
}

// The private great-circle and bearing helpers that used to live here are gone
// too: gui/track_metrics.hpp now holds the tested pair, and this view's hover
// readout, the track table's columns and the coverage accumulator all measure
// with the same arithmetic instead of with three copies of it. The old local
// bearing also answered 000 for a target directly overhead, where the tested
// one says explicitly that there is no such direction.

// Graticule step that keeps roughly 4-10 lines across the view at any zoom.
double graticuleStep(double spanDeg) {
    static const double steps[] = {0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 45};
    for (double s : steps) {
        if (spanDeg / s <= 10.0) { return s; }
    }
    return 45.0;
}

}  // namespace

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
    const bool mercator = tiles != nullptr && tiles->active();

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
            const double need = std::max(maxLon - minLon, (maxLat - minLat) * 2.0);
            spanDeg_ = std::clamp(need * 1.6, 0.5, 360.0);
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
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
                      IM_COL32(16, 20, 26, 255));

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

    const auto toScreen = [&](double lat, double lon) {
        double dx = lon - centreLon_;
        // Take the short way round: a target at +179 with the view at -179 is
        // two degrees away, not three hundred and fifty-eight.
        if (dx > 180.0) { dx -= 360.0; }
        if (dx < -180.0) { dx += 360.0; }
        const double x = dx / lonSpan * width + width * 0.5;
        const double y = mercator
                             ? (mercY(lat) - centreMercY) * pixPerWorld + height * 0.5
                             : (centreLat_ - lat) / latSpan * height + height * 0.5;
        return ImVec2(origin.x + static_cast<float>(x), origin.y + static_cast<float>(y));
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
                                  0.02, 360.0);
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
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        centreLon_ -= static_cast<double>(d.x) / width * lonSpan;
        // Same projection rule as the zoom: under tiles a degree of latitude
        // is not a constant number of pixels, and the linear version left the
        // map sliding slower than the cursor vertically.
        if (mercator) {
            centreLat_ = mercLat(mercY(centreLat_) - static_cast<double>(d.y) / pixPerWorld);
        } else {
            centreLat_ += static_cast<double>(d.y) / height * latSpan;
        }
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
                const ImVec2 a = toScreen(north, west);
                const ImVec2 b = toScreen(south, east);
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
    if (!haveTiles) {
        const ImU32 landCol = IM_COL32(96, 116, 136, 255);
        const double west = centreLon_ - lonSpan * 0.5;
        const double east = centreLon_ + lonSpan * 0.5;
        const double south = centreLat_ - latSpan * 0.5;
        const double north = centreLat_ + latSpan * 0.5;

        for (std::uint32_t r = 0; r < coastline::kRunCount; ++r) {
            const coastline::Run& run = coastline::kRuns[r];
            ImVec2 prev(0.0f, 0.0f);
            bool havePrev = false;
            double prevLon = 0.0;
            for (std::uint32_t i = 0; i < run.count; ++i) {
                const std::size_t k = (static_cast<std::size_t>(run.first) + i) * 2u;
                const double lon = static_cast<double>(coastline::kCoords[k]) / 100.0;
                const double lat = static_cast<double>(coastline::kCoords[k + 1]) / 100.0;
                const ImVec2 s = toScreen(lat, lon);
                if (havePrev) {
                    // Same antimeridian rule as the ground tracks: a segment
                    // that jumps more than half the world is a wrap, not a
                    // coast, and drawing it streaks a line across the map.
                    const bool wrap = std::fabs(lon - prevLon) > 180.0;
                    // Cheap reject of segments entirely outside the view. With
                    // 5127 points this is not about frame rate so much as
                    // about not asking ImGui to clip thousands of lines that
                    // cannot be seen.
                    const bool visible =
                        !(std::max(lon, prevLon) < west || std::min(lon, prevLon) > east ||
                          lat < south - latSpan || lat > north + latSpan);
                    if (!wrap && visible) { dl->AddLine(prev, s, landCol, 1.0f); }
                }
                prev = s;
                prevLon = lon;
                havePrev = true;
            }
        }
    }

    // --- graticule --------------------------------------------------------
    const double step = graticuleStep(spanDeg_);
    const ImU32 gridCol = IM_COL32(60, 70, 84, 255);
    const double lat0 = std::floor((centreLat_ - latSpan * 0.5) / step) * step;
    for (double lat = lat0; lat <= centreLat_ + latSpan * 0.5; lat += step) {
        const ImVec2 a = toScreen(lat, centreLon_ - lonSpan * 0.5);
        const ImVec2 b = toScreen(lat, centreLon_ + lonSpan * 0.5);
        dl->AddLine(a, b, gridCol);
        char buf[24];
        std::snprintf(buf, sizeof buf, "%.4g", lat);
        dl->AddText(ImVec2(origin.x + 3.0f, a.y - 14.0f), IM_COL32(120, 132, 148, 255), buf);
    }
    const double lon0 = std::floor((centreLon_ - lonSpan * 0.5) / step) * step;
    for (double lon = lon0; lon <= centreLon_ + lonSpan * 0.5; lon += step) {
        const ImVec2 a = toScreen(centreLat_ + latSpan * 0.5, lon);
        const ImVec2 b = toScreen(centreLat_ - latSpan * 0.5, lon);
        dl->AddLine(a, b, gridCol);
        char buf[24];
        std::snprintf(buf, sizeof buf, "%.4g", lon);
        dl->AddText(ImVec2(a.x + 3.0f, origin.y + height - 16.0f),
                    IM_COL32(120, 132, 148, 255), buf);
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
            dl->AddCircle(h, px, IM_COL32(70, 90, 110, 200), 64);
            char buf[24];
            std::snprintf(buf, sizeof buf, "%.0f km", r);
            dl->AddText(ImVec2(h.x + 3.0f, h.y - px - 14.0f),
                        IM_COL32(90, 110, 132, 255), buf);
        }
        dl->AddCircleFilled(h, 4.0f, IM_COL32(255, 255, 255, 255));
        dl->AddText(ImVec2(h.x + 6.0f, h.y + 2.0f), IM_COL32(220, 220, 220, 255), "RX");
    }

    // --- coverage overlay, over the rings and under the targets ------------
    //
    // The furthest anything has been heard in each five-degree sector, drawn as
    // one closed polygon around the receiver. It is the cheapest antenna
    // diagnostic there is: the nulls in a real pattern are visible as notches,
    // and a mast or a building in one direction shows up as a flat side.
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
        // Faint fill, brighter edge: the fill says where the coverage is and
        // must not hide the map or the targets inside it, and the edge is the
        // line the eye actually reads the shape from.
        // CONCAVE, not convex. A real coverage lobe has notches in it - that is
        // the whole reason to look at one - and AddConvexPolyFilled on a
        // concave outline fills the triangle fan rather than the shape,
        // painting over exactly the nulls the picture exists to show.
        dl->AddConcavePolyFilled(pts, CoverageMap::kBuckets, IM_COL32(80, 170, 255, 34));
        dl->AddPolyline(pts, CoverageMap::kBuckets, IM_COL32(110, 200, 255, 190),
                        ImDrawFlags_Closed, 1.5f);
    }

    // --- paths, under the targets ----------------------------------------
    for (const auto& p : paths) {
        if (p.points.size() < 2) { continue; }
        // THE TRAIL OBEYS THE SAME RULE AS ITS OWNER. A path carries no age of
        // its own, so the host looks the owner up; without this the marker
        // disappeared on schedule and the line under it did not, leaving a
        // trail that starts where the missing marker would have been and runs
        // off into empty space - a drawing that says "something is here" about
        // the one target the host has just decided is not.
        const cascade::core::TrackPresentation pres =
            cascade::core::pathPresentation(p, tracks);
        if (!pres.visible) { continue; }
        // THE TRAIL TAKES ITS OWNER'S COLOUR, altitude included, so a climbing
        // aircraft's trail is the same colour as its marker instead of the
        // generic per-kind line that used to sit under a banded marker.
        //
        // ONE COLOUR FOR THE WHOLE TRAIL, not a gradient along it: a
        // CascadePathPoint carries a latitude and a longitude and nothing else
        // (see the ABI), so the altitude at each vertex is simply not in the
        // data. Colouring per segment would mean inventing it.
        //
        // An UNOWNED path - a footprint circle, a predicted track, anything not
        // reporting as a target - has no altitude to take, and falls back to
        // the path's own kind colour exactly as before.
        ImU32 base = colourFor(p.kind);
        for (const auto& ht : tracks) {
            if (ht.plugin == p.plugin && p.id == ht.t.id) {
                base = colourForTrack(ht.t);
                break;
            }
        }
        const ImU32 col =
            fadedColour((base & 0x00FFFFFFu) | (120u << IM_COL32_A_SHIFT), pres.alpha);
        for (std::size_t i = 1; i < p.points.size(); ++i) {
            const double lonA = p.points[i - 1].lonDeg;
            const double lonB = p.points[i].lonDeg;
            // Do not draw the segment that wraps the antimeridian: joining
            // +179 to -179 would streak a line straight across the map, which
            // is what a naive ground-track plot always gets wrong.
            if (std::fabs(lonB - lonA) > 180.0) { continue; }
            dl->AddLine(toScreen(p.points[i - 1].latDeg, lonA),
                        toScreen(p.points[i].latDeg, lonB), col, 1.5f);
        }
    }

    // --- targets ----------------------------------------------------------
    hoveredId_.clear();
    // Whether anything actually drawn has a reported altitude, which is what
    // decides whether the altitude legend below is information or clutter.
    bool anyBanded = false;
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
        const bool altKnown = altitudeBandIndex(ht.t.altM) >= 0;
        if (altKnown) { anyBanded = true; }
        // An emergency also gets a ring of its own, so it survives being
        // selected (where the marker is knocked out of a disc and the emergency
        // hue is no longer the fill) and survives a colour-blind reading.
        if ((ht.t.flags & CASCADE_TRACK_FLAG_EMERGENCY) != 0u) {
            dl->AddCircle(s, 16.0f, fadedColour(IM_COL32(255, 45, 45, 255), pres.alpha), 0,
                          2.0f);
        }

        if (ht.t.kind == CASCADE_TRACK_AIRCRAFT) {
            // The silhouette IS the heading indicator, so no tick.
            if (picked) {
                dl->AddCircleFilled(s, 13.0f, col);
                addPlane(dl, s, ht.t.courseDeg, 8.0f, IM_COL32(16, 20, 26, 255));
            } else {
                addPlane(dl, s, ht.t.courseDeg, 9.0f, col, altKnown);
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
        dl->AddText(ImVec2(lblX, s.y - 6.0f), col, lbl);

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

    if (best != nullptr) {
        hoveredId_ = best->t.id;
        ImGui::BeginTooltip();
        ImGui::Text("%s", best->t.label[0] != '\0' ? best->t.label : best->t.id);
        ImGui::Separator();
        // Who this actually is, when a track-info plugin knows. Asking is
        // non-blocking and cached, so hovering is also what starts the lookup
        // for a target the per-frame sweep has not reached yet.
        if (info != nullptr && info->active()) {
            const TrackInfoCache::Info* d = info->get(best->t.id, best->t.kind);
            if (d != nullptr && d->known) {
                if (!d->registration.empty()) {
                    ImGui::Text("reg     %s", d->registration.c_str());
                }
                const std::string& type =
                    !d->typeName.empty() ? d->typeName : d->typeCode;
                if (!type.empty()) { ImGui::Text("type    %s", type.c_str()); }
                if (!d->operatorName.empty()) {
                    ImGui::Text("oper    %s", d->operatorName.c_str());
                }
                if (!d->country.empty()) {
                    ImGui::Text("reg'd   %s", d->country.c_str());
                }
                ImGui::Separator();
            } else if (d == nullptr) {
                ImGui::TextDisabled("looking up...");
                ImGui::Separator();
            }
        }
        ImGui::Text("id      %s", best->t.id);
        ImGui::Text("from    %s", best->plugin.c_str());
        ImGui::Text("pos     %.5f, %.5f", best->t.latDeg, best->t.lonDeg);
        // Unknown values are NaN by ABI contract, and are shown as unknown
        // rather than as zero - "0 kt" and "no speed reported" are different
        // facts and must not look the same.
        if (!std::isnan(best->t.altM)) {
            // The band is named as well as the number, so the colour on the map
            // and the figure in the tooltip can be tied together without
            // counting swatches in the legend.
            ImGui::Text("alt     %.0f m (%.0f ft, %s)", best->t.altM,
                        best->t.altM * 3.28084,
                        altBandStyle(altitudeBandIndex(best->t.altM)).label);
        } else {
            ImGui::TextDisabled("alt     unknown");
        }
        if (!std::isnan(best->t.speedMps)) {
            ImGui::Text("speed   %.0f kt", best->t.speedMps * 1.94384);
        } else {
            ImGui::TextDisabled("speed   unknown");
        }
        if (!std::isnan(best->t.courseDeg)) {
            ImGui::Text("course  %.0f deg", best->t.courseDeg);
        } else {
            ImGui::TextDisabled("course  unknown");
        }
        if (hasHome_) {
            const double km =
                greatCircleKm(homeLat_, homeLon_, best->t.latDeg, best->t.lonDeg);
            const double brg =
                initialBearingDeg(homeLat_, homeLon_, best->t.latDeg, best->t.lonDeg);
            // A target at the receiver's own coordinates has no bearing from
            // it, and printing "nan deg" would be worse than saying so.
            if (std::isnan(brg)) {
                ImGui::Text("range   %.1f km, bearing undefined", km);
            } else {
                ImGui::Text("range   %.1f km at %.0f deg", km, brg);
            }
        }
        ImGui::Text("age     %.1f s", static_cast<double>(best->t.ageMs) / 1000.0);
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
    if (anyBanded) {
        const float sw = kAltLegendSwatch;
        const float pad = kAltLegendPad;
        const float lh = ImGui::GetTextLineHeight();
        const float rowStep = lh + 2.0f;
        const float boxH = rowStep * static_cast<float>(kAltBandCount) + 8.0f;
        // MEASURED WITH THE FONT IN USE, not assumed. A constant here was 74 px
        // and clipped half the labels against the map's clip rect; the width
        // has to come from the same CalcTextSize that will draw them.
        const float boxW = altLegendWidth(
            [](const char* s) { return ImGui::CalcTextSize(s).x; });
        const ImVec2 tl(origin.x + width - boxW - 8.0f, origin.y + 8.0f);
        dl->AddRectFilled(tl, ImVec2(tl.x + boxW, tl.y + boxH), IM_COL32(16, 20, 26, 190),
                          3.0f);
        dl->AddRect(tl, ImVec2(tl.x + boxW, tl.y + boxH), IM_COL32(70, 82, 98, 200), 3.0f);
        for (int i = 0; i < kAltBandCount; ++i) {
            // Highest band at the TOP, which is the way an altitude scale is
            // read everywhere else.
            const AltBandStyle& s = altBandStyle(kAltBandCount - 1 - i);
            const float y = tl.y + 4.0f + rowStep * static_cast<float>(i);
            // Laid out from the same padding the width was computed from, so
            // the two cannot disagree about where the label starts.
            dl->AddRectFilled(ImVec2(tl.x + pad, y + (lh - sw) * 0.5f),
                              ImVec2(tl.x + pad + sw, y + (lh + sw) * 0.5f),
                              IM_COL32(s.r, s.g, s.b, 255), 2.0f);
            dl->AddText(ImVec2(tl.x + pad + sw + pad, y), IM_COL32(200, 208, 218, 255),
                        s.label);
        }
    }

    // Scale bar: a map with no basemap and no scale is a scatter plot.
    {
        const double kmPerPx = (latSpan * 111.32) / static_cast<double>(height);
        double barKm = 1.0;
        while (barKm / kmPerPx < 60.0) { barKm *= 2.0; }
        const float barPx = static_cast<float>(barKm / kmPerPx);
        const ImVec2 a(origin.x + 12.0f, origin.y + height - 26.0f);
        const ImVec2 b(a.x + barPx, a.y);
        dl->AddLine(a, b, IM_COL32(220, 220, 220, 220), 2.0f);
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.0f km", barKm);
        dl->AddText(ImVec2(a.x, a.y - 15.0f), IM_COL32(220, 220, 220, 220), buf);
    }

    dl->PopClipRect();
}

}  // namespace cascade::gui
