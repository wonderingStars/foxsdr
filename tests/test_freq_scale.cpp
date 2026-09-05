// Tests for gui/freq_scale.hpp — the shared frequency-axis view transform.
// Everything is pure math, so the whole contract is testable headless.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/freq_scale.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "test_check.hpp"

using cascade::gui::FreqScale;

namespace {

// Fixed-seed LCG (project convention: no <random>).
std::uint32_t g_lcg = 0xF00DFACEu;
double next01() {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return static_cast<double>(g_lcg >> 8) / 16777216.0;
}

// Independent reference for the documented tick-step rule: the smallest
// {1,2,5}*10^k that is >= minHz (up to FP noise), found by brute-force scan
// rather than by mirroring the implementation's exponent arithmetic.
double refNiceStep(double minHz) {
    for (int k = -12; k <= 14; ++k) {
        const double base = std::pow(10.0, k);
        const double mantissas[] = {1.0, 2.0, 5.0};
        for (double m : mantissas) {
            const double s = m * base;
            if (s >= minHz * (1.0 - 1e-9)) { return s; }
        }
    }
    return 0.0;
}

// True when step is {1,2,5}*10^k for some integer k.
bool isNiceStep(double step) {
    if (!(step > 0.0)) { return false; }
    const double e = std::floor(std::log10(step) + 1e-9);
    const double m = step / std::pow(10.0, e);
    return std::fabs(m - 1.0) < 1e-6 || std::fabs(m - 2.0) < 1e-6 ||
           std::fabs(m - 5.0) < 1e-6;
}

}  // namespace

