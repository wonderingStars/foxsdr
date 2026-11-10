// The SHARED 3D SCENE FRAMEWORK (src/slint/scene3d.hpp, docs 6.64) without Slint or a window: the camera's projection of known points, the z-buffer's depth order, the lines' dash
// coverage, the sphere's silhouette, the same scene giving byte-identical buffers, the mesh builders, the near-plane clip, the label anchors and the drag and wheel rules. It also
// MEASURES the frame time of the heaviest scene (a 160 x 96 surface at the 1440 x 900 window's well size) and prints it; the number is recorded in docs 6.64, never asserted (a loaded
// machine would turn a speed check into a flake).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "scene3d.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace foxsdr::scene3d;

namespace {

double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

constexpr Rgba kBlack{0, 0, 0, 255};
constexpr Rgba kRed{255, 0, 0, 255}, kBlue{0, 0, 255, 255}, kWhite{255, 255, 255, 255}, kGreen{0, 255, 0, 255};

struct Frame {
    int w, h;
    std::vector<Rgba> px;
    Frame(int w_, int h_) : w(w_), h(h_), px(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_)) {}
    const Rgba& at(int x, int y) const { return px[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]; }
    bool lit(int x, int y) const { const Rgba& c = at(x, y); return c.r != 0 || c.g != 0 || c.b != 0; }
    std::size_t litCount() const { std::size_t n = 0; for (int y = 0; y < h; ++y) { for (int x = 0; x < w; ++x) { n += lit(x, y) ? 1 : 0; } } return n; }
};

Camera cam(float az, float el, float zoom = 1.0f) {
    Camera c;
    c.azimuthDeg = az;
    c.elevationDeg = el;
    c.zoom = zoom;
    c.sceneRadius = 1.0f;
    return c;
}

bool sameBytes(const Frame& a, const Frame& b) { return a.px.size() == b.px.size() && std::memcmp(a.px.data(), b.px.data(), a.px.size() * sizeof(Rgba)) == 0; }

// ---- 1. projection of known points ----
void testProjection() {
    Scene s;
    Frame f(400, 300);
    s.begin(f.px.data(), f.w, f.h, kBlack, cam(0.0f, 30.0f));
    const float d = 1.0f / std::sin(16.0f * kPi / 180.0f);   // the eye's distance: radius / sin(fov / 2) at zoom 1
    const float focal = 150.0f / std::tan(16.0f * kPi / 180.0f);   // half of the shorter side over tan(fov / 2)
    const ScreenPoint c = s.project({0, 0, 0});
    CHECK(c.inFront);
    CHECK_NEAR(c.x, 200.0, 1e-3);
    CHECK_NEAR(c.y, 150.0, 1e-3);
    CHECK_NEAR(c.depth, d, 1e-3);
    // a point to the east is to the right at an azimuth of 0, by focal * X / depth (a move along the screen's right axis does not change the depth)
    const ScreenPoint e = s.project({0.5f, 0, 0});
    CHECK_NEAR(e.x, 200.0 + focal * 0.5 / d, 1e-2);
    CHECK_NEAR(e.y, 150.0, 1e-2);
    const ScreenPoint wst = s.project({-0.5f, 0, 0});
    CHECK_NEAR(wst.x + e.x, 400.0, 1e-2);   // symmetric about the middle
    // a point above is higher on the screen (smaller y) and a point north (away) is smaller than the same step toward the eye
    CHECK(s.project({0, 0, 0.5f}).y < 150.0f);
    CHECK(s.project({0, 0.5f, 0}).depth > c.depth);
    CHECK(s.project({0, -0.5f, 0}).depth < c.depth);
    // zoom halves the distance; the same point's offset from the centre doubles in the small-angle limit
    s.begin(f.px.data(), f.w, f.h, kBlack, cam(0.0f, 30.0f, 2.0f));
    CHECK_NEAR(s.project({0, 0, 0}).depth, d / 2.0, 1e-3);
    CHECK(s.project({0.5f, 0, 0}).x - 200.0f > 1.9f * (e.x - 200.0f));
    // azimuth 90: the eye is to the east, so a point east of the target is nearer than the target
    s.begin(f.px.data(), f.w, f.h, kBlack, cam(90.0f, 30.0f));
    CHECK(s.project({0.5f, 0, 0}).depth < s.project({0, 0, 0}).depth);
    CHECK_NEAR(s.project({0.5f, 0, 0}).x, 200.0, 25.0);   // and nearly on the centre line, a little off it by the elevation only
    // behind the eye: not in front
    const Vec3 eye = eyePosition(cam(0.0f, 30.0f));
    s.begin(f.px.data(), f.w, f.h, kBlack, cam(0.0f, 30.0f));
    CHECK(!s.project(eye + (eye * 2.0f)).inFront);
    // the camera's own rules
    CHECK_NEAR(clamped(cam(0, 200)).elevationDeg, kMaxElevationDeg, 1e-6);
    CHECK_NEAR(clamped(cam(0, -10)).elevationDeg, kMinElevationDeg, 1e-6);
    CHECK_NEAR(clamped(cam(0, 30, 100.0f)).zoom, kMaxZoom, 1e-6);
    CHECK_NEAR(clamped(cam(190, 30)).azimuthDeg, -170.0, 1e-4);
    CHECK_NEAR(clamped(cam(-190, 30)).azimuthDeg, 170.0, 1e-4);
}

