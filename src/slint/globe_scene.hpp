// globe_scene.hpp - THE GLOBE VIEWS' GEOMETRY (docs/engine-stage4.md 6.65, step 1): the pure maths every globe of the Slint window stands on, with no Slint,
// no renderer and no I/O. The HF propagation globe, the FT8 reception globe, the GOES disk, the satellite tracking globe and (through the same unit-sphere
// conventions) the solar system's date handling all call these functions; the scene renderer is lane A's (scene3d.*), which draws what these return.
//
// CONVENTIONS. Positions are Vec3 on the unit sphere (or scaled by a radius): x toward (lat 0, lon 0), y toward (lat 0, lon +90 E), z toward the north pole.
// Angles in degrees at the edges of this API, radians inside. The Earth is a SPHERE of the IUGG mean radius (kEarthRadiusKm): geodesic distances differ from
// the WGS 84 ellipsoid's by up to 0.5 %, which is finer than any of the views can show, and every figure the views print is labelled "about".
//
// VERIFIED AGAINST PUBLISHED FIGURES (tests/test_slint_globe_scene.cpp names each source): the Sun's position against the low-precision formulae of the
// Astronomical Almanac (accuracy 0.01 deg, 1950-2050) as checked by Meeus, Astronomical Algorithms 2nd ed., Example 25.a (1992 Oct 13: apparent right ascension
// 198.38083 deg, declination -7.78507 deg) and the USNO's March 2025 equinox (20 March 09:01 UT, declination 0); Greenwich mean sidereal time against Meeus
// Example 12.b (1987 Apr 10 19h21m UT = 8h34m57.09s); London-New York about 5 570 km and London-Sydney about 16 994 km; a geostationary satellite sees to 81.3 deg.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace foxsdr::globe {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kDegToRad = kPi / 180.0;
inline constexpr double kRadToDeg = 180.0 / kPi;
inline constexpr double kEarthRadiusKm = 6371.0088;      // IUGG mean radius

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};
inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double length(const Vec3& a) { return std::sqrt(dot(a, a)); }
Vec3 normalized(const Vec3& a);   // a zero vector comes back as (1, 0, 0), never NaN

struct LatLon {
    double lat = 0.0, lon = 0.0;   // degrees, north and east positive; lon in (-180, 180]
};

using Polyline = std::vector<Vec3>;

// ---- the sphere ----
double normalizeLon(double lonDeg);                   // into (-180, 180]
Vec3 toUnit(double latDeg, double lonDeg);            // radius 1
Vec3 toUnit(const LatLon& p);
// The point under a vector (any length). False for a zero or non-finite vector (the outputs are left alone).
bool fromVec(const Vec3& v, double& latDeg, double& lonDeg);
LatLon fromVec(const Vec3& v);

// ---- the coastline, graticule and small circles as 3D polylines ----
// The Natural Earth 1:110m coastline (gui/coastline_data.hpp, the data the flat map draws) as one polyline per run of the data, at `radius`.
std::vector<Polyline> coastline(double radius = 1.0);
// Meridians every `stepDeg` (from -180, the poles included once) and parallels every `stepDeg` (the equator included, the poles not), each sampled every
// `sampleDeg` of arc. stepDeg in [1, 90], sampleDeg in [0.5, 30]; anything else is clamped.
std::vector<Polyline> graticule(double stepDeg, double sampleDeg, double radius = 1.0);
// The points at angular distance `angularRadiusDeg` from `centre`, `samples` of them (>= 8), the first repeated at the end so the line closes.
Polyline smallCircle(const LatLon& centre, double angularRadiusDeg, int samples, double radius = 1.0);

// ---- land: the coastline as an equirectangular mask ----
// The land's inside from the coastline's rings (even-odd fill), one byte per cell, 255 land and 0 sea, row 0 the NORTH edge, column 0 the -180 meridian.
// A cell is sampled at its centre. Built once (about 20 ms at 1440 x 720) and kept by the caller.
class LandMask {
public:
    LandMask() = default;
    explicit LandMask(int width, int height) { build(width, height); }
    void build(int width, int height);
    bool empty() const { return cells_.empty(); }
    int width() const { return w_; }
    int height() const { return h_; }
    bool land(double latDeg, double lonDeg) const;       // the cell under the point; false while empty
    const std::vector<std::uint8_t>& cells() const { return cells_; }

private:
    int w_ = 0, h_ = 0;
    std::vector<std::uint8_t> cells_;
};

// ---- time and the Sun ----
inline constexpr double kJdUnixEpoch = 2440587.5;
double julianDay(std::int64_t unixSeconds);           // UTC seconds since 1970-01-01 -> Julian date (UT1 taken equal to UTC, as every view here does)
double julianDay(int year, int month, int day, int hour, int minute, double second);   // proleptic Gregorian, UTC
// Greenwich mean sidereal time in degrees, [0, 360).
double gmstDegrees(double jd);

