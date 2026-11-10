// scene3d.cpp - the shared 3D scene framework (see scene3d.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "scene3d.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace foxsdr::scene3d {

namespace {

constexpr float kDepthBias = 1.004f;     // a line or marker passes when 1/depth * bias >= the stored 1/depth
constexpr float kDeg = kPi / 180.0f;

std::uint8_t toByte(float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f); }

}  // namespace

Vec3 normalize(Vec3 a) {
    const float l = length(a);
    return l > 1e-20f ? a * (1.0f / l) : Vec3{0.0f, 0.0f, 1.0f};
}

Rgba mix(Rgba a, Rgba b, float t) {
    t = std::clamp(std::isfinite(t) ? t : 0.0f, 0.0f, 1.0f);
    const auto m = [t](std::uint8_t x, std::uint8_t y) { return toByte(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}
Rgba withAlpha(Rgba c, std::uint8_t a) { c.a = a; return c; }
Rgba scaled(Rgba c, float k) { return {toByte(c.r * k), toByte(c.g * k), toByte(c.b * k), c.a}; }

// ---- camera ----
Camera clamped(Camera c) {
    c.elevationDeg = std::clamp(std::isnan(c.elevationDeg) ? 32.0f : c.elevationDeg, kMinElevationDeg, kMaxElevationDeg);   // an infinity clamps to its end; only a NaN is replaced
    c.zoom = std::clamp(std::isnan(c.zoom) ? 1.0f : c.zoom, kMinZoom, kMaxZoom);
    float a = std::isfinite(c.azimuthDeg) ? c.azimuthDeg : 0.0f;
    a = std::fmod(a + 180.0f, 360.0f);
    if (a < 0.0f) { a += 360.0f; }
    c.azimuthDeg = a - 180.0f;
    c.sceneRadius = std::max(1e-6f, c.sceneRadius);
    c.fovDeg = std::clamp(c.fovDeg, 10.0f, 90.0f);
    return c;
}

Vec3 eyePosition(const Camera& c) {
    const float az = c.azimuthDeg * kDeg, el = c.elevationDeg * kDeg;
    const float d = c.sceneRadius / std::sin(c.fovDeg * 0.5f * kDeg) / std::max(1e-3f, c.zoom);
    return c.target + Vec3{std::sin(az) * std::cos(el), -std::cos(az) * std::cos(el), std::sin(el)} * d;
}

Camera dragged(Camera c, float dxPx, float dyPx) {
    c.azimuthDeg -= dxPx * kDegPerPixel;
    c.elevationDeg += dyPx * kDegPerPixel;
    return clamped(c);
}
Camera zoomed(Camera c, int notches) {
    c.zoom *= std::pow(kZoomPerNotch, static_cast<float>(notches));
    return clamped(c);
}

// ---- ramps ----
Ramp::Ramp(const std::vector<Stop>& stops) {
    if (stops.size() < 2) { lut_.fill(Rgba{128, 128, 128, 255}); return; }
    for (std::size_t i = 0; i < lut_.size(); ++i) {
        const float t = static_cast<float>(i) / 255.0f;
        std::size_t k = 1;
        while (k + 1 < stops.size() && t > stops[k].at) { ++k; }
        const Stop& a = stops[k - 1];
        const Stop& b = stops[k];
        const float span = b.at - a.at;
        lut_[i] = mix(a.c, b.c, span > 1e-6f ? (t - a.at) / span : 1.0f);
    }
}
Rgba Ramp::at(float t) const {
    if (!std::isfinite(t)) { t = 0.0f; }
    return lut_[static_cast<std::size_t>(std::clamp(t, 0.0f, 1.0f) * 255.0f + 0.5f)];
}
Ramp Ramp::heat() {
    Ramp r;
    for (std::size_t i = 0; i < r.lut_.size(); ++i) { r.lut_[i] = foxsdr::slinthost::heat(static_cast<float>(i) / 255.0f); }
    return r;
}
Ramp Ramp::altitude() {
    return Ramp({{0.00f, {58, 20, 170, 255}}, {0.30f, {150, 36, 160, 255}}, {0.55f, {214, 74, 130, 255}}, {0.80f, {240, 124, 84, 255}}, {1.00f, {255, 196, 96, 255}}});
}
Ramp Ramp::viridis() {
    return Ramp({{0.00f, {68, 1, 84, 255}}, {0.25f, {59, 82, 139, 255}}, {0.50f, {33, 145, 140, 255}}, {0.75f, {94, 201, 98, 255}}, {1.00f, {253, 231, 37, 255}}});
}
Ramp Ramp::timeTunnel() {
    return Ramp({{0.00f, {0, 229, 255, 255}}, {0.50f, {110, 120, 255, 255}}, {1.00f, {255, 0, 230, 255}}});
}
Ramp Ramp::blueHeight() {
    return Ramp({{0.00f, {12, 26, 58, 255}}, {0.45f, {30, 78, 140, 255}}, {1.00f, {120, 196, 255, 255}}});
}

// ---- meshes ----
Mesh buildHeightField(const HeightFieldSpec& s) {
    Mesh m;
    if (s.cols < 2 || s.rows < 2 || s.values == nullptr) { return m; }
    const std::size_t n = static_cast<std::size_t>(s.cols) * static_cast<std::size_t>(s.rows);
    m.v.resize(n);
    std::vector<unsigned char> ok(n, 0);
    const float span = std::max(1e-9f, s.hi - s.lo);
    const float cspan = std::max(1e-9f, s.colourHi - s.colourLo);
    const Ramp fallback = Ramp::heat();
    const Ramp& ramp = s.ramp != nullptr ? *s.ramp : fallback;
    for (int r = 0; r < s.rows; ++r) {
        for (int c = 0; c < s.cols; ++c) {
            const std::size_t i = static_cast<std::size_t>(r) * static_cast<std::size_t>(s.cols) + static_cast<std::size_t>(c);
            const float v = s.values[i];
            Vertex& vt = m.v[i];
            const float fx = static_cast<float>(c) / static_cast<float>(s.cols - 1), fy = static_cast<float>(r) / static_cast<float>(s.rows - 1);
            const bool finite = std::isfinite(v) && (s.colourValues == nullptr || std::isfinite(s.colourValues[i]));
            ok[i] = finite ? 1 : 0;
            const float nz = finite ? std::clamp((v - s.lo) / span, 0.0f, 1.0f) : 0.0f;
            vt.p = {s.origin.x + s.sx * fx, s.origin.y + s.sy * fy, s.origin.z + s.zScale * nz};
            const float cv = s.colourValues != nullptr ? (finite ? std::clamp((s.colourValues[i] - s.colourLo) / cspan, 0.0f, 1.0f) : 0.0f) : nz;
            vt.c = ramp.at(cv);
            vt.u = fx;
            vt.v = fy;
        }
    }
    m.idx.reserve(n * 6);
    for (int r = 0; r + 1 < s.rows; ++r) {
        for (int c = 0; c + 1 < s.cols; ++c) {
            const std::uint32_t i00 = static_cast<std::uint32_t>(r * s.cols + c), i10 = i00 + 1, i01 = i00 + static_cast<std::uint32_t>(s.cols), i11 = i01 + 1;
            if (!(ok[i00] && ok[i10] && ok[i01] && ok[i11])) { continue; }
            m.idx.insert(m.idx.end(), {i00, i10, i11, i00, i11, i01});
        }
    }
    return m;
}

Vec3 latLonToVec(double latDeg, double lonDeg, float radius) {
    const double la = latDeg * 3.14159265358979323846 / 180.0, lo = lonDeg * 3.14159265358979323846 / 180.0;
    return {static_cast<float>(std::cos(la) * std::cos(lo)) * radius, static_cast<float>(std::cos(la) * std::sin(lo)) * radius, static_cast<float>(std::sin(la)) * radius};
}

Mesh buildSphere(float radius, int lonSeg, int latSeg, Rgba colour) {
    Mesh m;
    lonSeg = std::max(3, lonSeg);
    latSeg = std::max(2, latSeg);
    for (int j = 0; j <= latSeg; ++j) {
        const float v = static_cast<float>(j) / static_cast<float>(latSeg);
        const double lat = 90.0 - 180.0 * static_cast<double>(v);
        for (int i = 0; i <= lonSeg; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(lonSeg);
            const double lon = -180.0 + 360.0 * static_cast<double>(u);
            Vertex vt;
            vt.p = latLonToVec(lat, lon, radius);
            vt.c = colour;
            vt.u = u;
            vt.v = v;
            m.v.push_back(vt);
        }
    }
    const std::uint32_t stride = static_cast<std::uint32_t>(lonSeg + 1);
    for (int j = 0; j < latSeg; ++j) {
        for (int i = 0; i < lonSeg; ++i) {
            const std::uint32_t a = static_cast<std::uint32_t>(j) * stride + static_cast<std::uint32_t>(i), b = a + 1, c = a + stride, d = c + 1;
            // the winding does not matter (two-sided); the pole rows collapse to a point and are skipped through their zero area
            m.idx.insert(m.idx.end(), {a, c, d, a, d, b});
        }
    }
    return m;
}

std::vector<Vec3> greatCircleArc(double lat0, double lon0, double lat1, double lon1, float radius, float liftFrac, int n) {
    n = std::max(1, n);
    const Vec3 a = latLonToVec(lat0, lon0, 1.0f), b = latLonToVec(lat1, lon1, 1.0f);
    const float cosw = std::clamp(dot(a, b), -1.0f, 1.0f);
    const float omega = std::acos(cosw);
    const float sinw = std::sin(omega);
    std::vector<Vec3> out;
    out.reserve(static_cast<std::size_t>(n) + 1);
    for (int i = 0; i <= n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        Vec3 p;
        if (sinw < 1e-5f) { p = normalize(lerp(a, b, t)); }
        else { p = a * (std::sin((1.0f - t) * omega) / sinw) + b * (std::sin(t * omega) / sinw); }
        out.push_back(p * (radius * (1.0f + liftFrac * std::sin(kPi * t))));
    }
    return out;
}

Rgba sampleTexture(const Texture& t, float u, float v) {
    if (t.px == nullptr || t.w <= 0 || t.h <= 0) { return Rgba{255, 255, 255, 255}; }
    u -= std::floor(u);
    v = std::clamp(v, 0.0f, 1.0f);
    const float fx = u * static_cast<float>(t.w) - 0.5f, fy = v * static_cast<float>(t.h) - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
    const auto at = [&](int x, int y) -> const Rgba& {
        x = ((x % t.w) + t.w) % t.w;
        y = std::clamp(y, 0, t.h - 1);
        return t.px[static_cast<std::size_t>(y) * static_cast<std::size_t>(t.w) + static_cast<std::size_t>(x)];
    };
    const Rgba top = mix(at(x0, y0), at(x0 + 1, y0), tx);
    const Rgba bot = mix(at(x0, y0 + 1), at(x0 + 1, y0 + 1), tx);
    return mix(top, bot, ty);
}

// ---- the scene ----
void Scene::begin(Rgba* px, int w, int h, Rgba background, const Camera& camera) {
    px_ = px;
    w_ = std::max(0, w);
    h_ = std::max(0, h);
    trisDrawn_ = 0;
    pxWritten_ = 0;
    cam_ = clamped(camera);
    const std::size_t n = static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_);
    if (inv_.size() < n) { inv_.resize(n); }
    if (px_ != nullptr) { std::fill(px_, px_ + n, background); }
    std::fill(inv_.begin(), inv_.begin() + static_cast<std::ptrdiff_t>(n), 0.0f);
    eye_ = eyePosition(cam_);
    fwd_ = normalize(cam_.target - eye_);
    right_ = normalize(cross(fwd_, Vec3{0.0f, 0.0f, 1.0f}));
    up_ = cross(right_, fwd_);
    const float half = static_cast<float>(std::max(1, std::min(w_, h_))) * 0.5f;
    focal_ = half / std::tan(cam_.fovDeg * 0.5f * kDeg);
}

Scene::VP Scene::toView(const Vertex& vtx) const {
    const Vec3 d = vtx.p - eye_;
    return {dot(d, right_), dot(d, up_), dot(d, fwd_), vtx.c, vtx.u, vtx.v};
}

bool Scene::toScreen(const VP& p, float& sx, float& sy, float& inv) const {
    if (p.z <= 1e-6f) { return false; }
    inv = 1.0f / p.z;
    sx = static_cast<float>(w_) * 0.5f + p.x * inv * focal_;
    sy = static_cast<float>(h_) * 0.5f - p.y * inv * focal_;
    return true;
}

ScreenPoint Scene::project(Vec3 p) const {
    ScreenPoint sp;
    const Vec3 d = p - eye_;
    const float z = dot(d, fwd_);
    sp.depth = z;
    if (z <= cam_.sceneRadius * 0.02f) { return sp; }
    sp.inFront = true;
    sp.x = static_cast<float>(w_) * 0.5f + dot(d, right_) / z * focal_;
    sp.y = static_cast<float>(h_) * 0.5f - dot(d, up_) / z * focal_;
    return sp;
}

float Scene::depthAt(int x, int y) const {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) { return 0.0f; }
    const float inv = inv_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x)];
    return inv > 0.0f ? 1.0f / inv : 0.0f;
}

