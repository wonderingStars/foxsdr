// The radio workspace's host logic (src/slint/workspace_logic.hpp, docs/multi-radio-records/screen-s3.md, stage S3), with a MADE-UP ENGINE: the number of radios comes from the facts the engine
// gives (fact.radio.count and fact.radio.<n>.state), a click in a pane selects through RADIO_FOCUS and the window follows only when the engine took it, a gesture is aimed at the radio of the
// pane it happened in, only the radios on screen are read and painted, the pictures are fitted to the pane, a page change shows the other radios, and the small window falls back to One.
// Radios 2 to 8 do not exist in this tree's engine (stage 4 is another lane's): every answer here is made up, and the live multi-radio path is UNVERIFIED by this test.
// No window, no engine: the host's pure units plus the pixel buffers of a RadioView (so it links Slint like test_slint_radio_view).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "radio_view.hpp"
#include "workspace_logic.hpp"
#include "workspace_testseam.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "core/app_commands.hpp"

using namespace foxsdr::slinthost;

namespace {

// The made-up engine: `radios` is the set of radio numbers it has; `facts` is whether it speaks the facts at all; focusAnswer overrides RADIO_FOCUS's answer (default: OK for a radio it has).
struct FakeEngine {
    std::vector<int> radios;
    bool facts = true;
    int countFact = -1;                    // -1: the number of radios; else this number is reported (a disagreeing engine)
    int32_t focusAnswer = 99;              // 99: by the rule
    std::vector<std::uint32_t> focusCalls;

    EngineReads reads() {
        EngineReads e;
        e.fact = [this](const char* name, std::string& out) -> int {
            if (!facts) { return -1; }
            const std::string n = name;
            if (n == "fact.radio.count") { out = std::to_string(countFact >= 0 ? countFact : static_cast<int>(radios.size())); return 1; }
            for (const int r : radios) { if (n == "fact.radio." + std::to_string(r) + ".state") { out = "running"; return 1; } }
            return -1;
        };
        e.focus = [this](std::uint32_t radio) -> int32_t {
            focusCalls.push_back(radio);
            if (focusAnswer != 99) { return focusAnswer; }
            return std::find(radios.begin(), radios.end(), static_cast<int>(radio)) != radios.end() ? FOXAPI_OK : FOXAPI_NO_DEVICE;
        };
        return e;
    }
};

std::vector<int> slotsOf(const std::vector<PaneCell>& cells) {
    std::vector<int> v;
    for (const auto& c : cells) { v.push_back(c.occupied ? c.slot + 1 : -(c.slot + 1)); }
    return v;
}

std::vector<int> iota(int n) { std::vector<int> v; for (int i = 1; i <= n; ++i) { v.push_back(i); } return v; }

FoxReceiverState tunedState(double centre, double tuned, double rate, bool running) {
    FoxReceiverState s{};
    s.structSize = sizeof(s);
    s.centreHz = centre; s.tunedHz = tuned; s.vfoOffsetHz = tuned - centre; s.sampleRateHz = rate; s.bandwidthHz = 12500;
    s.demodMode = FOXAPI_DEMOD_NFM; s.volume = 0.72; s.squelchDb = -60;
    if (running) { s.flags |= FOXAPI_RX_RUNNING; }
    std::snprintf(s.deviceName, sizeof(s.deviceName), "RTL-SDR #0");
    return s;
}

}  // namespace

