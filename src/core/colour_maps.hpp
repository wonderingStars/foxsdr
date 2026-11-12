// Waterfall colour maps of 256 entries, COMPUTED here in the OKLab colour space (no table is copied from anywhere, so there is no licence to
// carry). Five maps with our own names:
//
//   DeepSea     dark blue -> teal -> green -> yellow
//   Ember       black -> red -> orange -> yellow -> white (a heat map)
//   Slate       grey (neutral, dark to light)
//   Spectrum    a rainbow-like hue sweep (violet-blue, cyan, green, yellow, orange) whose LIGHTNESS still rises monotonically
//   SafeBlueYellow   dark blue -> grey -> light yellow, using only the lightness and the blue-yellow axis (nothing rides on red against
//                    green), for viewers with red-green colour blindness
//
// HOW. Each map is a path through OKLab given by a few (L, C, h) key points; the colour at every point of the path is pulled into the sRGB
// gamut by shrinking its chroma (lightness and hue kept); the path is sampled densely, and 256 entries are taken at EQUAL ARC LENGTH in
// OKLab so neighbouring entries differ by a near-constant perceptual step; finally each entry is rounded to 8 bits choosing, among the
// eight floor/ceil roundings of its channels, the one closest to the exact colour whose lightness does not fall below the previous entry's
// (so lightness is monotone in the table that is actually shipped, not only in the float maths).
//
// OKLab: Bjorn Ottosson, "A perceptual color space for image processing", https://bottosson.github.io/posts/oklab/ (2020-12-23, matrices as
// updated 2021-01-25); the code there is public domain (an MIT alternative is offered). sRGB transfer function: IEC 61966-2-1 (the constants
// 12.92, 0.0031308, 1.055, 0.055, 2.4).
//
// Pure, allocation only in buildColourMap (set-up); colourMap() builds each map once on first use.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cascade::core::colour {

struct Rgb8 {
    std::uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Rgb8&) const = default;
};
struct Lab {
    double L = 0, a = 0, b = 0;
};
struct Lin {
    double r = 0, g = 0, b = 0;
};

enum class MapId { DeepSea = 0, Ember, Slate, Spectrum, SafeBlueYellow };
constexpr int kMapCount = 5;
constexpr std::size_t kEntries = 256;
using Table = std::array<Rgb8, kEntries>;

// Our own plain names (ids are stable settings keys; names are the strings to translate).
inline const char* mapId(MapId m) {
    static const char* k[kMapCount] = {"deep-sea", "ember", "slate", "spectrum", "safe-blue-yellow"};
    const int i = static_cast<int>(m);
    return (i >= 0 && i < kMapCount) ? k[i] : k[0];
}
inline const char* mapName(MapId m) {
    static const char* k[kMapCount] = {"Deep sea", "Ember", "Slate", "Spectrum", "Blue-yellow safe"};
    const int i = static_cast<int>(m);
    return (i >= 0 && i < kMapCount) ? k[i] : k[0];
}