struct SunPosition {
    double declinationDeg = 0.0;
    double rightAscensionDeg = 0.0;   // [0, 360)
    double subLatDeg = 0.0;           // the sub-solar point: latitude = declination
    double subLonDeg = 0.0;           // longitude = right ascension - GMST, in (-180, 180]
};
// The Astronomical Almanac's low-precision formulae (accuracy about 0.01 deg between 1950 and 2050; outside that the answer is still finite and drifts).
SunPosition sunPosition(double jd);
// Sine of the Sun's elevation at a point (the dot product with the sub-solar unit vector): +1 sun overhead, 0 on the terminator, -1 anti-solar.
double sinSolarElevation(const LatLon& point, const SunPosition& sun);
double solarElevationDeg(const LatLon& point, const SunPosition& sun);
// The line of constant solar elevation: elevationDeg 0 is the day/night terminator, -6 the civil twilight's edge. A small circle about the sub-solar point.
Polyline terminator(const SunPosition& sun, double elevationDeg, int samples, double radius = 1.0);
// Daylight as a smooth 0..1 across a greyline band of +/- bandDeg of solar elevation (1 sunlit, 0 night); bandDeg <= 0 is the hard edge.
double daylight(double sinElevation, double bandDeg);

// ---- great circles, distance and bearing ----
double distanceKm(const LatLon& a, const LatLon& b);            // haversine on the sphere
double bearingDeg(const LatLon& from, const LatLon& to);        // initial true bearing, [0, 360); 0 for coincident points
LatLon destination(const LatLon& from, double bearing, double distKm);
// Points of the great circle from a to b (both ends included, `samples` >= 2 of them) at radius 1, each lifted by sin(pi t) * `liftFraction` of the radius
// (0 on the ground). Antipodal ends take the path through a fixed perpendicular, so the arc is still finite.
Polyline greatCircleArc(const LatLon& a, const LatLon& b, int samples, double liftFraction = 0.0);
// A multi-hop skywave path: `hops` equal ground hops from a to b, each lifted to `shellHeightKm * exaggeration` above the ground at its middle and back to the
// ground at its end (the ionosphere's reflection point), `samplesPerHop` points per hop. hops is clamped to [1, 8].
Polyline skywaveArc(const LatLon& a, const LatLon& b, int hops, double shellHeightKm, double exaggeration, int samplesPerHop);
// How many hops a path of distKm needs at the longest single hop the layer allows (about 4 000 km off the F2 layer, about 2 000 km off E); at least 1.
int hopCount(double distKm, double maxHopKm);

// ---- satellites ----
// Angular radius (degrees of arc on the sphere) of the circle a satellite at altKm sees down to `minElevationDeg` above the horizon:
// acos(R cos(e) / (R + h)) - e. At 0 deg elevation and 35 786 km this is the geostationary disc's 81.3 deg.
double footprintAngularRadiusDeg(double altKm, double minElevationDeg);
// The footprint on the surface as a closed small circle about the sub-satellite point.
Polyline footprintCircle(const LatLon& subPoint, double altKm, double minElevationDeg, int samples, double radius = 1.0);
// The sub-satellite point and altitude from an Earth-centred inertial position (km) at a Julian date: the position is rotated by GMST into the Earth-fixed frame
// and read off the sphere (a geocentric, not geodetic, latitude: up to 0.19 deg from the geodetic one). False for a zero or non-finite position.
bool subSatellitePoint(const Vec3& eciKm, double jd, LatLon& subPoint, double& altKm);

struct LookAngle {
    double elevationDeg = 0.0;    // above the horizon; negative: below it (not visible)
    double azimuthDeg = 0.0;      // true bearing, [0, 360)
    double slantRangeKm = 0.0;
    bool visible = false;
};
// Where a satellite at altKm over `sub` is seen from `rx` (a point on the surface), by spherical geometry.
LookAngle lookAngle(const LatLon& rx, const LatLon& sub, double altKm);

// ---- locators ----
// A Maidenhead locator of 2, 4, 6 or 8 characters (field AA..RR, square 00..99, subsquare aa..xx, extended 00..99; either case) to the CENTRE of its cell.
// False (outputs untouched) for anything else: wrong length, an out-of-range letter or digit. The inverse of core/maidenhead.hpp's maidenheadGrid6.
bool locatorToLatLon(const std::string& locator, double& latDeg, double& lonDeg);
bool locatorToLatLon(const std::string& locator, LatLon& out);

}  // namespace foxsdr::globe