// ---- 2. depth order ----
void testDepthOrder() {
    Scene s;
    Frame f(200, 200);
    // two big triangles facing the camera at az 0, one at y = -0.3 (nearer the eye) and one at y = +0.3, overlapping at the middle
    const auto tri = [](float y, Rgba c) {
        Vertex a, b, cc;
        a.p = {-0.5f, y, -0.5f};
        b.p = {0.5f, y, -0.5f};
        cc.p = {0.0f, y, 0.6f};
        a.c = b.c = cc.c = c;
        return std::array<Vertex, 3>{a, b, cc};
    };
    const auto nearTri = tri(-0.3f, kRed), farTri = tri(0.3f, kBlue);
    s.begin(f.px.data(), f.w, f.h, kBlack, cam(0.0f, 4.0f));
    s.triangle(nearTri[0], nearTri[1], nearTri[2], false);
    s.triangle(farTri[0], farTri[1], farTri[2], false);
    const Rgba mid1 = f.at(100, 110);
    Frame g(200, 200);
    s.begin(g.px.data(), g.w, g.h, kBlack, cam(0.0f, 4.0f));
    s.triangle(farTri[0], farTri[1], farTri[2], false);
    s.triangle(nearTri[0], nearTri[1], nearTri[2], false);
    const Rgba mid2 = g.at(100, 110);
    CHECK(mid1.r == 255 && mid1.b == 0);   // the near triangle hides the far one, drawn first or last
    CHECK(mid2.r == 255 && mid2.b == 0);
    CHECK(sameBytes(f, g));                // and the picture does not depend on the order
    CHECK(s.depthAt(100, 110) > 0.0f);
    CHECK(s.depthAt(2, 2) == 0.0f);        // nothing there

    // a line behind the near triangle is hidden by it, and the same line in front of it shows
    Frame h(200, 200);
    s.begin(h.px.data(), h.w, h.h, kBlack, cam(0.0f, 4.0f));
    s.triangle(nearTri[0], nearTri[1], nearTri[2], false);
    s.line({-0.1f, 0.6f, 0.0f}, {0.1f, 0.6f, 0.0f}, kGreen, 3.0f);   // behind the red triangle, over its middle
    int hidden = 0;
    for (int x = 85; x < 115; ++x) { for (int y = 60; y < 140; ++y) { hidden += h.at(x, y).g > 0 ? 1 : 0; } }
    CHECK(hidden == 0);
    Frame k(200, 200);
    s.begin(k.px.data(), k.w, k.h, kBlack, cam(0.0f, 4.0f));
    s.triangle(farTri[0], farTri[1], farTri[2], false);
    s.line({-0.1f, -0.6f, 0.0f}, {0.1f, -0.6f, 0.0f}, kGreen, 3.0f);   // in front of the blue triangle
    int greens = 0;
    for (int x = 85; x < 115; ++x) { for (int y = 60; y < 140; ++y) { greens += k.at(x, y).g > 200 ? 1 : 0; } }
    CHECK(greens > 20);
    CHECK(s.occluded({0.0f, 0.6f, 0.0f}) == true);   // the far side of the blue triangle's plane? the point sits behind it
}

