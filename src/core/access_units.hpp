// access_units.hpp - the unit-system model: metric, imperial, aviation and
// nautical, with the formatters' arithmetic for altitude, distance, speed,
// temperature and pressure. Pure; no window, no locale, no global state.
//
// WHY IT EXISTS. Units were fixed per object type in the code (the track detail
// block: an aircraft's altitude in feet and its speed in knots, a satellite's
// in kilometres and km/s, every range in kilometres; src/gui/track_metrics.hpp
// lines 1245-1280 and 1376-1380; the altitude ladder in src/gui/map_view.cpp).
// A user in the United States who wants miles, or a sailor who wants nautical
// miles everywhere, had no way to say so.
//
// THE EXISTING BEHAVIOUR IS A CHOICE, NOT A MISSING ONE. UnitSystem::Automatic
// is the default and returns, object by object, exactly the units the code has
// always printed (see unitFor). Only an explicit system overrides them, so a
// build that wires this in and leaves the setting alone changes nothing on any
// screen. tests/test_access_units.cpp pins that table, and also scans
// src/gui/track_metrics.hpp for the literal format strings it was copied from,
// so a later edit to either side turns the test red.
//
// EXACTNESS. Conversion factors are the NIST SP 811 Appendix B.8 values
// (opened 2026-11-11): foot 0.3048 m, statute mile 1609.344 m and nautical mile
// 1852 m are exact; the knot is one nautical mile per hour, so 1852/3600 m/s
// (NIST prints it rounded as 5.144 444 E-01 and marks it not exact, because it
// is the quotient); mile per hour 0.44704 m/s exact; inch of mercury
// (conventional) 3386.389 Pa; millibar = hectopascal = 100 Pa exact;
// Fahrenheit = (Celsius x 1.8) + 32 exact. Base units: metre, metre per second,
// kelvin, pascal.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>

