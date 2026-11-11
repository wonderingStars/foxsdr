// Zoom to a dragged selection (src/core/zoom_select.hpp): closed-form rectangle -> view mapping, clamping, minimum span, thin drags, history.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <limits>
#include <random>

#include "core/zoom_select.hpp"
#include "test_check.hpp"

using namespace cascade::core;

namespace {

SpectrumView base() { return {100.0e6, 2.0e6, 0.0, -100.0}; }
ZoomLimits limits() {
    ZoomLimits l;
    l.fullLoHz = 99.0e6;
    l.fullHiHz = 101.0e6;
    l.minSpanHz = 50.0e3;
    l.dbMin = -140.0;
    l.dbMax = 10.0;
    l.minDbRange = 5.0;
    return l;
}

void testClosedForm() {
    // Plot 1000 x 400 px. x 250..500 is a quarter to a half of 2 MHz from 99 MHz: 99.5 .. 100 MHz. y 100..300: a quarter to three quarters of 0 .. -100 dB.
    const ZoomResult r = zoomToSelection(base(), {250, 100, 500, 300}, 1000, 400, limits());
    CHECK(r.changed);
    CHECK_NEAR(r.view.centreHz, 99.75e6, 1e-3);
    CHECK_NEAR(r.view.spanHz, 0.5e6, 1e-3);
    CHECK_NEAR(r.view.topDb, -25.0, 1e-9);
    CHECK_NEAR(r.view.bottomDb, -75.0, 1e-9);
    // Corners in any order give the same view.
    const ZoomResult r2 = zoomToSelection(base(), {500, 300, 250, 100}, 1000, 400, limits());
    CHECK(r2.view == r.view);
    const ZoomResult r3 = zoomToSelection(base(), {250, 300, 500, 100}, 1000, 400, limits());
    CHECK(r3.view == r.view);
    std::printf("MEAS selection 250..500 x 100..300 px on 2 MHz / 100 dB: centre %.1f Hz, span %.1f Hz, %.2f .. %.2f dB\n", r.view.centreHz, r.view.spanHz,
                r.view.topDb, r.view.bottomDb);
    // A second zoom works on the new view (relative to what is drawn).
    const ZoomResult s = zoomToSelection(r.view, {0, 0, 500, 200}, 1000, 400, limits());
    CHECK_NEAR(s.view.spanHz, 0.25e6, 1e-3);
    CHECK_NEAR(s.view.centreHz, 99.75e6 - 0.125e6, 1e-3);
    CHECK_NEAR(s.view.topDb, -25.0, 1e-9);
    CHECK_NEAR(s.view.bottomDb, -50.0, 1e-9);
}

void testClickAndThin() {
    const SpectrumView v = base();
    CHECK(!zoomToSelection(v, {400, 200, 403, 203}, 1000, 400, limits()).changed);  // a click
    CHECK(zoomToSelection(v, {400, 200, 403, 203}, 1000, 400, limits()).view == v);
    // Thin vertical band (narrow, tall): the dB range zooms, the frequency view stays.
    const ZoomResult tall = zoomToSelection(v, {400, 100, 402, 300}, 1000, 400, limits());
    CHECK(tall.changed);
    CHECK_NEAR(tall.view.spanHz, v.spanHz, 1e-6);
    CHECK_NEAR(tall.view.centreHz, v.centreHz, 1e-6);
    CHECK_NEAR(tall.view.topDb, -25.0, 1e-9);
    // Thin horizontal band: frequency zooms, dB stays.
    const ZoomResult wide = zoomToSelection(v, {100, 200, 300, 202}, 1000, 400, limits());
    CHECK(wide.changed);
    CHECK_NEAR(wide.view.spanHz, 0.4e6, 1e-3);
    CHECK_NEAR(wide.view.topDb, 0.0, 1e-9);
    CHECK_NEAR(wide.view.bottomDb, -100.0, 1e-9);
    // FrequencyOnly (the waterfall): the tall drag leaves dB alone, and a purely vertical drag is a no-op.
    const ZoomResult fo = zoomToSelection(v, {250, 100, 500, 300}, 1000, 400, limits(), ZoomAxes::FrequencyOnly);
    CHECK_NEAR(fo.view.topDb, 0.0, 1e-9);
    CHECK_NEAR(fo.view.spanHz, 0.5e6, 1e-3);
    CHECK(!zoomToSelection(v, {400, 100, 402, 300}, 1000, 400, limits(), ZoomAxes::FrequencyOnly).changed);
}

void testMinimumsAndLimits() {
    // 8 px of 1000 on 2 MHz = 16 kHz < the 50 kHz minimum: widened about the selection's own centre (x = 500).
    const ZoomResult m = zoomToSelection(base(), {496, 0, 504, 0}, 1000, 400, limits());
    CHECK(m.changed);
    CHECK_NEAR(m.view.spanHz, 50.0e3, 1e-3);
    CHECK_NEAR(m.view.centreHz, 100.0e6, 1.0);
    // The same selection at the left edge: the widened band is pulled back inside the full range, keeping its width.
    const ZoomResult e = zoomToSelection(base(), {0, 0, 8, 0}, 1000, 400, limits());
    CHECK_NEAR(e.view.spanHz, 50.0e3, 1e-3);
    CHECK_NEAR(e.view.centreHz - e.view.spanHz / 2.0, 99.0e6, 1e-3);
    const ZoomResult e2 = zoomToSelection(base(), {992, 0, 1000, 0}, 1000, 400, limits());
    CHECK_NEAR(e2.view.centreHz + e2.view.spanHz / 2.0, 101.0e6, 1e-3);
    // A view that runs past the right end never leaves the full range after a zoom.
    SpectrumView off = {100.9e6, 1.6e6, 0.0, -100.0};
    const ZoomResult o = zoomToSelection(off, {0, 0, 1000, 0}, 1000, 400, limits());
    CHECK(o.view.centreHz + o.view.spanHz / 2.0 <= 101.0e6 + 1e-3);
    CHECK(o.view.centreHz - o.view.spanHz / 2.0 >= 99.0e6 - 1e-3);
    // dB: a 10 px band of 400 px on 100 dB = 2.5 dB < 5 dB minimum: widened about its middle.
    const ZoomResult d = zoomToSelection(base(), {0, 195, 1, 205}, 1000, 400, limits());
    CHECK_NEAR(d.view.topDb - d.view.bottomDb, 5.0, 1e-9);
    CHECK_NEAR((d.view.topDb + d.view.bottomDb) / 2.0, -50.0, 1e-9);
    // dB range near the top limit shifts back inside [-140, 10].
    SpectrumView hi = {100.0e6, 2.0e6, 8.0, -12.0};
    const ZoomResult h = zoomToSelection(hi, {0, 0, 0, 2}, 1000, 400, limits());  // 2 px: a click
    CHECK(!h.changed);
    const ZoomResult h2 = zoomToSelection(hi, {0, 0, 4, 7}, 1000, 400, limits());  // 7 px tall, thin in x: dB only, widened to 5 dB
    CHECK(h2.changed);
    CHECK(h2.view.topDb <= 10.0 + 1e-9);
    CHECK_NEAR(h2.view.topDb - h2.view.bottomDb, 5.0, 1e-9);
    // Selection outside the plot is clamped to it.
    const ZoomResult c = zoomToSelection(base(), {-500, -50, 5000, 900}, 1000, 400, limits());
    CHECK_NEAR(c.view.spanHz, 2.0e6, 1e-3);
    CHECK_NEAR(c.view.topDb, 0.0, 1e-9);
    CHECK_NEAR(c.view.bottomDb, -100.0, 1e-9);
    CHECK(!c.changed);  // the whole plot is the view as it is
}

void testBadInput() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const SpectrumView v = base();
    CHECK(!zoomToSelection(v, {nan, 0, 500, 300}, 1000, 400, limits()).changed);
    CHECK(!zoomToSelection(v, {0, 0, 500, 300}, 0, 400, limits()).changed);
    CHECK(!zoomToSelection(v, {0, 0, 500, 300}, 1000, -1, limits()).changed);
    CHECK(!zoomToSelection(v, {0, 0, 500, 300}, nan, 400, limits()).changed);
    SpectrumView zero = v;
    zero.spanHz = 0;
    CHECK(!zoomToSelection(zero, {0, 0, 500, 300}, 1000, 400, limits()).changed);
    SpectrumView flat = v;
    flat.topDb = flat.bottomDb;
    CHECK(!zoomToSelection(flat, {0, 0, 500, 300}, 1000, 400, limits()).changed);
    ZoomLimits bad = limits();
    bad.fullHiHz = bad.fullLoHz;
    CHECK(!zoomToSelection(v, {0, 0, 500, 300}, 1000, 400, bad).changed);
    bad = limits();
    bad.minSpanHz = nan;
    CHECK(!zoomToSelection(v, {0, 0, 500, 300}, 1000, 400, bad).changed);
    CHECK(zoomToSelection(zero, {0, 0, 500, 300}, 1000, 400, limits()).view == zero);
}

