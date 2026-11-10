// The RADIOSONDE ASCENT TRACK (src/slint/sonde3d_logic.hpp and sonde3d_compositor.hpp, docs 6.64) against a synthetic flight: the history's keeping rules, the burst and the climb rate read from
// the samples, the local kilometres, and the painted track - the ascent in the altitude's colours, the star and the dashed pink descent only after a burst, the cyan ground track, the orange
// receiver, the tag at the balloon, the box's words. No Slint, no engine, no window.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "sonde3d_compositor.hpp"
#include "sonde3d_logic.hpp"
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

// a flight: climbing 5 m/s for `up` seconds east-north-east of (51.5, -0.1), then (burst) falling 12 m/s for `down` seconds, one sample every 5 s
std::vector<SondeSample> flight(int upS, int downS) {
    std::vector<SondeSample> v;
    double t = 0.0;
    for (int s = 0; s <= upS; s += 5, t += 5.0) { v.push_back({t, 51.5 + 0.00004 * s, -0.1 + 0.00008 * s, 100.0 + 5.0 * s}); }
    const double top = 100.0 + 5.0 * upS;
    for (int s = 5; s <= downS; s += 5, t += 5.0) { v.push_back({t, 51.5 + 0.00004 * (upS + s), -0.1 + 0.00008 * (upS + s), top - 12.0 * s}); }
    return v;
}

SondeInput inputOf(const std::vector<SondeSample>& fl) {
    SondeInput in;
    in.camera = sonde3dDefaultCamera();
    const SondeDerived d = deriveSonde(fl);
    in.burst = d.burst;
    in.burstIndex = d.maxIndex;
    for (const SondeSample& s : fl) {
        SondeLocal l;
        localKm(51.5, -0.1, s.lat, s.lon, l.eastKm, l.northKm);
        l.altKm = std::isfinite(s.altM) ? s.altM / 1000.0 : kNaN;
        in.track.push_back(l);
    }
    return in;
}

std::size_t opaque(const std::vector<s3::Rgba>& px) { std::size_t n = 0; for (const auto& p : px) { n += p.a > 0 ? 1 : 0; } return n; }
std::size_t countWhere(const std::vector<s3::Rgba>& px, bool (*pred)(const s3::Rgba&)) {
    std::size_t n = 0;
    for (const s3::Rgba& p : px) { n += (p.a > 150 && pred(p)) ? 1 : 0; }
    return n;
}
bool purple(const s3::Rgba& p) { return p.r > 50 && p.r < 110 && p.g < 40 && p.b > 60 && p.b < 130; }      // the ramp's low end (viridis 0.0: 68, 1, 84)
bool yellow(const s3::Rgba& p) { return p.r > 200 && p.g > 200 && p.b < 110; }                              // its high end
bool pink(const s3::Rgba& p) { return p.r > 240 && p.g > 90 && p.g < 125 && p.b > 135 && p.b < 170; }      // the descent
bool white(const s3::Rgba& p) { return p.r > 245 && p.g > 245 && p.b > 245; }
bool orange(const s3::Rgba& p) { return p.r > 235 && p.g > 150 && p.g < 180 && p.b < 60; }
bool cyan(const s3::Rgba& p) { return p.r > 70 && p.r < 110 && p.g > 160 && p.g < 195 && p.b > 190 && p.b < 220; }
const s3::Label3D* find(const SondeResult& r, const std::string& text) {
    for (const s3::Label3D& l : r.labels) { if (l.text == text) { return &l; } }
    return nullptr;
}