namespace cascade::core::access {

enum class UnitSystem : std::uint8_t {
    Automatic = 0,  // the per-object units the application has always used
    Metric,         // m, km, km/h, degC, hPa
    Imperial,       // ft, mi, mph, degF, inHg
    Aviation,       // ft, nmi, kt, degC, hPa
    Nautical        // m, nmi, kt, degC, hPa
};
inline constexpr int kUnitSystemCount = 5;

enum class Quantity : std::uint8_t { Altitude = 0, Distance, Speed, Temperature, Pressure };
inline constexpr int kQuantityCount = 5;

// What a track IS decides how its altitude and speed are read: the same rule as
// map_view.cpp orbitalLadder() and detailTrackKind(): only a satellite is
// orbital; a station and anything unnamed keep the aviation units.
enum class ObjectKind : std::uint8_t { Aircraft = 0, Vessel, Satellite, Other };

enum class Unit : std::uint8_t {
    Metre = 0, Kilometre, Foot, StatuteMile, NauticalMile,      // length
    MetrePerSecond, KilometrePerHour, MilePerHour, Knot,        // speed
    KilometrePerSecond, MilePerSecond, NauticalMilePerSecond,   // orbital speed
    Celsius, Fahrenheit, Kelvin,                                // temperature
    Hectopascal, InchOfMercury, Kilopascal                      // pressure
};
inline constexpr int kUnitCount = 18;

namespace detail {

inline constexpr double kFootM = 0.3048;
inline constexpr double kMileM = 1609.344;
inline constexpr double kNauticalMileM = 1852.0;
inline constexpr double kInHgPa = 3386.389;

struct UnitRow {
    Unit unit;
    Quantity quantity;
    const char* symbol;  // not translated: a unit symbol, as printed on an instrument
    double perUnit;      // size of one unit in base units (not used for temperature)
    int decimals;        // default digits after the point
};

// The default decimals reproduce what the code prints today: "%.0f ft",
// "%.0f kt", "%.0f km", "%.2f km/s". The rest are stated choices.
inline constexpr UnitRow kRows[kUnitCount] = {
    {Unit::Metre, Quantity::Altitude, "m", 1.0, 0},
    {Unit::Kilometre, Quantity::Distance, "km", 1000.0, 0},
    {Unit::Foot, Quantity::Altitude, "ft", kFootM, 0},
    {Unit::StatuteMile, Quantity::Distance, "mi", kMileM, 0},
    {Unit::NauticalMile, Quantity::Distance, "nmi", kNauticalMileM, 0},
    {Unit::MetrePerSecond, Quantity::Speed, "m/s", 1.0, 0},
    {Unit::KilometrePerHour, Quantity::Speed, "km/h", 1000.0 / 3600.0, 0},
    {Unit::MilePerHour, Quantity::Speed, "mph", kMileM / 3600.0, 0},
    {Unit::Knot, Quantity::Speed, "kt", kNauticalMileM / 3600.0, 0},
    {Unit::KilometrePerSecond, Quantity::Speed, "km/s", 1000.0, 2},
    {Unit::MilePerSecond, Quantity::Speed, "mi/s", kMileM, 2},
    {Unit::NauticalMilePerSecond, Quantity::Speed, "nmi/s", kNauticalMileM, 2},
    {Unit::Celsius, Quantity::Temperature, "\xC2\xB0" "C", 1.0, 1},
    {Unit::Fahrenheit, Quantity::Temperature, "\xC2\xB0" "F", 1.0, 1},
    {Unit::Kelvin, Quantity::Temperature, "K", 1.0, 1},
    {Unit::Hectopascal, Quantity::Pressure, "hPa", 100.0, 0},
    {Unit::InchOfMercury, Quantity::Pressure, "inHg", kInHgPa, 2},
    {Unit::Kilopascal, Quantity::Pressure, "kPa", 1000.0, 1},
};

inline const UnitRow& row(Unit u) {
    const int i = static_cast<int>(u);
    return kRows[(i >= 0 && i < kUnitCount) ? i : 0];
}

}  // namespace detail

inline const char* unitSymbol(Unit u) { return detail::row(u).symbol; }
inline Quantity quantityOf(Unit u) { return detail::row(u).quantity; }
inline int defaultDecimals(Unit u) { return detail::row(u).decimals; }

// A length is both an altitude and a distance: Metre and Foot are listed as
// altitudes, Kilometre and the miles as distances, yet each can serve either
// (a metre-lettered range, a kilometre-lettered satellite altitude). So the
// "quantity" above is the unit's family, and compatible() is what convert uses.
inline bool compatible(Unit a, Unit b) {
    const auto fam = [](Unit u) {
        switch (detail::row(u).quantity) {
            case Quantity::Altitude:
            case Quantity::Distance: return 0;
            case Quantity::Speed: return 1;
            case Quantity::Temperature: return 2;
            case Quantity::Pressure: return 3;
        }
        return -1;
    };
    return fam(a) == fam(b);
}

// To and from the base unit (metre, m/s, kelvin, pascal). NaN stays NaN.
inline double toBase(Unit u, double v) {
    switch (u) {
        case Unit::Celsius: return v + 273.15;
        case Unit::Fahrenheit: return (v + 459.67) / 1.8;
        case Unit::Kelvin: return v;
        default: return v * detail::row(u).perUnit;
    }
}
inline double fromBase(Unit u, double base) {
    switch (u) {
        case Unit::Celsius: return base - 273.15;
        case Unit::Fahrenheit: return base * 1.8 - 459.67;
        case Unit::Kelvin: return base;
        default: return base / detail::row(u).perUnit;
    }
}

// NaN when the two units are not the same kind of thing (a foot into a knot).
inline double convert(double v, Unit from, Unit to) {
    if (!compatible(from, to)) { return std::numeric_limits<double>::quiet_NaN(); }
    return fromBase(to, toBase(from, v));
}

// THE UNITS A SYSTEM USES FOR A QUANTITY ON AN OBJECT.
//
// Automatic: Altitude -> km for a satellite, ft otherwise; Speed -> km/s for a
//   satellite, kt otherwise; Distance -> km for everything (greatCircleKm and
//   the range lines are kilometres); Temperature -> degC; Pressure -> hPa. The
//   last two have no display today; they are the units every receiver
//   weather source in the plugin set reports in.
// An explicit system: Metric m / km / km/h / degC / hPa; Imperial ft / mi / mph
//   / degF / inHg; Aviation ft / nmi / kt / degC / hPa; Nautical m / nmi / kt /
//   degC / hPa. A SATELLITE under an explicit system reads altitude in that
//   system's distance unit and speed in that unit per second (km/s, mi/s,
//   nmi/s): feet and knots are not how anything in orbit is read, the mistake
//   the orbital ladder was made to end. (Aviation pressure is hPa here, the
//   ICAO form; a US-aviation user picks Imperial or a per-quantity override.)
inline Unit unitFor(UnitSystem sys, ObjectKind kind, Quantity q) {
    const bool orbital = (kind == ObjectKind::Satellite);
    if (sys == UnitSystem::Automatic) {
        switch (q) {
            case Quantity::Altitude: return orbital ? Unit::Kilometre : Unit::Foot;
            case Quantity::Distance: return Unit::Kilometre;
            case Quantity::Speed: return orbital ? Unit::KilometrePerSecond : Unit::Knot;
            case Quantity::Temperature: return Unit::Celsius;
            case Quantity::Pressure: return Unit::Hectopascal;
        }
    }
    Unit dist = Unit::Kilometre, alt = Unit::Metre, speed = Unit::KilometrePerHour,
         orbSpeed = Unit::KilometrePerSecond, temp = Unit::Celsius, pres = Unit::Hectopascal;
    switch (sys) {
        case UnitSystem::Imperial:
            dist = Unit::StatuteMile; alt = Unit::Foot; speed = Unit::MilePerHour;
            orbSpeed = Unit::MilePerSecond; temp = Unit::Fahrenheit; pres = Unit::InchOfMercury;
            break;
        case UnitSystem::Aviation:
            dist = Unit::NauticalMile; alt = Unit::Foot; speed = Unit::Knot;
            orbSpeed = Unit::NauticalMilePerSecond;
            break;
        case UnitSystem::Nautical:
            dist = Unit::NauticalMile; alt = Unit::Metre; speed = Unit::Knot;
            orbSpeed = Unit::NauticalMilePerSecond;
            break;
        case UnitSystem::Metric:
        case UnitSystem::Automatic: break;
    }
    switch (q) {
        case Quantity::Altitude: return orbital ? dist : alt;
        case Quantity::Distance: return dist;
        case Quantity::Speed: return orbital ? orbSpeed : speed;
        case Quantity::Temperature: return temp;
        case Quantity::Pressure: return pres;
    }
    return Unit::Metre;
}

// A value ready to be lettered: the number in the chosen unit, its symbol and
// the digits to show. `known` is false for NaN / infinity (an unknown altitude
// is NaN by the ABI), so a caller prints its own "unknown" and never "nan ft".
struct Quantified {
    bool known = false;
    double value = 0.0;
    Unit unit = Unit::Metre;
    int decimals = 0;
};

// `baseValue` is in the base unit of the quantity: metres, m/s, kelvin, pascal.
inline Quantified quantify(UnitSystem sys, ObjectKind kind, Quantity q, double baseValue) {
    Quantified out;
    out.unit = unitFor(sys, kind, q);
    out.decimals = defaultDecimals(out.unit);
    if (!std::isfinite(baseValue)) { return out; }
    out.value = fromBase(out.unit, baseValue);
    out.known = true;
    return out;
}

// "421 km", "34000 ft", "370 kt": number, one space, symbol, exactly the shape
// of the strings in track_metrics.hpp. The number is formatted by `number`
// (digits after the point -> text) so a locale-aware formatter can be handed in
// (access_number_format.hpp); the default is the C-locale "%.*f" the existing
// strings use. Unknown -> empty string.
using NumberText = std::function<std::string(double value, int decimals)>;

inline std::string plainNumber(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals < 0 ? 0 : (decimals > 12 ? 12 : decimals), v);
    return buf;
}

