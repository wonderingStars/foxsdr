// radar3d_compositor.cpp - see radar3d_compositor.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "radar3d_compositor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace foxsdr::slintview3d {

using scene3d::Camera;
using scene3d::Marker;
using scene3d::Rgba;
using scene3d::Vec3;

namespace {

constexpr Rgba kRing{54, 150, 170, 120};
constexpr Rgba kRim{70, 180, 200, 170};
constexpr Rgba kSpoke{50, 110, 130, 70};
constexpr Rgba kAntenna{255, 120, 225, 255};
constexpr Rgba kNoAlt{150, 160, 176, 255};
constexpr Rgba kEmergency{255, 82, 82, 255};
constexpr Rgba kSelected{245, 246, 250, 255};
constexpr std::uint32_t kTickInk = 0xA9B0BFFF, kCaptionInk = 0xC8CEDAFF;
constexpr float kVectorLen = 0.09f;

std::uint32_t packed(Rgba c) { return (static_cast<std::uint32_t>(c.r) << 24) | (static_cast<std::uint32_t>(c.g) << 16) | (static_cast<std::uint32_t>(c.b) << 8) | c.a; }

Rgba colourOfAlt(double altFt) { return radar3dRamp().at(static_cast<float>(std::clamp(altFt / 100.0 / kRadar3dMaxFl, 0.0, 1.0))); }

}  // namespace

double radar3dBoxHeight(int scaleIndex) { return kRadar3dHeightAtX1 * static_cast<double>(scaleFactorOfIndex(scaleIndex)); }

Camera radar3dDefaultCamera() {
    Camera c;
    c.azimuthDeg = -40.0f;
    c.elevationDeg = 26.0f;
    c.zoom = 1.0f;
    return c;
}

const scene3d::Ramp& radar3dRamp() {
    static const scene3d::Ramp r = scene3d::Ramp::altitude();
    return r;
}

