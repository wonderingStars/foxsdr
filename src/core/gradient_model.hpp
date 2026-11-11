// gradient_model.hpp - the USER-EDITABLE WATERFALL GRADIENT as a MODEL (marker editor): colour stops, interpolation, validation, a 256-entry table and
// a one-line text form for the config. No window, no clock. The marker editor itself (the strip with draggable markers) is screen work.
//
// HOW THE FORMATS JOIN (read from feat/screen-b: src/core/colour_maps.hpp and docs/multi-radio-records/screen-b.md S7):
//   The waterfall compositor paints the newest line through a colour::Table = std::array<colour::Rgb8, 256> (core/colour_maps.hpp,
//   colourAt(table, x01)), and the strength key under the waterfall is "a 256 x 1 picture" of the same table. toTable() below returns EXACTLY that
//   type, so a user gradient is one more Table for the compositor: no new format on its side. colour_maps.hpp is included UNCHANGED (identical blob
//   to feat/screen-b); the stock maps are sampled into editable stops by fromMap(). The compositor-side choice "custom" and the config field that
//   holds serialise()'s string are the screen lane's hand-off (see the record).
//
// THE MODEL.
//   Stop = position 0..1 (quantised to 1/10000) + an 8-bit colour. A gradient has 2..kMaxStops stops, sorted by position with a gap of at least
//   kMinGap (1/1000) between neighbours, the first stop at 0 and the last at 1 (the ends keep their positions; only their colour changes; they are
//   never removed). Interpolation between stops: Srgb (straight on the encoded 8-bit values), LinearLight (decode, mix, encode: what light does), or
//   Oklab (mix in the perceptual space of core/colour_maps.hpp, the default; a colour that leaves the sRGB gamut is clamped).
//   validate() names the first thing wrong; the editor never produces an invalid gradient, a parsed one is checked the same way.
//
// TEXT FORM (one line, ASCII, at most kMaxTextLen characters, locale-free):  FOXGRAD1|<interp>|<name>|<pos>@<rrggbb>,<pos>@<rrggbb>,...
//   interp = srgb | linear | oklab;  name = up to 32 of [A-Za-z0-9 _-], may be empty;  pos = an integer 0..10000 (ten-thousandths);
//   rrggbb = six lower-case hex digits. parse() is strict: anything else is refused with the reason and nothing is half-built, and
//   serialise(parse(s)) == s for every accepted s that is already in this canonical spelling.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/colour_maps.hpp"

namespace cascade::core::gradient {

using colour::Rgb8;
using colour::Table;

inline constexpr std::size_t kMaxStops = 32;
inline constexpr double kMinGap = 0.001;
inline constexpr int kPosScale = 10000;
inline constexpr std::size_t kMaxNameLen = 32;
inline constexpr std::size_t kMaxTextLen = 4096;

enum class Interp : int { Srgb = 0, LinearLight = 1, Oklab = 2 };

inline const char* interpName(Interp i) {
    switch (i) {
        case Interp::Srgb: return "srgb";
        case Interp::LinearLight: return "linear";
        default: return "oklab";
    }
}

struct Stop {
    double pos = 0.0;
    Rgb8 colour{};
    bool operator==(const Stop&) const = default;
};

struct Gradient {
    std::vector<Stop> stops;
    Interp interp = Interp::Oklab;
    std::string name;
    bool operator==(const Gradient&) const = default;
};

enum class Problem : int {
    None = 0,
    TooFewStops,
    TooManyStops,
    PositionOutOfRange,
    PositionsNotIncreasing,
    GapTooSmall,
    FirstNotAtZero,
    LastNotAtOne,
    BadName,
    NotFinite,
};

inline const char* problemText(Problem p) {
    switch (p) {
        case Problem::None: return "";
        case Problem::TooFewStops: return "A gradient needs at least two colour stops.";
        case Problem::TooManyStops: return "A gradient can have at most 32 colour stops.";
        case Problem::PositionOutOfRange: return "A colour stop must sit between 0 and 1.";
        case Problem::PositionsNotIncreasing: return "The colour stops must be in order.";
        case Problem::GapTooSmall: return "Two colour stops are too close together.";
        case Problem::FirstNotAtZero: return "The first colour stop must be at the bottom of the scale.";
        case Problem::LastNotAtOne: return "The last colour stop must be at the top of the scale.";
        case Problem::BadName: return "The name can use letters, digits, space, - and _ (32 at most).";
        case Problem::NotFinite: return "A colour stop position is not a number.";
    }
    return "";
}

inline bool nameOk(std::string_view n) {
    if (n.size() > kMaxNameLen) { return false; }
    for (char c : n) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-';
        if (!ok) { return false; }
    }
    return true;
}

