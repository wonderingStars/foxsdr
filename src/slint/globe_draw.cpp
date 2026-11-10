// globe_draw.cpp - see globe_draw.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "globe_draw.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace foxsdr::globe {

namespace {

std::uint64_t nextRevision() {
    static std::uint64_t counter = 0;
    return ++counter;
}

std::uint32_t mix(std::uint32_t a, std::uint32_t b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    auto ch = [&](int shift) {
        const double x = static_cast<double>((a >> shift) & 0xFFu), y = static_cast<double>((b >> shift) & 0xFFu);
        return static_cast<std::uint32_t>(std::lround(x + (y - x) * t)) & 0xFFu;
    };
    return (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

std::uint32_t withAlpha(std::uint32_t argb, std::uint32_t alpha) { return (argb & 0x00FFFFFFu) | (alpha << 24); }

DrawLine lineOf(const Polyline& p, std::uint32_t argb, float w, bool dashed = false, bool closed = false) {
    DrawLine l;
    l.points = p;
    l.argb = argb;
    l.widthPx = w;
    l.dashed = dashed;
    l.closed = closed;
    return l;
}

void addGraticule(GlobeDrawList& list, double stepDeg, double radius) {
    const Palette& pal = palette();
    for (const Polyline& l : graticule(stepDeg, 3.0, radius)) {
        // the equator and the Greenwich meridian a little stronger (the SATVIEW spec)
        bool strong = false;
        double minLat = 91, maxLat = -91, lon0 = 0.0;
        for (const Vec3& p : l) {
            const LatLon q = fromVec(p);
            minLat = std::min(minLat, q.lat);
            maxLat = std::max(maxLat, q.lat);
        }
        if (!l.empty()) { lon0 = fromVec(l[l.size() / 2]).lon; }
        if (std::fabs(minLat) < 1e-6 && std::fabs(maxLat) < 1e-6) { strong = true; }
        if (maxLat - minLat > 179.0 && std::fabs(lon0) < 1e-6) { strong = true; }
        list.lines.push_back(lineOf(l, strong ? pal.equator : pal.graticule, strong ? 1.2f : 0.8f));
    }
}

Vec3 at(const LatLon& p, double radius = 1.0) { return toUnit(p) * radius; }

}  // namespace

const Palette& palette() {
    static const Palette p;
    return p;
}

double altitudeLift(double altKm) {
    if (!(altKm > 0.0) || !std::isfinite(altKm)) { return 0.0; }
    return std::min(altKm / kEarthRadiusKm, 0.25);
}

GlobeCamera initialCamera(ViewKind k, bool homeKnown, const LatLon& home) {
    GlobeCamera c = defaultCamera(k);
    if (k != ViewKind::Goes && k != ViewKind::SolarSystem && homeKnown) {
        c.centreLonDeg = home.lon;
        c.centreLatDeg = std::clamp(home.lat, -85.0, 85.0);
    }
    return c;
}

// ---- the surface ----

std::uint64_t earthBakeKey(const SunPosition* sun, bool greyline, int width, int height) {
    std::uint64_t k = 1469598103934665603ull;
    auto put = [&](std::int64_t v) { k = (k ^ static_cast<std::uint64_t>(v)) * 1099511628211ull; };
    put(width);
    put(height);
    put(greyline ? 1 : 0);
    if (sun != nullptr) {
        put(std::llround(sun->subLonDeg * 4.0));
        put(std::llround(sun->subLatDeg * 4.0));
        put(2);
    }
    return k;
}

void bakeEarth(Texture& out, const LandMask& land, const SunPosition* sun, bool greyline, int width, int height) {
    width = std::clamp(width, 16, 4096);
    height = std::clamp(height, 8, 2048);
    const Palette& pal = palette();
    out.width = width;
    out.height = height;
    out.argb.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0u);
    out.key = earthBakeKey(sun, greyline, width, height);
    out.revision = nextRevision();
    Vec3 s{0, 0, 1};
    if (sun != nullptr) { s = toUnit(sun->subLatDeg, sun->subLonDeg); }
    std::vector<double> cl(static_cast<std::size_t>(width)), sl(static_cast<std::size_t>(width));
    for (int c = 0; c < width; ++c) {
        const double lon = -180.0 + (c + 0.5) * 360.0 / width;
        cl[static_cast<std::size_t>(c)] = std::cos(lon * kDegToRad);
        sl[static_cast<std::size_t>(c)] = std::sin(lon * kDegToRad);
    }
    for (int r = 0; r < height; ++r) {
        const double lat = 90.0 - (r + 0.5) * 180.0 / height;
        const double cphi = std::cos(lat * kDegToRad), sphi = std::sin(lat * kDegToRad);
        for (int c = 0; c < width; ++c) {
            const double lon = -180.0 + (c + 0.5) * 360.0 / width;
            const bool isLand = land.land(lat, lon);
            double day = 1.0, elev = 90.0;
            if (sun != nullptr) {
                const double sinE = sphi * s.z + cphi * (cl[static_cast<std::size_t>(c)] * s.x + sl[static_cast<std::size_t>(c)] * s.y);
                day = daylight(sinE, 6.0);
                elev = std::asin(std::clamp(sinE, -1.0, 1.0)) * kRadToDeg;
            }
            std::uint32_t colour = mix(isLand ? pal.landNight : pal.oceanNight, isLand ? pal.landDay : pal.oceanDay, day);
            if (greyline && sun != nullptr && std::fabs(elev) < 6.0) { colour = mix(colour, pal.greyline, 0.30 * (1.0 - std::fabs(elev) / 6.0)); }
            out.argb[static_cast<std::size_t>(r) * static_cast<std::size_t>(width) + static_cast<std::size_t>(c)] = colour;
        }
    }
}

void bakeGoes(Texture& out, const std::vector<std::uint8_t>& pixels, int w, int h, bool rgb, double satLonDeg, int width, int height) {
    width = std::clamp(width, 16, 4096);
    height = std::clamp(height, 8, 2048);
    const Palette& pal = palette();
    out.width = width;
    out.height = height;
    out.argb.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), pal.oceanDay);
    out.key = 0;
    out.revision = nextRevision();
    const std::size_t bpp = rgb ? 3u : 1u;
    if (w <= 0 || h <= 0 || pixels.size() < static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * bpp) { return; }
    for (int r = 0; r < height; ++r) {
        const double lat = 90.0 - (r + 0.5) * 180.0 / height;
        for (int c = 0; c < width; ++c) {
            const double lon = -180.0 + (c + 0.5) * 360.0 / width;
            if (std::fabs(normalizeLon(lon - satLonDeg)) > 85.0) { continue; }
            double u = 0.0, v = 0.0;
            if (!goesPicturePoint(LatLon{lat, lon}, satLonDeg, u, v)) { continue; }
            const int px = std::clamp(static_cast<int>(u * w), 0, w - 1);
            const int py = std::clamp(static_cast<int>(v * h), 0, h - 1);
            const std::uint8_t* p = &pixels[(static_cast<std::size_t>(py) * static_cast<std::size_t>(w) + static_cast<std::size_t>(px)) * bpp];
            const std::uint32_t rr = p[0], gg = rgb ? p[1] : p[0], bb = rgb ? p[2] : p[0];
            if (rr == 0 && gg == 0 && bb == 0) { continue; }   // nothing received there
            const std::uint32_t pic = 0xFF000000u | (rr << 16) | (gg << 8) | bb;
            out.argb[static_cast<std::size_t>(r) * static_cast<std::size_t>(width) + static_cast<std::size_t>(c)] = mix(pal.oceanDay, pic, 0.92);
        }
    }
}