bool Scene::occluded(Vec3 p) const {
    const ScreenPoint sp = project(p);
    if (!sp.inFront) { return true; }
    const int x = static_cast<int>(std::floor(sp.x)), y = static_cast<int>(std::floor(sp.y));
    if (x < 0 || y < 0 || x >= w_ || y >= h_) { return true; }
    const float d = depthAt(x, y);
    return d > 0.0f && sp.depth > d * 1.02f;
}

void Scene::setLight(Vec3 towardLight, float ambient) {
    light_ = normalize(towardLight);
    ambient_ = std::clamp(ambient, 0.0f, 1.0f);
}

void Scene::blendPixel(int x, int y, Rgba c, float cover) {
    if (cover <= 0.0f) { return; }
    Rgba& d = px_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x)];
    const float sa = std::clamp(cover * static_cast<float>(c.a) / 255.0f, 0.0f, 1.0f);
    if (sa >= 0.999f) { d = Rgba{c.r, c.g, c.b, 255}; return; }
    const float da = static_cast<float>(d.a) / 255.0f;
    const float oa = sa + da * (1.0f - sa);
    if (oa <= 1e-6f) { return; }
    const auto ch = [&](std::uint8_t s, std::uint8_t dd) { return toByte((static_cast<float>(s) * sa + static_cast<float>(dd) * da * (1.0f - sa)) / oa); };
    d = Rgba{ch(c.r, d.r), ch(c.g, d.g), ch(c.b, d.b), toByte(oa * 255.0f)};
}

