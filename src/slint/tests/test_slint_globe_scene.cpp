// The globe views' geometry (src/slint/globe_scene.*, docs 6.65): no Slint, no window, no engine. Every figure is held against a published value, named at the check:
//   A. the sphere: lat/lon <-> vector, the axes, the poles, the longitude wrap
//   B. the coastline, graticule and small circles as 3D polylines (the data's own 134 runs and 5127 points)
//   C. the land mask from the flat map's rings (land and sea at named places, the world's land fraction, the antimeridian)
//   D. the Sun: Meeus Example 25.a, the USNO's 2025 equinox and solstice, Greenwich sidereal time (Meeus Example 12.b), the equation of time's November maximum
//   E. the terminator and the greyline band
//   F. great circles: London-New York, London-Sydney, bearings, the inverse, the arcs, the skywave hops
//   G. satellites: the footprint (the geostationary disc's 81.3 deg), the sub-satellite point, the look angle
//   H. Maidenhead locators, the inverse of the host's own maidenheadGrid6
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "globe_scene.hpp"
#include "map_compositor.hpp"
#include "core/maidenhead.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace foxsdr::globe;

namespace {

constexpr double kLondonLat = 51.5074, kLondonLon = -0.1278;     // the app's own sample for IO91wm (core/maidenhead.hpp)
const LatLon kLondon{kLondonLat, kLondonLon};
const LatLon kNewYork{40.7128, -74.0060};
const LatLon kSydney{-33.8688, 151.2093};

double angleBetweenDeg(const Vec3& a, const Vec3& b) {
    return std::acos(std::clamp(dot(normalized(a), normalized(b)), -1.0, 1.0)) * kRadToDeg;
}

// ------------------------------------------------------------------------------------------------------------------------
void sectionSphere() {
    // the axes
    CHECK_NEAR(toUnit(0, 0).x, 1.0, 1e-12);
    CHECK_NEAR(toUnit(0, 90).y, 1.0, 1e-12);
    CHECK_NEAR(toUnit(90, 0).z, 1.0, 1e-12);
    CHECK_NEAR(toUnit(-90, 33).z, -1.0, 1e-12);
    // a round trip across the whole sphere, 3 degrees apart
    int bad = 0, n = 0;
    for (int la = -90; la <= 90; la += 3) {
        for (int lo = -179; lo <= 180; lo += 3) {
            const Vec3 v = toUnit(la, lo);
            double a = 0, b = 0;
            CHECK(fromVec(v, a, b));
            ++n;
            if (std::fabs(a - la) > 1e-9) { ++bad; continue; }
            if (std::fabs(la) < 90 && std::fabs(normalizeLon(b - lo)) > 1e-9) { ++bad; }
        }
    }
    CHECK(n > 7000);
    CHECK(bad == 0);
    // a vector's length does not matter; a zero or non-finite one is refused and leaves the outputs alone
    double a = 11, b = 22;
    CHECK(fromVec(toUnit(10, 20) * 6371.0, a, b));
    CHECK_NEAR(a, 10.0, 1e-9);
    CHECK_NEAR(b, 20.0, 1e-9);
    a = 11; b = 22;
    CHECK(!fromVec(Vec3{0, 0, 0}, a, b));
    CHECK(!fromVec(Vec3{std::nan(""), 0, 1}, a, b));
    CHECK(a == 11.0 && b == 22.0);
    // the poles have no longitude: 0, not noise
    CHECK(fromVec(toUnit(90, 77), a, b));
    CHECK_NEAR(a, 90.0, 1e-9);
    CHECK_NEAR(b, 0.0, 1e-9);
    // the wrap
    CHECK_NEAR(normalizeLon(190), -170.0, 1e-12);
    CHECK_NEAR(normalizeLon(-190), 170.0, 1e-12);
    CHECK_NEAR(normalizeLon(180), 180.0, 1e-12);
    CHECK_NEAR(normalizeLon(-180), 180.0, 1e-12);
    CHECK_NEAR(normalizeLon(720.5), 0.5, 1e-9);
    // a zero vector normalises to something finite
    const Vec3 z = normalized(Vec3{0, 0, 0});
    CHECK(std::isfinite(z.x) && std::isfinite(z.y) && std::isfinite(z.z));
    CHECK_NEAR(length(z), 1.0, 1e-12);
}

// ------------------------------------------------------------------------------------------------------------------------
void sectionPolylines() {
    const std::vector<Polyline> coast = coastline(1.0);
    CHECK(coast.size() == 134u);                    // gui/coastline_data.hpp kRunCount
    std::size_t points = 0;
    bool unit = true;
    for (const Polyline& l : coast) {
        points += l.size();
        for (const Vec3& p : l) { if (std::fabs(length(p) - 1.0) > 1e-9) { unit = false; } }
    }
    CHECK(points == 5127u);                         // kPointCount
    CHECK(unit);
    // the first point of the first run is -163.71 lon, -78.60 lat (the data's first pair)
    const LatLon first = fromVec(coast[0][0]);
    CHECK_NEAR(first.lon, -163.71, 1e-6);
    CHECK_NEAR(first.lat, -78.60, 1e-6);
    // a radius scales every point
    const std::vector<Polyline> big = coastline(2.5);
    CHECK_NEAR(length(big[10][3]), 2.5, 1e-9);

    // the graticule at 15 degrees: 24 meridians pole to pole, the equator and 5 parallels either side
    const std::vector<Polyline> g = graticule(15.0, 5.0, 1.0);
    CHECK(g.size() == 24u + 11u);
    CHECK_NEAR(fromVec(g[0].front()).lat, -90.0, 1e-9);
    CHECK_NEAR(fromVec(g[0].back()).lat, 90.0, 1e-9);
    int equators = 0, meridians = 0;
    for (const Polyline& l : g) {
        double minLat = 91, maxLat = -91;
        for (const Vec3& p : l) {
            CHECK_NEAR(length(p), 1.0, 1e-9);
            const double lat = fromVec(p).lat;
            minLat = std::min(minLat, lat);
            maxLat = std::max(maxLat, lat);
        }
        if (maxLat - minLat < 1e-6 && std::fabs(maxLat) < 1e-6) { ++equators; }
        if (maxLat - minLat > 179.0) { ++meridians; }
    }
    CHECK(equators == 1);
    CHECK(meridians == 24);
    // the clamps: a silly step is a usable one
    CHECK(!graticule(0.0, 0.0, 1.0).empty());
    CHECK(graticule(500.0, 500.0, 1.0).size() >= 4u);

    // a small circle of 30 degrees about London: closed, every point 30 degrees from the centre, 17 points for 16 samples
    const Polyline c = smallCircle(kLondon, 30.0, 16, 1.0);
    CHECK(c.size() == 17u);
    CHECK_NEAR(c.front().x, c.back().x, 1e-12);
    CHECK_NEAR(c.front().y, c.back().y, 1e-12);
    for (const Vec3& p : c) { CHECK_NEAR(angleBetweenDeg(p, toUnit(kLondon)), 30.0, 1e-9); }
    // about the pole it still works, and fewer than 8 samples is 8
    const Polyline pc = smallCircle(LatLon{90.0, 0.0}, 10.0, 3, 1.0);
    CHECK(pc.size() == 9u);
    for (const Vec3& p : pc) { CHECK_NEAR(fromVec(p).lat, 80.0, 1e-9); }
}

// ------------------------------------------------------------------------------------------------------------------------
void sectionLand() {
    const LandMask m(1440, 720);
    CHECK(!m.empty());
    CHECK(m.width() == 1440 && m.height() == 720);
    // the premise of the wrap: some ring of the flat map runs past +/-180 (Eurasia's Chukotka tip, Antarctica), so the mask must draw its copies a world away
    int pastTheEdge = 0;
    for (const auto& r : foxsdr::slintmap::coastlineRings()) {
        double lo = 1e9, hi = -1e9;
        for (double x : r.lon) { lo = std::min(lo, x); hi = std::max(hi, x); }
        if (hi > 180.0 || lo < -180.0) { ++pastTheEdge; }
    }
    CHECK(pastTheEdge >= 1);
    // places that are land
    CHECK(m.land(51.5, -0.1));        // London
    CHECK(m.land(23.0, 10.0));        // the Sahara
    CHECK(m.land(-25.0, 134.0));      // central Australia
    CHECK(m.land(-80.0, 0.0));        // Antarctica
    CHECK(m.land(40.0, -100.0));      // the American midwest
    CHECK(m.land(64.0, -45.0));       // Greenland's ice
    CHECK(m.land(55.0, 90.0));        // Siberia
    CHECK(m.land(65.0, 175.0));       // Chukotka, west of the antimeridian
    CHECK(m.land(65.0, -155.0));      // Alaska, east of it
    CHECK(m.land(-16.5, 179.2));      // Vanua Levu (Fiji): its ring runs to 181.4 W-equivalent, so the +360 copy fills the east side of the map
    CHECK(m.land(66.0, -171.0));      // Chukotka's far end: east of the antimeridian by longitude, in the Eurasian ring that runs past +180 (East Cape is 169.65 W)
    // places that are sea
    CHECK(!m.land(45.0, -30.0));      // the North Atlantic
    CHECK(!m.land(0.0, -150.0));      // the Pacific
    CHECK(!m.land(-40.0, 80.0));      // the southern Indian Ocean
    CHECK(!m.land(60.0, 0.0));        // the North Sea's north
    CHECK(!m.land(89.0, 0.0));        // the Arctic Ocean
    // the world's land fraction: the Earth is about 29 % land; the 1:110m coast with Antarctica and without the lakes lands within 3 points of that
    std::size_t landCells = 0;
    for (std::uint8_t v : m.cells()) { if (v) { ++landCells; } }
    // area-weight the rows (a cell near a pole is a smaller piece of the sphere)
    double landArea = 0.0, totalArea = 0.0;
    for (int r = 0; r < m.height(); ++r) {
        const double lat = 90.0 - (r + 0.5) * 180.0 / m.height();
        const double wgt = std::cos(lat * kDegToRad);
        for (int c = 0; c < m.width(); ++c) {
            totalArea += wgt;
            if (m.cells()[static_cast<std::size_t>(r) * m.width() + c]) { landArea += wgt; }
        }
    }
    CHECK(landCells > 100000u);
    CHECK(landArea / totalArea > 0.26);
    CHECK(landArea / totalArea < 0.32);
    // the antimeridian is seamless: the same latitude either side of +/-180 is land in both halves of Chukotka-Alaska's neighbourhood only where the coast says so,
    // and the row's first and last cell are never a one-cell stripe of the wrong value across the whole planet
    int stripes = 0;
    for (int r = 0; r < m.height(); ++r) {
        const bool a = m.cells()[static_cast<std::size_t>(r) * m.width()] != 0;
        const bool b = m.cells()[static_cast<std::size_t>(r) * m.width() + m.width() - 1] != 0;
        const bool c = m.cells()[static_cast<std::size_t>(r) * m.width() + 1] != 0;
        if (a != b && a != c) { ++stripes; }
    }
    CHECK(stripes < 6);
    // out of range and unbuilt masks are honest
    CHECK(!LandMask().land(0, 0));
    CHECK(m.land(-95.0, 0.0) == m.land(-90.0, 0.0));
    CHECK(!m.land(std::nan(""), 0.0));
    CHECK(m.land(51.5, 359.9) == m.land(51.5, -0.1));
    LandMask empty(0, 0);
    CHECK(empty.empty());
}

// ------------------------------------------------------------------------------------------------------------------------
void sectionSun() {
    // Meeus, Astronomical Algorithms 2nd ed., Example 25.a: 1992 October 13, 0h: apparent right ascension 198.38083 deg, declination -7.78507 deg
    // (also in the R package oce's tests of sunDeclinationRightAscension, which cites it as Example 24.a of the first edition). The low-precision
    // formulae claim 0.01 deg; the check allows 0.02.
    const double jdMeeus = julianDay(1992, 10, 13, 0, 0, 0.0);
    CHECK_NEAR(jdMeeus, 2448908.5, 1e-9);           // Meeus's own JD for that instant
    const SunPosition sm = sunPosition(jdMeeus);
    CHECK_NEAR(sm.declinationDeg, -7.78507, 0.02);
    CHECK_NEAR(sm.rightAscensionDeg, 198.38083, 0.02);
    CHECK_NEAR(sm.subLatDeg, sm.declinationDeg, 1e-12);
    CHECK_NEAR(normalizeLon(sm.subLonDeg - (sm.rightAscensionDeg - gmstDegrees(jdMeeus))), 0.0, 1e-9);

    // the USNO's 2025 seasons (Griffith Observatory's table of the same times): the March equinox 20 March 09:01 UT, the June solstice 21 June 02:42 UT
    const SunPosition eq = sunPosition(julianDay(2025, 3, 20, 9, 1, 0.0));
    CHECK_NEAR(eq.declinationDeg, 0.0, 0.03);       // the declination crosses zero at 0.39 deg/day: 0.03 deg is 2 minutes
    const SunPosition so = sunPosition(julianDay(2025, 6, 21, 2, 42, 0.0));
    CHECK_NEAR(so.declinationDeg, 23.44, 0.02);
    const SunPosition wi = sunPosition(julianDay(2025, 12, 21, 15, 3, 0.0));
    CHECK_NEAR(wi.declinationDeg, -23.44, 0.02);
    const SunPosition au = sunPosition(julianDay(2025, 9, 22, 18, 19, 0.0));
    CHECK_NEAR(au.declinationDeg, 0.0, 0.03);

    // Greenwich mean sidereal time, Meeus Example 12.b: 1987 April 10, 19h21m00s UT = 8h34m57.0896s = 128.7371 deg
    const double jd12b = julianDay(1987, 4, 10, 19, 21, 0.0);
    CHECK_NEAR(jd12b, 2446896.30625, 1e-8);
    CHECK_NEAR(gmstDegrees(jd12b), (8.0 + 34.0 / 60.0 + 57.0896 / 3600.0) * 15.0, 0.001);
    CHECK(gmstDegrees(jd12b) >= 0.0 && gmstDegrees(jd12b) < 360.0);

    // the equation of time's November maximum (+16 min 25 s about 3 November, the published extreme): at 12:00 UTC the Sun is that far past the Greenwich meridian
    // clock, so its sub-solar longitude is west of Greenwich by 16.4 min * 0.25 deg/min = 4.1 deg
    const SunPosition nov = sunPosition(julianDay(2025, 11, 3, 12, 0, 0.0));
    CHECK_NEAR(nov.subLonDeg, -4.1, 0.3);
    // and the February minimum (-14 min 15 s about 11 February): the Sun is 3.6 deg EAST of Greenwich at 12:00 UTC
    const SunPosition feb = sunPosition(julianDay(2025, 2, 11, 12, 0, 0.0));
    CHECK_NEAR(feb.subLonDeg, 3.6, 0.3);

    // a Unix time and the calendar agree: 2025-03-20 09:01:00 UTC is 1742461260
    CHECK_NEAR(julianDay(static_cast<std::int64_t>(1742461260)), julianDay(2025, 3, 20, 9, 1, 0.0), 1e-9);
    CHECK_NEAR(julianDay(static_cast<std::int64_t>(0)), 2440587.5, 1e-12);
    // J2000: 2000 Jan 1 12:00 is JD 2451545.0
    CHECK_NEAR(julianDay(2000, 1, 1, 12, 0, 0.0), 2451545.0, 1e-9);
    // a Julian date far outside the formulae's years is still finite
    const SunPosition far = sunPosition(julianDay(1850, 6, 1, 0, 0, 0.0));
    CHECK(std::isfinite(far.declinationDeg) && std::fabs(far.declinationDeg) < 24.0);
}

// ------------------------------------------------------------------------------------------------------------------------
void sectionTerminator() {
    const SunPosition s = sunPosition(julianDay(2025, 3, 20, 12, 0, 0.0));
    // the Sun is overhead at its own sub-solar point, and a point on the other side of the Earth is in the night
    const LatLon sub{s.subLatDeg, s.subLonDeg};
    CHECK_NEAR(solarElevationDeg(sub, s), 90.0, 1e-6);
    CHECK_NEAR(solarElevationDeg(LatLon{-sub.lat, normalizeLon(sub.lon + 180.0)}, s), -90.0, 1e-6);
    CHECK_NEAR(sinSolarElevation(sub, s), 1.0, 1e-12);
    // the terminator: every point at solar elevation 0 (and 90 deg from the sub-solar point)
    const Polyline t = terminator(s, 0.0, 180, 1.0);
    CHECK(t.size() == 181u);
    for (const Vec3& p : t) {
        const LatLon q = fromVec(p);
        CHECK_NEAR(solarElevationDeg(q, s), 0.0, 1e-6);
    }
    // the civil twilight's edge, -6 deg, and a greyline band's far edge, +6
    for (const Vec3& p : terminator(s, -6.0, 90, 1.0)) { CHECK_NEAR(solarElevationDeg(fromVec(p), s), -6.0, 1e-6); }
    for (const Vec3& p : terminator(s, 6.0, 90, 1.0)) { CHECK_NEAR(solarElevationDeg(fromVec(p), s), 6.0, 1e-6); }
    // at the equinox the terminator is (nearly) a meridian circle: it passes close to both poles
    double nearestToNorth = 180.0;
    for (const Vec3& p : t) { nearestToNorth = std::min(nearestToNorth, angleBetweenDeg(p, Vec3{0, 0, 1})); }
    CHECK(nearestToNorth < 1.0);
    // at the June solstice the Sun's night covers the whole south polar cap and none of the north's: the north pole is lit and the south pole is dark
    const SunPosition sol = sunPosition(julianDay(2025, 6, 21, 12, 0, 0.0));
    CHECK(solarElevationDeg(LatLon{90.0, 0.0}, sol) > 23.0);
    CHECK(solarElevationDeg(LatLon{-90.0, 0.0}, sol) < -23.0);
    // the daylight ramp: 1 above the band, 0 below, 0.5 at the terminator, monotonic, and a hard edge for a zero band
    CHECK_NEAR(daylight(std::sin(10.0 * kDegToRad), 6.0), 1.0, 1e-12);
    CHECK_NEAR(daylight(std::sin(-10.0 * kDegToRad), 6.0), 0.0, 1e-12);
    CHECK_NEAR(daylight(0.0, 6.0), 0.5, 1e-12);
    CHECK_NEAR(daylight(std::sin(3.0 * kDegToRad), 6.0), 0.84375, 1e-3);   // the smoothstep, not a straight ramp (0.75)
    double prev = -1.0;
    bool mono = true;
    for (int e = -8; e <= 8; ++e) {
        const double d = daylight(std::sin(e * kDegToRad), 6.0);
        if (d < prev - 1e-12) { mono = false; }
        prev = d;
    }
    CHECK(mono);
    CHECK(daylight(0.001, 0.0) == 1.0);
    CHECK(daylight(-0.001, 0.0) == 0.0);
    CHECK(daylight(std::nan(""), 6.0) == 0.0);
}

// ------------------------------------------------------------------------------------------------------------------------
void sectionGreatCircle() {
    // London to New York: about 5 570 km, initial bearing 288 deg (the redesign's sample table gives 5 570 km / 288; the sources searched agree on ~5,570 km)
    CHECK_NEAR(distanceKm(kLondon, kNewYork), 5570.0, 15.0);
    CHECK_NEAR(bearingDeg(kLondon, kNewYork), 288.3, 0.5);
    // London to Sydney: about 16 994 km (distancefyi / citydistance quote 16,994; 16,966 to 16,987 elsewhere), initial bearing about 61 deg (60.7-61 quoted)
    CHECK_NEAR(distanceKm(kLondon, kSydney), 16994.0, 60.0);
    CHECK_NEAR(bearingDeg(kLondon, kSydney), 61.0, 1.0);
    // symmetric distance, antipodes half the circumference, nothing at all
    CHECK_NEAR(distanceKm(kLondon, kNewYork), distanceKm(kNewYork, kLondon), 1e-9);
    CHECK_NEAR(distanceKm(LatLon{10, 20}, LatLon{-10, -160}), kPi * kEarthRadiusKm, 1e-3);
    CHECK_NEAR(distanceKm(kLondon, kLondon), 0.0, 1e-9);
    CHECK(bearingDeg(kLondon, kLondon) == 0.0);
    // the cardinal bearings
    CHECK_NEAR(bearingDeg(LatLon{0, 0}, LatLon{10, 0}), 0.0, 1e-9);
    CHECK_NEAR(bearingDeg(LatLon{0, 0}, LatLon{0, 10}), 90.0, 1e-9);
    CHECK_NEAR(bearingDeg(LatLon{0, 0}, LatLon{-10, 0}), 180.0, 1e-9);
    CHECK_NEAR(bearingDeg(LatLon{0, 0}, LatLon{0, -10}), 270.0, 1e-9);
    // one degree of latitude is 111.195 km on this sphere
    CHECK_NEAR(distanceKm(LatLon{0, 0}, LatLon{1, 0}), kEarthRadiusKm * kDegToRad, 1e-6);
    // destination undoes distance and bearing
    for (const LatLon& to : {kNewYork, kSydney, LatLon{-33.9, 18.4}, LatLon{35.7, 139.7}}) {
        const LatLon d = destination(kLondon, bearingDeg(kLondon, to), distanceKm(kLondon, to));
        CHECK_NEAR(d.lat, to.lat, 1e-6);
        CHECK_NEAR(normalizeLon(d.lon - to.lon), 0.0, 1e-6);
    }

    // the arc: both ends, the right count, on the sphere when not lifted, the middle at the half-way point
    const Polyline arc = greatCircleArc(kLondon, kNewYork, 33, 0.0);
    CHECK(arc.size() == 33u);
    CHECK_NEAR(angleBetweenDeg(arc.front(), toUnit(kLondon)), 0.0, 1e-9);
    CHECK_NEAR(angleBetweenDeg(arc.back(), toUnit(kNewYork)), 0.0, 1e-9);
    for (const Vec3& p : arc) { CHECK_NEAR(length(p), 1.0, 1e-12); }
    const LatLon mid = fromVec(arc[16]);
    CHECK_NEAR(distanceKm(kLondon, mid), distanceKm(kLondon, kNewYork) / 2.0, 0.5);
    CHECK_NEAR(distanceKm(mid, kNewYork), distanceKm(kLondon, kNewYork) / 2.0, 0.5);
    // lifted: ground at both ends, `lift` at the middle, never below the ground
    const Polyline up = greatCircleArc(kLondon, kNewYork, 33, 0.05);
    CHECK_NEAR(length(up.front()), 1.0, 1e-12);
    CHECK_NEAR(length(up.back()), 1.0, 1e-12);
    CHECK_NEAR(length(up[16]), 1.05, 1e-12);
    for (const Vec3& p : up) { CHECK(length(p) >= 1.0 - 1e-12); }
    // degenerate: the same point twice, antipodes, one sample
    const Polyline same = greatCircleArc(kLondon, kLondon, 5, 0.0);
    for (const Vec3& p : same) { CHECK(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)); }
    const Polyline anti = greatCircleArc(LatLon{0, 0}, LatLon{0, 180}, 9, 0.0);
    for (const Vec3& p : anti) { CHECK_NEAR(length(p), 1.0, 1e-9); }
    CHECK_NEAR(angleBetweenDeg(anti.back(), toUnit(0, 180)), 0.0, 1e-9);
    CHECK(greatCircleArc(kLondon, kNewYork, 1, 0.0).size() == 2u);

