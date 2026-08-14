// Tests for gui/spectrum_view.hpp — pure display math only. draw() needs a
// live ImGui/GL context, so the testable surface is dbToY and gridlineDbs;
// nothing here touches ImGui.
//
// SPDX-License-Identifier: MIT
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

    return testSummary("test_spectrum_view");
}
