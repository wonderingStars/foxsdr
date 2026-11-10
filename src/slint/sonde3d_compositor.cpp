// sonde3d_compositor.cpp - see sonde3d_compositor.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "sonde3d_compositor.hpp"

#include <algorithm>
#include <cmath>

#include "sonde3d_logic.hpp"

namespace foxsdr::slintview3d {

using scene3d::Camera;
using scene3d::Dash;
using scene3d::Marker;
using scene3d::Rgba;
using scene3d::Vec3;

namespace {

constexpr Rgba kGround{86, 176, 204, 210};
constexpr Rgba kDescent{255, 105, 150, 255};
constexpr Rgba kRx{245, 165, 36, 255};
constexpr Rgba kStar{255, 255, 255, 255};
constexpr std::uint32_t kTickInk = 0xA9B0BFFF, kCaptionInk = 0xC8CEDAFF, kTagInk = 0xF5F7FAFF, kBurstInk = 0xFFFFFFFF;

std::string kmText(double v) { return formatKm(v); }

}  // namespace

Camera sonde3dDefaultCamera() {
    Camera c;
    c.azimuthDeg = -48.0f;
    c.elevationDeg = 22.0f;
    c.zoom = 1.0f;
    c.target = {kSondeBoxX * 0.5f, kSondeBoxY * 0.5f, kSondeBoxZ * 0.4f};
    c.sceneRadius = 1.75f;
    return c;
}

const scene3d::Ramp& sonde3dRamp() {
    static const scene3d::Ramp r = scene3d::Ramp::viridis();
    return r;
}

SondeResult paintSonde3d(scene3d::Scene& scene, Rgba* px, int w, int h, const SondeInput& in) {
    SondeResult res;
    Camera cam = in.camera;
    {
        const Camera d = sonde3dDefaultCamera();
        cam.target = d.target;
        cam.sceneRadius = d.sceneRadius;
    }
    scene.begin(px, w, h, Rgba{0, 0, 0, 0}, cam);
    res.hasTrack = !in.track.empty();
    res.burst = in.burst && res.hasTrack;

    // ---- the ranges: everything that is drawn, padded, at least 5 km across each way; the altitude axis to the ladder's 35 km or the next 5 km above the highest sample ----
    double eLo = 1e30, eHi = -1e30, nLo = 1e30, nHi = -1e30, maxAlt = 0.0;
    const auto take = [&](double e, double n) { eLo = std::min(eLo, e); eHi = std::max(eHi, e); nLo = std::min(nLo, n); nHi = std::max(nHi, n); };
    for (const SondeLocal& s : in.track) { take(s.eastKm, s.northKm); if (std::isfinite(s.altKm)) { maxAlt = std::max(maxAlt, s.altKm); } }
    for (const auto& g : in.ground) { take(g.first, g.second); }
    if (in.haveRx) { take(in.rxEastKm, in.rxNorthKm); }
    if (eLo > eHi) { eLo = -10.0; eHi = 10.0; nLo = -10.0; nHi = 10.0; }
    const auto pad = [](double& lo, double& hi) {
        const double span = std::max(5.0, hi - lo);
        const double mid = 0.5 * (lo + hi);
        lo = mid - span * 0.55;
        hi = mid + span * 0.55;
    };
    pad(eLo, eHi);
    pad(nLo, nHi);
    res.eastLoKm = eLo;
    res.eastHiKm = eHi;
    res.northLoKm = nLo;
    res.northHiKm = nHi;
    res.topKm = std::max(kSondeLadderTopKm, std::ceil(maxAlt / 5.0) * 5.0);

    const auto X = [&](double e) { return kSondeBoxX * static_cast<float>((e - eLo) / (eHi - eLo)); };
    const auto Y = [&](double n) { return kSondeBoxY * static_cast<float>((n - nLo) / (nHi - nLo)); };
    const auto Z = [&](double alt) { return std::isfinite(alt) ? kSondeBoxZ * static_cast<float>(std::clamp(alt / res.topKm, 0.0, 1.0)) : 0.0f; };

    scene3d::BoxSpec box;
    box.lo = {0.0f, 0.0f, 0.0f};
    box.hi = {kSondeBoxX, kSondeBoxY, kSondeBoxZ};
    box.xTicks = 5;
    box.yTicks = 5;
    box.zTicks = std::clamp(static_cast<int>(std::lround(res.topKm / 5.0)) + 1, 2, 12);   // a tick every 5 km: 0, 5, 10 ... 35
    const scene3d::BoxLayout lay = scene3d::drawBoxBack(scene, box);

    // ---- the ground track: the plugin's trail, else the track's own footprint ----
    {
        std::vector<Vec3> pts;
        if (!in.ground.empty()) { for (const auto& g : in.ground) { pts.push_back({X(g.first), Y(g.second), 0.0f}); } }
        else { for (const SondeLocal& s : in.track) { pts.push_back({X(s.eastKm), Y(s.northKm), 0.0f}); } }
        if (pts.size() > 1) {
            scene.polyline(pts, {}, kGround, 1.6f);
            res.groundSegments = pts.size() - 1;
        }
    }
    // ---- the receiver ----
    if (in.haveRx) { scene.marker({X(in.rxEastKm), Y(in.rxNorthKm), 0.0f}, Marker::TriangleUp, kRx, 13.0f); }

    // ---- the track: up in the altitude's colours, down dashed pink ----
    const std::size_t split = res.burst ? std::min(in.burstIndex, in.track.size() - 1) : in.track.size() - 1;
    {
        std::vector<Vec3> pts;
        std::vector<Rgba> cols;
        for (std::size_t i = 0; i <= split && i < in.track.size(); ++i) {
            const SondeLocal& s = in.track[i];
            pts.push_back({X(s.eastKm), Y(s.northKm), Z(s.altKm)});
            cols.push_back(sonde3dRamp().at(std::isfinite(s.altKm) ? static_cast<float>(std::clamp(s.altKm / kSondeLadderTopKm, 0.0, 1.0)) : 0.0f));
        }
        if (pts.size() > 1) {
            scene.polyline(pts, cols, cols.back(), 3.4f);
            res.ascentSegments = pts.size() - 1;
        }
    }
    if (res.burst && split + 1 < in.track.size()) {
        std::vector<Vec3> pts;
        for (std::size_t i = split; i < in.track.size(); ++i) { pts.push_back({X(in.track[i].eastKm), Y(in.track[i].northKm), Z(in.track[i].altKm)}); }
        if (pts.size() > 1) {
            scene.polyline(pts, {}, kDescent, 2.2f, Dash::Dashed);
            res.descentSegments = pts.size() - 1;
        }
    }
    // ---- the markers: the burst, the balloon's last position with its drop line, the end of a descent ----
    Vec3 burstAt{}, nowAt{};
    if (res.hasTrack) {
        const SondeLocal& last = in.track.back();
        nowAt = {X(last.eastKm), Y(last.northKm), Z(last.altKm)};
        if (nowAt.z > 0.003f) { scene.line(nowAt, {nowAt.x, nowAt.y, 0.0f}, scene3d::withAlpha(kGround, 120), 1.0f, Dash::Dotted); }
        if (res.burst) {
            const SondeLocal& b = in.track[split];
            burstAt = {X(b.eastKm), Y(b.northKm), Z(b.altKm)};
            scene.marker(burstAt, Marker::Star, kStar, 15.0f);
            scene.marker(nowAt, Marker::TriangleDown, kDescent, 12.0f);
        } else {
            scene.marker(nowAt, Marker::Dot, sonde3dRamp().at(std::isfinite(last.altKm) ? static_cast<float>(std::clamp(last.altKm / kSondeLadderTopKm, 0.0, 1.0)) : 0.0f), 9.0f);
        }
    }
    scene3d::drawBoxFront(scene, box);

    // ---- the words ----
    for (const scene3d::AxisTick& t : lay.ticks) {
        std::string text;
        if (t.axis == 0) { text = kmText(eLo + static_cast<double>(t.frac) * (eHi - eLo)); }
        else if (t.axis == 1) { text = kmText(nLo + static_cast<double>(t.frac) * (nHi - nLo)); }
        else { text = kmText(res.topKm * static_cast<double>(t.frac)); }
        scene3d::addLabel(scene, res.labels, t.labelPos, text, kTickInk, t.axis == 2 ? 1 : 0, 0, false);
    }
    scene3d::addLabel(scene, res.labels, lay.axisTitle[0], in.captionEast, kCaptionInk, 0, 1, false);
    scene3d::addLabel(scene, res.labels, lay.axisTitle[1], in.captionNorth, kCaptionInk, 0, 1, false);
    scene3d::addLabel(scene, res.labels, lay.axisTitle[2], in.captionAlt, kCaptionInk, 1, 1, false);
    if (res.hasTrack && !in.tag.empty()) {
        if (scene3d::addLabel(scene, res.labels, nowAt, in.tag, kTagInk, 1, 2, false)) { res.labels.back().x += 10.0f / static_cast<float>(std::max(1, w)); }
    }
    if (res.burst && !in.burstLabel.empty()) {
        if (scene3d::addLabel(scene, res.labels, burstAt, in.burstLabel, kBurstInk, 1, 2, false)) { res.labels.back().x += 10.0f / static_cast<float>(std::max(1, w)); }
    }
    return res;
}

}  // namespace foxsdr::slintview3d
