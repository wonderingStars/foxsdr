// bench_rail.hpp - the function rail's row and hint, for AppWindow sections
// written outside app_window.cpp.
//
// benchSection() and benchHint() live in an anonymous namespace in
// app_window.cpp, which is right for the nineteen sections drawn there and
// wrong for one drawn in its own file (app_window_language.cpp): it would
// have to draw its rail row by hand, and a second implementation of the row
// is how the rail's sections come to look different from each other. These
// two forward to the originals, defined beside them in app_window.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_BENCH_RAIL_HPP
#define CASCADE_GUI_BENCH_RAIL_HPP

namespace cascade::gui {

// benchSection(): draws one row of the rail and returns whether the section
// is open. `chipText` null draws neither chip nor lamp. `lampColour` is an
// ImU32 (theme::kPhosphor and friends); spelled as its underlying type so
// this header needs no ImGui.
bool railSection(const char* label, bool defaultOpen, const char* chipText,
                 unsigned int lampColour, bool lampLit);

// benchHint(): one line in the bench's quieter ink.
void railHint(const char* text);

}  // namespace cascade::gui

#endif  // CASCADE_GUI_BENCH_RAIL_HPP
