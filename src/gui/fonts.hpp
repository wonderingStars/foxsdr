// fonts.hpp - the three typefaces the bench is lettered in.
//
// WHY THIS EXISTS. Until it did, FoxSDR drew every word in ProggyClean, the
// 13-pixel bitmap face Dear ImGui falls back to when an application supplies
// none. That face is a fine default and completely wrong for this product: it
// is a 2000s programmer's terminal font sitting inside a 1960s instrument, it
// has one weight, and its digits are the same width and colour as its letters
// so a READING and a LEGEND look identical. The design handoff names its
// typefaces explicitly, and this is where they arrive.
//
// THE THREE ROLES, and they map onto the palette's rule one for one:
//
//   ui()       Saira Condensed Medium.  Everything a hand operates, and all
//              prose. Condensed because a bench panel is a crowded thing and
//              the design is drawn at 9-11px in a scaled artboard; Medium
//              rather than the design's Regular because at real size on dark
//              enamel a condensed 400 goes thin and grey, and a legend a user
//              squints at is a worse fault than one a shade too heavy.
//
//   legend()   Saira Condensed SemiBold. The engraved captions, section
//              plates and the maker's plate - the design's own weight 600.
//
//   reading()  Nova Mono. DIGITS, and very nearly nothing but. A counter's
//              figures sit on glass in a monospaced face so they stop
//              jittering sideways as they change, which is the single most
//              visible difference between an instrument and a form.
//
//              THE NARROWNESS OF THAT RULE IS MEASURED, NOT FASTIDIOUS. Nova
//              Mono draws its capital M as three close stems in a monospaced
//              cell; below about 20px they merge and the letter rasterises as
//              a solid block. "MUTED" in a status card came out as a filled
//              rectangle followed by UTED. So: figures take this face, words
//              take ui() whatever they are reporting, and a value carrying
//              units - "2.000 MS/s" - is a word for this purpose. The amber
//              of a reading is what carries the meaning; the monospacing is
//              only there to stop digits dancing.
//
// SIZES LIVE HERE, not at the call sites. A panel where three captions are
// 14px and a fourth is 15 because someone typed it twice is exactly the drift
// the theme file was written to stop.
//
// Dear ImGui 1.92 bakes a face at whatever size it is asked for, so each
// typeface is added to the atlas ONCE and drawn at any size through
// PushFont(font, size). There is no atlas cost to a new size, only to a new
// face.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_FONTS_HPP
#define CASCADE_GUI_FONTS_HPP

#include "imgui.h"

namespace cascade::gui::fonts {

// --- the sizes, by role ------------------------------------------------------
// Measured against the design handoff and then checked at 100% Windows scaling
// against the widths the layout already hard-codes.
//
// RAISED BY TWO POINTS THROUGHOUT, on the report that captions were hard to
// read. The design is drawn at 9-11 px in a scaled artboard and those figures
// were carried across too literally: on a real panel at 100% scaling they are
// small, and the engraved tones make them smaller still, because a caption cut
// dark into brass is about 2.3:1 contrast and is carrying most of the loss.
// Size is the half of that this file controls.
//
// EVERY HARD-CODED WIDTH IN THE INTERFACE WAS MEASURED AGAINST THE OLD NUMBERS.
// Changing them here is one edit and a sweep: keys, plates, chips, drum
// apertures, meter faces and card heights all have to be re-checked, because
// text that no longer fits does not wrap, it clips. Do not raise these again
// without walking the same surfaces.
//
// RAISED BY THREE PIXELS ACROSS THE BOARD in 0.79.0, at the user's request
// ("make the font size larger across the whole software by about 3px"),
// which was the third such request in a month and the largest. The sweep
// above was walked again: the rail, the status cards, the plates, the bench
// engravings and the scope's panels were all screenshotted at the new sizes.
inline constexpr float kUiSize = 21.0f;       // controls, prose, table cells
inline constexpr float kLegendSize = 19.0f;   // engraved captions, small plates
inline constexpr float kReadingSize = 20.0f;  // a number on glass
inline constexpr float kTinySize = 17.0f;     // the smallest engraving that
                                              // still has to be readable

// --- the faces ---------------------------------------------------------------
//
// NONE OF THESE EVER RETURNS NULL. Before load() has run, or after it failed,
// each returns whatever face is currently bound - so a missing typeface
// degrades to the wrong typeface and never to a crash. That matters more than
// it looks: ImFont::CalcTextSizeA is how half the instrument faces in this
// application position their own text, it takes no null, and a guard at every
// one of those call sites is a guard that will eventually be forgotten at one.
//
// Call them inside a frame. They ask ImGui for the current font on the
// fallback path, which needs a context.
ImFont* ui();
ImFont* legend();
ImFont* reading();

// Adds all three faces to the current atlas and makes ui() the default.
//
// Call once, after ImGui::CreateContext and before the first frame. Returns
// false if the atlas refused a face; the application is still perfectly usable
// in that case, wearing ImGui's own font, and callers should say so rather
// than abort.
bool load();

}  // namespace cascade::gui::fonts

#endif  // CASCADE_GUI_FONTS_HPP
