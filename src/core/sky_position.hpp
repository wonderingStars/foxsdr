// sky_position.hpp - azimuth and elevation of the Sun, the Moon and a short list of bright radio sources for an observer and a
// time (row M8 of the gaps survey, the tracker half; a Stellarium link is NOT built).
//
// STANDALONE and PURE. Not wired (see the record's WIRING HAND-OFF). No clock is read: the caller passes UNIX seconds (UT; the
// difference to terrestrial time, about 69 s, is ignored and costs the Moon about 0.01 degree).
//
// SOURCES (read 2026-11-11)
//  * Sun: U.S. Naval Observatory, "Approximate Solar Coordinates", https://aa.usno.navy.mil/faq/sun_approx : D = JD - 2451545.0;
//    g = 357.529 + 0.98560028 D; q = 280.459 + 0.98564736 D; L = q + 1.915 sin g + 0.020 sin 2g; e = 23.439 - 0.00000036 D;
//    RA = atan2(cos e sin L, cos L); sin(dec) = sin e sin L. The page states "about 1 arcminute" within two centuries of 2000.
//  * Moon: P. Schlyter, "How to compute planetary positions", https://stjarnhimlen.se/comp/ppcomp.html : the Moon's orbital elements
//    with the 12 longitude, 5 latitude and 2 distance perturbation terms, the ecliptic-to-equatorial rotation (obliquity
//    23.4393 - 3.563E-7 d) and the topocentric parallax correction in right ascension and declination. The page states 1 to 2
//    arcminutes. Day number there is d = JD - 2451543.5.
//  * Hour angle: local sidereal time = 280.46061837 + 360.98564736629 (JD - 2451545.0) + east longitude (degrees), the standard
//    Greenwich mean sidereal time expression; written from memory of the Astronomical Almanac: UNVERIFIED as a printed
//    formula (its result is checked against the March-2025 equinox and the 2024-04-08 eclipse in the test).
//  * Horizontal conversion: Schlyter, same page (azimuth 0 = north, 90 = east).
//  * Radio sources, J2000 positions as printed on Wikipedia (read 2026-11-11): Cassiopeia A 23h23m26.0s +58d48m41s; Cygnus A
//    19h59m28.3566s +40d44m02.096s; Crab Nebula (Taurus A) 05h34m31.97s +22d00m52.1s (the page's header line; its table gives
//    05h34m31.8s +22d01m03s); Virgo A = Messier 87 12h30m49.42338s +12d23m28.0439s; Sagittarius A* 17h45m40.0409s -29d00m28.118s.
//    Fixed sources are moved from J2000 to the date with a low-order precession (zeta, z, theta of the IAU 1976 model, written from
//    memory: UNVERIFIED), no nutation, aberration or proper motion.
//
// ACCURACY STATED. Elevation is GEOMETRIC (no atmospheric refraction; about 0.5 degree at the horizon). Sun: about 0.02 degree.
// Moon: about 0.05 degree after parallax. Radio sources: precession only, a few arcminutes. All are map-and-pointing grade.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "core/geo_math.hpp"

namespace cascade::core::sky {

using geo::degToRad;
using geo::radToDeg;
using geo::wrap360;

struct Equatorial {
    double raDeg = 0, decDeg = 0, distance = 0;   // distance: AU for the Sun, Earth radii for the Moon, 0 for the stars
};

struct AzEl {
    double azDeg = 0, elDeg = 0;
    Equatorial eq;
};

inline double julianDay(double unixSeconds) { return unixSeconds / 86400.0 + 2440587.5; }

inline double localSiderealDeg(double jd, double eastLonDeg) {
    return wrap360(280.46061837 + 360.98564736629 * (jd - 2451545.0) + eastLonDeg);
}

// Geocentric equatorial position of the Sun (of date).
inline Equatorial sunEquatorial(double jd) {
    const double D = jd - 2451545.0;
    const double g = degToRad(wrap360(357.529 + 0.98560028 * D));
    const double q = wrap360(280.459 + 0.98564736 * D);
    const double L = degToRad(wrap360(q + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g)));
    const double R = 1.00014 - 0.01671 * std::cos(g) - 0.00014 * std::cos(2 * g);
    const double e = degToRad(23.439 - 0.00000036 * D);
    Equatorial out;
    out.raDeg = wrap360(radToDeg(std::atan2(std::cos(e) * std::sin(L), std::cos(L))));
    out.decDeg = radToDeg(std::asin(std::sin(e) * std::sin(L)));
    out.distance = R;
    return out;
}

namespace detail {

inline double sind(double d) { return std::sin(degToRad(d)); }
inline double cosd(double d) { return std::cos(degToRad(d)); }

}  // namespace detail

