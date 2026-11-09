// The RADAR SCOPE's pure logic (src/slint/radar_logic.hpp) and its picture (radar_compositor.hpp) against synthetic tracks and a scripted track-info source: the range ladder and the wheel, the
// drag / click / double-click rules and the dragged_ guard, which aircraft are plotted (kind, presentation age, range, FILTER), the trails and their caps, HOME, SYS and FLIGHT, the gauges, the drums,
// the GAIN knob and its local copy, the empty state and degenerate sizes, and the painter's output bytes. No Slint, no engine, no window. The classic's arithmetic (ladder, polar maths, detail lines,
// knob rules) is pinned by tests/test_scope_view.cpp and tests/test_scope_gain.cpp over the same headers.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "radar_compositor.hpp"
#include "radar_logic.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace foxsdr::slintscope;
using foxsdr::slinthost::Rgba;
namespace gui = cascade::gui;
using cascade::core::HostTrack;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kRxLat = 51.5, kRxLon = -0.1;
constexpr float kTube = 700.0f, kCx = 350.0f, kCy = 350.0f, kR = 348.0f;   // the tube and its disc (radius = side / 2 - 2)

double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---- fixtures ----
HostTrack mk(const char* id, double lat, double lon, double altM = kNaN, const char* label = "", std::uint32_t flags = 0, std::uint64_t ageMs = 0, std::uint32_t kind = CASCADE_TRACK_AIRCRAFT,
             double course = kNaN, double speed = kNaN) {
    HostTrack h;
    std::strncpy(h.t.id, id, sizeof(h.t.id) - 1);
    std::strncpy(h.t.label, label, sizeof(h.t.label) - 1);
    h.t.latDeg = lat;
    h.t.lonDeg = lon;
    h.t.altM = altM;
    h.t.courseDeg = course;
    h.t.speedMps = speed;
    h.t.ageMs = ageMs;
    h.t.kind = kind;
    h.t.flags = flags;
    return h;
}
// an aircraft `nm` nautical miles from the receiver on `bearing`
HostTrack at(const char* id, double bearing, double nm, double altM = kNaN, const char* label = "", std::uint32_t flags = 0, std::uint64_t ageMs = 0, std::uint32_t kind = CASCADE_TRACK_AIRCRAFT,
             double course = kNaN, double speed = kNaN) {
    const gui::LatLon p = gui::destinationPoint(kRxLat, kRxLon, bearing, nm * 1.852);
    return mk(id, p.latDeg, p.lonDeg, altM, label, flags, ageMs, kind, course, speed);
}
void moveNorth(HostTrack& h, double metres) { h.t.latDeg += metres / 111320.0; }

RadarInputs inputs(double nowS = 100.0) {
    RadarInputs in;
    in.nowS = nowS;
    in.hasRx = true;
    in.rxLatDeg = kRxLat;
    in.rxLonDeg = kRxLon;
    return in;
}

struct FakeInfo : RadarInfoSource {
    RadarInfo answer;
    int calls = 0;
    std::string last;
    RadarInfo lookup(const HostTrack& t) override {
        ++calls;
        last = t.t.id;
        return answer;
    }
};

RadarScope make(int rangeNm = 100, float tube = kTube) {
    RadarScope s;
    s.setTubeSize(tube, tube);
    s.setRangeNm(rangeNm);
    return s;
}

