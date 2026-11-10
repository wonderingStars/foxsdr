// globe_scene.cpp - see globe_scene.hpp. Pure maths; the only other unit it reads is the flat map's coastline rings (map_compositor.hpp coastlineRings) and the
// Natural Earth data header.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "globe_scene.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "gui/coastline_data.hpp"
#include "map_compositor.hpp"

namespace foxsdr::globe {

namespace {

double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
bool finite3(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
double wrap360(double deg) {
    double r = std::fmod(deg, 360.0);
    if (r < 0.0) { r += 360.0; }
    return r;
}

}  // namespace

Vec3 normalized(const Vec3& a) {
    const double n = length(a);
    if (!(n > 0.0) || !std::isfinite(n)) { return {1.0, 0.0, 0.0}; }
    return {a.x / n, a.y / n, a.z / n};
}

// ---- the sphere ----

double normalizeLon(double lonDeg) {
    if (!std::isfinite(lonDeg)) { return 0.0; }
    double r = std::fmod(lonDeg + 180.0, 360.0);
    if (r <= 0.0) { r += 360.0; }
    return r - 180.0;
}

Vec3 toUnit(double latDeg, double lonDeg) {
    const double la = latDeg * kDegToRad, lo = lonDeg * kDegToRad;
    const double c = std::cos(la);
    return {c * std::cos(lo), c * std::sin(lo), std::sin(la)};
}
Vec3 toUnit(const LatLon& p) { return toUnit(p.lat, p.lon); }

bool fromVec(const Vec3& v, double& latDeg, double& lonDeg) {
    if (!finite3(v)) { return false; }
    const double n = length(v);
    if (!(n > 0.0)) { return false; }
    latDeg = std::asin(clampd(v.z / n, -1.0, 1.0)) * kRadToDeg;
    // at a pole the longitude is meaningless: 0 rather than atan2's noise
    lonDeg = (std::fabs(v.x) < 1e-15 && std::fabs(v.y) < 1e-15) ? 0.0 : normalizeLon(std::atan2(v.y, v.x) * kRadToDeg);
    return true;
}
LatLon fromVec(const Vec3& v) {
    LatLon p;
    fromVec(v, p.lat, p.lon);
    return p;
}

// ---- polylines ----

std::vector<Polyline> coastline(double radius) {
    namespace coast = cascade::gui::coastline;
    std::vector<Polyline> out;
    out.reserve(coast::kRunCount);
    for (std::uint32_t run = 0; run < coast::kRunCount; ++run) {
        Polyline line;
        line.reserve(coast::kRuns[run].count);
        for (std::uint32_t i = 0; i < coast::kRuns[run].count; ++i) {
            const std::size_t at = (static_cast<std::size_t>(coast::kRuns[run].first) + i) * 2u;
            const double lon = coast::kCoords[at] / 100.0;
            const double lat = coast::kCoords[at + 1] / 100.0;
            line.push_back(toUnit(lat, lon) * radius);
        }
        out.push_back(std::move(line));
    }
    return out;
}

std::vector<Polyline> graticule(double stepDeg, double sampleDeg, double radius) {
    stepDeg = clampd(stepDeg, 1.0, 90.0);
    sampleDeg = clampd(sampleDeg, 0.5, 30.0);
    std::vector<Polyline> out;
    // meridians: from the south pole to the north pole at each longitude
    const int nMer = static_cast<int>(std::floor(360.0 / stepDeg + 1e-9));
    const int perMer = std::max(2, static_cast<int>(std::ceil(180.0 / sampleDeg)) + 1);
    for (int m = 0; m < nMer; ++m) {
        const double lon = -180.0 + m * stepDeg;
        Polyline line;
        for (int i = 0; i < perMer; ++i) { line.push_back(toUnit(-90.0 + 180.0 * i / (perMer - 1), lon) * radius); }
        out.push_back(std::move(line));
    }
    // parallels: every step from the first multiple below the north pole down; the poles themselves are points, not lines
    const int perPar = std::max(8, static_cast<int>(std::ceil(360.0 / sampleDeg)) + 1);
    for (double lat = 0.0; lat < 90.0 - 1e-9; lat += stepDeg) {
        for (int sign = (lat == 0.0 ? 1 : -1); sign <= 1; sign += 2) {
            Polyline line;
            for (int i = 0; i < perPar; ++i) { line.push_back(toUnit(sign * lat, -180.0 + 360.0 * i / (perPar - 1)) * radius); }
            out.push_back(std::move(line));
        }
    }
    return out;
}

Polyline smallCircle(const LatLon& centre, double angularRadiusDeg, int samples, double radius) {
    samples = std::max(8, samples);
    const Vec3 c = toUnit(centre);
    // an orthonormal pair across the centre: u points north (or along y at a pole), v completes it
    Vec3 north{0.0, 0.0, 1.0};
    Vec3 u = north - c * dot(north, c);
    if (length(u) < 1e-9) { u = Vec3{0.0, 1.0, 0.0}; }
    u = normalized(u);
    const Vec3 v = cross(c, u);
    const double rho = angularRadiusDeg * kDegToRad;
    Polyline out;
    out.reserve(static_cast<std::size_t>(samples) + 1u);
    for (int i = 0; i <= samples; ++i) {
        const double a = 2.0 * kPi * (i == samples ? 0 : i) / samples;
        const Vec3 p = c * std::cos(rho) + (u * std::cos(a) + v * std::sin(a)) * std::sin(rho);
        out.push_back(normalized(p) * radius);
    }
    return out;
}

// ---- land ----

void LandMask::build(int width, int height) {
    w_ = std::max(0, width);
    h_ = std::max(0, height);
    cells_.assign(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_), 0u);
    if (w_ == 0 || h_ == 0) { return; }
    const auto& rings = foxsdr::slintmap::coastlineRings();
    std::vector<double> xs;
    for (int row = 0; row < h_; ++row) {
        const double lat = 90.0 - (row + 0.5) * 180.0 / h_;
        xs.clear();
        for (const auto& r : rings) {
            if (!r.closed) { continue; }
            const std::size_t n = r.lon.size();
            for (std::size_t i = 0; i < n; ++i) {
                const std::size_t j = (i + 1) % n;
                const double y0 = r.lat[i], y1 = r.lat[j];
                // half-open rule so a vertex exactly on the row is counted once
                if ((y0 <= lat) == (y1 <= lat)) { continue; }
                const double t = (lat - y0) / (y1 - y0);
                const double x = r.lon[i] + t * (r.lon[j] - r.lon[i]);
                // a ring that crosses the antimeridian runs past +/-180: its copies one world either side fill the other edge of the map
                for (int shift = -1; shift <= 1; ++shift) { xs.push_back(x + 360.0 * shift); }
            }
        }
        std::sort(xs.begin(), xs.end());
        std::uint8_t* line = &cells_[static_cast<std::size_t>(row) * static_cast<std::size_t>(w_)];
        for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
            // the cells whose centres lie in [xs[k], xs[k+1])
            const double c0 = (xs[k] + 180.0) * w_ / 360.0 - 0.5;
            const double c1 = (xs[k + 1] + 180.0) * w_ / 360.0 - 0.5;
            const int a = std::max(0, static_cast<int>(std::ceil(c0)));
            const int b = std::min(w_ - 1, static_cast<int>(std::ceil(c1)) - 1);
            for (int c = a; c <= b; ++c) { line[c] = 255u; }
        }
    }
}