    // the skywave: London to Sydney in 5 hops of 4 000 km at most (hopCount), 300 km shell, drawn 4x as high as true
    CHECK(hopCount(distanceKm(kLondon, kNewYork), 4000.0) == 2);
    CHECK(hopCount(distanceKm(kLondon, kSydney), 4000.0) == 5);
    CHECK(hopCount(100.0, 4000.0) == 1);
    CHECK(hopCount(4000.0, 4000.0) == 1);
    CHECK(hopCount(4000.1, 4000.0) == 2);
    CHECK(hopCount(0.0, 4000.0) == 1);
    CHECK(hopCount(5000.0, 0.0) == 1);
    const int hops = 3, per = 9;
    const Polyline sky = skywaveArc(kLondon, kNewYork, hops, 300.0, 4.0, per);
    CHECK(sky.size() == static_cast<std::size_t>(hops * (per - 1) + 1));
    CHECK_NEAR(angleBetweenDeg(sky.front(), toUnit(kLondon)), 0.0, 1e-9);
    CHECK_NEAR(angleBetweenDeg(sky.back(), toUnit(kNewYork)), 0.0, 1e-9);
    const double apex = 1.0 + 300.0 * 4.0 / kEarthRadiusKm;
    for (int h = 0; h < hops; ++h) {
        CHECK_NEAR(length(sky[static_cast<std::size_t>(h * (per - 1))]), 1.0, 1e-9);                // a reflection point is on the ground
        CHECK_NEAR(length(sky[static_cast<std::size_t>(h * (per - 1) + (per - 1) / 2)]), apex, 1e-9);   // the middle of a hop is at the shell
    }
    // every hop covers an equal third of the ground distance
    double groundTotal = 0.0;
    for (int h = 0; h < hops; ++h) {
        const LatLon p0 = fromVec(sky[static_cast<std::size_t>(h * (per - 1))]);
        const LatLon p1 = fromVec(sky[static_cast<std::size_t>((h + 1) * (per - 1))]);
        CHECK_NEAR(distanceKm(p0, p1), distanceKm(kLondon, kNewYork) / hops, 0.5);
        groundTotal += distanceKm(p0, p1);
    }
    CHECK_NEAR(groundTotal, distanceKm(kLondon, kNewYork), 1.0);
    CHECK(skywaveArc(kLondon, kNewYork, 0, 300.0, 1.0, per).size() == static_cast<std::size_t>(per));        // hops clamp to 1
    CHECK(skywaveArc(kLondon, kNewYork, 99, 300.0, 1.0, per).size() == static_cast<std::size_t>(8 * (per - 1) + 1));   // and to 8
}