inline double srgbDecode(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
inline double srgbEncode(double c) { return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055; }

inline Lab linearToOklab(const Lin& c) {
    const double l = 0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b;
    const double m = 0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b;
    const double s = 0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b;
    const double l_ = std::cbrt(l), m_ = std::cbrt(m), s_ = std::cbrt(s);
    return {0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_, 1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
            0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_};
}
inline Lin oklabToLinear(const Lab& c) {
    const double l_ = c.L + 0.3963377774 * c.a + 0.2158037573 * c.b;
    const double m_ = c.L - 0.1055613458 * c.a - 0.0638541728 * c.b;
    const double s_ = c.L - 0.0894841775 * c.a - 1.2914855480 * c.b;
    const double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    return {+4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s, -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
            -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s};
}
inline Lab rgb8ToOklab(const Rgb8& c) {
    return linearToOklab({srgbDecode(c.r / 255.0), srgbDecode(c.g / 255.0), srgbDecode(c.b / 255.0)});
}
inline double labDistance(const Lab& x, const Lab& y) {
    return std::sqrt((x.L - y.L) * (x.L - y.L) + (x.a - y.a) * (x.a - y.a) + (x.b - y.b) * (x.b - y.b));
}

namespace detail {

constexpr double kPi = 3.14159265358979323846;

struct Key {
    double t, L, C, h;  // t in [0,1]; h in degrees
};

inline bool inGamut(const Lin& c) {
    constexpr double e = 1e-6;
    return c.r >= -e && c.r <= 1 + e && c.g >= -e && c.g <= 1 + e && c.b >= -e && c.b <= 1 + e;
}

// Colour at (L, C, h) with the chroma shrunk until it fits the sRGB gamut (lightness and hue kept).
inline Lab fitGamut(double L, double C, double hDeg) {
    L = std::clamp(L, 0.0, 1.0);
    const double h = hDeg * kPi / 180.0;
    auto at = [&](double k) { return Lab{L, k * C * std::cos(h), k * C * std::sin(h)}; };
    if (inGamut(oklabToLinear(at(1.0)))) { return at(1.0); }
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (inGamut(oklabToLinear(at(mid)))) { lo = mid; } else { hi = mid; }
    }
    return at(lo);
}

inline void interp(const std::vector<Key>& k, double t, double* L, double* C, double* h) {
    std::size_t i = 0;
    while (i + 2 < k.size() && t > k[i + 1].t) { ++i; }
    const double u = std::clamp((t - k[i].t) / (k[i + 1].t - k[i].t), 0.0, 1.0);
    *L = k[i].L + u * (k[i + 1].L - k[i].L);
    *C = k[i].C + u * (k[i + 1].C - k[i].C);
    *h = k[i].h + u * (k[i + 1].h - k[i].h);
}

inline std::vector<Key> keysFor(MapId m) {
    switch (m) {
        case MapId::DeepSea:
            return {{0.0, 0.14, 0.05, 268}, {0.25, 0.38, 0.11, 235}, {0.5, 0.58, 0.12, 180}, {0.75, 0.78, 0.15, 135}, {1.0, 0.96, 0.17, 105}};
        case MapId::Ember:
            return {{0.0, 0.0, 0.0, 25}, {0.15, 0.22, 0.09, 25}, {0.4, 0.48, 0.19, 32}, {0.65, 0.72, 0.17, 65}, {0.85, 0.90, 0.12, 95}, {1.0, 1.0, 0.0, 100}};
        case MapId::Slate:
            return {{0.0, 0.08, 0.0, 0}, {1.0, 0.98, 0.0, 0}};
        case MapId::Spectrum:
            return {{0.0, 0.30, 0.14, 295}, {0.2, 0.46, 0.15, 262}, {0.4, 0.62, 0.15, 205}, {0.6, 0.76, 0.17, 142}, {0.8, 0.86, 0.17, 100}, {1.0, 0.93, 0.15, 55}};
        case MapId::SafeBlueYellow:
            // a stays 0 for the whole path: hue 270 (pure -b) for the first half, 90 (pure +b) for the second, chroma passing through ~0
            return {{0.0, 0.20, 0.12, 270}, {0.45, 0.58, 0.0, 270}, {0.5, 0.60, 0.0, 90}, {1.0, 0.92, 0.17, 90}};
    }
    return {};
}

}  // namespace detail

// The float path of a map, 256 entries at equal OKLab arc length (what the 8-bit table approximates).
inline std::array<Lab, kEntries> exactMap(MapId m) {
    const std::vector<detail::Key> keys = detail::keysFor(m);
    constexpr std::size_t D = 8192;
    std::vector<Lab> dense(D);
    std::vector<double> arc(D, 0.0);
    for (std::size_t i = 0; i < D; ++i) {
        double L, C, h;
        detail::interp(keys, static_cast<double>(i) / static_cast<double>(D - 1), &L, &C, &h);
        dense[i] = detail::fitGamut(L, C, h);
        if (i > 0) { arc[i] = arc[i - 1] + labDistance(dense[i - 1], dense[i]); }
    }
    // Walk the dense path so that every CHORD between neighbouring entries has the same length: bisect on that length until the 256th
    // entry lands on the end of the path (equal arc length would leave chords shorter where the path bends).
    std::array<Lab, kEntries> out;
    auto walk = [&](double chord, bool store) {
        std::size_t j = 0;
        Lab cur = dense[0];
        if (store) { out[0] = cur; }
        for (std::size_t e = 1; e < kEntries; ++e) {
            while (j + 1 < D && labDistance(cur, dense[j + 1]) < chord) { ++j; }
            if (j + 1 >= D) { if (store) { for (std::size_t k = e; k < kEntries; ++k) { out[k] = dense[D - 1]; } } return false; }
            // interpolate between dense[j] and dense[j+1] to the point at distance `chord` from cur
            const Lab a = dense[j], b = dense[j + 1];
            double lo = 0.0, hi = 1.0;
            for (int it = 0; it < 30; ++it) {
                const double mid = 0.5 * (lo + hi);
                const Lab q{a.L + mid * (b.L - a.L), a.a + mid * (b.a - a.a), a.b + mid * (b.b - a.b)};
                if (labDistance(cur, q) < chord) { lo = mid; } else { hi = mid; }
            }
            cur = {a.L + hi * (b.L - a.L), a.a + hi * (b.a - a.a), a.b + hi * (b.b - a.b)};
            if (store) { out[e] = cur; }
        }
        return true;  // 255 chords fitted before the path ran out: the chord is not too long
    };
    double lo = 0.0, hi = arc[D - 1] / static_cast<double>(kEntries - 1) * 1.5;
    for (int it = 0; it < 50; ++it) {
        const double mid = 0.5 * (lo + hi);
        if (walk(mid, false)) { lo = mid; } else { hi = mid; }  // fitted: try longer chords; ran out of path: shorter
    }
    walk(lo, true);
    out[kEntries - 1] = dense[D - 1];
    return out;
}

