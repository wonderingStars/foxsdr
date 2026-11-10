// The 3D AIRSPACE (src/slint/radar3d_logic.hpp and radar3d_compositor.hpp, docs 6.64) against synthetic aircraft: the polar-to-east/north conversion, the flight levels and their words, the
// ALTITUDE SCALE's heights, the label order, and the painted scene - an aircraft stands at its altitude (the label's anchor is where the renderer projects it), the colour follows the level, an
// unknown altitude is drawn on the ground and said so, the trails, the selection and the emergency mark appear only when asked, the rings and antenna are there with no aircraft at all.
// No Slint, no engine, no window.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "radar3d_compositor.hpp"
#include "radar3d_logic.hpp"
#include "test_check.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace foxsdr::slintview3d;
namespace s3 = foxsdr::scene3d;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

Radar3dAircraft ac(const char* id, const char* label, double e, double n, double altFt, double course = kNaN) {
    Radar3dAircraft a;
    a.id = id;
    a.label = label;
    a.eastNm = e;
    a.northNm = n;
    a.altFt = altFt;
    a.courseDeg = course;
    return a;
}

std::size_t countWhere(const std::vector<s3::Rgba>& px, bool (*pred)(const s3::Rgba&)) {
    std::size_t n = 0;
    for (const s3::Rgba& p : px) { n += (p.a > 150 && pred(p)) ? 1 : 0; }
    return n;
}
std::size_t opaque(const std::vector<s3::Rgba>& px) { std::size_t n = 0; for (const auto& p : px) { n += p.a > 0 ? 1 : 0; } return n; }
bool warm(const s3::Rgba& p) { return p.r > 215 && p.g > 110 && p.b < 130; }          // the top of the altitude ramp
bool grey(const s3::Rgba& p) { return std::abs(p.r - 150) < 8 && std::abs(p.g - 160) < 8 && std::abs(p.b - 176) < 8; }   // the no-altitude grey
bool white(const s3::Rgba& p) { return p.r > 240 && p.g > 240 && p.b > 244; }
bool red(const s3::Rgba& p) { return p.r > 245 && p.g > 70 && p.g < 95 && p.b > 70 && p.b < 95; }

Radar3dInput base() {
    Radar3dInput in;
    in.rangeNm = 100.0;
    in.camera = radar3dDefaultCamera();
    return in;
}

const s3::Label3D* find(const Radar3dResult& r, const std::string& text) {
    for (const s3::Label3D& l : r.labels) { if (l.text == text) { return &l; } }
    return nullptr;
}

