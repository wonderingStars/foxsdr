// THE NOISE FLOOR AUTOMATIC SQUELCH SITS ABOVE (0.99.74), in core/noise_floor_tracker.hpp.
// The tester: the SQUELCH knob wants "a button for AUTO or above the noise". The floor is the
// minimum of the last TEN seconds of 100 ms block means (three until the follow-up the owner
// asked for: a transmission longer than three seconds taught the floor and closed the gate on
// itself), and readings taken while the squelch gate is OPEN are not fed once a floor estimate
// exists, so a carrier never teaches it. This pins what that means:
//   * nothing is a measurement until half a second of blocks is in;
//   * steady noise reads as itself;
//   * a carrier that keys up for a second does NOT raise the floor (the whole reason it is a
//     minimum and not an average) and the threshold is not dragged up to it;
//   * noise that RISES and stays risen for longer than the window becomes the floor, and not
//     before - and, with no gate told about, likewise a carrier that holds the channel longer;
//   * TOLD THE GATE IS OPEN, a 20-second carrier after the floor is learnt leaves it unchanged,
//     through the carrier and after it; the first estimate still forms when the gate starts open;
//     a rise of the noise that stays under the gate's margin is followed within the window;
//   * ...and the one safety valve: a gate that stays open for longer than kNoiseFloorGateHoldS
//     (30 s) is the floor having risen past the threshold, not a carrier, or the squelch would
//     sit open on a noisier antenna for ever;
//   * "no data" (-200 before the first block and while stopped) is ignored and ages the
//     history out, never read as a very quiet channel;
//   * the threshold is the floor plus 8 dB, kept inside the slider's [-120, 0] dB span.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <limits>

#include "core/noise_floor_tracker.hpp"
#include "test_check.hpp"

namespace cc = cascade::core;

namespace {

constexpr double W = cc::kNoiseFloorWindowS;

// A frame loop's worth of readings: 60 a second, a little jitter either way, deterministic.
// `level(t)` is the channel power at time t, `gate(t)` whether the squelch gate is open.
template <class Level, class Gate>
void feedGate(cc::NoiseFloorTracker& t, double fromS, double toS, Level&& level, Gate&& gate) {
    int i = 0;
    for (double s = fromS; s < toS - 1.0e-9; s += 1.0 / 60.0, ++i) {
        const double jitter = (i % 3 == 0) ? -0.4 : (i % 3 == 1) ? 0.0 : 0.4;
        t.push(level(s) + jitter, s, gate(s));
    }
}

// The same with no gate told about (the tracker's own two-argument form).
template <class Level>
void feed(cc::NoiseFloorTracker& t, double fromS, double toS, Level&& level) {
    int i = 0;
    for (double s = fromS; s < toS - 1.0e-9; s += 1.0 / 60.0, ++i) {
        const double jitter = (i % 3 == 0) ? -0.4 : (i % 3 == 1) ? 0.0 : 0.4;
        t.push(level(s) + jitter, s);
    }
}

}  // namespace

