// page_geometry.hpp - the two size decisions AppWindow::beginPage makes about
// a torn-off page, pulled out so they can be checked without an ImGui frame.
//
// WHY THIS FILE EXISTS. A page's body is only drawn when the window is big
// enough to hold one: beginPage falls back to a bare brass strip below 80 px
// in either direction, and the well child is skipped when the inset leaves it
// under 8 px. Nothing used to stop a page being dragged to exactly that, and
// the resize grip on these windows is deliberately invisible - so a beta
// tester dragged the ACARS printer down to its title strip and could not get
// it back ("somehow I messed up the window size of the ACARS printer and
// cannot get it back", 2026-09-09). The floor below and the reset generation
// below are the two halves of the answer: one stops it happening, the other
// gets an already-wrong window back.
//
// Same split as gui/tune_control.hpp: WHAT the geometry should be is decided
// here, in functions with no ImGui in them; HOW it is applied stays in
// beginPage.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_PAGE_GEOMETRY_HPP
#define CASCADE_GUI_PAGE_GEOMETRY_HPP

#include <cstdint>

namespace cascade::gui {

// THE FLOOR A PAGE CANNOT BE DRAGGED UNDER. 80 x 80 is where beginPage stops
// drawing a body at all; these are that cut-off plus enough window left over
// to take hold of an edge and drag it back out again. They are one pair of
// numbers because the live window constraint and the remembered restore
// rectangle must agree - a window held at 240 x 140 that restores to 300 x 30
// would put the user straight back in the trap.
inline constexpr float kPageMinW = 240.0f;
inline constexpr float kPageMinH = 140.0f;

// Clamp a remembered page size up to the floor. Used on PageChrome::restoreW /
// restoreH, which are copied off the live window when a page is rolled up or
// maximised: a page that was already too small when its maximise key was
// pressed would otherwise restore just as small.
inline void clampPageSize(float& w, float& h) {
    if (!(w >= kPageMinW)) { w = kPageMinW; }  // NaN-safe: !(NaN >= x) is true
    if (!(h >= kPageMinH)) { h = kPageMinH; }
}

// WHETHER THIS PAGE STILL OWES THE USER A RE-PLACEMENT, and the counter that
// makes it happen exactly once.
//
// "Reset window sizes" on the fitted-modules window bumps one generation
// counter for the whole application; each page carries the generation it last
// acted on. A page whose counter is behind is re-placed on its next frame and
// then agrees again - which is what makes this work for a page that is not
// even being drawn when the key is pressed: it catches up whenever it is next
// opened, rather than needing to be found and told.
inline bool pageNeedsReset(std::uint32_t& seenGen, std::uint32_t currentGen) {
    if (seenGen == currentGen) { return false; }
    seenGen = currentGen;
    return true;
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_PAGE_GEOMETRY_HPP
