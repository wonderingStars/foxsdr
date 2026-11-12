// Tests for core/locator.hpp (Maidenhead 4/6/8 characters, distance and bearing) and core/geo_math.hpp.
// Reference values: Wikipedia "Maidenhead Locator System" (FN31pr contains 41.71463 N, -72.72713 E; the cell of a subsquare is
// 5' x 2.5'; two points in one subsquare are under 10.4 km apart), exact sphere geometry for the geo functions.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/locator.hpp"

#include <cmath>
#include <limits>
#include <random>
#include <string>

#include "test_check.hpp"

namespace loc = cascade::core::locator;
namespace geo = cascade::core::geo;

int main() {
    // --- decode: the cell centre of the page's own example --------------------------------------------------------------------
    loc::Position p;
    CHECK(loc::decode("FN31pr", p));
    CHECK(p.chars == 6);
    CHECK_NEAR(p.lat, 41.0 + 17 * 2.5 / 60 + 1.25 / 60, 1e-9);   // 41.72917
    CHECK_NEAR(p.lon, -74.0 + 15 * 5.0 / 60 + 2.5 / 60, 1e-9);   // -72.70833
    CHECK(loc::decode("fn31PR", p));                              // case-insensitive input
    CHECK_NEAR(p.lat, 41.729167, 1e-5);
    // the page's coordinates fall in that cell and encode back to it
    std::string s;
    CHECK(loc::encode(41.71463, -72.72713, 6, s));
    CHECK(s == "FN31pr");
    CHECK(loc::encode(41.71463, -72.72713, 4, s));
    CHECK(s == "FN31");
    CHECK(loc::encode(41.71463, -72.72713, 8, s));
    CHECK(s.size() == 8 && s.substr(0, 6) == "FN31pr");
    // Munich: the page's rules give the cell JN58td whose south-west corner is 48 07.5 N, 11 35 E
    CHECK(loc::encode(48.0 + 7.5 / 60 + 0.01, 11.0 + 35.0 / 60 + 0.01, 6, s));
    CHECK(s == "JN58td");
    CHECK(loc::decode("JN58td", p));
    CHECK_NEAR(p.lat, 48.125 + 1.25 / 60, 1e-9);
    CHECK_NEAR(p.lon, 11.0 + 35.0 / 60 + 2.5 / 60, 1e-9);

    // --- 4 and 8 characters: cell sizes -----------------------------------------------------------------------------------------
    CHECK(loc::decode("JN58", p));
    CHECK(p.chars == 4);
    CHECK_NEAR(p.lat, 48.5, 1e-9);     // centre of a 1 degree cell 48..49
    CHECK_NEAR(p.lon, 11.0, 1e-9);     // centre of a 2 degree cell 10..12
    CHECK(loc::decode("JN58td45", p));
    CHECK(p.chars == 8);
    CHECK_NEAR(p.lon, 11.0 + 35.0 / 60 + 4 * 30.0 / 3600 + 15.0 / 3600, 1e-9);
    CHECK_NEAR(p.lat, 48.125 + 5 * 15.0 / 3600 + 7.5 / 3600, 1e-9);

    // --- round trip: 20000 random points, error at most half a cell ----------------------------------------------------------------
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> la(-90, 90), lo(-180, 180);
    int bad = 0;
    for (int i = 0; i < 20000; ++i) {
        const double lat = la(rng), lon = lo(rng);
        for (int chars : {4, 6, 8}) {
            std::string g;
            if (!loc::encode(lat, lon, chars, g) || !loc::decode(g, p)) { ++bad; continue; }
            const double hl = chars == 4 ? 0.5 : (chars == 6 ? 1.25 / 60 : 7.5 / 3600);
            const double hw = chars == 4 ? 1.0 : (chars == 6 ? 2.5 / 60 : 15.0 / 3600);
            if (std::fabs(p.lat - lat) > hl + 1e-9 || std::fabs(p.lon - lon) > hw + 1e-9) ++bad;
        }
    }
    CHECK(bad == 0);

    // --- boundaries and refusals ------------------------------------------------------------------------------------------------
    CHECK(loc::encode(90.0, 180.0, 6, s));      // the pole / antimeridian fold into the last cell, not past 'R'
    CHECK(s.substr(0, 2) == "RR");
    CHECK(loc::encode(-90.0, -180.0, 4, s));
    CHECK(s == "AA00");
    CHECK(!loc::encode(90.5, 0, 6, s));
    CHECK(!loc::encode(0, 180.5, 6, s));
    CHECK(!loc::encode(std::numeric_limits<double>::quiet_NaN(), 0, 6, s));
    CHECK(!loc::encode(0, std::numeric_limits<double>::infinity(), 6, s));
    CHECK(!loc::encode(10, 10, 5, s));          // 5 characters is not a locator length
    CHECK(!loc::encode(10, 10, 2, s));
    CHECK(!loc::decode("", p));
    CHECK(!loc::decode("FN3", p));
    CHECK(!loc::decode("FN31p", p));
    CHECK(!loc::decode("FN31pr4", p));
    CHECK(!loc::decode("SN31", p));             // field letters stop at R
    CHECK(!loc::decode("FN31yy", p));           // subsquare letters stop at X
    CHECK(!loc::decode("FN3a", p));             // square must be digits
    CHECK(!loc::decode("FN31pr4x", p));         // extended square must be digits
    CHECK(!loc::decode("FN31 r", p));
    CHECK(!loc::decode(std::string("FN31pr\0\0", 8), p));
    CHECK(!loc::decode("FN31pr\xC3\xA9", p));
    {
        loc::Position keep;
        keep.lat = 7;
        CHECK(!loc::decode("ZZ99", keep));
        CHECK(keep.lat == 7);                   // out untouched on failure
    }

    // --- the claim on the page: one subsquare is under 10.4 km across ----------------------------------------------------------
    {
        // the widest cell is on the equator: JJ00aa spans lon 0..5', lat 0..2.5'
        const double d = geo::distanceKm(0, 0, 2.5 / 60, 5.0 / 60);
        CHECK(d < 10.4);
        CHECK(d > 10.0);
    }

    // --- distance and bearing ---------------------------------------------------------------------------------------------------
    CHECK_NEAR(geo::distanceKm(0, 0, 0, 1), geo::kEarthRadiusKm * geo::kPi / 180.0, 1e-9);   // 111.195 km a degree
    CHECK_NEAR(geo::distanceKm(0, 0, 90, 0), geo::kEarthRadiusKm * geo::kPi / 2.0, 1e-6);    // 10007.54 km, a quarter meridian
    CHECK_NEAR(geo::distanceKm(0, 0, 0, 180), geo::kEarthRadiusKm * geo::kPi, 1e-6);         // antipodal
    CHECK_NEAR(geo::distanceKm(10, 20, 10, 20), 0.0, 1e-12);
    CHECK_NEAR(geo::bearingDeg(0, 0, 1, 0), 0.0, 1e-9);
    CHECK_NEAR(geo::bearingDeg(0, 0, 0, 1), 90.0, 1e-9);
    CHECK_NEAR(geo::bearingDeg(0, 0, -1, 0), 180.0, 1e-9);
    CHECK_NEAR(geo::bearingDeg(0, 0, 0, -1), 270.0, 1e-9);
    CHECK(geo::bearingDeg(5, 5, 5, 5) == 0.0);
    {
        loc::Leg leg;
        CHECK(loc::between("IO91wm", "FN31pr", leg));      // London to Connecticut: about 5500 km, heading west-north-west
        CHECK(leg.distanceKm > 5400 && leg.distanceKm < 5650);
        CHECK(leg.bearingDeg > 280 && leg.bearingDeg < 300);
        loc::Leg back;
        CHECK(loc::between("FN31pr", "IO91wm", back));
        CHECK_NEAR(back.distanceKm, leg.distanceKm, 1e-9);
        CHECK(!loc::between("IO91wm", "bad", leg));
        CHECK(!loc::between("", "FN31", leg));
        // a point reached by destination() is at the stated distance and bearing
        const geo::LatLon q = geo::destination(51.5, -0.1, 289.0, 1234.5);
        CHECK_NEAR(geo::distanceKm(51.5, -0.1, q.lat, q.lon), 1234.5, 1e-6);
        CHECK_NEAR(geo::bearingDeg(51.5, -0.1, q.lat, q.lon), 289.0, 1e-6);
        // across the antimeridian the longitude wraps
        const geo::LatLon w = geo::destination(0, 179.5, 90, 111.195);
        CHECK(w.lon < -179.0 && w.lon > -180.0);
    }
    CHECK_NEAR(geo::wrapLon(190), -170, 1e-12);
    CHECK_NEAR(geo::wrapLon(-190), 170, 1e-12);
    CHECK_NEAR(geo::wrapLon(180), -180, 1e-12);
    CHECK_NEAR(geo::wrap360(-1), 359, 1e-12);

    return testSummary("test_locator");
}
