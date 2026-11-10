// RadioView (docs/multi-radio-records/screen-s1-s2.md, S1): the host's per-radio struct. Two views never share a thing (zoom, bins, trace, pixel buffers),
// the view arithmetic is the one the receiver view always had, a command is aimed at a radio by its number (0 = the selected radio), slots are made on first use up to
// the limit of eight, the picture size is an argument, and the two alternating pixel buffers still never detach.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "test_check.hpp"

#include "radio_view.hpp"

#include <cmath>
#include <cstdio>

using namespace foxsdr::slinthost;

namespace {

void primed(RadioView& rv, double centreHz, double spanHz) {
    rv.state.centreHz = centreHz;
    rv.state.sampleRateHz = spanHz;
    rv.lastInfo.centreHz = centreHz;
    rv.lastInfo.spanHz = spanHz;
}

}  // namespace

int main() {
    // ---- the view arithmetic of one radio ----
    {
        RadioView rv(0);
        primed(rv, 145.5e6, 2.4e6);
        CHECK(rv.currentView().spanHz == 2.4e6 && rv.currentView().centreHz == 145.5e6);   // no zoom: the full span
        CHECK(std::fabs(rv.frac(145.5e6) - 0.5f) < 1e-6f);
        CHECK(std::fabs(rv.hzAtFrac(0.0) - 144.3e6) < 1.0);
        rv.zoom(1.3, 0.5);
        CHECK(rv.view.spanHz > 0 && rv.view.spanHz < 2.4e6);
        CHECK(std::fabs(rv.currentView().spanHz - 2.4e6 / 1.3) < 1.0);
        rv.pan(0.1);
        CHECK(rv.view.centreHz < 145.5e6);                  // a positive drag fraction pulls the picture, so the window moves down the band (panBy)
        rv.zoom(1.0 / 1.3, 0.5);                    // zoomed back out: fully out is "no window", spanHz 0
        rv.resetView();
        CHECK(rv.view.spanHz == 0 && rv.view.centreHz == 0);
        for (int i = 0; i < 40; ++i) { rv.zoom(1.3, 0.5); }
        CHECK(rv.view.spanHz >= std::max(1.0, 2.4e6 / 1000.0) - 1e-6);   // never narrower than the minimum span
        // dbAtFrac reads the published frame, not the state's floor
        RadioView fr(1);
        primed(fr, 100e6, 1e6);
        fr.state.dbMin = -110.0;
        CHECK(fr.dbAtFrac(0.5) == -110.0);          // no frame yet
        fr.frame = {-90.f, -80.f, -70.f, -60.f};
        CHECK(fr.dbAtFrac(0.0) == -90.0);
        CHECK(fr.dbAtFrac(0.99) == -60.0);
    }

    // ---- two radios share nothing ----
    {
        RadioSet set;
        RadioView& a = set.at(0);
        RadioView& b = set.at(1);
        CHECK(&a != &b && a.slot == 0 && b.slot == 1);
        primed(a, 100e6, 2.4e6);
        primed(b, 162e6, 2.4e6);
        a.zoom(1.3, 0.25);
        CHECK(a.view.spanHz > 0);
        CHECK(b.view.spanHz == 0);                  // zooming radio 1 leaves radio 2's picture alone
        b.sinceSeq = 77;
        CHECK(a.sinceSeq == 0);
        a.frame = {1.f, 2.f};
        CHECK(b.frame.empty());
        a.traceState.peak.assign(8, -50.f);
        CHECK(b.traceState.peak.empty());
        a.sigs.changed("freq", "x");
        CHECK(b.sigs.changed("freq", "x"));         // radio 2's signature cache has not seen it
        CHECK(a.wells.traceMemory() != b.wells.traceMemory());
        CHECK(a.wells.picture.data() != b.wells.picture.data());
        a.plot = {400, 120};
        CHECK(b.plot.w == 980 && b.plot.h == 330);   // the default is today's plot
        CHECK(radioNumberOf(a) == 1u && radioNumberOf(b) == 2u);
    }

    // ---- slots: made on first use, at most eight, the picture size an argument ----
    {
        RadioSet set;
        CHECK(set.created() == 0 && !set.exists(0));
        set.at(0);
        CHECK(set.created() == 1 && set.exists(0) && !set.exists(1));
        set.at(7);
        CHECK(set.created() == 2 && set.exists(7));
        CHECK(set.at(8).slot == kMaxRadios - 1);    // past the limit it is the last slot, never a ninth
        CHECK(set.at(-3).slot == 0);
        CHECK(set.created() == 2);
        CHECK(kMaxRadios == 8);
        CHECK(set.selected == 0 && set.selectedView().slot == 0);
        set.selected = 5;
        CHECK(set.selectedView().slot == 5 && set.created() == 3);
        CHECK(set.at(0).wells.size.traceW == 1024 && set.at(0).wells.size.wfH == 238);   // today's pictures
        PictureSize pane;
        pane.traceW = 512; pane.traceH = 120; pane.wfW = 512; pane.wfH = 160;
        set.setPictureSize(pane);
        RadioView& late = set.at(3);
        CHECK(late.wells.size.traceW == 512 && late.wells.size.wfH == 160);
        CHECK(late.wells.picture.size() == 512u * 160u);
        CHECK(late.wells.trace[0].width() == 512u && late.wells.trace[0].height() == 120u);
        CHECK(set.at(0).wells.size.traceW == 1024);  // a radio already made keeps its pictures
    }

    // ---- a command is aimed by the radio's number; 0 leaves it on the selected radio ----
    {
        FoxCommand c{};
        c.target = 0;
        aimAt(c, 0);
        CHECK(c.target == 0u);
        aimAt(c, 3);
        CHECK(c.target == 3u);
        FoxCommand d{};
        d.target = 9;                                // an op's own object id survives an unaimed submit
        aimAt(d, 0);
        CHECK(d.target == 9u);
    }

    // ---- the two alternating buffers never detach while the window drops the older image ----
    {
        WellBuffers w;
        slint::Image shownTrace, shownWf;
        for (int i = 0; i < 8; ++i) {
            CHECK(w.traceMemory() != nullptr && w.waterfallMemory() != nullptr);
            shownTrace = w.traceImage();
            shownWf = w.waterfallImage();
            w.flip();
        }
        CHECK(w.frames == 8);
        CHECK(w.detaches == 0);
    }

    return testSummary("test_slint_radio_view");
}