namespace {

// a clipped vertex between two view-space vertices at the near plane
template <typename VP>
VP clipLerp(const VP& a, const VP& b, float nearZ) {
    const float t = (nearZ - a.z) / (b.z - a.z);
    const auto m = [t](std::uint8_t x, std::uint8_t y) { return toByte(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t); };
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, nearZ, Rgba{m(a.c.r, b.c.r), m(a.c.g, b.c.g), m(a.c.b, b.c.b), m(a.c.a, b.c.a)}, a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t};
}

}  // namespace

void Scene::triangle(const Vertex& a, const Vertex& b, const Vertex& c, bool lit) {
    if (px_ == nullptr || w_ <= 0 || h_ <= 0) { return; }
    triView(toView(a), toView(b), toView(c), lit);
}

void Scene::triView(const VP& va, const VP& vb, const VP& vc, bool lit) {
    float shade = 1.0f;
    if (lit) {
        const Vec3 e1{vb.x - va.x, vb.y - va.y, vb.z - va.z}, e2{vc.x - va.x, vc.y - va.y, vc.z - va.z};
        const Vec3 n = normalize(cross(e1, e2));
        const Vec3 lv{dot(light_, right_), dot(light_, up_), dot(light_, fwd_)};
        shade = ambient_ + (1.0f - ambient_) * std::fabs(dot(n, lv));
    }
    const float nearZ = cam_.sceneRadius * 0.02f;
    if (va.z >= nearZ && vb.z >= nearZ && vc.z >= nearZ) { rasterTriangle(va, vb, vc, shade); return; }
    const VP in[4] = {va, vb, vc, va};
    VP out[4];
    int n = 0;
    for (int i = 0; i < 3; ++i) {   // Sutherland-Hodgman against the near plane
        const VP& p = in[i];
        const VP& q = in[(i + 1) % 3];
        const bool pin = p.z >= nearZ, qin = q.z >= nearZ;
        if (pin) { out[n++] = p; }
        if (pin != qin) { out[n++] = pin ? clipLerp(p, q, nearZ) : clipLerp(q, p, nearZ); }
    }
    if (n < 3) { return; }
    rasterTriangle(out[0], out[1], out[2], shade);
    if (n == 4) { rasterTriangle(out[0], out[2], out[3], shade); }
}