const RadarTarget* target(const RadarFrame& f, const std::string& id) {
    for (const RadarTarget& t : f.targets) { if (t.id == id) return &t; }
    return nullptr;
}
std::set<std::string> targetIds(const RadarFrame& f) {
    std::set<std::string> s;
    for (const RadarTarget& t : f.targets) { s.insert(t.id); }
    return s;
}
using Ids = std::set<std::string>;
const RadarLine* line(const RadarFrame& f, const char* label) {
    for (const RadarLine& l : f.flight) { if (l.label == label) return &l; }
    return nullptr;
}
// where the classic's polar frame puts an aircraft at (bearing, nm) of a range-`full` scope on the 700 px tube
void expectedXY(double bearing, double nm, double full, float& x, float& y) {
    const double r = kR * nm / full, a = bearing * kPi / 180.0;
    x = kCx + static_cast<float>(r * std::sin(a));
    y = kCy - static_cast<float>(r * std::cos(a));
}

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
// <repo>/src/<rel>, from this file's own path (.../src/slint/tests/test_slint_host_radar.cpp)
std::string srcPath(const char* rel) {
    std::string p = __FILE__;
    for (int i = 0; i < 3; ++i) {
        const std::size_t cut = p.find_last_of("/\\");
        if (cut == std::string::npos) break;
        p.resize(cut);
    }
    return p + "/" + rel;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void ladderAndWheel() {
    RadarScope s;
    s.setRangeNm(10);
    CHECK(s.rangeNm() == 10);
    // the whole ladder up, one rung a step, and its end (no wrap)
    const int ladder[] = {25, 50, 100, 200, 400, 800, 1600};
    for (int nm : ladder) {
        CHECK(s.stepRange(+1));
        CHECK(s.rangeNm() == nm);
    }
    CHECK(!s.stepRange(+1));
    CHECK(s.rangeNm() == 1600);
    CHECK(!s.stepRange(+1));
    CHECK(s.rangeNm() == 1600);
    // and down to the other end
    for (int i = 5; i >= 0; --i) { CHECK(s.stepRange(-1)); }
    CHECK(s.rangeNm() == 25);
    CHECK(s.stepRange(-1));
    CHECK(s.rangeNm() == 10);
    CHECK(!s.stepRange(-1));
    CHECK(s.rangeNm() == 10);
    // a big step clamps rather than wrapping
    CHECK(s.stepRange(1000));
    CHECK(s.rangeNm() == 1600);
    CHECK(s.stepRange(-1000));
    CHECK(s.rangeNm() == 10);
    CHECK(!s.stepRange(0));
    // a value off the ladder snaps onto it
    s.setRangeNm(130);
    bool onLadder = false;
    for (int nm : {10, 25, 50, 100, 200, 400, 800, 1600}) { onLadder = onLadder || s.rangeNm() == nm; }
    CHECK(onLadder);

    // the bar's -/+ readout and its disabled ends
    RadarScope t = make(10);
    t.update({}, inputs(), nullptr);
    CHECK(t.frame().rangeText == "10 NM");
    CHECK(!t.frame().canStepDown);
    CHECK(t.frame().canStepUp);
    t.setRangeNm(1600);
    t.update({}, inputs(), nullptr);
    CHECK(t.frame().rangeText == "1600 NM");
    CHECK(t.frame().canStepDown);
    CHECK(!t.frame().canStepUp);
    t.setRangeNm(200);
    t.update({}, inputs(), nullptr);
    CHECK(t.frame().canStepDown && t.frame().canStepUp);

    // THE WHEEL: up = the range DOWN one rung (a tighter picture), down = UP one rung
    RadarScope w;
    w.setRangeNm(200);
    w.wheel(+1);
    CHECK(w.rangeNm() == 100);
    w.wheel(-1);
    CHECK(w.rangeNm() == 200);
    w.wheel(-1);
    CHECK(w.rangeNm() == 400);
    w.wheel(0);
    CHECK(w.rangeNm() == 400);
    for (int i = 0; i < 12; ++i) { w.wheel(+1); }   // never below the first rung
    CHECK(w.rangeNm() == 10);
    for (int i = 0; i < 12; ++i) { w.wheel(-1); }   // never above the last
    CHECK(w.rangeNm() == 1600);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void dragPansTheView() {
    RadarScope s = make(100);
    // no update yet: the scope does not know where the antenna is, so a drag is ignored
    s.dragBy(30, 0);
    CHECK(!s.panned());
    std::vector<HostTrack> tr = {at("A", 90, 50, 3000.0)};
    s.update(tr, inputs(), nullptr);
    CHECK(!s.panned());
    CHECK_NEAR(s.viewLatDeg(), kRxLat, 1e-12);
    CHECK_NEAR(s.viewLonDeg(), kRxLon, 1e-12);
    // a zero or non-finite delta pans nothing
    s.dragBy(0, 0);
    s.dragBy(std::nanf(""), 3);
    CHECK(!s.panned());

    // dragging RIGHT carries the map right: the point in the middle of the glass moves WEST (scopeViewMoved), by dx / pxPerNm
    s.dragBy(60, 0);
    CHECK(s.panned());
    CHECK(s.viewLonDeg() < kRxLon);
    CHECK_NEAR(s.viewLatDeg(), kRxLat, 0.01);
    const double km = gui::greatCircleKm(kRxLat, kRxLon, s.viewLatDeg(), s.viewLonDeg());
    CHECK_NEAR(km / 1.852, 60.0 / (kR / 100.0), 0.05);   // 60 px at 3.48 px a mile
    // dragging DOWN moves the middle NORTH
    s.dragBy(0, 40);
    CHECK(s.viewLatDeg() > kRxLat);

    // the picture follows: the frame says panned and where the antenna now is (east of the middle, because the view went west)
    s.update(tr, inputs(), nullptr);
    CHECK(s.frame().panned);
    CHECK(s.frame().rxMarkX > kCx + 30.0f);
    CHECK(s.frame().rxMarkY > kCy);   // and south of it, because the view went north
    // the rings are measured from the MIDDLE OF THE VIEW: the polar frame's centre does not move
    CHECK_NEAR(s.frame().centreX, kCx, 1e-3);
    CHECK_NEAR(s.frame().centreY, kCy, 1e-3);

    // THE RANGE STAYS ANCHORED TO THE VIEW CENTRE: stepping it does not move the view, and the place under the middle dot stays under it
    const double vlat = s.viewLatDeg(), vlon = s.viewLonDeg();
    std::vector<HostTrack> mid = {mk("MID", vlat, vlon, 1000.0, "MID")};
    s.update(mid, inputs(), nullptr);
    const RadarTarget* a = target(s.frame(), "MID");
    CHECK(a != nullptr);
    if (a != nullptr) { CHECK_NEAR(a->x, kCx, 0.01); CHECK_NEAR(a->y, kCy, 0.01); }
    CHECK(s.stepRange(+1));
    s.update(mid, inputs(), nullptr);
    CHECK(s.viewLatDeg() == vlat);
    CHECK(s.viewLonDeg() == vlon);
    a = target(s.frame(), "MID");
    CHECK(a != nullptr);
    if (a != nullptr) { CHECK_NEAR(a->x, kCx, 0.01); CHECK_NEAR(a->y, kCy, 0.01); }
    s.wheel(+1);
    s.update(mid, inputs(), nullptr);
    CHECK(s.viewLatDeg() == vlat && s.viewLonDeg() == vlon);

    // targets are culled and projected from the VIEW, not the antenna: the aircraft 50 NM east of the antenna is now off the 100 NM picture's middle but still placed by the view
    s.setRangeNm(100);
    std::vector<HostTrack> east = {at("E", 90, 50, 3000.0)};
    s.update(east, inputs(), nullptr);
    const RadarTarget* e = target(s.frame(), "E");
    CHECK(e != nullptr);
    if (e != nullptr) { CHECK(e->x > kCx + 150.0f); }

    // DOUBLE-CLICK puts the antenna back in the middle
    s.doubleClick();
    CHECK(!s.panned());
    s.update(east, inputs(), nullptr);
    CHECK(!s.frame().panned);
    CHECK(s.viewLatDeg() == kRxLat && s.viewLonDeg() == kRxLon);
    float ex = 0, ey = 0;
    expectedXY(90, 50, 100, ex, ey);
    e = target(s.frame(), "E");
    CHECK(e != nullptr);
    if (e != nullptr) { CHECK_NEAR(e->x, ex, 0.05); CHECK_NEAR(e->y, ey, 0.05); }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void clicks() {
    float ax = 0, ay = 0, bx = 0, by = 0;
    expectedXY(90, 50, 100, ax, ay);   // A: 50 NM east
    expectedXY(90, 56, 100, bx, by);   // B: 6 NM further out (21 px)
    std::vector<HostTrack> tr = {at("A", 90, 50, 3000.0, "AAA"), at("B", 90, 56, 9000.0, "BBB")};

    // a click on an aircraft selects the nearest one within its marker radius
    RadarScope s = make(100);
    s.update(tr, inputs(), nullptr);
    CHECK(s.frame().targets.size() == 2);
    const float markR = s.frame().targets.empty() ? 0.0f : s.frame().targets[0].markR;
    CHECK_NEAR(markR, 28.8f, 0.01);   // 48 px icon * 0.6
    s.releasePointer(ax + 4, ay + 3, false);
    CHECK(s.selectedId() == "A");
    s.update(tr, inputs(), nullptr);
    CHECK(target(s.frame(), "A") != nullptr && target(s.frame(), "A")->selected);
    CHECK(target(s.frame(), "B") != nullptr && !target(s.frame(), "B")->selected);
    CHECK(s.frame().selectedId == "A");
    CHECK(!s.panned());   // selecting never recentres
    // between the two the nearer wins
    s.releasePointer(bx - 3, by, false);
    CHECK(s.selectedId() == "B");
    s.releasePointer(ax + 9, ay, false);
    CHECK(s.selectedId() == "A");
    // JUST OUTSIDE the marker radius it is empty glass
    s.releasePointer(ax, ay - markR - 2.0f, false);
    CHECK(s.selectedId().empty());

    // a click on EMPTY glass clears the selection AND puts that point in the middle
    s = make(100);
    s.update(tr, inputs(), nullptr);
    s.setSelection("A");
    const float cxClick = kCx + 120.0f, cyClick = kCy - 40.0f;
    s.releasePointer(cxClick, cyClick, false);
    CHECK(s.selectedId().empty());
    CHECK(s.panned());
    // the clicked point is now under the middle: it was (bearing/range) from the old view = 120 px east, 40 px north of the middle
    const double eastNm = 120.0 / (kR / 100.0), northNm = 40.0 / (kR / 100.0);
    const double brg = std::atan2(eastNm, northNm) * 180.0 / kPi;
    const gui::LatLon want = gui::destinationPoint(kRxLat, kRxLon, brg, std::hypot(eastNm, northNm) * 1.852);
    CHECK(gui::greatCircleKm(want.latDeg, want.lonDeg, s.viewLatDeg(), s.viewLonDeg()) < 0.02);

    // A CLICK THAT ENDS A DRAG selects nothing and does not recentre (the dragged_ guard)
    RadarScope d = make(100);
    d.update(tr, inputs(), nullptr);
    d.setSelection("A");
    d.dragBy(25, 0);
    const double vlat = d.viewLatDeg(), vlon = d.viewLonDeg();
    d.releasePointer(kCx + 200, kCy + 100, false);   // the release at the end of the pan, over empty glass
    CHECK(d.selectedId() == "A");
    CHECK(d.viewLatDeg() == vlat && d.viewLonDeg() == vlon);
    // ...and only that one release is swallowed: the next plain click works
    d.releasePointer(kCx + 200, kCy + 100, false);
    CHECK(d.selectedId().empty());
    CHECK(d.viewLonDeg() != vlon || d.viewLatDeg() != vlat);
    // the host may also say the gesture was a drag itself
    RadarScope e = make(100);
    e.update(tr, inputs(), nullptr);
    e.setSelection("A");
    e.releasePointer(kCx + 200, kCy + 100, true);
    CHECK(e.selectedId() == "A");
    CHECK(!e.panned());
    // a click on an aircraft that ends a drag does not select it either
    RadarScope g = make(100);
    g.update(tr, inputs(), nullptr);
    g.dragBy(10, 0);
    g.releasePointer(ax - 10, ay, false);   // (the aircraft moved under the pan, but the release is swallowed)
    CHECK(g.selectedId().empty());

    // without a receiver position (or a tube) a click does nothing
    RadarScope n = make(100);
    RadarInputs none = inputs();
    none.hasRx = false;
    n.update(tr, none, nullptr);
    n.setSelection("A");
    n.releasePointer(kCx + 50, kCy, false);
    CHECK(n.selectedId() == "A");
    CHECK(!n.panned());
    n.releasePointer(std::nanf(""), 3.0f, false);
    CHECK(n.selectedId() == "A");
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void whatIsPlotted() {
    std::vector<HostTrack> tr = {
        at("IN1", 10, 20, 3000.0, "AAA1"),
        at("IN2", 200, 99.0, kNaN, ""),                                   // no altitude, no callsign, inside
        at("EDGE", 90, 100.0, 9000.0, "EDGE"),                             // exactly at full scale counts as inside (floating point permitting)
        at("OUT", 45, 130, 3000.0, "FAR"),                                 // beyond the range
        at("OLD", 300, 20, 3000.0, "OLD", 0, 60000),                      // 60 s: dropped
        at("FADE", 120, 20, 3000.0, "FADE", 0, 59000),                    // fading, still shown
        at("EST", 150, 20, 3000.0, "EST", CASCADE_TRACK_FLAG_ESTIMATED, 61000),   // an estimated fix lives to 180 s
        at("EST2", 160, 20, 3000.0, "EST2", CASCADE_TRACK_FLAG_ESTIMATED, 180000),
        at("SHIP", 60, 20, 0.0, "SHIP", 0, 0, CASCADE_TRACK_VESSEL),     // not an aircraft
        at("SAT", 70, 20, 400000.0, "SAT", 0, 0, CASCADE_TRACK_SATELLITE),
        mk("NAN", kNaN, kNaN, 3000.0, "NAN"),                              // no position
        at("EMG", 270, 40, 3000.0, "", CASCADE_TRACK_FLAG_EMERGENCY),      // an emergency without a callsign
    };
    RadarScope s = make(100);
    s.update(tr, inputs(), nullptr);
    const RadarFrame& f = s.frame();
    Ids want = {"IN1", "IN2", "FADE", "EST", "EMG"};
    // (EDGE sits on the ring: whether it rounds in or out is the classic's `<=` over a computed distance, so it is not asserted either way)
    Ids got = targetIds(f);
    got.erase("EDGE");
    CHECK(got == want);
    CHECK(f.plotted == static_cast<int>(f.targets.size()));
    CHECK(f.plottedText == std::to_string(f.plotted) + " PLOTTED");
    CHECK(f.homeCount == std::to_string(f.plotted));
    // the vessel, the satellite, the dropped and the positionless are never plotted at any range
    s.setRangeNm(1600);
    s.update(tr, inputs(), nullptr);
    got = targetIds(s.frame());
    CHECK(got.count("SHIP") == 0 && got.count("SAT") == 0 && got.count("OLD") == 0 && got.count("EST2") == 0 && got.count("NAN") == 0);
    CHECK(got.count("OUT") == 1);
    s.setRangeNm(100);

    // an exact boundary: an aircraft exactly at range through the view's own distance is inside (scopeInRange is `<=`), tested through the polar maths the scope uses
    std::vector<HostTrack> exact = {mk("X", kRxLat, kRxLon, 3000.0, "X")};   // on top of the antenna: range 0
    s.update(exact, inputs(), nullptr);
    CHECK(s.frame().targets.size() == 1);
    if (!s.frame().targets.empty()) { CHECK_NEAR(s.frame().targets[0].x, kCx, 1e-3); CHECK_NEAR(s.frame().targets[0].y, kCy, 1e-3); }

    // target decoration: colour by altitude band, grey when unknown, red for an emergency, faded by age, label = callsign else id
    s.update(tr, inputs(), nullptr);
    const RadarTarget* in1 = target(s.frame(), "IN1");
    const RadarTarget* in2 = target(s.frame(), "IN2");
    const RadarTarget* emg = target(s.frame(), "EMG");
    const RadarTarget* fade = target(s.frame(), "FADE");
    CHECK(in1 != nullptr && in2 != nullptr && emg != nullptr && fade != nullptr);
    if (in1 != nullptr && in2 != nullptr && emg != nullptr && fade != nullptr) {
        CHECK(in1->rgba == 0x9CDC28FFu);   // 3000 m = 9843 ft: the 5-10 kft band (156,220,40), fresh
        CHECK(in1->altKnown && !in1->emergency);
        CHECK(in1->label == "AAA1");
        CHECK(in2->rgba == 0xC4CCD4FFu);   // no altitude: the neutral grey (196,204,212)
        CHECK(!in2->altKnown);
        CHECK(in2->label == "IN2");        // no callsign: the id
        CHECK(emg->rgba == 0xFF2D2DFFu);   // the alert red outranks the band
        CHECK(emg->emergency && emg->altKnown);
        CHECK(emg->label == "EMG");
        CHECK((fade->rgba & 0xFFu) == 82u);   // 59 s: alpha 1 - (29/30) * 0.7 = 0.323
        CHECK((fade->rgba >> 8) == 0x9CDC28u);
        CHECK(in1->headingDeg == 0.0f);    // an unknown course
    }
    HostTrack hd = at("HD", 10, 20, 100.0, "HD", 0, 0, CASCADE_TRACK_AIRCRAFT, 123.5);
    std::vector<HostTrack> one = {hd};
    s.update(one, inputs(), nullptr);
    CHECK(!s.frame().targets.empty() && std::abs(s.frame().targets[0].headingDeg - 123.5f) < 1e-4f);
    CHECK(!s.frame().targets.empty() && s.frame().targets[0].rgba == 0xFF7A14FFu);   // 100 m = 328 ft: the first band (255,122,20)
    // the hit radius follows the icon size (the classic's max(14, 0.6 * px)) and the icon size is clamped
    RadarInputs big = inputs();
    big.aircraftIconPx = 96;
    s.update(one, big, nullptr);
    CHECK(!s.frame().targets.empty() && std::abs(s.frame().targets[0].markR - 57.6f) < 1e-3f);
    big.aircraftIconPx = 16;
    s.update(one, big, nullptr);
    CHECK(!s.frame().targets.empty() && std::abs(s.frame().targets[0].markR - 14.0f) < 1e-3f);
    big.aircraftIconPx = 2;   // clamped to 16 by the caller's rule, here too
    s.update(one, big, nullptr);
    CHECK(!s.frame().targets.empty() && std::abs(s.frame().targets[0].markR - 14.0f) < 1e-3f);
    big.aircraftIconPx = 4000;
    s.update(one, big, nullptr);
    CHECK(!s.frame().targets.empty() && std::abs(s.frame().targets[0].markR - 57.6f) < 1e-3f);
    // the formula is the classic's, read from its source (aircraft_icons.cpp draws with GL and cannot be linked here)
    const std::string icons = slurp(srcPath("gui/aircraft_icons.cpp"));
    CHECK(icons.find("float aircraftMarkerRadius(float sizePx) { return std::max(14.0f, sizePx * 0.6f); }") != std::string::npos);
    // the restated presentation rule reads the classic's constants and its aircraft branch
    const std::string pui = slurp(srcPath("core/plugin_ui.cpp"));
    CHECK(pui.find("fadeMs = kTrackFadeMsAircraft;") != std::string::npos);
    CHECK(pui.find("CASCADE_TRACK_FLAG_ESTIMATED) != 0u) ? kTrackDropMsAircraftEstimated") != std::string::npos);
    CHECK(pui.find(": kTrackDropMsAircraft;") != std::string::npos);
    CHECK(cascade::core::kTrackFadeMsAircraft == 30000ull && cascade::core::kTrackDropMsAircraft == 60000ull && cascade::core::kTrackDropMsAircraftEstimated == 180000ull);
    CHECK_NEAR(cascade::core::kTrackMinAlpha, 0.30f, 1e-6);
    // the age ramp itself: fresh to 30 s, linear to the floor, gone at the drop age
    std::vector<HostTrack> ages = {at("A29", 10, 20, 3000.0, "a", 0, 29999), at("A45", 20, 20, 3000.0, "b", 0, 45000), at("A59", 30, 20, 3000.0, "c", 0, 59999), at("A60", 40, 20, 3000.0, "d", 0, 60000)};
    s.update(ages, inputs(), nullptr);
    CHECK(target(s.frame(), "A60") == nullptr);
    CHECK(target(s.frame(), "A29") != nullptr && (target(s.frame(), "A29")->rgba & 0xFFu) == 255u);
    CHECK(target(s.frame(), "A45") != nullptr && (target(s.frame(), "A45")->rgba & 0xFFu) == 166u);   // 1 - 0.5 * 0.7
    CHECK(target(s.frame(), "A59") != nullptr && (target(s.frame(), "A59")->rgba & 0xFFu) == 77u);    // just above the 0.30 floor
}

void filters() {
    std::vector<HostTrack> tr = {at("N1", 10, 20, 3000.0, "NAMED1"), at("U1", 100, 30, 3000.0, ""), at("E1", 200, 40, 3000.0, "EMG1", CASCADE_TRACK_FLAG_EMERGENCY),
                                 at("E2", 250, 40, 3000.0, "", CASCADE_TRACK_FLAG_EMERGENCY), at("N2", 300, 50, 3000.0, "NAMED2")};
    RadarScope s = make(100);
    s.update(tr, inputs(), nullptr);
    CHECK(targetIds(s.frame()) == (Ids{"N1", "U1", "E1", "E2", "N2"}));
    CHECK(s.optFilter() == 0 && s.frame().options.size() == 4 && s.frame().options[3].value == "ALL");
    CHECK(s.frame().homeSub == "of 5 tracked   100 NM");
    // FILTER 1: alerts only
    s.cycleOption(3);
    s.update(tr, inputs(), nullptr);
    CHECK(s.optFilter() == 1);
    CHECK(targetIds(s.frame()) == (Ids{"E1", "E2"}));
    CHECK(s.frame().plotted == 2 && s.frame().plottedText == "2 PLOTTED" && s.frame().homeCount == "2");
    CHECK(s.frame().options[3].value == "ALERT");
    CHECK(s.frame().homeSub == "of 5 tracked   100 NM   ALERT ONLY");   // the other gate the count is taken behind
    CHECK(s.frame().contacts.size() == 5);                              // the register is not filtered
    // FILTER 2: named only (a callsign, not the id)
    s.cycleOption(3);
    s.update(tr, inputs(), nullptr);
    CHECK(s.optFilter() == 2);
    CHECK(targetIds(s.frame()) == (Ids{"N1", "E1", "N2"}));
    CHECK(s.frame().options[3].value == "NAMED");
    CHECK(s.frame().homeSub == "of 5 tracked   100 NM   NAMED ONLY");
    // and back round to ALL
    s.cycleOption(3);
    s.update(tr, inputs(), nullptr);
    CHECK(s.optFilter() == 0);
    CHECK(s.frame().targets.size() == 5);
    // the PLOTTED count is what is drawn, whatever the filter
    for (int i = 0; i < 3; ++i) {
        s.update(tr, inputs(), nullptr);
        CHECK(s.frame().plotted == static_cast<int>(s.frame().targets.size()));
        s.cycleOption(3);
    }
}

void emptyAndDegenerate() {
    std::vector<HostTrack> tr = {at("A", 90, 50, 3000.0, "AAA")};
    RadarScope s = make(100);
    RadarInputs none = inputs();
    none.hasRx = false;
    s.update(tr, none, nullptr);
    const RadarFrame& f = s.frame();
    CHECK(!f.hasRx);
    CHECK(f.targets.empty() && f.plotted == 0);
    CHECK(f.ringLabels.empty() && f.bearingLabels.empty());
    CHECK(f.rxText.empty());
    CHECK(f.rangeText == "100 NM");   // the bar still says it
    CHECK(f.sysAntenna == "NOT SET");
    CHECK(f.contacts.size() == 1 && f.contacts[0].detail == "no receiver position");
    // a receiver the flag says is set but whose figures are NaN is no receiver
    RadarInputs nan = inputs();
    nan.rxLatDeg = kNaN;
    s.update(tr, nan, nullptr);
    CHECK(!s.frame().hasRx && s.frame().targets.empty());
    // empty tracks, a stale selection and every default
    s.update({}, inputs(), nullptr);
    CHECK(s.frame().hasRx && s.frame().targets.empty() && s.frame().plotted == 0);
    CHECK(s.frame().homeEmptyTitle == "NOTHING BEING HEARD");
    CHECK(s.frame().homeEmptyBody.rfind("No aircraft position has reached this face.", 0) == 0);
    CHECK(s.frame().drumTracks == "000");

    // degenerate tube sizes: nothing is drawn, nothing breaks, the cabinet still answers
    for (float side : {0.0f, 1.0f, 39.0f, -5.0f, std::nanf("")}) {
        RadarScope t = make(100, side);
        t.update(tr, inputs(), nullptr);
        CHECK(t.frame().targets.empty() && t.frame().plotted == 0);
        CHECK(t.frame().radiusPx == 0.0f);
        CHECK(t.frame().ringLabels.empty());
        CHECK(t.frame().hasRx);
        CHECK(t.frame().drumTracks == "001");
        t.dragBy(10, 10);
        t.releasePointer(1, 1, false);
        t.wheel(+1);
        CHECK(!t.panned());
        CHECK(t.trailPointCount() == 0);
        std::vector<Rgba> px(16, Rgba{9, 9, 9, 9});
        RadarPaintScratch scratch;
        CHECK(paintRadarGround(px.data(), 4, 4, t, scratch) == 0);
        for (const Rgba& p : px) { CHECK(p.a == 0); }
    }
    // the smallest tube that draws: 40 px
    RadarScope m = make(100, 40.0f);
    m.update(tr, inputs(), nullptr);
    CHECK(m.frame().radiusPx == 18.0f);
    CHECK(m.frame().ringLabels.size() == 4);
    CHECK(m.frame().bearingLabels.size() == 4);   // the cardinals only
    CHECK(!m.frame().showThreeDigitBearings);
    // a non-square tube: the disc is the shorter side, centred
    RadarScope wide;
    wide.setTubeSize(900, 500);
    wide.setRangeNm(100);
    wide.update(tr, inputs(), nullptr);
    CHECK(wide.frame().centreX == 450.0f && wide.frame().centreY == 250.0f && wide.frame().radiusPx == 248.0f);
    // odd inputs: huge range, NaN signal, negative everything, an empty id
    RadarInputs odd = inputs();
    odd.signalDb = kNaN;
    odd.running = true;
    odd.gainLoDb = kNaN;
    odd.gainHiDb = kNaN;
    odd.haveGain = true;
    odd.deviceOpen = true;
    odd.trailWidthPx = -3;
    odd.aircraftIconPx = -1;
    std::vector<HostTrack> weird = {mk("", 1e9, -1e9, -1e12, ""), mk("INF", std::numeric_limits<double>::infinity(), 0.0, std::numeric_limits<double>::infinity(), "i")};
    RadarScope w = make(1600);
    w.update(weird, odd, nullptr);
    CHECK(w.frame().targets.size() <= 2);
    CHECK(w.frame().sigText == "--");
    CHECK(w.frame().knobTicks[0] == 0 && w.frame().knobTicks[4] == 60);   // the NaN travel falls back to 0..60
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void trails() {
    // a trail BEGINS when the aircraft is first seen: one point, no history
    RadarScope s = make(100);
    HostTrack a = at("A", 0, 40, 3000.0, "AAA");
    HostTrack far = at("FAR", 0, 400, 3000.0, "FAR");
    std::vector<HostTrack> tr = {a, far};
    s.update(tr, inputs(), nullptr);
    CHECK(s.trailPointCount() == 1);   // only the plotted aircraft is recorded
    CHECK(s.trailKeys() == std::vector<std::string>({"A"}));
    CHECK(s.trails().empty());         // a single point is no trail

    // THE 50 m RULE: 30 m is the same place, 60 m from the last STORED point is a new one
    moveNorth(tr[0], 30.0);
    s.update(tr, inputs(), nullptr);
    CHECK(s.trailPointCount() == 1);
    moveNorth(tr[0], 30.0);   // 60 m from the stored point
    s.update(tr, inputs(), nullptr);
    CHECK(s.trailPointCount() == 2);
    moveNorth(tr[0], 49.0);
    s.update(tr, inputs(), nullptr);
    CHECK(s.trailPointCount() == 2);
    moveNorth(tr[0], 5.0);    // 54 m
    s.update(tr, inputs(), nullptr);
    CHECK(s.trailPointCount() == 3);
    // east-west distance is shortened by the cosine of the latitude: 40 m of longitude at 51.5 N is 25 m of ground
    HostTrack e = at("E", 90, 30, 3000.0, "EEE");
    RadarScope ew = make(100);
    std::vector<HostTrack> et = {e};
    ew.update(et, inputs(), nullptr);
    et[0].t.lonDeg += 40.0 / (111320.0 * std::cos(et[0].t.latDeg * kPi / 180.0));
    ew.update(et, inputs(), nullptr);
    CHECK(ew.trailPointCount() == 1);
    et[0].t.lonDeg += 40.0 / (111320.0 * std::cos(et[0].t.latDeg * kPi / 180.0));   // 80 m from the stored point
    ew.update(et, inputs(), nullptr);
    CHECK(ew.trailPointCount() == 2);

    // the output: oldest first, in tube pixels, a colour per point from the altitude band, the first point carrying its leg's colour
    CHECK(s.trails().size() == 1);
    if (!s.trails().empty()) {
        const RadarTrail& t = s.trails()[0];
        CHECK(t.id == "A" && !t.selected && t.points.size() == 3);
        if (t.points.size() == 3) {
            CHECK(t.points[0].y > t.points[1].y && t.points[1].y > t.points[2].y);   // moving north is up the picture
            CHECK(t.points[0].rgba == t.points[1].rgba);
            CHECK((t.points[1].rgba >> 8) == 0x9CDC28u);   // 5-10 kft
            const int alpha = static_cast<int>(t.points[2].rgba & 0xFFu);
            CHECK(alpha >= 60 && alpha <= 210);
            CHECK(alpha > static_cast<int>(t.points[1].rgba & 0xFFu));   // the leg nearest now is the brightest
        }
    }
    // the selected aircraft's trail is flagged (and brighter: x1 instead of x0.75)
    const std::uint32_t plainAlpha = s.trails().empty() || s.trails()[0].points.empty() ? 0u : (s.trails()[0].points.back().rgba & 0xFFu);
    s.setSelection("A");
    s.update(tr, inputs(), nullptr);
    CHECK(s.trails().size() == 1 && s.trails()[0].selected);
    CHECK(s.trails().size() == 1 && (s.trails()[0].points.back().rgba & 0xFFu) > plainAlpha);

    // TRAILS off: still recorded, not output (switching them on shows where traffic has been)
    s.clearSelection();
    s.cycleOption(2);   // LINE -> RIBBON
    CHECK(s.optTrail() == 2);
    s.cycleOption(2);   // -> OFF
    CHECK(s.optTrail() == 0);
    s.update(tr, inputs(), nullptr);
    CHECK(s.trails().empty());
    CHECK(s.trailPointCount() == 3);
    s.cycleOption(2);   // -> LINE
    s.update(tr, inputs(), nullptr);
    CHECK(s.optTrail() == 1 && s.trails().size() == 1);

    // a leg with an end outside the range is dropped, not clipped: the run is split in two
    RadarScope split = make(100);
    const struct { double brg, nm; } way[] = {{0, 10}, {0, 15}, {0, 40}, {90, 12}, {90, 16}};
    std::vector<HostTrack> xs;
    for (const auto& w : way) {
        xs = {at("X", w.brg, w.nm, 3000.0, "X")};
        split.update(xs, inputs(), nullptr);   // recorded at 100 NM, where every fix is inside
    }
    CHECK(split.trailPointCount() == 5);
    CHECK(split.trails().size() == 1 && split.trails()[0].points.size() == 5);
    split.setRangeNm(25);   // 40 NM is now beyond the ring
    split.update(xs, inputs(), nullptr);
    CHECK(split.trailPointCount() == 5);   // nothing recorded is lost
    CHECK(split.trails().size() == 2);
    if (split.trails().size() == 2) {
        CHECK(split.trails()[0].id == "X" && split.trails()[0].points.size() == 2);
        CHECK(split.trails()[1].id == "X" && split.trails()[1].points.size() == 2);
        CHECK(split.trails()[0].points[0].y > split.trails()[0].points[1].y);   // the first run goes north
        CHECK(split.trails()[1].points[0].x < split.trails()[1].points[1].x);   // the second goes east
    }
    split.setRangeNm(10);   // only the first fix and a sliver: no leg has both ends inside
    split.update(xs, inputs(), nullptr);
    CHECK(split.trails().empty());

    // THE 256-POINT CAP: the oldest go first
    RadarScope cap = make(100);
    std::vector<HostTrack> ct = {at("C", 0, 40, 3000.0, "C")};
    cap.update(ct, inputs(), nullptr);
    const float startY = kCy - kR * 0.4f;
    for (int i = 0; i < 300; ++i) {
        moveNorth(ct[0], 100.0);
        cap.update(ct, inputs(), nullptr);
    }
    CHECK(cap.trailPointCount() == 256);
    CHECK(cap.trails().size() == 1 && cap.trails()[0].points.size() == 256);
    if (!cap.trails().empty() && !cap.trails()[0].points.empty()) {
        CHECK(cap.trails()[0].points.front().y < startY - 5.0f);   // the start of the track is gone
        CHECK(cap.trails()[0].points.back().y < cap.trails()[0].points.front().y);
    }

    // THE 256-TRACK CAP: never more than 256 trails, the first 256 in track order get them
    RadarScope many = make(100);
    std::vector<HostTrack> mt;
    for (int i = 0; i < 300; ++i) {
        char id[16];
        std::snprintf(id, sizeof(id), "C%03d", i);
        mt.push_back(at(id, i * 1.2, 20.0 + (i % 50) * 1.5, 3000.0, id));
    }
    many.update(mt, inputs(), nullptr);
    CHECK(many.frame().targets.size() == 300);
    CHECK(many.trailKeys().size() == 256);
    CHECK(many.trailPointCount() == 256);
    const auto has = [&](const char* k) { return std::find(many.trailKeys().begin(), many.trailKeys().end(), std::string(k)) != many.trailKeys().end(); };
    CHECK(has("C000") && has("C255") && !has("C256") && !has("C299"));
    // a new set replaces the dead ones: the gone aircraft's trails are evicted so the newcomers get theirs
    std::vector<HostTrack> nt;
    for (int i = 0; i < 10; ++i) {
        char id[16];
        std::snprintf(id, sizeof(id), "D%03d", i);
        nt.push_back(at(id, i * 30.0, 30.0, 3000.0, id));
    }
    many.update(nt, inputs(), nullptr);
    CHECK(many.trailKeys().size() == 10);
    CHECK(has("D000") && has("D009") && !has("C000"));
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void lcd() {
    RadarInfoSource* none = nullptr;
    // HOME: nearest first, from the ANTENNA, every visible aircraft whatever the range or the FILTER
    std::vector<HostTrack> tr = {at("FAR", 40, 300, 12000.0, "FARR"), at("MID", 120, 20, 3000.0, "MIDD"), at("NEAR", 200, 5, kNaN, ""), at("EMG", 90, 60, 100.0, "", CASCADE_TRACK_FLAG_EMERGENCY),
                                 at("GONE", 10, 10, 3000.0, "GONE", 0, 70000), at("SHIP", 10, 1, 0.0, "SHIP", 0, 0, CASCADE_TRACK_VESSEL)};
    RadarScope s = make(100);
    s.update(tr, inputs(), none);
    const RadarFrame& f = s.frame();
    CHECK(f.contacts.size() == 4);
    if (f.contacts.size() == 4) {
        CHECK(f.contacts[0].id == "NEAR" && f.contacts[1].id == "MID" && f.contacts[2].id == "EMG" && f.contacts[3].id == "FAR");
        CHECK(f.contacts[0].name == "NEAR");   // no callsign: the id, never blank
        CHECK(f.contacts[1].name == "MIDD");
        CHECK(f.contacts[0].alt == "NO ALT" && f.contacts[0].rgba == 0u);
        CHECK(f.contacts[1].alt == "FL098");   // 3000 m * 3.28084 / 100 = 98.4
        CHECK((f.contacts[1].rgba >> 8) == 0x9CDC28u && (f.contacts[1].rgba & 0xFFu) == 255u);
        CHECK(f.contacts[3].alt == "FL393");
        CHECK(f.contacts[2].alert && !f.contacts[0].alert);
        CHECK(f.contacts[0].detail == "5 NM   200 deg");
        CHECK(f.contacts[1].detail == "20 NM   120 deg");
        CHECK(f.contacts[3].detail == "300 NM   040 deg");
        CHECK(f.homeEmptyTitle.empty() && f.homeEmptyBody.empty());
    }
    CHECK(f.homeSub == "of 5 tracked   100 NM");   // GONE is tracked, though not current
    CHECK(f.drumTracks == "005");                  // the odometer counts every aircraft held
    // an aircraft straight overhead has a range and no bearing: "000" is not claimed as a direction by a crash, the row prints
    std::vector<HostTrack> over = {mk("OVR", kRxLat, kRxLon, 1000.0, "OVR")};
    s.update(over, inputs(), none);
    CHECK(s.frame().contacts.size() == 1 && s.frame().contacts[0].detail == "0 NM   000 deg");
    // measured from the antenna even when the view has been dragged
    s.update(tr, inputs(), none);
    s.dragBy(100, 100);
    s.update(tr, inputs(), none);
    CHECK(s.frame().contacts.size() == 4 && s.frame().contacts[0].id == "NEAR" && s.frame().contacts[0].detail == "5 NM   200 deg");

    // the register's empty states: aircraft heard but none current, one or several; nothing at all
    std::vector<HostTrack> old1 = {at("O1", 10, 10, 3000.0, "O1", 0, 61000)};
    s.update(old1, inputs(), none);
    CHECK(s.frame().homeEmptyTitle == "NOTHING CURRENT");
    CHECK(s.frame().homeEmptyBody == "1 aircraft has been heard and none has reported inside the last 60 seconds, which is the age the host drops a target at. They have gone out of range, or the decoder has "
                                     "stopped being fed - the Fitted modules window says whether it is still being fed.");
    std::vector<HostTrack> old2 = {old1[0], at("O2", 20, 10, 3000.0, "O2", 0, 99000)};
    s.update(old2, inputs(), none);
    CHECK(s.frame().homeEmptyBody == "2 aircraft have been heard and none has reported inside the last 60 seconds, which is the age the host drops a target at. They have gone out of range, or the decoder has "
                                     "stopped being fed - the Fitted modules window says whether it is still being fed.");
    s.update({}, inputs(), none);
    CHECK(s.frame().homeEmptyTitle == "NOTHING BEING HEARD");

    // A HOME ROW'S CLICK selects that aircraft and switches to FLIGHT
    RadarScope h = make(100);
    h.update(tr, inputs(), none);
    CHECK(h.lcdScreen() == 1);   // FLIGHT is the default tab
    h.setLcdScreen(0);
    CHECK(h.lcdScreen() == 0);
    h.selectContact("FAR");
    CHECK(h.selectedId() == "FAR");
    CHECK(h.lcdScreen() == 1);
    h.setLcdScreen(7);   // out of range: clamped
    CHECK(h.lcdScreen() == 2);
    h.setLcdScreen(-3);
    CHECK(h.lcdScreen() == 0);
    h.update(tr, inputs(), none);
    CHECK(h.frame().lcdScreen == 0);
    h.setLcdShown(false);
    h.update(tr, inputs(), none);
    CHECK(!h.frame().lcdShown);

    // SYS: the four switches cycle and say their value
    RadarScope o = make(100);
    o.update(tr, inputs(), none);
    const auto optionText = [&]() {
        std::string out;
        for (const RadarOption& r : o.frame().options) { out += r.name + "=" + r.value + ";"; }
        return out;
    };
    o.update(tr, inputs(), none);
    CHECK(optionText() == "PHOSPHOR=ON;SWEEP=ON;TRAILS=LINE;FILTER=ALL;");
    o.cycleOption(0);
    o.update(tr, inputs(), none);
    CHECK(!o.optPhosphor() && optionText() == "PHOSPHOR=OFF;SWEEP=ON;TRAILS=LINE;FILTER=ALL;");
    o.cycleOption(0);
    o.cycleOption(1);
    o.update(tr, inputs(), none);
    CHECK(o.optPhosphor() && !o.optSweep() && optionText() == "PHOSPHOR=ON;SWEEP=OFF;TRAILS=LINE;FILTER=ALL;");
    o.cycleOption(1);
    const char* trailWords[] = {"RIBBON", "OFF", "LINE", "RIBBON"};
    for (int i = 0; i < 4; ++i) {
        o.cycleOption(2);
        o.update(tr, inputs(), none);
        CHECK(optionText().find(std::string("TRAILS=") + trailWords[i] + ";") != std::string::npos);
    }
    const char* filterWords[] = {"ALERT", "NAMED", "ALL", "ALERT"};
    for (int i = 0; i < 4; ++i) {
        o.cycleOption(3);
        o.update(tr, inputs(), none);
        CHECK(optionText().find(std::string("FILTER=") + filterWords[i] + ";") != std::string::npos);
    }
    o.cycleOption(9);   // no such row
    o.cycleOption(-1);
    CHECK(o.optFilter() == 1);
    CHECK(o.frame().sysRange == "100 NM");
    CHECK(o.frame().sysAntenna == "51.500 -0.100");
    CHECK(o.frame().sysHelp.rfind("Drag the face to move the view, double-click to recentre, and the wheel steps the range.", 0) == 0);
    CHECK(o.frame().phosphor && o.frame().sweep && o.frame().trail == 2 && o.frame().filter == 1);   // (four trail presses from LINE land on RIBBON, four filter presses on ALERT)

    // FLIGHT
    RadarScope fl = make(100);
    FakeInfo info;
    std::vector<HostTrack> ft = {at("AAA111", 45, 30, 10000.0, "BAW123", 0, 0, CASCADE_TRACK_AIRCRAFT, 90.0, 200.0)};
    fl.update(ft, inputs(), &info);
    CHECK(fl.frame().flight.empty());
    CHECK(fl.frame().flightTitle == "NOTHING SELECTED");
    CHECK(fl.frame().flightBody == "Click an aircraft on the scope to see its details.");
    CHECK(info.calls == 0);   // nothing selected: nothing looked up
    fl.setSelection("AAA111");
    // no track-info source attached at all: the registry rows say nothing is looking
    fl.update(ft, inputs(), nullptr);
    CHECK(fl.frame().flight.size() == 10);
    const RadarLine* l = line(fl.frame(), "OPERATOR");
    CHECK(l != nullptr && l->value == "NO LOOKUP RUNNING" && l->dim);
    // a source that answered
    info.answer.active = true;
    info.answer.registration = "G-ABCD";
    info.answer.typeCode = "B738";
    info.answer.typeName = "BOEING 737-800";
    info.answer.operatorName = "BRITISH AIRWAYS";
    fl.update(ft, inputs(), &info);
    CHECK(info.calls == 1 && info.last == "AAA111");
    const char* labels[] = {"FLIGHT", "OPERATOR", "TYPE", "REG", "STATUS", "ALTITUDE", "SPEED", "HEADING", "RANGE", "BEARING"};
    const char* values[] = {"BAW123", "BRITISH AIRWAYS", "BOEING 737-800", "G-ABCD", "NORMAL", "32808 FT", "389 KT", "090 DEG", "30.0 NM", "045 DEG"};
    CHECK(fl.frame().flight.size() == 10);
    for (std::size_t i = 0; i < 10 && i < fl.frame().flight.size(); ++i) {
        CHECK(fl.frame().flight[i].label == labels[i]);
        CHECK(fl.frame().flight[i].value == values[i]);
        CHECK(!fl.frame().flight[i].dim && !fl.frame().flight[i].alert);
    }
    CHECK(fl.frame().flightFoot == "No squawk, route or airline mark: the track interface does not carry them.");
    CHECK(fl.frame().flightTitle.empty() && fl.frame().flightBody.empty());
    // the type code stands in for an absent spelled-out name
    info.answer.typeName.clear();
    fl.update(ft, inputs(), &info);
    l = line(fl.frame(), "TYPE");
    CHECK(l != nullptr && l->value == "B738");
    // pending, and a source with no answer
    info.answer = RadarInfo();
    info.answer.active = true;
    info.answer.pending = true;
    fl.update(ft, inputs(), &info);
    l = line(fl.frame(), "REG");
    CHECK(l != nullptr && l->value == "LOOKING UP" && l->dim);
    info.answer.pending = false;
    fl.update(ft, inputs(), &info);
    l = line(fl.frame(), "REG");
    CHECK(l != nullptr && l->value == "NO DATA" && l->dim);
    // an emergency is the alert row; the unknowns are dim
    std::vector<HostTrack> et = {at("EMG1", 45, 30, kNaN, "", CASCADE_TRACK_FLAG_EMERGENCY)};
    fl.setSelection("EMG1");
    fl.update(et, inputs(), &info);
    l = line(fl.frame(), "STATUS");
    CHECK(l != nullptr && l->value == "EMERGENCY" && l->alert);
    l = line(fl.frame(), "ALTITUDE");
    CHECK(l != nullptr && l->value == "NO DATA" && l->dim);
    l = line(fl.frame(), "FLIGHT");
    CHECK(l != nullptr && l->value == "EMG1");   // no callsign: the id
    // selected but gone (not in the list) or no longer plotted (stale, out of range, filtered): TARGET LOST, and the selection is kept
    fl.setSelection("GHOST");
    fl.update(ft, inputs(), &info);
    CHECK(fl.frame().flight.empty() && fl.frame().flightTitle == "TARGET LOST");
    CHECK(fl.frame().flightBody == "GHOST is no longer being plotted - it has gone quiet or left the selected range.");
    CHECK(fl.selectedId() == "GHOST");
    fl.setSelection("AAA111");
    fl.setRangeNm(10);   // 30 NM is out of a 10 NM picture
    fl.update(ft, inputs(), &info);
    CHECK(fl.frame().flightTitle == "TARGET LOST" && fl.frame().flight.empty());
    fl.setRangeNm(100);   // and it comes back by itself
    fl.update(ft, inputs(), &info);
    CHECK(fl.frame().flight.size() == 10 && fl.frame().flightTitle.empty());
    // the registry is asked only while the FLIGHT screen shows
    const int before = info.calls;
    fl.setLcdScreen(0);
    fl.update(ft, inputs(), &info);
    CHECK(info.calls == before && fl.frame().flight.empty());
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void cabinet() {
    RadarScope s = make(100);
    std::vector<HostTrack> tr = {at("A", 45, 30, 6500.0, "AAA"), at("NA", 90, 10, kNaN, "NNN")};
    // SIG: stopped / non-finite -> no bar and "--"
    RadarInputs in = inputs();
    in.running = false;
    in.signalDb = -50.0;
    s.update(tr, in, nullptr);
    CHECK(s.frame().sigText == "--" && s.frame().sigLit == 0 && s.frame().sigFrac == 0.0f);
    in.running = true;
    in.signalDb = kNaN;
    s.update(tr, in, nullptr);
    CHECK(s.frame().sigText == "--" && s.frame().sigLit == 0);
    in.signalDb = -std::numeric_limits<double>::infinity();
    s.update(tr, in, nullptr);
    CHECK(s.frame().sigText == "--");
    // -100..0 dBFS across 18 segments, lit = round(frac * 18)
    const struct { double db; int lit; const char* text; } sig[] = {{-50.0, 9, "-50"}, {0.0, 18, "0"}, {-100.0, 0, "-100"}, {-75.0, 5, "-75"}, {-25.0, 14, "-25"}, {-150.0, 0, "-150"}, {20.0, 18, "20"}};
    for (const auto& c : sig) {
        in.signalDb = c.db;
        s.update(tr, in, nullptr);
        CHECK(s.frame().sigLit == c.lit);
        CHECK(s.frame().sigText == c.text);
    }
    in.signalDb = -50.0;
    s.update(tr, in, nullptr);
    CHECK_NEAR(s.frame().sigFrac, 0.5, 1e-6);

    // ALT: nothing selected "--"; selected with an altitude FL; selected without "NO ALT"; selected but unknown id "NO ALT"
    s.update(tr, in, nullptr);
    CHECK(s.frame().altText == "--" && s.frame().altLit == 0);
    s.setSelection("A");
    s.update(tr, in, nullptr);
    CHECK(s.frame().altText == "FL213");   // 6500 m = 21325 ft
    CHECK_NEAR(s.frame().altFrac, 0.5, 1e-6);
    CHECK(s.frame().altLit == 9);
    s.setSelection("NA");
    s.update(tr, in, nullptr);
    CHECK(s.frame().altText == "NO ALT" && s.frame().altLit == 0);
    s.setSelection("ZZ");
    s.update(tr, in, nullptr);
    CHECK(s.frame().altText == "NO ALT");
    // the gauge reads an aircraft that is not plotted too (it looks at every aircraft the host holds)
    std::vector<HostTrack> offscope = {at("HI", 0, 500, 13000.0, "HI")};
    s.setSelection("HI");
    s.update(offscope, in, nullptr);
    CHECK(s.frame().targets.empty() && s.frame().altText == "FL426" && s.frame().altLit == 18);
    CHECK(s.frame().drumCaption == "TGT RANGE NM" && s.frame().drumRange == "0500");

    // THE DRUMS: SET RANGE NM with the scope's range, 4 digits; TGT RANGE NM with the selected aircraft's range from the antenna
    s.clearSelection();
    s.update(tr, in, nullptr);
    CHECK(s.frame().drumCaption == "SET RANGE NM" && s.frame().drumRange == "0100" && s.frame().drumTracks == "002");
    s.setRangeNm(1600);
    s.update(tr, in, nullptr);
    CHECK(s.frame().drumRange == "1600");
    s.setRangeNm(10);
    s.update(tr, in, nullptr);
    CHECK(s.frame().drumRange == "0010");
    s.setSelection("A");
    s.update(tr, in, nullptr);
    CHECK(s.frame().drumCaption == "TGT RANGE NM" && s.frame().drumRange == "0030");   // 30 NM
    s.setSelection("ZZ");
    s.update(tr, in, nullptr);
    CHECK(s.frame().drumCaption == "SET RANGE NM");   // the selection names nothing the host holds
    // the count is every aircraft, ungated, and clamped to all nines
    std::vector<HostTrack> crowd;
    for (int i = 0; i < 1100; ++i) {
        char id[16];
        std::snprintf(id, sizeof(id), "K%04d", i);
        crowd.push_back(at(id, (i % 360), 300.0 + (i % 100), 1000.0, id, 0, (i % 3 == 0) ? 90000 : 0));
    }
    crowd.push_back(at("SHIP", 0, 5, 0.0, "SHIP", 0, 0, CASCADE_TRACK_VESSEL));
    s.update(crowd, in, nullptr);
    CHECK(s.frame().drumTracks == "999");
    s.update({}, in, nullptr);
    CHECK(s.frame().drumTracks == "000");

    // POWER
    in.running = true;
    s.update(tr, in, nullptr);
    CHECK(s.frame().powerOn && s.frame().powerText == "ON LINE");
    in.running = false;
    s.update(tr, in, nullptr);
    CHECK(!s.frame().powerOn && s.frame().powerText == "STANDBY");

    // corner readouts
    s.update(tr, in, nullptr);
    CHECK(s.frame().modeText == "MODE ADS-B");
    CHECK(s.frame().rxText == "51.5N 000.1W");
    CHECK(s.frame().rangeCornerText == "10 NM");
    RadarInputs south = inputs();
    south.rxLatDeg = -33.9;
    south.rxLonDeg = 151.2;
    s.update(tr, south, nullptr);
    CHECK(s.frame().rxText == "33.9S 151.2E");
    // ring and bearing words
    RadarScope g = make(100);
    g.update({}, inputs(), nullptr);
    CHECK(g.frame().ringLabels.size() == 4);
    const char* rings[] = {"25 NM", "50 NM", "75 NM", "100 NM"};
    for (std::size_t i = 0; i < 4 && i < g.frame().ringLabels.size(); ++i) { CHECK(g.frame().ringLabels[i].text == rings[i]); CHECK(g.frame().ringLabels[i].strong == (i == 3)); }
    g.setRangeNm(10);
    g.update({}, inputs(), nullptr);
    const char* rings10[] = {"2.5 NM", "5 NM", "7.5 NM", "10 NM"};
    for (std::size_t i = 0; i < 4 && i < g.frame().ringLabels.size(); ++i) { CHECK(g.frame().ringLabels[i].text == rings10[i]); }
    // the labels sit on the north radial: each ring's label above (the outer one inside) its ring
    if (g.frame().ringLabels.size() == 4) {
        CHECK(g.frame().ringLabels[0].y > g.frame().ringLabels[1].y && g.frame().ringLabels[1].y > g.frame().ringLabels[2].y);
        CHECK(g.frame().ringLabels[3].y > kCy - kR && g.frame().ringLabels[2].y < kCy - kR * 0.75f);
    }
    // twelve bearing labels on a big face, the cardinals alone on a small one
    CHECK(g.frame().showThreeDigitBearings && g.frame().bearingLabels.size() == 12);
    if (g.frame().bearingLabels.size() == 12) {
        CHECK(g.frame().bearingLabels[0].text == "N" && g.frame().bearingLabels[0].strong);
        CHECK(g.frame().bearingLabels[1].text == "030" && !g.frame().bearingLabels[1].strong);
        CHECK(g.frame().bearingLabels[3].text == "E" && g.frame().bearingLabels[6].text == "S" && g.frame().bearingLabels[9].text == "W");
        CHECK_NEAR(g.frame().bearingLabels[0].x, kCx, 1e-3);
        CHECK_NEAR(g.frame().bearingLabels[0].y, kCy - kR * 0.9f, 1e-3);
        CHECK_NEAR(g.frame().bearingLabels[3].x, kCx + kR * 0.9f, 1e-3);
    }
    RadarScope small = make(100, 300.0f);   // radius 148: under 150
    small.update({}, inputs(), nullptr);
    CHECK(!small.frame().showThreeDigitBearings && small.frame().bearingLabels.size() == 4);
    RadarScope edge = make(100, 304.0f);    // radius 150: exactly the threshold
    edge.update({}, inputs(), nullptr);
    CHECK(edge.frame().showThreeDigitBearings && edge.frame().bearingLabels.size() == 12);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void knob() {
    RadarScope s = make(100);
    std::vector<HostTrack> none;
    RadarInputs in = inputs(100.0);
    // no device: dead, N/A, "no device"
    s.update(none, in, nullptr);
    CHECK(s.frame().knobDead && s.frame().knobReadout == "N/A" && s.frame().knobNote == "no device" && s.frame().knobLit == -1);
    CHECK_NEAR(s.frame().knobFrac, 0.5f, 1e-6);
    CHECK(s.frame().knobTicks[0] == 0 && s.frame().knobTicks[4] == 60);   // the fallback travel
    CHECK(std::isnan(s.knobDetent(+1, in)));
    // a device open with no gain list is still no device
    in.deviceOpen = true;
    s.update(none, in, nullptr);
    CHECK(s.frame().knobDead && s.frame().knobReadout == "N/A");
    // a device with a stage
    in.haveGain = true;
    in.gainStage = "PGA";
    in.gainUnit = "dB";
    in.gainLoDb = 0.0f;
    in.gainHiDb = 76.0f;
    in.gainDb = 30.0f;
    s.update(none, in, nullptr);
    CHECK(!s.frame().knobDead);
    CHECK(s.frame().knobReadout == "30 dB" && s.frame().knobNote == "PGA");
    const int ticks[] = {0, 19, 38, 57, 76};
    for (int i = 0; i < 5; ++i) { CHECK(s.frame().knobTicks[i] == ticks[i]); }
    CHECK(s.frame().knobLit == 2);   // 30 / 76 * 4 = 1.6 -> the third
    CHECK_NEAR(s.frame().knobFrac, 30.0 / 76.0, 1e-6);
    // a stage measured in steps shows the bare figure
    RadarInputs steps = in;
    steps.gainUnit = "step";
    steps.gainDb = 7.0f;
    steps.gainHiDb = 14.0f;
    s.update(none, steps, nullptr);
    CHECK(s.frame().knobReadout == "7");
    steps.gainUnit = "";
    s.update(none, steps, nullptr);
    CHECK(s.frame().knobReadout == "7");
    // a stage that reports no travel falls back to 0..60
    RadarInputs flat = in;
    flat.gainLoDb = flat.gainHiDb = 0.0f;
    flat.gainDb = 45.0f;
    s.update(none, flat, nullptr);
    CHECK(s.frame().knobTicks[0] == 0 && s.frame().knobTicks[1] == 15 && s.frame().knobTicks[4] == 60 && s.frame().knobLit == 3);
    // lit tick clamps at the ends
    RadarInputs low = in;
    low.gainDb = -10.0f;
    s.update(none, low, nullptr);
    CHECK(s.frame().knobLit == 0);
    low.gainDb = 100.0f;
    s.update(none, low, nullptr);
    CHECK(s.frame().knobLit == 4);
    // AGC: dead, dark, AUTO
    RadarInputs agc = in;
    agc.deviceAgc = true;
    s.update(none, agc, nullptr);
    CHECK(s.frame().knobDead && s.frame().knobReadout == "AUTO" && s.frame().knobNote == "auto gain is on" && s.frame().knobLit == -1);
    CHECK(std::isnan(s.knobDetent(+1, agc)));

    // THE DETENT: two units, in the stage's unit, clamped to the travel
    RadarScope k = make(100);
    k.update(none, in, nullptr);
    CHECK(k.knobDetent(+1, in) == 32.0);
    // TWO DETENTS BEFORE THE READBACK ARE TWO DETENTS (the readback still says 30)
    CHECK(k.knobDetent(+1, in) == 34.0);
    CHECK(k.knobDetent(-3, in) == 28.0);   // 34 - 6
    // the knob shows the figure it sent while the readback has not caught up
    k.update(none, in, nullptr);
    CHECK(k.frame().knobReadout == "28 dB");
    // the readback arrives equal to what was sent: from there it is the truth
    RadarInputs caught = in;
    caught.gainDb = 28.0f;
    caught.nowS = 100.1;
    k.update(none, caught, nullptr);
    CHECK(k.frame().knobReadout == "28 dB");
    caught.gainDb = 40.0f;   // another source moved the stage: the readback is believed
    caught.nowS = 100.2;
    CHECK(k.knobDetent(+1, caught) == 42.0);
    // the kept figure lasts 0.5 s: a radio that quantised the command differently shows through after that
    RadarScope h = make(100);
    h.update(none, in, nullptr);
    CHECK(h.knobDetent(+1, in) == 32.0);   // sent at 100.0, readback still 30
    RadarInputs later = in;
    later.nowS = 100.4;
    CHECK(h.knobDetent(+1, later) == 34.0);
    later.nowS = 100.95;   // 0.55 s after the second send
    CHECK(h.knobDetent(+1, later) == 32.0);   // back to the readback (30) + 2
    // a different first stage is not the kept figure
    RadarScope st = make(100);
    st.update(none, in, nullptr);
    CHECK(st.knobDetent(+1, in) == 32.0);
    RadarInputs other = in;
    other.gainStage = "LNA";
    CHECK(st.knobDetent(+1, other) == 32.0);   // 30 + 2, not 34
    // clamped at both ends; no change = nothing to send
    RadarInputs top = in;
    top.gainDb = 75.0f;
    RadarScope tp = make(100);
    CHECK(tp.knobDetent(+1, top) == 76.0);
    RadarInputs atTop = in;
    atTop.gainDb = 76.0f;
    RadarScope tp2 = make(100);
    CHECK(std::isnan(tp2.knobDetent(+1, atTop)));
    CHECK(tp2.knobDetent(+100, in) == 76.0);   // clamped from 30
    RadarInputs bottom = in;
    bottom.gainDb = 0.0f;
    RadarScope bt = make(100);
    CHECK(std::isnan(bt.knobDetent(-1, bottom)));
    CHECK(bt.knobDetent(-100, in) == 0.0);
    // zero steps, a dead knob and AGC send nothing and keep nothing
    RadarScope z = make(100);
    CHECK(std::isnan(z.knobDetent(0, in)));
    CHECK(std::isnan(z.knobDetent(+1, agc)));
    RadarInputs nodev = in;
    nodev.deviceOpen = false;
    CHECK(std::isnan(z.knobDetent(+1, nodev)));
    CHECK(z.knobDetent(+1, in) == 32.0);   // nothing was kept by the refused ones
    // a stage in steps: two steps a detent
    RadarScope sp = make(100);
    CHECK(sp.knobDetent(+1, steps) == 9.0);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void dirtyAndSweep() {
    RadarScope s = make(100);
    CHECK(s.dirty());   // a fresh scope has never been painted
    std::vector<HostTrack> tr = {at("A", 0, 40, 3000.0, "A")};
    s.update(tr, inputs(), nullptr);
    CHECK(s.dirty());
    s.markPainted();
    CHECK(!s.dirty());
    // the same inputs again: the picture is the same
    s.update(tr, inputs(), nullptr);
    CHECK(!s.dirty());
    // a target that moved less than 50 m changes no ground: targets are Slint items, not part of the picture
    moveNorth(tr[0], 20.0);
    s.update(tr, inputs(), nullptr);
    CHECK(!s.dirty());
    // a new trail point does
    moveNorth(tr[0], 100.0);
    s.update(tr, inputs(), nullptr);
    CHECK(s.dirty());
    s.markPainted();
    // gestures and options do
    s.wheel(+1);
    CHECK(s.dirty());
    s.markPainted();
    s.update(tr, inputs(), nullptr);
    CHECK(s.dirty());   // (the range moved the polar frame)
    s.markPainted();
    s.cycleOption(0);
    CHECK(s.dirty());
    s.update(tr, inputs(), nullptr);
    s.markPainted();
    s.setTubeSize(600, 600);
    CHECK(s.dirty());
    s.markPainted();
    s.setTubeSize(600, 600);   // unchanged
    CHECK(!s.dirty());
    // the sweep: one turn in 4.2 s, clockwise from north
    CHECK(radarSweepDegrees(0.0) == 0.0f);
    CHECK_NEAR(radarSweepDegrees(1.05), 90.0, 0.01);
    CHECK_NEAR(radarSweepDegrees(2.1), 180.0, 0.01);
    CHECK_NEAR(radarSweepDegrees(3.15), 270.0, 0.01);
    CHECK_NEAR(radarSweepDegrees(4.2 + 1.05), 90.0, 0.01);
    CHECK_NEAR(radarSweepDegrees(4.2 * 1000.0 + 2.1), 180.0, 0.05);
    CHECK(radarSweepDegrees(-1.0) == 0.0f);
    CHECK(radarSweepDegrees(std::nan("")) == 0.0f);
    CHECK(radarSweepDegrees(4.2) < 0.01f || radarSweepDegrees(4.2) > 359.99f);
    // the classic's rule, read from its source
    const std::string sv = slurp(srcPath("gui/scope_view.cpp"));
    CHECK(sv.find("std::fmod(ImGui::GetTime() / 4.2, 1.0) * 360.0") != std::string::npos);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// the painter
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
std::vector<Rgba> paint(const RadarScope& s, int w, int h, std::size_t* strokes = nullptr) {
    std::vector<Rgba> px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), Rgba{1, 2, 3, 4});
    RadarPaintScratch scratch;
    const std::size_t n = paintRadarGround(px.data(), w, h, s, scratch);
    if (strokes != nullptr) { *strokes = n; }
    return px;
}
const Rgba& pxAt(const std::vector<Rgba>& b, int w, int x, int y) { return b[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]; }
bool same(const std::vector<Rgba>& a, const std::vector<Rgba>& b) { return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(Rgba)) == 0; }
std::size_t differing(const std::vector<Rgba>& a, const std::vector<Rgba>& b) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) { if (std::memcmp(&a[i], &b[i], sizeof(Rgba)) != 0) ++n; }
    return n;
}
// the mean green across twelve bearings (15 + 30 k degrees: clear of every tick and of the crosshair) at distance d from the middle
double meanGreen(const std::vector<Rgba>& b, int w, double d) {
    double sum = 0;
    for (int k = 0; k < 12; ++k) {
        const double a = (15.0 + 30.0 * k) * kPi / 180.0;
        sum += pxAt(b, w, static_cast<int>(std::floor(350.0 + d * std::sin(a))), static_cast<int>(std::floor(350.0 - d * std::cos(a)))).g;
    }
    return sum / 12.0;
}
double ringBoost(const std::vector<Rgba>& b, int w, double r) {
    double peak = 0;
    for (double d : {-1.5, -0.5, 0.5, 1.5}) { peak = std::max(peak, meanGreen(b, w, r + d)); }
    return peak - 0.5 * (meanGreen(b, w, r - 8.0) + meanGreen(b, w, r + 8.0));
}
// pixels inside 0.9 of the disc's radius (the rim's own pixels, where an outer ring and a cardinal tick overlap, are as bright as a coast)
std::size_t countCoast(const std::vector<Rgba>& b, int w, bool (*pred)(const Rgba&)) {
    std::size_t n = 0;
    for (int y = 0; y < w; ++y) {
        for (int x = 0; x < w; ++x) {
            if (std::hypot(x + 0.5 - 350.0, y + 0.5 - 350.0) < 0.9 * 348.0 && pred(pxAt(b, w, x, y))) { ++n; }
        }
    }
    return n;
}

void painter() {
    const int W = 700;
    // a scope in the middle of Kansas: no coast within reach of a 10 NM picture, so the ground is the plain glass
    RadarInputs kansas = inputs();
    kansas.rxLatDeg = 38.5;
    kansas.rxLonDeg = -98.0;
    RadarScope plain = make(10);
    plain.update({}, kansas, nullptr);
    std::size_t strokesOn = 0;
    const std::vector<Rgba> on = paint(plain, W, W, &strokesOn);
    // deterministic: the same inputs, the same bytes (and a fresh scratch makes no difference)
    CHECK(same(on, paint(plain, W, W)));
    CHECK(strokesOn > 0);
    // THE TUBE IS A DISC on the glass: the corners are transparent, the inside opaque
    CHECK(pxAt(on, W, 0, 0).a == 0 && pxAt(on, W, W - 1, 0).a == 0 && pxAt(on, W, 0, W - 1).a == 0 && pxAt(on, W, W - 1, W - 1).a == 0);
    CHECK(pxAt(on, W, 350, 350).a == 255 && pxAt(on, W, 350, 12).a == 255 && pxAt(on, W, 30, 350).a == 255);
    CHECK(pxAt(on, W, 350, 0).a == 0 && pxAt(on, W, 0, 350).a == 0);   // beyond the rim's margin (350 px out; the disc's mask is 349 + half a pixel)
    CHECK(pxAt(on, W, 350, 2).a == 255);                                 // 348 px out: the outer ring's own pixels
    // the PHOSPHOR wash: a plain point of the glass (no ring, tick, crosshair or scan line on it) is greener with it than without
    RadarScope off = make(10);
    off.cycleOption(0);
    off.update({}, kansas, nullptr);
    CHECK(!off.optPhosphor());
    const std::vector<Rgba> offPx = paint(off, W, W);
    // a pixel on row 300 (300 % 3 == 0: no scan line), at about bearing 20 degrees, 0.37 of the radius out
    const int px0 = static_cast<int>(std::floor(350.0 + 130.0 * std::sin(20.0 * kPi / 180.0))), py0 = static_cast<int>(std::floor(350.0 - 130.0 * std::cos(20.0 * kPi / 180.0)));
    const int py = py0 - (py0 % 3);   // a row that has no scan line (rows 1, 4, 7 ... are the dark ones)
    const Rgba withWash = pxAt(on, W, px0, py), without = pxAt(offPx, W, px0, py);
    CHECK(without.r == 10 && without.g == 28 && without.b == 13 && without.a == 255);   // the plain dark ground
    CHECK(withWash.g > without.g + 10 && withWash.r > without.r && withWash.a == 255);
    CHECK(differing(on, offPx) > 100000);   // the wash is everywhere

    // the RINGS at 1/4, 1/2, 3/4 and the full radius of the disc, the outer one brighter; none in between
    CHECK(ringBoost(offPx, W, kR * 0.25) > 6.0);
    CHECK(ringBoost(offPx, W, kR * 0.50) > 6.0);
    CHECK(ringBoost(offPx, W, kR * 0.75) > 6.0);
    CHECK(ringBoost(offPx, W, kR) > 20.0);
    CHECK(ringBoost(offPx, W, kR) > 2.0 * ringBoost(offPx, W, kR * 0.5));
    CHECK(ringBoost(offPx, W, kR * 0.375) < 3.0);
    CHECK(ringBoost(offPx, W, kR * 0.625) < 3.0);
    CHECK(ringBoost(offPx, W, kR * 0.875) < 3.0);
    // the twelve bearing ticks: at 30 degrees a tick stands inside the rim, none at 15; the cardinals' reach further in
    const auto green = [&](const std::vector<Rgba>& b, double bearing, double d) {
        const double a = bearing * kPi / 180.0;
        return static_cast<int>(pxAt(b, W, static_cast<int>(std::floor(350.0 + d * std::sin(a))), static_cast<int>(std::floor(350.0 - d * std::cos(a)))).g);
    };
    const double dTick = kR * 0.985;
    CHECK(green(offPx, 30, dTick) + green(offPx, 150, dTick) > green(offPx, 15, dTick) + green(offPx, 165, dTick) + 12);
    const double dCard = kR * 0.96;   // beyond the minor ticks' reach (0.975) inside the cardinals' (0.955)
    CHECK(green(offPx, 90, dCard) + green(offPx, 270, dCard) > green(offPx, 60, dCard) + green(offPx, 300, dCard) + 12);
    // the crosshair through the middle of the view
    CHECK(pxAt(offPx, W, 350, 350 - 100).g > pxAt(offPx, W, 352, 350 - 100).g + 10);
    // the scan lines: every third row is darker
    CHECK(pxAt(offPx, W, 310, 301).g < pxAt(offPx, W, 310, 300).g);
    CHECK(pxAt(offPx, W, 310, 304).g < pxAt(offPx, W, 310, 303).g);

    // TARGETS ARE NOT PAINTED: they are Slint items. An aircraft with no history paints the same ground.
    RadarScope withTarget = make(10);
    std::vector<HostTrack> one = {mk("T", 38.5 + 0.03, -98.0 + 0.03, 3000.0, "TTT")};
    withTarget.update(one, kansas, nullptr);
    CHECK(withTarget.frame().targets.size() == 1);
    CHECK(same(paint(withTarget, W, W), on));
    // and emergencies, labels and selections do not paint either
    withTarget.setSelection("T");
    withTarget.update(one, kansas, nullptr);
    CHECK(same(paint(withTarget, W, W), on));

    // THE PANNED VIEW moves the ground and the trails, not the instrument: on plain ground the rings and ticks are byte for byte where they were
    RadarScope panned = make(10);
    panned.update({}, kansas, nullptr);
    panned.dragBy(80, 20);
    panned.update({}, kansas, nullptr);
    CHECK(panned.frame().panned && panned.frame().rxMarkX > kCx + 40.0f);
    CHECK(same(paint(panned, W, W), on));
    // ...whereas the coast moves: a Cornish scope at 100 NM
    RadarInputs cornwall = inputs();
    cornwall.rxLatDeg = 50.0;
    cornwall.rxLonDeg = -5.0;
    RadarScope coast = make(100);
    coast.update({}, cornwall, nullptr);
    std::size_t coastStrokes = 0, offStrokes = 0;
    const std::vector<Rgba> coastPx = paint(coast, W, W, &coastStrokes);
    RadarScope coastOff = make(100);
    coastOff.cycleOption(0);
    coastOff.update({}, cornwall, nullptr);
    const std::vector<Rgba> coastOffPx = paint(coastOff, W, W, &offStrokes);
    CHECK(coastStrokes > offStrokes + 10);   // the coastline's segments
    const auto isCoast = [](const Rgba& p) { return p.g > 110 && p.r < 120 && p.b < 90 && p.a == 255; };
    CHECK(countCoast(coastPx, W, isCoast) > 200);
    CHECK(countCoast(coastOffPx, W, isCoast) == 0);   // PHOSPHOR off: the plain dark ground, no coast
    coast.dragBy(150, 0);
    coast.update({}, cornwall, nullptr);
    const std::vector<Rgba> coastPanned = paint(coast, W, W);
    CHECK(differing(coastPx, coastPanned) > 1000);
    CHECK(coast.frame().panned);
    // the rings are still centred on the tube after a pan
    CHECK(ringBoost(coastOffPx, W, kR * 0.5) > 6.0);
    coastOff.dragBy(150, 0);
    coastOff.update({}, cornwall, nullptr);
    CHECK(same(paint(coastOff, W, W), coastOffPx));

    // the empty state: the dark glass alone
    RadarScope empty = make(10);
    RadarInputs noRx = inputs();
    noRx.hasRx = false;
    empty.update({}, noRx, nullptr);
    std::size_t emptyStrokes = 99;
    const std::vector<Rgba> emptyPx = paint(empty, W, W, &emptyStrokes);
    CHECK(emptyStrokes == 0);
    CHECK(pxAt(emptyPx, W, 350, 350).g == 28 && pxAt(emptyPx, W, 350, 350).a == 255);
    CHECK(pxAt(emptyPx, W, 5, 5).a == 0);
    // a buffer that is not square: the disc is the shorter side, centred
    RadarScope wide;
    wide.setTubeSize(900, 700);
    wide.setRangeNm(10);
    wide.update({}, kansas, nullptr);
    const std::vector<Rgba> widePx = paint(wide, 900, 700);
    CHECK(pxAt(widePx, 900, 450, 350).a == 255 && pxAt(widePx, 900, 450 - 340, 350).a == 255 && pxAt(widePx, 900, 450 - 360, 350).a == 0 && pxAt(widePx, 900, 0, 0).a == 0);
    // bad arguments
    RadarPaintScratch scratch;
    CHECK(paintRadarGround(nullptr, 100, 100, plain, scratch) == 0);
    std::vector<Rgba> tiny(4);
    CHECK(paintRadarGround(tiny.data(), 0, 100, plain, scratch) == 0);
    CHECK(paintRadarGround(tiny.data(), -4, -4, plain, scratch) == 0);
}

void painterTrails() {
    const int W = 700;
    RadarInputs kansas = inputs();
    kansas.rxLatDeg = 38.5;
    kansas.rxLonDeg = -98.0;
    kansas.trailWidthPx = 4;
    // an aircraft at 36000 ft crossing the east side of the disc, northbound: eleven fixes 0.5 NM apart
    std::vector<HostTrack> tr = {mk("T", 38.5 - 0.03, -98.0 + 0.04, 11000.0, "TTT")};
    RadarScope line = make(10);
    RadarScope bare = make(10);
    bare.update({}, kansas, nullptr);
    for (int i = 0; i < 12; ++i) {
        line.update(tr, kansas, nullptr);
        tr[0].t.latDeg += 0.5 / 60.0;
    }
    CHECK(line.trails().size() == 1 && line.trails()[0].points.size() == 12);
    std::size_t sLine = 0, sBare = 0;
    const std::vector<Rgba> withTrail = paint(line, W, W, &sLine);
    const std::vector<Rgba> noTrail = paint(bare, W, W, &sBare);
    CHECK(sLine == sBare + 11);   // eleven legs
    const std::size_t lineDiff = differing(withTrail, noTrail);
    CHECK(lineDiff > 300);
    // the colour of the altitude band along the trail: 36090 ft is the > 30 kft band, indigo (130,140,255): blue far above the glass's own
    bool indigo = false;
    for (const RadarTrail::Point& p : line.trails()[0].points) {
        const int x = static_cast<int>(std::round(p.x)), y = static_cast<int>(std::round(p.y));
        if (x >= 0 && y >= 0 && x < W && y < W && pxAt(withTrail, W, x, y).b > 120) { indigo = true; }
    }
    CHECK(indigo);
    // RIBBON is the same path drawn 3 px wider
    line.cycleOption(2);
    line.update(tr, kansas, nullptr);
    CHECK(line.optTrail() == 2);
    const std::vector<Rgba> ribbon = paint(line, W, W);
    CHECK(differing(ribbon, noTrail) > lineDiff * 3 / 2);
    // OFF: no trail pixels at all, though the points are still kept
    line.cycleOption(2);
    line.update(tr, kansas, nullptr);
    CHECK(line.optTrail() == 0 && line.trailPointCount() >= 12);
    std::size_t sOff = 0;
    const std::vector<Rgba> trailsOff = paint(line, W, W, &sOff);
    CHECK(same(trailsOff, noTrail));
    CHECK(sOff == sBare);
    // the selected aircraft's trail is a little wider
    line.cycleOption(2);   // LINE
    line.setSelection("T");
    line.update(tr, kansas, nullptr);
    const std::vector<Rgba> picked = paint(line, W, W);
    CHECK(differing(picked, noTrail) > lineDiff);
    // the user's width: a thinner trail paints fewer pixels
    RadarInputs thin = kansas;
    thin.trailWidthPx = 1;
    line.clearSelection();
    line.update(tr, thin, nullptr);
    CHECK(differing(paint(line, W, W), noTrail) < lineDiff);
    // painting twice paints the same bytes
    CHECK(same(paint(line, W, W), paint(line, W, W)));
}

void sweepFan() {
    const int N = 400;
    std::vector<Rgba> a(static_cast<std::size_t>(N) * N, Rgba{7, 7, 7, 7}), b(a.size());
    paintSweepFan(a.data(), N);
    paintSweepFan(b.data(), N);
    CHECK(same(a, b));
    const double c = N * 0.5;
    const auto alphaAt = [&](double bearingDeg, double r) {
        const double t = bearingDeg * kPi / 180.0;
        return static_cast<int>(pxAt(a, N, static_cast<int>(std::floor(c + r * std::sin(t))), static_cast<int>(std::floor(c - r * std::cos(t)))).a);
    };
    // the leading edge is straight up; the tail lies 55 degrees ANTICLOCKWISE of it (the fan turns clockwise); nothing beyond either edge
    CHECK(alphaAt(-3, 150) > 60);
    CHECK(alphaAt(-3, 150) <= 77);
    CHECK(alphaAt(-25, 150) > 20 && alphaAt(-25, 150) < alphaAt(-3, 150));
    CHECK(alphaAt(-50, 150) > 0 && alphaAt(-50, 150) < alphaAt(-25, 150));   // alpha fades towards the tail
    CHECK(alphaAt(-58, 150) == 0 && alphaAt(-80, 150) == 0 && alphaAt(-180, 150) == 0);
    CHECK(alphaAt(+3, 150) == 0 && alphaAt(+30, 150) == 0 && alphaAt(+90, 150) == 0 && alphaAt(+170, 150) == 0);
    // the whole wedge is one colour (the phosphor green) and lies inside the disc
    CHECK(pxAt(a, N, 140, 100).r == 134 && pxAt(a, N, 140, 100).g == 214 && pxAt(a, N, 140, 100).b == 74);
    CHECK(alphaAt(-20, 205) == 0);   // beyond the radius of N / 2
    CHECK(alphaAt(-20, 190) > 0);
    CHECK(pxAt(a, N, 0, 0).a == 0 && pxAt(a, N, N - 1, N - 1).a == 0);
    // steps of the 44-step ramp: the alpha is flat inside a step (1.25 degrees) and falls between steps
    CHECK(alphaAt(-10.2, 150) == alphaAt(-10.9, 150) || alphaAt(-10.2, 150) == alphaAt(-10.9, 150) + 1);
    // bad arguments
    paintSweepFan(nullptr, 10);
    paintSweepFan(a.data(), 0);
    std::vector<Rgba> one(1);
    paintSweepFan(one.data(), 1);
}

void cost() {
    // 40 aircraft, each with a full 256-point trail, a 200 NM scope on a 700 px tube
    RadarScope s;
    s.setTubeSize(700, 700);
    s.setRangeNm(200);
    std::vector<HostTrack> tr;
    for (int i = 0; i < 40; ++i) {
        char id[16];
        std::snprintf(id, sizeof(id), "P%02d", i);
        tr.push_back(at(id, i * 9.0, 40.0 + (i % 8) * 12.0, 8000.0 + i * 100.0, id));
    }
    RadarInputs in = inputs();
    in.trailWidthPx = 4;
    const double t0 = nowMs();
    for (int step = 0; step < 300; ++step) {
        s.update(tr, in, nullptr);
        for (HostTrack& t : tr) { moveNorth(t, 120.0); }
    }
    const double updateMs = (nowMs() - t0) / 300.0;
    CHECK(s.trailPointCount() == 40u * 256u);
    std::vector<Rgba> px(700u * 700u);
    RadarPaintScratch scratch;
    std::size_t strokes = 0;
    double best = 1e9;
    for (int i = 0; i < 5; ++i) {
        const double a = nowMs();
        strokes = paintRadarGround(px.data(), 700, 700, s, scratch);
        best = std::min(best, nowMs() - a);
    }
    std::printf("radar cost: paintRadarGround 700x700, 40 tracks x 256-point trails, %zu strokes: %.2f ms (best of 5); update() with that load: %.3f ms a tick\n", strokes, best, updateMs);
    CHECK(best < 150.0);
    CHECK(strokes > 40u * 255u);
    // the fan is painted once
    std::vector<Rgba> fan(700u * 700u);
    const double b0 = nowMs();
    paintSweepFan(fan.data(), 700);
    std::printf("radar cost: paintSweepFan 700: %.2f ms\n", nowMs() - b0);
    CHECK(nowMs() - b0 < 150.0);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash must not eat the lines before it
    ladderAndWheel();
    dragPansTheView();
    clicks();
    whatIsPlotted();
    filters();
    emptyAndDegenerate();
    trails();
    lcd();
    cabinet();
    knob();
    dirtyAndSweep();
    painter();
    painterTrails();
    sweepFan();
    cost();
    return testSummary("test_slint_host_radar");
}