inline double quantise(double pos) { return std::round(std::clamp(pos, 0.0, 1.0) * kPosScale) / kPosScale; }

inline Problem validate(const Gradient& g) {
    if (!nameOk(g.name)) { return Problem::BadName; }
    if (g.stops.size() < 2) { return Problem::TooFewStops; }
    if (g.stops.size() > kMaxStops) { return Problem::TooManyStops; }
    for (const Stop& s : g.stops) {
        if (!std::isfinite(s.pos)) { return Problem::NotFinite; }
        if (s.pos < 0.0 || s.pos > 1.0) { return Problem::PositionOutOfRange; }
    }
    if (g.stops.front().pos != 0.0) { return Problem::FirstNotAtZero; }
    if (g.stops.back().pos != 1.0) { return Problem::LastNotAtOne; }
    for (std::size_t i = 1; i < g.stops.size(); ++i) {
        const double d = g.stops[i].pos - g.stops[i - 1].pos;
        if (d <= 0.0) { return Problem::PositionsNotIncreasing; }
        if (d < kMinGap - 1e-12) { return Problem::GapTooSmall; }
    }
    return Problem::None;
}

// The colour of the gradient at x in [0, 1] (NaN and below 0 read as 0, above 1 as 1). The gradient must be valid.
inline Rgb8 colourAtPosition(const Gradient& g, double x) {
    if (g.stops.empty()) { return {}; }
    if (!(x > 0.0)) { x = 0.0; }
    if (x > 1.0) { x = 1.0; }
    std::size_t i = 0;
    while (i + 2 < g.stops.size() && x > g.stops[i + 1].pos) { ++i; }
    if (g.stops.size() == 1) { return g.stops[0].colour; }
    const Stop& a = g.stops[i];
    const Stop& b = g.stops[i + 1];
    const double span = b.pos - a.pos;
    const double u = span > 0.0 ? std::clamp((x - a.pos) / span, 0.0, 1.0) : 0.0;
    auto enc8 = [](double v) { return static_cast<std::uint8_t>(std::clamp(std::floor(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5), 0.0, 255.0)); };
    switch (g.interp) {
        case Interp::Srgb: {
            auto mix = [&](std::uint8_t p, std::uint8_t q) { return static_cast<std::uint8_t>(std::floor(p + u * (static_cast<double>(q) - p) + 0.5)); };
            return {mix(a.colour.r, b.colour.r), mix(a.colour.g, b.colour.g), mix(a.colour.b, b.colour.b)};
        }
        case Interp::LinearLight: {
            auto mix = [&](std::uint8_t p, std::uint8_t q) {
                const double lp = colour::srgbDecode(p / 255.0), lq = colour::srgbDecode(q / 255.0);
                return enc8(colour::srgbEncode(lp + u * (lq - lp)));
            };
            return {mix(a.colour.r, b.colour.r), mix(a.colour.g, b.colour.g), mix(a.colour.b, b.colour.b)};
        }
        default: {
            const colour::Lab la = colour::rgb8ToOklab(a.colour), lb = colour::rgb8ToOklab(b.colour);
            const colour::Lab m{la.L + u * (lb.L - la.L), la.a + u * (lb.a - la.a), la.b + u * (lb.b - la.b)};
            const colour::Lin lin = colour::oklabToLinear(m);
            return {enc8(colour::srgbEncode(std::clamp(lin.r, 0.0, 1.0))), enc8(colour::srgbEncode(std::clamp(lin.g, 0.0, 1.0))),
                    enc8(colour::srgbEncode(std::clamp(lin.b, 0.0, 1.0)))};
        }
    }
}

// The 256-entry table the compositor reads: entry i is the gradient at i / 255. An invalid gradient gives the grey ramp (never garbage).
inline Table toTable(const Gradient& g) {
    Table t;
    if (validate(g) != Problem::None) {
        for (std::size_t i = 0; i < colour::kEntries; ++i) { t[i] = {static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i)}; }
        return t;
    }
    for (std::size_t i = 0; i < colour::kEntries; ++i) { t[i] = colourAtPosition(g, static_cast<double>(i) / 255.0); }
    return t;
}

