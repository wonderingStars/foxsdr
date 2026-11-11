// access_palette.hpp - colour-vision-safe and high-contrast UI colour sets as
// DATA, with the verifier that proves them.
//
// FIVE SETS: Normal (the Workbench skin's own values for these roles),
// Deuteranopia-safe, Protanopia-safe, Tritanopia-safe, High contrast. Each is
// one table of the colours whose MEANING is carried by hue: the four status
// colours (ok / warn / bad / info: lamps, chips, meter ends), the three trace
// colours (live / peak / average), the eight radio-identity colours (the engine
// drives up to eight radios), plus the text and marker colours that have to
// clear a contrast floor (foreground, muted, accent, focus ring), over four
// grounds (body, panel, well = spectrum ground, card).
//
// The waterfall colour map is NOT here: the colour-blind-safe waterfall map is
// lane B's S7 (feat/signal-blocks-3). The band-plan colours are already the
// Okabe-Ito set (band_plan.hpp). The altitude ladders belong to the map
// (map_view.cpp) and to the map lane.
//
// THE VERIFIER (functions below; tests/test_access_palette.cpp runs it):
//
//  1. CONTRAST. WCAG 2.2 relative luminance
//       L = 0.2126 R + 0.7152 G + 0.0722 B, with each sRGB channel c (0..1)
//       linearised as c/12.92 when c <= 0.04045, else ((c + 0.055)/1.055)^2.4,
//     and contrast ratio (L1 + 0.05)/(L2 + 0.05), L1 the lighter. Both read on
//     2026-11-11 from the WCAG 2.2 glossary and Success Criterion 1.4.3
//     (https://www.w3.org/TR/WCAG22/#dfn-relative-luminance ; #contrast-minimum).
//     The page notes that before May 2021 the threshold was written 0.03928;
//     0.04045 is used. Floors: 4.5:1 for text (SC 1.4.3) and 3:1 for graphical
//     objects and UI components (SC 1.4.11); the high-contrast set is held to
//     7:1 for text and 4.5:1 for graphics, which is the enhanced level of SC
//     1.4.6 (SC 1.4.6 was not opened: treat 7:1 as this lane's own stated floor). Every colour in this
//     file is checked against ALL FOUR grounds of its set.
//
//  2. COLOUR-VISION SIMULATION. Machado, Oliveira and Fernandes, "A
//     Physiologically-based Model for Simulation of Color Vision Deficiency",
//     IEEE TVCG 15(6), 2009, severity 1.0 (dichromacy), the matrices as
//     published at https://www.inf.ufrgs.br/~oliveira/pubs_files/CVD_Simulation/
//     CVD_Simulation.html (opened 2026-11-11, Table 1: protanomaly,
//     deuteranomaly, tritanomaly; severity 1.0 "the highest severity or a case
//     of dichromacy"). The page does not say which colour space they act on; the
//     simulation is applied to LINEAR RGB (the encoding gamma removed, as for
//     WCAG luminance) and converted back: that choice is this lane's and is
//     UNVERIFIED against the paper. Each matrix row sums to 1, so white stays
//     white (the test asserts it).
//
//  3. DISTANCE. CIE 1976 Delta E (Euclidean distance in CIELAB, D65 white
//     0.95047, 1.0, 1.08883, sRGB primaries as in IEC 61966-2-1: the standard
//     sRGB matrix, textbook constants, not re-fetched). The threshold is
//     kMinDeltaE = 25, this lane's stated choice, not a standard's. The sets
//     below were searched to clear it with margin (the tests print the smallest
//     distance per group and simulation).
//
// WHAT "PASS" MEANS. A palette passes under a vision when every colour clears
// its contrast floor against every ground AND every pair inside each group
// (status, trace, radio) is at least kMinDeltaE apart AFTER simulating that
// vision. requiredVisions(id) says which visions a set is held to: Normal holds
// to normal vision only (it is the Workbench look and is NOT colour-blind-safe:
// its green/red status pair collapses under protanopia and deuteranopia; the
// test pins that fact so nobody mistakes it for a safe set), a CVD set to normal
// vision plus its own deficiency, High contrast to all four.
//
// NOT PROVEN HERE: that people with these conditions read the sets well. A
// simulation matrix is a model. No person was shown anything.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace cascade::core::access {