// ---- 3. dashed line coverage ----
void testDashes() {
    Scene s;
    const auto row = [&](Dash d, float width) {
        Frame f(300, 100);
        s.begin(f.px.data(), f.w, f.h, kBlack, cam(0.0f, 4.0f));
        // a straight horizontal line across the picture's middle (an x-parallel line at the target's height)
        s.line({-4.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 0.0f}, kWhite, width, d);
        int lit = 0, run = 0, longest = 0, transitions = 0;
        bool prev = false;
        const int y = 50;
        for (int x = 20; x < 280; ++x) {
            const bool on = f.at(x, y).r > 128;
            lit += on ? 1 : 0;
            run = on ? run + 1 : 0;
            longest = std::max(longest, run);
            transitions += (on != prev) ? 1 : 0;
            prev = on;
        }
        struct R { int lit, longest, transitions; };
        return R{lit, longest, transitions};
    };
    const auto solid = row(Dash::Solid, 1.5f), dashed = row(Dash::Dashed, 1.5f), dotted = row(Dash::Dotted, 1.5f);
    CHECK(solid.lit >= 255);                 // every pixel of the 260 looked at
    CHECK(solid.transitions <= 2);
    CHECK(dashed.lit > 110 && dashed.lit < 180);   // about 7 of every 12 pixels
    CHECK(dashed.longest >= 6 && dashed.longest <= 9);
    CHECK(dashed.transitions >= 30);         // a gap every 12 px over 260 px: 21 periods
    CHECK(dotted.lit > 40 && dotted.lit < 110);
    CHECK(dotted.longest <= 3);
    CHECK(dotted.lit < dashed.lit);
    // a wider dash is longer: the pattern scales with the line
    const auto wide = row(Dash::Dashed, 4.5f);
    CHECK(wide.longest > dashed.longest);
}

// ---- 4. the sphere's silhouette ----
void testSphere() {
    Scene s;
    Frame f(200, 200);
    s.begin(f.px.data(), f.w, f.h, kBlack, cam(20.0f, 25.0f));
    const Mesh m = buildSphere(1.0f, 64, 32, kWhite);
    CHECK(m.v.size() == 65u * 33u);
    CHECK(m.idx.size() == 64u * 32u * 6u);
    s.setLight({0.3f, -0.4f, 0.8f}, 1.0f);   // no shading: a plain white disc
    const std::size_t drawn = s.drawMesh(m, true);
    CHECK(drawn > 3000);
    // radius 1 at the fit distance: the silhouette is the circle of the shorter side's half (100 px) about the centre
    const double area = static_cast<double>(f.litCount());
    CHECK_NEAR(area, 3.14159265 * 100.0 * 100.0, 3.14159265 * 100.0 * 100.0 * 0.02);
    CHECK(f.lit(100, 100));
    CHECK(!f.lit(1, 1) && !f.lit(198, 1) && !f.lit(1, 198) && !f.lit(198, 198));   // the corners are empty
    int outside = 0;
    for (int y = 0; y < f.h; ++y) { for (int x = 0; x < f.w; ++x) { const double r = std::hypot(x + 0.5 - 100.0, y + 0.5 - 100.0); outside += (r > 102.5 && f.lit(x, y)) ? 1 : 0; } }
    CHECK(outside == 0);
    int inside = 0, total = 0;
    for (int y = 0; y < f.h; ++y) { for (int x = 0; x < f.w; ++x) { const double r = std::hypot(x + 0.5 - 100.0, y + 0.5 - 100.0); if (r < 96.0) { ++total; inside += f.lit(x, y) ? 1 : 0; } } }
    CHECK(inside == total);   // no hole in the disc: the poles and the seam are closed
    // lit meshes shade: with a low ambient the side away from the light is darker than the side toward it
    Frame g(200, 200);
    s.begin(g.px.data(), g.w, g.h, kBlack, cam(0.0f, 10.0f));
    s.setLight({0.0f, -1.0f, 0.0f}, 0.2f);   // the light is at the eye's side: the middle of the disc faces it
    s.drawMesh(m, true);
    CHECK(g.at(100, 100).r > g.at(100 + 90, 100).r);
    // a bound texture colours it: a two-colour picture, west half red, east half blue
    std::vector<Rgba> tex(64 * 32);
    for (int y = 0; y < 32; ++y) { for (int x = 0; x < 64; ++x) { tex[static_cast<std::size_t>(y * 64 + x)] = x < 16 ? kRed : kBlue; } }
    const Texture t{64, 32, tex.data()};
    Frame h(200, 200);
    s.begin(h.px.data(), h.w, h.h, kBlack, cam(0.0f, 10.0f));
    s.setLight({0.0f, -1.0f, 0.0f}, 1.0f);
    s.bindTexture(&t);
    s.drawMesh(m, true);
    s.bindTexture(nullptr);
    // at azimuth 0 the eye is on the -y side, so the longitude facing it is -90 = u 0.25, the picture's red/blue boundary; west of it (longitudes under -90) is red, east of it blue
    CHECK(h.at(100 - 60, 100).r > 200 && h.at(100 - 60, 100).b < 60);
    CHECK(h.at(100 + 60, 100).b > 200 && h.at(100 + 60, 100).r < 60);
    CHECK(sampleTexture(t, 0.1f, 0.5f).r == 255);
    CHECK(sampleTexture(t, 0.75f, 0.5f).b == 255);
    CHECK(sampleTexture(t, 1.1f, 0.5f).r == 255);    // u wraps
}

