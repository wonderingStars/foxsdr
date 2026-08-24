// Tests for gui/track_metrics.hpp - the pure half of the map's three
// receiver-relative features: the great-circle range and bearing behind the
// track table's DISTANCE and BEARING columns, the altitude band that decides a
// marker's colour, and the per-bearing coverage accumulator.
//
// WHY THE GEODESY IS CHECKED TWO WAYS. The axis-aligned pairs (due north, due
// east on the equator, across the antimeridian) have analytic answers - the
// distance is the radius times the angle in radians and the bearing is exactly
// 0 or 90 - so those are asserted against the arithmetic rather than against a
// number somebody remembered. The oblique southern-hemisphere pair has no such
// closed form, so it is cross-checked against the SPHERICAL LAW OF COSINES
// computed here in the test: a different derivation of the same quantity, which
// is the only cross-check that can catch a haversine with a sign or a factor
// wrong, since a remembered constant would only prove the memory.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "gui/track_metrics.hpp"
#include "test_check.hpp"

using cascade::gui::altBandStyle;
using cascade::gui::altitudeBandIndex;
using cascade::gui::CoverageMap;
using cascade::gui::coverageVertex;
using cascade::gui::CoverageVertex;
using cascade::gui::destinationPoint;
using cascade::gui::greatCircleKm;
using cascade::gui::initialBearingDeg;
using cascade::gui::LatLon;
using cascade::gui::lonDeltaDeg;
using cascade::gui::kAltBandCount;
using cascade::gui::sortTrackRows;
using cascade::gui::TrackRow;
using cascade::gui::TrackSortKey;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kR = 6371.0;

double deg2rad(double d) { return d * kPi / 180.0; }

// An INDEPENDENT distance: the spherical law of cosines, which shares no term
// with the haversine under test. It is the formula haversine replaced (it loses
// precision at small separations), so it is only used here on a pair hundreds
// of kilometres apart, where it is accurate to well under a metre.
double lawOfCosinesKm(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = deg2rad(lat1);
    const double p2 = deg2rad(lat2);
    const double dl = deg2rad(lon2 - lon1);
    double c = std::sin(p1) * std::sin(p2) + std::cos(p1) * std::cos(p2) * std::cos(dl);
    if (c > 1.0) { c = 1.0; }
    if (c < -1.0) { c = -1.0; }
    return kR * std::acos(c);
}

// One degree of arc on this sphere, which is what the axis-aligned expectations
// are built from rather than from a recalled "111 km".
const double kKmPerDegree = kR * kPi / 180.0;

// Bounds-safe row access, so an assertion about row i still RUNS - and fails -
// when the sort produced fewer rows than it should have, instead of vanishing
// inside an `if (i < size)` and quietly shrinking the check tally.
TrackRow rowAt(const std::vector<TrackRow>& rows, std::size_t i) {
    if (i < rows.size()) { return rows[i]; }
    return TrackRow{};
}

// The ids of a row vector in order, compared as a WHOLE value: an order
// assertion that compares element by element under a size guard is an
// assertion that disappears when the size is wrong.
std::vector<std::string> idsOf(const std::vector<TrackRow>& rows) {
    std::vector<std::string> out;
    out.reserve(rows.size());
    for (const TrackRow& r : rows) { out.push_back(r.id); }
    return out;
}

TrackRow makeRow(const char* id, const char* label, double altM, double speedMps,
                 double courseDeg, double distanceKm, double bearingDeg,
                 std::uint64_t ageMs, std::size_t source) {
    TrackRow r;
    r.id = id;
    r.label = label;
    r.altM = altM;
    r.speedMps = speedMps;
    r.courseDeg = courseDeg;
    r.distanceKm = distanceKm;
    r.bearingDeg = bearingDeg;
    r.ageMs = ageMs;
    r.source = source;
    return r;
}

const double kNaN = std::numeric_limits<double>::quiet_NaN();

// An INDEPENDENT destination point, by 3-D vector arithmetic rather than by the
// spherical-trig direct formula under test. The start point, the local east and
// north unit vectors and the initial direction of travel are built as Cartesian
// vectors and the destination is P*cos(delta) + D*sin(delta) - a derivation that
// shares no trigonometric identity with the asin/atan2 pair in
// destinationPoint, so a sign or a swapped term in one cannot hide in the
// other. This is the same trick lawOfCosinesKm plays on the haversine.
std::pair<double, double> destVectorForm(double latDeg, double lonDeg, double bearingDeg,
                                         double km) {
    const double p = deg2rad(latDeg);
    const double l = deg2rad(lonDeg);
    const double b = deg2rad(bearingDeg);
    const double d = km / kR;
    const double P[3] = {std::cos(p) * std::cos(l), std::cos(p) * std::sin(l), std::sin(p)};
    const double E[3] = {-std::sin(l), std::cos(l), 0.0};
    const double N[3] = {-std::sin(p) * std::cos(l), -std::sin(p) * std::sin(l), std::cos(p)};
    double Q[3];
    for (int i = 0; i < 3; ++i) {
        const double dir = N[i] * std::cos(b) + E[i] * std::sin(b);
        Q[i] = P[i] * std::cos(d) + dir * std::sin(d);
    }
    return {std::asin(Q[2]) * 180.0 / kPi, std::atan2(Q[1], Q[0]) * 180.0 / kPi};
}

}  // namespace