struct Rgb {
    std::uint8_t r = 0, g = 0, b = 0;
    constexpr bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; }
};
constexpr Rgb rgbHex(std::uint32_t v) {
    return Rgb{static_cast<std::uint8_t>((v >> 16) & 0xFF), static_cast<std::uint8_t>((v >> 8) & 0xFF),
               static_cast<std::uint8_t>(v & 0xFF)};
}
inline std::string toHex(Rgb c) {
    char b[8];
    std::snprintf(b, sizeof b, "#%02X%02X%02X", c.r, c.g, c.b);
    return b;
}

enum class PaletteId : std::uint8_t { Normal = 0, Deuteranopia, Protanopia, Tritanopia, HighContrast };
inline constexpr int kPaletteCount = 5;

enum class Vision : std::uint8_t { Normal = 0, Protanopia, Deuteranopia, Tritanopia };

enum class Role : std::uint8_t {
    Fg = 0, Muted, Accent, Focus,
    StatusOk, StatusWarn, StatusBad, StatusInfo,
    TraceLive, TracePeak, TraceAverage,
    Radio1, Radio2, Radio3, Radio4, Radio5, Radio6, Radio7, Radio8
};
inline constexpr int kRoleCount = 19;

enum class Ground : std::uint8_t { Body = 0, Panel, Well, Card };
inline constexpr int kGroundCount = 4;

inline const char* roleName(Role r) {
    static const char* const names[kRoleCount] = {
        "fg", "muted", "accent", "focus", "status-ok", "status-warn", "status-bad", "status-info",
        "trace-live", "trace-peak", "trace-average", "radio-1", "radio-2", "radio-3", "radio-4",
        "radio-5", "radio-6", "radio-7", "radio-8"};
    return names[static_cast<int>(r)];
}

// How a role is held: text (4.5 / 7), graphic (3 / 4.5).
enum class RoleKind : std::uint8_t { Text, Graphic };
inline RoleKind roleKind(Role r) {
    return (r == Role::Fg || r == Role::Muted) ? RoleKind::Text : RoleKind::Graphic;
}
// Status and trace colours are also read as text (chip foregrounds, trace
// labels), so they, the accent and the text roles are held to the TEXT floor
// against the grounds: stricter than SC 1.4.11 and satisfied by the searched
// sets. The radio-identity colours and the focus ring are graphics (a tag, a
// ring) and are held to the GRAPHIC floor. (Eight identity colours that stay
// 25 apart under all four visions AND clear 7:1 on black do not exist: a search
// of 800 candidates topped out at 22.6; at 4.5:1 it reaches 26.0. So the
// high-contrast radio colours are graphics at 4.5:1.)
inline bool heldToTextFloor(Role r) {
    return !(r == Role::Focus || (r >= Role::Radio1 && r <= Role::Radio8));
}

struct Palette {
    PaletteId id;
    const char* key;      // stored setting value
    const char* label;    // English label for the setting (list in the record; not translated here)
    std::array<Rgb, kGroundCount> ground;
    std::array<Rgb, kRoleCount> role;
    double textFloor;     // contrast ratio for text roles
    double graphicFloor;  // for graphical roles
};

// ---- the data ---------------------------------------------------------------