void testZoomOutAndHistory() {
    const ZoomLimits l = limits();
    SpectrumView z = {99.75e6, 0.5e6, -25, -75};
    const SpectrumView o = zoomOut(z, 2.0, l);
    CHECK_NEAR(o.spanHz, 1.0e6, 1e-3);
    CHECK_NEAR(o.centreHz, 99.75e6, 1e-3);
    CHECK_NEAR(o.topDb, -25, 1e-12);
    const SpectrumView o2 = zoomOut(z, 100.0, l);  // capped at the full span, centre pulled to the middle of it
    CHECK_NEAR(o2.spanHz, 2.0e6, 1e-3);
    CHECK_NEAR(o2.centreHz, 100.0e6, 1e-3);
    CHECK(zoomOut(z, 0.5, l) == z);
    CHECK(zoomOut(z, std::numeric_limits<double>::infinity(), l) == z);

    ViewHistory h;
    SpectrumView out;
    CHECK(!h.pop(out) && h.size() == 0);
    h.push(base());
    h.push(base());  // the same view twice is stored once
    CHECK(h.size() == 1);
    for (int i = 1; i <= 40; ++i) { h.push({100.0e6 + i, 1e6, 0, -50}); }
    CHECK(h.size() == kViewHistoryDepth);
    CHECK(h.pop(out) && out.centreHz == 100.0e6 + 40);  // newest first
    for (std::size_t i = 1; i < kViewHistoryDepth; ++i) { CHECK(h.pop(out)); }
    CHECK(out.centreHz == 100.0e6 + 25);  // the oldest kept: 40 - 16 + 1
    CHECK(!h.pop(out));
    h.push(base());
    h.clear();
    CHECK(h.size() == 0);
}