// Geocentric equatorial position of the Moon (of date), Schlyter's method with perturbations.
inline Equatorial moonEquatorial(double jd) {
    using detail::cosd;
    using detail::sind;
    const double d = jd - 2451543.5;
    const double Nm = wrap360(125.1228 - 0.0529538083 * d);
    const double im = 5.1454;
    const double wm = wrap360(318.0634 + 0.1643573223 * d);
    const double a = 60.2666, e = 0.054900;
    const double Mm = wrap360(115.3654 + 13.0649929509 * d);
    const double Ms = wrap360(356.0470 + 0.9856002585 * d);
    const double ws = 282.9404 + 4.70935e-5 * d;

    // Kepler's equation by iteration (e = 0.0549).
    double E = Mm + radToDeg(e) * sind(Mm) * (1.0 + e * cosd(Mm));
    for (int i = 0; i < 20; ++i) {
        const double E1 = E - (E - radToDeg(e) * sind(E) - Mm) / (1.0 - e * cosd(E));
        const bool done = std::fabs(E1 - E) < 1e-6;
        E = E1;
        if (done) break;
    }
    const double xv = a * (cosd(E) - e), yv = a * std::sqrt(1.0 - e * e) * sind(E);
    const double v = radToDeg(std::atan2(yv, xv));
    double r = std::sqrt(xv * xv + yv * yv);
    const double xh = r * (cosd(Nm) * cosd(v + wm) - sind(Nm) * sind(v + wm) * cosd(im));
    const double yh = r * (sind(Nm) * cosd(v + wm) + cosd(Nm) * sind(v + wm) * cosd(im));
    const double zh = r * (sind(v + wm) * sind(im));
    double lon = radToDeg(std::atan2(yh, xh));
    double lat = radToDeg(std::atan2(zh, std::sqrt(xh * xh + yh * yh)));

    const double Ls = Ms + ws, Lm = Mm + wm + Nm;
    const double D = Lm - Ls, F = Lm - Nm;
    lon += -1.274 * sind(Mm - 2 * D) + 0.658 * sind(2 * D) - 0.186 * sind(Ms) - 0.059 * sind(2 * Mm - 2 * D) -
           0.057 * sind(Mm - 2 * D + Ms) + 0.053 * sind(Mm + 2 * D) + 0.046 * sind(2 * D - Ms) + 0.041 * sind(Mm - Ms) -
           0.035 * sind(D) - 0.031 * sind(Mm + Ms) - 0.015 * sind(2 * F - 2 * D) + 0.011 * sind(Mm - 4 * D);
    lat += -0.173 * sind(F - 2 * D) - 0.055 * sind(Mm - F - 2 * D) - 0.046 * sind(Mm + F - 2 * D) + 0.033 * sind(F + 2 * D) +
           0.017 * sind(2 * Mm + F);
    r += -0.58 * cosd(Mm - 2 * D) - 0.46 * cosd(2 * D);

    const double xg = r * cosd(lon) * cosd(lat), yg = r * sind(lon) * cosd(lat), zg = r * sind(lat);
    const double ecl = 23.4393 - 3.563e-7 * d;
    const double xe = xg, ye = yg * cosd(ecl) - zg * sind(ecl), ze = yg * sind(ecl) + zg * cosd(ecl);
    Equatorial out;
    out.raDeg = wrap360(radToDeg(std::atan2(ye, xe)));
    out.decDeg = radToDeg(std::atan2(ze, std::sqrt(xe * xe + ye * ye)));
    out.distance = r;
    return out;
}

// J2000 right ascension / declination (degrees) to the equinox of date, low-order precession.
inline Equatorial precessJ2000(double raDeg, double decDeg, double jd) {
    const double T = (jd - 2451545.0) / 36525.0;
    const double zeta = degToRad((2306.2181 * T + 0.30188 * T * T) / 3600.0);
    const double z = degToRad((2306.2181 * T + 1.09468 * T * T) / 3600.0);
    const double theta = degToRad((2004.3109 * T - 0.42665 * T * T) / 3600.0);
    const double ra = degToRad(raDeg), dec = degToRad(decDeg);
    const double A = std::cos(dec) * std::sin(ra + zeta);
    const double B = std::cos(theta) * std::cos(dec) * std::cos(ra + zeta) - std::sin(theta) * std::sin(dec);
    const double C = std::sin(theta) * std::cos(dec) * std::cos(ra + zeta) + std::cos(theta) * std::sin(dec);
    Equatorial out;
    out.raDeg = wrap360(radToDeg(std::atan2(A, B) + z));
    out.decDeg = radToDeg(std::asin(C > 1 ? 1 : (C < -1 ? -1 : C)));
    return out;
}