// ---- 5. determinism ----
Mesh surface(int cols, int rows, float phase) {
    std::vector<float> v(static_cast<std::size_t>(cols * rows));
    for (int r = 0; r < rows; ++r) { for (int c = 0; c < cols; ++c) { v[static_cast<std::size_t>(r * cols + c)] = std::sin(0.31f * static_cast<float>(c) + phase) * std::cos(0.23f * static_cast<float>(r)) * 0.5f + 0.5f; } }
    const Ramp ramp = Ramp::heat();
    HeightFieldSpec spec;
    spec.cols = cols;
    spec.rows = rows;
    spec.values = v.data();
    spec.origin = {-1.0f, -1.0f, 0.0f};
    spec.sx = 2.0f;
    spec.sy = 2.0f;
    spec.zScale = 0.8f;
    spec.ramp = &ramp;
    return buildHeightField(spec);
}

void drawSample(Scene& s, Frame& f, const Camera& c) {
    s.begin(f.px.data(), f.w, f.h, Rgba{10, 12, 18, 255}, c);
    BoxSpec box;
    box.lo = {-1.0f, -1.0f, 0.0f};
    box.hi = {1.0f, 1.0f, 0.8f};
    drawBoxBack(s, box);
    s.setLight({0.3f, -0.4f, 0.8f}, 0.5f);
    s.drawMesh(surface(40, 30, 0.0f), true);
    drawBoxFront(s, box);
    s.polyline({{-1, -1, 0.2f}, {0, 0, 0.8f}, {1, 1, 0.1f}}, {}, kWhite, 2.0f, Dash::Dashed);
    s.marker({0, 0, 0.8f}, Marker::Star, kWhite, 12.0f);
    s.point({0.5f, 0.5f, 0.3f}, kGreen, 3.0f);
}

void testDeterminism() {
    Scene s1, s2;
    Frame a(320, 220), b(320, 220), c(320, 220);
    const Camera cm = cam(-58.0f, 32.0f);
    drawSample(s1, a, cm);
    drawSample(s2, b, cm);
    CHECK(sameBytes(a, b));                   // two scenes, the same pictures
    Frame other(320, 220);
    drawSample(s1, other, cam(10.0f, 50.0f));   // the first scene is reused for something else...
    drawSample(s1, c, cm);
    CHECK(sameBytes(a, c));                   // ...and gives the first picture again
    CHECK(!sameBytes(a, other));
    CHECK(a.litCount() > 5000);
}

