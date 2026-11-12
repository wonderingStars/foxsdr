// locator.hpp - Maidenhead locators to and from latitude / longitude (4, 6 and 8 characters) and the distance and bearing
// between two of them. Row M1b of the gaps survey (FT8 locators on the map).
//
// The tree already has core/maidenhead.hpp (maidenheadGrid6: lat/lon -> 6 characters ONLY, for the plugin receiver-locator
// capability). It cannot decode, cannot do 4 or 8 characters and has no distance; it is left untouched and this file is named
// locator.hpp so the two do not collide.
//
// STANDALONE. Not wired (see the record's WIRING HAND-OFF).
// SOURCE: Wikipedia, "Maidenhead Locator System", https://en.wikipedia.org/wiki/Maidenhead_Locator_System (read 2026-11-11):
// longitude pair first then latitude; field = letters A-R, 20 deg x 10 deg; square = digits 0-9, 2 deg x 1 deg;
// subsquare = letters A-X, 5 arc-minutes x 2.5 arc-minutes; extended square = digits, 30 arc-seconds x 15 arc-seconds.
// Subsquare letters are written lowercase here (the page's own example FN31pr) and read in either case. A decoded locator
// is the CENTRE of its cell, so the position error is at most half a cell.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <string>
#include <string_view>

#include "core/geo_math.hpp"

namespace cascade::core::locator {

struct Position {
    double lat = 0, lon = 0;
    int chars = 0;
};

// Accepts exactly 4, 6 or 8 characters of the right alphabets for each pair. Returns false (out untouched) otherwise.
inline bool decode(std::string_view loc, Position& out) {
    const std::size_t n = loc.size();
    if (n != 4 && n != 6 && n != 8) return false;
    auto up = [](char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; };
    auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    const char f0 = up(loc[0]), f1 = up(loc[1]);
    if (f0 < 'A' || f0 > 'R' || f1 < 'A' || f1 > 'R') return false;
    if (!isDigit(loc[2]) || !isDigit(loc[3])) return false;
    double lon = -180.0 + (f0 - 'A') * 20.0 + (loc[2] - '0') * 2.0;
    double lat = -90.0 + (f1 - 'A') * 10.0 + (loc[3] - '0') * 1.0;
    double lonSize = 2.0, latSize = 1.0;
    if (n >= 6) {
        const char s0 = up(loc[4]), s1 = up(loc[5]);
        if (s0 < 'A' || s0 > 'X' || s1 < 'A' || s1 > 'X') return false;
        lonSize = 5.0 / 60.0;
        latSize = 2.5 / 60.0;
        lon += (s0 - 'A') * lonSize;
        lat += (s1 - 'A') * latSize;
    }
    if (n == 8) {
        if (!isDigit(loc[6]) || !isDigit(loc[7])) return false;
        lonSize = 30.0 / 3600.0;
        latSize = 15.0 / 3600.0;
        lon += (loc[6] - '0') * lonSize;
        lat += (loc[7] - '0') * latSize;
    }
    out.lon = lon + lonSize / 2.0;
    out.lat = lat + latSize / 2.0;
    out.chars = static_cast<int>(n);
    return true;
}

// chars must be 4, 6 or 8; lat/lon must be finite and in range (lat 90 and lon 180 fold into the last cell).
inline bool encode(double lat, double lon, int chars, std::string& out) {
    if (chars != 4 && chars != 6 && chars != 8) return false;
    if (!std::isfinite(lat) || !std::isfinite(lon) || lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return false;
    if (lon >= 180.0) lon = 180.0 - 1e-9;
    if (lat >= 90.0) lat = 90.0 - 1e-9;
    double x = lon + 180.0, y = lat + 90.0;
    std::string s;
    const int fx = static_cast<int>(x / 20.0), fy = static_cast<int>(y / 10.0);
    s += static_cast<char>('A' + fx);
    s += static_cast<char>('A' + fy);
    x -= fx * 20.0;
    y -= fy * 10.0;
    const int sx = static_cast<int>(x / 2.0), sy = static_cast<int>(y / 1.0);
    s += static_cast<char>('0' + sx);
    s += static_cast<char>('0' + sy);
    x -= sx * 2.0;
    y -= sy * 1.0;
    if (chars >= 6) {
        const double w = 5.0 / 60.0, h = 2.5 / 60.0;
        int ux = static_cast<int>(x / w), uy = static_cast<int>(y / h);
        if (ux > 23) ux = 23;
        if (uy > 23) uy = 23;
        s += static_cast<char>('a' + ux);
        s += static_cast<char>('a' + uy);
        x -= ux * w;
        y -= uy * h;
        if (chars == 8) {
            int ex = static_cast<int>(x / (30.0 / 3600.0)), ey = static_cast<int>(y / (15.0 / 3600.0));
            if (ex > 9) ex = 9;
            if (ey > 9) ey = 9;
            s += static_cast<char>('0' + ex);
            s += static_cast<char>('0' + ey);
        }
    }
    out = std::move(s);
    return true;
}

struct Leg {
    double distanceKm = 0, bearingDeg = 0;
};

inline bool between(std::string_view a, std::string_view b, Leg& out) {
    Position pa, pb;
    if (!decode(a, pa) || !decode(b, pb)) return false;
    out.distanceKm = geo::distanceKm(pa.lat, pa.lon, pb.lat, pb.lon);
    out.bearingDeg = geo::bearingDeg(pa.lat, pa.lon, pb.lat, pb.lon);
    return true;
}

}  // namespace cascade::core::locator