void testLogic() {
    double e = 0, n = 0;
    polarToEastNorth(100.0, 90.0, e, n);
    CHECK_NEAR(e, 100.0, 1e-9);
    CHECK_NEAR(n, 0.0, 1e-9);
    polarToEastNorth(50.0, 0.0, e, n);
    CHECK_NEAR(e, 0.0, 1e-9);
    CHECK_NEAR(n, 50.0, 1e-9);
    polarToEastNorth(100.0, 225.0, e, n);
    CHECK_NEAR(e, -70.7106781, 1e-6);
    CHECK_NEAR(n, -70.7106781, 1e-6);
    polarToEastNorth(0.0, kNaN, e, n);   // overhead: no bearing
    CHECK(e == 0.0 && n == 0.0);
    polarToEastNorth(kNaN, 10.0, e, n);
    CHECK(e == 0.0 && n == 0.0);
    CHECK(flightLevelText(31000.0, "NO ALT") == "FL310");
    CHECK(flightLevelText(5000.0, "NO ALT") == "FL050");
    CHECK(flightLevelText(45000.0, "NO ALT") == "FL450");
    CHECK(flightLevelText(-200.0, "NO ALT") == "FL000");
    CHECK(flightLevelText(kNaN, "NO ALT") == "NO ALT");
    CHECK(flightLevelText(kNaN, "PAS D'ALT") == "PAS D'ALT");
    CHECK_NEAR(flightLevel(31000.0), 310.0, 1e-9);
    CHECK(std::isnan(flightLevel(kNaN)));
    CHECK_NEAR(sceneHeight(0.0, 1), 0.0, 1e-9);
    CHECK_NEAR(sceneHeight(40000.0, 1), kRadar3dHeightAtX1, 1e-9);
    CHECK_NEAR(sceneHeight(40000.0, 2), 2.0 * kRadar3dHeightAtX1, 1e-9);
    CHECK_NEAR(sceneHeight(40000.0, 4), 4.0 * kRadar3dHeightAtX1, 1e-9);
    CHECK_NEAR(sceneHeight(20000.0, 1), kRadar3dHeightAtX1 / 2.0, 1e-9);
    CHECK_NEAR(sceneHeight(60000.0, 1), kRadar3dHeightAtX1, 1e-9);   // above the key: the top
    CHECK_NEAR(sceneHeight(-500.0, 1), 0.0, 1e-9);
    CHECK_NEAR(sceneHeight(kNaN, 4), 0.0, 1e-9);                      // unknown: the ground
    CHECK(scaleFactorOfIndex(0) == 1 && scaleFactorOfIndex(1) == 2 && scaleFactorOfIndex(2) == 4);
    CHECK(scaleFactorOfIndex(-3) == 1 && scaleFactorOfIndex(9) == 4);
    CHECK_NEAR(radar3dBoxHeight(1), 2.0 * radar3dBoxHeight(0), 1e-9);
    // the label order: the selected first, then the nearest, at most N
    std::vector<Radar3dAircraft> v = {ac("a", "A", 80, 0, 1), ac("b", "B", 10, 0, 1), ac("c", "C", 40, 0, 1), ac("d", "D", 20, 0, 1)};
    v[0].selected = true;
    const std::vector<std::size_t> o = labelOrder(v, 3);
    CHECK((o == std::vector<std::size_t>{0, 1, 3}));
    CHECK(labelOrder(v, 10).size() == 4u);
    CHECK(labelOrder({}, 3).empty());
    CHECK(labelOrder(v, 0).empty());
}