// ---- 6. the builders ----
void testBuilders() {
    // holes: a NaN removes the quads around it
    std::vector<float> v(9, 0.5f);
    HeightFieldSpec spec;
    spec.cols = 3;
    spec.rows = 3;
    spec.values = v.data();
    CHECK(buildHeightField(spec).idx.size() == 4u * 6u);
    v[4] = std::nanf("");   // the middle point is in all four quads
    CHECK(buildHeightField(spec).idx.empty());
    v[4] = 0.5f;
    v[0] = std::nanf("");   // a corner is in one
    CHECK(buildHeightField(spec).idx.size() == 3u * 6u);
    // z follows the value between lo and hi, clamped; the colour follows the ramp
    v.assign(9, 0.0f);
    v[8] = 10.0f;
    spec.lo = 0.0f;
    spec.hi = 1.0f;
    spec.zScale = 2.0f;
    const Mesh m = buildHeightField(spec);
    CHECK_NEAR(m.v[8].p.z, 2.0, 1e-6);   // clamped to the top
    CHECK_NEAR(m.v[0].p.z, 0.0, 1e-6);
    CHECK(m.v[8].c.r > m.v[0].c.r);
    CHECK_NEAR(m.v[8].p.x, 1.0, 1e-6);
    CHECK_NEAR(m.v[8].p.y, 1.0, 1e-6);
    spec.cols = 1;
    CHECK(buildHeightField(spec).idx.empty());   // not a grid
    // the great circle: ends on the sphere, the middle lifted, all points at least the radius
    const auto arc = greatCircleArc(51.5, -0.1, 40.7, -74.0, 2.0f, 0.2f, 32);
    CHECK(arc.size() == 33u);
    CHECK_NEAR(length(arc.front()), 2.0, 1e-4);
    CHECK_NEAR(length(arc.back()), 2.0, 1e-4);
    CHECK_NEAR(length(arc[16]), 2.0 * 1.2, 1e-3);
    const Vec3 london = latLonToVec(51.5, -0.1, 2.0f);
    CHECK_NEAR(arc.front().x, london.x, 1e-4);
    CHECK_NEAR(latLonToVec(90.0, 0.0, 1.0f).z, 1.0, 1e-6);
    CHECK_NEAR(latLonToVec(0.0, 90.0, 1.0f).y, 1.0, 1e-6);
    // a degenerate arc (the same point twice) does not produce NaNs
    const auto same = greatCircleArc(10, 10, 10, 10, 1.0f, 0.1f, 4);
    for (const Vec3& p : same) { CHECK(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)); }
    // the ramps: their ends and monotone-ish brightness for the single-hue one
    const Ramp heat = Ramp::heat();
    CHECK(heat.at(0.0f).b > heat.at(0.0f).r);          // the deep navy
    CHECK(heat.at(1.0f).r == 255 && heat.at(1.0f).g == 255);
    CHECK(heat.at(std::nanf("")).r == heat.at(0.0f).r);
    CHECK(Ramp::viridis().at(1.0f).g > 200 && Ramp::viridis().at(0.0f).g < 20);
    CHECK(Ramp::altitude().at(0.0f).b > Ramp::altitude().at(1.0f).b);
    CHECK(Ramp::timeTunnel().at(0.0f).g > 200 && Ramp::timeTunnel().at(1.0f).g < 20);
    const Ramp blue = Ramp::blueHeight();
    CHECK(blue.at(1.0f).b > blue.at(0.0f).b && blue.at(1.0f).r > blue.at(0.0f).r);
    CHECK(Ramp().at(0.5f).a == 0);   // an unset ramp is transparent black, and never reads outside its table
    CHECK(mix(kBlack, kWhite, 0.5f).r >= 127 && mix(kBlack, kWhite, 0.5f).r <= 128);
}