int main() {
    std::printf("test_noise_floor_tracker\n");
    CHECK_NEAR(W, 10.0, 1.0e-12);  // the window the owner asked for (3 s before)

    // --- empty: nothing is a measurement -------------------------------------------------
    {
        std::printf("  empty and short histories have no floor\n");
        cc::NoiseFloorTracker t;
        CHECK(std::isnan(t.floorDb()));
        CHECK(t.blocks() == 0);
        // 0.45 s of steady -90: four closed blocks, under the five a measurement needs.
        feed(t, 0.0, 0.45, [](double) { return -90.0; });
        CHECK(t.blocks() < cc::kNoiseFloorMinBlocks);
        CHECK(std::isnan(t.floorDb()));
        // ...and a second of it is enough.
        feed(t, 0.45, 1.0, [](double) { return -90.0; });
        CHECK(!std::isnan(t.floorDb()));
        CHECK_NEAR(t.floorDb(), -90.0, 0.5);
    }

    // --- steady noise reads as itself -------------------------------------------------------
    {
        std::printf("  steady -90 dB gives a floor of -90 dB\n");
        cc::NoiseFloorTracker t;
        feed(t, 0.0, W + 5.0, [](double) { return -90.0; });
        CHECK_NEAR(t.floorDb(), -90.0, 0.5);
        // Held for the whole window: the history is bounded, not a growing list.
        CHECK(t.blocks() <= static_cast<int>(W / cc::kNoiseFloorBlockS) + 2);
        // (A block closes at the first 60 Hz reading 100 ms or more after it opened: 117 ms.)
        CHECK(t.blocks() >= static_cast<int>(W / (cc::kNoiseFloorBlockS + 1.0 / 60.0)) - 3);
    }

    // --- a block is the MEAN of its readings ---------------------------------------------------
    {
        std::printf("  a block holds the mean of what was read in it\n");
        cc::NoiseFloorTracker t;
        // Readings every 40 ms cycling -92, -88, -84: a tenth of a second is three of them
        // (the block closes at the first reading 100 ms or more after it opened, 120 ms on),
        // so every block means -88 whichever way the cycle falls.
        const double cycle[3] = {-92.0, -88.0, -84.0};
        for (int i = 0; i < 90; ++i) { t.push(cycle[i % 3], static_cast<double>(i) * 0.04); }
        CHECK(!std::isnan(t.floorDb()));
        CHECK_NEAR(t.floorDb(), -88.0, 0.01);
    }

    // --- bursts do not raise the floor ----------------------------------------------------------
    {
        std::printf("  -90 dB noise with one-second carriers at -40 dB keeps a floor of -90 dB\n");
        cc::NoiseFloorTracker t;
        // A carrier every two seconds, one second long (on from 1 to 2, 3 to 4, ...).
        const auto level = [](double s) { return (static_cast<int>(std::floor(s)) % 2 == 1) ? -40.0 : -90.0; };
        double worst = -1.0e9;
        double s = 0.0;
        for (; s < 14.0; s += 1.0 / 60.0) {
            t.push(level(s) + 0.0, s);
            if (s > 1.0 && !std::isnan(t.floorDb())) { worst = std::max(worst, t.floorDb()); }
        }
        CHECK(worst <= -89.0);
        CHECK_NEAR(t.floorDb(), -90.0, 0.5);
        // The threshold it gives stays 8 dB over the noise through every carrier.
        CHECK_NEAR(cc::autoSquelchTargetDb(worst), -82.0, 1.0);
    }

    // --- raised noise becomes the floor, but only after the window ------------------------------
    {
        std::printf("  noise rising to -80 dB for longer than the 10 s window becomes the floor, "
                    "not before\n");
        cc::NoiseFloorTracker t;
        const double r = W + 2.0;  // the rise: after a learnt floor
        feed(t, 0.0, r, [](double) { return -90.0; });
        CHECK_NEAR(t.floorDb(), -90.0, 0.5);
        // At -80 from r: one second in, the quietest block of the window is still -90.
        feed(t, r, r + 1.0, [](double) { return -80.0; });
        CHECK_NEAR(t.floorDb(), -90.0, 0.5);
        // Nearly the whole window in: still -90.
        feed(t, r + 1.0, r + W - 1.0, [](double) { return -80.0; });
        CHECK_NEAR(t.floorDb(), -90.0, 0.5);
        // More than the window in: the -90 blocks have aged out and the floor has followed.
        feed(t, r + W - 1.0, r + W + 0.6, [](double) { return -80.0; });
        CHECK_NEAR(t.floorDb(), -80.0, 0.5);
    }

    // --- with no gate told about, a carrier longer than the window IS the floor ---------------
    {
        std::printf("  a carrier longer than the window raises the floor when no gate is told about\n");
        cc::NoiseFloorTracker t;
        const double a = W + 1.0;
        feed(t, 0.0, a, [](double) { return -90.0; });
        feed(t, a, a + W + 0.6, [](double) { return -50.0; });
        CHECK_NEAR(t.floorDb(), -50.0, 0.5);
        // ...and when it drops, the floor falls back the moment a quiet block is in.
        feed(t, a + W + 0.6, a + W + 1.0, [](double) { return -90.0; });
        CHECK_NEAR(t.floorDb(), -90.0, 0.5);
    }

    // --- TOLD THE GATE IS OPEN, a carrier does not teach the floor -------------------------------
    {
        std::printf("  a 20 s carrier with the gate open, after the floor is learnt, leaves the "
                    "floor at -90 dB\n");
        cc::NoiseFloorTracker t;
        // Learn -90 with the gate shut (6 s: over the five blocks, under the window).
        feedGate(t, 0.0, 6.0, [](double) { return -90.0; }, [](double) { return false; });
        CHECK_NEAR(t.floorDb(), -90.0, 0.5);
        // The carrier: 20 s at -40 dB with the gate open - longer than the old 3 s window and
        // twice the new one, so every block of the learnt floor has aged out by its middle.
        double worst = -1.0e9;
        double lowest = 1.0e9;
        int nan = 0;
        for (double s = 6.0; s < 26.0 - 1.0e-9; s += 1.0 / 60.0) {
            t.push(-40.0, s, true);
            const double f = t.floorDb();
            if (std::isnan(f)) { ++nan; continue; }
            worst = std::max(worst, f);
            lowest = std::min(lowest, f);
        }
        std::printf("    during the carrier the floor stayed between %.2f and %.2f, NaN %d times\n",
                    lowest, worst, nan);
        CHECK(nan == 0);              // an estimate held through the carrier, never lost
        CHECK(worst <= -89.5);        // never dragged up toward the carrier
        CHECK_NEAR(t.floorDb(), -90.0, 0.5);
        // The carrier ends: still -90, and then -90 from the new blocks.
        feedGate(t, 26.0, 26.2, [](double) { return -90.0; }, [](double) { return false; });
        CHECK_NEAR(t.floorDb(), -90.0, 0.5);
        feedGate(t, 26.2, 28.0, [](double) { return -90.0; }, [](double) { return false; });
        CHECK_NEAR(t.floorDb(), -90.0, 0.5);
        CHECK(t.blocks() >= cc::kNoiseFloorMinBlocks);
        CHECK_NEAR(cc::autoSquelchTargetDb(t.floorDb()), -82.0, 0.5);
    }

    {
        std::printf("  the first estimate still forms when the gate starts open\n");
        cc::NoiseFloorTracker t;
        // The threshold at power-up is wherever the config left it: the gate is open on the
        // noise itself from the first reading. Nothing is held back before an estimate exists.
        feedGate(t, 0.0, 0.45, [](double) { return -70.0; }, [](double) { return true; });
        CHECK(std::isnan(t.floorDb()));
        feedGate(t, 0.45, 1.0, [](double) { return -70.0; }, [](double) { return true; });
        CHECK(!std::isnan(t.floorDb()));
        CHECK_NEAR(t.floorDb(), -70.0, 0.5);
        // From here the gate being open feeds nothing: a louder stretch changes nothing.
        feedGate(t, 1.0, 5.0, [](double) { return -30.0; }, [](double) { return true; });
        CHECK_NEAR(t.floorDb(), -70.0, 0.5);
    }

    {
        std::printf("  a rise of the noise under the gate's margin is followed within the window\n");
        cc::NoiseFloorTracker t;
        // The gate the application has: open when the level is over the threshold, which is
        // the floor plus 8 dB (-82 at a floor of -90).
        double threshold = cc::autoSquelchTargetDb(-90.0);
        CHECK_NEAR(threshold, -82.0, 1.0e-9);
        feedGate(t, 0.0, 6.0, [](double) { return -90.0; }, [](double) { return false; });
        CHECK_NEAR(t.floorDb(), -90.0, 0.5);
        const double r = 6.0;
        // -85 is 5 dB up: under the -82 threshold, so the gate stays shut and the readings are fed.
        const auto open = [&](double) { return -85.0 > threshold; };
        CHECK(!open(0.0));
        feedGate(t, r, r + W - 1.0, [](double) { return -85.0; }, open);
        CHECK_NEAR(t.floorDb(), -90.0, 0.5);  // not yet: the -90 blocks are still in the window
        feedGate(t, r + W - 1.0, r + W + 0.6, [](double) { return -85.0; }, open);
        CHECK_NEAR(t.floorDb(), -85.0, 0.5);  // followed, inside the window
        threshold = cc::autoSquelchTargetDb(t.floorDb());
        CHECK_NEAR(threshold, -77.0, 0.5);
    }

    // --- the safety valve: a gate open for longer than the hold is a risen floor ---------------
    {
        std::printf("  a gate held open longer than %.0f s is a floor that rose past the threshold\n",
                    cc::kNoiseFloorGateHoldS);
        cc::NoiseFloorTracker t;
        feedGate(t, 0.0, 6.0, [](double) { return -90.0; }, [](double) { return false; });
        // The noise steps up 15 dB (a noisier antenna): over the -82 threshold, so the gate
        // opens and stays open. Held back for the hold...
        // THE HOLD IS PINNED AS A NUMBER AND BY ITS EFFECT (30 s; longer than the 20 s carrier
        // above and the window, short enough that the squelch is not open on noise for a minute).
        CHECK_NEAR(cc::kNoiseFloorGateHoldS, 30.0, 1.0e-12);
        CHECK(cc::kNoiseFloorGateHoldS > 20.0);
        CHECK(cc::kNoiseFloorGateHoldS > W);
        const double r = 6.0;
        feedGate(t, r, r + 29.0, [](double) { return -75.0; }, [](double) { return true; });
        CHECK_NEAR(t.floorDb(), -90.0, 0.5);
        // ...and then it is the floor, within a second or two of the hold running out, instead
        // of the squelch sitting open on the noise for ever.
        feedGate(t, r + 29.0, r + 31.5, [](double) { return -75.0; }, [](double) { return true; });
        CHECK_NEAR(t.floorDb(), -75.0, 0.5);
        // Carriers that come and go never run the hold: a gate shut for a moment starts it over.
        cc::NoiseFloorTracker u;
        feedGate(u, 0.0, 6.0, [](double) { return -90.0; }, [](double) { return false; });
        double s = 6.0;
        for (int k = 0; k < 6; ++k) {  // six 20 s carriers, a shut-gate second between each
            feedGate(u, s, s + 20.0, [](double) { return -40.0; }, [](double) { return true; });
            s += 20.0;
            feedGate(u, s, s + 1.0, [](double) { return -90.0; }, [](double) { return false; });
            s += 1.0;
        }
        CHECK_NEAR(u.floorDb(), -90.0, 0.5);
    }

    // --- "no data" is ignored ----------------------------------------------------------------------
    {
        std::printf("  readings of -200 dB are ignored, and age the history out\n");
        cc::NoiseFloorTracker t;
        feed(t, 0.0, 2.0, [](double) { return -90.0; });
        CHECK_NEAR(t.floorDb(), -90.0, 0.5);
        // A second of the pipeline's "no data": the floor does not drop to -200.
        feed(t, 2.0, 3.0, [](double) { return -200.0; });
        CHECK_NEAR(t.floorDb(), -90.0, 0.5);
        // -200 alone never makes a floor.
        cc::NoiseFloorTracker only;
        feed(only, 0.0, 5.0, [](double) { return -200.0; });
        CHECK(std::isnan(only.floorDb()));
        CHECK(only.blocks() == 0);
        // Long enough with nothing, and the old history is gone: not a stale floor.
        feed(t, 3.0, W + 3.0, [](double) { return -200.0; });
        CHECK(std::isnan(t.floorDb()));
        // The same with the gate open: still "no data", still nothing held.
        cc::NoiseFloorTracker g;
        feedGate(g, 0.0, 2.0, [](double) { return -90.0; }, [](double) { return false; });
        feedGate(g, 2.0, W + 3.0, [](double) { return -200.0; }, [](double) { return true; });
        CHECK(std::isnan(g.floorDb()));
        // The boundary itself: -150 is "no data", a hair above it is a very quiet channel.
        cc::NoiseFloorTracker edge;
        for (double s = 0.0; s < 1.0; s += 1.0 / 60.0) { edge.push(-150.0, s); }
        CHECK(std::isnan(edge.floorDb()));
        for (double s = 0.0; s < 1.0; s += 1.0 / 60.0) { edge.push(-149.9, s); }
        CHECK(!std::isnan(edge.floorDb()));
        // A reading that is not a number is not a measurement either.
        cc::NoiseFloorTracker nan;
        for (double s = 0.0; s < 1.0; s += 1.0 / 60.0) { nan.push(std::nan(""), s); }
        CHECK(std::isnan(nan.floorDb()));
    }

    // --- reset, and a clock that goes back ---------------------------------------------------------
    {
        std::printf("  reset forgets; a clock that goes backwards starts again\n");
        cc::NoiseFloorTracker t;
        feed(t, 0.0, 2.0, [](double) { return -90.0; });
        CHECK(!std::isnan(t.floorDb()));
        t.reset();
        CHECK(std::isnan(t.floorDb()));
        CHECK(t.blocks() == 0);
        feed(t, 0.0, 2.0, [](double) { return -70.0; });
        CHECK_NEAR(t.floorDb(), -70.0, 0.5);
        // A new session on a clock that restarted at 0 does not inherit the old blocks.
        cc::NoiseFloorTracker u;
        feed(u, 100.0, 102.0, [](double) { return -90.0; });
        feed(u, 0.0, 2.0, [](double) { return -60.0; });
        CHECK_NEAR(u.floorDb(), -60.0, 0.5);
        // Reset also forgets a floor HELD through a carrier, and the gate's run: the next
        // estimate is the next channel's.
        cc::NoiseFloorTracker h;
        feedGate(h, 0.0, 6.0, [](double) { return -90.0; }, [](double) { return false; });
        feedGate(h, 6.0, 20.0, [](double) { return -40.0; }, [](double) { return true; });
        CHECK_NEAR(h.floorDb(), -90.0, 0.5);  // held, the ring having drained
        h.reset();
        CHECK(std::isnan(h.floorDb()));
        feedGate(h, 20.0, 21.0, [](double) { return -70.0; }, [](double) { return true; });
        CHECK_NEAR(h.floorDb(), -70.0, 0.5);  // a fresh channel: everything is fed again
    }

    // --- the threshold: the margin, and the span --------------------------------------------------
    {
        std::printf("  the threshold is the floor plus 8 dB, inside [-120, 0]\n");
        CHECK_NEAR(cc::kSquelchAutoMarginDb, 8.0, 1.0e-12);
        CHECK_NEAR(cc::autoSquelchTargetDb(-90.0), -82.0, 1.0e-9);
        CHECK_NEAR(cc::autoSquelchTargetDb(-60.0), -52.0, 1.0e-9);
        // Clamped at the bottom: -130 + 8 = -122 is under the slider's -120.
        CHECK_NEAR(cc::autoSquelchTargetDb(-130.0), -120.0, 1.0e-9);
        CHECK_NEAR(cc::autoSquelchTargetDb(-128.0), -120.0, 1.0e-9);
        CHECK_NEAR(cc::autoSquelchTargetDb(-127.0), -119.0, 1.0e-9);
        // ...and at the top: -5 + 8 = 3 is over the slider's 0.
        CHECK_NEAR(cc::autoSquelchTargetDb(-5.0), 0.0, 1.0e-9);
        CHECK_NEAR(cc::autoSquelchTargetDb(-8.0), 0.0, 1.0e-9);
        CHECK_NEAR(cc::autoSquelchTargetDb(-9.0), -1.0, 1.0e-9);
        // No measurement, no threshold.
        CHECK(std::isnan(cc::autoSquelchTargetDb(std::nan(""))));
        CHECK(std::isnan(cc::autoSquelchTargetDb(std::numeric_limits<double>::infinity())));
        // End to end: steady -90 noise gives -82.
        cc::NoiseFloorTracker t;
        feed(t, 0.0, 3.0, [](double) { return -90.0; });
        CHECK_NEAR(cc::autoSquelchTargetDb(t.floorDb()), -82.0, 0.5);
    }

    return testSummary("test_noise_floor_tracker");
}