// ---- 1. propagation ----

GlobeDrawList drawPropagation(const PropagationScene& scene, const ViewState& view, const Texture& earth) {
    GlobeDrawList l;
    const Palette& pal = palette();
    l.texture = earth;
    l.camera = view.cameraSet ? view.camera : initialCamera(ViewKind::Propagation, scene.homeKnown, scene.home);
    addGraticule(l, 30.0, 1.002);
    if (!scene.homeKnown) {
        l.emptyNote = "Set your locator in the plugin's settings, or the receiver position, to draw the paths.";
        return l;
    }
    for (const GlobePath& p : scene.paths) {
        l.lines.push_back(lineOf(skywaveArc(p.from, p.to, p.hops, 300.0, 6.0, 24), p.argb, 2.0f));
    }
    for (const GlobeMarker& m : scene.markers) {
        DrawMarker d;
        d.at = at(m.at, 1.004);
        d.argb = m.argb;
        d.shape = m.shape == 1 ? MarkerShape::Triangle : (m.shape == 2 ? MarkerShape::Square : MarkerShape::Dot);
        d.sizePx = m.shape == 1 ? 8.0f : 5.0f;
        d.label = m.label;
        l.markers.push_back(std::move(d));
    }
    (void)pal;
    return l;
}

// ---- 2. FT8 ----

