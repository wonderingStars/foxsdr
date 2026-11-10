// tunnel3d_compositor.cpp - see tunnel3d_compositor.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "tunnel3d_compositor.hpp"

#include <algorithm>
#include <cmath>

#include "tunnel3d_logic.hpp"

namespace foxsdr::slintview3d {

using scene3d::Camera;
using scene3d::Rgba;
using scene3d::Vec3;

namespace {

constexpr float kHalfT = 1.5f, kHalfIQ = 0.5f;
constexpr std::uint32_t kTickInk = 0xA9B0BFFF, kCaptionInk = 0xC8CEDAFF;
constexpr Rgba kRing{90, 105, 135, 42};
constexpr Rgba kCentreLine{150, 160, 180, 70};
constexpr Rgba kInsetFrame{120, 130, 150, 200};
constexpr Rgba kInsetGround{12, 14, 20, 150};

void blendPx(Rgba* px, int w, int h, int x, int y, Rgba c) {
    if (x < 0 || y < 0 || x >= w || y >= h) { return; }
    Rgba& d = px[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
    const float sa = static_cast<float>(c.a) / 255.0f, da = static_cast<float>(d.a) / 255.0f;
    const float oa = sa + da * (1.0f - sa);
    if (oa <= 1e-6f) { return; }
    const auto ch = [&](std::uint8_t s, std::uint8_t dd) { return static_cast<std::uint8_t>(std::clamp((static_cast<float>(s) * sa + static_cast<float>(dd) * da * (1.0f - sa)) / oa + 0.5f, 0.0f, 255.0f)); };
    d = Rgba{ch(c.r, d.r), ch(c.g, d.g), ch(c.b, d.b), static_cast<std::uint8_t>(std::clamp(oa * 255.0f + 0.5f, 0.0f, 255.0f))};
}

Rgba colourOf(const TunnelInput& in, std::size_t k, std::size_t n, double axis) {
    if (in.colourBy == 1) {
        const double r = std::hypot(static_cast<double>(in.i[k]), static_cast<double>(in.q[k])) / axis;
        return tunnelRamp(1).at(static_cast<float>(std::clamp(r, 0.0, 1.0)));
    }
    return tunnelRamp(0).at(n > 1 ? static_cast<float>(k) / static_cast<float>(n - 1) : 1.0f);
}

}  // namespace

Camera tunnelDefaultCamera() {
    Camera c;
    c.azimuthDeg = -35.0f;
    c.elevationDeg = 18.0f;
    c.zoom = 1.0f;
    c.target = {0.0f, 0.0f, 0.0f};
    c.sceneRadius = 1.3f;   // the box is long and flat: fitted to the well's height with room for the words, not to its diagonal
    return c;
}

const scene3d::Ramp& tunnelRamp(int colourBy) {
    static const scene3d::Ramp time = scene3d::Ramp::timeTunnel();
    static const scene3d::Ramp level = scene3d::Ramp::viridis();
    return colourBy == 1 ? level : time;
}

TunnelResult paintTunnel(scene3d::Scene& scene, Rgba* px, int w, int h, const TunnelInput& in) {
    TunnelResult res;
    Camera cam = in.camera;
    {
        const Camera d = tunnelDefaultCamera();
        cam.target = d.target;
        cam.sceneRadius = d.sceneRadius;
    }
    scene.begin(px, w, h, Rgba{0, 0, 0, 0}, cam);
    const std::size_t n = (in.i != nullptr && in.q != nullptr) ? in.n : 0;
    double peak = 0.0;
    for (std::size_t k = 0; k < n; ++k) { peak = std::max({peak, std::fabs(static_cast<double>(in.i[k])), std::fabs(static_cast<double>(in.q[k]))}); }
    res.axis = niceAxis(peak);
    res.spanMs = availableMs(n, in.rateHz);

    scene3d::BoxSpec box;
    box.lo = {-kHalfT, -kHalfIQ, -kHalfIQ};
    box.hi = {kHalfT, kHalfIQ, kHalfIQ};
    box.xTicks = 5;
    box.yTicks = 5;
    box.zTicks = 5;
    box.floorGrid = Rgba{80, 90, 110, 50};
    const scene3d::BoxLayout lay = scene3d::drawBoxBack(scene, box);

    // the tunnel: the cross-section's ring at five times, and the middle line it runs along
    for (int k = 0; k < 5; ++k) {
        const float x = -kHalfT + 2.0f * kHalfT * static_cast<float>(k) / 4.0f;
        std::vector<Vec3> ring;
        for (int a = 0; a <= 48; ++a) {
            const float t = 2.0f * scene3d::kPi * static_cast<float>(a) / 48.0f;
            ring.push_back({x, kHalfIQ * std::cos(t), kHalfIQ * std::sin(t)});
        }
        scene.polyline(ring, {}, kRing, 1.0f);
    }
    scene.line({-kHalfT, 0.0f, 0.0f}, {kHalfT, 0.0f, 0.0f}, kCentreLine, 1.0f, scene3d::Dash::Dashed);

    // the samples: time along, I across (y), Q up (z)
    for (std::size_t k = 0; k < n; ++k) {
        const float x = -kHalfT + 2.0f * kHalfT * (n > 1 ? static_cast<float>(k) / static_cast<float>(n - 1) : 1.0f);
        const float y = std::clamp(in.i[k] / static_cast<float>(res.axis), -1.0f, 1.0f) * kHalfIQ;
        const float z = std::clamp(in.q[k] / static_cast<float>(res.axis), -1.0f, 1.0f) * kHalfIQ;
        scene.point({x, y, z}, colourOf(in, k, n, res.axis), 1.7f);
        ++res.points;
    }
    scene3d::drawBoxFront(scene, box);

    // the words
    for (const scene3d::AxisTick& t : lay.ticks) {
        std::string text;
        if (t.axis == 0) { text = formatMs(res.spanMs * static_cast<double>(t.frac), res.spanMs); }
        else { text = formatAxis((2.0 * static_cast<double>(t.frac) - 1.0) * res.axis, res.axis); }
        scene3d::addLabel(scene, res.labels, t.labelPos, text, kTickInk, t.axis == 2 ? 1 : 0, 0, false);
    }
    scene3d::addLabel(scene, res.labels, lay.axisTitle[0], in.captionTime, kCaptionInk, 0, 1, false);
    scene3d::addLabel(scene, res.labels, lay.axisTitle[1], in.captionI, kCaptionInk, 0, 1, false);
    scene3d::addLabel(scene, res.labels, lay.axisTitle[2], in.captionQ, kCaptionInk, 1, 1, false);

    // the inset: the same samples in the plane, I across and Q up, the newest few hundred (a QPSK's four dots)
    if (in.inset && w >= 220 && h >= 160) {
        const int size = std::clamp(std::min(w, h) * 28 / 100, 72, 150);
        const int x0 = 10, y0 = 10;
        res.insetX = x0;
        res.insetY = y0;
        res.insetSize = size;
        for (int y = 0; y < size; ++y) { for (int x = 0; x < size; ++x) { blendPx(px, w, h, x0 + x, y0 + y, kInsetGround); } }
        for (int k = 0; k < size; ++k) {
            blendPx(px, w, h, x0 + k, y0, kInsetFrame);
            blendPx(px, w, h, x0 + k, y0 + size - 1, kInsetFrame);
            blendPx(px, w, h, x0, y0 + k, kInsetFrame);
            blendPx(px, w, h, x0 + size - 1, y0 + k, kInsetFrame);
            blendPx(px, w, h, x0 + k, y0 + size / 2, Rgba{110, 120, 140, 70});
            blendPx(px, w, h, x0 + size / 2, y0 + k, Rgba{110, 120, 140, 70});
        }
        const std::size_t from = n > 600 ? n - 600 : 0;
        const float half = static_cast<float>(size) * 0.5f - 3.0f;
        for (std::size_t k = from; k < n; ++k) {
            const int cx = x0 + size / 2 + static_cast<int>(std::lround(std::clamp(in.i[k] / static_cast<float>(res.axis), -1.0f, 1.0f) * half));
            const int cy = y0 + size / 2 - static_cast<int>(std::lround(std::clamp(in.q[k] / static_cast<float>(res.axis), -1.0f, 1.0f) * half));
            const Rgba c = colourOf(in, k, n, res.axis);
            for (int dy = 0; dy <= 1; ++dy) { for (int dx = 0; dx <= 1; ++dx) { blendPx(px, w, h, cx + dx, cy + dy, Rgba{c.r, c.g, c.b, 230}); } }
        }
    }
    return res;
}

}  // namespace foxsdr::slintview3d