int main() {
    const double qnan = std::nan("");

    // ------------------------------------------------------------------
    // Zoom-about-cursor invariant: the Hz under the cursor is identical
    // before and after zoomAt (1e-6 relative), for cursor positions well
    // away from 0.5 so a zoom-about-center bug cannot hide.
    // ------------------------------------------------------------------
    {
        FreqScale fs;
        fs.setSpan(100e6, 10e6);  // full span [95 MHz, 105 MHz]
        const double fracs[] = {0.0, 0.1, 0.25, 0.5, 0.7, 1.0};
        const double factors[] = {1.3, 2.0, 4.0};
        for (double frac : fracs) {
            for (double factor : factors) {
                fs.resetView();
                const double before = fs.xToHz(frac);
                fs.zoomAt(frac, factor);
                const double after = fs.xToHz(frac);
                CHECK_NEAR(after, before, std::fabs(before) * 1e-6);
            }
        }

        // Chained random zoom-ins: the invariant must hold at every step,
        // including once the span has hit the minimum-span clamp (the span
        // clamp changes the width, never the anchor).
        fs.resetView();
        for (int i = 0; i < 40; ++i) {
            const double frac = next01();
            const double factor = 1.1 + 3.0 * next01();  // always zooming in
            const double before = fs.xToHz(frac);
            fs.zoomAt(frac, factor);
            CHECK_NEAR(fs.xToHz(frac), before, std::fabs(before) * 1e-6);
        }

        // Zoom OUT with the view well interior: no edge clamp is triggered,
        // so the cursor invariant must hold in this direction too.
        fs.resetView();
        fs.zoomAt(0.5, 4.0);  // view [98.75, 101.25] MHz, interior
        const double before = fs.xToHz(0.3);
        fs.zoomAt(0.3, 0.8);  // span 2.5 -> 3.125 MHz, stays interior
        CHECK_NEAR(fs.xToHz(0.3), before, std::fabs(before) * 1e-6);
    }

    // ------------------------------------------------------------------
    // Zoom clamps: at the full span (zoom out) and at rate/1000 (zoom in).
    // ------------------------------------------------------------------
    {
        FreqScale fs;
        fs.setSpan(100e6, 10e6);

        // Zooming out past the full span pins the view to exactly the span.
        fs.zoomAt(0.5, 2.0);
        fs.zoomAt(0.3, 0.25);  // wants 20 MHz > 10 MHz full
        CHECK_NEAR(fs.viewLowHz(), 95e6, 1e-3);
        CHECK_NEAR(fs.viewHighHz(), 105e6, 1e-3);
        fs.zoomAt(0.9, 0.001);  // still pinned, off-center anchor
        CHECK_NEAR(fs.viewLowHz(), 95e6, 1e-3);
        CHECK_NEAR(fs.viewHighHz(), 105e6, 1e-3);

        // Zooming in beyond rate/1000 pins the span to exactly rate/1000.
        fs.resetView();
        fs.zoomAt(0.3, 1e9);
        CHECK_NEAR(fs.viewHighHz() - fs.viewLowHz(), 10e6 / 1000.0, 1e-2);
        fs.zoomAt(0.7, 5.0);  // further zoom-in cannot shrink it more
        CHECK_NEAR(fs.viewHighHz() - fs.viewLowHz(), 10e6 / 1000.0, 1e-2);

        // Zoom-out anchored at an edge-adjacent view needs the positional
        // clamp: the window must shift back inside the full span.
        fs.resetView();
        fs.zoomAt(0.0, 4.0);   // view [95, 97.5] MHz, hugging the low edge
        fs.zoomAt(1.0, 0.5);   // wants [92.5, 97.5]; must shift to [95, 100]
        CHECK_NEAR(fs.viewLowHz(), 95e6, 1e-3);
        CHECK_NEAR(fs.viewHighHz(), 100e6, 1e-3);

        // Degenerate arguments are no-ops, not corruption.
        const double lo = fs.viewLowHz();
        const double hi = fs.viewHighHz();
        fs.zoomAt(qnan, 2.0);
        fs.zoomAt(0.5, qnan);
        fs.zoomAt(0.5, 0.0);
        fs.zoomAt(0.5, -3.0);
        CHECK_NEAR(fs.viewLowHz(), lo, 0.0);
        CHECK_NEAR(fs.viewHighHz(), hi, 0.0);
    }

    // ------------------------------------------------------------------
    // Pan: exact shift by fracDelta * span, clamped at both edges with the
    // span preserved.
    // ------------------------------------------------------------------
    {
        FreqScale fs;
        fs.setSpan(100e6, 10e6);
        fs.zoomAt(0.5, 4.0);  // view [98.75, 101.25] MHz, span 2.5 MHz

        fs.pan(0.5);  // +1.25 MHz
        CHECK_NEAR(fs.viewLowHz(), 100e6, 1e-3);
        CHECK_NEAR(fs.viewHighHz(), 102.5e6, 1e-3);

        fs.pan(1e6);  // clamps at the high edge
        CHECK_NEAR(fs.viewHighHz(), 105e6, 1e-3);
        CHECK_NEAR(fs.viewLowHz(), 102.5e6, 1e-3);

        fs.pan(-1e9);  // clamps at the low edge
        CHECK_NEAR(fs.viewLowHz(), 95e6, 1e-3);
        CHECK_NEAR(fs.viewHighHz(), 97.5e6, 1e-3);

        fs.pan(0.0);  // no-op
        CHECK_NEAR(fs.viewLowHz(), 95e6, 1e-3);

        fs.pan(qnan);  // guarded no-op
        CHECK_NEAR(fs.viewLowHz(), 95e6, 1e-3);
        CHECK_NEAR(fs.viewHighHz() - fs.viewLowHz(), 2.5e6, 1e-3);
    }

    // ------------------------------------------------------------------
    // xToHz / hzToX: exact endpoints, roundtrips both ways, off-view
    // values land outside [0, 1] instead of clamping.
    // ------------------------------------------------------------------
    {
        FreqScale fs;
        fs.setSpan(100e6, 10e6);
        CHECK_NEAR(fs.xToHz(0.0), 95e6, 1e-3);
        CHECK_NEAR(fs.xToHz(1.0), 105e6, 1e-3);
        CHECK_NEAR(fs.hzToX(95e6), 0.0, 1e-12);
        CHECK_NEAR(fs.hzToX(105e6), 1.0, 1e-12);
        CHECK_NEAR(fs.hzToX(85e6), -1.0, 1e-9);   // off-view low
        CHECK_NEAR(fs.hzToX(110e6), 1.5, 1e-9);   // off-view high

        fs.zoomAt(0.37, 6.0);  // roundtrip must survive an odd zoom state
        for (int i = 0; i < 100; ++i) {
            const double hz = fs.viewLowHz() +
                              next01() * (fs.viewHighHz() - fs.viewLowHz());
            CHECK_NEAR(fs.xToHz(fs.hzToX(hz)), hz, std::fabs(hz) * 1e-9 + 1e-3);
            const double frac = next01() * 1.4 - 0.2;  // includes off-widget
            CHECK_NEAR(fs.hzToX(fs.xToHz(frac)), frac, 1e-9);
        }
    }

    // ------------------------------------------------------------------
    // visibleBinRange: full view is exactly [0, N-1]; half views hit the
    // midpoint; degenerate sizes are defined.
    // ------------------------------------------------------------------
    {
        FreqScale fs;
        fs.setSpan(100e6, 10e6);
        double first = -1.0, last = -1.0;

        fs.visibleBinRange(4096, first, last);
        CHECK_NEAR(first, 0.0, 1e-6);
        CHECK_NEAR(last, 4095.0, 1e-6);

        fs.zoomAt(0.0, 2.0);  // lower half [95, 100] MHz
        fs.visibleBinRange(4096, first, last);
        CHECK_NEAR(first, 0.0, 1e-6);
        CHECK_NEAR(last, 4095.0 / 2.0, 1e-6);

        fs.resetView();
        fs.zoomAt(1.0, 2.0);  // upper half [100, 105] MHz
        fs.visibleBinRange(4096, first, last);
        CHECK_NEAR(first, 4095.0 / 2.0, 1e-6);
        CHECK_NEAR(last, 4095.0, 1e-6);

        fs.visibleBinRange(1, first, last);  // single bin: no extent
        CHECK_NEAR(first, 0.0, 0.0);
        CHECK_NEAR(last, 0.0, 0.0);
        fs.visibleBinRange(0, first, last);
        CHECK_NEAR(first, 0.0, 0.0);
        CHECK_NEAR(last, 0.0, 0.0);

        FreqScale inert;  // no setSpan: "show everything"
        inert.visibleBinRange(1024, first, last);
        CHECK_NEAR(first, 0.0, 0.0);
        CHECK_NEAR(last, 1023.0, 0.0);
    }

    // ------------------------------------------------------------------
    // Ticks: pinned label strings for every unit (the ONE formatting rule),
    // exact tick positions, cap behavior, and guard rails.
    // ------------------------------------------------------------------
    {
        FreqScale fs;
        double hz[128];
        char labels[128][16];

        // THE WORKED EXAMPLES ARE 1100 PX WIDE. Each puts ten divisions on the
        // axis, and ten divisions of 1100 px are 110 px apart - over the
        // 104 px floor freq_scale.cpp sets for the current lettering. At the
        // 1000 px they were written for, the divisions were 100 px apart,
        // which the old 80 px floor accepted and the current one does not:
        // the step would double and every pinned label below would move.
        //
        // MHz axis, integer labels: [95, 105] MHz at 1100 px -> 1 MHz step.
        fs.setSpan(100e6, 10e6);
        int n = fs.ticks(1100.0, hz, labels, 128);
        CHECK(n == 11);
        for (int i = 0; i < n; ++i) {
            CHECK_NEAR(hz[i], 95e6 + 1e6 * i, 1e-3);
        }
        CHECK(std::strcmp(labels[0], "95 MHz") == 0);
        CHECK(std::strcmp(labels[5], "100 MHz") == 0);
        CHECK(std::strcmp(labels[10], "105 MHz") == 0);

        // MHz axis, one decimal (the task's "100.3 MHz" example): full span
        // [100, 101] MHz at 1100 px -> 100 kHz step, unit MHz, 1 decimal.
        fs.setSpan(100.5e6, 1e6);
        n = fs.ticks(1100.0, hz, labels, 128);
        CHECK(n == 11);
        CHECK_NEAR(hz[3], 100.3e6, 1e-3);
        CHECK(std::strcmp(labels[0], "100.0 MHz") == 0);
        CHECK(std::strcmp(labels[3], "100.3 MHz") == 0);
        CHECK(std::strcmp(labels[10], "101.0 MHz") == 0);

        // kHz axis: [400, 600] kHz at 1100 px -> 20 kHz step, integer kHz.
        fs.setSpan(500e3, 200e3);
        n = fs.ticks(1100.0, hz, labels, 128);
        CHECK(n == 11);
        CHECK(std::strcmp(labels[0], "400 kHz") == 0);
        CHECK(std::strcmp(labels[1], "420 kHz") == 0);
        CHECK(std::strcmp(labels[10], "600 kHz") == 0);

        // Hz axis: [300, 700] Hz at 500 px -> 100 Hz step.
        fs.setSpan(500.0, 400.0);
        n = fs.ticks(500.0, hz, labels, 128);
        CHECK(n == 5);
        CHECK(std::strcmp(labels[0], "300 Hz") == 0);
        CHECK(std::strcmp(labels[4], "700 Hz") == 0);

        // GHz axis, decimals from a 2 MHz step: [5.89, 5.91] GHz at 1100 px.
        fs.setSpan(5.9e9, 20e6);
        n = fs.ticks(1100.0, hz, labels, 128);
        CHECK(n == 11);
        CHECK(std::strcmp(labels[0], "5.890 GHz") == 0);
        CHECK(std::strcmp(labels[1], "5.892 GHz") == 0);
        CHECK(std::strcmp(labels[10], "5.910 GHz") == 0);

        // Axis straddling zero: negative labels, and 0 must not print -0.0.
        fs.setSpan(0.0, 2e6);  // [-1, 1] MHz -> 200 kHz step
        n = fs.ticks(1100.0, hz, labels, 128);
        CHECK(n == 11);
        CHECK(std::strcmp(labels[0], "-1.0 MHz") == 0);
        CHECK(std::strcmp(labels[5], "0.0 MHz") == 0);
        CHECK(std::strcmp(labels[10], "1.0 MHz") == 0);
        CHECK_NEAR(hz[5], 0.0, 0.0);

        // Cap: writes exactly `cap` entries, from the low end, ascending;
        // the slots past the cap stay untouched.
        fs.setSpan(100e6, 10e6);
        hz[5] = 777.0;
        std::strcpy(labels[5], "sentinel");
        n = fs.ticks(1100.0, hz, labels, 5);
        CHECK(n == 5);
        for (int i = 0; i < n; ++i) {
            CHECK_NEAR(hz[i], 95e6 + 1e6 * i, 1e-3);
        }
        CHECK_NEAR(hz[5], 777.0, 0.0);
        CHECK(std::strcmp(labels[5], "sentinel") == 0);

        // Guard rails.
        CHECK(fs.ticks(0.0, hz, labels, 128) == 0);
        CHECK(fs.ticks(-100.0, hz, labels, 128) == 0);
        CHECK(fs.ticks(qnan, hz, labels, 128) == 0);
        CHECK(fs.ticks(1000.0, nullptr, labels, 128) == 0);
        CHECK(fs.ticks(1000.0, hz, nullptr, 128) == 0);
        CHECK(fs.ticks(1000.0, hz, labels, 0) == 0);
        FreqScale inert;
        CHECK(inert.ticks(1000.0, hz, labels, 128) == 0);
    }

    // ------------------------------------------------------------------
    // Tick properties under random spans/zooms: exact step multiples, step
    // from {1,2,5}*10^k and matching the documented smallest-fitting rule,
    // label pitch >= 104 px (FreqScale's kMinTickSpacingPx, sized for the
    // widest label at the current lettering plus an end label's slide), every
    // tick inside the view, count <= cap, and the view invariant holds after
    // every mutation.
    // ------------------------------------------------------------------
    {
        for (int iter = 0; iter < 50; ++iter) {
            FreqScale fs;
            const double center = 1e5 + next01() * 2e9;
            const double rate = 1e3 * std::pow(10.0, next01() * 5.0);
            const double width = 200.0 + next01() * 3800.0;
            fs.setSpan(center, rate);
            fs.zoomAt(next01(), 0.5 + next01() * 7.0);
            fs.pan(next01() - 0.5);

            // View invariant after arbitrary zoom + pan.
            const double span = fs.viewHighHz() - fs.viewLowHz();
            const double tolAbs = 1e-9 * rate;
            CHECK(fs.viewLowHz() >= (center - rate / 2.0) - tolAbs);
            CHECK(fs.viewHighHz() <= (center + rate / 2.0) + tolAbs);
            CHECK(span >= rate / 1000.0 * (1.0 - 1e-9));
            CHECK(span <= rate * (1.0 + 1e-9));

            double hz[128];
            char labels[128][16];
            const int n = fs.ticks(width, hz, labels, 128);
            CHECK(n <= 128);
            if (n == 0) { continue; }

            const double step = refNiceStep(span * 104.0 / width);
            CHECK(isNiceStep(step));
            for (int i = 0; i < n; ++i) {
                // Inside the view (to FP noise) and an exact step multiple.
                CHECK(hz[i] >= fs.viewLowHz() - tolAbs);
                CHECK(hz[i] <= fs.viewHighHz() + tolAbs);
                const double r = hz[i] / step;
                CHECK(std::fabs(r - std::floor(r + 0.5)) < 1e-6);
                if (i > 0) {
                    // Consecutive pitch >= 104 px (to FP noise), and no
                    // skipped multiples.
                    CHECK((hz[i] - hz[i - 1]) * width / span >= 104.0 * (1.0 - 1e-9));
                    CHECK_NEAR(hz[i] - hz[i - 1], step, step * 1e-9);
                }
            }
            // Completeness: every multiple inside the view is emitted
            // (count matches the brute-force interval count).
            const int expect = static_cast<int>(std::floor(fs.viewHighHz() / step + 1e-9)) -
                               static_cast<int>(std::ceil(fs.viewLowHz() / step - 1e-9)) + 1;
            CHECK(n == expect);
        }
    }

    // ------------------------------------------------------------------
    // setSpan: preserves a still-valid zoom window, resets an invalid one
    // (outside the new span OR below the new minimum span).
    // ------------------------------------------------------------------
    {
        FreqScale fs;
        fs.setSpan(100e6, 10e6);
        fs.zoomAt(0.5, 4.0);  // view [98.75, 101.25] MHz

        // Small retune: the old view fits in [96, 106] MHz -> preserved.
        fs.setSpan(101e6, 10e6);
        CHECK_NEAR(fs.viewLowHz(), 98.75e6, 1e-3);
        CHECK_NEAR(fs.viewHighHz(), 101.25e6, 1e-3);

        // Big retune: the old view is outside [195, 205] MHz -> reset.
        fs.setSpan(200e6, 10e6);
        CHECK_NEAR(fs.viewLowHz(), 195e6, 1e-3);
        CHECK_NEAR(fs.viewHighHz(), 205e6, 1e-3);

        // Rate jump: old min-span view (10 kHz) is inside the new full span
        // but below the new floor (100 kHz) -> reset, keeping the invariant.
        fs.setSpan(100e6, 10e6);
        fs.zoomAt(0.5, 1e9);  // span clamps to 10 kHz
        fs.setSpan(100e6, 100e6);
        CHECK_NEAR(fs.viewLowHz(), 50e6, 1e-3);
        CHECK_NEAR(fs.viewHighHz(), 150e6, 1e-3);

        // resetView returns to the full span after zoom + pan.
        fs.zoomAt(0.2, 8.0);
        fs.pan(0.3);
        fs.resetView();
        CHECK_NEAR(fs.viewLowHz(), 50e6, 1e-3);
        CHECK_NEAR(fs.viewHighHz(), 150e6, 1e-3);

        // Invalid rate collapses to the inert state instead of storing a
        // poisoned span; transforms stay finite and ticks stay silent.
        fs.setSpan(1e6, -5.0);
        CHECK_NEAR(fs.viewLowHz(), fs.viewHighHz(), 0.0);
        CHECK_NEAR(fs.xToHz(0.5), 1e6, 1e-6);
        CHECK_NEAR(fs.hzToX(2e6), 0.0, 0.0);
        double hz2[4];
        char labels2[4][16];
        CHECK(fs.ticks(1000.0, hz2, labels2, 4) == 0);
        fs.setSpan(qnan, 1e6);  // NaN center is inert too
        double f1 = -1.0, f2 = -1.0;
        fs.visibleBinRange(512, f1, f2);
        CHECK_NEAR(f1, 0.0, 0.0);
        CHECK_NEAR(f2, 511.0, 0.0);

        // Inert default: mutators no-op without crashing.
        FreqScale inert;
        inert.zoomAt(0.5, 2.0);
        inert.pan(1.0);
        inert.resetView();
        CHECK_NEAR(inert.xToHz(0.5), 0.0, 0.0);
        CHECK_NEAR(inert.hzToX(1e6), 0.0, 0.0);
    }

    return testSummary("test_freq_scale");
}
