// instrument_face.hpp - the faces drawn for CASCADE_CAP_INSTRUMENT windows.
//
// A plugin that declares an instrument hands the host a kind and a handful of
// figures and words (plugin_abi.h, the INSTRUMENT section, is the contract);
// this is where each kind becomes a piece of equipment on the bench. One
// function per kind, each in its own file, so the pager and the fax machine
// can be worked on by different hands without either touching the other -
// the dispatch below is the only place they meet.
//
// EVERY FACE OBEYS THE THREE RULES THE REST OF THE BENCH OBEYS:
//   - a caption is engraved, a live figure is on glass (theme.hpp explains);
//   - "no reading" is drawn as no reading - a blank cell, a flag, a dark lamp
//     - never as a zero, because "we measured nothing" and "we have not
//     measured" are opposite claims and have been confused here before;
//   - nothing on a face is a picture of a control. A lamp that lights has a
//     reason to, a counter counts something real, and a face never invents
//     a figure the plugin did not supply.
//
// The GENERIC face is what a kind this host does not know gets: every filled
// slot on glass, labelled by its slot number, plus the lamps. It is also what
// each kind's file draws until that kind has a face of its own.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_INSTRUMENT_FACE_HPP
#define CASCADE_GUI_INSTRUMENT_FACE_HPP

#include <cstdint>

#include "core/plugin_ui.hpp"
#include "imgui.h"

namespace cascade::gui {

// What a face is told beyond the plugin's state: whether the event the state
// describes has been looked at yet (the window was open when it arrived), and
// the wall clock in seconds for anything that blinks or holds.
struct InstrumentCue {
    bool unread = false;   // state.seq has advanced since this window was last drawn
    double nowSec = 0.0;   // ImGui::GetTime()
};

// Draws the face for `in.kind` between `tl` and `br` on `dl`. Returns the
// height it used, so the caller can lay the instrument's memory beneath it.
// The rectangle is the caller's to size; a face that needs less draws less
// and returns less, and one that needs more scales its drawing down rather
// than over the edge.
float drawInstrumentFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                         const cascade::core::HostInstrument& in, const InstrumentCue& cue);

// The plain readout, and the fallback for every kind above.
float drawGenericFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                      const cascade::core::HostInstrument& in, const InstrumentCue& cue);

// One per kind, each in instrument_<kind>.cpp. Same contract as above.
float drawPagerFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                    const cascade::core::HostInstrument& in, const InstrumentCue& cue);
float drawTeleprinterFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                          const cascade::core::HostInstrument& in, const InstrumentCue& cue);
float drawToneAlertFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                        const cascade::core::HostInstrument& in, const InstrumentCue& cue);
float drawNavBearingFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                         const cascade::core::HostInstrument& in, const InstrumentCue& cue);
float drawFaxFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                  const cascade::core::HostInstrument& in, const InstrumentCue& cue);
float drawBeaconFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                     const cascade::core::HostInstrument& in, const InstrumentCue& cue);
float drawMeterFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                    const cascade::core::HostInstrument& in, const InstrumentCue& cue);
float drawWeatherConsoleFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                             const cascade::core::HostInstrument& in,
                             const InstrumentCue& cue);

// The word the rail's chip shows for an instrument row: NEW while an event
// has arrived that the window has not shown, else the kind's own idle word
// (a pager says the unread count, a course indicator says the radial, and so
// on). Pure, so tests can pin it. `out` must hold at least 16 bytes.
void instrumentChip(const cascade::core::HostInstrument& in, bool unread, char* out,
                    std::size_t cap);

}  // namespace cascade::gui

#endif  // CASCADE_GUI_INSTRUMENT_FACE_HPP
