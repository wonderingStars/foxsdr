// Tests for gui/spectrum_view.hpp — pure display math only. draw() needs a
// live ImGui/GL context, so the testable surface is dbToY, gridlineDbs, and
// the zoom/VFO statics binToXFrac and hitTest; nothing here touches ImGui.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/spectrum_view.hpp"

#include <cmath>
#include <cstdint>

#include "test_check.hpp"

using cascade::gui::SpectrumView;
using cascade::gui::dbToY;

namespace {

// Independent reference for the linear map, computed in double from the
// definition (dbMax -> yTop, dbMin -> yBottom, clamped) rather than from the
// implementation.
double refDbToY(double db, double dbMin, double dbMax, double yTop, double yBottom) {
    double t = (dbMax - db) / (dbMax - dbMin);
    if (t < 0.0) { t = 0.0; }
    if (t > 1.0) { t = 1.0; }
    return yTop + t * (yBottom - yTop);
}

// Independent reference for the gridline set: brute-force scan of every
// multiple of 10 in a generous window, boundary-inclusive.
int refGridlines(float dbMin, float dbMax, float* out) {
    int count = 0;
    for (int g = -1000; g <= 1000; g += 10) {
        const float gf = static_cast<float>(g);
        if (gf >= dbMin && gf <= dbMax) { out[count++] = gf; }
    }
    return count;
}

}  // namespace

