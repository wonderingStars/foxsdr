// Tests for core/access_units.hpp: the unit-system model.
//
// What must hold: the DEFAULT (Automatic) is exactly the units the code has
// always printed, the four explicit systems pick the units the header states,
// every conversion round-trips, the exact factors are exact, and an unknown
// value never prints as a number.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/access_units.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

#include "test_check.hpp"

namespace {

using namespace cascade::core::access;

std::string readSourceFile(const char* relative) {
    // __FILE__ is .../tests/test_access_units.cpp: the repository is one up.
    std::string here = __FILE__;
    const std::size_t cut = here.find_last_of("/\\");
    std::string root = (cut == std::string::npos) ? std::string(".") : here.substr(0, cut);
    const std::size_t up = root.find_last_of("/\\");
    root = (up == std::string::npos) ? std::string(".") : root.substr(0, up);
    std::ifstream in(root + "/" + relative, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void tableIsInEnumOrder() {
    for (int i = 0; i < kUnitCount; ++i) {
        CHECK(static_cast<int>(detail::kRows[i].unit) == i);
        CHECK(detail::kRows[i].symbol != nullptr && detail::kRows[i].symbol[0] != '\0');
    }
}

void automaticIsTheLegacyTable() {
    // From src/gui/track_metrics.hpp: aircraft / vessel / station: "%.0f ft",
    // "%.0f kt"; a satellite: "%.0f km", "%.2f km/s"; every range in km.
    const ObjectKind land[] = {ObjectKind::Aircraft, ObjectKind::Vessel, ObjectKind::Other};
    for (ObjectKind k : land) {
        CHECK(unitFor(UnitSystem::Automatic, k, Quantity::Altitude) == Unit::Foot);
        CHECK(unitFor(UnitSystem::Automatic, k, Quantity::Speed) == Unit::Knot);
        CHECK(unitFor(UnitSystem::Automatic, k, Quantity::Distance) == Unit::Kilometre);
    }
    CHECK(unitFor(UnitSystem::Automatic, ObjectKind::Satellite, Quantity::Altitude) == Unit::Kilometre);
    CHECK(unitFor(UnitSystem::Automatic, ObjectKind::Satellite, Quantity::Speed) == Unit::KilometrePerSecond);
    CHECK(unitFor(UnitSystem::Automatic, ObjectKind::Satellite, Quantity::Distance) == Unit::Kilometre);
    CHECK(unitFor(UnitSystem::Automatic, ObjectKind::Aircraft, Quantity::Temperature) == Unit::Celsius);
    CHECK(unitFor(UnitSystem::Automatic, ObjectKind::Aircraft, Quantity::Pressure) == Unit::Hectopascal);

    // The default digits are the digits the strings print.
    CHECK(defaultDecimals(Unit::Foot) == 0);
    CHECK(defaultDecimals(Unit::Knot) == 0);
    CHECK(defaultDecimals(Unit::Kilometre) == 0);
    CHECK(defaultDecimals(Unit::KilometrePerSecond) == 2);

    // And the rendered text equals the existing strings for the same inputs.
    // ISS: 421000 m, 7660 m/s -> "421 km", "7.66 km/s".
    CHECK(text(quantify(UnitSystem::Automatic, ObjectKind::Satellite, Quantity::Altitude, 421000.0)) == "421 km");
    CHECK(text(quantify(UnitSystem::Automatic, ObjectKind::Satellite, Quantity::Speed, 7660.0)) == "7.66 km/s");
    // An airliner: 10668 m = 35000 ft; 190 m/s: track_metrics prints
    // "%.0f kt" of speedMps * 1.94384 = 369.3 -> "369 kt".
    CHECK(text(quantify(UnitSystem::Automatic, ObjectKind::Aircraft, Quantity::Altitude, 10668.0)) == "35000 ft");
    CHECK(text(quantify(UnitSystem::Automatic, ObjectKind::Aircraft, Quantity::Speed, 190.0)) == "369 kt");
}

void automaticStillMatchesTheSourceItWasCopiedFrom() {
    // The literals the default was copied from must still be in the code. If
    // someone changes the detail block's units, this fails and the table above
    // has to be revisited with it.
    const std::string src = readSourceFile("src/gui/track_metrics.hpp");
    CHECK(!src.empty());
    CHECK(src.find("\"%.0f ft\"") != std::string::npos);
    CHECK(src.find("\"%.0f km\"") != std::string::npos);
    CHECK(src.find("\"speed   %.2f km/s\"") != std::string::npos);
    CHECK(src.find("\"speed   %.0f kt\"") != std::string::npos);
}

void explicitSystems() {
    struct Row { UnitSystem s; Unit alt, dist, speed, temp, pres; };
    const Row rows[] = {
        {UnitSystem::Metric, Unit::Metre, Unit::Kilometre, Unit::KilometrePerHour, Unit::Celsius, Unit::Hectopascal},
        {UnitSystem::Imperial, Unit::Foot, Unit::StatuteMile, Unit::MilePerHour, Unit::Fahrenheit, Unit::InchOfMercury},
        {UnitSystem::Aviation, Unit::Foot, Unit::NauticalMile, Unit::Knot, Unit::Celsius, Unit::Hectopascal},
        {UnitSystem::Nautical, Unit::Metre, Unit::NauticalMile, Unit::Knot, Unit::Celsius, Unit::Hectopascal},
    };
    for (const Row& r : rows) {
        CHECK(unitFor(r.s, ObjectKind::Aircraft, Quantity::Altitude) == r.alt);
        CHECK(unitFor(r.s, ObjectKind::Vessel, Quantity::Distance) == r.dist);
        CHECK(unitFor(r.s, ObjectKind::Aircraft, Quantity::Speed) == r.speed);
        CHECK(unitFor(r.s, ObjectKind::Other, Quantity::Temperature) == r.temp);
        CHECK(unitFor(r.s, ObjectKind::Other, Quantity::Pressure) == r.pres);
    }
    // A satellite under an explicit system: altitude in the distance unit,
    // speed per second in it - never feet or knots.
    CHECK(unitFor(UnitSystem::Imperial, ObjectKind::Satellite, Quantity::Altitude) == Unit::StatuteMile);
    CHECK(unitFor(UnitSystem::Imperial, ObjectKind::Satellite, Quantity::Speed) == Unit::MilePerSecond);
    CHECK(unitFor(UnitSystem::Aviation, ObjectKind::Satellite, Quantity::Altitude) == Unit::NauticalMile);
    CHECK(unitFor(UnitSystem::Aviation, ObjectKind::Satellite, Quantity::Speed) == Unit::NauticalMilePerSecond);
    CHECK(unitFor(UnitSystem::Metric, ObjectKind::Satellite, Quantity::Altitude) == Unit::Kilometre);
    CHECK(unitFor(UnitSystem::Metric, ObjectKind::Satellite, Quantity::Speed) == Unit::KilometrePerSecond);

    // Rendered: 35000 ft is 10668 m; 10.668 km.
    CHECK(text(quantify(UnitSystem::Imperial, ObjectKind::Aircraft, Quantity::Altitude, 10668.0)) == "35000 ft");
    CHECK(text(quantify(UnitSystem::Metric, ObjectKind::Aircraft, Quantity::Altitude, 10668.0)) == "10668 m");
    CHECK(text(quantify(UnitSystem::Imperial, ObjectKind::Aircraft, Quantity::Distance, 1609.344 * 12)) == "12 mi");
    CHECK(text(quantify(UnitSystem::Nautical, ObjectKind::Vessel, Quantity::Distance, 1852.0 * 7)) == "7 nmi");
    CHECK(text(quantify(UnitSystem::Metric, ObjectKind::Aircraft, Quantity::Speed, 100.0)) == "360 km/h");
    CHECK(text(quantify(UnitSystem::Aviation, ObjectKind::Aircraft, Quantity::Speed, 1852.0 / 3600.0 * 250.0)) == "250 kt");
    CHECK(text(quantify(UnitSystem::Imperial, ObjectKind::Other, Quantity::Temperature, 273.15)) == "32.0 \xC2\xB0" "F");
    CHECK(text(quantify(UnitSystem::Metric, ObjectKind::Other, Quantity::Pressure, 101325.0)) == "1013 hPa");
    CHECK(text(quantify(UnitSystem::Imperial, ObjectKind::Other, Quantity::Pressure, 101325.0)) == "29.92 inHg");
}

void exactFactors() {
    CHECK(convert(1.0, Unit::Foot, Unit::Metre) == 0.3048);
    CHECK(convert(1.0, Unit::StatuteMile, Unit::Metre) == 1609.344);
    CHECK(convert(1.0, Unit::NauticalMile, Unit::Metre) == 1852.0);
    CHECK_NEAR(convert(1.0, Unit::Knot, Unit::MetrePerSecond), 0.514444444444, 1e-9);
    CHECK_NEAR(convert(1.0, Unit::MilePerHour, Unit::MetrePerSecond), 0.44704, 1e-12);
    CHECK_NEAR(convert(1.0, Unit::MilePerHour, Unit::KilometrePerHour), 1.609344, 1e-12);
    CHECK_NEAR(convert(1.0, Unit::Hectopascal, Unit::Kilopascal), 0.1, 1e-12);
    CHECK_NEAR(convert(1.0, Unit::InchOfMercury, Unit::Kilopascal), 3.386389, 1e-9);
    CHECK_NEAR(convert(0.0, Unit::Celsius, Unit::Fahrenheit), 32.0, 1e-9);
    CHECK_NEAR(convert(100.0, Unit::Celsius, Unit::Fahrenheit), 212.0, 1e-9);
    CHECK_NEAR(convert(-40.0, Unit::Celsius, Unit::Fahrenheit), -40.0, 1e-9);
    CHECK_NEAR(convert(0.0, Unit::Kelvin, Unit::Celsius), -273.15, 1e-9);
    CHECK_NEAR(convert(-459.67, Unit::Fahrenheit, Unit::Kelvin), 0.0, 1e-9);
}

void roundTrips() {
    // Every unit into every compatible unit and back, over a spread of values
    // including zero, negatives (below-sea-level altitude, sub-zero air) and big.
    const double values[] = {0.0, 1.0, -1.0, 0.001, -273.0, 12.5, 421000.0, 1.0e7, -1500.25};
    for (int a = 0; a < kUnitCount; ++a) {
        for (int b = 0; b < kUnitCount; ++b) {
            const Unit ua = static_cast<Unit>(a), ub = static_cast<Unit>(b);
            if (!compatible(ua, ub)) { continue; }
            for (double v : values) {
                const double there = convert(v, ua, ub);
                const double back = convert(there, ub, ua);
                CHECK(std::isfinite(there));
                CHECK(std::fabs(back - v) <= 1e-9 * (1.0 + std::fabs(v)));
            }
        }
    }
}

void mismatchedAndUnknown() {
    CHECK(std::isnan(convert(1.0, Unit::Foot, Unit::Knot)));
    CHECK(std::isnan(convert(1.0, Unit::Celsius, Unit::Hectopascal)));
    CHECK(std::isnan(convert(std::nan(""), Unit::Foot, Unit::Metre)));
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    CHECK(!quantify(UnitSystem::Automatic, ObjectKind::Aircraft, Quantity::Altitude, nan).known);
    CHECK(!quantify(UnitSystem::Automatic, ObjectKind::Aircraft, Quantity::Altitude, inf).known);
    CHECK(text(quantify(UnitSystem::Imperial, ObjectKind::Aircraft, Quantity::Altitude, nan)).empty());
    // A quantity that is zero: "0 ft", and a tiny negative is not "-0 ft".
    CHECK(text(quantify(UnitSystem::Imperial, ObjectKind::Aircraft, Quantity::Altitude, 0.0)) == "0 ft");
    CHECK(text(quantify(UnitSystem::Imperial, ObjectKind::Aircraft, Quantity::Altitude, -0.01)) == "0 ft");
    // A real negative altitude (Dead Sea shore, about -430 m).
    CHECK(text(quantify(UnitSystem::Metric, ObjectKind::Aircraft, Quantity::Altitude, -430.0)) == "-430 m");
}

void customNumberFormatter() {
    // The locale-aware formatter is handed in; the model only chooses digits.
    const NumberText comma = [](double v, int d) {
        std::string s = plainNumber(v, d);
        for (char& c : s) { if (c == '.') { c = ','; } }
        return s;
    };
    CHECK(text(quantify(UnitSystem::Automatic, ObjectKind::Satellite, Quantity::Speed, 7660.0), comma) == "7,66 km/s");
}

void namesRoundTrip() {
    for (int i = 0; i < kUnitSystemCount; ++i) {
        const UnitSystem s = static_cast<UnitSystem>(i);
        CHECK(unitSystemFromName(unitSystemName(s)) == s);
    }
    CHECK(unitSystemFromName("") == UnitSystem::Automatic);
    CHECK(unitSystemFromName("furlongs") == UnitSystem::Automatic);
    CHECK(unitSystemFromName("IMPERIAL") == UnitSystem::Automatic);  // exact names only
}

}  // namespace

int main() {
    tableIsInEnumOrder();
    automaticIsTheLegacyTable();
    automaticStillMatchesTheSourceItWasCopiedFrom();
    explicitSystems();
    exactFactors();
    roundTrips();
    mismatchedAndUnknown();
    customNumberFormatter();
    namesRoundTrip();
    return testSummary("test_access_units");
}
