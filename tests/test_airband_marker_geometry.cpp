// Tests for gui/airband_marker_geometry.hpp - where an Airband channel's mark
// lies on the spectrum (0.99.66: the receiver's spectrum shows the channels
// the monitor is playing in place of the VFO band it is not demodulating).
//
// What each leg proves:
//  1. A 10 kHz channel on a 10 MHz span over 1000 px is a tenth of a pixel
//     wide; its mark is forced to kAirbandMarkMinPx (3 px), centred on the
//     channel - the rule the pure function exists for.
//  2. Zoomed in until the channel is wider than 3 px, the mark is the
//     channel's true width, not the minimum.
//  3. The boundary: a channel exactly the minimum wide, and one a pixel wider,
//     are both drawn at their own width (the minimum only ever widens).
//  4. A channel whose CENTRE is outside the view has no mark; one centred just
//     inside has one that may run past the panel edge (the painter clips).
//  5. An unusable panel or scale, a bad frequency, or a bad bandwidth never
//     yields garbage: no mark, or the minimum mark.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <limits>

#include "gui/airband_marker_geometry.hpp"
#include "gui/freq_scale.hpp"
#include "test_check.hpp"

int main() {
    using cascade::gui::airbandMarkExtent;
    using cascade::gui::AirbandMarkExtent;
    using cascade::gui::FreqScale;
    using cascade::gui::kAirbandMarkMinPx;

    static_assert(kAirbandMarkMinPx == 3.0f, "the request was a mark of at least three pixels");

    constexpr float kX0 = 100.0f;
    constexpr float kW = 1000.0f;

    // --- 1. A sub-pixel channel is forced to the minimum, centred. ------------
    {
        FreqScale wide;
        wide.setSpan(120.0e6, 10.0e6);   // view 115 - 125 MHz: 10 kHz per pixel
        AirbandMarkExtent e;
        CHECK(airbandMarkExtent(wide, 120.0e6, 10.0e3, kX0, kW, e));
        CHECK_NEAR(e.xCentre, 600.0, 1e-3);
        CHECK_NEAR(e.xHi - e.xLo, kAirbandMarkMinPx, 1e-3);
        CHECK_NEAR(0.5 * (e.xLo + e.xHi), e.xCentre, 1e-3);
        // Off-centre it is centred on ITS channel: 121 MHz is 100 px to the right.
        CHECK(airbandMarkExtent(wide, 121.0e6, 10.0e3, kX0, kW, e));
        CHECK_NEAR(e.xCentre, 700.0, 1e-3);
        CHECK_NEAR(e.xHi - e.xLo, kAirbandMarkMinPx, 1e-3);
        CHECK_NEAR(0.5 * (e.xLo + e.xHi), 700.0, 1e-3);
    }

    // --- 2. Zoomed in, the channel's own width wins. --------------------------
    {
        FreqScale narrow;
        narrow.setSpan(120.0e6, 100.0e3);   // view 119.95 - 120.05 MHz: 100 Hz per pixel
        AirbandMarkExtent e;
        CHECK(airbandMarkExtent(narrow, 120.0e6, 10.0e3, kX0, kW, e));
        CHECK_NEAR(e.xLo, 550.0, 1e-2);
        CHECK_NEAR(e.xHi, 650.0, 1e-2);
        CHECK_NEAR(e.xCentre, 600.0, 1e-2);
    }

    // --- 3. The boundary: the minimum only ever widens. -----------------------
    {
        FreqScale wide;
        wide.setSpan(120.0e6, 10.0e6);
        AirbandMarkExtent e;
        CHECK(airbandMarkExtent(wide, 120.0e6, 30.0e3, kX0, kW, e));   // exactly 3 px
        CHECK_NEAR(e.xHi - e.xLo, 3.0, 1e-3);
        CHECK(airbandMarkExtent(wide, 120.0e6, 40.0e3, kX0, kW, e));   // 4 px
        CHECK_NEAR(e.xHi - e.xLo, 4.0, 1e-3);
        CHECK(airbandMarkExtent(wide, 120.0e6, 20.0e3, kX0, kW, e));   // 2 px: widened to 3
        CHECK_NEAR(e.xHi - e.xLo, 3.0, 1e-3);
    }

    // --- 4. In or out of the view is decided by the channel's centre. ---------
    {
        FreqScale wide;
        wide.setSpan(120.0e6, 10.0e6);
        AirbandMarkExtent e;
        CHECK(!airbandMarkExtent(wide, 126.0e6, 10.0e3, kX0, kW, e));
        CHECK(!airbandMarkExtent(wide, 114.0e6, 10.0e3, kX0, kW, e));
        CHECK(!airbandMarkExtent(wide, 125.001e6, 10.0e3, kX0, kW, e));   // centre just outside
        CHECK(airbandMarkExtent(wide, 124.999e6, 10.0e3, kX0, kW, e));    // centre just inside
        CHECK(e.xHi > kX0 + kW - 5.0f);   // runs to the panel's edge (the painter clips what passes it)
    }

    // --- 5. Nothing unusable becomes a mark, or garbage. ----------------------
    {
        AirbandMarkExtent e;
        const FreqScale inert;   // a default scale has no span
        CHECK(!airbandMarkExtent(inert, 0.0, 10.0e3, kX0, kW, e));
        CHECK(!airbandMarkExtent(inert, 120.0e6, 10.0e3, kX0, kW, e));

        FreqScale wide;
        wide.setSpan(120.0e6, 10.0e6);
        CHECK(!airbandMarkExtent(wide, 120.0e6, 10.0e3, kX0, 0.0f, e));
        CHECK(!airbandMarkExtent(wide, 120.0e6, 10.0e3, kX0, -5.0f, e));
        CHECK(!airbandMarkExtent(wide, std::numeric_limits<double>::quiet_NaN(), 10.0e3, kX0, kW, e));
        // A missing or nonsense bandwidth still marks the channel, at the minimum.
        CHECK(airbandMarkExtent(wide, 120.0e6, 0.0, kX0, kW, e));
        CHECK_NEAR(e.xHi - e.xLo, kAirbandMarkMinPx, 1e-3);
        CHECK(airbandMarkExtent(wide, 120.0e6, -10.0e3, kX0, kW, e));
        CHECK_NEAR(e.xHi - e.xLo, kAirbandMarkMinPx, 1e-3);
        CHECK(airbandMarkExtent(wide, 120.0e6, std::numeric_limits<double>::quiet_NaN(), kX0, kW, e));
        CHECK_NEAR(e.xHi - e.xLo, kAirbandMarkMinPx, 1e-3);
    }

    return testSummary("test_airband_marker_geometry");
}
