// geo_math.hpp - great-circle distance, bearing and destination on a sphere, shared by the map models.
//
// STANDALONE. Nothing here is wired (docs/feature-records/net-map-models.md).
// Sphere model: mean Earth radius 6371.0088 km (IUGG mean radius R1). Haversine distance, initial bearing and the direct
// (destination) formula are the standard spherical formulas; on a sphere the error against the WGS-84 ellipsoid is up to
// about 0.5 percent, which is stated here and is plenty for a map overlay (not for survey work).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>

namespace cascade::core::geo {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kEarthRadiusKm = 6371.0088;

inline double degToRad(double d) { return d * kPi / 180.0; }
inline double radToDeg(double r) { return r * 180.0 / kPi; }

inline bool validLatLon(double lat, double lon) {
    return std::isfinite(lat) && std::isfinite(lon) && lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

// Wraps any finite longitude into [-180, 180).
inline double wrapLon(double lon) {
    if (!std::isfinite(lon)) return 0.0;
    double w = std::fmod(lon + 180.0, 360.0);
    if (w < 0) w += 360.0;
    return w - 180.0;
}

// Wraps any finite angle into [0, 360).
inline double wrap360(double a) {
    double w = std::fmod(a, 360.0);
    if (w < 0) w += 360.0;
    return w;
}

inline double distanceKm(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = degToRad(lat1), p2 = degToRad(lat2);
    const double dp = p2 - p1, dl = degToRad(lon2 - lon1);
    const double s1 = std::sin(dp / 2), s2 = std::sin(dl / 2);
    double a = s1 * s1 + std::cos(p1) * std::cos(p2) * s2 * s2;
    if (a > 1.0) a = 1.0;
    return 2.0 * kEarthRadiusKm * std::asin(std::sqrt(a));
}

// Initial bearing from point 1 to point 2, degrees clockwise from north in [0, 360). Coincident points give 0.
inline double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = degToRad(lat1), p2 = degToRad(lat2), dl = degToRad(lon2 - lon1);
    const double y = std::sin(dl) * std::cos(p2);
    const double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    if (x == 0.0 && y == 0.0) return 0.0;
    return wrap360(radToDeg(std::atan2(y, x)));
}

struct LatLon {
    double lat = 0, lon = 0;
};

inline LatLon destination(double lat, double lon, double bearing, double distKm) {
    const double d = distKm / kEarthRadiusKm, b = degToRad(bearing), p1 = degToRad(lat), l1 = degToRad(lon);
    double sp2 = std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(b);
    if (sp2 > 1) sp2 = 1;
    if (sp2 < -1) sp2 = -1;
    const double p2 = std::asin(sp2);
    const double l2 = l1 + std::atan2(std::sin(b) * std::sin(d) * std::cos(p1), std::cos(d) - std::sin(p1) * sp2);
    return {radToDeg(p2), wrapLon(radToDeg(l2))};
}

}  // namespace cascade::core::geo