int main() {
    // --- dbToY endpoints: exact, both screen-style (yTop < yBottom) and
    // math-style (yTop > yBottom) orientations.
    CHECK_NEAR(dbToY(0.0f, -100.0f, 0.0f, 10.0f, 210.0f), 10.0, 1e-9);
    CHECK_NEAR(dbToY(-100.0f, -100.0f, 0.0f, 10.0f, 210.0f), 210.0, 1e-9);
    CHECK_NEAR(dbToY(0.0f, -100.0f, 0.0f, 500.0f, 100.0f), 500.0, 1e-9);
    CHECK_NEAR(dbToY(-100.0f, -100.0f, 0.0f, 500.0f, 100.0f), 100.0, 1e-9);

    // --- Midpoint exact: -50 dB in [-100, 0] is halfway down.
    CHECK_NEAR(dbToY(-50.0f, -100.0f, 0.0f, 10.0f, 210.0f), 110.0, 1e-9);

    // --- Clamping both sides: outside values pin to the matching edge.
    CHECK_NEAR(dbToY(25.0f, -100.0f, 0.0f, 10.0f, 210.0f), 10.0, 1e-9);
    CHECK_NEAR(dbToY(-180.0f, -100.0f, 0.0f, 10.0f, 210.0f), 210.0, 1e-9);

    // --- Degenerate ranges must not divide by zero. Documented behavior:
    // dbMin >= dbMax (and NaN endpoints) return yBottom for every input.
    CHECK_NEAR(dbToY(-50.0f, -40.0f, -40.0f, 10.0f, 210.0f), 210.0, 1e-9);
    CHECK_NEAR(dbToY(-40.0f, -40.0f, -40.0f, 10.0f, 210.0f), 210.0, 1e-9);
    CHECK_NEAR(dbToY(-50.0f, 0.0f, -100.0f, 10.0f, 210.0f), 210.0, 1e-9);   // inverted
    CHECK_NEAR(dbToY(999.0f, 0.0f, -100.0f, 10.0f, 210.0f), 210.0, 1e-9);   // inverted + above
    const float qnan = std::nanf("");
    CHECK_NEAR(dbToY(-50.0f, qnan, 0.0f, 10.0f, 210.0f), 210.0, 1e-9);
    CHECK_NEAR(dbToY(-50.0f, -100.0f, qnan, 10.0f, 210.0f), 210.0, 1e-9);
    // NaN db with a VALID range: a poisoned bin drops to the floor (yBottom),
    // never leaking NaN into draw coordinates.
    CHECK_NEAR(dbToY(qnan, -100.0f, 0.0f, 10.0f, 210.0f), 210.0, 1e-9);
    CHECK(std::isfinite(dbToY(qnan, -100.0f, 0.0f, 10.0f, 210.0f)));

    // --- Linearity sweep against the in-test reference, fixed-seed LCG.
    std::uint32_t lcg = 0xC0FFEE01u;
    auto next01 = [&lcg]() {
        lcg = lcg * 1664525u + 1013904223u;
        return static_cast<double>(lcg >> 8) / 16777216.0;
    };
    for (int i = 0; i < 200; ++i) {
        const float db = static_cast<float>(-160.0 + 200.0 * next01());  // spans past both edges
        const float got = dbToY(db, -120.0f, -20.0f, 33.0f, 477.0f);
        const double want = refDbToY(db, -120.0, -20.0, 33.0, 477.0);
        CHECK_NEAR(got, want, 1e-3);
    }

    // --- gridlineDbs against the brute-force reference across assorted
    // ranges: exact boundaries, offset boundaries, straddling zero, tiny
    // ranges containing zero or one line.
    struct Range {
        float lo, hi;
    };
    const Range ranges[] = {
        {-100.0f, 0.0f},   // both boundaries are lines
        {-95.0f, -5.0f},   // neither boundary is a line
        {-25.0f, 25.0f},   // straddles zero
        {-90.0f, -90.0f},  // single-point range ON a line
        {-89.5f, -80.5f},  // interval containing no multiple of 10
        {0.0f, 130.0f},    // positive-only
    };
    for (const Range& r : ranges) {
        float got[256];
        float want[256];
        const int gotN = SpectrumView::gridlineDbs(r.lo, r.hi, got, 256);
        const int wantN = refGridlines(r.lo, r.hi, want);
        CHECK(gotN == wantN);
        for (int i = 0; i < gotN && i < wantN; ++i) { CHECK_NEAR(got[i], want[i], 0.0); }
    }

    // Spot-check the canonical range explicitly (not only via the reference):
    // [-100, 0] must be exactly -100, -90, ..., 0 — 11 lines, ascending.
    {
        float out[32];
        const int n = SpectrumView::gridlineDbs(-100.0f, 0.0f, out, 32);
        CHECK(n == 11);
        for (int i = 0; i < n; ++i) { CHECK_NEAR(out[i], -100.0f + 10.0f * static_cast<float>(i), 0.0); }
    }

    // --- Cap respected: only `cap` values written, in ascending order from
    // the bottom of the range; the slot past the cap stays untouched.
    {
        float out[8] = {0, 0, 0, 0, 0, 12345.0f, 0, 0};
        const int n = SpectrumView::gridlineDbs(-100.0f, 0.0f, out, 5);
        CHECK(n == 5);
        for (int i = 0; i < n; ++i) { CHECK_NEAR(out[i], -100.0f + 10.0f * static_cast<float>(i), 0.0); }
        CHECK_NEAR(out[5], 12345.0f, 0.0);  // sentinel untouched
    }

    // --- Guard rails: null output, non-positive cap, degenerate range.
    {
        float out[4];
        CHECK(SpectrumView::gridlineDbs(-100.0f, 0.0f, nullptr, 8) == 0);
        CHECK(SpectrumView::gridlineDbs(-100.0f, 0.0f, out, 0) == 0);
        CHECK(SpectrumView::gridlineDbs(-100.0f, 0.0f, out, -3) == 0);
        CHECK(SpectrumView::gridlineDbs(0.0f, -100.0f, out, 4) == 0);   // inverted
        CHECK(SpectrumView::gridlineDbs(qnan, 0.0f, out, 4) == 0);      // NaN bound
    }

    // ===================== zoom + VFO additions =============================

    using VfoBand = SpectrumView::VfoBand;
    using VfoHit = SpectrumView::VfoHit;

    // --- hitTest full decision matrix. Every expectation is derived from
    // the header contract, and band/tolerance values are picked binary-exact
    // (multiples of 2^-4 and 2^-5) so "distance exactly equals tolerance"
    // rows carry no double rounding dust.
    {
        struct HitCase {
            double mouse;
            double x0, x1;
            double tol;
            VfoHit want;
            const char* why;
        };
        const HitCase cases[] = {
            // Ordinary band [0.25, 0.75], tolerance 1/16.
            {0.5, 0.25, 0.75, 0.0625, VfoHit::Center, "deep inside -> Center"},
            {0.375, 0.25, 0.75, 0.0625, VfoHit::Center, "inside, beyond edge tol -> Center"},
            {0.625, 0.25, 0.75, 0.0625, VfoHit::Center, "inside, beyond edge tol -> Center"},
            {0.25, 0.25, 0.75, 0.0625, VfoHit::EdgeLow, "exactly on low edge"},
            {0.75, 0.25, 0.75, 0.0625, VfoHit::EdgeHigh, "exactly on high edge"},
            {0.1875, 0.25, 0.75, 0.0625, VfoHit::EdgeLow, "outside-left at exact tol (inclusive)"},
            {0.8125, 0.25, 0.75, 0.0625, VfoHit::EdgeHigh, "outside-right at exact tol (inclusive)"},
            {0.3125, 0.25, 0.75, 0.0625, VfoHit::EdgeLow, "inside at exact tol: edge beats center"},
            {0.6875, 0.25, 0.75, 0.0625, VfoHit::EdgeHigh, "inside at exact tol: edge beats center"},
            {0.28, 0.25, 0.75, 0.0625, VfoHit::EdgeLow, "inside within tol: edge beats center"},
            {0.72, 0.25, 0.75, 0.0625, VfoHit::EdgeHigh, "inside within tol: edge beats center"},
            {0.15, 0.25, 0.75, 0.0625, VfoHit::None, "outside-left beyond tol"},
            {0.85, 0.25, 0.75, 0.0625, VfoHit::None, "outside-right beyond tol"},
            {0.0, 0.25, 0.75, 0.0625, VfoHit::None, "far left of band"},
            {1.0, 0.25, 0.75, 0.0625, VfoHit::None, "far right of band"},
            // Inverted band behaves exactly like the ordered one.
            {0.5, 0.75, 0.25, 0.0625, VfoHit::Center, "inverted band: Center"},
            {0.28, 0.75, 0.25, 0.0625, VfoHit::EdgeLow, "inverted band: low edge"},
            {0.72, 0.75, 0.25, 0.0625, VfoHit::EdgeHigh, "inverted band: high edge"},
            {0.85, 0.75, 0.25, 0.0625, VfoHit::None, "inverted band: outside"},
            // Narrow band [7/16, 9/16] with tolerance wider than the band:
            // both edges are in tolerance everywhere, nearer edge must win.
            {0.45, 0.4375, 0.5625, 0.25, VfoHit::EdgeLow, "both in tol: nearer edge is low"},
            {0.55, 0.4375, 0.5625, 0.25, VfoHit::EdgeHigh, "both in tol: nearer edge is high"},
            {0.5, 0.4375, 0.5625, 0.25, VfoHit::EdgeHigh, "exact tie inside: mouse > lo -> high"},
            // Degenerate zero-width band: side of approach picks the edge.
            {0.5, 0.5, 0.5, 0.0625, VfoHit::EdgeLow, "zero-width, on the point -> low"},
            {0.46875, 0.5, 0.5, 0.0625, VfoHit::EdgeLow, "zero-width, from the left -> low"},
            {0.53125, 0.5, 0.5, 0.0625, VfoHit::EdgeHigh, "zero-width, from the right -> high"},
            {0.4, 0.5, 0.5, 0.0625, VfoHit::None, "zero-width, beyond tol left"},
            {0.6, 0.5, 0.5, 0.0625, VfoHit::None, "zero-width, beyond tol right"},
            // Zero tolerance: exact hits only, Center still works.
            {0.5, 0.5, 0.5, 0.0, VfoHit::EdgeLow, "zero-width + zero tol, exact hit"},
            {0.25, 0.25, 0.75, 0.0, VfoHit::EdgeLow, "zero tol, exact low edge"},
            {0.75, 0.25, 0.75, 0.0, VfoHit::EdgeHigh, "zero tol, exact high edge"},
            {0.5, 0.25, 0.75, 0.0, VfoHit::Center, "zero tol, inside -> Center"},
            {0.2, 0.25, 0.75, 0.0, VfoHit::None, "zero tol, outside -> None"},
            // Negative tolerance is treated as zero, not as a rejection.
            {0.3, 0.25, 0.75, -0.5, VfoHit::Center, "negative tol == 0: inside is Center"},
            {0.25, 0.25, 0.75, -0.5, VfoHit::EdgeLow, "negative tol == 0: exact edge hits"},
        };
        const int caseCount = static_cast<int>(sizeof(cases) / sizeof(cases[0]));
        for (int i = 0; i < caseCount; ++i) {
            const HitCase& c = cases[i];
            const VfoBand band{c.x0, c.x1, false};
            const VfoHit got = SpectrumView::hitTest(c.mouse, band, c.tol);
            if (got != c.want) {
                std::printf("  hitTest case %d: %s (mouse=%g band=[%g,%g] tol=%g)\n",
                            i, c.why, c.mouse, c.x0, c.x1, c.tol);
            }
            CHECK(got == c.want);
        }

        // dragging is caller-owned display state; it must never change the
        // classification.
        const VfoBand dragBand{0.25, 0.75, true};
        CHECK(SpectrumView::hitTest(0.5, dragBand, 0.0625) == VfoHit::Center);
        CHECK(SpectrumView::hitTest(0.25, dragBand, 0.0625) == VfoHit::EdgeLow);

        // NaN anywhere must classify as None (a poisoned mouse or band can
        // never produce a phantom grab); NaN tolerance reads as 0.
        const VfoBand okBand{0.25, 0.75, false};
        CHECK(SpectrumView::hitTest(static_cast<double>(qnan), okBand, 0.0625) == VfoHit::None);
        const VfoBand nanLo{static_cast<double>(qnan), 0.75, false};
        CHECK(SpectrumView::hitTest(0.5, nanLo, 0.0625) == VfoHit::None);
        const VfoBand nanHi{0.25, static_cast<double>(qnan), false};
        CHECK(SpectrumView::hitTest(0.5, nanHi, 0.0625) == VfoHit::None);
        CHECK(SpectrumView::hitTest(0.5, okBand, static_cast<double>(qnan)) == VfoHit::Center);
        CHECK(SpectrumView::hitTest(0.25, okBand, static_cast<double>(qnan)) == VfoHit::EdgeLow);
    }

    // --- binToXFrac: identity for the full range. draw() lays bin i of n at
    // x fraction i/(n-1); the windowed map must reproduce that exactly for
    // the window [0, n-1] or zoomed and unzoomed traces would jump.
    {
        for (int i = 0; i < 16; ++i) {
            CHECK_NEAR(SpectrumView::binToXFrac(static_cast<double>(i), 0.0, 15.0),
                       static_cast<double>(i) / 15.0, 1e-7);
        }
        CHECK_NEAR(SpectrumView::binToXFrac(0.0, 0.0, 15.0), 0.0, 0.0);
        CHECK_NEAR(SpectrumView::binToXFrac(15.0, 0.0, 15.0), 1.0, 0.0);
        CHECK_NEAR(SpectrumView::binToXFrac(7.5, 0.0, 15.0), 0.5, 1e-7);
    }

    // --- binToXFrac: half-range window [4, 12] of a 16-bin spectrum, plus a
    // fractional-edge window. All expectations are exact binary fractions.
    {
        CHECK_NEAR(SpectrumView::binToXFrac(4.0, 4.0, 12.0), 0.0, 0.0);
        CHECK_NEAR(SpectrumView::binToXFrac(12.0, 4.0, 12.0), 1.0, 0.0);
        CHECK_NEAR(SpectrumView::binToXFrac(8.0, 4.0, 12.0), 0.5, 0.0);
        CHECK_NEAR(SpectrumView::binToXFrac(6.0, 4.0, 12.0), 0.25, 0.0);
        CHECK_NEAR(SpectrumView::binToXFrac(10.0, 4.0, 12.0), 0.75, 0.0);
        // Unclamped by contract: a result outside [0, 1] reports where an
        // out-of-window bin went instead of silently pinning it to an edge.
        CHECK_NEAR(SpectrumView::binToXFrac(0.0, 4.0, 12.0), -0.5, 0.0);
        CHECK_NEAR(SpectrumView::binToXFrac(16.0, 4.0, 12.0), 1.5, 0.0);
        // Fractional window cut positions (where edge interpolation lands).
        CHECK_NEAR(SpectrumView::binToXFrac(2.5, 2.5, 5.5), 0.0, 0.0);
        CHECK_NEAR(SpectrumView::binToXFrac(5.5, 2.5, 5.5), 1.0, 0.0);
        CHECK_NEAR(SpectrumView::binToXFrac(4.0, 2.5, 5.5), 0.5, 0.0);
        CHECK_NEAR(SpectrumView::binToXFrac(3.25, 2.5, 5.5), 0.25, 0.0);
        // Degenerate windows: zero-width, inverted, NaN -> 0 by contract.
        CHECK_NEAR(SpectrumView::binToXFrac(3.0, 5.0, 5.0), 0.0, 0.0);
        CHECK_NEAR(SpectrumView::binToXFrac(3.0, 7.0, 2.0), 0.0, 0.0);
        CHECK_NEAR(SpectrumView::binToXFrac(3.0, static_cast<double>(qnan), 12.0), 0.0, 0.0);
        CHECK_NEAR(SpectrumView::binToXFrac(3.0, 4.0, static_cast<double>(qnan)), 0.0, 0.0);
    }

    // --- binToXFrac linearity sweep against the defining formula, random
    // windows and bins (including out-of-window bins), fixed-seed LCG.
    for (int i = 0; i < 200; ++i) {
        const double first = -50.0 + 100.0 * next01();
        const double span = 0.5 + 99.5 * next01();
        const double bin = first - 10.0 + (span + 20.0) * next01();
        const double want = (bin - first) / span;
        CHECK_NEAR(SpectrumView::binToXFrac(bin, first, first + span), want, 1e-5);
    }

    return testSummary("test_spectrum_view");
}