// ------------------------------------------------------------------------------------------------------------------------
void sectionSatellites() {
    // a geostationary satellite (35 786 km) sees down to 81.3 deg of latitude from its sub-point: the published coverage limit
    CHECK_NEAR(footprintAngularRadiusDeg(35786.0, 0.0), 81.3, 0.1);
    // a NOAA at 870 km: acos(R / (R + h)) = 28.4 deg of arc, 3 160 km on the ground
    CHECK_NEAR(footprintAngularRadiusDeg(870.0, 0.0), std::acos(kEarthRadiusKm / (kEarthRadiusKm + 870.0)) * kRadToDeg, 1e-9);
    CHECK_NEAR(footprintAngularRadiusDeg(870.0, 0.0), 28.4, 0.1);
    // a mask of 10 degrees shrinks it, and an altitude of nothing sees nothing
    CHECK(footprintAngularRadiusDeg(870.0, 10.0) < footprintAngularRadiusDeg(870.0, 0.0) - 5.0);
    CHECK_NEAR(footprintAngularRadiusDeg(0.0, 0.0), 0.0, 1e-9);
    CHECK_NEAR(footprintAngularRadiusDeg(-50.0, 0.0), 0.0, 1e-9);
    // the circle is every point at that radius, and the satellite sees the ground at its edge on the horizon
    const LatLon sub{58.2, -18.4};
    const Polyline fp = footprintCircle(sub, 870.0, 0.0, 72, 1.0);
    CHECK(fp.size() == 73u);
    for (const Vec3& p : fp) {
        CHECK_NEAR(angleBetweenDeg(p, toUnit(sub)), footprintAngularRadiusDeg(870.0, 0.0), 1e-9);
        const LookAngle la = lookAngle(fromVec(p), sub, 870.0);
        CHECK_NEAR(la.elevationDeg, 0.0, 1e-6);
    }
    // 5 degrees of mask: the edge sits at 5 degrees elevation
    for (const Vec3& p : footprintCircle(sub, 870.0, 5.0, 36, 1.0)) { CHECK_NEAR(lookAngle(fromVec(p), sub, 870.0).elevationDeg, 5.0, 1e-6); }

    // the sub-satellite point from an inertial position: build the position from a chosen Earth-fixed one and the date's sidereal angle, then read it back
    const double jd = julianDay(2025, 3, 20, 9, 1, 0.0);
    const double th = gmstDegrees(jd) * kDegToRad;
    const LatLon want{35.0, 100.0};
    const double rkm = kEarthRadiusKm + 870.0;
    const Vec3 ecef = toUnit(want) * rkm;
    const Vec3 eci{ecef.x * std::cos(th) - ecef.y * std::sin(th), ecef.x * std::sin(th) + ecef.y * std::cos(th), ecef.z};
    LatLon got;
    double alt = 0.0;
    CHECK(subSatellitePoint(eci, jd, got, alt));
    CHECK_NEAR(got.lat, want.lat, 1e-9);
    CHECK_NEAR(normalizeLon(got.lon - want.lon), 0.0, 1e-9);
    CHECK_NEAR(alt, 870.0, 1e-6);
    CHECK(!subSatellitePoint(Vec3{0, 0, 0}, jd, got, alt));
    CHECK(!subSatellitePoint(Vec3{std::nan(""), 1, 1}, jd, got, alt));
    // a point on the Earth's axis stays on the axis whatever the date
    CHECK(subSatellitePoint(Vec3{0, 0, 7000.0}, jd, got, alt));
    CHECK_NEAR(got.lat, 90.0, 1e-9);

    // the look angle: overhead is 90 deg at the satellite's own altitude; a GEO over 0,0 seen from the equator's edge of the disc is on the horizon; from London
    // (51.5074 N) to a GEO due south the textbook elevation atan((cos g - R/(R+h)) / sin g) with cos g = cos(lat) is 31.1 deg
    const LookAngle over = lookAngle(LatLon{10, 20}, LatLon{10, 20}, 870.0);
    CHECK_NEAR(over.elevationDeg, 90.0, 1e-6);
    CHECK_NEAR(over.slantRangeKm, 870.0, 1e-6);
    CHECK(over.visible);
    const LookAngle edge = lookAngle(LatLon{0, footprintAngularRadiusDeg(35786.0, 0.0)}, LatLon{0, 0}, 35786.0);
    CHECK_NEAR(edge.elevationDeg, 0.0, 1e-6);
    CHECK_NEAR(edge.azimuthDeg, 270.0, 1e-6);   // looking back west along the equator
    const LookAngle lon = lookAngle(LatLon{kLondonLat, 0.0}, LatLon{0.0, 0.0}, 35786.0);
    const double g = kLondonLat * kDegToRad;
    const double textbook = std::atan((std::cos(g) - kEarthRadiusKm / (kEarthRadiusKm + 35786.0)) / std::sin(g)) * kRadToDeg;
    CHECK_NEAR(lon.elevationDeg, textbook, 1e-6);
    CHECK_NEAR(lon.elevationDeg, 31.1, 0.2);
    CHECK_NEAR(lon.azimuthDeg, 180.0, 1e-6);
    CHECK_NEAR(lon.slantRangeKm, 38500.0, 100.0);   // London to a GEO due south: about 38 500 km (a ground-station figure for 51.5 N)
    // the far side of the Earth cannot see it
    const LookAngle back = lookAngle(LatLon{0, 180}, LatLon{0, 0}, 35786.0);
    CHECK(back.elevationDeg < -50.0);
    CHECK(!back.visible);
}