void testPaint() {
    s3::Scene scene;
    std::vector<s3::Rgba> px(420 * 300);

    // no aircraft: the ground and the box, and the words of the box
    Radar3dInput in = base();
    Radar3dResult r0 = paintRadar3d(scene, px.data(), 420, 300, in);
    CHECK(r0.aircraft == 0u);
    CHECK(opaque(px) > 1500);
    CHECK(r0.labels.size() == 18u);   // 15 ticks and 3 captions
    CHECK(find(r0, "-100") != nullptr && find(r0, "100") != nullptr && find(r0, "0") != nullptr);
    CHECK(find(r0, "400") != nullptr);
    CHECK(find(r0, "NM E") != nullptr && find(r0, "NM N") != nullptr && find(r0, "FL") != nullptr);
    CHECK(countWhere(px, [](const s3::Rgba& p) { return p.r > 240 && p.g > 100 && p.g < 140 && p.b > 205; }) > 5);   // the antenna's pink cross

    // three aircraft: each at its altitude, with its callsign and level
    in.aircraft = {ac("A1", "BAW123", 20, 10, 31000, 45), ac("A2", "RYR8KJ", -30, 40, 24000, 200), ac("A3", "EZY45", 50, -20, 12000, 300)};
    std::vector<s3::Rgba> p1(420 * 300);
    Radar3dResult r1 = paintRadar3d(scene, p1.data(), 420, 300, in);
    CHECK(r1.aircraft == 3u && r1.onGround == 0u);
    CHECK(r1.labels.size() == 21u);
    const s3::Label3D* l1 = find(r1, "BAW123 FL310");
    CHECK(l1 != nullptr);
    CHECK(find(r1, "RYR8KJ FL240") != nullptr && find(r1, "EZY45 FL120") != nullptr);
    if (l1 != nullptr) {
        // the label stands at the aircraft: the renderer projects (0.2, 0.1, height of FL310) to the label's anchor (the label is 9 px to its right)
        const s3::ScreenPoint sp = scene.project({0.2f, 0.1f, static_cast<float>(sceneHeight(31000.0, 1))});
        CHECK_NEAR(l1->x * 420.0f, sp.x + 9.0f, 0.6);
        CHECK_NEAR(l1->y * 300.0f, sp.y, 0.6);
        CHECK(l1->align == 1);
        CHECK(l1->kind == 2);
    }
    // a higher aircraft is higher on the screen than the same position lower
    Radar3dInput hi = base();
    hi.aircraft = {ac("H", "HIGH", 0, 0, 38000), ac("L", "LOW", 0, 0, 5000)};
    std::vector<s3::Rgba> p2(420 * 300);
    Radar3dResult r2 = paintRadar3d(scene, p2.data(), 420, 300, hi);
    const s3::Label3D* high = find(r2, "HIGH FL380");
    const s3::Label3D* low = find(r2, "LOW FL050");
    CHECK(high != nullptr && low != nullptr);
    if (high != nullptr && low != nullptr) { CHECK(high->y < low->y); }
    // the colour follows the level: FL390 is warm, FL10 is not
    Radar3dInput top = base();
    top.aircraft = {ac("T", "TOP", 10, 10, 39000)};
    std::vector<s3::Rgba> p3(420 * 300), p4(420 * 300);
    paintRadar3d(scene, p3.data(), 420, 300, top);
    Radar3dInput bot = base();
    bot.aircraft = {ac("B", "BOT", 10, 10, 1000)};
    paintRadar3d(scene, p4.data(), 420, 300, bot);
    CHECK(countWhere(p3, warm) > 20);
    CHECK(countWhere(p4, warm) == 0u);

    // ALTITUDE SCALE: the same aircraft stands higher at x2 and x4
    Radar3dInput s1 = base(), s2 = base(), s4 = base();
    s1.aircraft = s2.aircraft = s4.aircraft = {ac("S", "SAME", 0, 0, 30000)};
    s2.scaleIndex = 1;
    s4.scaleIndex = 2;
    std::vector<s3::Rgba> q(420 * 300);
    const auto yOf = [&](const Radar3dInput& i) { const Radar3dResult rr = paintRadar3d(scene, q.data(), 420, 300, i); const s3::Label3D* l = find(rr, "SAME FL300"); return l != nullptr ? l->y : 99.0f; };
    const float y1 = yOf(s1);
    const float y2 = yOf(s2);
    const float y4 = yOf(s4);
    CHECK(y1 < 90.0f && y2 < 90.0f && y4 < 90.0f);
    CHECK(y2 != y1 && y4 != y2);   // the scene changed with the scale (the box, and with it the framing)

    // an unknown altitude is on the ground, said so, drawn grey
    Radar3dInput na = base();
    na.aircraft = {ac("N", "NOALT1", 30, 30, kNaN)};
    std::vector<s3::Rgba> p5(420 * 300);
    Radar3dResult r5 = paintRadar3d(scene, p5.data(), 420, 300, na);
    CHECK(r5.aircraft == 1u && r5.onGround == 1u);
    CHECK(find(r5, "NOALT1 NO ALT") != nullptr);
    CHECK(countWhere(p5, grey) > 8);
    const s3::ScreenPoint gp = scene.project({0.3f, 0.3f, 0.0f});
    const s3::Label3D* nl = find(r5, "NOALT1 NO ALT");
    if (nl != nullptr) { CHECK_NEAR(nl->y * 300.0f, gp.y, 0.6); }
    // ... in the translated words too
    na.noAlt = "PAS D'ALT";
    CHECK(find(paintRadar3d(scene, p5.data(), 420, 300, na), "NOALT1 PAS D'ALT") != nullptr);

    // the trail: through the air, only when the scope's TRAILS are on, the ribbon wider than the line
    Radar3dInput tr = base();
    Radar3dAircraft t = ac("TR", "TRAIL1", 40, 0, 30000, 90);
    for (int i = 0; i < 10; ++i) { t.trail.push_back({-20.0 + 6.0 * i, 0.0, 28000.0 + 200.0 * i}); }
    tr.aircraft = {t};
    tr.trails = 0;
    std::vector<s3::Rgba> off(420 * 300), line(420 * 300), ribbon(420 * 300);
    CHECK(paintRadar3d(scene, off.data(), 420, 300, tr).trailSegments == 0u);
    tr.trails = 1;
    CHECK(paintRadar3d(scene, line.data(), 420, 300, tr).trailSegments == 9u);
    tr.trails = 2;
    paintRadar3d(scene, ribbon.data(), 420, 300, tr);
    CHECK(opaque(line) > opaque(off));
    CHECK(opaque(ribbon) > opaque(line));
    // a trail point far outside the range is left out, not drawn across the picture
    tr.trails = 1;
    tr.aircraft[0].trail.push_back({900.0, 900.0, 30000.0});
    CHECK(paintRadar3d(scene, off.data(), 420, 300, tr).trailSegments == 9u);

    // the selection and the emergency are marked when they are, and not otherwise
    Radar3dInput sel = base();
    sel.aircraft = {ac("S1", "SELCT1", 10, 10, 20000)};
    std::vector<s3::Rgba> a0(420 * 300), a1(420 * 300), a2(420 * 300);
    paintRadar3d(scene, a0.data(), 420, 300, sel);
    sel.aircraft[0].selected = true;
    paintRadar3d(scene, a1.data(), 420, 300, sel);
    sel.aircraft[0].selected = false;
    sel.aircraft[0].emergency = true;
    paintRadar3d(scene, a2.data(), 420, 300, sel);
    CHECK(countWhere(a0, white) == 0u && countWhere(a1, white) > 8u);
    CHECK(countWhere(a0, red) == 0u && countWhere(a2, red) > 8u);

    // an aircraft just past the rim sits on the rim; none of this crashes; a crowd is labelled by the nearest N only
    Radar3dInput crowd = base();
    for (int i = 0; i < 40; ++i) { crowd.aircraft.push_back(ac(("c" + std::to_string(i)).c_str(), ("CC" + std::to_string(100 + i)).c_str(), -90.0 + 4.5 * i, -90.0 + 4.5 * i, 30000.0 - 500.0 * i)); }
    crowd.aircraft.push_back(ac("far", "FAR1", 500.0, 0.0, 20000));
    std::vector<s3::Rgba> pc(420 * 300);
    const Radar3dResult rc = paintRadar3d(scene, pc.data(), 420, 300, crowd);
    CHECK(rc.aircraft == 41u);
    std::size_t labelled = 0;
    for (const s3::Label3D& l : rc.labels) { labelled += l.kind == 2 ? 1 : 0; }
    CHECK(labelled == crowd.maxLabels);
    crowd.maxLabels = 3;
    labelled = 0;
    for (const s3::Label3D& l : paintRadar3d(scene, pc.data(), 420, 300, crowd).labels) { labelled += l.kind == 2 ? 1 : 0; }
    CHECK(labelled == 3u);

    // determinism, and the camera (TILT) moves the picture
    std::vector<s3::Rgba> d1(420 * 300), d2(420 * 300), d3(420 * 300);
    Radar3dInput det = base();
    det.aircraft = in.aircraft;
    paintRadar3d(scene, d1.data(), 420, 300, det);
    paintRadar3d(scene, d2.data(), 420, 300, det);
    CHECK(std::memcmp(d1.data(), d2.data(), d1.size() * sizeof(s3::Rgba)) == 0);
    det.camera.elevationDeg = 60.0f;
    paintRadar3d(scene, d3.data(), 420, 300, det);
    CHECK(std::memcmp(d1.data(), d3.data(), d1.size() * sizeof(s3::Rgba)) != 0);
    // the ramp's ends
    CHECK(radar3dRamp().at(0.0f).b > radar3dRamp().at(1.0f).b);
    CHECK(radar3dRamp().at(1.0f).r > 240);
    // degenerate sizes are survived
    paintRadar3d(scene, nullptr, 0, 0, det);
    CHECK(scene.width() == 0);
}

}  // namespace

int main() {
    testLogic();
    testPaint();
    return testSummary("test_slint_host_radar3d");
}
