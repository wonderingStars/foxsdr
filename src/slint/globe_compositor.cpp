// globe_compositor.cpp - see globe_compositor.hpp. SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "globe_compositor.hpp"

#include <algorithm>
#include <cmath>

namespace foxsdr::globe {

namespace {

scene3d::Vec3 toF(const Vec3& v) { return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}; }

scene3d::Marker markerOf(MarkerShape s) {
    switch (s) {
        case MarkerShape::Triangle: return scene3d::Marker::TriangleUp;
        case MarkerShape::Square: return scene3d::Marker::Square;
        case MarkerShape::Ring: return scene3d::Marker::Diamond;
        case MarkerShape::Star: return scene3d::Marker::Star;
        default: return scene3d::Marker::Dot;
    }
}

// The framework's dash restarts on every segment, so a polyline of short segments (an arc, a ring) would come out solid. A dashed line is cut here instead, by its length in
// world units (the globe's radius is 1): `on` drawn, `off` left, the pieces drawn as solid polylines.
std::vector<std::vector<scene3d::Vec3>> dashed(const std::vector<scene3d::Vec3>& pts, float on, float off) {
    std::vector<std::vector<scene3d::Vec3>> out;
    std::vector<scene3d::Vec3> cur;
    float phase = 0.0f;       // distance into the current on or off stretch
    bool drawing = true;
    if (pts.empty()) { return out; }
    cur.push_back(pts.front());
    for (std::size_t i = 1; i < pts.size(); ++i) {
        scene3d::Vec3 a = pts[i - 1];
        const scene3d::Vec3 b = pts[i];
        float left = scene3d::length(b - a);
        while (left > 0.0f) {
            const float limit = drawing ? on : off;
            const float room = limit - phase;
            if (left < room) {
                phase += left;
                if (drawing) { cur.push_back(b); }
                break;
            }
            const scene3d::Vec3 cut = a + (b - a) * (room / left);
            if (drawing) { cur.push_back(cut); out.push_back(cur); cur.clear(); }
            else { cur.push_back(cut); }
            a = cut;
            left = scene3d::length(b - a);
            phase = 0.0f;
            drawing = !drawing;
        }
    }
    if (drawing && cur.size() >= 2u) { out.push_back(cur); }
    return out;
}

}  // namespace

Rotation rotationFor(const GlobeCamera& c) {
    const double th = (-90.0 - c.centreLonDeg) * kDegToRad;
    const double be = (c.centreLatDeg - 90.0) * kDegToRad;
    const double cz = std::cos(th), sz = std::sin(th), cx = std::cos(be), sx = std::sin(be);
    // Rx(be) * Rz(th)
    Rotation r{};
    r.m[0][0] = cz;       r.m[0][1] = -sz;      r.m[0][2] = 0.0;
    r.m[1][0] = cx * sz;  r.m[1][1] = cx * cz;  r.m[1][2] = -sx;
    r.m[2][0] = sx * sz;  r.m[2][1] = sx * cz;  r.m[2][2] = cx;
    return r;
}

Vec3 rotate(const Rotation& r, const Vec3& v) {
    return {r.m[0][0] * v.x + r.m[0][1] * v.y + r.m[0][2] * v.z, r.m[1][0] * v.x + r.m[1][1] * v.y + r.m[1][2] * v.z, r.m[2][0] * v.x + r.m[2][1] * v.y + r.m[2][2] * v.z};
}

scene3d::Rgba toRgba(std::uint32_t argb) {
    return scene3d::Rgba{static_cast<std::uint8_t>((argb >> 16) & 0xFFu), static_cast<std::uint8_t>((argb >> 8) & 0xFFu), static_cast<std::uint8_t>(argb & 0xFFu),
                         static_cast<std::uint8_t>((argb >> 24) & 0xFFu)};
}

std::uint32_t toLabelRgba(std::uint32_t argb) { return ((argb & 0x00FFFFFFu) << 8) | ((argb >> 24) & 0xFFu); }

