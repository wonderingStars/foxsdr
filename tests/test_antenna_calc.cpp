// Tests for core/antenna_calc.hpp. Reference values: Wikipedia "Free-space path loss" (10 km at 2.4 GHz = 120.05 dB), "Dipole
// antenna" (143 / f metres, 468 / f feet), "Velocity factor" (100 MHz, 0.82, quarter wave = 0.62 m).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/antenna_calc.hpp"

#include <cmath>
#include <limits>

#include "test_check.hpp"

namespace ant = cascade::core::antenna;

int main() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    // wavelength
    CHECK_NEAR(*ant::wavelengthM(100.0), 2.99792458, 1e-9);
    CHECK_NEAR(*ant::wavelengthM(1.0), 299.792458, 1e-6);
    CHECK_NEAR(*ant::wavelengthM(1296.0), 0.23133, 1e-4);

    // half-wave dipole: 143 / f metres and 468 / f feet
    CHECK_NEAR(*ant::dipoleLengthM(14.2), 143.0 / 14.2, 0.01);
    CHECK_NEAR(ant::metresToFeet(*ant::dipoleLengthM(14.2)), 468.0 / 14.2, 0.15);
    CHECK_NEAR(*ant::dipoleLengthM(145.0), 143.0 / 145.0, 0.002);
    CHECK_NEAR(*ant::dipoleLengthM(7.1, 0.98), 0.5 * 299.792458 / 7.1 * 0.98, 1e-9);   // a thin-wire factor from the page
    CHECK(!ant::dipoleLengthM(14.2, 0.5));     // factor out of 0.90..1.0
    CHECK(!ant::dipoleLengthM(14.2, 1.2));
    CHECK(!ant::dipoleLengthM(14.2, nan));

    // quarter-wave ground plane: half the dipole; radials 5 percent longer
    const auto gp = ant::groundPlaneM(145.0);
    CHECK(gp.has_value());
    CHECK_NEAR(gp->radiatorM, 0.5 * 143.0 / 145.0, 0.002);
    CHECK_NEAR(gp->radialM, gp->radiatorM * 1.05, 1e-12);

    // Yagi starting lengths
    const auto y = ant::yagiStartingLengths(144.2, 5);
    CHECK(y.has_value() && y->size() == 5);
    CHECK((*y)[0].lengthM > (*y)[1].lengthM);          // reflector longer than the driven element
    CHECK((*y)[1].lengthM > (*y)[2].lengthM);          // driven longer than the first director
    CHECK((*y)[2].lengthM > (*y)[3].lengthM && (*y)[3].lengthM > (*y)[4].lengthM);   // directors step down
    CHECK((*y)[0].positionM == 0.0 && (*y)[1].positionM > 0.0 && (*y)[4].positionM > (*y)[3].positionM);
    CHECK_NEAR((*y)[1].lengthM, 0.473 * *ant::wavelengthM(144.2), 1e-12);
    CHECK(ant::yagiStartingLengths(144.2, 2)->size() == 2);
    CHECK(!ant::yagiStartingLengths(144.2, 1));
    CHECK(!ant::yagiStartingLengths(144.2, 0));
    CHECK(!ant::yagiStartingLengths(144.2, 21));
    CHECK(!ant::yagiStartingLengths(0.0, 5));

    // coax: the page's worked example, 0.62 m
    CHECK_NEAR(*ant::coaxLengthM(100.0, 0.25, 0.82), 0.82 * 0.25 * 2.99792458, 1e-9);
    CHECK_NEAR(*ant::coaxLengthM(100.0, 0.25, 0.82), 0.62, 0.01);
    CHECK_NEAR(*ant::coaxLengthM(100.0, 0.25, 1.0), 0.25 * 2.99792458, 1e-9);   // vacuum
    CHECK(!ant::coaxLengthM(100.0, 0.25, 0.0));
    CHECK(!ant::coaxLengthM(100.0, 0.25, 1.01));       // faster than light
    CHECK(!ant::coaxLengthM(100.0, 0.25, -0.5));
    CHECK(!ant::coaxLengthM(100.0, 0.0, 0.66));
    CHECK(!ant::coaxLengthM(100.0, -1.0, 0.66));
    CHECK(!ant::coaxLengthM(100.0, 1000.0, 0.66));
    CHECK(!ant::coaxLengthM(100.0, nan, 0.66));

    // free-space path loss
    CHECK_NEAR(*ant::fsplDb(10.0, 2400.0), 120.05, 0.02);
    CHECK_NEAR(*ant::fsplDb(1.0, 1.0), 32.4478, 1e-3);          // the constant itself
    CHECK_NEAR(*ant::fsplDb(20.0, 100.0) - *ant::fsplDb(10.0, 100.0), 6.0206, 1e-3);   // doubling the distance: +6.02 dB
    CHECK_NEAR(*ant::fsplDb(10.0, 200.0) - *ant::fsplDb(10.0, 100.0), 6.0206, 1e-3);
    CHECK(ant::fsplFarField(10.0, 2400.0));
    CHECK(!ant::fsplFarField(0.001, 100.0));                    // 1 m at 100 MHz is under 10 wavelengths
    CHECK(!ant::fsplDb(0.0, 100.0));
    CHECK(!ant::fsplDb(-5.0, 100.0));
    CHECK(!ant::fsplDb(5.0, 0.0));
    CHECK(!ant::fsplDb(5.0, -100.0));
    CHECK(!ant::fsplDb(nan, 100.0));
    CHECK(!ant::fsplDb(5.0, inf));

    // frequency range edges
    CHECK(ant::wavelengthM(ant::kMinMHz).has_value());
    CHECK(ant::wavelengthM(ant::kMaxMHz).has_value());
    CHECK(!ant::wavelengthM(ant::kMinMHz / 2));
    CHECK(!ant::wavelengthM(ant::kMaxMHz * 2));
    CHECK(!ant::wavelengthM(0.0));
    CHECK(!ant::wavelengthM(-1.0));
    CHECK(!ant::wavelengthM(nan));
    CHECK(!ant::wavelengthM(inf));

    return testSummary("test_antenna_calc");
}
