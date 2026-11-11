// Tests for core/sky_position.hpp. Published values used (all read 2026-11-11):
//  * March equinox 2025: 20 March 09:01 UT (Wikipedia "March equinox", citing the U.S. Naval Observatory): the Sun's declination
//    is 0 at that moment.
//  * Total solar eclipse of 8 April 2024 (Wikipedia): greatest eclipse 18:18:29 UTC at 25.3 N 104.1 W, magnitude 1.0566, so the
//    Moon's centre is within 0.9 arc-minutes of the Sun's centre as seen from that point.
//  * The sources' J2000 coordinates, used for circumpolar / culmination consistency (no published az/el exists for them here).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/sky_position.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

#include "test_check.hpp"

namespace sky = cascade::core::sky;

static double unixOf(int y, int m, int d, int hh, int mm, double ss) {
    // days from civil (Hinnant)
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = era * 146097L + static_cast<long>(doe) - 719468L;
    return days * 86400.0 + hh * 3600.0 + mm * 60.0 + ss;
}

int main() {
    // time base: unix epoch is JD 2440587.5; 2000-01-01 12:00 UT is JD 2451545.0
    CHECK_NEAR(sky::julianDay(unixOf(2000, 1, 1, 12, 0, 0)), 2451545.0, 1e-9);
    CHECK_NEAR(sky::julianDay(0.0), 2440587.5, 1e-12);

    // Sun: declination zero at the 2025 March equinox
    {
        const double jd = sky::julianDay(unixOf(2025, 3, 20, 9, 1, 0));
        const auto eq = sky::sunEquatorial(jd);
        std::printf("INFO equinox 2025-03-20 09:01 UT: Sun dec %.4f deg, RA %.4f deg\n", eq.decDeg, eq.raDeg);
        CHECK(std::fabs(eq.decDeg) < 0.03);   // the page's accuracy is about 1 arc-minute (0.017 deg)
        CHECK(std::fabs(eq.raDeg) < 0.05 || std::fabs(eq.raDeg - 360.0) < 0.05);
        // a day later the declination is about +0.39 degree
        const auto eq2 = sky::sunEquatorial(jd + 1.0);
        CHECK(eq2.decDeg > 0.30 && eq2.decDeg < 0.45);
        // earth-sun distance stays in 0.983..1.017 AU over a year
        double lo = 9, hi = 0;
        for (int i = 0; i < 366; ++i) {
            const double R = sky::sunEquatorial(jd + i).distance;
            lo = std::fmin(lo, R);
            hi = std::fmax(hi, R);
        }
        CHECK(lo > 0.982 && hi < 1.018);
    }
    // Sun at local solar noon on the equinox at 45 N, 0 E: the equation of time is about -7.5 min, so noon is near 12:07 UT
    {
        double best = -99, bestAz = 0;
        for (int mn = 0; mn < 30; ++mn) {
            const auto a = sky::sunAzEl(45.0, 0.0, sky::julianDay(unixOf(2025, 3, 20, 12, mn, 0)));
            if (a.elDeg > best) { best = a.elDeg; bestAz = a.azDeg; }
        }
        std::printf("INFO equinox noon 45N: max Sun elevation %.3f deg at az %.2f\n", best, bestAz);
        CHECK_NEAR(best, 45.0, 0.3);
        CHECK_NEAR(bestAz, 180.0, 3.0);
    }
    // Sun az/el in the four quadrants at the equinox, 0 N 0 E: rises east, sets west
    {
        const auto morning = sky::sunAzEl(0.0, 0.0, sky::julianDay(unixOf(2025, 3, 20, 7, 0, 0)));
        const auto evening = sky::sunAzEl(0.0, 0.0, sky::julianDay(unixOf(2025, 3, 20, 17, 0, 0)));
        CHECK(morning.elDeg > 0 && morning.azDeg > 80 && morning.azDeg < 100);
        CHECK(evening.elDeg > 0 && evening.azDeg > 260 && evening.azDeg < 280);
        const auto midnight = sky::sunAzEl(0.0, 0.0, sky::julianDay(unixOf(2025, 3, 20, 0, 0, 0)));
        CHECK(midnight.elDeg < -80);
    }

    // Eclipse: the Moon's centre lies within a fraction of a degree of the Sun's centre; the Sun is high
    {
        const double jd = sky::julianDay(unixOf(2024, 4, 8, 18, 18, 29));
        const auto sun = sky::sunAzEl(25.3, -104.1, jd);
        const auto moon = sky::moonAzEl(25.3, -104.1, jd);
        const double sep = sky::separationDeg(sun, moon);
        std::printf("INFO eclipse 2024-04-08 18:18:29 UT at 25.3N 104.1W: Sun az %.2f el %.2f; Moon az %.2f el %.2f; separation %.4f deg\n",
                    sun.azDeg, sun.elDeg, moon.azDeg, moon.elDeg, sep);
        CHECK(sun.elDeg > 60 && sun.elDeg < 80);
        CHECK(sep < 0.1);                    // published: under 0.015 deg at the point; the formulae are good to ~0.05 deg
        // the same instant seen from far away (the UK, where the Sun is below the horizon): separation is the Moon parallax (up to about 1 deg)
        const auto sunUk = sky::sunAzEl(51.5, -0.1, jd);
        const auto moonUk = sky::moonAzEl(51.5, -0.1, jd);
        CHECK(sky::separationDeg(sunUk, moonUk) > 0.3 && sky::separationDeg(sunUk, moonUk) < 1.6);   // parallax moves the Moon off the Sun
        // an hour before and after the Moon and Sun drift apart at about half a degree an hour
        const double sepBefore = sky::separationDeg(sky::sunAzEl(25.3, -104.1, jd - 2.0 / 24), sky::moonAzEl(25.3, -104.1, jd - 2.0 / 24));
        const double sepAfter = sky::separationDeg(sky::sunAzEl(25.3, -104.1, jd + 2.0 / 24), sky::moonAzEl(25.3, -104.1, jd + 2.0 / 24));
        CHECK(sepBefore > 0.6 && sepBefore < 1.6);
        CHECK(sepAfter > 0.6 && sepAfter < 1.6);
    }
    // Moon distance stays in its range over a month
    {
        double lo = 99, hi = 0;
        const double jd0 = sky::julianDay(unixOf(2024, 4, 1, 0, 0, 0));
        for (int h = 0; h < 30 * 24; ++h) {
            const auto m = sky::moonEquatorial(jd0 + h / 24.0);
            lo = std::fmin(lo, m.distance);
            hi = std::fmax(hi, m.distance);
        }
        std::printf("INFO Moon distance over April 2024: %.2f .. %.2f Earth radii\n", lo, hi);
        CHECK(lo > 55.5 && hi < 64.5);
        CHECK(hi - lo > 5);
    }

    // Radio sources: circumpolar / culmination consistency at 51.5 N (the sources' J2000 declinations, precessed to the date)
    {
        const double jd0 = sky::julianDay(unixOf(2025, 6, 1, 0, 0, 0));
        double casMax = -99, casMin = 99, sgrMax = -99, cygMax = -99;
        double casMaxAz = 0;
        for (int mn = 0; mn < 1440; mn += 2) {
            const double jd = jd0 + mn / 1440.0;
            const auto all = sky::skyPositions(51.5, 0.0, (jd - 2440587.5) * 86400.0);
            CHECK(all.size() == 7);
            const auto& cas = all[2].pos;
            if (cas.elDeg > casMax) { casMax = cas.elDeg; casMaxAz = cas.azDeg; }
            casMin = std::fmin(casMin, cas.elDeg);
            sgrMax = std::fmax(sgrMax, all[6].pos.elDeg);
            cygMax = std::fmax(cygMax, all[3].pos.elDeg);
        }
        std::printf("INFO 51.5N: Cas A el %.2f..%.2f (max at az %.1f), Cyg A max %.2f, Sgr A* max %.2f\n", casMin, casMax, casMaxAz, cygMax, sgrMax);
        CHECK_NEAR(casMax, 90.0 - std::fabs(51.5 - 58.9), 0.4);   // upper culmination
        CHECK_NEAR(casMin, 51.5 + 58.9 - 90.0, 0.4);              // lower culmination (circumpolar)
        CHECK(std::fmin(casMaxAz, 360.0 - casMaxAz) < 2.5);   // dec 58.9 is north of 51.5 N: it culminates north of the zenith, az 0
        CHECK_NEAR(cygMax, 90.0 - (51.5 - 40.8), 0.4);
        CHECK_NEAR(sgrMax, 90.0 - 51.5 - 29.0, 0.4);
    }

    // Refusals
    CHECK(sky::skyPositions(91.0, 0.0, 0.0).empty());
    CHECK(sky::skyPositions(0.0, 181.0, 0.0).empty());
    CHECK(sky::skyPositions(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0).empty());
    CHECK(sky::skyPositions(0.0, 0.0, std::numeric_limits<double>::infinity()).empty());
    CHECK(sky::skyPositions(90.0, 0.0, 1.7e9).size() == 7);      // the poles are valid observers
    CHECK(sky::skyPositions(-90.0, 180.0, 1.7e9).size() == 7);
    for (const auto& b : sky::skyPositions(90.0, 0.0, 1.7e9)) CHECK(std::isfinite(b.pos.azDeg) && std::isfinite(b.pos.elDeg));
    for (const auto& b : sky::skyPositions(0.0, 0.0, 1.7e9)) CHECK(b.pos.azDeg >= 0 && b.pos.azDeg < 360);

    return testSummary("test_sky_position");
}
