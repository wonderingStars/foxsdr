// solar_ephemeris.hpp - THE SOLAR SYSTEM'S POSITIONS for the 3D orbits view (docs/engine-stage4.md 6.65). Pure maths, no Slint, no I/O.
//
// THE PLUGIN PUBLISHES TOO LITTLE. The Solar System View plugin's panel gives each planet's distance from the Earth in AU, its elongation (angle from the Sun in
// the sky, E or W) and its distance from the Sun for TODAY only. A picture of the orbits needs where each planet IS on its orbit - the heliocentric position - and a
// date scrubber needs it for any date, so the host computes it. It uses the method the plugin itself names on its SYSTEM page ("Orbits: JPL approximate elements
// (Standish, Williams) 1800-2050"): E. M. Standish, "Keplerian Elements for Approximate Positions of the Major Planets", JPL Solar System Dynamics, Table 1
// (valid 1800 AD - 2050 AD), the element values read from https://ssd.jpl.nasa.gov/planets/approx_pos.html on 2026-11-10 and kept below digit for digit. The Earth is the
// Earth-Moon barycentre of that table, as the page says it may be. The Moon's direction and distance are the principal terms of the lunar series of Meeus,
// Astronomical Algorithms 2nd ed., chapter 47 (about 0.1 deg and a few hundred km), enough for a ring "not to scale".
//
// VERIFIED (tests/test_slint_host_globe.cpp): the heliocentric distance, the distance from the Earth, the elongation and its E / W side of all seven planets and the
// Moon's distance and illuminated fraction against JPL Horizons (geocentric observer table, 2026-11-10 00:00 UT, retrieved 2026-11-10), and the Mars opposition of January 2025.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <string>
#include <vector>

namespace foxsdr::solar {

struct Xyz {
    double x = 0.0, y = 0.0, z = 0.0;
};

// The planets other than the Earth, nearest the Sun first.
enum class Planet : int { Mercury = 0, Venus, Mars, Jupiter, Saturn, Uranus, Neptune };
inline constexpr int kPlanetCount = 7;
const char* planetName(Planet p);                    // "Mercury" .. "Neptune"
const char* planetName(int index);                   // "" outside 0..6

// Heliocentric position in the J2000 ecliptic frame, astronomical units. jd is a Julian date (UTC is used for TDB: the difference of about a minute moves a planet
// by less than the method's own error).
Xyz planetHeliocentric(Planet p, double jd);
Xyz earthHeliocentric(double jd);                    // the Earth-Moon barycentre

struct PlanetFacts {
    Planet planet = Planet::Mercury;
    Xyz helio;                    // heliocentric position, AU
    double sunDistanceAu = 0.0;   // r: from the Sun
    double earthDistanceAu = 0.0; // from the Earth
    double elongationDeg = 0.0;   // angle Sun - Earth - planet, 0..180
    bool evening = false;         // east of the Sun (it sets after the Sun): "E"; false: west, "W", the morning side
};
PlanetFacts planetFacts(Planet p, double jd);
double earthSunDistanceAu(double jd);

struct MoonFacts {
    Xyz geocentric;                // ecliptic frame, km
    double distanceKm = 0.0;
    double elongationDeg = 0.0;    // angle Sun - Earth - Moon
    double illuminatedFraction = 0.0;   // 0..1
    double eclipticLonDeg = 0.0;   // geocentric, of date
    bool evening = false;          // east of the Sun: the waxing side
};
MoonFacts moonFacts(double jd);

// ---- the orbit rings ----
// The orbit of a planet as a closed polyline of `samples` points (>= 16) in AU, the ellipse of the elements at jd (the plane and shape the planet is on at that date).
std::vector<Xyz> orbitRing(Planet p, double jd, int samples);
std::vector<Xyz> earthOrbitRing(double jd, int samples);

// ---- the display scale ----
// The radius a body at heliocentric distance r AU is drawn at, 0..1 of the outermost ring (rMaxAu). LINEAR is r / rMaxAu; LOG is the plugin's own knee form,
// log(1 + r / knee) / log(1 + rMaxAu / knee), knee 0.1 AU (the plugin's "Log-scale orbits" setting): the inner planets are not crushed into the Sun.
inline constexpr double kLogKneeAu = 0.1;
double displayRadius(double rAu, double rMaxAu, bool logScale);
// A position moved to its display radius along the same direction (the height above the ecliptic is scaled by the same factor).
Xyz displayPosition(const Xyz& heliocentricAu, double rMaxAu, bool logScale);

}  // namespace foxsdr::solar