GlobeDrawList drawFt8(const std::vector<Ft8Spot>& spots, bool rxKnown, const LatLon& rx, const ViewState& view, const Texture& earth) {
    GlobeDrawList l;
    l.texture = earth;
    l.camera = view.cameraSet ? view.camera : initialCamera(ViewKind::Ft8, rxKnown, rx);
    addGraticule(l, 30.0, 1.002);
    if (!rxKnown) { l.emptyNote = "Set the receiver position (or the plugin's locator) to draw the paths."; }
    const Ft8Summary sum = summariseSpots(spots);
    for (const Ft8Spot& s : spots) {
        const std::uint32_t colour = bandColour(s.bandId);
        if (rxKnown) {
            const double lift = 0.02 + 0.10 * std::min(1.0, s.km / 20000.0);
            l.lines.push_back(lineOf(greatCircleArc(rx, s.at, 28, lift), withAlpha(colour, 0xD0u), 1.6f));
        }
        DrawMarker d;
        d.at = at(s.at, 1.004);
        d.argb = colour;
        d.sizePx = 4.0f;
        if (sum.hasFurthest && s.call == sum.furthest.call && s.bandId == sum.furthest.bandId) { d.label = s.call; d.sizePx = 6.0f; }
        l.markers.push_back(std::move(d));
    }
    if (rxKnown) {
        DrawMarker d;
        d.at = at(rx, 1.004);
        d.shape = MarkerShape::Triangle;
        d.argb = palette().white;
        d.sizePx = 8.0f;
        d.label = "RX";
        l.markers.push_back(std::move(d));
    }
    if (spots.empty() && rxKnown) { l.emptyNote = "No stations heard yet in this time window."; }
    return l;
}

// ---- 3. GOES ----

GlobeDrawList drawGoes(const ViewState& view, double satLonDeg, bool rxKnown, const LatLon& rx, const Texture& surface) {
    GlobeDrawList l;
    const Palette& pal = palette();
    l.texture = surface;
    l.camera = view.cameraSet ? view.camera : GlobeCamera{satLonDeg, 0.0, 1.0};
    addGraticule(l, 15.0, 1.002);
    const LatLon sub{0.0, satLonDeg};
    l.lines.push_back(lineOf(footprintCircle(sub, kGoesAltitudeKm, 0.0, 180, 1.006), pal.limb, 1.6f, true, true));
    DrawMarker s;
    s.at = at(sub, 1.006);
    s.shape = MarkerShape::Square;
    s.argb = 0xFFF5A524u;
    s.sizePx = 8.0f;
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "SUB-SAT %.1f %c %.1f %c", std::fabs(sub.lat), sub.lat >= 0 ? 'N' : 'S', std::fabs(sub.lon), sub.lon >= 0 ? 'E' : 'W');
        s.label = buf;
    }
    l.markers.push_back(std::move(s));
    if (rxKnown) {
        DrawMarker d;
        d.at = at(rx, 1.006);
        d.shape = MarkerShape::Triangle;
        d.argb = 0xFFFF6B9Du;
        d.sizePx = 8.0f;
        d.label = "RX";
        l.markers.push_back(std::move(d));
    }
    return l;
}

// ---- 4. the solar system ----