Radar3dResult paintRadar3d(scene3d::Scene& scene, Rgba* px, int w, int h, const Radar3dInput& in) {
    Radar3dResult res;
    const float zH = static_cast<float>(radar3dBoxHeight(in.scaleIndex));
    const int factor = scaleFactorOfIndex(in.scaleIndex);
    Camera cam = in.camera;
    cam.target = {0.0f, 0.0f, zH * 0.35f};
    cam.sceneRadius = 0.5f * std::sqrt(8.0f + zH * zH) * 1.22f;   // the box and the words around it: the corners and the tick labels must stay inside a square well
    scene.begin(px, w, h, Rgba{0, 0, 0, 0}, cam);
    const double range = std::max(1.0, in.rangeNm);

    scene3d::BoxSpec box;
    box.lo = {-1.0f, -1.0f, 0.0f};
    box.hi = {1.0f, 1.0f, zH};
    box.xTicks = 5;
    box.yTicks = 5;
    box.zTicks = 5;
    box.floorGrid = Rgba{80, 90, 110, 40};
    const scene3d::BoxLayout lay = scene3d::drawBoxBack(scene, box);

    // ---- the ground: four range rings, twelve bearing spokes, the antenna ----
    for (int k = 1; k <= 4; ++k) {
        std::vector<Vec3> ring;
        const float r = static_cast<float>(k) / 4.0f;
        for (int i = 0; i <= 72; ++i) {
            const float a = 2.0f * scene3d::kPi * static_cast<float>(i) / 72.0f;
            ring.push_back({r * std::sin(a), r * std::cos(a), 0.0f});
        }
        scene.polyline(ring, {}, k == 4 ? kRim : kRing, k == 4 ? 1.6f : 1.0f);
    }
    for (int b = 0; b < 360; b += 30) {
        const float a = static_cast<float>(b) * scene3d::kPi / 180.0f;
        scene.line({0.0f, 0.0f, 0.0f}, {std::sin(a), std::cos(a), 0.0f}, kSpoke, 1.0f);
    }
    scene.marker({0.0f, 0.0f, 0.0f}, Marker::Cross, kAntenna, 13.0f);

    // ---- the aircraft ----
    struct Placed {
        Vec3 p;
        Rgba c;
    };
    std::vector<Placed> placed(in.aircraft.size());
    for (std::size_t i = 0; i < in.aircraft.size(); ++i) {
        const Radar3dAircraft& a = in.aircraft[i];
        float x = static_cast<float>(a.eastNm / range), y = static_cast<float>(a.northNm / range);
        const float rr = std::hypot(x, y);
        if (rr > 1.0f) { x /= rr; y /= rr; }   // just outside the rim (the range gate is on the scope's side): it sits on the rim
        const bool known = std::isfinite(a.altFt);
        const float z = static_cast<float>(sceneHeight(a.altFt, factor));
        const Rgba colour = known ? colourOfAlt(a.altFt) : kNoAlt;
        placed[i] = {{x, y, z}, colour};
        // the trail first: through the air at its altitudes, each point in the colour of its own level
        if (in.trails != 0 && a.trail.size() > 1) {
            std::vector<Vec3> pts;
            std::vector<Rgba> cols;
            float lastZ = 0.0f;
            for (const Radar3dTrailPoint& t : a.trail) {
                const float tx = static_cast<float>(t.eastNm / range), ty = static_cast<float>(t.northNm / range);
                if (std::hypot(tx, ty) > 1.05f) { continue; }
                const bool tk = std::isfinite(t.altFt);
                if (tk) { lastZ = static_cast<float>(sceneHeight(t.altFt, factor)); }
                pts.push_back({tx, ty, lastZ});
                cols.push_back(scene3d::withAlpha(tk ? colourOfAlt(t.altFt) : kNoAlt, 170));
            }
            if (pts.size() > 1) {
                scene.polyline(pts, cols, colour, in.trails == 2 ? 2.6f : 1.3f);
                res.trailSegments += pts.size() - 1;
            }
        }
        // the drop line and its shadow on the ground
        if (z > 0.002f) {
            scene.line({x, y, z}, {x, y, 0.0f}, scene3d::withAlpha(colour, 90), 1.0f);
            scene.point({x, y, 0.0f}, scene3d::withAlpha(colour, 120), 2.0f);
        }
        // the vector along the course
        if (std::isfinite(a.courseDeg)) {
            const float c = static_cast<float>(a.courseDeg) * scene3d::kPi / 180.0f;
            scene.line({x, y, z}, {x + std::sin(c) * kVectorLen, y + std::cos(c) * kVectorLen, z}, colour, 2.0f);
        }
    }
    for (std::size_t i = 0; i < in.aircraft.size(); ++i) {
        const Radar3dAircraft& a = in.aircraft[i];
        const Placed& pl = placed[i];
        if (a.selected) { scene.point(pl.p, kSelected, 7.0f); }
        if (a.emergency) { scene.point(pl.p, kEmergency, 8.5f); }
        scene.marker(pl.p, std::isfinite(a.altFt) ? Marker::Dot : Marker::Diamond, pl.c, 8.0f);
        ++res.aircraft;
        if (!std::isfinite(a.altFt)) { ++res.onGround; }
    }
    scene3d::drawBoxFront(scene, box);

    // ---- the words ----
    for (const scene3d::AxisTick& t : lay.ticks) {
        char b[32];
        if (t.axis < 2) { std::snprintf(b, sizeof(b), "%.0f", -range + 2.0 * range * static_cast<double>(t.frac)); }
        else { std::snprintf(b, sizeof(b), "%.0f", kRadar3dMaxFl * static_cast<double>(t.frac)); }
        scene3d::addLabel(scene, res.labels, t.labelPos, b, kTickInk, t.axis == 2 ? 1 : 0, 0, false);
    }
    scene3d::addLabel(scene, res.labels, lay.axisTitle[0], in.captionEast, kCaptionInk, 0, 1, false);
    scene3d::addLabel(scene, res.labels, lay.axisTitle[1], in.captionNorth, kCaptionInk, 0, 1, false);
    scene3d::addLabel(scene, res.labels, lay.axisTitle[2], in.captionLevel, kCaptionInk, 1, 1, false);
    for (const std::size_t i : labelOrder(in.aircraft, in.maxLabels)) {
        const Radar3dAircraft& a = in.aircraft[i];
        const std::string text = (a.label.empty() ? a.id : a.label) + " " + flightLevelText(a.altFt, in.noAlt);
        if (scene3d::addLabel(scene, res.labels, placed[i].p, text, packed(placed[i].c), 1, 2, false)) { res.labels.back().x += 9.0f / static_cast<float>(std::max(1, w)); }
    }
    return res;
}

}  // namespace foxsdr::slintview3d