std::size_t Scene::drawMesh(const Mesh& m, bool lit) {
    const std::size_t before = trisDrawn_;
    if (px_ == nullptr || w_ <= 0 || h_ <= 0) { return 0; }
    vcache_.resize(m.v.size());   // each vertex is shared by up to six triangles: transformed once
    for (std::size_t i = 0; i < m.v.size(); ++i) { vcache_[i] = toView(m.v[i]); }
    for (std::size_t i = 0; i + 2 < m.idx.size(); i += 3) { triView(vcache_[m.idx[i]], vcache_[m.idx[i + 1]], vcache_[m.idx[i + 2]], lit); }
    return trisDrawn_ - before;
}

void Scene::rasterTriangle(const VP& a, const VP& b, const VP& c, float shade) {
    float ax, ay, ia, bx, by, ib, cx, cy, ic;
    if (!toScreen(a, ax, ay, ia) || !toScreen(b, bx, by, ib) || !toScreen(c, cx, cy, ic)) { return; }
    const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (std::fabs(area) < 1e-7f) { return; }
    // (floor and ceil are library calls without SSE4.1: truncation of a clamped non-negative value is the same and ~10x cheaper in a loop run for every triangle)
    const float fw = static_cast<float>(w_), fh = static_cast<float>(h_);
    const float minx = std::min({ax, bx, cx}), maxx = std::max({ax, bx, cx}), miny = std::min({ay, by, cy}), maxy = std::max({ay, by, cy});
    if (maxx < 0.0f || maxy < 0.0f || minx > fw || miny > fh) { return; }
    const int x0 = static_cast<int>(std::max(0.0f, minx)), x1 = std::min(w_ - 1, static_cast<int>(std::min(fw, maxx) + 0.999f));
    const int y0 = static_cast<int>(std::max(0.0f, miny)), y1 = std::min(h_ - 1, static_cast<int>(std::min(fh, maxy) + 0.999f));
    if (x0 > x1 || y0 > y1) { return; }
    ++trisDrawn_;
    const float invArea = 1.0f / area;
    const float dw0 = (by - cy) * invArea, dw1 = (cy - ay) * invArea;
    const bool translucent = a.c.a < 255 || b.c.a < 255 || c.c.a < 255;
    const bool textured = tex_ != nullptr;
    const float ra = a.c.r, ga = a.c.g, ba = a.c.b, aa = a.c.a;
    const float rb = b.c.r, gb = b.c.g, bb = b.c.b, ab = b.c.a;
    const float rc = c.c.r, gc = c.c.g, bc = c.c.b, ac = c.c.a;
    // the pixels of a row inside the triangle are a span: each of the three edge functions is linear in x, so the span's ends are solved, not searched for
    const float dw2 = -dw0 - dw1;
    const float r0 = std::fabs(dw0) > 1e-12f ? 1.0f / dw0 : 0.0f, r1 = std::fabs(dw1) > 1e-12f ? 1.0f / dw1 : 0.0f, r2 = std::fabs(dw2) > 1e-12f ? 1.0f / dw2 : 0.0f;
    for (int y = y0; y <= y1; ++y) {
        const float py = static_cast<float>(y) + 0.5f, pxs = static_cast<float>(x0) + 0.5f;
        const float w0s = ((bx - pxs) * (cy - py) - (by - py) * (cx - pxs)) * invArea;
        const float w1s = ((cx - pxs) * (ay - py) - (cy - py) * (ax - pxs)) * invArea;
        const float w2s = 1.0f - w0s - w1s;
        const float span = static_cast<float>(x1 - x0);
        float xl = 0.0f, xr = span;   // offsets from x0 where every weight is >= 0
        const auto clip = [&](float ws, float dws, float rdws) {
            if (rdws == 0.0f) { return ws >= 0.0f; }
            const float at = -ws * rdws;   // the offset where this weight crosses zero
            if (dws > 0.0f) { xl = std::max(xl, at); } else { xr = std::min(xr, at); }
            return true;
        };
        if (!clip(w0s, dw0, r0) || !clip(w1s, dw1, r1) || !clip(w2s, dw2, r2)) { continue; }
        if (xr < 0.0f || xl > span) { continue; }
        const int xa = x0 + static_cast<int>(xl + 0.999f), xb = x0 + static_cast<int>(xr);
        if (xa > xb) { continue; }
        float w0 = w0s + dw0 * static_cast<float>(xa - x0), w1 = w1s + dw1 * static_cast<float>(xa - x0);
        std::size_t idx = static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(xa);
        for (int x = xa; x <= xb; ++x, ++idx, w0 += dw0, w1 += dw1) {
            const float w2 = 1.0f - w0 - w1;
            const float inv = w0 * ia + w1 * ib + w2 * ic;
            if (inv <= inv_[idx]) { continue; }
            float r = w0 * ra + w1 * rb + w2 * rc, g = w0 * ga + w1 * gb + w2 * gc, bl = w0 * ba + w1 * bb + w2 * bc;
            float al = translucent ? w0 * aa + w1 * ab + w2 * ac : 255.0f;
            if (textured) {
                const float u = (w0 * a.u * ia + w1 * b.u * ib + w2 * c.u * ic) / inv, v = (w0 * a.v * ia + w1 * b.v * ib + w2 * c.v * ic) / inv;
                const Rgba t = sampleTexture(*tex_, u, v);
                r = r * static_cast<float>(t.r) / 255.0f;
                g = g * static_cast<float>(t.g) / 255.0f;
                bl = bl * static_cast<float>(t.b) / 255.0f;
                al = al * static_cast<float>(t.a) / 255.0f;
            }
            const Rgba col{toByte(r * shade), toByte(g * shade), toByte(bl * shade), toByte(al)};
            if (translucent || textured) {
                if (col.a == 0) { continue; }
                if (col.a < 255) { blendPixel(x, y, col, 1.0f); ++pxWritten_; continue; }
            }
            px_[idx] = Rgba{col.r, col.g, col.b, 255};
            inv_[idx] = inv;
            ++pxWritten_;
        }
    }
}