// True when the lightness (OKLab L) never falls from entry to entry by more than `tolerance`: a map that is not monotone draws two levels in one colour.
inline bool lightnessMonotone(const Table& t, double tolerance = 0.002) {
    double prev = -1.0;
    for (const Rgb8& c : t) {
        const double L = colour::rgb8ToOklab(c).L;
        if (L < prev - tolerance) { return false; }
        prev = std::max(prev, L);
    }
    return true;
}

inline Gradient defaultGradient() {
    Gradient g;
    g.name = "Custom";
    g.stops = {{0.0, {0, 0, 0}}, {1.0, {255, 255, 255}}};
    return g;
}

// The stock map `m` sampled at `count` equally spaced stops (2..kMaxStops), as a starting point for editing.
inline Gradient fromMap(colour::MapId m, std::size_t count) {
    count = std::min(std::max<std::size_t>(count, 2), kMaxStops);
    const Table& t = colour::colourMap(m);
    Gradient g;
    g.interp = Interp::Oklab;
    g.name = colour::mapName(m);
    for (char& c : g.name) { if (!(std::isalnum(static_cast<unsigned char>(c)) || c == ' ')) { c = ' '; } }
    for (std::size_t i = 0; i < count; ++i) {
        const double p = (count == 1) ? 0.0 : static_cast<double>(i) / static_cast<double>(count - 1);
        Stop s;
        s.pos = quantise(p);
        s.colour = t[static_cast<std::size_t>(std::lround(p * 255.0))];
        g.stops.push_back(s);
    }
    return g;
}

// ---------------------------------------------------------------- editing

class GradientEditor {
public:
    GradientEditor() : g_(defaultGradient()) {}
    explicit GradientEditor(Gradient g) : g_(validate(g) == Problem::None ? std::move(g) : defaultGradient()) {}

    const Gradient& gradient() const { return g_; }
    std::size_t size() const { return g_.stops.size(); }