bool LandMask::land(double latDeg, double lonDeg) const {
    if (cells_.empty() || !std::isfinite(latDeg) || !std::isfinite(lonDeg)) { return false; }
    const double lat = clampd(latDeg, -90.0, 90.0);
    const double lon = normalizeLon(lonDeg);
    int row = static_cast<int>(std::floor((90.0 - lat) * h_ / 180.0));
    int col = static_cast<int>(std::floor((lon + 180.0) * w_ / 360.0));
    row = std::clamp(row, 0, h_ - 1);
    col = std::clamp(col, 0, w_ - 1);
    return cells_[static_cast<std::size_t>(row) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(col)] != 0u;
}

// ---- time and the Sun ----

double julianDay(std::int64_t unixSeconds) { return static_cast<double>(unixSeconds) / 86400.0 + kJdUnixEpoch; }

double julianDay(int year, int month, int day, int hour, int minute, double second) {
    // Meeus ch. 7 (Gregorian calendar)
    int y = year, m = month;
    if (m <= 2) { y -= 1; m += 12; }
    const int a = y / 100;
    const int b = 2 - a + a / 4;
    const double dayFrac = day + (hour + (minute + second / 60.0) / 60.0) / 24.0;
    return std::floor(365.25 * (y + 4716)) + std::floor(30.6001 * (m + 1)) + dayFrac + b - 1524.5;
}