void Scene::line(Vec3 a, Vec3 b, Rgba colour, float widthPx, Dash dash) {
    if (px_ == nullptr || w_ <= 0 || h_ <= 0) { return; }
    Vertex va, vb;
    va.p = a;
    vb.p = b;
    va.c = vb.c = colour;
    VP pa = toView(va), pb = toView(vb);
    const float nearZ = cam_.sceneRadius * 0.02f;
    if (pa.z < nearZ && pb.z < nearZ) { return; }
    if (pa.z < nearZ) { pa = clipLerp(pa, pb, nearZ); }
    if (pb.z < nearZ) { pb = clipLerp(pb, pa, nearZ); }
    drawLineView(pa, pb, widthPx, dash);
}

void Scene::drawLineView(VP pa, VP pb, float widthPx, Dash dash) {
    float x0, y0, i0, x1, y1, i1;
    if (!toScreen(pa, x0, y0, i0) || !toScreen(pb, x1, y1, i1)) { return; }
    const float fullLen = std::hypot(x1 - x0, y1 - y0);
    const float hw = std::max(0.5f, widthPx) * 0.5f;
    // Liang-Barsky against the picture grown by the half width: the wall of a box seen from close up can have endpoints a million pixels away
    float t0 = 0.0f, t1 = 1.0f;
    const float dx = x1 - x0, dy = y1 - y0;
    const float xmin = -hw - 2.0f, xmax = static_cast<float>(w_) + hw + 2.0f, ymin = -hw - 2.0f, ymax = static_cast<float>(h_) + hw + 2.0f;
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {x0 - xmin, xmax - x0, y0 - ymin, ymax - y0};
    for (int k = 0; k < 4; ++k) {
        if (std::fabs(p[k]) < 1e-12f) {
            if (q[k] < 0.0f) { return; }
            continue;
        }
        const float r = q[k] / p[k];
        if (p[k] < 0.0f) { if (r > t1) { return; } t0 = std::max(t0, r); }
        else { if (r < t0) { return; } t1 = std::min(t1, r); }
    }
    if (t0 > t1) { return; }
    const float s0 = t0 * fullLen;   // the dash phase at the clipped start
    const float cx0 = x0 + dx * t0, cy0 = y0 + dy * t0, cx1 = x0 + dx * t1, cy1 = y0 + dy * t1;
    const float ci0 = i0 + (i1 - i0) * t0, ci1 = i0 + (i1 - i0) * t1;
    const float L = std::hypot(cx1 - cx0, cy1 - cy0);
    const Rgba col = pa.c;   // one colour per segment (polyline() sends each segment its own)
    const float wscale = std::max(1.0f, widthPx / 1.5f);
    const float on = dash == Dash::Dashed ? 7.0f * wscale : 1.5f * wscale, period = dash == Dash::Dashed ? 12.0f * wscale : 5.0f * wscale;
    if (L < 1e-3f) {   // a dot
        const int x = static_cast<int>(std::floor(cx0)), y = static_cast<int>(std::floor(cy0));
        if (x >= 0 && y >= 0 && x < w_ && y < h_ && ci0 * kDepthBias >= inv_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x)]) { blendPixel(x, y, col, 1.0f); ++pxWritten_; }
        return;
    }
    const float ux = (cx1 - cx0) / L, uy = (cy1 - cy0) / L;
    const bool steep = std::fabs(uy) > std::fabs(ux);
    const float major = steep ? std::fabs(uy) : std::fabs(ux);   // cos of the angle to the major axis
    const float ext = (hw + 1.0f) / major;
    const auto visit = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= w_ || y >= h_) { return; }
        const float rx = static_cast<float>(x) + 0.5f - cx0, ry = static_cast<float>(y) + 0.5f - cy0;
        const float s = rx * ux + ry * uy;       // along the line
        const float d = std::fabs(rx * uy - ry * ux);   // across it
        float cover = std::clamp(hw + 0.5f - d, 0.0f, 1.0f);
        cover *= std::clamp(s + 0.5f, 0.0f, 1.0f) * std::clamp(L - s + 0.5f, 0.0f, 1.0f);
        if (cover <= 0.0f) { return; }
        if (dash != Dash::Solid) {
            const float ph = std::fmod(s0 + std::max(0.0f, s), period);
            if (ph >= on) { return; }
        }
        const float inv = ci0 + (ci1 - ci0) * std::clamp(s / L, 0.0f, 1.0f);
        const std::size_t idx = static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x);
        if (inv * kDepthBias < inv_[idx]) { return; }
        blendPixel(x, y, col, cover);
        ++pxWritten_;
    };
    if (!steep) {
        const int xa = static_cast<int>(std::floor(std::min(cx0, cx1) - hw - 1.0f)), xb = static_cast<int>(std::ceil(std::max(cx0, cx1) + hw + 1.0f));
        for (int x = xa; x <= xb; ++x) {
            const float yc = cy0 + (static_cast<float>(x) + 0.5f - cx0) * uy / ux;
            const int ya = static_cast<int>(std::floor(yc - ext)), yb = static_cast<int>(std::ceil(yc + ext));
            for (int y = ya; y <= yb; ++y) { visit(x, y); }
        }
    } else {
        const int ya = static_cast<int>(std::floor(std::min(cy0, cy1) - hw - 1.0f)), yb = static_cast<int>(std::ceil(std::max(cy0, cy1) + hw + 1.0f));
        for (int y = ya; y <= yb; ++y) {
            const float xc = cx0 + (static_cast<float>(y) + 0.5f - cy0) * ux / uy;
            const int xa = static_cast<int>(std::floor(xc - ext)), xb = static_cast<int>(std::ceil(xc + ext));
            for (int x = xa; x <= xb; ++x) { visit(x, y); }
        }
    }
}

