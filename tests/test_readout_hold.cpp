// Tests for gui/readout_hold.hpp (0.99.4): the holds that keep FRAME TIME and
// the AUDIO - UNDERRUNS card from reprinting their text every frame.
//
// The property under test is the one the field report asked for - "printed
// slower" - stated as numbers: fed at 60 frames a second for three seconds,
// a held readout changes its shown value at most six times, where the live
// value changed on nearly every frame.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <limits>

#include "gui/readout_hold.hpp"
#include "test_check.hpp"

using cascade::gui::HeldMean;
using cascade::gui::HeldSample;
using cascade::gui::kReadoutHoldS;

int main() {
    const double frameS = 1.0 / 60.0;

    // --- HeldMean ------------------------------------------------------------
    {
        HeldMean h;
        CHECK(!h.have());
        // The first sample shows at once: no blank readout at start-up.
        h.add(10.0, 0.0081);
        CHECK(h.have());
        CHECK_NEAR(h.value(), 0.0081, 1e-12);

        // A jittering frame time, alternating 8 ms and 9 ms every frame, for
        // three seconds. The live text would change on every frame; the held
        // one may change only when a window closes.
        int changes = 0;
        double last = h.value();
        double t = 10.0;
        for (int i = 0; i < 180; ++i) {
            t += frameS;
            h.add(t, (i % 2 == 0) ? 0.008 : 0.009);
            if (h.value() != last) {
                ++changes;
                last = h.value();
            }
        }
        CHECK(changes >= 1);
        CHECK(changes <= static_cast<int>(3.0 / kReadoutHoldS));
        // And what it shows is the MEAN, not whichever frame was last.
        CHECK_NEAR(h.value(), 0.0085, 0.0002);
    }
    {
        // Nothing changes before the period has elapsed.
        HeldMean h(0.5);
        h.add(0.0, 0.010);
        h.add(0.1, 0.020);
        h.add(0.2, 0.030);
        h.add(0.49, 0.040);
        CHECK_NEAR(h.value(), 0.010, 1e-12);
        // The window closes: the mean of every sample in it (10, 20, 30, 40, 50).
        h.add(0.5, 0.050);
        CHECK_NEAR(h.value(), 0.030, 1e-12);
    }
    {
        // Non-finite samples are ignored, never shown.
        HeldMean h(0.5);
        h.add(0.0, std::numeric_limits<double>::quiet_NaN());
        CHECK(!h.have());
        h.add(0.1, 0.010);
        h.add(0.2, std::numeric_limits<double>::infinity());
        h.add(0.7, 0.020);
        CHECK(h.have());
        CHECK(std::isfinite(h.value()));
        CHECK_NEAR(h.value(), 0.015, 1e-12);
    }
    {
        // A clock that jumps backwards restarts the window instead of freezing
        // the readout until time catches up with the old origin.
        HeldMean h(0.5);
        h.add(100.0, 0.010);
        h.add(100.6, 0.010);
        h.add(1.0, 0.030);   // origin reset
        h.add(1.6, 0.030);
        CHECK_NEAR(h.value(), 0.030, 1e-12);
    }

    // --- HeldSample ------------------------------------------------------------
    {
        HeldSample<unsigned long long> h(0.5);
        // First call shows the live value immediately.
        CHECK(h.value(0.0, 3ull) == 3ull);
        // A counter climbing every frame is NOT reprinted every frame.
        CHECK(h.value(0.1, 4ull) == 3ull);
        CHECK(h.value(0.49, 9ull) == 3ull);
        // Re-read once the period has passed.
        CHECK(h.value(0.5, 10ull) == 10ull);
        CHECK(h.value(0.6, 11ull) == 10ull);

        int changes = 0;
        unsigned long long last = h.value(0.6, 11ull);
        double t = 0.6;
        unsigned long long live = 11ull;
        for (int i = 0; i < 180; ++i) {
            t += frameS;
            ++live;
            const unsigned long long shown = h.value(t, live);
            if (shown != last) {
                ++changes;
                last = shown;
            }
        }
        CHECK(changes >= 1);
        CHECK(changes <= static_cast<int>(3.0 / 0.5) + 1);
        // It is never AHEAD of the live value and never more than one period
        // (30 frames) behind it.
        CHECK(last <= live);
        CHECK(live - last <= 31ull);
    }
    {
        // A backwards clock refreshes rather than holding forever.
        HeldSample<int> h(0.5);
        CHECK(h.value(100.0, 1) == 1);
        CHECK(h.value(5.0, 2) == 2);
    }

    return testSummary("test_readout_hold");
}
