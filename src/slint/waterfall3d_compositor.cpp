// waterfall3d_compositor.cpp - see waterfall3d_compositor.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "waterfall3d_compositor.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "waterfall3d_logic.hpp"

namespace foxsdr::slintview3d {

using scene3d::Camera;
using scene3d::Dash;
using scene3d::Rgba;
using scene3d::Vec3;

namespace {

constexpr Rgba kVfo{245, 165, 36, 255};            // the accent amber
constexpr Rgba kMarker{241, 234, 214, 255};        // the markers' ivory
constexpr std::uint32_t kTickInk = 0xA9B0BFFF;     // axis tick words
constexpr std::uint32_t kCaptionInk = 0xC8CEDAFF;  // axis captions
constexpr std::uint32_t kVfoInk = 0xF5A524FF;
constexpr std::uint32_t kMarkerInk = 0xF1EAD6FF;

}  // namespace

Camera waterfall3dDefaultCamera() {
    Camera c;
    c.azimuthDeg = -58.0f;
    c.elevationDeg = 32.0f;
    c.zoom = 1.0f;
    c.target = {0.0f, 0.0f, kWf3dBoxZ * 0.35f};
    c.sceneRadius = 1.3f;
    return c;
}

const scene3d::Ramp& waterfall3dRamp(int shading) {
    static const scene3d::Ramp height = scene3d::Ramp::blueHeight();
    static const scene3d::Ramp ramp = scene3d::Ramp::heat();
    return shading == 1 ? ramp : height;
}

Wf3dResult paintWaterfall3d(scene3d::Scene& scene, Rgba* px, int w, int h, const Wf3dInput& in) {
    Wf3dResult res;
    Camera cam = in.camera;   // zoom, azimuth and elevation are the user's; the framing is the view's
    {
        const Camera d = waterfall3dDefaultCamera();
        cam.target = d.target;
        cam.sceneRadius = d.sceneRadius;
    }
    scene.begin(px, w, h, Rgba{0, 0, 0, 0}, cam);

    scene3d::BoxSpec box;
    box.lo = {-kWf3dBoxX, -kWf3dBoxY, 0.0f};
    box.hi = {kWf3dBoxX, kWf3dBoxY, kWf3dBoxZ};
    box.xTicks = 5;
    box.yTicks = 5;
    box.zTicks = 5;
    const scene3d::BoxLayout lay = scene3d::drawBoxBack(scene, box);

    // ---- the surface ----
    std::size_t realRows = 0;
    if (in.grid != nullptr && in.rows >= 2 && in.cols >= 2) {
        for (int r = 0; r < in.rows; ++r) {
            if (std::isfinite(in.grid[static_cast<std::size_t>(r) * static_cast<std::size_t>(in.cols)])) { ++realRows; }
        }
        res.hasData = realRows > 0;
        scene3d::HeightFieldSpec spec;
        spec.cols = in.cols;
        spec.rows = in.rows;
        spec.values = in.grid;
        spec.origin = {-kWf3dBoxX, -kWf3dBoxY, 0.0f};
        spec.sx = 2.0f * kWf3dBoxX;
        spec.sy = 2.0f * kWf3dBoxY;
        spec.zScale = kWf3dBoxZ;
        spec.lo = static_cast<float>(in.dbMin);
        spec.hi = static_cast<float>(std::max(in.dbMin + 1.0, in.dbMax));
        spec.ramp = &waterfall3dRamp(in.shading);
        const scene3d::Mesh mesh = scene3d::buildHeightField(spec);
        scene.setLight({-0.35f, -0.55f, 0.75f}, 0.55f);
        res.triangles = scene.drawMesh(mesh, true);

        // ---- the VFO and the markers: lines along the surface at their frequencies ----
        const auto surfaceZ = [&](double xFrac, int r) {
            const int c = std::clamp(static_cast<int>(std::lround(xFrac * static_cast<double>(in.cols - 1))), 0, in.cols - 1);
            const float v = in.grid[static_cast<std::size_t>(r) * static_cast<std::size_t>(in.cols) + static_cast<std::size_t>(c)];
            if (!std::isfinite(v)) { return std::numeric_limits<float>::quiet_NaN(); }
            const float n = std::clamp((v - spec.lo) / (spec.hi - spec.lo), 0.0f, 1.0f);
            return n * kWf3dBoxZ;
        };
        const auto lineAlong = [&](double hz, Rgba colour, float width, Dash dash) {
            if (!(in.spanHz > 0.0)) { return false; }
            const double f = (hz - (in.centreHz - in.spanHz / 2.0)) / in.spanHz;
            if (f < 0.0 || f > 1.0) { return false; }
            const float x = -kWf3dBoxX + 2.0f * kWf3dBoxX * static_cast<float>(f);
            // on the floor, the whole length of the age axis
            scene.line({x, -kWf3dBoxY, 0.0f}, {x, kWf3dBoxY, 0.0f}, scene3d::withAlpha(colour, 110), 1.0f, dash);
            // and on the surface, row by row, where there is data
            std::vector<Vec3> pts;
            for (int r = 0; r < in.rows; ++r) {
                const float z = surfaceZ(f, r);
                if (!std::isfinite(z)) { if (pts.size() > 1) { scene.polyline(pts, {}, colour, width, dash); } pts.clear(); continue; }
                const float y = -kWf3dBoxY + 2.0f * kWf3dBoxY * static_cast<float>(r) / static_cast<float>(in.rows - 1);
                pts.push_back({x, y, z + 0.012f});
            }
            if (pts.size() > 1) { scene.polyline(pts, {}, colour, width, dash); }
            return true;
        };
        for (const Wf3dMarker& m : in.markers) { lineAlong(m.hz, kMarker, 1.4f, Dash::Dashed); }
        if (in.showVfo) { lineAlong(in.tunedHz, kVfo, 2.2f, Dash::Solid); }
    }
    scene3d::drawBoxFront(scene, box);

    // ---- the words: the ticks and captions are of the box, whatever the data is ----
    if (in.spanHz > 0.0) {
        const double fStep = in.spanHz / static_cast<double>(box.xTicks - 1);
        const double aStep = in.depthS / static_cast<double>(box.yTicks - 1);
        const double dStep = (in.dbMax - in.dbMin) / static_cast<double>(box.zTicks - 1);
        for (const scene3d::AxisTick& t : lay.ticks) {
            std::string text;
            if (t.axis == 0) { text = formatMHz(in.centreHz - in.spanHz / 2.0 + static_cast<double>(t.frac) * in.spanHz, fStep); }
            else if (t.axis == 1) { text = formatAge(static_cast<double>(t.frac) * in.depthS, aStep); }
            else { text = formatDb(in.dbMin + static_cast<double>(t.frac) * (in.dbMax - in.dbMin)); }
            scene3d::addLabel(scene, res.labels, t.labelPos, text, kTickInk, t.axis == 2 ? 1 : 0, 0, false);
        }
        scene3d::addLabel(scene, res.labels, lay.axisTitle[0], in.captionFreq, kCaptionInk, 0, 1, false);
        scene3d::addLabel(scene, res.labels, lay.axisTitle[1], in.captionAge, kCaptionInk, 0, 1, false);
        scene3d::addLabel(scene, res.labels, lay.axisTitle[2], in.captionDb, kCaptionInk, 1, 1, false);
    }
    // the VFO's frequency at the near end of its line, and each marker's tab at the far end of its line, above the box
    const auto fracOfHz = [&](double hz) { return in.spanHz > 0.0 ? (hz - (in.centreHz - in.spanHz / 2.0)) / in.spanHz : -1.0; };
    if (res.hasData && in.showVfo) {
        const double f = fracOfHz(in.tunedHz);
        if (f >= 0.0 && f <= 1.0) {
            const float x = -kWf3dBoxX + 2.0f * kWf3dBoxX * static_cast<float>(f);
            scene3d::addLabel(scene, res.labels, {x, -kWf3dBoxY * 1.1f, 0.0f}, formatMHz(in.tunedHz, 1000.0), kVfoInk, 0, 2, false);
        }
    }
    if (res.hasData) {
        for (const Wf3dMarker& m : in.markers) {
            const double f = fracOfHz(m.hz);
            if (f < 0.0 || f > 1.0) { continue; }
            const float x = -kWf3dBoxX + 2.0f * kWf3dBoxX * static_cast<float>(f);
            scene3d::addLabel(scene, res.labels, {x, kWf3dBoxY, kWf3dBoxZ * 1.05f}, m.label, kMarkerInk, 0, 2, false);
        }
    }
    return res;
}

}  // namespace foxsdr::slintview3d