void Scene::polyline(const std::vector<Vec3>& pts, const std::vector<Rgba>& colours, Rgba colour, float widthPx, Dash dash) {
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const Rgba c = (!colours.empty() && i < colours.size()) ? colours[i] : colour;
        line(pts[i - 1], pts[i], c, widthPx, dash);
    }
}

void Scene::point(Vec3 p, Rgba colour, float radiusPx) {
    if (px_ == nullptr) { return; }
    const ScreenPoint sp = project(p);
    if (!sp.inFront) { return; }
    const int cxp = static_cast<int>(std::floor(sp.x)), cyp = static_cast<int>(std::floor(sp.y));
    if (cxp < -64 || cyp < -64 || cxp >= w_ + 64 || cyp >= h_ + 64) { return; }
    const float inv = 1.0f / sp.depth;
    const float r = std::max(0.5f, radiusPx);
    const int R = static_cast<int>(std::ceil(r)) + 1;
    for (int y = cyp - R; y <= cyp + R; ++y) {
        for (int x = cxp - R; x <= cxp + R; ++x) {
            if (x < 0 || y < 0 || x >= w_ || y >= h_) { continue; }
            const float d = std::hypot(static_cast<float>(x) + 0.5f - sp.x, static_cast<float>(y) + 0.5f - sp.y);
            const float cover = std::clamp(r + 0.5f - d, 0.0f, 1.0f);
            if (cover <= 0.0f) { continue; }
            if (inv * kDepthBias < inv_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x)]) { continue; }
            blendPixel(x, y, colour, cover);
            ++pxWritten_;
        }
    }
}

namespace {

bool inPoly(const float* xs, const float* ys, int n, float px, float py) {
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if (((ys[i] > py) != (ys[j] > py)) && (px < (xs[j] - xs[i]) * (py - ys[i]) / (ys[j] - ys[i]) + xs[i])) { in = !in; }
    }
    return in;
}

// 1 when the sample (dx, dy) (pixels from the marker's centre) is inside the shape of size s (its bounding box side)
bool inShape(Marker m, float dx, float dy, float s) {
    const float h = s * 0.5f;
    switch (m) {
    case Marker::Dot: return dx * dx + dy * dy <= h * h;
    case Marker::Square: return std::fabs(dx) <= h && std::fabs(dy) <= h;
    case Marker::Diamond: return std::fabs(dx) + std::fabs(dy) <= h * 1.25f;
    case Marker::Cross: { const float t = std::max(0.9f, s * 0.2f); return (std::fabs(dx) <= h && std::fabs(dy) <= t) || (std::fabs(dy) <= h && std::fabs(dx) <= t); }
    case Marker::TriangleUp: { const float xs[3] = {0.0f, -h, h}, ys[3] = {-h, h * 0.8f, h * 0.8f}; return inPoly(xs, ys, 3, dx, dy); }
    case Marker::TriangleDown: { const float xs[3] = {0.0f, -h, h}, ys[3] = {h, -h * 0.8f, -h * 0.8f}; return inPoly(xs, ys, 3, dx, dy); }
    case Marker::Star: {
        float xs[10], ys[10];
        for (int i = 0; i < 10; ++i) {
            const float a = -kPi * 0.5f + static_cast<float>(i) * kPi / 5.0f, rr = (i % 2 == 0) ? h * 1.15f : h * 0.48f;
            xs[i] = std::cos(a) * rr;
            ys[i] = std::sin(a) * rr;
        }
        return inPoly(xs, ys, 10, dx, dy);
    }
    }
    return false;
}

}  // namespace