    // Adds a stop at `pos` with `c`; returns its index, or -1 when refused (full, not a number, or within kMinGap of a stop that is there).
    int addStop(double pos, Rgb8 c) {
        if (!std::isfinite(pos) || g_.stops.size() >= kMaxStops) { return -1; }
        pos = quantise(pos);
        if (pos <= 0.0 || pos >= 1.0) { return -1; }  // the ends exist already
        std::size_t at = 0;
        while (at < g_.stops.size() && g_.stops[at].pos < pos) { ++at; }
        if (at < g_.stops.size() && g_.stops[at].pos - pos < kMinGap - 1e-12) { return -1; }
        if (at > 0 && pos - g_.stops[at - 1].pos < kMinGap - 1e-12) { return -1; }
        g_.stops.insert(g_.stops.begin() + static_cast<std::ptrdiff_t>(at), Stop{pos, c});
        return static_cast<int>(at);
    }
    // Interior stops only (the ends and the last two stops that would leave fewer than two are refused).
    bool removeStop(std::size_t i) {
        if (i == 0 || i + 1 >= g_.stops.size()) { return false; }
        g_.stops.erase(g_.stops.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    // Moves an interior stop; the position is held between its neighbours (kMinGap clear of each). Returns the position it took, or -1 if refused.
    double moveStop(std::size_t i, double pos) {
        if (i == 0 || i + 1 >= g_.stops.size() || !std::isfinite(pos)) { return -1.0; }
        const double lo = g_.stops[i - 1].pos + kMinGap, hi = g_.stops[i + 1].pos - kMinGap;
        if (lo > hi) { return -1.0; }
        const double p = std::clamp(quantise(pos), std::ceil(lo * kPosScale - 1e-9) / kPosScale, std::floor(hi * kPosScale + 1e-9) / kPosScale);
        g_.stops[i].pos = p;
        return p;
    }
    bool setColour(std::size_t i, Rgb8 c) {
        if (i >= g_.stops.size()) { return false; }
        g_.stops[i].colour = c;
        return true;
    }
    void setInterp(Interp i) { g_.interp = i; }
    bool setName(const std::string& n) {
        if (!nameOk(n)) { return false; }
        g_.name = n;
        return true;
    }
    // Turns the scale upside down (stop positions mirrored, colours kept).
    void reverse() {
        std::reverse(g_.stops.begin(), g_.stops.end());
        for (Stop& s : g_.stops) { s.pos = quantise(1.0 - s.pos); }
    }
    void reset() { g_ = defaultGradient(); }
    // Index of the stop nearest to `pos` within `reach` (for picking a marker with the pointer), or -1.
    int pick(double pos, double reach) const {
        int best = -1;
        double bd = reach;
        for (std::size_t i = 0; i < g_.stops.size(); ++i) {
            const double d = std::fabs(g_.stops[i].pos - pos);
            if (d <= bd) { bd = d; best = static_cast<int>(i); }
        }
        return best;
    }

private:
    Gradient g_;
};

// -------------------------------------------------------------- text form

inline std::string serialise(const Gradient& g) {
    static const char* hex = "0123456789abcdef";
    std::string s = "FOXGRAD1|";
    s += interpName(g.interp);
    s += '|';
    s += g.name;
    s += '|';
    for (std::size_t i = 0; i < g.stops.size(); ++i) {
        if (i > 0) { s += ','; }
        s += std::to_string(static_cast<int>(std::lround(g.stops[i].pos * kPosScale)));
        s += '@';
        for (std::uint8_t v : {g.stops[i].colour.r, g.stops[i].colour.g, g.stops[i].colour.b}) {
            s += hex[v >> 4];
            s += hex[v & 15];
        }
    }
    return s;
}

struct ParseResult {
    bool ok = false;
    Problem problem = Problem::None;  // the model's reason when the text was well formed but the gradient is not valid
    const char* syntax = "";          // a short reason when the text itself is malformed
    Gradient gradient;
};

inline ParseResult parse(std::string_view text) {
    ParseResult r;
    auto fail = [&](const char* why) {
        r.ok = false;
        r.syntax = why;
        r.gradient = Gradient{};
        return r;
    };
    if (text.size() > kMaxTextLen) { return fail("too long"); }
    constexpr std::string_view kHead = "FOXGRAD1|";
    if (text.substr(0, kHead.size()) != kHead) { return fail("not a gradient"); }
    text.remove_prefix(kHead.size());
    const std::size_t p1 = text.find('|');
    if (p1 == std::string_view::npos) { return fail("no name field"); }
    const std::string_view interp = text.substr(0, p1);
    text.remove_prefix(p1 + 1);
    const std::size_t p2 = text.find('|');
    if (p2 == std::string_view::npos) { return fail("no stops field"); }
    const std::string_view name = text.substr(0, p2);
    text.remove_prefix(p2 + 1);
    Gradient g;
    if (interp == "srgb") { g.interp = Interp::Srgb; }
    else if (interp == "linear") { g.interp = Interp::LinearLight; }
    else if (interp == "oklab") { g.interp = Interp::Oklab; }
    else { return fail("unknown interpolation"); }
    if (!nameOk(name)) { r.ok = false; r.problem = Problem::BadName; return r; }
    g.name = std::string(name);
    auto hexv = [](char c) -> int {
        if (c >= '0' && c <= '9') { return c - '0'; }
        if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
        return -1;
    };
    while (true) {
        const std::size_t comma = text.find(',');
        const std::string_view item = text.substr(0, comma);
        const std::size_t at = item.find('@');
        if (at == std::string_view::npos || at == 0 || at > 5) { return fail("bad stop"); }
        int pos = 0;
        const auto fc = std::from_chars(item.data(), item.data() + at, pos);
        if (fc.ec != std::errc() || fc.ptr != item.data() + at || pos < 0 || pos > kPosScale) { return fail("bad position"); }
        if (item.size() != at + 7) { return fail("bad colour"); }
        std::uint8_t ch[3];
        for (int k = 0; k < 3; ++k) {
            const int h = hexv(item[at + 1 + 2 * static_cast<std::size_t>(k)]), l = hexv(item[at + 2 + 2 * static_cast<std::size_t>(k)]);
            if (h < 0 || l < 0) { return fail("bad colour"); }
            ch[k] = static_cast<std::uint8_t>(h * 16 + l);
        }
        if (g.stops.size() >= kMaxStops) { r.ok = false; r.problem = Problem::TooManyStops; return r; }
        g.stops.push_back({static_cast<double>(pos) / kPosScale, {ch[0], ch[1], ch[2]}});
        if (comma == std::string_view::npos) { break; }
        text.remove_prefix(comma + 1);
    }
    const Problem p = validate(g);
    if (p != Problem::None) { r.ok = false; r.problem = p; return r; }
    r.ok = true;
    r.gradient = std::move(g);
    return r;
}

}  // namespace cascade::core::gradient
