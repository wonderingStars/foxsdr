// freq_marker_layout.hpp - where the waterfall's frequency markers land: which
// are on screen, and which row along the waterfall's foot each one's tab
// ("M3 145.502") sits on. Pure, so tests/test_freq_marker_layout.cpp checks it
// without an ImGui frame; AppWindow::drawFreqMarkers only draws what this says.
//
// THE RULES, each tested:
//   - a marker outside [loHz, hiHz] is not drawn at all; a degenerate view
//     (no span, no width) draws nothing rather than dividing by zero;
//   - a tab reads rightward from its line, and flips to the line's left side
//     where it would run off the panel's right edge;
//   - tabs that would touch stack upward from the foot, row by row (the
//     bookmark names' own row rule, gui/bookmark_marker_geometry.hpp); past
//     the row budget a marker keeps its line and loses its tab, so a crowded
//     span never grows a tower of plates over the picture;
//   - what is already on a row - the waterfall's own foot plate, which the
//     first rendered check showed a tab sitting on top of - is taken as
//     occupied, so a tab moves up past it rather than covering its words.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_FREQ_MARKER_LAYOUT_HPP
#define CASCADE_GUI_FREQ_MARKER_LAYOUT_HPP

#include <cstddef>
#include <utility>
#include <vector>

#include "gui/bookmark_marker_geometry.hpp"

namespace cascade::gui {

// The most rows of tabs stacked above the waterfall's foot.
inline constexpr int kMarkerTabMaxRows = 3;

// Screen x of `hz` in a panel showing [loHz, hiHz] across [x0, x0 + width],
// or false when it is off the panel or the view is degenerate.
inline bool markerScreenX(double hz, double loHz, double hiHz, float x0, float width, float& x) {
    const double span = hiHz - loHz;
    if (!(span > 0.0) || !(width > 0.0f)) { return false; }
    if (!(hz >= loHz && hz <= hiHz)) { return false; }
    x = x0 + static_cast<float>((hz - loHz) / span) * width;
    return true;
}

struct MarkerTabIn {
    float x = 0.0f;  // the marker's line, screen x
    float w = 0.0f;  // its tab's full width, padding included
};

struct MarkerTabOut {
    int row = -1;     // 0 = the row on the foot; -1 = no tab (line only)
    float a = 0.0f;   // the tab's left edge
    float e = 0.0f;   // and its right edge
};

// Places tabs in the order given (the caller passes them left to right), each
// on the lowest row it does not touch. [panelX0, panelX1] is the panel.
// `occupied[r]` lists x extents already taken on row r before any tab is
// placed; rows past its end have nothing on them.
inline std::vector<MarkerTabOut> layoutMarkerTabs(
    const std::vector<MarkerTabIn>& in, float panelX0, float panelX1, int maxRows,
    float gapPx = 3.0f, const std::vector<std::vector<std::pair<float, float>>>& occupied = {}) {
    std::vector<MarkerTabOut> out(in.size());
    if (maxRows < 1) { return out; }
    std::vector<std::vector<std::pair<float, float>>> placed(static_cast<std::size_t>(maxRows));
    for (std::size_t r = 0; r < placed.size() && r < occupied.size(); ++r) { placed[r] = occupied[r]; }
    for (std::size_t i = 0; i < in.size(); ++i) {
        float a = in[i].x;
        float e = a + in[i].w;
        if (e > panelX1) {  // runs off the right: read leftward from the line
            e = in[i].x;
            a = e - in[i].w;
        }
        if (a < panelX0 || e > panelX1) { continue; }  // wider than the room either side
        const int row = chooseBookmarkNameRow(placed, a, e, gapPx);
        if (row < 0) { continue; }
        placed[static_cast<std::size_t>(row)].emplace_back(a, e);
        out[i] = MarkerTabOut{row, a, e};
    }
    return out;
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_FREQ_MARKER_LAYOUT_HPP