void testRandomDragsStayInside() {
    std::mt19937_64 rng(3);
    std::uniform_real_distribution<double> px(-200.0, 1200.0), py(-100.0, 500.0);
    const ZoomLimits l = limits();
    SpectrumView v = base();
    int changes = 0;
    for (int i = 0; i < 20000; ++i) {
        const ZoomResult r = zoomToSelection(v, {px(rng), py(rng), px(rng), py(rng)}, 1000, 400, l, (i % 3 == 0) ? ZoomAxes::FrequencyOnly : ZoomAxes::Both);
        const SpectrumView& n = r.view;
        CHECK(n.spanHz >= l.minSpanHz - 1e-6 || !r.changed);
        CHECK(n.centreHz - n.spanHz / 2.0 >= l.fullLoHz - 1e-3);
        CHECK(n.centreHz + n.spanHz / 2.0 <= l.fullHiHz + 1e-3);
        CHECK(n.topDb <= l.dbMax + 1e-9 && n.bottomDb >= l.dbMin - 1e-9);
        CHECK(n.topDb - n.bottomDb >= l.minDbRange - 1e-9 || !r.changed);
        if (r.changed) { ++changes; v = n; }
        if (i % 50 == 49) {
            v = zoomOut(v, 40.0, l);
            v.topDb = 0;
            v.bottomDb = -100;
        }
    }
    std::printf("MEAS 20000 random drags: %d applied, every result inside the limits\n", changes);
    CHECK(changes > 1000);
}

}  // namespace

int main() {
    testClosedForm();
    testClickAndThin();
    testMinimumsAndLimits();
    testBadInput();
    testZoomOutAndHistory();
    testRandomDragsStayInside();
    return testSummary("test_zoom_select");
}