inline Table buildColourMap(MapId m) {
    const std::array<Lab, kEntries> ex = exactMap(m);
    Table t;
    double prevL = -1.0;
    for (std::size_t e = 0; e < kEntries; ++e) {
        const Lin lin = oklabToLinear(ex[e]);
        const double enc[3] = {std::clamp(srgbEncode(std::clamp(lin.r, 0.0, 1.0)), 0.0, 1.0) * 255.0,
                               std::clamp(srgbEncode(std::clamp(lin.g, 0.0, 1.0)), 0.0, 1.0) * 255.0,
                               std::clamp(srgbEncode(std::clamp(lin.b, 0.0, 1.0)), 0.0, 1.0) * 255.0};
        Rgb8 best{};
        double bestD = 1e9, bestL = -1.0;
        Rgb8 fallback{};
        double fallbackL = -1.0;
        for (int mask = 0; mask < 64; ++mask) {  // each channel: floor-1, floor, ceil, ceil+1
            const bool neutral = std::fabs(ex[e].a) < 1e-9 && std::fabs(ex[e].b) < 1e-9;  // a grey stays exactly grey
            const int dr = mask & 3, dg = (mask >> 2) & 3, db = (mask >> 4) & 3;
            if (neutral && !(dr == dg && dg == db)) { continue; }
            auto pick = [](double v, int d) { return static_cast<std::uint8_t>(std::clamp(std::floor(v) + (d - 1), 0.0, 255.0)); };
            Rgb8 c;
            c.r = pick(enc[0], dr);
            c.g = pick(enc[1], dg);
            c.b = pick(enc[2], db);
            const Lab cl = rgb8ToOklab(c);
            const double d = labDistance(cl, ex[e]);
            if (cl.L >= prevL && d < bestD) {
                bestD = d;
                best = c;
                bestL = cl.L;
            }
            if (cl.L > fallbackL) {
                fallbackL = cl.L;
                fallback = c;
            }
        }
        if (bestD >= 1e9) {  // no rounding keeps the lightness from falling: take the lightest (stays monotone from here on)
            best = fallback;
            bestL = fallbackL;
        }
        t[e] = best;
        prevL = std::max(prevL, bestL);
    }
    return t;
}

// Each map built once on first use (thread-safe static initialisation); never null.
inline const Table& colourMap(MapId m) {
    static const std::array<Table, kMapCount> all = [] {
        std::array<Table, kMapCount> a;
        for (int i = 0; i < kMapCount; ++i) { a[static_cast<std::size_t>(i)] = buildColourMap(static_cast<MapId>(i)); }
        return a;
    }();
    const int i = static_cast<int>(m);
    return all[static_cast<std::size_t>((i >= 0 && i < kMapCount) ? i : 0)];
}

// Colour for a normalised level; NaN and below 0 read as entry 0, above 1 as entry 255.
inline Rgb8 colourAt(const Table& t, float x01) {
    if (!(x01 > 0.0f)) { return t[0]; }
    if (x01 >= 1.0f) { return t[kEntries - 1]; }
    return t[static_cast<std::size_t>(x01 * 255.0f + 0.5f)];
}

}  // namespace cascade::core::colour