int main() {
    // ---- the layouts ----
    CHECK(panesOf(kLayoutOne) == 1 && panesOf(kLayoutTwo) == 2 && panesOf(kLayoutFour) == 4 && panesOf(99) == 1 && panesOf(-3) == 1);
    CHECK(pageSizeOf(kLayoutOne) == 1 && pageSizeOf(kLayoutTwo) == 2 && pageSizeOf(kLayoutFour) == 4);

    // ---- THE COUNT COMES FROM THE ENGINE ----
    {
        RadioSet set; Workspace ws(set);
        FakeEngine eng; eng.radios = iota(3);
        ws.readFacts(eng.reads());
        CHECK(ws.radioCount() == 3);
        CHECK(ws.mask() == 0b111u);
        CHECK(ws.occupied(0) && ws.occupied(2) && !ws.occupied(3));
        CHECK(ws.highestSlot() == 2);
        // the engine opens a fourth, then drops the first: the mask is what exists, the numbers do not close up
        eng.radios = {2, 3, 4};
        ws.readFacts(eng.reads());
        CHECK(ws.radioCount() == 3 && ws.mask() == 0b1110u && !ws.occupied(0) && ws.occupied(3));
        CHECK(set.selected == 1);                               // the selection (radio 1) left the open radios: it moves to the lowest that exists, radio 2
        // eight radios
        eng.radios = iota(8);
        ws.readFacts(eng.reads());
        CHECK(ws.radioCount() == 8 && ws.mask() == 0xffu && ws.highestSlot() == 7);
        // an engine without the facts has one radio (today's engine before stage 3)
        FakeEngine old; old.facts = false;
        ws.readFacts(old.reads());
        CHECK(ws.radioCount() == 1 && ws.mask() == 1u && set.selected == 0);
        // a count with no per-radio facts: the radios are 1..count
        FakeEngine countOnly; countOnly.radios = {}; countOnly.countFact = 3;
        ws.readFacts(countOnly.reads());
        CHECK(ws.radioCount() == 3 && ws.mask() == 0b111u);
        // a made-up number above the limit never opens a ninth slot
        FakeEngine many; many.radios = iota(8); many.countFact = 12;
        ws.readFacts(many.reads());
        CHECK(ws.radioCount() == 8);
    }

    // ---- WHAT IS SHOWN: the layout, the one-radio rule and the small window ----
    {
        RadioSet set; Workspace ws(set);
        FakeEngine eng; eng.radios = iota(4);
        ws.readFacts(eng.reads());
        CHECK(ws.layout() == kLayoutOne && ws.shownLayout(1600, 900) == kLayoutOne);
        ws.setLayout(kLayoutFour);
        CHECK(ws.layout() == kLayoutFour && ws.shownLayout(1600, 900) == kLayoutFour && ws.shownLayout(1440, 900) == kLayoutFour);
        CHECK(ws.shownLayout(1280, 720) == kLayoutFour);                          // exactly the minimum is enough
        CHECK(ws.shownLayout(1279, 900) == kLayoutOne && ws.shownLayout(1600, 719) == kLayoutOne);   // either side below it falls back to One
        CHECK(ws.layout() == kLayoutFour);                                        // the CHOSEN layout is kept: it returns when the window is wide again
        CHECK(ws.fellBack(1279, 900) && ws.fellBack(1600, 600) && !ws.fellBack(1280, 720) && !ws.fellBack(1600, 900));
        ws.setLayout(kLayoutTwo);
        CHECK(ws.shownLayout(1440, 900) == kLayoutTwo);
        ws.setLayout(99); CHECK(ws.layout() == kLayoutFour);                      // clamped
        ws.setLayout(-4); CHECK(ws.layout() == kLayoutOne);
        // one radio: a multi-pane layout shows One, and there is nothing to fall back from
        FakeEngine one; one.radios = {1};
        ws.readFacts(one.reads());
        ws.setLayout(kLayoutFour);
        CHECK(ws.shownLayout(1600, 900) == kLayoutOne && !ws.fellBack(600, 400));
    }

    // ---- CELLS AND PAGES ----
    {
        RadioSet set; Workspace ws(set);
        FakeEngine eng; eng.radios = iota(8);
        ws.readFacts(eng.reads());
        ws.setLayout(kLayoutFour);
        CHECK(ws.pageCount() == 2 && ws.page() == 0);
        CHECK((slotsOf(ws.cells(1600, 900)) == std::vector<int>{1, 2, 3, 4}));
        ws.setPage(1);
        CHECK(ws.page() == 1 && (slotsOf(ws.cells(1600, 900)) == std::vector<int>{5, 6, 7, 8}));   // a second PAGE of four, never more than four panes
        ws.setPage(7); CHECK(ws.page() == 1);                                     // clamped to the pages there are
        ws.setPage(-2); CHECK(ws.page() == 0);
        // the small window: One, the selected radio
        CHECK((slotsOf(ws.cells(1000, 600)) == std::vector<int>{1}));
        // Two pages through the pairs: all eight radios stay reachable
        ws.setLayout(kLayoutTwo);
        CHECK(ws.pageCount() == 4);
        ws.setPage(3);
        CHECK((slotsOf(ws.cells(1600, 900)) == std::vector<int>{7, 8}));
        // fewer radios than a page: the empty cells are marked (the seam of S4's card), and there is no second page
        FakeEngine six; six.radios = iota(6);
        ws.readFacts(six.reads());
        ws.setLayout(kLayoutFour);
        CHECK(ws.pageCount() == 2);
        ws.setPage(1);
        CHECK((slotsOf(ws.cells(1600, 900)) == std::vector<int>{5, 6, -7, -8}));
        FakeEngine three; three.radios = iota(3);
        ws.readFacts(three.reads());
        CHECK(ws.pageCount() == 1 && ws.page() == 0);                              // the page it was on no longer exists
        CHECK((slotsOf(ws.cells(1600, 900)) == std::vector<int>{1, 2, 3, -4}));
        // layout One: the selected radio is the only cell
        ws.setLayout(kLayoutOne);
        CHECK((slotsOf(ws.cells(1600, 900)) == std::vector<int>{1}));
    }

    // ---- THE PAGE FOLLOWS THE SELECTION when the layout changes ----
    {
        RadioSet set; Workspace ws(set);
        FakeEngine eng; eng.radios = iota(8);
        ws.readFacts(eng.reads());
        ws.setLayout(kLayoutFour);
        auto e = eng.reads();
        CHECK(ws.select(6, e) && ws.selectedRadio() == 6 && ws.page() == 1);       // radio 6 lives on the second page of four
        ws.setLayout(kLayoutTwo);
        CHECK(ws.page() == 2);                                                      // and in the third pair
        ws.setLayout(kLayoutFour);
        CHECK(ws.page() == 1);
        ws.setLayout(kLayoutOne);
        CHECK(ws.page() == 5);                                                      // (layout One has a page per radio: unused, but defined)
    }

    // ---- SELECTING IS THE SESSION'S FOCUS ----
    {
        RadioSet set; Workspace ws(set);
        FakeEngine eng; eng.radios = iota(4);
        ws.readFacts(eng.reads());
        ws.setLayout(kLayoutFour);
        auto e = eng.reads();
        CHECK(ws.selectedRadio() == 1);
        CHECK(ws.select(3, e));
        CHECK(eng.focusCalls.size() == 1 && eng.focusCalls[0] == 3u);               // RADIO_FOCUS 3 went to the engine
        CHECK(ws.selectedRadio() == 3 && set.selected == 2);
        // the same radio again: nothing to ask
        CHECK(ws.select(3, e));
        CHECK(eng.focusCalls.size() == 1);
        // a radio that is not open: refused here, the engine is not asked, the selection stays
        CHECK(!ws.select(7, e) && !ws.select(0, e) && !ws.select(-1, e) && !ws.select(9, e));
        CHECK(eng.focusCalls.size() == 1 && ws.selectedRadio() == 3);
        // the engine refuses: the window does not move
        eng.focusAnswer = FOXAPI_NO_DEVICE;
        CHECK(!ws.select(2, e));
        CHECK(eng.focusCalls.size() == 2 && eng.focusCalls[1] == 2u && ws.selectedRadio() == 3);
        eng.focusAnswer = FOXAPI_OUT_OF_RANGE;
        CHECK(!ws.select(2, e) && ws.selectedRadio() == 3);
        // NO_CHANGE (the engine already had that focus) is a yes
        eng.focusAnswer = FOXAPI_NO_CHANGE;
        CHECK(ws.select(4, e) && ws.selectedRadio() == 4);
        // no engine function at all: refused
        EngineReads none;
        CHECK(!ws.select(1, none) && ws.selectedRadio() == 4);
        // radio 4 goes away: the selection moves to a radio that exists
        FakeEngine three; three.radios = iota(3);
        ws.readFacts(three.reads());
        CHECK(ws.selectedRadio() == 1);
    }

    // ---- ONLY THE RADIOS ON SCREEN ARE TICKED (and the selected one, which the strip shows) ----
    {
        RadioSet set; Workspace ws(set);
        FakeEngine eng; eng.radios = iota(8);
        ws.readFacts(eng.reads());
        auto e = eng.reads();
        // layout One: the selected radio alone
        CHECK((ws.slotsToTick(1600, 900) == std::vector<int>{0}));
        ws.setLayout(kLayoutFour);
        CHECK((ws.slotsToTick(1600, 900) == std::vector<int>{0, 1, 2, 3}));
        ws.setPage(1);
        CHECK((ws.slotsToTick(1600, 900) == std::vector<int>{4, 5, 6, 7, 0}));      // radios 5 to 8 and, because the strip shows it, the selected radio 1
        CHECK(ws.select(6, e));                                                     // selecting radio 6: it is on the page, so nothing is added
        CHECK((ws.slotsToTick(1600, 900) == std::vector<int>{4, 5, 6, 7}));
        ws.setPage(0);
        CHECK((ws.slotsToTick(1600, 900) == std::vector<int>{0, 1, 2, 3, 5}));      // page one on screen again: radio 6 is selected and off screen
        // the small window falls back to One: only the selected radio is read
        CHECK((ws.slotsToTick(1000, 600) == std::vector<int>{5}));
        // empty cells are not ticked
        FakeEngine two; two.radios = {1, 2};
        ws.readFacts(two.reads());
        CHECK((ws.slotsToTick(1600, 900) == std::vector<int>{0, 1}));
        // layout Two shows two
        ws.setLayout(kLayoutTwo);
        FakeEngine five; five.radios = iota(5);
        ws.readFacts(five.reads());
        ws.setPage(1);
        CHECK((ws.slotsToTick(1600, 900) == std::vector<int>{2, 3, 0}));
    }

    // ---- THE PICTURES ARE FITTED TO THE PANE ----
    {
        const PlotSize p = panePlot(563, 353);
        const PictureSize fit = panePictures(563, 353);
        const PictureSize today;
        CHECK(fit.traceW == static_cast<std::uint32_t>(p.w) && fit.wfW == static_cast<std::uint32_t>(p.w));
        CHECK(p.w == 563 - kPaneGutterW);                                          // the plot starts after the age strip's gutter
        CHECK(fit.traceW < today.traceW && fit.traceH < today.traceH && fit.wfH < today.wfH);   // smaller than today's 1024 x 340 and 1024 x 238
        CHECK(fit.traceH > 40 && fit.wfH > 40 && fit.traceH + fit.wfH + kPaneHeaderH + kPaneReadoutH + kPaneSplitterH + kPaneSpectrumTop + kPaneSpectrumFoot + kPaneWaterfallFoot <= 353);
        // a bigger pane, a bigger picture; a tiny pane never goes below 16 px
        CHECK(panePictures(643, 353).traceW > fit.traceW);
        const PictureSize tiny = panePictures(30, 40);
        CHECK(tiny.traceW >= 16 && tiny.traceH >= 16 && tiny.wfW >= 16 && tiny.wfH >= 16);
        // the split moves the share
        CHECK(panePictures(563, 353, 0.7f).traceH > panePictures(563, 353, 0.3f).traceH);

        RadioSet set; Workspace ws(set);
        FakeEngine eng; eng.radios = iota(4);
        ws.readFacts(eng.reads());
        RadioView& rv = set.at(1);
        CHECK(rv.wells.size.traceW == 1024u);                                       // made at the size of today's screen
        CHECK(ws.fitPictures(1, 563, 353));
        CHECK(rv.wells.size.traceW == fit.traceW && rv.wells.size.traceH == fit.traceH && rv.wells.size.wfW == fit.wfW && rv.wells.size.wfH == fit.wfH);
        CHECK(rv.plot.w == p.w && rv.plot.h == p.h);                                // the axes are laid out over the fitted plot
        CHECK(rv.sinceSeq == 0u);                                                   // the next frame paints at once
        rv.sinceSeq = 77;
        CHECK(!ws.fitPictures(1, 563, 353) && rv.sinceSeq == 77u);                  // the same size: no new buffers, no restarted history
        CHECK(!ws.fitPictures(1, 566, 355) && rv.sinceSeq == 77u);                  // a few pixels are not worth a restart (a window drag)
        CHECK(ws.fitPictures(1, 643, 353) && rv.wells.size.traceW == panePictures(643, 353).traceW);
        // the pane's pixel memory is the fitted size
        CHECK(rv.wells.picture.size() == static_cast<std::size_t>(rv.wells.size.wfW) * rv.wells.size.wfH);
        // back to layout One: today's size
        CHECK(ws.restorePictures(1) && rv.wells.size.traceW == 1024u && rv.wells.size.traceH == 340u && rv.wells.size.wfH == 238u);
        CHECK(!ws.restorePictures(1));
    }

    // ---- WHAT A PANE SAYS ----
    {
        FoxReceiverState s = tunedState(145.0e6, 145.5e6, 2.4e6, true);
        s.flags |= FOXAPI_RX_DECODER_ACTIVE | FOXAPI_RX_BIAS_TEE_ON;
        PaneTexts t = paneTexts(2, s, "32 dB");
        CHECK(t.prefix == "Radio 2" && t.selectLabel == "Select Radio 2" && t.stopLabel == "Stop Radio 2");
        CHECK(t.spectrumLabel == "Radio 2 spectrum" && t.waterfallLabel == "Radio 2 waterfall");
        CHECK(t.runTip == "Radio 2 is running" && t.decTip == "A decoder is running on Radio 2" && t.muteTip == "Radio 2 is muted" && t.failTip == "Radio 2 has failed");
        CHECK(t.device == "RTL-SDR #0" && t.frequency == "145.500000 MHz" && t.mode == "NFM");
        CHECK(t.gain == "32 dB" && t.squelch == "-60 dB" && t.volume == "72%" && t.rate == "2.400 MS/s");
        CHECK(t.running && t.decoding && t.biasTee && !t.muted && !t.failed);
        s.flags &= ~FOXAPI_RX_RUNNING;
        CHECK(paneTexts(5, s, "").stopLabel == "Start Radio 5");                    // a stopped radio offers START
        CHECK(paneTexts(5, s, "").gain == "--");                                   // no figure known, no AGC: dashes
        s.flags |= FOXAPI_RX_DEVICE_AGC;
        CHECK(paneTexts(5, s, "").gain == "AUTO");
        s.squelchDb = -200;
        CHECK(paneTexts(5, s, "").squelch == "--");
        s.flags |= FOXAPI_RX_FAULTED | FOXAPI_RX_MUTED;
        const PaneTexts f = paneTexts(8, s, "");
        CHECK(f.failed && f.muted && f.prefix == "Radio 8");
        CHECK(pageLabel(1, 4) == "Radios 1-4" && pageLabel(5, 8) == "Radios 5-8");
        CHECK(pageTip(5, 8) == "Show radios 5 to 8");
        CHECK(smallWindowNotice() == "Several radios need a window of at least 1280 x 720. Showing one radio.");
    }

    // ---- A GESTURE IN A PANE IS AIMED AT THE PANE'S RADIO ----
    {
        RadioSet set;
        std::vector<std::pair<FoxCommand, std::uint32_t>> sent;
        PaneGestures g(set, [&](const FoxCommand& c, std::uint32_t radio) { sent.push_back({c, radio}); });
        // radios 1 and 2 on different bands; radio 2 is stopped
        RadioView& r1 = set.at(0);
        RadioView& r2 = set.at(1);
        r1.state = tunedState(145.0e6, 145.0e6, 2.0e6, true);
        r2.state = tunedState(98.0e6, 98.0e6, 2.0e6, false);
        // click in pane 2 at a quarter of its plot: VFO_TO_ABSOLUTE at radio 2's 97.5 MHz, aimed at radio 2
        g.click(2, 0.25f, false);
        CHECK(sent.size() == 1);
        CHECK(sent[0].first.op == FOXAPP_OP_VFO_TO_ABSOLUTE && std::fabs(sent[0].first.num[0] - 97.5e6) < 1.0 && sent[0].first.ival[0] == 1 && sent[0].second == 2u);
        // the same click in pane 1 is radio 1's 144.5 MHz
        g.click(1, 0.25f, true);
        CHECK(std::fabs(sent[1].first.num[0] - 144.5e6) < 1.0 && sent[1].first.ival[0] == 0 && sent[1].second == 1u);
        // the command carries the target once the host aims it (aimAt is submit's)
        FoxCommand c = sent[0].first; aimAt(c, sent[0].second);
        CHECK(c.target == 2u);
        FoxCommand c1 = sent[1].first; aimAt(c1, sent[1].second);
        CHECK(c1.target == 1u);
        // STOP/START: radio 2 is stopped, so the key starts it; radio 1 runs, so the key stops it
        g.stop(2); CHECK(sent[2].first.op == FOXAPI_OP_RUN && sent[2].first.ival[0] == 1 && sent[2].second == 2u);
        g.stop(1); CHECK(sent[3].first.ival[0] == 0 && sent[3].second == 1u);
        // the band drag: the VFO offset inside radio 2's baseband, and the tick of the drag is remembered by THAT radio only
        g.bandDrag(2, 0.1f, 42);
        CHECK(sent[4].first.op == FOXAPI_OP_SET_VFO_OFFSET && std::fabs(sent[4].first.num[0] - 0.1 * 2.0e6) < 1.0 && sent[4].second == 2u);
        CHECK(r2.lastBandDragTick == 42 && r1.lastBandDragTick != 42);
        g.edgeDrag(2, 0.01f);
        CHECK(sent[5].first.op == FOXAPP_OP_SET_BANDWIDTH_DRAG && std::fabs(sent[5].first.num[0] - 0.02 * 2.0e6) < 1.0 && sent[5].second == 2u);
        // the wheel: zoom is the radio's own, and sends nothing; shift steps the tuning of that radio
        const std::size_t before = sent.size();
        g.wheel(2, 0.5f, 1, false);
        CHECK(sent.size() == before);
        CHECK(r2.view.spanHz > 0 && r2.view.spanHz < 2.0e6 && r1.view.spanHz == 0);   // radio 2 zoomed in; radio 1 did not
        g.pan(2, 0.1f);
        g.resetView(2);
        CHECK(r2.view.spanHz == 0);
        g.wheel(1, 0.5f, -1, true);
        CHECK(sent.back().first.op == FOXAPI_OP_STEP_TUNE && sent.back().first.ival[0] == -1 && sent.back().second == 1u);
        // the selection is not touched by any gesture
        CHECK(set.selected == 0);
    }

    // ---- THE TEST SEAM: made-up radios, each with its own tone ----
    {
        CHECK(testRadiosFromEnv("4", true) == 4 && testRadiosFromEnv("4", false) == 0);   // only a bounded run
        CHECK(testRadiosFromEnv(nullptr, true) == 0 && testRadiosFromEnv("", true) == 0 && testRadiosFromEnv("1", true) == 0 && testRadiosFromEnv("9", true) == 0 && testRadiosFromEnv("x", true) == 0);
        CHECK(testRadiosFromEnv("8", true) == 8 && testRadiosFromEnv("2", true) == 2);
        SeamRadios seam(4);
        CHECK(seam.count() == 4 && seam.mask() == 0b1111u && !seam.fake(0) && seam.fake(1) && seam.fake(3) && !seam.fake(4));
        CHECK(seam.focus(1) == FOXAPI_OK && seam.focus(4) == FOXAPI_OK && seam.focus(5) == FOXAPI_NO_DEVICE && seam.focus(0) == FOXAPI_NO_DEVICE);
        const FoxReceiverState real = tunedState(145.0e6, 145.5e6, 2.4e6, true);
        CHECK(seam.state(0, real).tunedHz == 145.5e6);                              // radio 1 is the engine's own
        CHECK(std::string(seam.state(1, real).deviceName) == "Test radio 2" && std::string(seam.state(3, real).deviceName) == "Test radio 4");
        CHECK(seam.state(1, real).tunedHz != seam.state(2, real).tunedHz);
        CHECK((seam.state(2, real).flags & FOXAPI_RX_RUNNING) == 0 && (seam.state(1, real).flags & FOXAPI_RX_RUNNING) != 0);
        // each radio's spectrum has its own tone: the loudest bin is at its place and no other radio's
        std::vector<float> bins(1024);
        std::vector<int> peakAt;
        for (int slot = 1; slot < 4; ++slot) {
            FoxSpectrumInfo info{}; info.structSize = sizeof(info);
            CHECK(seam.spectrum(slot, 0, info, bins.data(), 1024, real) == FOXAPI_OK);
            CHECK(info.copied == 1024u && info.binCount == 1024u && info.seq >= 1u);
            const int at = static_cast<int>(std::max_element(bins.begin(), bins.end()) - bins.begin());
            CHECK(std::abs(at - static_cast<int>(SeamRadios::toneFraction(slot) * 1024)) <= 2);
            for (const int other : peakAt) { CHECK(std::abs(other - at) > 40); }
            peakAt.push_back(at);
            std::uint64_t first = info.seq;
            seam.spectrum(slot, first, info, bins.data(), 1024, real);
            CHECK(info.seq > first);                                                // a new frame each call
        }
        // a command aimed at a made-up radio changes ITS state and no other
        FoxCommand tune{}; tune.structSize = sizeof(tune); tune.op = FOXAPP_OP_VFO_TO_ABSOLUTE; tune.num[0] = 100.2e6;
        CHECK(seam.apply(1, tune, real));
        CHECK(seam.state(1, real).tunedHz == 100.2e6 && seam.state(2, real).tunedHz != 100.2e6 && seam.state(0, real).tunedHz == 145.5e6);
        FoxCommand run{}; run.structSize = sizeof(run); run.op = FOXAPI_OP_RUN; run.ival[0] = 0;
        CHECK(seam.apply(1, run, real) && (seam.state(1, real).flags & FOXAPI_RX_RUNNING) == 0);
        CHECK(!seam.apply(0, tune, real));                                          // radio 1 is the engine's: not the seam's to change
        FoxCommand other{}; other.structSize = sizeof(other); other.op = FOXAPI_OP_SET_VOLUME;
        CHECK(!seam.apply(1, other, real));
    }

    return testSummary("test_slint_workspace_host");
}
