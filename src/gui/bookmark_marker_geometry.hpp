// bookmark_marker_geometry.hpp - the row a clashing bookmark name stacks onto,
// pulled out of AppWindow::drawBookmarkMarkers so it can be checked without an
// ImGui frame or a spectrum to draw one on.
//
// WHY THIS FILE EXISTS. Two bookmarks a couple of kHz apart on a wide-span
// spectrum land a few pixels from one another; the un-stacked code drew the
// first name and silently dropped the second one's text (its tick still
// showed) the moment their extents touched. An Italian user asked for both to
// be readable. The fix is NOT "make room" - the spectrum has none to give -
// it is "try the row below": up to three rows of names between the panel's
// header and the tick strip, favourites still drawn first so they still win
// whichever row they land on.
//
// Same split as gui/page_geometry.hpp: WHAT the geometry should be is decided
// here, in functions with no ImGui in them; HOW it is applied (walking the
// bookmark list, measuring text, drawing it) stays in AppWindow::
// drawBookmarkMarkers.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_BOOKMARK_MARKER_GEOMETRY_HPP
#define CASCADE_GUI_BOOKMARK_MARKER_GEOMETRY_HPP

#include <cstddef>
#include <utility>
#include <vector>

namespace cascade::gui {

// THE MOST ROWS A NAME CAN STACK ONTO with "Stack close names" on. Three, not
// unbounded: a dense span would otherwise grow a tower of names nobody could
// read either, and the request was "read both names", not "read every name".
inline constexpr int kBookmarkNameMaxRows = 3;

// THE GAP a candidate extent must clear on either side of an already-placed
// one before the two are read as separate words rather than one run-together
// smear - unchanged from the single-row rule this replaces.
inline constexpr float kBookmarkNameGapPx = 6.0f;

// Chooses which row a candidate name's horizontal extent [a, e] lands on.
//
// placedByRow[r] holds the [start, end) extents already placed on row r, in
// the same left-to-right pixel units as a/e. Rows are tried in order
// starting at 0 (so a name always prefers the row nearest the spectrum's
// header over one further down), and the first row the candidate does not
// clash on - within gapPx of any extent already there - wins. -1 means every
// row in placedByRow clashed, which is when the caller leaves the name off
// (its tick still shows) exactly as the un-stacked code always did for its
// one row.
//
// placedByRow.size() IS the row budget for this call: pass a vector of one
// to get today's single-row behaviour (stacking off, or a spectrum too short
// to offer a second row), and up to kBookmarkNameMaxRows to allow stacking.
inline int chooseBookmarkNameRow(const std::vector<std::vector<std::pair<float, float>>>& placedByRow,
                                  float a, float e, float gapPx = kBookmarkNameGapPx) {
    for (std::size_t r = 0; r < placedByRow.size(); ++r) {
        bool clash = false;
        for (const auto& p : placedByRow[r]) {
            if (a < p.second + gapPx && e + gapPx > p.first) {
                clash = true;
                break;
            }
        }
        if (!clash) { return static_cast<int>(r); }
    }
    return -1;
}

// HOW MANY ROWS FIT between the label row's own y (labelY, where row 0 always
// sits - it is drawn there unconditionally today, however little room is
// left below it) and the top of the tick strip, at the given row pitch.
//
// Row 0 is always available (returns at least 1, even for availableHeight
// <= 0 - the short-spectrum case the un-stacked code already lived with).
// Each row after that costs one more rowPitch of the space between labelY
// and the strip; the count never exceeds `cap` (kBookmarkNameMaxRows in the
// real call, or 1 to turn stacking off outright).
inline int bookmarkNameRowCapacity(float availableHeight, float rowPitch, int cap) {
    if (cap < 1) { cap = 1; }
    int rows = 1;
    if (rowPitch > 0.0f && availableHeight > 0.0f) {
        rows += static_cast<int>(availableHeight / rowPitch);
    }
    if (rows > cap) { rows = cap; }
    if (rows < 1) { rows = 1; }
    return rows;
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_BOOKMARK_MARKER_GEOMETRY_HPP