// ------------------------------------------------------------------------------------------------------------------------
void sectionLocator() {
    double la = 0, lo = 0;
    // IO91wm is London (51.5074 N, 0.1278 W): the centre of the cell, 5' by 2.5'
    CHECK(locatorToLatLon("IO91wm", la, lo));
    CHECK_NEAR(la, 51.5 + 1.25 / 60.0, 1e-9);
    CHECK_NEAR(lo, -0.125, 1e-9);
    CHECK(locatorToLatLon("io91wm", la, lo));          // either case
    CHECK_NEAR(lo, -0.125, 1e-9);
    // the four-character square FN42 (New England) is 2 by 1 degrees: centre 42.5 N 71 W
    CHECK(locatorToLatLon("FN42", la, lo));
    CHECK_NEAR(la, 42.5, 1e-9);
    CHECK_NEAR(lo, -71.0, 1e-9);
    // JN58td is Munich's: 48.1458 N 11.625 E
    CHECK(locatorToLatLon("JN58td", la, lo));
    CHECK_NEAR(la, 48.1458333, 1e-6);
    CHECK_NEAR(lo, 11.625, 1e-9);
    // a field alone is 20 by 10 degrees: JO is 0-20 E, 50-60 N, centre 10 E 55 N
    CHECK(locatorToLatLon("JO", la, lo));
    CHECK_NEAR(la, 55.0, 1e-9);
    CHECK_NEAR(lo, 10.0, 1e-9);
    // eight characters: the extended square (1/10 of the subsquare; the first digit is longitude, the second latitude), centre of IO91wm45
    CHECK(locatorToLatLon("IO91wm45", la, lo));
    CHECK_NEAR(la, 51.0 + 12.0 * (1.0 / 24.0) + 5.0 * (1.0 / 240.0) + 0.5 * (1.0 / 240.0), 1e-9);
    CHECK_NEAR(lo, -2.0 + 22.0 * (2.0 / 24.0) + 4.0 * (2.0 / 240.0) + 0.5 * (2.0 / 240.0), 1e-9);
    // the ends of the grid: AA00aa is the south-west corner cell, RR99xx the north-east
    CHECK(locatorToLatLon("AA00aa", la, lo));
    CHECK(la < -89.9 && lo < -179.9);
    CHECK(locatorToLatLon("RR99xx", la, lo));
    CHECK(la > 89.9 && lo > 179.9);
    // refused, outputs untouched
    la = 7; lo = 8;
    for (const char* bad : {"", "I", "IO9", "IO91w", "IO91wmx", "SS11", "IO9l", "IO91yy", "IO91wm4", "IO91wm4x", "I0", "AA00ay", "11"}) {
        CHECK(!locatorToLatLon(bad, la, lo));
    }
    CHECK(la == 7.0 && lo == 8.0);
    LatLon p{1, 2};
    CHECK(!locatorToLatLon("nonsense", p));
    CHECK(p.lat == 1.0 && p.lon == 2.0);
    CHECK(locatorToLatLon("IO91wm", p));
    CHECK_NEAR(p.lat, 51.5208333, 1e-6);
    // the round trip with the host's own encoder: grid6 of any point decodes to within half a subsquare of it, and decoding then encoding gives the same grid back
    int far = 0, again = 0, n = 0;
    for (int i = 0; i < 4000; ++i) {
        const double lat = -89.9 + 179.8 * ((i * 7919) % 4000) / 4000.0;
        const double lon = -179.9 + 359.8 * ((i * 104729) % 4000) / 4000.0;
        char grid[7];
        CHECK(cascade::core::maidenheadGrid6(lat, lon, grid));
        LatLon c;
        CHECK(locatorToLatLon(grid, c));
        ++n;
        if (std::fabs(c.lat - lat) > 1.25 / 60.0 + 1e-9 || std::fabs(c.lon - lon) > 2.5 / 60.0 + 1e-9) { ++far; }
        char grid2[7];
        CHECK(cascade::core::maidenheadGrid6(c.lat, c.lon, grid2));
        if (std::string(grid) != grid2) { ++again; }
    }
    CHECK(n == 4000);
    CHECK(far == 0);
    CHECK(again == 0);
    // and the app's own sample: encoding London gives the locator the decoder maps back near London
    char london[7];
    CHECK(cascade::core::maidenheadGrid6(kLondonLat, kLondonLon, london));
    CHECK(std::string(london) == "IO91wm");
}

}  // namespace

int main() {
    sectionSphere();
    sectionPolylines();
    sectionLand();
    sectionSun();
    sectionTerminator();
    sectionGreatCircle();
    sectionSatellites();
    sectionLocator();
    return testSummary("test_slint_globe_scene");
}
