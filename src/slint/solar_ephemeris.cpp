// solar_ephemeris.cpp - see solar_ephemeris.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "solar_ephemeris.hpp"

#include <algorithm>
#include <cmath>

namespace foxsdr::solar {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRad = kPi / 180.0;
constexpr double kAuKm = 149597870.7;

// E. M. Standish, JPL "Keplerian Elements for Approximate Positions of the Major Planets", Table 1 (1800 AD - 2050 AD): per body the six elements at J2000 and
// their rates per Julian century, in the order a (au), e, I (deg), L (deg), longitude of perihelion (deg), longitude of the ascending node (deg).
struct Elements {
    double a, e, i, l, peri, node;
};
struct Body {
    Elements at, rate;
};
constexpr Body kMercury{{0.38709927, 0.20563593, 7.00497902, 252.25032350, 77.45779628, 48.33076593},
                        {0.00000037, 0.00001906, -0.00594749, 149472.67411175, 0.16047689, -0.12534081}};
constexpr Body kVenus{{0.72333566, 0.00677672, 3.39467605, 181.97909950, 131.60246718, 76.67984255},
                      {0.00000390, -0.00004107, -0.00078890, 58517.81538729, 0.00268329, -0.27769418}};
constexpr Body kEarth{{1.00000261, 0.01671123, -0.00001531, 100.46457166, 102.93768193, 0.0},    // "EM Bary"
                      {0.00000562, -0.00004392, -0.01294668, 35999.37244981, 0.32327364, 0.0}};
constexpr Body kMars{{1.52371034, 0.09339410, 1.84969142, -4.55343205, -23.94362959, 49.55953891},
                     {0.00001847, 0.00007882, -0.00813131, 19140.30268499, 0.44441088, -0.29257343}};
constexpr Body kJupiter{{5.20288700, 0.04838624, 1.30439695, 34.39644051, 14.72847983, 100.47390909},
                        {-0.00011607, -0.00013253, -0.00183714, 3034.74612775, 0.21252668, 0.20469106}};
constexpr Body kSaturn{{9.53667594, 0.05386179, 2.48599187, 49.95424423, 92.59887831, 113.66242448},
                       {-0.00125060, -0.00050991, 0.00193609, 1222.49362201, -0.41897216, -0.28867794}};
constexpr Body kUranus{{19.18916464, 0.04725744, 0.77263783, 313.23810451, 170.95427630, 74.01692503},
                       {-0.00196176, -0.00004397, -0.00242939, 428.48202785, 0.40805281, 0.04240589}};
constexpr Body kNeptune{{30.06992276, 0.00859048, 1.77004347, -55.12002969, 44.96476227, 131.78422574},
                        {0.00026291, 0.00005105, 0.00035372, 218.45945325, -0.32241464, -0.00508664}};

const Body& bodyOf(Planet p) {
    switch (p) {
        case Planet::Mercury: return kMercury;
        case Planet::Venus: return kVenus;
        case Planet::Mars: return kMars;
        case Planet::Jupiter: return kJupiter;
        case Planet::Saturn: return kSaturn;
        case Planet::Uranus: return kUranus;
        case Planet::Neptune: return kNeptune;
    }
    return kMercury;
}

double centuries(double jd) { return (jd - 2451545.0) / 36525.0; }

Elements at(const Body& b, double t) {
    return {b.at.a + b.rate.a * t, b.at.e + b.rate.e * t, b.at.i + b.rate.i * t, b.at.l + b.rate.l * t, b.at.peri + b.rate.peri * t, b.at.node + b.rate.node * t};
}

// the orbital-plane point (xp, yp) in the ecliptic frame
Xyz rotate(const Elements& el, double xp, double yp) {
    const double w = (el.peri - el.node) * kRad, om = el.node * kRad, in = el.i * kRad;
    const double cw = std::cos(w), sw = std::sin(w), co = std::cos(om), so = std::sin(om), ci = std::cos(in), si = std::sin(in);
    Xyz p;
    p.x = (cw * co - sw * so * ci) * xp + (-sw * co - cw * so * ci) * yp;
    p.y = (cw * so + sw * co * ci) * xp + (-sw * so + cw * co * ci) * yp;
    p.z = (sw * si) * xp + (cw * si) * yp;
    return p;
}

double wrapPm180(double deg) {
    double r = std::fmod(deg + 180.0, 360.0);
    if (r < 0.0) { r += 360.0; }
    return r - 180.0;
}

// Kepler's equation by Newton's method, radians
double solveKepler(double mRad, double e) {
    double eAnom = mRad + e * std::sin(mRad);
    for (int i = 0; i < 30; ++i) {
        const double d = (eAnom - e * std::sin(eAnom) - mRad) / (1.0 - e * std::cos(eAnom));
        eAnom -= d;
        if (std::fabs(d) < 1e-12) { break; }
    }
    return eAnom;
}

Xyz positionOf(const Body& b, double jd) {
    const Elements el = at(b, centuries(jd));
    const double m = wrapPm180(el.l - el.peri) * kRad;
    const double eAnom = solveKepler(m, el.e);
    const double xp = el.a * (std::cos(eAnom) - el.e);
    const double yp = el.a * std::sqrt(1.0 - el.e * el.e) * std::sin(eAnom);
    return rotate(el, xp, yp);
}

Xyz sub(const Xyz& a, const Xyz& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double dotp(const Xyz& a, const Xyz& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double norm(const Xyz& a) { return std::sqrt(dotp(a, a)); }

double angleBetweenDeg(const Xyz& a, const Xyz& b) {
    const double c = dotp(a, b) / (norm(a) * norm(b));
    return std::acos(std::clamp(c, -1.0, 1.0)) / kRad;
}

double eclipticLonDeg(const Xyz& v) { return std::atan2(v.y, v.x) / kRad; }

}  // namespace

const char* planetName(Planet p) { return planetName(static_cast<int>(p)); }
const char* planetName(int index) {
    static const char* const names[kPlanetCount] = {"Mercury", "Venus", "Mars", "Jupiter", "Saturn", "Uranus", "Neptune"};
    return (index >= 0 && index < kPlanetCount) ? names[index] : "";
}

Xyz planetHeliocentric(Planet p, double jd) { return positionOf(bodyOf(p), jd); }
Xyz earthHeliocentric(double jd) { return positionOf(kEarth, jd); }

double earthSunDistanceAu(double jd) { return norm(earthHeliocentric(jd)); }

PlanetFacts planetFacts(Planet p, double jd) {
    PlanetFacts f;
    f.planet = p;
    f.helio = planetHeliocentric(p, jd);
    const Xyz earth = earthHeliocentric(jd);
    const Xyz geo = sub(f.helio, earth);
    const Xyz sun = {-earth.x, -earth.y, -earth.z};   // the Sun as seen from the Earth
    f.sunDistanceAu = norm(f.helio);
    f.earthDistanceAu = norm(geo);
    f.elongationDeg = angleBetweenDeg(sun, geo);
    f.evening = wrapPm180(eclipticLonDeg(geo) - eclipticLonDeg(sun)) > 0.0;
    return f;
}

MoonFacts moonFacts(double jd) {
    const double t = centuries(jd);
    const double lp = 218.3164477 + 481267.88123421 * t;
    const double d = (297.8501921 + 445267.1114034 * t) * kRad;
    const double m = (357.5291092 + 35999.0502909 * t) * kRad;
    const double mp = (134.9633964 + 477198.8675055 * t) * kRad;
    const double f = (93.2720950 + 483202.0175233 * t) * kRad;
    // Meeus ch. 47, table 47.A / 47.B, the terms of 0.01 deg and 1000 km and up (longitude in 1e-6 deg, distance in 1e-3 km, coefficient of E left out: the
    // Earth's orbital eccentricity factor is 1 within 0.002 for the next centuries)
    const double lon = lp + (6288774.0 * std::sin(mp) + 1274027.0 * std::sin(2 * d - mp) + 658314.0 * std::sin(2 * d) + 213618.0 * std::sin(2 * mp) -
                             185116.0 * std::sin(m) - 114332.0 * std::sin(2 * f) + 58793.0 * std::sin(2 * d - 2 * mp) + 57066.0 * std::sin(2 * d - m - mp) +
                             53322.0 * std::sin(2 * d + mp) + 45758.0 * std::sin(2 * d - m) - 40923.0 * std::sin(m - mp) - 34720.0 * std::sin(d) -
                             30383.0 * std::sin(m + mp) + 15327.0 * std::sin(2 * d - 2 * f)) / 1e6;
    const double lat = (5128122.0 * std::sin(f) + 280602.0 * std::sin(mp + f) + 277693.0 * std::sin(mp - f) + 173237.0 * std::sin(2 * d - f)) / 1e6;
    const double dist = 385000.56 + (-20905355.0 * std::cos(mp) - 3699111.0 * std::cos(2 * d - mp) - 2955968.0 * std::cos(2 * d) - 569925.0 * std::cos(2 * mp) +
                                     48888.0 * std::cos(m) - 3149.0 * std::cos(2 * f) + 246158.0 * std::cos(2 * d - 2 * mp) - 152138.0 * std::cos(2 * d - m - mp) -
                                     170733.0 * std::cos(2 * d + mp) - 204586.0 * std::cos(2 * d - m) - 129620.0 * std::cos(m - mp) + 108743.0 * std::cos(d) +
                                     104755.0 * std::cos(m + mp)) / 1000.0;
    // the series is of date; the planets are J2000: take the precession (1.3970 deg per century) off the longitude
    const double lonJ2000 = lon - 1.3970 * t;
    MoonFacts out;
    out.distanceKm = dist;
    out.eclipticLonDeg = lon;
    out.geocentric = {dist * std::cos(lat * kRad) * std::cos(lonJ2000 * kRad), dist * std::cos(lat * kRad) * std::sin(lonJ2000 * kRad), dist * std::sin(lat * kRad)};
    const Xyz earth = earthHeliocentric(jd);
    const Xyz sun = {-earth.x, -earth.y, -earth.z};
    out.elongationDeg = angleBetweenDeg(sun, out.geocentric);
    out.evening = wrapPm180(lonJ2000 - eclipticLonDeg(sun)) > 0.0;
    // the phase angle at the Moon: tan i = R sin(psi) / (D - R cos(psi))
    const double r = norm(earth) * kAuKm;
    const double psi = out.elongationDeg * kRad;
    const double i = std::atan2(r * std::sin(psi), dist - r * std::cos(psi));
    out.illuminatedFraction = (1.0 + std::cos(i)) / 2.0;
    return out;
}

namespace {
std::vector<Xyz> ringOf(const Body& b, double jd, int samples) {
    samples = std::max(16, samples);
    const Elements el = at(b, centuries(jd));
    std::vector<Xyz> out;
    out.reserve(static_cast<std::size_t>(samples) + 1u);
    for (int k = 0; k <= samples; ++k) {
        const double eAnom = 2.0 * kPi * (k == samples ? 0 : k) / samples;
        out.push_back(rotate(el, el.a * (std::cos(eAnom) - el.e), el.a * std::sqrt(1.0 - el.e * el.e) * std::sin(eAnom)));
    }
    return out;
}
}  // namespace

std::vector<Xyz> orbitRing(Planet p, double jd, int samples) { return ringOf(bodyOf(p), jd, samples); }
std::vector<Xyz> earthOrbitRing(double jd, int samples) { return ringOf(kEarth, jd, samples); }

double displayRadius(double rAu, double rMaxAu, bool logScale) {
    if (!(rAu > 0.0) || !(rMaxAu > 0.0) || !std::isfinite(rAu)) { return 0.0; }
    if (!logScale) { return rAu / rMaxAu; }
    return std::log1p(rAu / kLogKneeAu) / std::log1p(rMaxAu / kLogKneeAu);
}

Xyz displayPosition(const Xyz& h, double rMaxAu, bool logScale) {
    const double r = norm(h);
    if (!(r > 0.0)) { return {0.0, 0.0, 0.0}; }
    const double s = displayRadius(r, rMaxAu, logScale) / r;
    return {h.x * s, h.y * s, h.z * s};
}

}  // namespace foxsdr::solar