void Scene::marker(Vec3 p, Marker shape, Rgba colour, float sizePx) {
    if (px_ == nullptr) { return; }
    const ScreenPoint sp = project(p);
    if (!sp.inFront) { return; }
    const float s = std::max(2.0f, sizePx);
    const int cxp = static_cast<int>(std::floor(sp.x)), cyp = static_cast<int>(std::floor(sp.y));
    if (cxp < -64 || cyp < -64 || cxp >= w_ + 64 || cyp >= h_ + 64) { return; }
    const float inv = 1.0f / sp.depth;
    const int R = static_cast<int>(std::ceil(s * 0.7f)) + 2;
    for (int y = cyp - R; y <= cyp + R; ++y) {
        for (int x = cxp - R; x <= cxp + R; ++x) {
            if (x < 0 || y < 0 || x >= w_ || y >= h_) { continue; }
            int hit = 0;
            for (int sy = 0; sy < 3; ++sy) {
                for (int sx = 0; sx < 3; ++sx) {
                    const float dx = static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / 3.0f - sp.x, dy = static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / 3.0f - sp.y;
                    hit += inShape(shape, dx, dy, s) ? 1 : 0;
                }
            }
            if (hit == 0) { continue; }
            if (inv * kDepthBias < inv_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x)]) { continue; }
            blendPixel(x, y, colour, static_cast<float>(hit) / 9.0f);
            ++pxWritten_;
        }
    }
}

// ---- the axis box ----
namespace {

// the corner of the box (bit 0 x, bit 1 y, bit 2 z: 1 = hi)
Vec3 corner(const BoxSpec& b, int k) { return {(k & 1) ? b.hi.x : b.lo.x, (k & 2) ? b.hi.y : b.lo.y, (k & 4) ? b.hi.z : b.lo.z}; }

}  // namespace

BoxLayout drawBoxBack(Scene& s, const BoxSpec& b) {
    BoxLayout lay;
    const Vec3 eye = eyePosition(s.camera());
    const Vec3 mid = (b.lo + b.hi) * 0.5f;
    const float backX = eye.x >= mid.x ? b.lo.x : b.hi.x, backY = eye.y >= mid.y ? b.lo.y : b.hi.y;   // the walls behind the data
    const float frontX = eye.x >= mid.x ? b.hi.x : b.lo.x, frontY = eye.y >= mid.y ? b.hi.y : b.lo.y;
    const auto at = [](float lo, float hi, int i, int n) { return n <= 1 ? lo : lo + (hi - lo) * static_cast<float>(i) / static_cast<float>(n - 1); };
    if (b.floor) {
        for (int i = 0; i < b.xTicks; ++i) { const float x = at(b.lo.x, b.hi.x, i, b.xTicks); s.line({x, b.lo.y, b.lo.z}, {x, b.hi.y, b.lo.z}, b.floorGrid, 1.0f); }
        for (int j = 0; j < b.yTicks; ++j) { const float y = at(b.lo.y, b.hi.y, j, b.yTicks); s.line({b.lo.x, y, b.lo.z}, {b.hi.x, y, b.lo.z}, b.floorGrid, 1.0f); }
    }
    if (b.walls) {
        for (int j = 0; j < b.yTicks; ++j) { const float y = at(b.lo.y, b.hi.y, j, b.yTicks); s.line({backX, y, b.lo.z}, {backX, y, b.hi.z}, b.grid, 1.0f); }
        for (int k = 0; k < b.zTicks; ++k) { const float z = at(b.lo.z, b.hi.z, k, b.zTicks); s.line({backX, b.lo.y, z}, {backX, b.hi.y, z}, b.grid, 1.0f); }
        for (int i = 0; i < b.xTicks; ++i) { const float x = at(b.lo.x, b.hi.x, i, b.xTicks); s.line({x, backY, b.lo.z}, {x, backY, b.hi.z}, b.grid, 1.0f); }
        for (int k = 0; k < b.zTicks; ++k) { const float z = at(b.lo.z, b.hi.z, k, b.zTicks); s.line({b.lo.x, backY, z}, {b.hi.x, backY, z}, b.grid, 1.0f); }
        // the walls' outlines
        s.line({backX, backY, b.lo.z}, {backX, backY, b.hi.z}, b.edge, b.edgeWidthPx);
        s.line({backX, backY, b.hi.z}, {frontX, backY, b.hi.z}, b.edge, b.edgeWidthPx);
        s.line({backX, backY, b.hi.z}, {backX, frontY, b.hi.z}, b.edge, b.edgeWidthPx);
        s.line({frontX, backY, b.lo.z}, {frontX, backY, b.hi.z}, b.edge, b.edgeWidthPx);
        s.line({backX, frontY, b.lo.z}, {backX, frontY, b.hi.z}, b.edge, b.edgeWidthPx);
    }
    s.line({b.lo.x, b.lo.y, b.lo.z}, {b.hi.x, b.lo.y, b.lo.z}, b.edge, b.edgeWidthPx);
    s.line({b.hi.x, b.lo.y, b.lo.z}, {b.hi.x, b.hi.y, b.lo.z}, b.edge, b.edgeWidthPx);
    s.line({b.hi.x, b.hi.y, b.lo.z}, {b.lo.x, b.hi.y, b.lo.z}, b.edge, b.edgeWidthPx);
    s.line({b.lo.x, b.hi.y, b.lo.z}, {b.lo.x, b.lo.y, b.lo.z}, b.edge, b.edgeWidthPx);

    // ---- where the tick labels go ----
    const float ox = (frontX >= mid.x ? 1.0f : -1.0f), oy = (frontY >= mid.y ? 1.0f : -1.0f);   // outward from the box on the front sides
    const float dxw = b.hi.x - b.lo.x, dyw = b.hi.y - b.lo.y, dzw = b.hi.z - b.lo.z;
    for (int i = 0; i < b.xTicks; ++i) {   // along the floor's front edge in y
        const float f = b.xTicks <= 1 ? 0.0f : static_cast<float>(i) / static_cast<float>(b.xTicks - 1);
        const float x = at(b.lo.x, b.hi.x, i, b.xTicks);
        lay.ticks.push_back({{x, frontY, b.lo.z}, {x, frontY + oy * dyw * 0.07f, b.lo.z - dzw * 0.02f}, f, 0});
    }
    for (int j = 0; j < b.yTicks; ++j) {   // along the floor's front edge in x
        const float f = b.yTicks <= 1 ? 0.0f : static_cast<float>(j) / static_cast<float>(b.yTicks - 1);
        const float y = at(b.lo.y, b.hi.y, j, b.yTicks);
        lay.ticks.push_back({{frontX, y, b.lo.z}, {frontX + ox * dxw * 0.07f, y, b.lo.z - dzw * 0.02f}, f, 1});
    }
    // the z axis stands on the vertical edge whose base is furthest right on the screen, leaving out the corner nearest the eye (its edge would cut across the data)
    int best = -1;
    float bestX = -1e30f, nearest = 1e30f;
    int nearestK = 0;
    for (int k = 0; k < 4; ++k) {
        const Vec3 c = corner(b, k);
        const float d = length(c - eye);
        if (d < nearest) { nearest = d; nearestK = k; }
    }
    for (int k = 0; k < 4; ++k) {
        if (k == nearestK) { continue; }
        const ScreenPoint sp = s.project(corner(b, k));
        if (sp.inFront && sp.x > bestX) { bestX = sp.x; best = k; }
    }
    if (best < 0) { best = 3 - nearestK; }
    const Vec3 zc = corner(b, best);
    const float zox = zc.x >= mid.x ? 1.0f : -1.0f, zoy = zc.y >= mid.y ? 1.0f : -1.0f;
    for (int k = 0; k < b.zTicks; ++k) {
        const float f = b.zTicks <= 1 ? 0.0f : static_cast<float>(k) / static_cast<float>(b.zTicks - 1);
        const float z = at(b.lo.z, b.hi.z, k, b.zTicks);
        lay.ticks.push_back({{zc.x, zc.y, z}, {zc.x + zox * dxw * 0.05f, zc.y + zoy * dyw * 0.05f, z}, f, 2});
    }
    lay.axisTitle[0] = {mid.x, frontY + oy * dyw * 0.18f, b.lo.z - dzw * 0.04f};
    lay.axisTitle[1] = {frontX + ox * dxw * 0.18f, mid.y, b.lo.z - dzw * 0.04f};
    lay.axisTitle[2] = {zc.x + zox * dxw * 0.16f, zc.y + zoy * dyw * 0.16f, (b.lo.z + b.hi.z) * 0.5f};
    return lay;
}

