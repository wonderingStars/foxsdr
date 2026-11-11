// mark_layout.hpp - THE NAMES OF PLUGIN MARKS ON THE SPECTRUM AND THE WATERFALL, DECLUTTERED (docs/multi-radio-records/screen-s1.md, lane S1 item 4; design section 6, the screen part).
//
// WHY. A plugin may set up to CASCADE_MAX_MARKERS_PER_PLUGIN (64) marks and there may be several plugins; a station list (EiBi, a DX cluster) puts hundreds of them in one 2.4 MHz window. Their
// names were drawn as one line of text at the mark's line with no collision handling, so close marks drew over each other. The rule here:
//   * a name starts at its line and takes `chars * charPx + gap` pixels; names are placed from left to right into up to `maxRows` rows, each into the first row it fits in after the name before it;
//   * a name that fits no row is COLLAPSED: not drawn, and counted into a "+N" tab. Collapsed names within `tabPx` of each other share one tab, which sits on the tab row (the row under the last
//     name row) at the first of them; the tab's click lists the names it stands for;
//   * a mark with no name has no name to place (its line is drawn as ever); a mark outside the window (pos < 0 or > 1) is not placed.
// The marks' lines are not touched: every mark keeps its line whatever happens to its name. The number of marks is not limited here (the ABI limits it, per plugin).
//
// Pure: no Slint, no engine. The cost is O(n log n) in the marks (a sort), measured at 0, 64 and 256 marks in test_slint_host_marks.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace foxsdr::slintmarks {

struct MarkName {
    double pos = 0;          // 0..1 across the plot: where the mark's line is (a span's left edge)
    std::size_t chars = 0;   // the name's length in characters (UTF-8 code points)
};

struct MarkTab {
    double pos = 0;                  // where the tab sits (the first collapsed name's line)
    std::vector<std::size_t> items;  // the marks (indices into the input) the tab stands for, left to right
};

struct MarkLayout {
    std::vector<int> row;            // per input mark: its row 0..maxRows-1, or -1 = no name is drawn (no name, outside the window, or collapsed)
    std::vector<MarkTab> tabs;       // the "+N" tabs, left to right
};

// plotPx: the plot's width in pixels; charPx: one character's width; gapPx: the space kept after a name; tabPx: the width of a "+N" tab (names collapsed closer than this share a tab).
MarkLayout declutter(const std::vector<MarkName>& marks, double plotPx, double charPx, int maxRows, double gapPx = 4.0, double tabPx = 30.0);

// Characters (UTF-8 code points) in a name.
std::size_t characterCount(const std::string& s);

// A mark's tooltip: "<name>  <frequency>" and, when the mark came from a named plugin, a second line that says so (`sourceLine`, the translated sentence "from <plugin>", "" = none). The credit
// a plugin owes its data source (HamQTH asks for one) is the plugin's own to show: the marker ABI carries a 32-byte name and no credit field (ENGINE CHANGES WANTED in the S1 record).
std::string tooltipText(const std::string& name, const std::string& hz, const std::string& sourceLine);

}  // namespace foxsdr::slintmarks