int main() {
    // --- great-circle distance: analytic pairs -------------------------------
    {
        // NORTH-SOUTH, ten degrees up the prime meridian. On a sphere this is
        // exactly ten degrees of arc whatever the longitude.
        CHECK_NEAR(greatCircleKm(0.0, 0.0, 10.0, 0.0), 10.0 * kKmPerDegree, 1e-6);
        // ...and southwards from the equator, the same arc.
        CHECK_NEAR(greatCircleKm(0.0, 0.0, -10.0, 0.0), 10.0 * kKmPerDegree, 1e-6);

        // EAST-WEST, ten degrees along the equator, where a degree of longitude
        // is a degree of arc. (It is not, away from the equator - see below.)
        CHECK_NEAR(greatCircleKm(0.0, 0.0, 0.0, 10.0), 10.0 * kKmPerDegree, 1e-6);

        // ...AND THE SAME TEN DEGREES AT 60 DEGREES NORTH IS HALF THE DISTANCE,
        // because cos(60) is exactly 0.5. This is the check that a "degrees
        // times 111" range column could never pass, and the reason the column
        // needs real geodesy at all. (Along a parallel the great circle is
        // slightly SHORTER than the parallel itself, so the comparison is an
        // upper bound, not an equality.)
        const double at60 = greatCircleKm(60.0, 0.0, 60.0, 10.0);
        CHECK(at60 < 0.5 * 10.0 * kKmPerDegree);
        CHECK(at60 > 0.49 * 10.0 * kKmPerDegree);

        // THE ANTIMERIDIAN. +179.5 to -179.5 is one degree apart, not 359.
        CHECK_NEAR(greatCircleKm(0.0, 179.5, 0.0, -179.5), kKmPerDegree, 1e-6);
        // And the long way round is not taken from either side.
        CHECK_NEAR(greatCircleKm(0.0, -179.5, 0.0, 179.5), kKmPerDegree, 1e-6);
        // Off the equator too, where the naive "difference of longitudes"
        // version would also be wrong about the scale.
        CHECK(greatCircleKm(51.0, 179.9, 51.0, -179.9) < 20.0);

        // IDENTICAL POINTS: distance exactly zero, no negative-square-root or
        // NaN escaping the formula.
        CHECK(greatCircleKm(51.5, -0.12, 51.5, -0.12) == 0.0);
        CHECK(greatCircleKm(-33.87, 151.21, -33.87, 151.21) == 0.0);

        // ANTIPODES: half the circumference, and the formula must not overshoot
        // into a domain error at the far limit.
        CHECK_NEAR(greatCircleKm(0.0, 0.0, 0.0, 180.0), kR * kPi, 1e-6);
        CHECK_NEAR(greatCircleKm(45.0, 10.0, -45.0, -170.0), kR * kPi, 1e-6);
    }

    // --- great-circle distance: southern hemisphere, cross-checked -----------
    {
        // Sydney to Melbourne: an oblique pair, both coordinates negative in
        // latitude, checked against the law of cosines rather than a recalled
        // figure. A degenerate haversine (dropping the cos(lat) product, say)
        // is off by hundreds of kilometres here and the cross-check catches it.
        const double sydLat = -33.8688, sydLon = 151.2093;
        const double melLat = -37.8136, melLon = 144.9631;
        const double hav = greatCircleKm(sydLat, sydLon, melLat, melLon);
        const double loc = lawOfCosinesKm(sydLat, sydLon, melLat, melLon);
        CHECK_NEAR(hav, loc, 0.001);
        // Sanity on the magnitude, so a formula that agreed with a broken
        // cross-check would still be caught: this pair is several hundred km
        // apart, not tens and not thousands.
        CHECK(hav > 600.0);
        CHECK(hav < 800.0);
        // SYMMETRIC: distance has no direction.
        CHECK_NEAR(greatCircleKm(melLat, melLon, sydLat, sydLon), hav, 1e-9);
    }

    // --- initial bearing -----------------------------------------------------
    {
        // Due north, due south, due east, due west - the four analytic cases.
        CHECK_NEAR(initialBearingDeg(0.0, 0.0, 10.0, 0.0), 0.0, 1e-9);
        CHECK_NEAR(initialBearingDeg(0.0, 0.0, -10.0, 0.0), 180.0, 1e-9);
        CHECK_NEAR(initialBearingDeg(0.0, 0.0, 0.0, 10.0), 90.0, 1e-9);
        CHECK_NEAR(initialBearingDeg(0.0, 0.0, 0.0, -10.0), 270.0, 1e-9);

        // Due north and due south IN THE SOUTHERN HEMISPHERE, where a sign
        // error in the latitude terms would flip the answer.
        CHECK_NEAR(initialBearingDeg(-20.0, 30.0, -10.0, 30.0), 0.0, 1e-9);
        CHECK_NEAR(initialBearingDeg(-10.0, 30.0, -20.0, 30.0), 180.0, 1e-9);

        // ACROSS THE ANTIMERIDIAN: due east from +179.5 to -179.5, which the
        // unwrapped difference of longitudes would call due WEST.
        CHECK_NEAR(initialBearingDeg(0.0, 179.5, 0.0, -179.5), 90.0, 1e-9);
        CHECK_NEAR(initialBearingDeg(0.0, -179.5, 0.0, 179.5), 270.0, 1e-9);

        // ALWAYS IN 0..360, never negative: a bearing of -90 is a bearing no
        // rotator and no compass rose accepts.
        for (double lon = -180.0; lon <= 180.0; lon += 7.5) {
            const double b = initialBearingDeg(51.5, -0.12, 40.0, lon);
            CHECK(b >= 0.0);
            CHECK(b < 360.0);
        }

        // SOUTHERN-HEMISPHERE OBLIQUE: Sydney to Melbourne runs south-west, so
        // the bearing sits in the third quadrant. Checked as a quadrant rather
        // than a constant for the same reason as above - and the reciprocal
        // must come back within a degree of 180 away, which no single wrong
        // formula satisfies by accident.
        const double out = initialBearingDeg(-33.8688, 151.2093, -37.8136, 144.9631);
        CHECK(out > 180.0);
        CHECK(out < 270.0);
        const double back = initialBearingDeg(-37.8136, 144.9631, -33.8688, 151.2093);
        CHECK(back > 0.0);
        CHECK(back < 90.0);
        // On a sphere the two differ by 180 plus the convergence of the
        // meridians, which over this pair is a few degrees, not none and not
        // tens.
        const double delta = std::fabs(out - back - 180.0);
        CHECK(delta > 0.5);
        CHECK(delta < 10.0);

        // IDENTICAL POINTS: explicitly undefined. atan2(0,0) is 0.0 in IEEE
        // arithmetic, so an implementation that just lets the formula run
        // reports due north for a target directly overhead.
        CHECK(std::isnan(initialBearingDeg(51.5, -0.12, 51.5, -0.12)));
        CHECK(std::isnan(initialBearingDeg(-33.87, 151.21, -33.87, 151.21)));
        CHECK(std::isnan(initialBearingDeg(0.0, 0.0, 0.0, 0.0)));
    }

    // --- the direct problem: destinationPoint --------------------------------
    //
    // WHY THIS IS NOT TESTED AGAINST ITSELF. A destination formula that is
    // wrong is still self-consistent: it will happily satisfy any property
    // stated in terms of its own output. So the block below anchors on three
    // kinds of evidence that do not come from the function - an EXTERNAL
    // published worked example, ANALYTIC answers on the axes, and an
    // INDEPENDENT vector-form derivation computed here in the test - and only
    // then adds the round-trip properties against greatCircleKm and
    // initialBearingDeg.
    {
        // THE EXTERNAL REFERENCE, and the only number here that comes from
        // outside this repository. It is the worked example published with
        // Chris Veness's Movable Type "destination point given distance and
        // bearing" reference implementation, which uses the same R = 6371 km
        // sphere: start 53 deg 19' 14" N, 001 deg 43' 47" W, travel 124.8 km on
        // a bearing of 096 deg 01' 18", and the published destination is
        // 53 deg 11' 18" N, 000 deg 08' 00" E - that is 53.188333 N,
        // 0.133333 E, quoted to the nearest arcsecond.
        //
        // The tolerance is 0.0005 deg, about 1.8 arcseconds: loose enough for
        // the published value's own rounding (one arcsecond is ~31 m) and
        // still thousands of times tighter than the error any wrong formula
        // produces here. The plate-carree shortcut this replaced answers
        // 53.20295 N, 0.13674 E - 0.0146 deg of latitude out, twenty-nine
        // times the tolerance, so this assertion genuinely discriminates.
        {
            const LatLon d = destinationPoint(53.320556, -1.729722, 96.021667, 124.8);
            CHECK_NEAR(d.latDeg, 53.188333, 0.0005);
            CHECK_NEAR(d.lonDeg, 0.133333, 0.0005);
        }

        // DUE NORTH and DUE SOUTH along a meridian: analytic. Ten degrees of
        // arc is ten degrees of latitude and the longitude does not move.
        {
            const LatLon n = destinationPoint(0.0, 0.0, 0.0, 10.0 * kKmPerDegree);
            CHECK_NEAR(n.latDeg, 10.0, 1e-9);
            CHECK_NEAR(n.lonDeg, 0.0, 1e-9);
            const LatLon s = destinationPoint(-20.0, 30.0, 180.0, 10.0 * kKmPerDegree);
            CHECK_NEAR(s.latDeg, -30.0, 1e-9);
            CHECK_NEAR(s.lonDeg, 30.0, 1e-9);
        }

        // DUE EAST ALONG THE EQUATOR: analytic, and the one case where east is
        // also a great circle, so the latitude stays at zero exactly.
        {
            const LatLon e = destinationPoint(0.0, 0.0, 90.0, 10.0 * kKmPerDegree);
            CHECK_NEAR(e.latDeg, 0.0, 1e-9);
            CHECK_NEAR(e.lonDeg, 10.0, 1e-9);
            const LatLon w = destinationPoint(0.0, 0.0, 270.0, 10.0 * kKmPerDegree);
            CHECK_NEAR(w.latDeg, 0.0, 1e-9);
            CHECK_NEAR(w.lonDeg, -10.0, 1e-9);
        }

        // DUE EAST OFF THE EQUATOR IS NOT A PARALLEL, and this is the single
        // clearest statement that the answer is a great circle rather than a
        // flat-earth offset. By Clairaut's relation a great circle leaving a
        // northern latitude on a bearing of exactly 090 has its NORTHERNMOST
        // point at the departure, so it must run south of the parallel from
        // there on. The plate-carree version holds the latitude fixed - it
        // answers 51.5 for this - so a failure of the first line below is
        // precisely the flat-earth bug.
        {
            const LatLon e = destinationPoint(51.5, -0.12, 90.0, 500.0);
            CHECK(e.latDeg < 51.5 - 0.2);
            CHECK_NEAR(e.latDeg, 51.278824, 1e-5);
            CHECK_NEAR(e.lonDeg, 7.080023, 1e-5);
        }

        // ACROSS THE ANTIMERIDIAN. This is the receiver position the coverage
        // outline streaked across the whole map from, and the reason
        // destinationPoint normalises: 111 km due east of 0.0 / 179.5 is at
        // 180.498 unwrapped, which is not a longitude. The place is
        // -179.50175 E, and the check that |lon| <= 180 is what pins the
        // normalisation rather than merely the arithmetic.
        {
            const LatLon d = destinationPoint(0.0, 179.5, 90.0, 111.0);
            CHECK_NEAR(d.latDeg, 0.0, 1e-9);
            CHECK_NEAR(d.lonDeg, -179.501753, 1e-6);
            CHECK(d.lonDeg >= -180.0);
            CHECK(d.lonDeg <= 180.0);
            // And westwards back over it, from the other side.
            const LatLon w = destinationPoint(0.0, -179.5, 270.0, 111.0);
            CHECK_NEAR(w.lonDeg, 179.501753, 1e-6);
        }

        // SOUTHERN HEMISPHERE, OBLIQUE - no analytic answer, so it is checked
        // against the independent vector form. Sydney, 800 km on 240 true.
        {
            const LatLon d = destinationPoint(-33.87, 151.21, 240.0, 800.0);
            const std::pair<double, double> v = destVectorForm(-33.87, 151.21, 240.0, 800.0);
            CHECK_NEAR(d.latDeg, v.first, 1e-9);
            CHECK_NEAR(d.lonDeg, v.second, 1e-9);
            // It really did go south-west, which no amount of agreement
            // between two derivations of a mirrored formula would show.
            CHECK(d.latDeg < -33.87);
            CHECK(d.lonDeg < 151.21);
        }

        // POLE-ADJACENT, and specifically OVER the pole: from 89.9 N due north
        // for 100 km. 100 km is 0.89927 deg of arc, so the path passes the pole
        // with 0.79927 deg to run and comes down the far side to 89.20068 N -
        // and the far side is the ANTIMERIDIAN of the meridian it went up, so
        // the longitude flips by 180. A formula that just adds the latitude
        // offset answers 90.79927 N, which is not a latitude at all.
        {
            const LatLon d = destinationPoint(89.9, 0.0, 0.0, 100.0);
            CHECK_NEAR(d.latDeg, 89.200678, 1e-5);
            CHECK(d.latDeg <= 90.0);
            CHECK_NEAR(std::fabs(d.lonDeg), 180.0, 1e-9);
            // A bearing that is not exactly polar, so the meridian arithmetic
            // is exercised away from the degenerate case too.
            const LatLon o = destinationPoint(89.9, 0.0, 10.0, 100.0);
            const std::pair<double, double> ov = destVectorForm(89.9, 0.0, 10.0, 100.0);
            CHECK_NEAR(o.latDeg, ov.first, 1e-9);
            CHECK_NEAR(o.lonDeg, ov.second, 1e-9);
            CHECK_NEAR(o.latDeg, 89.198971, 1e-5);
            CHECK_NEAR(o.lonDeg, 168.757933, 1e-5);
        }

        // THE ROUND TRIP, over a spread of bearings and distances: the whole
        // point of putting this beside greatCircleKm is that the two are
        // inverses, so the distance back out has to be the distance that went
        // in, and the initial bearing has to be the bearing that went in.
        //
        // THE SPREAD STOPS AT 20000 km, just short of pi*R = 20015 km. Past the
        // ANTIPODE the round trip stops being an identity for a reason that is
        // a property of the sphere and not of this code: a point 20100 km out -
        // which CoverageMap::record does accept - has come back round and is
        // only 19930 km away by the short path, so greatCircleKm correctly
        // reports 19930. Asserting the identity there would be asserting that
        // the sphere is wrong.
        {
            int checked = 0;
            for (int b = 0; b < 360; b += 7) {
                for (double d : {1.0, 50.0, 300.0, 1000.0, 5000.0, 15000.0, 20000.0}) {
                    const LatLon p =
                        destinationPoint(51.5, -0.12, static_cast<double>(b), d);
                    CHECK_NEAR(greatCircleKm(51.5, -0.12, p.latDeg, p.lonDeg), d, 1e-6);
                    // The bearing comes back too, which the distance alone
                    // cannot show: a formula that went the right distance in
                    // the wrong direction passes a distance-only round trip.
                    const double back =
                        initialBearingDeg(51.5, -0.12, p.latDeg, p.lonDeg);
                    double diff = std::fabs(back - static_cast<double>(b));
                    if (diff > 180.0) { diff = 360.0 - diff; }
                    CHECK_NEAR(diff, 0.0, 1e-6);
                    ++checked;
                }
            }
            // The loop ran the number of times it looks like it ran: 52
            // bearings by 7 distances. Without this a broken range expression
            // would silently reduce the block above to nothing.
            CHECK(checked == 52 * 7);
        }

        // Past the antipode, stated as its own expectation rather than left
        // as a gap: 20100 km out from London is 19930 km back.
        {
            const LatLon p = destinationPoint(51.5, -0.12, 42.5, 20100.0);
            CHECK_NEAR(greatCircleKm(51.5, -0.12, p.latDeg, p.lonDeg),
                       2.0 * kR * kPi - 20100.0, 1e-6);
        }

        // AND THE ERROR THE FIX EXISTS TO REMOVE, measured rather than
        // asserted from memory. The plate-carree offset the outline shipped
        // with is recomputed here and compared against the great-circle
        // destination from the same start and bearing: the header claims 1.1,
        // 9.9, 27.4 and 110 km at 100, 300, 500 and 1000 km from 51.5 N on
        // 042.5, and if a later "tidy-up" reintroduces the shortcut these
        // bounds are what fails.
        {
            const double a = deg2rad(42.5);
            const double cosLat = std::cos(deg2rad(51.5));
            struct Case { double km; double lo; double hi; };
            const Case cases[] = {
                {100.0, 1.0, 1.3}, {300.0, 9.5, 10.3}, {500.0, 27.0, 28.0}, {1000.0, 108.0, 112.0}};
            for (const Case& c : cases) {
                const double flatLat = 51.5 + c.km / 111.32 * std::cos(a);
                const double flatLon = -0.12 + c.km / (111.32 * cosLat) * std::sin(a);
                const LatLon good = destinationPoint(51.5, -0.12, 42.5, c.km);
                const double err =
                    greatCircleKm(flatLat, flatLon, good.latDeg, good.lonDeg);
                CHECK(err > c.lo);
                CHECK(err < c.hi);
            }
        }
    }

    // --- continuous longitude for a connected shape --------------------------
    {
        // THE SHORT WAY ROUND, which is the entire content of the function.
        CHECK_NEAR(lonDeltaDeg(0.0, 10.0), 10.0, 1e-9);
        CHECK_NEAR(lonDeltaDeg(10.0, 0.0), -10.0, 1e-9);
        CHECK_NEAR(lonDeltaDeg(51.5, 51.5), 0.0, 1e-9);
        // ACROSS THE ANTIMERIDIAN, both ways. A raw subtraction answers -359
        // and +359 for these two, which is a line across the world.
        CHECK_NEAR(lonDeltaDeg(179.5, -179.5), 1.0, 1e-9);
        CHECK_NEAR(lonDeltaDeg(-179.5, 179.5), -1.0, 1e-9);
        CHECK_NEAR(lonDeltaDeg(179.9, -179.9), 0.2, 1e-9);
        // NOTHING EVER LEAVES -180..180, whatever it is handed - including a
        // pair that is a whole world apart and one already at the edge.
        for (double from : {-180.0, -179.5, -90.0, 0.0, 90.0, 179.5, 180.0}) {
            for (double to : {-180.0, -179.5, -90.0, 0.0, 90.0, 179.5, 180.0}) {
                const double d = lonDeltaDeg(from, to);
                CHECK(d >= -180.0);
                CHECK(d <= 180.0);
            }
        }
    }

    // --- the coverage outline's vertices -------------------------------------
    //
    // This is the pure half of the drawing fix: the outline is placed by a
    // latitude and a CONTINUOUS longitude offset from the receiver, so the
    // pixel step from the receiver is a plain scale of the offset and no vertex
    // is free to wrap independently of its neighbours.
    {
        // AN ORDINARY RECEIVER, nowhere near the antimeridian: the offset is
        // just the difference, and the vertex is the destination point.
        {
            const CoverageVertex v = coverageVertex(51.5, -0.12, 42.5, 500.0);
            const LatLon d = destinationPoint(51.5, -0.12, 42.5, 500.0);
            CHECK_NEAR(v.latDeg, d.latDeg, 1e-9);
            CHECK_NEAR(v.dLonDeg, d.lonDeg - (-0.12), 1e-9);
            // North-east of London, which is where 042.5 points.
            CHECK(v.latDeg > 51.5);
            CHECK(v.dLonDeg > 0.0);
        }

        // THE ANTIMERIDIAN RECEIVER, which is the reproduction. 0.0 / 179.5
        // with one sighting 111 km due east: the vertex longitude is 180.498,
        // the projection normalises it to -179.502, and the outline used to be
        // drawn from the right-hand edge of the map to the left-hand one. As
        // an OFFSET it is +0.998 degrees and there is nothing to wrap.
        {
            const CoverageVertex v = coverageVertex(0.0, 179.5, 90.0, 111.0);
            CHECK_NEAR(v.latDeg, 0.0, 1e-9);
            // 111 km divided by one degree of arc on this sphere
            // (6371*pi/180 = 111.19493 km) is 0.9982470 degrees.
            CHECK_NEAR(v.dLonDeg, 0.9982470, 1e-6);
            // The bug, stated as the thing that must not happen: a raw
            // subtraction of the normalised longitudes gives -359.0018.
            CHECK(std::fabs(v.dLonDeg) < 2.0);
        }

        // AND ROUND THE WHOLE COMPASS FROM THERE. Every one of the 72 buckets
        // a full CoverageMap would produce has to stay a small offset - one
        // vertex jumping most of a world is one streak, and the fill takes the
        // same vertices the polyline does.
        {
            double widest = 0.0;
            int n = 0;
            for (int i = 0; i < CoverageMap::kBuckets; ++i) {
                const double brg = (static_cast<double>(i) + 0.5) * CoverageMap::kBucketDeg;
                const CoverageVertex v = coverageVertex(0.0, 179.5, brg, 300.0);
                widest = std::max(widest, std::fabs(v.dLonDeg));
                ++n;
            }
            CHECK(n == CoverageMap::kBuckets);
            // 300 km is at most 2.7 degrees of longitude at the equator.
            CHECK(widest < 3.0);
            CHECK(widest > 2.6);
        }

        // NEIGHBOURING VERTICES STAY NEIGHBOURS. The streak is visible as a
        // discontinuity between consecutive buckets, so that is what is
        // measured: no step between adjacent vertices of a smooth outline may
        // be anywhere near a world wide.
        {
            double biggestStep = 0.0;
            for (double homeLon : {179.5, -179.5, 180.0, 0.0, -0.12}) {
                // Start from the LAST bucket, so the wrap from 357.5 back to
                // 2.5 - the closing edge of the polygon - is measured too.
                CoverageVertex prev = coverageVertex(
                    0.0, homeLon,
                    (static_cast<double>(CoverageMap::kBuckets) - 0.5) * CoverageMap::kBucketDeg,
                    400.0);
                for (int i = 0; i < CoverageMap::kBuckets; ++i) {
                    const double brg =
                        (static_cast<double>(i) + 0.5) * CoverageMap::kBucketDeg;
                    const CoverageVertex v = coverageVertex(0.0, homeLon, brg, 400.0);
                    biggestStep = std::max(biggestStep, std::fabs(v.dLonDeg - prev.dLonDeg));
                    prev = v;
                }
            }
            CHECK(biggestStep < 2.0);
        }
    }

    // --- altitude bands ------------------------------------------------------
    {
        // UNKNOWN IS ITS OWN ANSWER, not a band. NaN is the ABI's "the source
        // does not know", and an infinity is what a broken plugin sends.
        CHECK(altitudeBandIndex(kNaN) == -1);
        CHECK(altitudeBandIndex(std::numeric_limits<double>::infinity()) == -1);
        CHECK(altitudeBandIndex(-std::numeric_limits<double>::infinity()) == -1);

        // SEA LEVEL IS A BAND, and specifically not the same answer as unknown:
        // this is the whole distinction the ABI comment on altM exists to make.
        CHECK(altitudeBandIndex(0.0) == 0);
        CHECK(altitudeBandIndex(0.0) != altitudeBandIndex(kNaN));
        // Below sea level is a real place, not a decode error.
        CHECK(altitudeBandIndex(-400.0) == 0);

        // The boundaries, in feet, converted to the metres the ABI uses. Just
        // under each boundary is the lower band, just over it the higher one.
        const double ftToM = 1.0 / 3.28084;
        const double bounds[] = {1000.0, 5000.0, 10000.0, 20000.0, 30000.0};
        for (int i = 0; i < 5; ++i) {
            CHECK(altitudeBandIndex((bounds[i] - 1.0) * ftToM) == i);
            CHECK(altitudeBandIndex((bounds[i] + 1.0) * ftToM) == i + 1);
        }
        // Airliner cruise is the top band; so is anything absurd above it.
        CHECK(altitudeBandIndex(11000.0) == kAltBandCount - 1);
        CHECK(altitudeBandIndex(1.0e9) == kAltBandCount - 1);

        // EVERY BAND HAS A DISTINCT COLOUR. Two bands that render the same
        // would make an approach and a cruise look alike, which is the one
        // thing the colouring exists to prevent.
        for (int i = 0; i < kAltBandCount; ++i) {
            for (int j = i + 1; j < kAltBandCount; ++j) {
                const cascade::gui::AltBandStyle& a = altBandStyle(i);
                const cascade::gui::AltBandStyle& b = altBandStyle(j);
                CHECK(a.r != b.r || a.g != b.g || a.b != b.b);
            }
        }
        // ...and each is bright enough to read over the dark empty map and dark
        // enough to read over a pale OSM raster. A colour whose channels are
        // all near an extreme fails one background or the other.
        for (int i = 0; i < kAltBandCount; ++i) {
            const cascade::gui::AltBandStyle& s = altBandStyle(i);
            const int lum = (299 * s.r + 587 * s.g + 114 * s.b) / 1000;
            CHECK(lum >= 90);
            CHECK(lum <= 210);
            CHECK(s.label != nullptr);
            CHECK(s.label[0] != '\0');
        }

        // BOUNDS-SAFE, including the -1 that altitudeBandIndex itself returns.
        CHECK(altBandStyle(-1).label != nullptr);
        CHECK(altBandStyle(-1000).label != nullptr);
        CHECK(altBandStyle(kAltBandCount).label != nullptr);
        CHECK(altBandStyle(1000000).label != nullptr);
        // Clamping, not wrapping: an out-of-range index lands on an END band.
        CHECK(altBandStyle(-5).r == altBandStyle(0).r);
        CHECK(altBandStyle(-5).g == altBandStyle(0).g);
        CHECK(altBandStyle(999).r == altBandStyle(kAltBandCount - 1).r);
        CHECK(altBandStyle(999).g == altBandStyle(kAltBandCount - 1).g);
    }

    // --- coverage: bucketing -------------------------------------------------
    {
        CoverageMap cov;
        CHECK(cov.empty());
        CHECK(cov.filledBuckets() == 0);
        CHECK(cov.peakKm() == 0.0);
        // The geometry every bucket index below is computed from. A compile-
        // time assertion rather than a CHECK: MSVC warns on a constant
        // conditional, and the fact is knowable without running anything.
        static_assert(CoverageMap::kBuckets == 72, "five-degree buckets");
        CHECK_NEAR(CoverageMap::kBucketDeg, 5.0, 1e-9);

        // Due north lands in bucket 0, and so does everything up to but not
        // including five degrees.
        cov.record(0.0, 10.0);
        CHECK(cov.maxKm(0) == 10.0);
        CHECK(cov.filledBuckets() == 1);
        cov.record(4.999, 12.0);
        CHECK(cov.maxKm(0) == 12.0);
        CHECK(cov.filledBuckets() == 1);
        // Five degrees exactly is the NEXT bucket.
        cov.record(5.0, 3.0);
        CHECK(cov.maxKm(1) == 3.0);
        CHECK(cov.filledBuckets() == 2);
        // Due east, south and west land where the compass says.
        cov.record(90.0, 7.0);
        CHECK(cov.maxKm(18) == 7.0);
        cov.record(180.0, 8.0);
        CHECK(cov.maxKm(36) == 8.0);
        cov.record(270.0, 9.0);
        CHECK(cov.maxKm(54) == 9.0);
        CHECK(cov.filledBuckets() == 5);

        // THE WRAP AT 360. 359.9 is the last bucket and 360 is the first, and
        // an unnormalised index would be one past the end of the array.
        cov.record(359.999, 21.0);
        CHECK(cov.maxKm(CoverageMap::kBuckets - 1) == 21.0);
        cov.record(360.0, 30.0);
        CHECK(cov.maxKm(0) == 30.0);
        // Bearings outside 0..360 are normalised, not dropped and not clamped:
        // -90 is due west and 450 is due east.
        cov.record(-90.0, 40.0);
        CHECK(cov.maxKm(54) == 40.0);
        cov.record(450.0, 50.0);
        CHECK(cov.maxKm(18) == 50.0);
        cov.record(-720.0 + 180.0, 60.0);
        CHECK(cov.maxKm(36) == 60.0);

        // BOUNDS-SAFE READS. Nothing here may index the array out of range, and
        // an out-of-range bucket reads as "nothing heard".
        CHECK(cov.maxKm(-1) == 0.0);
        CHECK(cov.maxKm(CoverageMap::kBuckets) == 0.0);
        CHECK(cov.maxKm(1000000) == 0.0);
    }

    // --- coverage: it takes the MAXIMUM ------------------------------------
    {
        CoverageMap cov;
        cov.record(33.0, 120.0);
        CHECK(cov.maxKm(6) == 120.0);
        // A nearer sighting in the same bucket does not lower the record: the
        // question is how far this antenna has EVER heard that way.
        cov.record(33.0, 5.0);
        CHECK(cov.maxKm(6) == 120.0);
        cov.record(31.0, 80.0);
        CHECK(cov.maxKm(6) == 120.0);
        // A further one does raise it.
        cov.record(34.9, 250.0);
        CHECK(cov.maxKm(6) == 250.0);
        CHECK(cov.peakKm() == 250.0);
        // ...and only its own bucket moves.
        CHECK(cov.maxKm(5) == 0.0);
        CHECK(cov.maxKm(7) == 0.0);
        CHECK(cov.filledBuckets() == 1);

        // JUNK IS NOT A MEASUREMENT. A NaN bearing is what initialBearingDeg
        // returns for a target on top of the receiver; a NaN or non-positive
        // range is a broken plugin or an unset receiver position. None of them
        // may create a bucket, and none may destroy one.
        const double before = cov.maxKm(6);
        cov.record(kNaN, 100.0);
        cov.record(33.0, kNaN);
        cov.record(33.0, 0.0);
        cov.record(33.0, -50.0);
        cov.record(33.0, std::numeric_limits<double>::infinity());
        cov.record(std::numeric_limits<double>::infinity(), 100.0);
        // Past half the earth's circumference is not a terrestrial sighting.
        cov.record(33.0, 25000.0);
        CHECK(cov.maxKm(6) == before);
        CHECK(cov.filledBuckets() == 1);
        CHECK(cov.peakKm() == 250.0);
    }

    // --- coverage: reset -----------------------------------------------------
    {
        CoverageMap cov;
        for (int i = 0; i < CoverageMap::kBuckets; ++i) {
            cov.record(static_cast<double>(i) * CoverageMap::kBucketDeg + 2.5,
                       10.0 + static_cast<double>(i));
        }
        CHECK(cov.filledBuckets() == CoverageMap::kBuckets);
        CHECK(!cov.empty());
        CHECK(cov.peakKm() == 10.0 + static_cast<double>(CoverageMap::kBuckets - 1));

        cov.reset();
        CHECK(cov.empty());
        CHECK(cov.filledBuckets() == 0);
        CHECK(cov.peakKm() == 0.0);
        // EVERY bucket, not just the counter: a reset that cleared the count
        // and left the distances would redraw the old lobe the moment one new
        // sighting arrived.
        int nonZero = 0;
        for (int i = 0; i < CoverageMap::kBuckets; ++i) {
            if (cov.maxKm(i) != 0.0) { ++nonZero; }
        }
        CHECK(nonZero == 0);

        // And it accumulates again from nothing afterwards.
        cov.record(45.0, 60.0);
        CHECK(cov.filledBuckets() == 1);
        CHECK(cov.maxKm(9) == 60.0);
        CHECK(cov.peakKm() == 60.0);
    }

    // --- the table sort ------------------------------------------------------
    {
        // Five rows whose every column orders them differently, so a sort that
        // ignored its key would fail on all but one column.
        const auto build = []() {
            std::vector<TrackRow> v;
            v.push_back(makeRow("aa1", "DELTA", 3000.0, 200.0, 10.0, 50.0, 350.0, 4000, 0));
            v.push_back(makeRow("bb2", "ALPHA", 1000.0, 100.0, 350.0, 90.0, 10.0, 1000, 1));
            v.push_back(makeRow("cc3", "CHARLIE", 9000.0, 50.0, 180.0, 10.0, 200.0, 3000, 2));
            v.push_back(makeRow("dd4", "BRAVO", 5000.0, 250.0, 90.0, 120.0, 90.0, 2000, 3));
            v.push_back(makeRow("ee5", "ECHO", 200.0, 150.0, 270.0, 70.0, 275.0, 5000, 4));
            return v;
        };

        std::vector<TrackRow> v = build();
        sortTrackRows(v, TrackSortKey::Label, true);
        CHECK(idsOf(v) == std::vector<std::string>({"bb2", "dd4", "cc3", "aa1", "ee5"}));
        sortTrackRows(v, TrackSortKey::Label, false);
        CHECK(idsOf(v) == std::vector<std::string>({"ee5", "aa1", "cc3", "dd4", "bb2"}));

        v = build();
        sortTrackRows(v, TrackSortKey::Id, true);
        CHECK(idsOf(v) == std::vector<std::string>({"aa1", "bb2", "cc3", "dd4", "ee5"}));
        sortTrackRows(v, TrackSortKey::Id, false);
        CHECK(idsOf(v) == std::vector<std::string>({"ee5", "dd4", "cc3", "bb2", "aa1"}));

        v = build();
        sortTrackRows(v, TrackSortKey::Altitude, true);
        CHECK(idsOf(v) == std::vector<std::string>({"ee5", "bb2", "aa1", "dd4", "cc3"}));
        sortTrackRows(v, TrackSortKey::Altitude, false);
        CHECK(idsOf(v) == std::vector<std::string>({"cc3", "dd4", "aa1", "bb2", "ee5"}));

        v = build();
        sortTrackRows(v, TrackSortKey::Speed, true);
        CHECK(idsOf(v) == std::vector<std::string>({"cc3", "bb2", "ee5", "aa1", "dd4"}));

        v = build();
        sortTrackRows(v, TrackSortKey::Course, true);
        CHECK(idsOf(v) == std::vector<std::string>({"aa1", "dd4", "cc3", "ee5", "bb2"}));

        v = build();
        sortTrackRows(v, TrackSortKey::Distance, true);
        CHECK(idsOf(v) == std::vector<std::string>({"cc3", "aa1", "ee5", "bb2", "dd4"}));
        sortTrackRows(v, TrackSortKey::Distance, false);
        CHECK(idsOf(v) == std::vector<std::string>({"dd4", "bb2", "ee5", "aa1", "cc3"}));

        v = build();
        sortTrackRows(v, TrackSortKey::Bearing, true);
        CHECK(idsOf(v) == std::vector<std::string>({"bb2", "dd4", "cc3", "ee5", "aa1"}));

        v = build();
        sortTrackRows(v, TrackSortKey::Age, true);
        CHECK(idsOf(v) == std::vector<std::string>({"bb2", "dd4", "cc3", "aa1", "ee5"}));
        sortTrackRows(v, TrackSortKey::Age, false);
        CHECK(idsOf(v) == std::vector<std::string>({"ee5", "aa1", "cc3", "dd4", "bb2"}));

        // THE SOURCE INDEX RIDES WITH THE ROW. Clicking a sorted row must
        // still select the target it names, which it can only do if the index
        // back into the host's vector moved with the row.
        v = build();
        sortTrackRows(v, TrackSortKey::Altitude, false);
        CHECK(rowAt(v, 0).id == "cc3");
        CHECK(rowAt(v, 0).source == 2u);
        CHECK(rowAt(v, 4).id == "ee5");
        CHECK(rowAt(v, 4).source == 4u);
    }

    // --- the table sort: unknowns go last, both ways -------------------------
    {
        std::vector<TrackRow> v;
        v.push_back(makeRow("known-hi", "H", 9000.0, 90.0, 90.0, 200.0, 90.0, 10, 0));
        v.push_back(makeRow("unknown", "U", kNaN, kNaN, kNaN, kNaN, kNaN, 20, 1));
        v.push_back(makeRow("known-lo", "L", 100.0, 10.0, 10.0, 20.0, 10.0, 30, 2));

        const TrackSortKey keys[] = {TrackSortKey::Altitude, TrackSortKey::Speed,
                                     TrackSortKey::Course, TrackSortKey::Distance,
                                     TrackSortKey::Bearing};
        for (TrackSortKey k : keys) {
            std::vector<TrackRow> asc = v;
            sortTrackRows(asc, k, true);
            CHECK(idsOf(asc) ==
                  std::vector<std::string>({"known-lo", "known-hi", "unknown"}));
            std::vector<TrackRow> desc = v;
            sortTrackRows(desc, k, false);
            CHECK(idsOf(desc) ==
                  std::vector<std::string>({"known-hi", "known-lo", "unknown"}));
        }

        // All-unknown does not crash and does not reorder.
        std::vector<TrackRow> allNaN;
        allNaN.push_back(makeRow("x", "X", kNaN, kNaN, kNaN, kNaN, kNaN, 1, 0));
        allNaN.push_back(makeRow("y", "Y", kNaN, kNaN, kNaN, kNaN, kNaN, 2, 1));
        sortTrackRows(allNaN, TrackSortKey::Distance, true);
        CHECK(idsOf(allNaN) == std::vector<std::string>({"x", "y"}));
        sortTrackRows(allNaN, TrackSortKey::Distance, false);
        CHECK(idsOf(allNaN) == std::vector<std::string>({"x", "y"}));

        // Empty and single-row vectors are not special cases anywhere.
        std::vector<TrackRow> none;
        sortTrackRows(none, TrackSortKey::Age, true);
        CHECK(none.empty());
        std::vector<TrackRow> one;
        one.push_back(makeRow("solo", "S", 1.0, 1.0, 1.0, 1.0, 1.0, 1, 0));
        sortTrackRows(one, TrackSortKey::Age, false);
        CHECK(idsOf(one) == std::vector<std::string>({"solo"}));
    }

    // --- the table sort is STABLE -------------------------------------------
    {
        // Four rows tied on age, in a known order. A stable sort leaves them
        // exactly as they were; an unstable one is free to shuffle rows the
        // user is reading, which is what makes a live table unusable.
        std::vector<TrackRow> v;
        v.push_back(makeRow("p", "P", 1.0, 1.0, 1.0, 1.0, 1.0, 500, 0));
        v.push_back(makeRow("q", "Q", 2.0, 2.0, 2.0, 2.0, 2.0, 500, 1));
        v.push_back(makeRow("r", "R", 3.0, 3.0, 3.0, 3.0, 3.0, 500, 2));
        v.push_back(makeRow("s", "S", 4.0, 4.0, 4.0, 4.0, 4.0, 500, 3));
        sortTrackRows(v, TrackSortKey::Age, true);
        CHECK(idsOf(v) == std::vector<std::string>({"p", "q", "r", "s"}));
        sortTrackRows(v, TrackSortKey::Age, false);
        CHECK(idsOf(v) == std::vector<std::string>({"p", "q", "r", "s"}));
    }

    // --- the altitude legend is WIDE ENOUGH FOR ITS OWN LABELS ---------------
    {
        // The shipped legend used a hard-coded 74 px panel, and three of its
        // six labels drew past it and were clipped by the map's clip rect:
        // "20-30 kft" and "10-20 kft" lost their final "t"s and "> 30 kft" lost
        // its last character. A legend the user cannot read is worse than none,
        // because nothing on screen says it has been truncated.
        //
        // Measured with a fixed-advance stand-in for the font rather than with
        // ImGui, which needs a context and a window: what is under test is the
        // arithmetic that turns label widths into a panel width, and it must
        // hold for ANY font, so the test asserts the PROPERTY - every label
        // fits, whatever the per-character advance - rather than a number.
        for (const float advance : {6.0f, 7.0f, 13.5f}) {
            const auto measure = [advance](const char* s) {
                return advance * static_cast<float>(std::strlen(s));
            };
            const float w = cascade::gui::altLegendWidth(measure);
            // The fixed parts, in the order the legend draws them: pad, swatch,
            // pad, label, pad.
            const float fixed = cascade::gui::kAltLegendPad + cascade::gui::kAltLegendSwatch +
                                cascade::gui::kAltLegendPad + cascade::gui::kAltLegendPad;
            for (int i = 0; i < kAltBandCount; ++i) {
                CHECK(w >= fixed + measure(altBandStyle(i).label));
            }
            // ...and no wider than the longest label needs, so the legend does
            // not creep across the map as bands are renamed.
            float longest = 0.0f;
            for (int i = 0; i < kAltBandCount; ++i) {
                const float lw = measure(altBandStyle(i).label);
                if (lw > longest) { longest = lw; }
            }
            CHECK_NEAR(w, fixed + longest, 0.001);
        }
    }

    // --- the table leaves room for the line drawn under it -------------------
    {
        using cascade::gui::tableHeightReservingLines;
        // Nothing to reserve: zero, which is ImGui's "take what is left" and
        // the behaviour every other table in the app wants.
        CHECK_NEAR(tableHeightReservingLines(200.0f, 18.0f, 0), 0.0f, 1e-6);
        CHECK_NEAR(tableHeightReservingLines(200.0f, 18.0f, -1), 0.0f, 1e-6);
        // One line reserved: exactly that line, off the bottom.
        CHECK_NEAR(tableHeightReservingLines(200.0f, 18.0f, 1), 182.0f, 1e-6);
        CHECK_NEAR(tableHeightReservingLines(200.0f, 18.0f, 2), 164.0f, 1e-6);
        // A PANE TOO SHORT TO HOLD BOTH still returns a positive height. Zero
        // here would mean "use everything left" and would put the reserved line
        // back below the fold - the exact bug this reserves against - so the
        // squeezed case must never produce it.
        CHECK(tableHeightReservingLines(10.0f, 18.0f, 1) > 0.0f);
        CHECK(tableHeightReservingLines(0.0f, 18.0f, 1) > 0.0f);
        CHECK(tableHeightReservingLines(-40.0f, 18.0f, 1) > 0.0f);
        // A degenerate line height cannot make it negative either.
        CHECK(tableHeightReservingLines(200.0f, 0.0f, 1) > 0.0f);
    }

    // --- the column-to-sort-key mapping and the opening order ----------------
    {
        using cascade::gui::kTrackSortDefaultColumn;
        using cascade::gui::trackSortKeyForColumn;
        // The eight columns, in the order the table declares them.
        CHECK(trackSortKeyForColumn(0) == TrackSortKey::Label);
        CHECK(trackSortKeyForColumn(1) == TrackSortKey::Id);
        CHECK(trackSortKeyForColumn(2) == TrackSortKey::Altitude);
        CHECK(trackSortKeyForColumn(3) == TrackSortKey::Speed);
        CHECK(trackSortKeyForColumn(4) == TrackSortKey::Course);
        CHECK(trackSortKeyForColumn(5) == TrackSortKey::Distance);
        CHECK(trackSortKeyForColumn(6) == TrackSortKey::Bearing);
        CHECK(trackSortKeyForColumn(7) == TrackSortKey::Age);
        // Out of range answers with the default rather than with whatever the
        // last case happened to be.
        CHECK(trackSortKeyForColumn(-1) == TrackSortKey::Label);
        CHECK(trackSortKeyForColumn(99) == TrackSortKey::Label);

        // THE TABLE OPENS SORTED BY CALLSIGN. The window's remembered sort key
        // was initialised to Distance while the CALLSIGN column carried
        // ImGuiTableColumnFlags_DefaultSort, so ImGui reported Label on the
        // first frame and overwrote it: the code said one thing and the screen
        // did another. The two halves are now one constant - this one, and a
        // static_assert next to the flagged column in app_window.cpp.
        CHECK(trackSortKeyForColumn(kTrackSortDefaultColumn) == TrackSortKey::Label);
    }

    return testSummary("test_track_metrics");
}
