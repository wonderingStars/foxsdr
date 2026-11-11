// The radio workspace's logic - see workspace_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "workspace_logic.hpp"

#include "core/app_commands.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "host_logic.hpp"

#include <algorithm>
#include <cstdlib>

namespace foxsdr::slinthost {

int panesOf(int layout) { return layout == kLayoutTwo ? 2 : layout == kLayoutFour ? 4 : 1; }
int pageSizeOf(int layout) { return panesOf(layout); }

namespace {

// The spectrum's share of the pane body under the readout row, as ui/radio_pane.slint computes it: max(60, (body - splitter) * split).
int spectrumWellH(int cellH, float split) {
    const int body = std::max(0, cellH - kPaneHeaderH - kPaneReadoutH);
    return std::max(60, static_cast<int>(static_cast<float>(std::max(0, body - kPaneSplitterH)) * split));
}
int waterfallWellH(int cellH, float split) {
    const int body = std::max(0, cellH - kPaneHeaderH - kPaneReadoutH);
    return std::max(0, body - spectrumWellH(cellH, split) - kPaneSplitterH);
}

}  // namespace

PlotSize panePlot(int cellW, int cellH, float split) {
    PlotSize p;
    p.w = std::max(16, cellW - kPaneGutterW);
    p.h = std::max(16, spectrumWellH(cellH, split) - kPaneSpectrumTop - kPaneSpectrumFoot);
    return p;
}

PictureSize panePictures(int cellW, int cellH, float split) {
    const PlotSize plot = panePlot(cellW, cellH, split);
    PictureSize s;
    s.traceW = static_cast<std::uint32_t>(plot.w);
    s.traceH = static_cast<std::uint32_t>(plot.h);
    s.wfW = static_cast<std::uint32_t>(plot.w);
    s.wfH = static_cast<std::uint32_t>(std::max(16, waterfallWellH(cellH, split) - kPaneWaterfallFoot));
    return s;
}

PaneTexts paneTexts(int radio, const FoxReceiverState& st, const std::string& gain) {
    PaneTexts t;
    // each sentence is a literal tr() argument, so the catalogue test sees the key (a key passed through a helper's parameter would not be found)
    t.prefix = cascade::core::formatText(cascade::i18n::tr("Radio %d"), radio);
    t.selectLabel = cascade::core::formatText(cascade::i18n::tr("Select Radio %d"), radio);
    t.stopLabel = (st.flags & FOXAPI_RX_RUNNING) ? cascade::core::formatText(cascade::i18n::tr("Stop Radio %d"), radio) : cascade::core::formatText(cascade::i18n::tr("Start Radio %d"), radio);
    t.spectrumLabel = cascade::core::formatText(cascade::i18n::tr("Radio %d spectrum"), radio);
    t.waterfallLabel = cascade::core::formatText(cascade::i18n::tr("Radio %d waterfall"), radio);
    t.resizeLabel = "Resize Radio " + std::to_string(radio);   // English only, needs translation: not in the prepared set of strings
    t.closeLabel = "Remove Radio " + std::to_string(radio);    // English only, needs translation: not in the prepared set of strings (stage S4)
    t.runTip = cascade::core::formatText(cascade::i18n::tr("Radio %d is running"), radio);
    t.decTip = cascade::core::formatText(cascade::i18n::tr("A decoder is running on Radio %d"), radio);
    t.muteTip = cascade::core::formatText(cascade::i18n::tr("Radio %d is muted"), radio);
    t.failTip = cascade::core::formatText(cascade::i18n::tr("Radio %d has failed"), radio);
    t.device = st.deviceName;
    t.frequency = fmtMHz(st.tunedHz);
    t.mode = modeName(st.demodMode);
    t.gain = !gain.empty() ? gain : (st.flags & FOXAPI_RX_DEVICE_AGC) ? std::string(cascade::i18n::tr("AUTO")) : std::string("--");
    if (st.squelchDb <= -199.0) { t.squelch = "--"; }
    else { cascade::core::formatUtf8(t.squelch, "%.0f dB", st.squelchDb); }
    cascade::core::formatUtf8(t.volume, "%.0f%%", st.volume * 100.0);
    t.rate = fmtRate(st.sampleRateHz);
    t.running = (st.flags & FOXAPI_RX_RUNNING) != 0;
    t.decoding = (st.flags & FOXAPI_RX_DECODER_ACTIVE) != 0;
    t.muted = (st.flags & FOXAPI_RX_MUTED) != 0 || st.audioMutedBy[0] != '\0';
    t.failed = (st.flags & FOXAPI_RX_FAULTED) != 0;
    t.biasTee = (st.flags & FOXAPI_RX_BIAS_TEE_ON) != 0;
    return t;
}

std::string pageLabel(int firstRadio, int lastRadio) { return cascade::core::formatText(cascade::i18n::tr("Radios %d-%d"), firstRadio, lastRadio); }
std::string pageTip(int firstRadio, int lastRadio) { return cascade::core::formatText(cascade::i18n::tr("Show radios %d to %d"), firstRadio, lastRadio); }
std::string smallWindowNotice() { return cascade::core::formatText(cascade::i18n::tr("Several radios need a window of at least %d x %d. Showing one radio."), kMultiMinW, kMultiMinH); }

// ---------------------------------------------------------------- PaneGestures

namespace {
FoxCommand opOf(std::uint32_t code) { FoxCommand c{}; c.structSize = sizeof(c); c.op = code; return c; }
}  // namespace

void PaneGestures::stop(int radio) {
    FoxCommand c = opOf(FOXAPI_OP_RUN);
    c.ival[0] = (view(radio).state.flags & FOXAPI_RX_RUNNING) ? 0 : 1;
    submit_(c, static_cast<std::uint32_t>(radio));
}
void PaneGestures::click(int radio, float xFrac, bool shift) {
    FoxCommand c = opOf(FOXAPP_OP_VFO_TO_ABSOLUTE);
    c.num[0] = view(radio).hzAtFrac(xFrac);
    c.ival[0] = shift ? 0 : 1;
    submit_(c, static_cast<std::uint32_t>(radio));
}
void PaneGestures::bandDrag(int radio, float dxFrac, int tick) {
    RadioView& rv = view(radio);
    FoxCommand c = opOf(FOXAPI_OP_SET_VFO_OFFSET);
    c.num[0] = rv.state.tunedHz + static_cast<double>(dxFrac) * rv.currentView().spanHz - rv.state.centreHz;
    submit_(c, static_cast<std::uint32_t>(radio));
    rv.lastBandDragTick = tick;
}
void PaneGestures::edgeDrag(int radio, float halfFrac) {
    FoxCommand c = opOf(FOXAPP_OP_SET_BANDWIDTH_DRAG);
    c.num[0] = 2.0 * static_cast<double>(halfFrac) * view(radio).currentView().spanHz;
    submit_(c, static_cast<std::uint32_t>(radio));
}
void PaneGestures::wheel(int radio, float xFrac, int notches, bool shift) {
    RadioView& rv = view(radio);
    if (shift) {
        FoxCommand c = opOf(FOXAPI_OP_STEP_TUNE);
        c.ival[0] = notches > 0 ? 1 : -1;
        c.num[0] = modeStepHz(rv.state.demodMode);
        submit_(c, static_cast<std::uint32_t>(radio));
        return;
    }
    rv.zoom(notches > 0 ? kZoomStepPerNotch : 1.0 / kZoomStepPerNotch, xFrac);
}
void PaneGestures::pan(int radio, float dxFrac) { view(radio).pan(dxFrac); }
void PaneGestures::resetView(int radio) { view(radio).resetView(); }

// ---------------------------------------------------------------- Workspace

int Workspace::highestSlot() const {
    for (int i = kMaxRadios - 1; i >= 0; --i) { if (occupied(i)) { return i; } }
    return -1;
}

void Workspace::readFacts(const EngineReads& engine) {
    std::string text;
    int count = 0;
    std::uint32_t mask = 0;
    bool haveFacts = false;
    if (engine.fact && engine.fact("fact.radio.count", text) >= 0) {
        haveFacts = true;
        count = std::atoi(text.c_str());
        for (int n = 1; n <= kMaxRadios; ++n) {
            std::string name = "fact.radio." + std::to_string(n) + ".state";
            std::string v;
            if (engine.fact(name.c_str(), v) >= 0) { mask |= 1u << (n - 1); }
        }
    }
    if (!haveFacts) { setRadios(1u, 1); return; }    // an engine without the facts has one radio
    // The mask is what exists; the count is the engine's word for how many. If the two disagree the mask is trusted (a number names a radio, as RADIO_FOCUS has it).
    if (mask == 0 && count > 0) { for (int i = 0; i < std::min(count, kMaxRadios); ++i) { mask |= 1u << i; } }
    setRadios(mask, std::max(count, 0));
}

void Workspace::setRadios(std::uint32_t mask, int count) {
    mask_ = mask & ((1u << kMaxRadios) - 1u);
    int n = 0;
    for (int i = 0; i < kMaxRadios; ++i) { n += ((mask_ >> i) & 1u) ? 1 : 0; }
    count_ = n;
    (void)count;    // the engine's own count; the mask is the truth used here (see readFacts)
    // the selection stays on a radio that exists: the lowest one when its radio has gone
    if (n > 0 && !occupied(radios_.selected)) {
        for (int i = 0; i < kMaxRadios; ++i) { if (occupied(i)) { radios_.selected = i; break; } }
    }
    setPage(page_);
}

void Workspace::setLayout(int layout) {
    layout_ = std::clamp(layout, 0, kLayoutCount - 1);
    page_ = radios_.selected / pageSizeOf(layout_);
    setPage(page_);
}

int Workspace::pageCount() const {
    const int size = pageSizeOf(layout_);
    const int top = highestSlot();
    return top < 0 ? 1 : top / size + 1;
}

void Workspace::setPage(int page) { page_ = std::clamp(page, 0, pageCount() - 1); }

int Workspace::shownLayout(int winW, int winH) const {
    if (layout_ == kLayoutOne || count_ <= 1) { return kLayoutOne; }
    if (winW < kMultiMinW || winH < kMultiMinH) { return kLayoutOne; }
    return layout_;
}

bool Workspace::fellBack(int winW, int winH) const {
    return layout_ != kLayoutOne && count_ > 1 && (winW < kMultiMinW || winH < kMultiMinH);
}

std::vector<PaneCell> Workspace::cells(int winW, int winH) const {
    std::vector<PaneCell> out;
    const int shown = shownLayout(winW, winH);
    if (shown == kLayoutOne) { out.push_back({radios_.selected, occupied(radios_.selected)}); return out; }
    const int size = pageSizeOf(shown);
    for (int i = 0; i < size; ++i) {
        const int slot = page_ * size + i;
        out.push_back({slot, occupied(slot)});
    }
    return out;
}

std::vector<int> Workspace::slotsToTick(int winW, int winH) const {
    std::vector<int> slots;
    const auto add = [&](int s) { if (s >= 0 && s < kMaxRadios && std::find(slots.begin(), slots.end(), s) == slots.end()) { slots.push_back(s); } };
    for (const PaneCell& c : cells(winW, winH)) { if (c.occupied) { add(c.slot); } }
    add(radios_.selected);
    return slots;
}

bool Workspace::select(int radio, const EngineReads& engine) {
    const int slot = radio - 1;
    if (!occupied(slot)) { return false; }
    if (slot != radios_.selected) {
        const int32_t status = engine.focus ? engine.focus(static_cast<std::uint32_t>(radio)) : FOXAPI_NO_DEVICE;
        if (status != FOXAPI_OK && status != FOXAPI_NO_CHANGE) { return false; }
        radios_.selected = slot;
    }
    // the page that holds the selected radio is the page shown
    const int size = pageSizeOf(layout_);
    page_ = std::clamp(slot / size, 0, pageCount() - 1);
    return true;
}

bool Workspace::stepPage(int dir, int winW, int winH, const EngineReads& engine) {
    const int step = dir < 0 ? -1 : 1;
    if (shownLayout(winW, winH) == kLayoutOne) {
        for (int t = radios_.selected + step; t >= 0 && t < kMaxRadios; t += step) {
            if (occupied(t)) { return select(t + 1, engine); }
        }
        return false;
    }
    const int before = page_;
    setPage(page_ + step);
    return page_ != before;
}

bool Workspace::fitPictures(int slot, int cellW, int cellH, float split) {
    RadioView& rv = radios_.at(slot);
    const PictureSize want = panePictures(cellW, cellH, split);
    const PictureSize& have = rv.wells.size;
    const auto closeTo = [](std::uint32_t a, std::uint32_t b) { return (a > b ? a - b : b - a) <= 8u; };
    rv.plot = panePlot(cellW, cellH, split);
    if (closeTo(have.traceW, want.traceW) && closeTo(have.traceH, want.traceH) && closeTo(have.wfW, want.wfW) && closeTo(have.wfH, want.wfH)) { return false; }
    rv.resizePictures(want);
    return true;
}

bool Workspace::restorePictures(int slot) {
    RadioView& rv = radios_.at(slot);
    const PictureSize today;
    const PictureSize& have = rv.wells.size;
    if (have.traceW == today.traceW && have.traceH == today.traceH && have.wfW == today.wfW && have.wfH == today.wfH) { return false; }
    rv.resizePictures(today);
    return true;
}

}  // namespace foxsdr::slinthost
