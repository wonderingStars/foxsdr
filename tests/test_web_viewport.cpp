// Tests for the browser waterfall's zoom and pan maths (net/web_viewport.hpp).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

#include "net/web_viewport.hpp"
#include "test_check.hpp"

using namespace cascade::net;

namespace {

constexpr std::int64_t kBins = 4096;
const double kNaN = std::numeric_limits<double>::quiet_NaN();
const double kInf = std::numeric_limits<double>::infinity();

bool inside(const View& v, std::int64_t total) {
    return std::isfinite(v.first) && std::isfinite(v.count) && v.first >= 0.0 && v.count >= std::min(kMinVisibleBins, double(total)) &&
           v.count <= double(total) && v.first + v.count <= double(total) + 1e-9;
}

SpectrumSpan span() {
    SpectrumSpan s;
    s.totalBins = kBins;
    s.centreHz = 7.1e6;
    s.spanHz = 2.4e6;  // 6.0 .. 8.4 MHz scale
    return s;
}

void testClamp() {
    CHECK(clampView({-5.0, 100.0}, kBins) == (View{0.0, 100.0}));
    CHECK(clampView({4000.0, 500.0}, kBins) == (View{3596.0, 500.0}));        // pushed past the right end
    CHECK(clampView({0.0, 1.0}, kBins) == (View{0.0, kMinVisibleBins}));       // too narrow
    CHECK(clampView({0.0, 1e9}, kBins) == (View{0.0, double(kBins)}));         // too wide
    CHECK(clampView({kNaN, 100.0}, kBins) == fullView(kBins));
    CHECK(clampView({10.0, kInf}, kBins) == fullView(kBins));
    CHECK(clampView({10.0, 10.0}, 0) == (View{0.0, 0.0}));
    CHECK(clampView({10.0, 10.0}, -3) == (View{0.0, 0.0}));
    // fewer bins than the minimum: the whole row is the only legal view
    CHECK(clampView({0.0, 3.0}, 8) == (View{0.0, 8.0}));
    CHECK(clampView({2.0, 100.0}, 8) == (View{0.0, 8.0}));
}

void testZoomKeepsBinUnderPointer() {
    const View full = fullView(kBins);
    for (const double anchor : {0.0, 0.25, 0.5, 0.9, 1.0}) {
        View v = zoomAt(full, kBins, anchor, 4.0);
        CHECK_NEAR(v.count, kBins / 4.0, 1e-9);
        CHECK(inside(v, kBins));
        // the bin that was under the pointer is still under it
        const double before = full.first + anchor * full.count;
        const double after = v.first + anchor * v.count;
        // at the edges the clamp can move it; elsewhere it must hold exactly
        if (anchor > 0.0 && anchor < 1.0) {
            CHECK_NEAR(after, before, 1e-6);
        }
    }
    // zoom in then out at the same pointer returns the view while no end clamps
    View mid{1000.0, 2000.0};
    View in = zoomAt(mid, kBins, 0.5, 2.0);
    View back = zoomAt(in, kBins, 0.5, 0.5);
    CHECK_NEAR(back.first, mid.first, 1e-6);
    CHECK_NEAR(back.count, mid.count, 1e-6);
    // limits: zoom in a lot stops at the minimum, zoom out a lot stops at the full row
    CHECK_NEAR(zoomAt(full, kBins, 0.5, 1e9).count, kMinVisibleBins, 1e-9);
    CHECK(zoomAt(mid, kBins, 0.5, 1e-9) == full);
    // bad inputs leave the view as it was
    CHECK(zoomAt(mid, kBins, 0.5, 0.0) == mid);
    CHECK(zoomAt(mid, kBins, 0.5, -2.0) == mid);
    CHECK(zoomAt(mid, kBins, 0.5, kNaN) == mid);
    CHECK(zoomAt(mid, kBins, 0.5, kInf) == mid);
    // a NaN anchor means the middle, an out-of-range one is clamped to the edge
    CHECK(zoomAt(mid, kBins, kNaN, 2.0) == zoomAt(mid, kBins, 0.5, 2.0));
    CHECK(zoomAt(mid, kBins, 7.0, 2.0) == zoomAt(mid, kBins, 1.0, 2.0));
    CHECK(zoomAt(mid, kBins, -7.0, 2.0) == zoomAt(mid, kBins, 0.0, 2.0));
    CHECK(zoomAt(mid, 0, 0.5, 2.0) == fullView(0));
}

void testWheel() {
    CHECK_NEAR(wheelFactor(0.0), 1.0, 0.0);
    CHECK(wheelFactor(-100.0) > 1.0);   // wheel up zooms in
    CHECK(wheelFactor(100.0) < 1.0);
    CHECK_NEAR(wheelFactor(-100.0), kWheelZoomPerNotch, 1e-12);
    CHECK_NEAR(wheelFactor(-100.0) * wheelFactor(100.0), 1.0, 1e-12);
    CHECK_NEAR(wheelFactor(-1e6), kMaxZoomStep, 1e-12);       // a trackpad flick is capped
    CHECK_NEAR(wheelFactor(1e6), 1.0 / kMaxZoomStep, 1e-12);
    CHECK_NEAR(wheelFactor(kNaN), 1.0, 0.0);
    CHECK_NEAR(wheelFactor(kInf), 1.0, 0.0);
}

void testPan() {
    View v{1000.0, 1000.0};
    // drag RIGHT by a quarter of the canvas: the picture follows, so the view moves to LOWER bins
    View r = panByPixels(v, kBins, 100.0, 400.0);
    CHECK_NEAR(r.first, 1000.0 - 250.0, 1e-9);
    CHECK_NEAR(r.count, 1000.0, 0.0);
    View l = panByPixels(v, kBins, -100.0, 400.0);
    CHECK_NEAR(l.first, 1250.0, 1e-9);
    // ends clamp
    CHECK_NEAR(panByPixels(v, kBins, 1e9, 400.0).first, 0.0, 0.0);
    CHECK_NEAR(panByPixels(v, kBins, -1e9, 400.0).first, double(kBins) - 1000.0, 1e-9);
    // bad inputs
    CHECK(panByPixels(v, kBins, 10.0, 0.0) == v);
    CHECK(panByPixels(v, kBins, 10.0, -5.0) == v);
    CHECK(panByPixels(v, kBins, kNaN, 400.0) == v);
    CHECK(panByPixels(v, kBins, 10.0, kNaN) == v);
    CHECK(panByPixels(v, 0, 10.0, 400.0) == fullView(0));
    // keys
    CHECK_NEAR(panByBins(v, kBins, 8.0).first, 1008.0, 0.0);
    CHECK_NEAR(panByBins(v, kBins, -8e9).first, 0.0, 0.0);
    CHECK(panByBins(v, kBins, kNaN) == v);
    // panning a full view does nothing
    CHECK(panByPixels(fullView(kBins), kBins, 50.0, 400.0) == fullView(kBins));
}

void testPointerToHz() {
    const SpectrumSpan s = span();
    const double low = 7.1e6 - 1.2e6;
    // full view: the left edge is the span's low edge, the right edge its high edge, the middle the centre
    CHECK_NEAR(pointerToHz(fullView(kBins), s, 0.0, 800.0), low, 1e-6);
    CHECK_NEAR(pointerToHz(fullView(kBins), s, 800.0, 800.0), 8.3e6, 1e-6);
    CHECK_NEAR(pointerToHz(fullView(kBins), s, 400.0, 800.0), 7.1e6, 1e-6);
    // a zoomed view: the pointer maps inside the visible part
    const View z{1024.0, 1024.0};  // a quarter, starting a quarter in
    CHECK_NEAR(pointerToHz(z, s, 0.0, 800.0), low + 0.25 * 2.4e6, 1e-6);
    CHECK_NEAR(pointerToHz(z, s, 800.0, 800.0), low + 0.5 * 2.4e6, 1e-6);
    // the pointer is clamped to the canvas
    CHECK_NEAR(pointerToHz(z, s, -50.0, 800.0), pointerToHz(z, s, 0.0, 800.0), 0.0);
    CHECK_NEAR(pointerToHz(z, s, 5000.0, 800.0), pointerToHz(z, s, 800.0, 800.0), 0.0);
    CHECK_NEAR(pointerToHz(z, s, kNaN, 800.0), pointerToHz(z, s, 0.0, 800.0), 0.0);
    // degenerate
    CHECK_NEAR(pointerToHz(z, s, 10.0, 0.0), low, 1e-6);
    SpectrumSpan empty;
    CHECK_NEAR(pointerToHz(z, empty, 10.0, 800.0), 0.0, 0.0);
    // inverse: hzToPointer(pointerToHz(x)) == x
    for (const double x : {0.0, 1.0, 123.4, 400.0, 799.0, 800.0}) {
        const double hz = pointerToHz(z, s, x, 800.0);
        CHECK_NEAR(hzToPointer(z, s, hz, 800.0), x, 1e-6);
    }
    // a frequency left of the view is a negative pointer, right of it beyond the width
    CHECK(hzToPointer(z, s, low, 800.0) < 0.0);
    CHECK(hzToPointer(z, s, 8.3e6, 800.0) > 800.0);
    CHECK_NEAR(hzToPointer(z, s, 7.0e6, 0.0), 0.0, 0.0);
}

void testRangeAndBins() {
    const SpectrumSpan s = span();
    const View v = viewForRange(s, 7.1e6, 7.4e6);
    CHECK_NEAR(v.count, 0.3e6 / 2.4e6 * kBins, 1e-6);
    CHECK_NEAR(v.first, 1.2e6 / 2.4e6 * kBins, 1e-6);
    CHECK(viewForRange(s, 7.4e6, 7.1e6) == v);           // reversed
    CHECK(viewForRange(s, 1.0e6, 99.0e6) == fullView(kBins));  // wider than the row
    CHECK(viewForRange(s, kNaN, 7.0e6) == fullView(kBins));
    CHECK_NEAR(viewForRange(s, 7.1e6, 7.1e6 + 10.0).count, kMinVisibleBins, 1e-9);  // a sliver widens to the minimum
    SpectrumSpan zero = s;
    zero.spanHz = 0.0;
    CHECK(viewForRange(zero, 1.0, 2.0) == fullView(kBins));
    CHECK(viewForRange(SpectrumSpan{}, 1.0, 2.0) == fullView(0));

    BinRange r = visibleBins(View{10.5, 20.0}, kBins);
    CHECK(r.firstBin == 10 && r.endBin == 31);
    r = visibleBins(fullView(kBins), kBins);
    CHECK(r.firstBin == 0 && r.endBin == kBins);
    r = visibleBins(View{kNaN, 5.0}, kBins);
    CHECK(r.firstBin == 0 && r.endBin == kBins);
    r = visibleBins(View{0.0, 10.0}, 0);
    CHECK(r.firstBin == 0 && r.endBin == 0);
}

// Random operation sequences, including hostile values: the view never leaves the row and never goes non-finite.
void testRandomWalkStaysLegal() {
    std::mt19937_64 rng(12345);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    const double specials[] = {kNaN, kInf, -kInf, 0.0, -0.0, 1e300, -1e300, 1e-300};
    View v = fullView(kBins);
    int bad = 0;
    for (int i = 0; i < 20000; ++i) {
        const double a = (rng() % 11 == 0) ? specials[rng() % 8] : u(rng) * 3000.0;
        const double b = (rng() % 11 == 0) ? specials[rng() % 8] : u(rng) * 3.0;
        switch (rng() % 5) {
            case 0: v = zoomAt(v, kBins, b, a / 1000.0 + 1.0); break;
            case 1: v = panByPixels(v, kBins, a, 800.0); break;
            case 2: v = panByBins(v, kBins, a); break;
            case 3: v = zoomAt(v, kBins, u(rng), wheelFactor(a)); break;
            default: v = clampView(View{a, b * 1000.0}, kBins); break;
        }
        if (!inside(v, kBins)) { ++bad; }
        const BinRange r = visibleBins(v, kBins);
        if (r.firstBin < 0 || r.endBin > kBins || r.endBin < r.firstBin) { ++bad; }
    }
    CHECK(bad == 0);
}

}  // namespace

int main() {
    testClamp();
    testZoomKeepsBinUnderPointer();
    testWheel();
    testPan();
    testPointerToHz();
    testRangeAndBins();
    testRandomWalkStaysLegal();
    return testSummary("test_web_viewport");
}
