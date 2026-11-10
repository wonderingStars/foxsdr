// survey3d_compositor.cpp - see survey3d_compositor.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "survey3d_compositor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace foxsdr::slintview3d {

using scene3d::Camera;
using scene3d::Rgba;
using scene3d::Vec3;

namespace {

constexpr std::uint32_t kTickInk = 0xA9B0BFFF, kCaptionInk = 0xC8CEDAFF, kRankInk = 0xF5F7FAFF;

}  // namespace

Camera survey3dDefaultCamera() {
    Camera c;
    c.azimuthDeg = -52.0f;
    c.elevationDeg = 36.0f;
    c.zoom = 1.0f;
    c.target = {0.0f, 0.0f, kSurveyBoxZ * 0.3f};
    c.sceneRadius = 1.4f;
    return c;
}

SurveyResult paintSurvey3d(scene3d::Scene& scene, Rgba* px, int w, int h, const SurveyInput& in) {
    SurveyResult res;
    Camera cam = in.camera;
    {
        const Camera d = survey3dDefaultCamera();
        cam.target = d.target;
        cam.sceneRadius = d.sceneRadius;
    }
    scene.begin(px, w, h, Rgba{0, 0, 0, 0}, cam);
    res.hasData = !in.channels.empty();
    res.floorDb = floorDb(in.channels);
    spanOf(in.channels, res.loHz, res.hiHz);
    levelRange(in.channels, res.floorDb, res.loDb, res.hiDb);

    scene3d::BoxSpec box;
    box.lo = {-kSurveyBoxX, -kSurveyBoxY, 0.0f};
    box.hi = {kSurveyBoxX, kSurveyBoxY, kSurveyBoxZ};
    box.xTicks = 5;
    box.yTicks = 5;
    box.zTicks = 5;
    const scene3d::BoxLayout lay = scene3d::drawBoxBack(scene, box);

    const auto xOf = [&](double hz) { return -kSurveyBoxX + 2.0f * kSurveyBoxX * static_cast<float>((hz - res.loHz) / std::max(1.0, res.hiHz - res.loHz)); };
    const auto yOf = [&](double occPct) { return -kSurveyBoxY + 2.0f * kSurveyBoxY * static_cast<float>(std::clamp(occPct, 0.0, 100.0) / 100.0); };
    const auto zOf = [&](double db) { return kSurveyBoxZ * static_cast<float>(std::clamp((db - res.loDb) / std::max(1.0, res.hiDb - res.loDb), 0.0, 1.0)); };

    if (res.hasData) {
        const int cols = std::clamp(w / 5, 80, 240), rows = 28;
        std::vector<float> grid;
        ridgeGrid(in.channels, res.loHz, res.hiHz, res.floorDb, rows, cols, grid);
        static const scene3d::Ramp ramp = scene3d::Ramp::blueHeight();
        scene3d::HeightFieldSpec spec;
        spec.cols = cols;
        spec.rows = rows;
        spec.values = grid.data();
        spec.origin = {-kSurveyBoxX, -kSurveyBoxY, 0.0f};
        spec.sx = 2.0f * kSurveyBoxX;
        spec.sy = 2.0f * kSurveyBoxY;
        spec.zScale = kSurveyBoxZ;
        spec.lo = static_cast<float>(res.loDb);
        spec.hi = static_cast<float>(res.hiDb);
        spec.ramp = &ramp;
        scene.setLight({-0.4f, -0.5f, 0.75f}, 0.5f);
        res.triangles = scene.drawMesh(scene3d::buildHeightField(spec), true);
    }
    scene3d::drawBoxFront(scene, box);

    // the words
    if (res.hasData) {
        const double span = res.hiHz - res.loHz;
        for (const scene3d::AxisTick& t : lay.ticks) {
            std::string text;
            if (t.axis == 0) { text = formatSurveyMHz(res.loHz + static_cast<double>(t.frac) * span, span); }
            else if (t.axis == 1) { char b[16]; std::snprintf(b, sizeof(b), "%.0f", 100.0 * static_cast<double>(t.frac)); text = b; }
            else { char b[16]; std::snprintf(b, sizeof(b), "%.0f", res.loDb + static_cast<double>(t.frac) * (res.hiDb - res.loDb)); text = b; }
            scene3d::addLabel(scene, res.labels, t.labelPos, text, kTickInk, t.axis == 2 ? 1 : 0, 0, false);
        }
        scene3d::addLabel(scene, res.labels, lay.axisTitle[0], in.captionFreq, kCaptionInk, 0, 1, false);
        scene3d::addLabel(scene, res.labels, lay.axisTitle[1], in.captionSeen, kCaptionInk, 0, 1, false);
        scene3d::addLabel(scene, res.labels, lay.axisTitle[2], in.captionDb, kCaptionInk, 1, 1, false);
        // the rank numbers at the ridges' far ends (the Seen share), as high as the ridge
        for (std::size_t i = 0; i < in.channels.size() && i < in.rankLabels; ++i) {
            const SurveyChannel& c = in.channels[i];
            const double peak = c.havePeak ? c.peakDb : (c.haveOver ? res.floorDb + c.overDb : res.floorDb);
            const Vec3 p{xOf(c.freqHz), yOf(c.haveSeen ? c.seenPct : 100.0), zOf(peak) + 0.035f};
            if (scene3d::addLabel(scene, res.labels, p, std::to_string(i + 1), kRankInk, 0, 2, false)) { ++res.ranked; }
        }
    }
    return res;
}

}  // namespace foxsdr::slintview3d