double gmstDegrees(double jd) {
    const double d = jd - 2451545.0;
    const double t = d / 36525.0;
    // IAU 1982 (Meeus 12.4)
    return wrap360(280.46061837 + 360.98564736629 * d + 0.000387933 * t * t - t * t * t / 38710000.0);
}

SunPosition sunPosition(double jd) {
    // Astronomical Almanac, "Low precision formulae for the Sun's coordinates" (section C)
    const double n = jd - 2451545.0;
    const double L = wrap360(280.460 + 0.9856474 * n);
    const double g = wrap360(357.528 + 0.9856003 * n) * kDegToRad;
    const double lambda = (L + 1.915 * std::sin(g) + 0.020 * std::sin(2.0 * g)) * kDegToRad;
    const double eps = (23.439 - 0.0000004 * n) * kDegToRad;
    SunPosition s;
    s.rightAscensionDeg = wrap360(std::atan2(std::cos(eps) * std::sin(lambda), std::cos(lambda)) * kRadToDeg);
    s.declinationDeg = std::asin(std::sin(eps) * std::sin(lambda)) * kRadToDeg;
    s.subLatDeg = s.declinationDeg;
    s.subLonDeg = normalizeLon(s.rightAscensionDeg - gmstDegrees(jd));
    return s;
}

double sinSolarElevation(const LatLon& point, const SunPosition& sun) {
    return dot(toUnit(point), toUnit(sun.subLatDeg, sun.subLonDeg));
}
double solarElevationDeg(const LatLon& point, const SunPosition& sun) {
    return std::asin(clampd(sinSolarElevation(point, sun), -1.0, 1.0)) * kRadToDeg;
}

Polyline terminator(const SunPosition& sun, double elevationDeg, int samples, double radius) {
    return smallCircle(LatLon{sun.subLatDeg, sun.subLonDeg}, 90.0 - elevationDeg, samples, radius);
}