// ---- 7. the near plane, labels, the box ----
void testClipLabelsBox() {
    Scene s;
    Frame f(300, 200);
    // zoomed right in, the eye is inside the scene: a floor under it crosses the near plane and must still draw without a crash or a flood
    Camera inside = cam(0.0f, 40.0f, 5.0f);
    s.begin(f.px.data(), f.w, f.h, kBlack, inside);
    Vertex a, b, c;
    a.p = {-5.0f, -5.0f, -0.1f};
    b.p = {5.0f, -5.0f, -0.1f};
    c.p = {0.0f, 6.0f, -0.1f};
    a.c = b.c = c.c = kRed;
    s.triangle(a, b, c, false);
    CHECK(f.litCount() > 1000);
    CHECK(f.litCount() <= static_cast<std::size_t>(f.w * f.h));
    // a line that passes behind the eye on its way is clipped, not mirrored
    Frame g(300, 200);
    s.begin(g.px.data(), g.w, g.h, kBlack, cam(0.0f, 30.0f));
    const Vec3 eye = eyePosition(cam(0.0f, 30.0f));
    s.line(eye * 2.0f, eye * 3.0f, kWhite, 2.0f);   // wholly behind the eye
    CHECK(g.litCount() == 0);
    s.line({0, 0, 0}, eye * 3.0f + Vec3{2.0f, 0.0f, 0.0f}, kWhite, 2.0f);   // from the target to a point behind the eye (off the line of sight): the part in front still draws
    CHECK(g.litCount() > 20);
    // labels: in front and on the picture is placed as a fraction; behind the eye or hidden behind a drawn surface is not
    Frame h(300, 200);
    s.begin(h.px.data(), h.w, h.h, kBlack, cam(0.0f, 30.0f));
    std::vector<Label3D> labels;
    CHECK(addLabel(s, labels, {0, 0, 0}, "mid", 0xffffffff, 0, 2, true));
    CHECK(labels.size() == 1u);
    CHECK_NEAR(labels[0].x, 0.5, 1e-4);
    CHECK_NEAR(labels[0].y, 0.5, 1e-4);
    CHECK(labels[0].text == "mid");
    CHECK(!addLabel(s, labels, eye * 2.0f, "behind", 0xffffffff, 0, 2, true));
    CHECK(!addLabel(s, labels, {50.0f, 0, 0}, "off", 0xffffffff, 0, 2, true));
    CHECK(labels.size() == 1u);
    // a screen the surface covers hides a label behind it: a big floor under the target, the label below it
    Vertex f1, f2, f3, f4;
    f1.p = {-3, -3, 0.0f}; f2.p = {3, -3, 0.0f}; f3.p = {3, 3, 0.0f}; f4.p = {-3, 3, 0.0f};
    f1.c = f2.c = f3.c = f4.c = kBlue;
    s.triangle(f1, f2, f3, false);
    s.triangle(f1, f3, f4, false);
    CHECK(!addLabel(s, labels, {0, 0, -0.5f}, "under", 0xffffffff, 0, 2, true));    // hidden under the floor as seen from above
    CHECK(addLabel(s, labels, {0, 0, -0.5f}, "under", 0xffffffff, 0, 2, false));    // the caller may keep it anyway
    CHECK(addLabel(s, labels, {0, 0, 0.5f}, "over", 0xffffffff, 0, 2, true));
    // the box: its tick counts, the walls turn with the camera (the back wall is the far one)
    BoxSpec box;
    box.lo = {0, 0, 0};
    box.hi = {10, 4, 2};
    box.xTicks = 6;
    box.yTicks = 3;
    box.zTicks = 5;
    Frame k(300, 200);
    s.begin(k.px.data(), k.w, k.h, kBlack, cam(-58.0f, 32.0f));
    const BoxLayout lay = drawBoxBack(s, box);
    CHECK(lay.ticks.size() == 6u + 3u + 5u);
    int per[3] = {0, 0, 0};
    for (const AxisTick& t : lay.ticks) { ++per[t.axis]; }
    CHECK(per[0] == 6 && per[1] == 3 && per[2] == 5);
    CHECK(k.litCount() > 500);
    // the tick for fraction 0 on the x axis sits at the box's low x on the near side of y
    const AxisTick* x0 = nullptr;
    for (const AxisTick& t : lay.ticks) { if (t.axis == 0 && t.frac == 0.0f) { x0 = &t; } }
    CHECK(x0 != nullptr);
    CHECK_NEAR(x0->pos.x, 0.0, 1e-6);
    // from the other side the walls are the other pair: the picture differs
    Frame k2(300, 200);
    s.begin(k2.px.data(), k2.w, k2.h, kBlack, cam(122.0f, 32.0f));
    drawBoxBack(s, box);
    CHECK(!sameBytes(k, k2));
    // an empty size and a null buffer are survived
    s.begin(nullptr, 0, 0, kBlack, cam(0, 30));
    s.line({0, 0, 0}, {1, 1, 1}, kWhite, 1.0f);
    s.drawMesh(surface(4, 4, 0.0f), false);
    s.marker({0, 0, 0}, Marker::Dot, kWhite, 5.0f);
    CHECK(s.width() == 0 && s.height() == 0);
}