inline const std::array<Palette, kPaletteCount>& palettes() {
    static const std::array<Palette, kPaletteCount> p = {{
        {PaletteId::Normal, "normal", "Normal",
         {rgbHex(0x15171B), rgbHex(0x1D2026), rgbHex(0x0D0F12), rgbHex(0x20242B)},
         {rgbHex(0xE6E8EC), rgbHex(0x9AA3AD), rgbHex(0xF5A524), rgbHex(0x7AD7FF),
          rgbHex(0x3DDC84), rgbHex(0xF5A524), rgbHex(0xFF5C5C), rgbHex(0xFFD27A),
          rgbHex(0x7AD7FF), rgbHex(0xFF9F43), rgbHex(0xF1EAD6),
          rgbHex(0x4DA3FF), rgbHex(0xF5A524), rgbHex(0x3DDC84), rgbHex(0xB48CFF),
          rgbHex(0xFF7A7A), rgbHex(0x4DD0E1), rgbHex(0xC6E64D), rgbHex(0xFF8AD8)},
         4.5, 3.0},
        {PaletteId::Deuteranopia, "deuteranopia", "Colour-blind safe: green-weak (deuteranopia)",
         {rgbHex(0x15171B), rgbHex(0x1D2026), rgbHex(0x0D0F12), rgbHex(0x20242B)},
         {rgbHex(0xE6E8EC), rgbHex(0x9AA3AD), rgbHex(0xF5A524), rgbHex(0x7AD7FF),
          rgbHex(0x00B2FF), rgbHex(0xFFFF00), rgbHex(0xDF6749), rgbHex(0xFFFFFF),
          rgbHex(0x7AD7FF), rgbHex(0xFF9F43), rgbHex(0xF1EAD6),
          rgbHex(0xCF81A8), rgbHex(0x887AFF), rgbHex(0x46B95D), rgbHex(0xFF4D00),
          rgbHex(0xCCFFD1), rgbHex(0xFFFF00), rgbHex(0x7ACAFF), rgbHex(0xF8F881)},
         4.5, 3.0},
        {PaletteId::Protanopia, "protanopia", "Colour-blind safe: red-weak (protanopia)",
         {rgbHex(0x15171B), rgbHex(0x1D2026), rgbHex(0x0D0F12), rgbHex(0x20242B)},
         {rgbHex(0xE6E8EC), rgbHex(0x9AA3AD), rgbHex(0xF5A524), rgbHex(0x7AD7FF),
          rgbHex(0x0D96F2), rgbHex(0xD9B526), rgbHex(0xFF5263), rgbHex(0xFFFFFF),
          rgbHex(0x7AD7FF), rgbHex(0xFF9F43), rgbHex(0xF1EAD6),
          rgbHex(0xCFE1FC), rgbHex(0xA881CF), rgbHex(0xFFFF29), rgbHex(0xE56C84),
          rgbHex(0x6CE56C), rgbHex(0xF45A34), rgbHex(0xCCFFCC), rgbHex(0xEA29FF)},
         4.5, 3.0},
        {PaletteId::Tritanopia, "tritanopia", "Colour-blind safe: blue-weak (tritanopia)",
         {rgbHex(0x15171B), rgbHex(0x1D2026), rgbHex(0x0D0F12), rgbHex(0x20242B)},
         {rgbHex(0xE6E8EC), rgbHex(0x9AA3AD), rgbHex(0xF5A524), rgbHex(0x7AD7FF),
          rgbHex(0x46B974), rgbHex(0xFCF3CF), rgbHex(0xFF5429), rgbHex(0xDC52FF),
          rgbHex(0x7AD7FF), rgbHex(0xFF9F43), rgbHex(0xF1EAD6),
          rgbHex(0xFF4D00), rgbHex(0xFFE500), rgbHex(0xCFF8FC), rgbHex(0x00FF4D),
          rgbHex(0xFF00FF), rgbHex(0x4696B9), rgbHex(0xC48164), rgbHex(0x9981CF)},
         4.5, 3.0},
        {PaletteId::HighContrast, "high-contrast", "High contrast",
         {rgbHex(0x000000), rgbHex(0x000000), rgbHex(0x000000), rgbHex(0x000000)},
         {rgbHex(0xFFFFFF), rgbHex(0xE0E0E0), rgbHex(0xFFD400), rgbHex(0x00FFFF),
          rgbHex(0x00B2FF), rgbHex(0xCCBD33), rgbHex(0xE085A0), rgbHex(0xFFFFFF),
          rgbHex(0x7AD7FF), rgbHex(0xFF9F43), rgbHex(0xF1EAD6),
          rgbHex(0x8C96D9), rgbHex(0x00FF66), rgbHex(0xFF8800), rgbHex(0xB866FF),
          rgbHex(0xDF2086), rgbHex(0xA6E3F2), rgbHex(0xFF0044), rgbHex(0xD9A18C)},
         7.0, 4.5},
    }};
    return p;
}

inline const Palette& paletteFor(PaletteId id) {
    const int i = static_cast<int>(id);
    return palettes()[static_cast<std::size_t>((i >= 0 && i < kPaletteCount) ? i : 0)];
}
// Stored name -> id; anything unknown is Normal.
inline PaletteId paletteIdFromKey(const std::string& key) {
    for (const Palette& p : palettes()) { if (key == p.key) { return p.id; } }
    return PaletteId::Normal;
}