void drawBoxFront(Scene& s, const BoxSpec& b) {
    const Vec3 eye = eyePosition(s.camera());
    const Vec3 mid = (b.lo + b.hi) * 0.5f;
    const float frontX = eye.x >= mid.x ? b.hi.x : b.lo.x, frontY = eye.y >= mid.y ? b.hi.y : b.lo.y;
    const float dxw = b.hi.x - b.lo.x, dyw = b.hi.y - b.lo.y;
    const float ox = (frontX >= mid.x ? 1.0f : -1.0f), oy = (frontY >= mid.y ? 1.0f : -1.0f);
    s.line({b.lo.x, frontY, b.lo.z}, {b.hi.x, frontY, b.lo.z}, b.edge, b.edgeWidthPx);
    s.line({frontX, b.lo.y, b.lo.z}, {frontX, b.hi.y, b.lo.z}, b.edge, b.edgeWidthPx);
    for (int i = 0; i < b.xTicks; ++i) {
        const float x = b.xTicks <= 1 ? b.lo.x : b.lo.x + dxw * static_cast<float>(i) / static_cast<float>(b.xTicks - 1);
        s.line({x, frontY, b.lo.z}, {x, frontY + oy * dyw * 0.025f, b.lo.z}, b.edge, 1.2f);
    }
    for (int j = 0; j < b.yTicks; ++j) {
        const float y = b.yTicks <= 1 ? b.lo.y : b.lo.y + dyw * static_cast<float>(j) / static_cast<float>(b.yTicks - 1);
        s.line({frontX, y, b.lo.z}, {frontX + ox * dxw * 0.025f, y, b.lo.z}, b.edge, 1.2f);
    }
}

bool addLabel(const Scene& s, std::vector<Label3D>& out, Vec3 p, const std::string& text, std::uint32_t rgba, int align, int kind, bool hideBehind) {
    const ScreenPoint sp = s.project(p);
    if (!sp.inFront || s.width() <= 0 || s.height() <= 0) { return false; }
    if (sp.x < -8.0f || sp.y < -8.0f || sp.x > static_cast<float>(s.width()) + 8.0f || sp.y > static_cast<float>(s.height()) + 8.0f) { return false; }
    if (hideBehind && s.occluded(p)) { return false; }
    out.push_back({sp.x / static_cast<float>(s.width()), sp.y / static_cast<float>(s.height()), text, rgba, align, kind});
    return true;
}

}  // namespace foxsdr::scene3d