// ---- 8. the drag and wheel rules ----
void testDragZoom() {
    Camera c = cam(0.0f, 30.0f);
    const Camera r = dragged(c, 100.0f, 0.0f);
    CHECK_NEAR(r.azimuthDeg, -100.0 * kDegPerPixel, 1e-4);   // drag right: the scene turns right, the eye goes left
    CHECK_NEAR(r.elevationDeg, 30.0, 1e-6);
    const Camera d = dragged(c, 0.0f, 20.0f);
    CHECK_NEAR(d.elevationDeg, 30.0 + 20.0 * kDegPerPixel, 1e-4);
    CHECK_NEAR(dragged(c, 0.0f, 1000.0f).elevationDeg, kMaxElevationDeg, 1e-6);
    CHECK_NEAR(dragged(c, 0.0f, -1000.0f).elevationDeg, kMinElevationDeg, 1e-6);
    CHECK(zoomed(c, 1).zoom > 1.0f);
    CHECK(zoomed(c, -1).zoom < 1.0f);
    CHECK_NEAR(zoomed(zoomed(c, 3), -3).zoom, 1.0, 1e-4);
    CHECK_NEAR(zoomed(c, 1000).zoom, kMaxZoom, 1e-6);
    CHECK_NEAR(zoomed(c, -1000).zoom, kMinZoom, 1e-6);
}

// ---- 9. the frame time (printed, not asserted) ----
void measure() {
    const int w = 1100, h = 520;   // the receiver view's well at the design's 1440 x 900 window
    std::vector<Rgba> px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    Scene s;
    const Mesh surf = surface(160, 96, 0.0f);
    const Camera cm = cam(-58.0f, 32.0f);
    BoxSpec box;
    box.lo = {-1.0f, -1.0f, 0.0f};
    box.hi = {1.0f, 1.0f, 0.8f};
    double best = 1e9, sum = 0.0, split[4] = {0, 0, 0, 0};
    const int N = 20;
    for (int i = 0; i < N; ++i) {
        const double t0 = nowMs();
        s.begin(px.data(), w, h, Rgba{10, 12, 18, 255}, cm);
        const double t1 = nowMs();
        drawBoxBack(s, box);
        const double t2 = nowMs();
        s.setLight({0.3f, -0.4f, 0.8f}, 0.5f);
        s.drawMesh(surf, true);
        const double t3 = nowMs();
        drawBoxFront(s, box);
        const double dt = nowMs() - t0;
        if (dt < best) { best = dt; split[0] = t1 - t0; split[1] = t2 - t1; split[2] = t3 - t2; split[3] = nowMs() - t3; }
        sum += dt;
    }
    std::printf("scene3d frame: %d x %d, 160 x 96 height field (%zu triangles) + box: best %.2f ms, mean %.2f ms over %d frames\n", w, h, surf.idx.size() / 3, best, sum / N, N);
    std::printf("scene3d frame split (best frame): clear %.2f ms, box back %.2f ms, surface %.2f ms, box front %.2f ms; %zu px written\n", split[0], split[1], split[2], split[3], s.pixelsWritten());
    // the sphere, textured, filling the same well
    std::vector<Rgba> tex(512 * 256, Rgba{40, 90, 160, 255});
    const Texture t{512, 256, tex.data()};
    const Mesh sph = buildSphere(1.0f, 96, 48, kWhite);
    double sb = 1e9;
    for (int i = 0; i < 10; ++i) {
        const double t0 = nowMs();
        s.begin(px.data(), w, h, Rgba{10, 12, 18, 255}, cam(20.0f, 25.0f));
        s.setLight({0.3f, -0.4f, 0.8f}, 0.5f);
        s.bindTexture(&t);
        s.drawMesh(sph, true);
        s.bindTexture(nullptr);
        sb = std::min(sb, nowMs() - t0);
    }
    std::printf("scene3d frame: %d x %d, textured sphere 96 x 48 (%zu triangles): best %.2f ms\n", w, h, sph.idx.size() / 3, sb);
}

}  // namespace

int main() {
    testProjection();
    testDepthOrder();
    testDashes();
    testSphere();
    testDeterminism();
    testBuilders();
    testClipLabelsBox();
    testDragZoom();
    measure();
    return testSummary("test_slint_scene3d");
}