// Equatorial (of date) to azimuth / elevation for an observer; lat north-positive, lon east-positive, degrees.
inline AzEl toHorizontal(const Equatorial& eq, double latDeg, double lonDeg, double jd) {
    const double lst = localSiderealDeg(jd, lonDeg);
    const double ha = degToRad(lst - eq.raDeg), dec = degToRad(eq.decDeg), lat = degToRad(latDeg);
    const double x = std::cos(ha) * std::cos(dec), y = std::sin(ha) * std::cos(dec), z = std::sin(dec);
    const double xh = x * std::sin(lat) - z * std::cos(lat);
    const double yh = y;
    const double zh = x * std::cos(lat) + z * std::sin(lat);
    AzEl out;
    out.azDeg = wrap360(radToDeg(std::atan2(yh, xh)) + 180.0);
    out.elDeg = radToDeg(std::asin(zh > 1 ? 1 : (zh < -1 ? -1 : zh)));
    out.eq = eq;
    return out;
}

// Moon as seen from the surface: the equatorial parallax correction of the page, then horizontal coordinates.
inline AzEl moonAzEl(double latDeg, double lonDeg, double jd) {
    using detail::cosd;
    using detail::sind;
    const Equatorial g = moonEquatorial(jd);
    const double mpar = radToDeg(std::asin(1.0 / g.distance));
    const double gclat = latDeg - 0.1924 * sind(2 * latDeg);
    const double rho = 0.99833 + 0.00167 * cosd(2 * latDeg);
    double ha = localSiderealDeg(jd, lonDeg) - g.raDeg;
    ha = std::fmod(ha + 540.0, 360.0) - 180.0;
    const double gg = radToDeg(std::atan(std::tan(degToRad(gclat)) / cosd(ha)));
    Equatorial top = g;
    top.raDeg = wrap360(g.raDeg - mpar * rho * cosd(gclat) * sind(ha) / cosd(g.decDeg));
    if (std::fabs(gclat) < 1e-9 || std::fabs(sind(gg)) < 1e-12)
        top.decDeg = g.decDeg - mpar * rho * sind(-g.decDeg) * cosd(ha);
    else
        top.decDeg = g.decDeg - mpar * rho * sind(gclat) * sind(gg - g.decDeg) / sind(gg);
    return toHorizontal(top, latDeg, lonDeg, jd);
}

inline AzEl sunAzEl(double latDeg, double lonDeg, double jd) { return toHorizontal(sunEquatorial(jd), latDeg, lonDeg, jd); }

struct RadioSource {
    const char* name;
    double raHours, decDeg;   // J2000
};

inline const std::vector<RadioSource>& radioSources() {
    static const std::vector<RadioSource> k = {
        {"Cassiopeia A", 23.0 + 23.0 / 60 + 26.0 / 3600, 58.0 + 48.0 / 60 + 41.0 / 3600},
        {"Cygnus A", 19.0 + 59.0 / 60 + 28.3566 / 3600, 40.0 + 44.0 / 60 + 2.096 / 3600},
        {"Taurus A (Crab)", 5.0 + 34.0 / 60 + 31.97 / 3600, 22.0 + 0.0 / 60 + 52.1 / 3600},
        {"Virgo A (M87)", 12.0 + 30.0 / 60 + 49.42338 / 3600, 12.0 + 23.0 / 60 + 28.0439 / 3600},
        {"Sagittarius A*", 17.0 + 45.0 / 60 + 40.0409 / 3600, -(29.0 + 0.0 / 60 + 28.118 / 3600)},
    };
    return k;
}

struct SkyBody {
    std::string name;
    AzEl pos;
};

// Sun, Moon, then the five sources, for an observer and a UNIX time. Returns empty for a non-finite or out-of-range input.
inline std::vector<SkyBody> skyPositions(double latDeg, double lonDeg, double unixSeconds) {
    std::vector<SkyBody> out;
    if (!geo::validLatLon(latDeg, lonDeg) || !std::isfinite(unixSeconds)) return out;
    const double jd = julianDay(unixSeconds);
    out.push_back({"Sun", sunAzEl(latDeg, lonDeg, jd)});
    out.push_back({"Moon", moonAzEl(latDeg, lonDeg, jd)});
    for (const RadioSource& s : radioSources()) {
        const Equatorial eq = precessJ2000(s.raHours * 15.0, s.decDeg, jd);
        out.push_back({s.name, toHorizontal(eq, latDeg, lonDeg, jd)});
    }
    return out;
}

// Angular separation between two directions (degrees).
inline double separationDeg(const AzEl& a, const AzEl& b) {
    const double e1 = degToRad(a.elDeg), e2 = degToRad(b.elDeg), da = degToRad(a.azDeg - b.azDeg);
    double c = std::sin(e1) * std::sin(e2) + std::cos(e1) * std::cos(e2) * std::cos(da);
    if (c > 1) c = 1;
    if (c < -1) c = -1;
    return radToDeg(std::acos(c));
}

}  // namespace cascade::core::sky