void testLogic() {
    // the history keeps what moved, ignores nonsense, and is bounded
    SondeHistory h;
    CHECK(h.add("S1", 0.0, 51.5, -0.1, 100.0));
    CHECK(!h.add("S1", 1.0, 51.5, -0.1, 100.5));                  // the same place and height, a second later: not kept
    CHECK(h.add("S1", 2.0, 51.5, -0.1, 104.0));                   // 4 m higher: kept
    CHECK(h.add("S1", 3.0, 51.5001, -0.1, 104.0));                // 11 m north: kept
    CHECK(h.add("S1", 20.0, 51.5001, -0.1, 104.0));              // not moved, but 17 s since the last kept one: kept
    CHECK(!h.add("S1", 10.0, 51.5002, -0.1, 110.0));              // older than the newest: refused
    CHECK(!h.add("", 5.0, 51.5, -0.1, 1.0));
    CHECK(!h.add("S2", 5.0, kNaN, -0.1, 1.0));
    CHECK(!h.add("S2", 5.0, 95.0, -0.1, 1.0));
    CHECK(h.of("S1") != nullptr && h.of("S1")->size() == 4u);
    CHECK(h.of("S2") == nullptr);
    CHECK(h.add("S3", 1.0, 51.0, 0.0, kNaN));                     // no altitude: a position without one
    CHECK(h.add("S3", 2.0, 51.0, 0.0, 200.0));                    // the altitude arriving is a change
    CHECK(std::isnan(h.of("S3")->front().altM) && h.of("S3")->back().altM == 200.0);
    // at most 16 balloons: the one heard longest ago goes
    SondeHistory many;
    for (int i = 0; i < 20; ++i) { many.add("B" + std::to_string(i), static_cast<double>(i), 50.0, 0.0, 1.0); }
    CHECK(many.trackCount() == kSondeMaxTracks);
    CHECK(many.of("B0") == nullptr && many.of("B19") != nullptr);
    // pruning: a balloon neither live nor heard for the age is forgotten
    SondeHistory pr;
    pr.add("OLD", 0.0, 50.0, 0.0, 1.0);
    pr.add("LIVE", 0.0, 50.0, 0.0, 1.0);
    pr.add("RECENT", 900.0, 50.0, 0.0, 1.0);
    pr.prune({"LIVE"}, 1000.0, 600.0);
    CHECK(pr.of("OLD") == nullptr && pr.of("LIVE") != nullptr && pr.of("RECENT") != nullptr);
    // a sample cap
    SondeHistory cap;
    for (int i = 0; i < 4100; ++i) { cap.add("C", static_cast<double>(i) * 20.0, 50.0, 0.0, 100.0); }
    CHECK(cap.of("C")->size() == kSondeMaxSamples);
    cap.clear();
    CHECK(cap.trackCount() == 0u);

    // derived: an ascent, then a burst
    const std::vector<SondeSample> up = flight(600, 0);
    SondeDerived d = deriveSonde(up);
    CHECK(d.hasTrack && !d.burst && d.haveClimb);
    CHECK_NEAR(d.climbMs, 5.0, 0.01);
    CHECK_NEAR(d.maxAltM, 100.0 + 5.0 * 600, 1e-9);
    CHECK(d.maxIndex + 1 == up.size());
    const std::vector<SondeSample> down = flight(600, 120);
    d = deriveSonde(down);
    CHECK(d.burst);
    CHECK_NEAR(d.maxAltM, 3100.0, 1e-9);
    CHECK(d.climbMs < -10.0);                                      // 12 m/s down
    CHECK(std::string(climbText(d.climbMs, d.haveClimb)).find("-12") == 0);
    CHECK(climbText(5.12, true) == "+5.1 m/s");
    CHECK(climbText(0.0, false).empty());
    CHECK(climbText(kNaN, true).empty());
    // a wobble of 100 m is not a burst; a short track has no climb rate
    std::vector<SondeSample> wob = flight(300, 0);
    wob.push_back({wob.back().tS + 5.0, wob.back().lat, wob.back().lon, wob.back().altM - 100.0});
    CHECK(!deriveSonde(wob).burst);
    CHECK(!deriveSonde(flight(5, 0)).haveClimb);
    CHECK(!deriveSonde({}).hasTrack);
    // a track with no altitudes at all
    CHECK(!deriveSonde({{0.0, 50.0, 0.0, kNaN}, {10.0, 50.0, 0.1, kNaN}}).burst);
    // local kilometres
    double e = 0, n = 0;
    localKm(51.5, -0.1, 51.5, -0.1, e, n);
    CHECK(e == 0.0 && n == 0.0);
    localKm(0.0, 0.0, 0.0, 1.0, e, n);
    CHECK_NEAR(e, 111.195, 0.01);
    CHECK_NEAR(n, 0.0, 1e-9);
    localKm(0.0, 0.0, 1.0, 0.0, e, n);
    CHECK_NEAR(n, 111.195, 0.01);
    localKm(60.0, 0.0, 60.0, 1.0, e, n);
    CHECK_NEAR(e, 111.195 * 0.5, 0.01);                            // a degree of longitude is half as long at 60 degrees
    localKm(0.0, 179.5, 0.0, -179.5, e, n);
    CHECK_NEAR(e, 111.195, 0.01);                                  // across the date line
    CHECK_NEAR(niceCeilKm(3.0), 5.0, 1e-12);
    CHECK_NEAR(niceCeilKm(21.0), 25.0, 1e-12);
    CHECK_NEAR(niceCeilKm(700.0), 700.0, 1e-12);
    CHECK_NEAR(niceCeilKm(-1.0), 5.0, 1e-12);
    CHECK(formatKm(85.0) == "85" && formatKm(12.5) == "12.5" && formatKm(0.04) == "0" && formatKm(-0.02) == "0" && formatKm(-3.0) == "-3");
    CHECK_NEAR(ladderFraction(17500.0), 0.5, 1e-12);
    CHECK_NEAR(ladderFraction(99000.0), 1.0, 1e-12);
    CHECK_NEAR(ladderFraction(-5.0), 0.0, 1e-12);
    CHECK(ladderFraction(kNaN) == -1.0);
}

