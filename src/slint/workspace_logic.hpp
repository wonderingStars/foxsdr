// workspace_logic.hpp - THE RADIO WORKSPACE'S LOGIC (docs/multi-radio-records/screen-s3.md, stage S3): which radios there are, which layout is shown, which radios are on
// screen, which one is selected, what a pane says in words, and how big its pictures are. Pure arithmetic over the engine's answers: it reads nothing and draws nothing, so
// the host tests drive it with a made-up engine (a table of std::functions) and the window's publisher (workspace_publish.cpp) turns its answers into the model of
// ui/workspace.slint.
//
// THE RULES (design 3.1 to 3.5, owner decisions of 2026-11-10):
//  - the number of radios comes from the engine: fact.radio.count and, for each radio that exists, fact.radio.<n>.state. An engine without those facts has one radio.
//  - LAYOUTS: One (today's screen, the selected radio), Two (side by side) and Four (2 x 2). A radio's pane exists only in a multi-pane layout; never more than four panes.
//  - PAGES: a page holds as many radios as the layout has panes (Four: 1-4, 5-8; Two: 1-2, 3-4, 5-6, 7-8). A page is offered only when a radio lives beyond the first one.
//  - SELECTING a radio is the session's focus: RADIO_FOCUS goes to the engine and the window's `selected` follows only when the engine took it.
//  - THE SMALL WINDOW: a multi-pane layout needs 1280 x 720; below it the window shows One (the chosen layout returns when the window is wide again).
//  - A pane that is not on screen is not read and not painted; the selected radio is read every tick, because the strip, the panels and the banks show it.
//  - PICTURES: in a multi-pane layout each pane's two pictures are fitted to the pane's own plot (design 4, S0: 0.4 ms of host work for four panes against 1.0 ms).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "foxsdr_api.h"
#include "radio_view.hpp"

namespace foxsdr::slinthost {

// ---- the layouts ----
constexpr int kLayoutOne = 0;     // today's receiver view: the selected radio
constexpr int kLayoutTwo = 1;     // two panes side by side
constexpr int kLayoutFour = 2;    // 2 x 2
constexpr int kLayoutCount = 3;

// The window a multi-pane layout needs (design 3.5 item 8, a starting value): below it the window shows One.
constexpr int kMultiMinW = 1280;
constexpr int kMultiMinH = 720;

int panesOf(int layout);          // the panes on screen: One 1, Two 2, Four 4 (anything else 1)
int pageSizeOf(int layout);       // the radios a page holds: One 1, Two 2, Four 4

// One cell of the shown layout: the radio slot (0-based) it holds and whether a radio is there. An empty cell is the seam of stage S4's "Add a radio" card.
struct PaneCell {
    int slot = 0;
    bool occupied = false;
};

// ---- the pane's geometry: the numbers of ui/radio_pane.slint and the views inside it (header 30, readout row 28, splitter 6; the plot's gutter 44, the ribbon 8 + 15, the axis 18, the waterfall's foot 28 + 4) ----
constexpr int kPaneHeaderH = 30;
constexpr int kPaneReadoutH = 28;
constexpr int kPaneSplitterH = 6;
constexpr int kPaneGutterW = 44;
constexpr int kPaneSpectrumTop = 23;      // the ribbon (small: the main screen ships ribbon-size 1, 8 px) and its names (15) above the compact spectrum plot
constexpr int kPaneSpectrumFoot = 18;     // the axis under the plot
constexpr int kPaneWaterfallFoot = 32;    // the foot band and its gap under the waterfall's picture
constexpr float kPaneSplitDefault = 0.45f;

// A pane of `cellW` x `cellH` pixels: the plot the axes are laid out over, and the pictures fitted to it. The trace fills the spectrum's plot, the waterfall picture the
// waterfall well above its foot. Never below 16 pixels either way.
PlotSize panePlot(int cellW, int cellH, float split = kPaneSplitDefault);
PictureSize panePictures(int cellW, int cellH, float split = kPaneSplitDefault);

// ---- what a pane says, in words (every sentence through tr(); every control names its radio) ----
struct PaneTexts {
    std::string prefix, selectLabel, stopLabel, spectrumLabel, waterfallLabel, resizeLabel, closeLabel;
    std::string runTip, decTip, muteTip, failTip;
    std::string device, frequency, mode, gain, squelch, volume, rate;
    bool running = false, decoding = false, muted = false, failed = false, biasTee = false;
};
// `gain` is the figure when the host knows it ("32 dB", "AUTO"); empty falls back to what the state alone says (AUTO with the device's AGC on, else "--"). `radio` is 1-based.
PaneTexts paneTexts(int radio, const FoxReceiverState& st, const std::string& gain);

// The words of the workspace bar: "Radios 1-4" and "Show radios 1 to 4" for a page (first and last radio of it, 1-based), and the small-window notice.
std::string pageLabel(int firstRadio, int lastRadio);
std::string pageTip(int firstRadio, int lastRadio);
std::string smallWindowNotice();

// ---- the panes' gestures: what each one asks of the engine, aimed at the pane's radio ----
// The wheel's zoom factor per notch: the receiver view's (main.cpp kZoomPerNotch is this value).
constexpr double kZoomStepPerNotch = 1.3;

// A gesture in a pane is the receiver view's gesture over THAT radio's picture: the pane's own zoom and state, the command aimed at the pane's radio (FoxCommand.target = its number, radio_view.hpp
// aimAt) and the selection untouched. Selecting is the workspace's. `submit` takes the command and the radio's number (1-based).
class PaneGestures {
public:
    using Submit = std::function<void(const FoxCommand&, std::uint32_t radio)>;
    PaneGestures(RadioSet& radios, Submit submit) : radios_(radios), submit_(std::move(submit)) {}
    void stop(int radio);                                   // RUN, the opposite of what the radio does now
    void click(int radio, float xFrac, bool shift);         // click to tune: VFO_TO_ABSOLUTE at the pointer's frequency (shift: no snap)
    void bandDrag(int radio, float dxFrac, int tick);       // SET_VFO_OFFSET, the VFO moves inside the baseband; remembers the tick so the picture does not follow the hand
    void edgeDrag(int radio, float halfFrac);               // SET_BANDWIDTH_DRAG
    void wheel(int radio, float xFrac, int notches, bool shift);   // zoom about the pointer; with shift one step of the mode's raster (STEP_TUNE)
    void pan(int radio, float dxFrac);
    void resetView(int radio);

private:
    RadioView& view(int radio) { return radios_.at(radio - 1); }
    RadioSet& radios_;
    Submit submit_;
};

// ---- the engine, as the workspace sees it: a table of functions, so the real one is the API table and a test's is made up ----
struct EngineReads {
    // the text of a fact ("fact.radio.count"); a negative answer means the engine has no such fact
    std::function<int(const char* name, std::string& out)> fact;
    // the engine's answer to RADIO_FOCUS for a radio (1-based): FOXAPI_OK, FOXAPI_NO_CHANGE, FOXAPI_NO_DEVICE ...
    std::function<int32_t(std::uint32_t radio)> focus;
};

class Workspace {
public:
    explicit Workspace(RadioSet& radios) : radios_(radios) {}

