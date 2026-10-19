// Tests for gui/passband_slide.hpp - where Shift + wheel puts the tuned frequency (0.99.82).
// Pure arithmetic; the gesture through the application is tests/test_passband_slide_app.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/passband_slide.hpp"

#include <cmath>

#include "test_check.hpp"

using cascade::gui::passbandRasterStep;
using cascade::gui::planPassbandSlide;
using cascade::gui::PassbandSlide;

int main() {
    const double nan = std::nan("");
    const double inf = HUGE_VAL;

    // --- the raster step -----------------------------------------------------------------------
    // On the raster: exactly `notches` steps.
    CHECK_NEAR(passbandRasterStep(100.300e6, 1000.0, 1.0), 100.301e6, 1e-6);
    CHECK_NEAR(passbandRasterStep(100.300e6, 1000.0, -1.0), 100.299e6, 1e-6);
    CHECK_NEAR(passbandRasterStep(100.300e6, 1000.0, 7.0), 100.307e6, 1e-6);
    CHECK_NEAR(passbandRasterStep(100.300e6, 1000.0, -90.0), 100.210e6, 1e-6);
    // Off it: the first notch only reaches the raster in its direction.
    CHECK_NEAR(passbandRasterStep(100.3004e6, 1000.0, 1.0), 100.301e6, 1e-6);
    CHECK_NEAR(passbandRasterStep(100.3004e6, 1000.0, -1.0), 100.300e6, 1e-6);
    CHECK_NEAR(passbandRasterStep(100.3006e6, 1000.0, 2.0), 100.302e6, 1e-6);
    CHECK_NEAR(passbandRasterStep(100.3006e6, 1000.0, -2.0), 100.299e6, 1e-6);
    // The noise of a round trip through a double does not count as off the raster.
    CHECK_NEAR(passbandRasterStep(100.300e6 + 1e-7, 1000.0, 1.0), 100.301e6, 1e-6);
    CHECK_NEAR(passbandRasterStep(100.300e6 - 1e-7, 1000.0, -1.0), 100.299e6, 1e-6);
    // The other modes' rasters: NFM 12.5 kHz, WFM 100 kHz, AM 9 kHz.
    CHECK_NEAR(passbandRasterStep(145.5e6, 12500.0, 1.0), 145.5125e6, 1e-6);
    CHECK_NEAR(passbandRasterStep(100.1e6, 100000.0, -3.0), 99.8e6, 1e-6);
    CHECK_NEAR(passbandRasterStep(1.116e6, 9000.0, 1.0), 1.125e6, 1e-6);
    // A fraction truncates to the whole notches the caller counted; nothing to do for none.
    CHECK_NEAR(passbandRasterStep(100.3e6, 1000.0, 2.9), 100.302e6, 1e-6);
    CHECK_NEAR(passbandRasterStep(100.3e6, 1000.0, 0.0), 100.3e6, 0.0);
    // Rubbish in, the frequency back.
    CHECK_NEAR(passbandRasterStep(100.3e6, 0.0, 1.0), 100.3e6, 0.0);
    CHECK_NEAR(passbandRasterStep(100.3e6, -5.0, 1.0), 100.3e6, 0.0);
    CHECK_NEAR(passbandRasterStep(100.3e6, 1000.0, nan), 100.3e6, 0.0);
    CHECK_NEAR(passbandRasterStep(100.3e6, 1000.0, inf), 100.3e6, 0.0);
    CHECK(std::isnan(passbandRasterStep(nan, 1000.0, 1.0)));

    // --- the plan, inside the span ---------------------------------------------------------------
    // A 2 MS/s source at 100 MHz, a 3 kHz filter: the VFO reaches +/- 998.5 kHz.
    const double centre = 100e6, rate = 2e6, bw = 3000.0, step = 1000.0;
    {
        const PassbandSlide p =
            planPassbandSlide(centre + 300000.0, centre, rate, bw, step, 3.0, 0.0);
        CHECK(p.moved && !p.retuneSource);
        CHECK_NEAR(p.wantAbsHz, centre + 303000.0, 1e-6);
        CHECK_NEAR(p.offsetHz, 303000.0, 1e-6);
    }
    {
        const PassbandSlide p =
            planPassbandSlide(centre + 300000.0, centre, rate, bw, step, -2.0, 0.0);
        CHECK(p.moved && !p.retuneSource);
        CHECK_NEAR(p.offsetHz, 298000.0, 1e-6);
    }
    // The last raster point inside the reach is still inside.
    {
        const PassbandSlide p =
            planPassbandSlide(centre + 997000.0, centre, rate, bw, step, 1.0, 0.0);
        CHECK(p.moved && !p.retuneSource);
        CHECK_NEAR(p.offsetHz, 998000.0, 1e-6);
    }
    // No notches, nothing to do.
    {
        const PassbandSlide p = planPassbandSlide(centre + 3e5, centre, rate, bw, step, 0.0, 0.0);
        CHECK(!p.moved);
    }

    // --- past the end: the passband at the end of its reach, the source takes up the rest -----------
    {
        const PassbandSlide p =
            planPassbandSlide(centre + 998000.0, centre, rate, bw, step, 1.0, 0.0);
        CHECK(p.moved && p.retuneSource);
        CHECK_NEAR(p.wantAbsHz, centre + 999000.0, 1e-6);
        CHECK_NEAR(p.offsetHz, 998500.0, 1e-6);
        CHECK_NEAR(p.centreHz, centre + 500.0, 1e-6);
        CHECK_NEAR(p.centreHz + p.offsetHz, p.wantAbsHz, 1e-6);
    }
    {
        // Five notches from 997 kHz: 1002 kHz up, the passband at 998.5 kHz, the source 3.5 kHz up.
        const PassbandSlide p =
            planPassbandSlide(centre + 997000.0, centre, rate, bw, step, 5.0, 0.0);
        CHECK(p.moved && p.retuneSource);
        CHECK_NEAR(p.offsetHz, 998500.0, 1e-6);
        CHECK_NEAR(p.centreHz, centre + 3500.0, 1e-6);
    }
    {
        // Going down, symmetric.
        const PassbandSlide p =
            planPassbandSlide(centre - 997000.0, centre, rate, bw, step, -5.0, 0.0);
        CHECK(p.moved && p.retuneSource);
        CHECK_NEAR(p.offsetHz, -998500.0, 1e-6);
        CHECK_NEAR(p.centreHz, centre - 3500.0, 1e-6);
    }
    {
        // Already at the end, one more notch: only the source moves, by exactly the step.
        const PassbandSlide p =
            planPassbandSlide(centre + 998500.0, centre, rate, bw, step, 1.0, 0.0);
        CHECK(p.moved && p.retuneSource);
        CHECK_NEAR(p.offsetHz, 998500.0, 1e-6);
        CHECK_NEAR(p.wantAbsHz - (centre + 998500.0), 500.0, 1e-6);   // the raster point above
    }
    {
        // A filter wider than the span has no reach: it sits in the middle and the source walks.
        const PassbandSlide p =
            planPassbandSlide(centre, centre, 2e6, 2.5e6, 100000.0, 1.0, 0.0);
        CHECK(p.moved && p.retuneSource);
        CHECK_NEAR(p.offsetHz, 0.0, 0.0);
        CHECK_NEAR(p.centreHz, centre + 100000.0, 1e-6);
    }

    // --- the floor: a slide stops at the lowest tuned frequency the controls allow -----------------
    {
        const PassbandSlide p = planPassbandSlide(5000.0, 0.0, 48000.0, 2400.0, 1000.0, -20.0, 0.0);
        CHECK(p.moved);
        CHECK_NEAR(p.wantAbsHz, 0.0, 1e-9);
        const PassbandSlide q = planPassbandSlide(0.0, 0.0, 48000.0, 2400.0, 1000.0, -3.0, 0.0);
        CHECK(!q.moved);   // already there: nothing to do
    }

    // --- rubbish in: nothing moves ----------------------------------------------------------------
    CHECK(!planPassbandSlide(nan, centre, rate, bw, step, 1.0, 0.0).moved);
    CHECK(!planPassbandSlide(centre, nan, rate, bw, step, 1.0, 0.0).moved);
    CHECK(!planPassbandSlide(centre, centre, 0.0, bw, step, 1.0, 0.0).moved);
    CHECK(!planPassbandSlide(centre, centre, -2e6, bw, step, 1.0, 0.0).moved);
    CHECK(!planPassbandSlide(centre, centre, rate, nan, step, 1.0, 0.0).moved);
    CHECK(!planPassbandSlide(centre, centre, rate, bw, 0.0, 1.0, 0.0).moved);

    return testSummary("test_passband_slide");
}