void testPaint() {
    s3::Scene scene;
    std::vector<s3::Rgba> px(480 * 320);

    // an ascent, no burst yet: purple to yellow-green through the altitudes, no star, no pink, no orange without a receiver
    const std::vector<SondeSample> up = flight(1200, 0);
    SondeInput in = inputOf(up);
    in.tag = "S4720113  6100 m  +5 m/s";
    const SondeResult r = paintSonde3d(scene, px.data(), 480, 320, in);
    CHECK(r.hasTrack && !r.burst);
    CHECK(r.ascentSegments == up.size() - 1 && r.descentSegments == 0u);
    CHECK(r.groundSegments == up.size() - 1);
    CHECK(r.topKm == 35.0);
    CHECK(countWhere(px, purple) > 5 || countWhere(px, [](const s3::Rgba& p) { return p.b > 100 && p.g < 100; }) > 5);
    CHECK(countWhere(px, white) == 0u);
    CHECK(countWhere(px, pink) == 0u);
    CHECK(countWhere(px, orange) == 0u);
    CHECK(countWhere(px, cyan) > 20);                                // the ground track
    // 8 altitude ticks (every 5 km to 35), 5 + 5 others, 3 captions, the balloon's tag
    CHECK(r.labels.size() == 22u);
    CHECK(find(r, "S4720113  6100 m  +5 m/s") != nullptr);
    CHECK(find(r, "km E") != nullptr && find(r, "km N") != nullptr && find(r, "altitude km") != nullptr);
    CHECK(find(r, "0") != nullptr && find(r, "35") != nullptr && find(r, "15") != nullptr && find(r, "30") != nullptr);   // the altitude ticks: 0, 5, 10 ... 35
    // the tag stands at the balloon: where the renderer projects the newest sample (plus the label's 10 px offset)
    const s3::Label3D* tag = find(r, "S4720113  6100 m  +5 m/s");
    if (tag != nullptr) { CHECK(tag->kind == 2 && tag->align == 1); }

    // with a receiver: the orange triangle
    SondeInput rx = in;
    rx.haveRx = true;
    rx.rxEastKm = 0.0;
    rx.rxNorthKm = 0.0;
    std::vector<s3::Rgba> p2(480 * 320);
    paintSonde3d(scene, p2.data(), 480, 320, rx);
    CHECK(countWhere(p2, orange) > 10);

    // a burst: the star, the dashed pink descent, the burst label
    const std::vector<SondeSample> fl = flight(1200, 600);
    SondeInput b = inputOf(fl);
    b.burstLabel = "BURST 6 km";
    std::vector<s3::Rgba> p3(480 * 320);
    const SondeResult rb = paintSonde3d(scene, p3.data(), 480, 320, b);
    CHECK(rb.burst);
    CHECK(rb.ascentSegments > 0 && rb.descentSegments > 0);
    CHECK(rb.ascentSegments + rb.descentSegments == fl.size() - 1);
    CHECK(countWhere(p3, white) > 15);
    CHECK(countWhere(p3, pink) > 15);
    CHECK(find(rb, "BURST 6 km") != nullptr);
    // the same flight without the burst flag draws neither
    SondeInput nb = b;
    nb.burst = false;
    std::vector<s3::Rgba> p4(480 * 320);
    const SondeResult rn = paintSonde3d(scene, p4.data(), 480, 320, nb);
    CHECK(!rn.burst && rn.descentSegments == 0u);
    CHECK(countWhere(p4, white) == 0u);
    CHECK(find(rn, "BURST 6 km") == nullptr);

    // the plugin's own ground trail replaces the footprint
    SondeInput gt = in;
    gt.ground = {{0.0, 0.0}, {10.0, 3.0}, {20.0, 5.0}, {30.0, 6.0}};
    std::vector<s3::Rgba> p5(480 * 320);
    CHECK(paintSonde3d(scene, p5.data(), 480, 320, gt).groundSegments == 3u);

    // no track at all: the box, its words (over the default 20 km square), nothing else, not even a tag
    SondeInput none;
    none.camera = sonde3dDefaultCamera();
    none.tag = "never drawn";
    std::vector<s3::Rgba> p6(480 * 320);
    const SondeResult re = paintSonde3d(scene, p6.data(), 480, 320, none);
    CHECK(!re.hasTrack && re.ascentSegments == 0u && re.groundSegments == 0u);
    CHECK(re.labels.size() == 21u && find(re, "never drawn") == nullptr);
    CHECK(opaque(p6) > 500);
    // a balloon heard without an altitude is on the ground, and the ladder's ceiling grows for one above 35 km
    SondeInput low = in;
    for (SondeLocal& s : low.track) { s.altKm = kNaN; }
    std::vector<s3::Rgba> p7(480 * 320);
    CHECK(paintSonde3d(scene, p7.data(), 480, 320, low).hasTrack);
    SondeInput high = in;
    high.track.back().altKm = 41.0;
    std::vector<s3::Rgba> p8(480 * 320);
    CHECK_NEAR(paintSonde3d(scene, p8.data(), 480, 320, high).topKm, 45.0, 1e-12);

    // determinism and the camera
    std::vector<s3::Rgba> d1(480 * 320), d2(480 * 320), d3(480 * 320);
    paintSonde3d(scene, d1.data(), 480, 320, b);
    paintSonde3d(scene, d2.data(), 480, 320, b);
    CHECK(std::memcmp(d1.data(), d2.data(), d1.size() * sizeof(s3::Rgba)) == 0);
    SondeInput turned = b;
    turned.camera.azimuthDeg = 30.0f;
    paintSonde3d(scene, d3.data(), 480, 320, turned);
    CHECK(std::memcmp(d1.data(), d3.data(), d1.size() * sizeof(s3::Rgba)) != 0);
    // a single point, and degenerate sizes
    SondeInput one;
    one.camera = sonde3dDefaultCamera();
    one.track.push_back({1.0, 2.0, 3.0});
    std::vector<s3::Rgba> p9(480 * 320);
    CHECK(paintSonde3d(scene, p9.data(), 480, 320, one).hasTrack);
    paintSonde3d(scene, nullptr, 0, 0, b);
    CHECK(scene.width() == 0);
    CHECK(sonde3dRamp().at(0.0f).b > sonde3dRamp().at(1.0f).b);
}

}  // namespace

int main() {
    testLogic();
    testPaint();
    return testSummary("test_slint_host_sonde3d");
}
