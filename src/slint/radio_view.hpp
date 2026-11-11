// radio_view.hpp - the per-radio host struct of the multi-radio screen (docs/multi-radio-records/screen-s1-s2.md, S1).
//
// Everything the Slint host keeps about ONE radio's picture lives in a RadioView: the receiver state it last read, the spectrum bins and the
// frame sequence, the trace state, the two alternating pixel buffers of the trace and the waterfall, the zoom and span window, the cache of
// published model signatures, and the figures (axes, ribbon, marks, peak readout) derived from them. The host holds one per radio slot
// (RadioSet); the frame tick runs the per-pane work for each visible pane and the strip, panels and banks once for the selected radio.
//
// This file holds the data and the arithmetic that needs no engine call: reading the engine is main.cpp's, and in the host the first
// radio's reads are today's read_state / read_spectrum (the per-radio reads arrive with the engine's stage 3). Nothing here assumes a number
// of radios beyond kMaxRadios, and nothing assumes a plot size: the plot (the pixels an axis is laid out over) and the painted picture
// sizes are arguments.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <slint.h>

#include "foxsdr_api.h"
#include "host_logic.hpp"
#include "signatures.hpp"

namespace foxsdr::slinthost {

static_assert(sizeof(Rgba) == sizeof(slint::Rgba8Pixel));

// The most radios the window can show: the engine's receiver limit (docs/multi-radio-engine-design.md, the limit of eight). Slots are
// created on first use, so the limit costs nothing until a radio is shown.
constexpr int kMaxRadios = 8;

// The pixels an axis is laid out over (a tick's pitch, a name's stacking). Passed to the figures; today's screen is 980 x 330.
struct PlotSize {
    int w = 980;
    int h = 330;
};

// The painted size of a radio's two pictures (image-fit fill scales them to the plot). Today's screen: 1024 x 340 trace, 1024 x 238 waterfall.
struct PictureSize {
    std::uint32_t traceW = 1024, traceH = 340;
    std::uint32_t wfW = 1024, wfH = 238;
};

// Two persistent buffers per well, alternated: the one the window still shows is never written, so begin() never
// detaches (copies) and nothing is allocated per frame. `detaches` counts the times it did anyway.
struct WellBuffers {
    PictureSize size;
    std::array<slint::SharedPixelBuffer<slint::Rgba8Pixel>, 2> trace;
    std::array<slint::SharedPixelBuffer<slint::Rgba8Pixel>, 2> waterfall;
    std::array<const void*, 2> tracePtr{nullptr, nullptr};
    std::array<const void*, 2> wfPtr{nullptr, nullptr};
    std::vector<Rgba> picture;                  // the canonical waterfall picture
    int which = 0;
    int detaches = 0;
    int frames = 0;

    explicit WellBuffers(PictureSize s = {})
        : size(s),
          trace{slint::SharedPixelBuffer<slint::Rgba8Pixel>(s.traceW, s.traceH), slint::SharedPixelBuffer<slint::Rgba8Pixel>(s.traceW, s.traceH)},
          waterfall{slint::SharedPixelBuffer<slint::Rgba8Pixel>(s.wfW, s.wfH), slint::SharedPixelBuffer<slint::Rgba8Pixel>(s.wfW, s.wfH)},
          picture(static_cast<std::size_t>(s.wfW) * s.wfH, Rgba{0x06, 0x07, 0x0a, 255}) {}

    Rgba* traceMemory() {
        auto& b = trace[static_cast<std::size_t>(which)];
        auto* p = b.begin();
        if (tracePtr[static_cast<std::size_t>(which)] != nullptr && tracePtr[static_cast<std::size_t>(which)] != p) { ++detaches; }
        tracePtr[static_cast<std::size_t>(which)] = p;
        return reinterpret_cast<Rgba*>(p);
    }
    Rgba* waterfallMemory() {
        auto& b = waterfall[static_cast<std::size_t>(which)];
        auto* p = b.begin();
        if (wfPtr[static_cast<std::size_t>(which)] != nullptr && wfPtr[static_cast<std::size_t>(which)] != p) { ++detaches; }
        wfPtr[static_cast<std::size_t>(which)] = p;
        return reinterpret_cast<Rgba*>(p);
    }
    slint::Image traceImage() { return slint::Image(trace[static_cast<std::size_t>(which)]); }
    slint::Image waterfallImage() { return slint::Image(waterfall[static_cast<std::size_t>(which)]); }
    void flip() { which ^= 1; ++frames; }
};

// What a picture shows besides the two images, derived from the radio's state, view and the shared lists. Plain data: the host turns it into
// the main window's properties (layout One) or, later, one element of the pane model. `newFrame` says the engine published a frame this tick,
// and the frame-only fields (caption, span, foot, age ticks, peak) are meaningful only then.
struct FigureBand { float from = 0, to = 0; std::string label; };
struct FigureBookmark { float pos = 0; std::string label; bool favourite = false; int row = 0; };
struct FigureMark { float pos = 0, width = 0; std::string label; std::uint8_t a = 0, r = 0, g = 0, b = 0; bool dashed = false; };
struct FigureMarker { float pos = 0; std::string label; };

struct PaneFigures {
    bool newFrame = false;
    ViewWindow view{0, 0};                      // the window the axes and overlays were laid out for
    std::string spectrumCaption, spanText, footLeft, peakText, peakAge;
    bool peakAgeSet = false, showPeak = false;
    std::vector<Tick> ageTicks, freqTicks, dbTicks;
    float vfoPos = 0, vfoHalf = 0;
    std::vector<FigureBand> ribbon;
    std::vector<FigureBookmark> bookmarks;
    std::vector<FigureMark> marks, wfMarks;
    std::vector<FigureMarker> markers;
    int markerCount = 0;
    std::string keyFoot, footTuned, footRest;
    std::vector<std::string> keyLabels;
};

struct RadioView {
    int slot = 0;                               // 0-based; the radio's number on screen and in a command's target is slot + 1
    FoxReceiverState state{};                   // the radio's last read state
    std::vector<float> bins = std::vector<float>(16384);
    std::vector<float> frame;                   // the last published frame (dBFS per bin)
    FoxSpectrumInfo lastInfo{};
    std::uint64_t sinceSeq = 0;
    TraceState traceState;
    ViewWindow view{0, 0};                      // the zoomed window; spanHz 0 = the full span
    WellBuffers wells;
    std::deque<std::int64_t> frameTimes;        // publish stamps for the lines/s and history figures
    std::int64_t lastFrameMs = 0;
    // THE PICTURE FOLLOWS A RETUNE THAT LEAVES IT (core/view_follow.hpp): the tuned frequency as the last tick saw it (NaN before the first),
    // and the tick of the last drag of the band, so the picture is never moved under the hand.
    double lastTunedHz = std::numeric_limits<double>::quiet_NaN();
    int lastBandDragTick = -1000;
    Signatures sigs;                            // the signatures of this radio's published figures (layout One: the main window's properties)
    Signatures paneSigs;                        // the same for this radio's row in the workspace's model (workspace_publish.cpp): a second cache, so the two targets never share one
    PlotSize plot;
    PaneFigures fig;