double daylight(double sinElevation, double bandDeg) {
    if (!std::isfinite(sinElevation)) { return 0.0; }
    if (!(bandDeg > 0.0)) { return sinElevation > 0.0 ? 1.0 : 0.0; }
    const double e = std::asin(clampd(sinElevation, -1.0, 1.0)) * kRadToDeg;   // elevation, degrees
    const double t = clampd((e + bandDeg) / (2.0 * bandDeg), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// ---- great circles ----

double distanceKm(const LatLon& a, const LatLon& b) {
    const double p1 = a.lat * kDegToRad, p2 = b.lat * kDegToRad;
    const double dp = p2 - p1, dl = (b.lon - a.lon) * kDegToRad;
    const double h = std::sin(dp / 2.0) * std::sin(dp / 2.0) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2.0) * std::sin(dl / 2.0);
    return 2.0 * kEarthRadiusKm * std::asin(std::min(1.0, std::sqrt(h)));
}

double bearingDeg(const LatLon& from, const LatLon& to) {
    const double p1 = from.lat * kDegToRad, p2 = to.lat * kDegToRad, dl = (to.lon - from.lon) * kDegToRad;
    const double y = std::sin(dl) * std::cos(p2);
    const double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    if (std::fabs(x) < 1e-15 && std::fabs(y) < 1e-15) { return 0.0; }
    return wrap360(std::atan2(y, x) * kRadToDeg);
}

LatLon destination(const LatLon& from, double bearing, double distKm) {
    const double d = distKm / kEarthRadiusKm, th = bearing * kDegToRad;
    const double p1 = from.lat * kDegToRad, l1 = from.lon * kDegToRad;
    const double p2 = std::asin(clampd(std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(th), -1.0, 1.0));
    const double l2 = l1 + std::atan2(std::sin(th) * std::sin(d) * std::cos(p1), std::cos(d) - std::sin(p1) * std::sin(p2));
    return LatLon{p2 * kRadToDeg, normalizeLon(l2 * kRadToDeg)};
}

Polyline greatCircleArc(const LatLon& a, const LatLon& b, int samples, double liftFraction) {
    samples = std::max(2, samples);
    const Vec3 va = toUnit(a), vb = toUnit(b);
    const double cosw = clampd(dot(va, vb), -1.0, 1.0);
    const double w = std::acos(cosw);
    Vec3 perp{};   // used only when the ends coincide or are opposite
    if (w < 1e-9 || w > kPi - 1e-9) {
        perp = cross(va, Vec3{0.0, 0.0, 1.0});
        if (length(perp) < 1e-9) { perp = Vec3{0.0, 1.0, 0.0}; }
        perp = normalized(perp);
    }
    Polyline out;
    out.reserve(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        const double t = static_cast<double>(i) / (samples - 1);
        Vec3 p;
        if (w < 1e-9) {
            p = va;
        } else if (w > kPi - 1e-9) {
            p = va * std::cos(kPi * t) + perp * std::sin(kPi * t);
        } else {
            p = (va * std::sin((1.0 - t) * w) + vb * std::sin(t * w)) * (1.0 / std::sin(w));
        }
        out.push_back(normalized(p) * (1.0 + liftFraction * std::sin(kPi * t)));
    }
    return out;
}

Polyline skywaveArc(const LatLon& a, const LatLon& b, int hops, double shellHeightKm, double exaggeration, int samplesPerHop) {
    hops = std::clamp(hops, 1, 8);
    samplesPerHop = std::max(3, samplesPerHop);
    const double lift = std::max(0.0, shellHeightKm) * std::max(0.0, exaggeration) / kEarthRadiusKm;
    const Vec3 va = toUnit(a), vb = toUnit(b);
    // the hops' end points along the ONE great circle (so each hop's own arc stays on it)
    std::vector<LatLon> stops;
    const Polyline ground = greatCircleArc(a, b, hops + 1, 0.0);
    for (const Vec3& p : ground) { stops.push_back(fromVec(p)); }
    (void)va;
    (void)vb;
    Polyline out;
    for (int h = 0; h < hops; ++h) {
        const Polyline hop = greatCircleArc(stops[static_cast<std::size_t>(h)], stops[static_cast<std::size_t>(h) + 1u], samplesPerHop, lift);
        for (std::size_t i = (h == 0 ? 0u : 1u); i < hop.size(); ++i) { out.push_back(hop[i]); }
    }
    return out;
}

int hopCount(double distKm, double maxHopKm) {
    if (!(maxHopKm > 0.0) || !(distKm > 0.0) || !std::isfinite(distKm)) { return 1; }
    return std::max(1, static_cast<int>(std::ceil(distKm / maxHopKm - 1e-9)));
}

// ---- satellites ----

double footprintAngularRadiusDeg(double altKm, double minElevationDeg) {
    const double e = clampd(minElevationDeg, 0.0, 89.0) * kDegToRad;
    const double h = std::max(0.0, altKm);
    return (std::acos(clampd(kEarthRadiusKm * std::cos(e) / (kEarthRadiusKm + h), -1.0, 1.0)) - e) * kRadToDeg;
}

Polyline footprintCircle(const LatLon& subPoint, double altKm, double minElevationDeg, int samples, double radius) {
    return smallCircle(subPoint, footprintAngularRadiusDeg(altKm, minElevationDeg), samples, radius);
}

bool subSatellitePoint(const Vec3& eciKm, double jd, LatLon& subPoint, double& altKm) {
    if (!finite3(eciKm) || !(length(eciKm) > 0.0)) { return false; }
    const double th = gmstDegrees(jd) * kDegToRad;
    // rotate by -GMST about z: the Earth-fixed frame
    const Vec3 ecef{eciKm.x * std::cos(th) + eciKm.y * std::sin(th), -eciKm.x * std::sin(th) + eciKm.y * std::cos(th), eciKm.z};
    subPoint = fromVec(ecef);
    altKm = length(ecef) - kEarthRadiusKm;
    return true;
}

LookAngle lookAngle(const LatLon& rx, const LatLon& sub, double altKm) {
    LookAngle out;
    const double r = kEarthRadiusKm, rs = kEarthRadiusKm + std::max(0.0, altKm);
    const double gamma = std::acos(clampd(dot(toUnit(rx), toUnit(sub)), -1.0, 1.0));   // the central angle between the two
    // the slant range by the law of cosines, the elevation from the geometry of the triangle centre - observer - satellite
    const double rho2 = r * r + rs * rs - 2.0 * r * rs * std::cos(gamma);
    out.slantRangeKm = std::sqrt(std::max(0.0, rho2));
    if (out.slantRangeKm > 1e-9) {
        out.elevationDeg = std::asin(clampd((rs * std::cos(gamma) - r) / out.slantRangeKm, -1.0, 1.0)) * kRadToDeg;
    } else {
        out.elevationDeg = 90.0;
    }
    out.azimuthDeg = bearingDeg(rx, sub);
    out.visible = out.elevationDeg >= 0.0;
    return out;
}

// ---- locators ----

bool locatorToLatLon(const std::string& locator, double& latDeg, double& lonDeg) {
    const std::size_t n = locator.size();
    if (n != 2 && n != 4 && n != 6 && n != 8) { return false; }
    auto upper = [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); };
    auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    const char f0 = upper(locator[0]), f1 = upper(locator[1]);
    if (f0 < 'A' || f0 > 'R' || f1 < 'A' || f1 > 'R') { return false; }
    double lon = -180.0 + (f0 - 'A') * 20.0;
    double lat = -90.0 + (f1 - 'A') * 10.0;
    double cellLon = 20.0, cellLat = 10.0;
    if (n >= 4) {
        if (!isDigit(locator[2]) || !isDigit(locator[3])) { return false; }
        lon += (locator[2] - '0') * 2.0;
        lat += (locator[3] - '0') * 1.0;
        cellLon = 2.0;
        cellLat = 1.0;
    }
    if (n >= 6) {
        const char s0 = upper(locator[4]), s1 = upper(locator[5]);
        if (s0 < 'A' || s0 > 'X' || s1 < 'A' || s1 > 'X') { return false; }
        lon += (s0 - 'A') * (2.0 / 24.0);
        lat += (s1 - 'A') * (1.0 / 24.0);
        cellLon = 2.0 / 24.0;
        cellLat = 1.0 / 24.0;
    }
    if (n == 8) {
        if (!isDigit(locator[6]) || !isDigit(locator[7])) { return false; }
        lon += (locator[6] - '0') * (2.0 / 240.0);
        lat += (locator[7] - '0') * (1.0 / 240.0);
        cellLon = 2.0 / 240.0;
        cellLat = 1.0 / 240.0;
    }
    lonDeg = lon + cellLon / 2.0;
    latDeg = lat + cellLat / 2.0;
    return true;
}

bool locatorToLatLon(const std::string& locator, LatLon& out) {
    double la = 0.0, lo = 0.0;
    if (!locatorToLatLon(locator, la, lo)) { return false; }
    out = LatLon{la, lo};
    return true;
}

}  // namespace foxsdr::globe