inline std::string text(const Quantified& q, const NumberText& number = nullptr) {
    if (!q.known) { return std::string(); }
    // "-0" is not a reading anyone wants: a rounded-to-zero negative prints as "0".
    double v = q.value;
    const double scale = std::pow(10.0, q.decimals);
    if (std::fabs(v) * scale < 0.5) { v = 0.0; }
    const std::string n = number ? number(v, q.decimals) : plainNumber(v, q.decimals);
    return n + " " + unitSymbol(q.unit);
}

// Parse the stored choice; anything unknown is Automatic (the safe default).
inline UnitSystem unitSystemFromName(const std::string& s) {
    if (s == "metric") { return UnitSystem::Metric; }
    if (s == "imperial") { return UnitSystem::Imperial; }
    if (s == "aviation") { return UnitSystem::Aviation; }
    if (s == "nautical") { return UnitSystem::Nautical; }
    return UnitSystem::Automatic;
}
inline const char* unitSystemName(UnitSystem s) {
    switch (s) {
        case UnitSystem::Metric: return "metric";
        case UnitSystem::Imperial: return "imperial";
        case UnitSystem::Aviation: return "aviation";
        case UnitSystem::Nautical: return "nautical";
        case UnitSystem::Automatic: break;
    }
    return "automatic";
}

}  // namespace cascade::core::access