    explicit RadioView(int slotIndex = 0, PictureSize pictures = {}) : slot(slotIndex), wells(pictures) {
        state.structSize = sizeof(state);
        lastInfo.structSize = sizeof(lastInfo);
    }

    // ---- the view window and its fractions ----
    double fullCentre() const { return lastInfo.spanHz > 0 ? lastInfo.centreHz : state.centreHz; }
    double fullSpan() const { return lastInfo.spanHz > 0 ? lastInfo.spanHz : state.sampleRateHz; }
    ViewWindow currentView() const {
        if (view.spanHz > 0 && view.spanHz < fullSpan()) { return view; }
        return {fullCentre(), fullSpan()};
    }
    float frac(double hz) const { const ViewWindow v = currentView(); return static_cast<float>(fracOf(hz, v.centreHz, v.spanHz)); }
    double hzAtFrac(double f) const { const ViewWindow v = currentView(); return hzAt(f, v.centreHz, v.spanHz); }
    double dbAtFrac(double f) const {
        if (frame.empty() || !(lastInfo.spanHz > 0)) { return state.dbMin; }
        const double hz = hzAtFrac(f);
        const double fullFrac = fracOf(hz, lastInfo.centreHz, lastInfo.spanHz);
        const std::size_t i = static_cast<std::size_t>(std::clamp(fullFrac, 0.0, 0.999999) * static_cast<double>(frame.size()));
        return frame[std::min(i, frame.size() - 1)];
    }
    void zoom(double factor, double aboutFrac) {
        view = zoomAbout(currentView(), fullCentre(), fullSpan(), factor, aboutFrac, std::max(1.0, state.sampleRateHz / 1000.0));
        if (view.spanHz >= fullSpan() * 0.999) { view = {0, 0}; }
    }
    void pan(double dFrac) {
        view = panBy(currentView(), fullCentre(), fullSpan(), dFrac);
        if (view.spanHz >= fullSpan() * 0.999) { view = {0, 0}; }
    }
    void resetView() { view = {0, 0}; }
    // THE PICTURES REFITTED (docs/multi-radio-records/screen-s3.md): new buffers of the new size, the trace's held lines and the waterfall's history cleared (what a hidden page does),
    // and the next frame painted at once. The window shows the old images until the next publish.
    void resizePictures(PictureSize s) {
        wells = WellBuffers(s);
        resetTrace(traceState);
        sinceSeq = 0;
    }
};

// A command aimed at a radio: the radio's number (slot + 1) goes in FoxCommand.target; 0 leaves it alone, which is the selected radio (the engine's focus).
inline void aimAt(FoxCommand& c, std::uint32_t radioNumber) {
    if (radioNumber != 0) { c.target = radioNumber; }
}
inline std::uint32_t radioNumberOf(const RadioView& rv) { return static_cast<std::uint32_t>(rv.slot) + 1u; }

// The host's radio slots: created on first use, so a radio nobody shows costs nothing (a RadioView holds about 5.7 MB of pictures).
class RadioSet {
public:
    RadioView& at(int slot) {
        const std::size_t i = static_cast<std::size_t>(std::clamp(slot, 0, kMaxRadios - 1));
        if (!views_[i]) { views_[i] = std::make_unique<RadioView>(static_cast<int>(i), pictures_); }
        return *views_[i];
    }
    bool exists(int slot) const { return slot >= 0 && slot < kMaxRadios && views_[static_cast<std::size_t>(slot)] != nullptr; }
    int created() const { int n = 0; for (const auto& v : views_) { n += v ? 1 : 0; } return n; }
    // The picture size a radio created from now on gets (the pane size, fixed by a later measurement).
    void setPictureSize(PictureSize s) { pictures_ = s; }
    int selected = 0;                           // the slot whose values the strip, the panels and the banks show
    RadioView& selectedView() { return at(selected); }

private:
    std::array<std::unique_ptr<RadioView>, kMaxRadios> views_;
    PictureSize pictures_;
};

}  // namespace foxsdr::slinthost