    // ---- the radios the engine has ----
    // Reads fact.radio.count and fact.radio.<n>.state (n = 1..kMaxRadios). An engine with no facts has radio 1.
    void readFacts(const EngineReads& engine);
    // The same from an answer already in hand: a mask (bit i = slot i) and a count. Keeps the selection on a radio that exists, and the page on a page that exists.
    void setRadios(std::uint32_t mask, int count);
    int radioCount() const { return count_; }
    std::uint32_t mask() const { return mask_; }
    bool occupied(int slot) const { return slot >= 0 && slot < kMaxRadios && ((mask_ >> slot) & 1u) != 0; }
    int highestSlot() const;                  // -1 with none

    // ---- the layout and the page ----
    int layout() const { return layout_; }    // the chosen layout; what is SHOWN is shownLayout()
    void setLayout(int layout);               // keeps the page that holds the selected radio
    int page() const { return page_; }
    void setPage(int page);                   // clamped to the pages there are
    int pageCount() const;                    // of the chosen layout; 1 when no radio lives beyond the first page
    int shownLayout(int winW, int winH) const;       // after the small-window fallback and the one-radio rule
    bool fellBack(int winW, int winH) const;         // a multi-pane layout was chosen, there are radios to show, and the window is too small
    std::vector<PaneCell> cells(int winW, int winH) const;   // the shown layout's cells: One = the selected radio, else the page's panes
    // The slots whose pictures the tick reads and paints: the occupied cells, plus the selected radio when it is not one of them (the strip shows it). Each once.
    std::vector<int> slotsToTick(int winW, int winH) const;

    // ---- selecting ----
    int selectedRadio() const { return radios_.selected + 1; }   // 1-based
    // Selects radio `radio` (1-based): asks the engine (RADIO_FOCUS) and follows only when it answers OK or NO_CHANGE. The page that holds it becomes the page shown.
    // False when no such radio is open or the engine refused; the selection is then untouched.
    bool select(int radio, const EngineReads& engine);
    // The page keys (Alt+PageUp / Alt+PageDown, screen stage S5): `dir` -1 or +1. In a multi-pane layout that is shown, the previous / next page (clamped at the ends, no wrap); where One is shown
    // (the layout, or the small-window fallback) a page IS a radio, so the key selects the previous / next radio that exists. True when something moved.
    bool stepPage(int dir, int winW, int winH, const EngineReads& engine);

    // ---- pictures ----
    // Fits a radio's pictures to a pane of cellW x cellH (see panePictures); the history restarts only when the size changed by more than a few pixels. True if it was refitted.
    bool fitPictures(int slot, int cellW, int cellH, float split = kPaneSplitDefault);
    // Back to the picture size of today's screen (layout One). True if it was refitted.
    bool restorePictures(int slot);

private:
    RadioSet& radios_;
    int layout_ = kLayoutOne;
    int page_ = 0;
    int count_ = 1;
    std::uint32_t mask_ = 1u;
};

}  // namespace foxsdr::slinthost
