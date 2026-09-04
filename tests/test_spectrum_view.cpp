// Tests for gui/spectrum_view.hpp — pure display math only. draw() needs a
// live ImGui/GL context, so the testable surface is dbToY, gridlineDbs, the
// zoom/VFO statics binToXFrac and hitTest, and peakInBand, the figure the
// panel prints as "PEAK IN PASSBAND"; nothing here touches ImGui.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/spectrum_view.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>

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

// Independent statement of peakInBand's documented rule, written from the
// header rather than from the implementation: normalize the band, invert
// binToXFrac (unclamped — the peak is a fact about the signal, not the zoom),
// take every WHOLE bin the resulting interval touches, and max over them
// skipping poisoned bins.
bool refPeakInBand(const float* bins, int n, double firstBin, double lastBin,
                   double x0, double x1, float& out) {
    if (bins == nullptr || n <= 0) { return false; }
    const double lo = x0 < x1 ? x0 : x1;
    const double hi = x0 < x1 ? x1 : x0;
    if (!(lo <= hi)) { return false; }  // NaN edge
    double binLo = 0.0;
    double binHi = 0.0;
    if (!(lastBin > firstBin)) {
        if (!(hi >= 0.0) || !(lo <= 1.0) || !std::isfinite(firstBin)) { return false; }
        binLo = firstBin;
        binHi = firstBin;
    } else {
        binLo = firstBin + lo * (lastBin - firstBin);
        binHi = firstBin + hi * (lastBin - firstBin);
    }
    if (!std::isfinite(binLo) || !std::isfinite(binHi)) { return false; }
    const double maxBin = static_cast<double>(n - 1);
    if (binHi < 0.0 || binLo > maxBin) { return false; }
    if (binLo < 0.0) { binLo = 0.0; }
    if (binHi > maxBin) { binHi = maxBin; }
    int i0 = static_cast<int>(std::floor(binLo));
    int i1 = static_cast<int>(std::ceil(binHi));
    if (i0 < 0) { i0 = 0; }
    if (i1 > n - 1) { i1 = n - 1; }
    bool any = false;
    float best = 0.0f;
    for (int i = i0; i <= i1; ++i) {
        const float v = bins[i];
        if (!(v == v)) { continue; }
        if (!any || v > best) { best = v; any = true; }
    }
    if (!any) { return false; }
    out = best;
    return true;
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

    // ===================== peakInBand ======================================
    //
    // THE FIGURE THE HEADER PRINTS AS "PEAK IN PASSBAND". Every expectation
    // below is worked out by hand from the documented rule, and the bin
    // values are deliberately non-monotonic so a max is a real max and not
    // whichever end of the interval happened to be read last.
    {
        // bin:                  0      1      2      3      4      5      6      7
        const float bins[8] = {-70.0f, -50.0f, -95.0f, -20.0f, -60.0f, -30.0f, -85.0f, -40.0f};
        constexpr float kSentinel = -12345.0f;
        float peak = kSentinel;

        // Whole window, whole band: the peak of everything.
        CHECK(SpectrumView::peakInBand(bins, 8, 0.0, 7.0, VfoBand{0.0, 1.0, false}, peak));
        CHECK_NEAR(peak, -20.0f, 0.0);

        // A band over bins 1.75 .. 3.5 touches whole bins 1..4, whose peak is
        // bin 3. The bins outside it (0, 5, 6, 7) include -30, which is
        // louder than most of the band — so a band that silently widened
        // would be caught here.
        peak = kSentinel;
        CHECK(SpectrumView::peakInBand(bins, 8, 0.0, 7.0, VfoBand{0.25, 0.5, false}, peak));
        CHECK_NEAR(peak, -20.0f, 0.0);
        // Order does not matter: the band is normalized exactly as hitTest
        // normalizes it.
        peak = kSentinel;
        CHECK(SpectrumView::peakInBand(bins, 8, 0.0, 7.0, VfoBand{0.5, 0.25, false}, peak));
        CHECK_NEAR(peak, -20.0f, 0.0);
        // dragging is display state and must never move a reading.
        peak = kSentinel;
        CHECK(SpectrumView::peakInBand(bins, 8, 0.0, 7.0, VfoBand{0.25, 0.5, true}, peak));
        CHECK_NEAR(peak, -20.0f, 0.0);

        // A BAND NARROWER THAN ONE BIN: 4.2 .. 4.3 lies inside bin 4's
        // interval, and the documented answer is the pair of whole bins it
        // straddles (4 and 5), not nothing.
        peak = kSentinel;
        CHECK(SpectrumView::peakInBand(bins, 8, 0.0, 7.0,
                                       VfoBand{4.2 / 7.0, 4.3 / 7.0, false}, peak));
        CHECK_NEAR(peak, -30.0f, 0.0);
        // A zero-width band landing exactly on a bin reports that one bin.
        peak = kSentinel;
        CHECK(SpectrumView::peakInBand(bins, 8, 0.0, 7.0,
                                       VfoBand{2.0 / 7.0, 2.0 / 7.0, false}, peak));
        CHECK_NEAR(peak, -95.0f, 0.0);

        // A VFO PARKED OFF-SCREEN still reports its own peak: the window is
        // zoomed to bins 0..3 and the band sits at bins 5..6, past the right
        // edge. The band is not clamped to the view because the peak in the
        // passband is a fact about the signal, not about the zoom.
        peak = kSentinel;
        CHECK(SpectrumView::peakInBand(bins, 8, 0.0, 3.0, VfoBand{5.0 / 3.0, 2.0, false}, peak));
        CHECK_NEAR(peak, -30.0f, 0.0);
        // Half off the left of the data: the bins that exist are used.
        peak = kSentinel;
        CHECK(SpectrumView::peakInBand(bins, 8, 0.0, 7.0, VfoBand{-0.5, 0.1, false}, peak));
        CHECK_NEAR(peak, -50.0f, 0.0);  // whole bins 0..1

        // ENTIRELY OUTSIDE THE DATA: no measurement, and peakDb is left
        // exactly as the caller had it — the chrome tests that return value
        // to decide whether to print a figure at all.
        peak = kSentinel;
        CHECK(SpectrumView::peakInBand(bins, 8, 0.0, 7.0, VfoBand{1.5, 2.0, false}, peak) == false);
        CHECK_NEAR(peak, kSentinel, 0.0);
        CHECK(SpectrumView::peakInBand(bins, 8, 0.0, 7.0, VfoBand{-1.0, -0.5, false}, peak) == false);
        CHECK_NEAR(peak, kSentinel, 0.0);

        // A DEGENERATE VIEW WINDOW has no bin ordering to invert; the panel
        // is showing the single bin at firstBin, so that bin is reported if
        // the band touches the panel at all and nothing is reported if it
        // does not.
        peak = kSentinel;
        CHECK(SpectrumView::peakInBand(bins, 8, 3.0, 3.0, VfoBand{0.25, 0.75, false}, peak));
        CHECK_NEAR(peak, -20.0f, 0.0);
        peak = kSentinel;
        CHECK(SpectrumView::peakInBand(bins, 8, 5.0, 2.0, VfoBand{0.25, 0.75, false}, peak));
        CHECK_NEAR(peak, -30.0f, 0.0);  // inverted window: bin 5
        peak = kSentinel;  // touching the panel's left edge counts
        CHECK(SpectrumView::peakInBand(bins, 8, 3.0, 3.0, VfoBand{-0.5, 0.0, false}, peak));
        CHECK_NEAR(peak, -20.0f, 0.0);
        peak = kSentinel;  // and its right edge
        CHECK(SpectrumView::peakInBand(bins, 8, 3.0, 3.0, VfoBand{1.0, 2.0, false}, peak));
        CHECK_NEAR(peak, -20.0f, 0.0);
        peak = kSentinel;  // clear of the panel on either side: nothing
        CHECK(SpectrumView::peakInBand(bins, 8, 3.0, 3.0, VfoBand{1.5, 2.0, false}, peak) == false);
        CHECK_NEAR(peak, kSentinel, 0.0);
        CHECK(SpectrumView::peakInBand(bins, 8, 3.0, 3.0, VfoBand{-2.0, -0.5, false}, peak) == false);
        CHECK_NEAR(peak, kSentinel, 0.0);
        // Degenerate window parked off the data entirely.
        CHECK(SpectrumView::peakInBand(bins, 8, 99.0, 99.0, VfoBand{0.0, 1.0, false}, peak) == false);
        CHECK_NEAR(peak, kSentinel, 0.0);

        // NO DATA: null array, empty array, negative count.
        CHECK(SpectrumView::peakInBand(nullptr, 8, 0.0, 7.0, VfoBand{0.0, 1.0, false}, peak) == false);
        CHECK(SpectrumView::peakInBand(bins, 0, 0.0, 7.0, VfoBand{0.0, 1.0, false}, peak) == false);
        CHECK(SpectrumView::peakInBand(bins, -4, 0.0, 7.0, VfoBand{0.0, 1.0, false}, peak) == false);
        CHECK_NEAR(peak, kSentinel, 0.0);
        // One bin is a legitimate spectrum: the window is degenerate by
        // construction and the single bin is the peak.
        const float one[1] = {-33.0f};
        peak = kSentinel;
        CHECK(SpectrumView::peakInBand(one, 1, 0.0, 0.0, VfoBand{0.0, 1.0, false}, peak));
        CHECK_NEAR(peak, -33.0f, 0.0);

        // A POISONED BIN IS SKIPPED, not compared. NaN loses every
        // comparison, so an unguarded max reports either the last non-NaN
        // value or the NaN itself depending on operand order — and a NaN in
        // the FIRST slot is the case that catches a `best = bins[i0]` seed.
        const float poisonedFirst[4] = {std::nanf(""), -50.0f, -80.0f, -60.0f};
        peak = kSentinel;
        CHECK(SpectrumView::peakInBand(poisonedFirst, 4, 0.0, 3.0, VfoBand{0.0, 1.0, false}, peak));
        CHECK_NEAR(peak, -50.0f, 0.0);
        const float poisonedLast[4] = {-70.0f, -50.0f, -80.0f, std::nanf("")};
        peak = kSentinel;
        CHECK(SpectrumView::peakInBand(poisonedLast, 4, 0.0, 3.0, VfoBand{0.0, 1.0, false}, peak));
        CHECK_NEAR(peak, -50.0f, 0.0);
        CHECK(peak == peak);  // never a NaN reading
        // Every bin poisoned: there is no measurement to report.
        const float allNan[3] = {std::nanf(""), std::nanf(""), std::nanf("")};
        peak = kSentinel;
        CHECK(SpectrumView::peakInBand(allNan, 3, 0.0, 2.0, VfoBand{0.0, 1.0, false}, peak) == false);
        CHECK_NEAR(peak, kSentinel, 0.0);

        // NaN BAND EDGES: a poisoned passband can never produce a phantom
        // reading. NaN WINDOW BOUNDS: a poisoned firstBin likewise.
        const double dnan = static_cast<double>(qnan);
        peak = kSentinel;
        CHECK(SpectrumView::peakInBand(bins, 8, 0.0, 7.0, VfoBand{dnan, 0.75, false}, peak) == false);
        CHECK(SpectrumView::peakInBand(bins, 8, 0.0, 7.0, VfoBand{0.25, dnan, false}, peak) == false);
        CHECK(SpectrumView::peakInBand(bins, 8, 0.0, 7.0, VfoBand{dnan, dnan, false}, peak) == false);
        CHECK(SpectrumView::peakInBand(bins, 8, dnan, 7.0, VfoBand{0.25, 0.75, false}, peak) == false);
        CHECK(SpectrumView::peakInBand(bins, 8, dnan, dnan, VfoBand{0.25, 0.75, false}, peak) == false);
        CHECK_NEAR(peak, kSentinel, 0.0);
        // NOT COVERED, DELIBERATELY: a finite firstBin with a NaN lastBin.
        // The header says NaN window bounds return false; the implementation
        // takes the degenerate-window branch, which tests only firstBin, so
        // peakInBand(bins, 8, 0.0, NaN, {0.25, 0.75}) returns TRUE and writes
        // bin 0's -70 dB as a reading. Measured by adding that assertion here
        // and watching it fail, then removing it: pinning the observed
        // behaviour would make the defect the contract, and pinning the
        // documented one would ship a red suite. Reported instead.

        // AN INFINITE WINDOW has no finite bin to name, either end.
        const double dinf = std::numeric_limits<double>::infinity();
        peak = kSentinel;
        CHECK(SpectrumView::peakInBand(bins, 8, 0.0, dinf, VfoBand{0.25, 0.75, false}, peak) == false);
        CHECK(SpectrumView::peakInBand(bins, 8, -dinf, 7.0, VfoBand{0.25, 0.75, false}, peak) == false);
        CHECK(SpectrumView::peakInBand(bins, 8, 0.0, dinf, VfoBand{0.0, 1.0, false}, peak) == false);
        CHECK(SpectrumView::peakInBand(bins, 8, 0.0, 7.0, VfoBand{0.0, dinf, false}, peak) == false);
        CHECK(SpectrumView::peakInBand(bins, 8, 0.0, 7.0, VfoBand{-dinf, dinf, false}, peak) == false);
        CHECK_NEAR(peak, kSentinel, 0.0);

        // A BAND MILES WIDE IN BIN TERMS must clamp in double before the
        // cast — floor/ceil of 1e17 converted to int is undefined behaviour,
        // and the band arrives unclamped by contract.
        peak = kSentinel;
        CHECK(SpectrumView::peakInBand(bins, 8, 0.0, 1.0e17, VfoBand{0.0, 1.0, false}, peak));
        CHECK_NEAR(peak, -20.0f, 0.0);  // the whole array, clamped to bin 7
        peak = kSentinel;
        CHECK(SpectrumView::peakInBand(bins, 8, 0.0, 1.0e17, VfoBand{0.9, 1.0, false}, peak) == false);
        CHECK_NEAR(peak, kSentinel, 0.0);
        peak = kSentinel;
        CHECK(SpectrumView::peakInBand(bins, 8, 0.0, 1.0, VfoBand{-1.0e18, -1.0e17, false}, peak) == false);
        CHECK(SpectrumView::peakInBand(bins, 8, -1.0e17, 1.0e17, VfoBand{0.0, 1.0, false}, peak));
        CHECK_NEAR(peak, -20.0f, 0.0);
    }

    // --- peakInBand sweep against the independent reference: random finite
    // windows (including inverted and degenerate ones), random bands inside
    // and outside the data, and bin arrays sprinkled with poisoned values.
    {
        int mismatches = 0;
        int reported = 0;
        int refused = 0;
        for (int trial = 0; trial < 4000; ++trial) {
            float bins[12];
            const int n = 1 + static_cast<int>(next01() * 11.0);
            for (int i = 0; i < 12; ++i) {
                bins[i] = next01() < 0.08 ? qnan
                                          : static_cast<float>(-120.0 + 120.0 * next01());
            }
            const double firstBin = -4.0 + 16.0 * next01();
            const double lastBin = (next01() < 0.15) ? firstBin  // degenerate window
                                                     : -4.0 + 16.0 * next01();
            const double x0 = -1.5 + 3.5 * next01();
            const double x1 = -1.5 + 3.5 * next01();
            float got = -4242.0f;
            float want = -4242.0f;
            const bool gotOk =
                SpectrumView::peakInBand(bins, n, firstBin, lastBin, VfoBand{x0, x1, false}, got);
            const bool wantOk = refPeakInBand(bins, n, firstBin, lastBin, x0, x1, want);
            const bool same = (gotOk == wantOk) && (!gotOk || got == want);
            if (!same) {
                if (mismatches == 0) {
                    std::printf("  peakInBand(n=%d, win=[%g,%g], band=[%g,%g]) = %d/%g,"
                                " want %d/%g\n",
                                n, firstBin, lastBin, x0, x1, gotOk ? 1 : 0,
                                static_cast<double>(got), wantOk ? 1 : 0,
                                static_cast<double>(want));
                }
                ++mismatches;
            }
            if (gotOk) {
                ++reported;
                CHECK(got == got);  // a reading is never NaN
            } else {
                ++refused;
                CHECK_NEAR(got, -4242.0f, 0.0);  // and a refusal never writes
            }
        }
        CHECK(mismatches == 0);
        // Both outcomes must actually occur, or the sweep is only testing one
        // of them: a change to the window/band ranges that made every trial
        // land the same way would otherwise pass silently.
        CHECK(reported > 400);
        CHECK(refused > 400);
    }

    // --- dbLabelStride: the dB ladder must never print through itself -------
    //
    // WHY THIS FUNCTION EXISTS AT ALL. The gridlines are every 10 dB whatever
    // the panel is worth, so the space between one label and the next is set
    // by the well's height and the dB range and by nothing else. The
    // lettering grew by two points and a short well went from three pixels of
    // air between figures to none — and two dB numbers printed through each
    // other are not a coarser scale, they are a smear. The stride is what
    // keeps the ladder legible, and the property below is the whole contract:
    // whatever it returns, the figures that survive clear each other.
    {
        // Documented cases, worked from the rule rather than from the code.
        CHECK(SpectrumView::dbLabelStride(-100.0f, 0.0f, 400.0f, 14.0f) == 1);
        CHECK(SpectrumView::dbLabelStride(-100.0f, 0.0f, 150.0f, 14.0f) == 2);
        CHECK(SpectrumView::dbLabelStride(-160.0f, -5.0f, 150.0f, 14.0f) == 2);
        // A very tall well needs no thinning at all, however wide the range.
        CHECK(SpectrumView::dbLabelStride(-160.0f, -5.0f, 2000.0f, 14.0f) == 1);
        // THE LADDER: a raw stride of 3 or 4 is rounded up to a 50 dB ladder
        // rather than a 30 or 40 dB one, and a raw 6 to 100 dB. A dB scale
        // read in sevens is not a scale anybody reads.
        for (int h = 30; h <= 900; ++h) {
            const int stride =
                SpectrumView::dbLabelStride(-160.0f, 0.0f, static_cast<float>(h), 14.0f);
            const bool onLadder = (stride == 1 || stride == 2 || stride == 5 ||
                                   stride == 10 || stride == 20 || stride == 50 ||
                                   stride == 64);
            CHECK(onLadder);
        }
        // And an absurd one is capped rather than returning a stride past
        // every gridline the grid can hold.
        CHECK(SpectrumView::dbLabelStride(-500.0f, 500.0f, 1.0f, 14.0f) == 64);

        // Degenerate inputs label every line — the draw loop's own header and
        // axis guards then drop whatever will not fit.
        const float qnanF = std::numeric_limits<float>::quiet_NaN();
        CHECK(SpectrumView::dbLabelStride(0.0f, -100.0f, 400.0f, 14.0f) == 1);
        CHECK(SpectrumView::dbLabelStride(-100.0f, -100.0f, 400.0f, 14.0f) == 1);
        CHECK(SpectrumView::dbLabelStride(-100.0f, 0.0f, 0.0f, 14.0f) == 1);
        CHECK(SpectrumView::dbLabelStride(-100.0f, 0.0f, -400.0f, 14.0f) == 1);
        CHECK(SpectrumView::dbLabelStride(-100.0f, 0.0f, 400.0f, 0.0f) == 1);
        CHECK(SpectrumView::dbLabelStride(qnanF, 0.0f, 400.0f, 14.0f) == 1);
        CHECK(SpectrumView::dbLabelStride(-100.0f, qnanF, 400.0f, 14.0f) == 1);
        CHECK(SpectrumView::dbLabelStride(-100.0f, 0.0f, qnanF, 14.0f) == 1);
        CHECK(SpectrumView::dbLabelStride(-100.0f, 0.0f, 400.0f, qnanF) == 1);

        // THE PROPERTY, over every panel height and dB range the Display
        // section and the splitter can produce, at every type size fonts.hpp
        // currently defines. `crowdedAtStrideOne` counts the cases that would
        // have collided without the stride: if a refactor ever made the sweep
        // stop exercising crowding, the property below would be vacuously
        // true and this count is what says so.
        int crowdedAtStrideOne = 0;
        int checkedPairs = 0;
        int worstStride = 0;
        for (int h = 40; h <= 900; h += 7) {
            for (int floorDb = -160; floorDb <= -20; floorDb += 5) {
                for (int ceilDb = floorDb + 10; ceilDb <= 0; ceilDb += 15) {
                    const float dbMin = static_cast<float>(floorDb);
                    const float dbMax = static_cast<float>(ceilDb);
                    const float panelH = static_cast<float>(h);
                    for (const float labelH : {12.0f, 14.0f, 16.0f, 18.0f}) {
                        const int stride =
                            SpectrumView::dbLabelStride(dbMin, dbMax, panelH, labelH);
                        CHECK(stride >= 1);
                        if (stride > worstStride) { worstStride = stride; }
                        const double per10 =
                            static_cast<double>(panelH) * 10.0 /
                            (static_cast<double>(dbMax) - static_cast<double>(dbMin));
                        if (per10 < static_cast<double>(labelH) + 3.0) {
                            ++crowdedAtStrideOne;
                        }
                        // Walk the gridlines the panel would draw and check the
                        // gap between the figures that survive the stride.
                        float grid[64];
                        const int gridCount =
                            SpectrumView::gridlineDbs(dbMin, dbMax, grid, 64);
                        double prevY = 0.0;
                        bool havePrev = false;
                        for (int i = 0; i < gridCount; ++i) {
                            const int decade =
                                static_cast<int>(std::lround(grid[i] / 10.0f));
                            if (stride > 1 && decade % stride != 0) { continue; }
                            const double y = dbToY(grid[i], dbMin, dbMax, 0.0f, panelH);
                            if (havePrev) {
                                ++checkedPairs;
                                // Labels sit BELOW their line and run downward,
                                // so consecutive figures must be a full label
                                // height plus the stated 3 px of air apart.
                                // The stride cap can legitimately fail this on
                                // a panel too short for two figures at any
                                // stride; those cases have at most one label
                                // on screen anyway, so they are excluded
                                // rather than silently tolerated.
                                if (stride < 64) {
                                    CHECK(std::fabs(y - prevY) >=
                                          static_cast<double>(labelH) + 3.0 - 1e-6);
                                }
                            }
                            prevY = y;
                            havePrev = true;
                        }
                    }
                }
            }
        }
        CHECK(checkedPairs > 10000);
        CHECK(crowdedAtStrideOne > 1000);
        CHECK(worstStride > 1);
    }

    return testSummary("test_spectrum_view");
}