// ---- groups that must stay distinguishable ----------------------------------------

inline constexpr double kMinDeltaE = 25.0;   // CIE76, this lane's stated choice

struct Group {
    const char* name;
    std::vector<Role> members;
};
inline const std::vector<Group>& distinctGroups() {
    static const std::vector<Group> g = {
        {"status", {Role::StatusOk, Role::StatusWarn, Role::StatusBad, Role::StatusInfo}},
        {"trace", {Role::TraceLive, Role::TracePeak, Role::TraceAverage}},
        {"radio", {Role::Radio1, Role::Radio2, Role::Radio3, Role::Radio4, Role::Radio5,
                   Role::Radio6, Role::Radio7, Role::Radio8}},
    };
    return g;
}

// ---- the arithmetic ------------------------------------------------------------

inline double srgbToLinear(double c8) {
    const double c = c8 / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
inline double linearToSrgb8(double l) {
    l = l < 0.0 ? 0.0 : (l > 1.0 ? 1.0 : l);
    const double v = l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
    return v * 255.0;
}

inline double relativeLuminance(Rgb c) {
    return 0.2126 * srgbToLinear(c.r) + 0.7152 * srgbToLinear(c.g) + 0.0722 * srgbToLinear(c.b);
}
inline double contrastRatio(Rgb a, Rgb b) {
    const double la = relativeLuminance(a), lb = relativeLuminance(b);
    const double hi = la > lb ? la : lb, lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

// Machado, Oliveira, Fernandes 2009, Table 1, severity 1.0, row-major.
inline const double (&cvdMatrix(Vision v))[9] {
    static const double prot[9] = {0.152286, 1.052583, -0.204868, 0.114503, 0.786281, 0.099216,
                                   -0.003882, -0.048116, 1.051998};
    static const double deut[9] = {0.367322, 0.860646, -0.227968, 0.280085, 0.672501, 0.047413,
                                   -0.011820, 0.042940, 0.968881};
    static const double trit[9] = {1.255528, -0.076749, -0.178779, -0.078411, 0.930809, 0.147602,
                                   0.004733, 0.691367, 0.303900};
    static const double ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    switch (v) {
        case Vision::Protanopia: return prot;
        case Vision::Deuteranopia: return deut;
        case Vision::Tritanopia: return trit;
        case Vision::Normal: break;
    }
    return ident;
}

// The colour as the given vision sees it, as 8-bit-scale doubles (not rounded:
// distances are measured on the unrounded values).
struct RgbD { double r, g, b; };
inline RgbD simulate(Rgb c, Vision v) {
    if (v == Vision::Normal) { return {static_cast<double>(c.r), static_cast<double>(c.g), static_cast<double>(c.b)}; }
    const double lr = srgbToLinear(c.r), lg = srgbToLinear(c.g), lb = srgbToLinear(c.b);
    const double (&m)[9] = cvdMatrix(v);
    return {linearToSrgb8(m[0] * lr + m[1] * lg + m[2] * lb), linearToSrgb8(m[3] * lr + m[4] * lg + m[5] * lb),
            linearToSrgb8(m[6] * lr + m[7] * lg + m[8] * lb)};
}

struct Lab { double l, a, b; };
inline Lab toLab(const RgbD& c) {
    const double r = srgbToLinear(c.r), g = srgbToLinear(c.g), b = srgbToLinear(c.b);
    const double x = 0.4124564 * r + 0.3575761 * g + 0.1804375 * b;
    const double y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b;
    const double z = 0.0193339 * r + 0.1191920 * g + 0.9503041 * b;
    const auto f = [](double t) { return t > 216.0 / 24389.0 ? std::cbrt(t) : (24389.0 / 27.0 * t + 16.0) / 116.0; };
    const double fx = f(x / 0.95047), fy = f(y / 1.0), fz = f(z / 1.08883);
    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}
inline double deltaE76(const Lab& a, const Lab& b) {
    const double dl = a.l - b.l, da = a.a - b.a, db = a.b - b.b;
    return std::sqrt(dl * dl + da * da + db * db);
}
// Distance between two colours as `v` sees them.
inline double distanceUnder(Rgb a, Rgb b, Vision v) { return deltaE76(toLab(simulate(a, v)), toLab(simulate(b, v))); }

// ---- the verifier --------------------------------------------------------------

struct Failure {
    std::string what;    // e.g. "contrast radio-3 on well 2.9 < 4.5" or "status-ok vs status-bad under deuteranopia 9.1 < 25"
};

inline std::vector<Vision> requiredVisions(PaletteId id) {
    switch (id) {
        case PaletteId::Normal: return {Vision::Normal};
        case PaletteId::Deuteranopia: return {Vision::Normal, Vision::Deuteranopia};
        case PaletteId::Protanopia: return {Vision::Normal, Vision::Protanopia};
        case PaletteId::Tritanopia: return {Vision::Normal, Vision::Tritanopia};
        case PaletteId::HighContrast: return {Vision::Normal, Vision::Protanopia, Vision::Deuteranopia, Vision::Tritanopia};
    }
    return {Vision::Normal};
}

inline const char* visionName(Vision v) {
    switch (v) {
        case Vision::Protanopia: return "protanopia";
        case Vision::Deuteranopia: return "deuteranopia";
        case Vision::Tritanopia: return "tritanopia";
        case Vision::Normal: break;
    }
    return "normal vision";
}

inline const char* groundName(Ground g) {
    static const char* const n[kGroundCount] = {"body", "panel", "well", "card"};
    return n[static_cast<int>(g)];
}

// Contrast of every role against every ground of the palette. (Vision does not
// enter: WCAG contrast is defined on the colours as specified.)
inline std::vector<Failure> verifyContrast(const Palette& p) {
    std::vector<Failure> out;
    for (int r = 0; r < kRoleCount; ++r) {
        const Role role = static_cast<Role>(r);
        const double floor = heldToTextFloor(role) ? p.textFloor : p.graphicFloor;
        for (int g = 0; g < kGroundCount; ++g) {
            const double cr = contrastRatio(p.role[static_cast<std::size_t>(r)], p.ground[static_cast<std::size_t>(g)]);
            if (cr < floor) {
                char b[160];
                std::snprintf(b, sizeof b, "contrast %s on %s %.2f < %.1f", roleName(role),
                              groundName(static_cast<Ground>(g)), cr, floor);
                out.push_back({b});
            }
        }
    }
    return out;
}

// Every pair inside each group under one vision.
inline std::vector<Failure> verifyDistinct(const Palette& p, Vision v, double minDeltaE = kMinDeltaE) {
    std::vector<Failure> out;
    for (const Group& grp : distinctGroups()) {
        for (std::size_t i = 0; i < grp.members.size(); ++i) {
            for (std::size_t j = i + 1; j < grp.members.size(); ++j) {
                const double d = distanceUnder(p.role[static_cast<std::size_t>(grp.members[i])],
                                               p.role[static_cast<std::size_t>(grp.members[j])], v);
                if (d < minDeltaE) {
                    char b[200];
                    std::snprintf(b, sizeof b, "%s vs %s under %s %.1f < %.0f", roleName(grp.members[i]),
                                  roleName(grp.members[j]), visionName(v), d, minDeltaE);
                    out.push_back({b});
                }
            }
        }
    }
    return out;
}

// The smallest distance inside any group under one vision (for reports).
inline double smallestDistance(const Palette& p, Vision v) {
    double m = 1e9;
    for (const Group& grp : distinctGroups()) {
        for (std::size_t i = 0; i < grp.members.size(); ++i) {
            for (std::size_t j = i + 1; j < grp.members.size(); ++j) {
                const double d = distanceUnder(p.role[static_cast<std::size_t>(grp.members[i])],
                                               p.role[static_cast<std::size_t>(grp.members[j])], v);
                if (d < m) { m = d; }
            }
        }
    }
    return m;
}

// Everything a palette is held to.
inline std::vector<Failure> verifyPalette(const Palette& p) {
    std::vector<Failure> out = verifyContrast(p);
    for (Vision v : requiredVisions(p.id)) {
        const std::vector<Failure> f = verifyDistinct(p, v);
        out.insert(out.end(), f.begin(), f.end());
    }
    return out;
}

}  // namespace cascade::core::access