GlobeDrawList drawOrbits(const OrbitScene& scene, const ViewState& view) {
    GlobeDrawList l;
    const Palette& pal = palette();
    l.space = GlobeDrawList::Space::Orbits;
    l.sphere = false;
    l.camera = GlobeCamera{view.spinDeg, view.tiltDeg, view.camera.zoom};
    if (!view.cameraSet) { l.camera.zoom = 1.0; }
    for (std::size_t i = 0; i < scene.rings.size(); ++i) {
        Polyline pl;
        for (const solar::Xyz& q : scene.rings[i]) { pl.push_back(Vec3{q.x, q.y, q.z}); }
        DrawLine d = lineOf(pl, i + 1 == scene.rings.size() ? 0x99aab4c8u : pal.orbitRing, 1.0f, false, true);
        d.surface = false;
        l.lines.push_back(std::move(d));
    }
    DrawMarker sun;
    sun.at = Vec3{0, 0, 0};
    sun.argb = pal.sun;
    sun.sizePx = 11.0f;
    sun.hiddenBehind = false;
    l.markers.push_back(std::move(sun));
    for (const BodyView& b : scene.bodies) {
        DrawMarker d;
        d.at = Vec3{b.pos.x, b.pos.y, b.pos.z};
        d.argb = b.argb;
        d.sizePx = static_cast<float>(b.radiusPx);
        d.label = b.name;
        d.hiddenBehind = false;
        l.markers.push_back(std::move(d));
    }
    DrawMarker earth;
    earth.at = Vec3{scene.earth.x, scene.earth.y, scene.earth.z};
    earth.argb = 0xFF4DA3FFu;
    earth.sizePx = 5.0f;
    earth.label = "Earth";
    earth.hiddenBehind = false;
    l.markers.push_back(std::move(earth));
    if (scene.showMoon) {
        DrawMarker m;
        m.at = Vec3{scene.moonDisplay.x, scene.moonDisplay.y, scene.moonDisplay.z};
        m.argb = 0xFFDDE3EEu;
        m.sizePx = 3.0f;
        m.label = "Moon";
        m.hiddenBehind = false;
        l.markers.push_back(std::move(m));
        // the ring about the Earth the Moon is carried on, not to scale
        Polyline ring;
        for (int k = 0; k <= 48; ++k) {
            const double a = 2.0 * kPi * (k == 48 ? 0 : k) / 48.0;
            ring.push_back(Vec3{scene.earth.x + 0.05 * std::cos(a), scene.earth.y + 0.05 * std::sin(a), scene.earth.z});
        }
        DrawLine d = lineOf(ring, 0x66dde3eeu, 0.8f, true, true);
        d.surface = false;
        l.lines.push_back(std::move(d));
    }
    return l;
}

// ---- 5. satellites ----

GlobeDrawList drawSatellites(const SatSceneIn& in, const ViewState& view, const Texture& earth) {
    GlobeDrawList l;
    l.texture = earth;
    l.camera = view.cameraSet ? view.camera : initialCamera(ViewKind::Satellites, in.rxKnown, in.rx);
    addGraticule(l, 30.0, 1.002);
    if (in.sats.empty() && in.paths.empty()) { l.emptyNote = "No satellites are being tracked yet."; }
    if (in.groundTracks) {
        for (const SatPathIn& p : in.paths) {
            Polyline pl;
            for (const LatLon& q : p.points) { pl.push_back(at(q, 1.003)); }
            if (pl.size() >= 2) { l.lines.push_back(lineOf(pl, 0xFFF5A524u, 1.4f, p.dashed, p.closed)); }
        }
    }
    const SatIn* selected = nullptr;
    for (const SatIn& s : in.sats) {
        if (s.id == in.selectedId) { selected = &s; }
    }
    for (const SatIn& s : in.sats) {
        DrawMarker d;
        d.at = at(s.at, 1.0 + (s.hasAlt ? altitudeLift(s.altKm) : 0.0) + 0.004);
        d.shape = MarkerShape::Square;
        d.argb = s.hasAlt ? orbitClassColour(s.altKm) : palette().white;
        d.sizePx = &s == selected ? 8.0f : 5.0f;
        d.label = s.label.empty() ? s.id : s.label;
        l.markers.push_back(std::move(d));
    }
    if (in.footprints && selected != nullptr && selected->hasAlt) {
        l.lines.push_back(lineOf(footprintCircle(selected->at, selected->altKm, 0.0, 120, 1.004), 0xFF6FD3FFu, 1.6f, false, true));
    }
    if (in.rxKnown) {
        DrawMarker d;
        d.at = at(in.rx, 1.004);
        d.shape = MarkerShape::Triangle;
        d.argb = 0xFFFF6B9Du;
        d.sizePx = 8.0f;
        d.label = "RX";
        l.markers.push_back(std::move(d));
        if (selected != nullptr && selected->hasAlt) {
            Polyline slant{at(in.rx, 1.004), at(selected->at, 1.0 + altitudeLift(selected->altKm) + 0.004)};
            l.lines.push_back(lineOf(slant, palette().white, 1.0f, true));
        }
    }
    return l;
}

}  // namespace foxsdr::globe
