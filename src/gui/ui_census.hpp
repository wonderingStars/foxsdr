// ui_census.hpp - WHAT THE INTERFACE DREW, listed, so a test can prove that no
// theme hides a function.
//
// WHY IT EXISTS (themes, 2026-09-25). Six themes change colours, typefaces,
// corner radii and the counter's size - and the owner's rule for every one of
// them is that no theme may hide or remove a control. That is not something a
// screenshot proves: a key clipped off the end of a rail, a status card
// skipped because enlarged figures made it too tall, or a meter dropped
// because an enlarged counter pushed it off the bar all look like a design
// choice in a picture. So the parts of the main window that ARE the
// functions note themselves here as they draw - every rail bank key, every
// rail section and switch row, every lettered key, the deck's transport,
// lamps, counter, volume dial and meters, and every status card - and
// tests/test_theme_census.cpp runs the real application once per theme and
// requires the SAME list from each, and the deck's parts not to overlap.
//
// OFF UNLESS ASKED FOR: FOXSDR_UI_CENSUS=<file> (a --frames run only) turns
// it on, cycles the rail through its five banks so every bank's sections are
// drawn, and writes the list to <file> when the run ends. With it unset every
// note() is one branch on a cached bool.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_UI_CENSUS_HPP
#define CASCADE_GUI_UI_CENSUS_HPP

#include <string>

namespace cascade::gui::census {

// True when FOXSDR_UI_CENSUS names a file (read once).
bool enabled();
// A part of the interface was drawn this run. `what` is a stable name
// ("bank:2", "section:Source###source", "deck:meter.rate").
void note(const std::string& what);
// Its rectangle on screen, the last one drawn (for the overlap check).
void rect(const std::string& what, float x0, float y0, float x1, float y1);
// Writes "item <name>" lines (sorted, unique) and "rect <name> x0 y0 x1 y1"
// lines to the file FOXSDR_UI_CENSUS names. Returns false if it could not.
bool write();

}  // namespace cascade::gui::census

#endif  // CASCADE_GUI_UI_CENSUS_HPP