bool GlobeCompositor::paint(const GlobeDrawList& list, scene3d::Rgba* px, int width, int height, std::vector<scene3d::Label3D>& labels) {
    labels.clear();
    if (width < 16 || height < 16 || px == nullptr) { return false; }
    const bool globe = list.space == GlobeDrawList::Space::Globe;
    scene3d::Camera cam;
    Rotation rot{};
    if (globe) {
        rot = rotationFor(list.camera);
        cam.azimuthDeg = 0.0f;
        cam.elevationDeg = scene3d::kMaxElevationDeg;
        cam.zoom = static_cast<float>(list.camera.zoom);
        cam.sceneRadius = 1.18f;
    } else {
        cam.azimuthDeg = static_cast<float>(list.camera.centreLonDeg);
        cam.elevationDeg = static_cast<float>(list.camera.centreLatDeg);
        cam.zoom = static_cast<float>(list.camera.zoom);
        cam.sceneRadius = 1.12f;
    }
    scene_.begin(px, width, height, scene3d::Rgba{11, 13, 18, 255}, cam);
    const auto place = [&](const Vec3& v) { return toF(globe ? rotate(rot, v) : v); };

    if (globe && list.sphere) {
        if (sphere_.v.empty()) { sphere_ = scene3d::buildSphere(1.0f, 96, 48, scene3d::Rgba{255, 255, 255, 255}); }
        scene3d::Texture tex;
        if (!list.texture.empty()) {
            if (texKey_ != list.texture.key || texRevision_ != list.texture.revision || texW_ != list.texture.width || texH_ != list.texture.height) {
                texPx_.resize(list.texture.argb.size());
                for (std::size_t i = 0; i < list.texture.argb.size(); ++i) { texPx_[i] = toRgba(list.texture.argb[i]); }
                texKey_ = list.texture.key;
                texRevision_ = list.texture.revision;
                texW_ = list.texture.width;
                texH_ = list.texture.height;
            }
            tex.w = texW_;
            tex.h = texH_;
            tex.px = texPx_.data();
        }
        scene3d::Mesh m = sphere_;
        for (scene3d::Vertex& vt : m.v) {
            const Vec3 p = rotate(rot, Vec3{vt.p.x, vt.p.y, vt.p.z});
            vt.p = toF(p);
            if (tex.px == nullptr) { vt.c = toRgba(palette().oceanDay); }
        }
        scene_.bindTexture(tex.px != nullptr ? &tex : nullptr);
        scene_.drawMesh(m, false);
        scene_.bindTexture(nullptr);
    }
    for (const DrawLine& l : list.lines) {
        std::vector<scene3d::Vec3> pts;
        pts.reserve(l.points.size() + 1u);
        for (const Vec3& p : l.points) { pts.push_back(place(p)); }
        if (l.closed && !pts.empty()) { pts.push_back(pts.front()); }
        if (pts.size() < 2u) { continue; }
        if (l.dashed) {
            for (const auto& piece : dashed(pts, 0.035f, 0.025f)) { if (piece.size() >= 2u) { scene_.polyline(piece, {}, toRgba(l.argb), l.widthPx, scene3d::Dash::Solid); } }
        } else {
            scene_.polyline(pts, {}, toRgba(l.argb), l.widthPx, scene3d::Dash::Solid);
        }
    }
    for (const DrawMarker& mk : list.markers) { scene_.marker(place(mk.at), markerOf(mk.shape), toRgba(mk.argb), mk.sizePx); }
    for (const DrawMarker& mk : list.markers) {
        if (!mk.label.empty()) { scene3d::addLabel(scene_, labels, place(mk.at), mk.label, toLabelRgba(palette().text), 1, 2, mk.hiddenBehind); }
    }
    for (const DrawLabel& lb : list.labels) { scene3d::addLabel(scene_, labels, place(lb.anchor), lb.text, toLabelRgba(lb.argb), 1, 3, lb.hiddenBehind); }
    return true;
}

}  // namespace foxsdr::globe
