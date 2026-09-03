// scope_face.hpp - the enclosure drawn around the radar scope: the bezel's
// gauges, the range knob and the power button.
//
// SEPARATE FROM scope_view.hpp ON PURPOSE. That header is deliberately free of
// ImGui: it holds the scope's arithmetic - ranges, bearings, projections,
// readout formatting - and its purity is what lets tests/test_scope_view.cpp
// include it and exercise 286 checks without a graphics context. Drawing
// declarations put there broke the build the moment they were added, which is
// the separation doing its job. Anything needing an ImDrawList lives here.
//
// EVERY CONTROL DECLARED HERE DOES SOMETHING. A knob that only looks like a
// knob is worse than no knob: it invites a hand and then refuses it. The range
// knob steps the same ladder the toolbar buttons do, the power button starts
// and stops the receiver, and each gauge is fed by a real measurement and says
// which. Nothing here is a picture of a control.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_SCOPE_FACE_HPP
#define CASCADE_GUI_SCOPE_FACE_HPP

#include "imgui.h"

namespace cascade::gui {

// One vertical linear gauge, of the kind flanking the screens on the
// photographed instrument. `frac` is 0..1 of full scale and `label` names what
// is measured, because a bare bar is decoration and a named one is an
// instrument.
//
// WITH NO READING IT DRAWS THE SCALE AND NO BAR. A bar sitting at zero says
// "we measured, and it is nothing", which is the opposite of "we have not
// measured" - a conflation this product has already been bitten by once, in a
// panel that rendered clean zeroes when it had no data at all.
void drawScopeGauge(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                    const char* label, double frac, bool haveReading,
                    const char* readout);

// A machined bay: the recessed panel every group of controls sits in. Two-stop
// vertical gradient, a hairline border and a one-pixel top highlight, which is
// what separates a moulded panel from a dark rectangle. `lower` picks the
// lower deck's warmer pair (#22231b -> #131409) over the upper deck's
// (#1c1d16 -> #101108); the design uses both and they are not interchangeable.
void addScopeBay(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, bool lower);

// One odometer drum group - the RANGE NM and TRACKS counters on the maker's
// plate. `digits` cells, each showing one digit of `value`, right-aligned and
// zero-padded exactly as a mechanical counter does.
void drawScopeDrums(ImDrawList* dl, const ImVec2& tl, float cellW, float cellH,
                    int digits, int value, const char* caption);

// The RANGE knob. Returns the number of ladder steps asked for this frame:
// negative anticlockwise, positive clockwise, zero for no change. Drag and
// wheel both work; the caller keeps keyboard-reachable buttons beside it,
// because a knob alone cannot be operated without a mouse.
// `fraction` is where the control currently sits on its own travel, 0 at the
// anticlockwise stop and 1 at the clockwise one. The pointer is drawn at that
// angle, so the knob SHOWS its setting rather than merely accepting changes to
// it - a dial whose index never moves is a picture of a dial.
int drawScopeKnob(ImDrawList* dl, const ImVec2& centre, float radius,
                  const char* label, const char* valueText, bool interactive,
                  float fraction);

// The ladder's own numbers, laid out on the arc the knob's pointer sweeps, with
// the selected one picked out. Without them the knob is a dial with no scale -
// it can be turned, and nothing on the panel says what turning it selects.
void drawScopeKnobTicks(ImDrawList* dl, const ImVec2& centre, float radius,
                        const int* values, int count, int selectedIndex);

// --- the 1960s bench --------------------------------------------------------
//
// A second palette, and deliberately not the scope's. The radar face is a
// phosphor instrument; the bench the receiver itself sits on is brass, dark
// walnut-brown and amber nixie-style digits, which is what the handoff's
// "FoxSDR Desktop - 1960s" reference specifies. Keeping the two apart is the
// point: the round green thing is the picture the radio made, and everything
// around it is the machine that made it.

// One digit of the tuned-frequency counter, in its own machined aperture.
// `bright` is false for the leading zeros ahead of the first significant
// figure, which are dimmed so the eye reads only the live value.
void drawFreqDrumCell(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, char digit,
                      bool bright, float fontPx);

// The well the whole counter is recessed into.
void drawFreqDrumWell(ImDrawList* dl, const ImVec2& tl, const ImVec2& br);

// The VOLUME dial: brass ring, cream pointer, and a tick arc. Returns the new
// value in 0..1, or a negative number when the user did not move it.
float drawBrassVolumeKnob(ImDrawList* dl, const ImVec2& centre, float radius,
                          float value);

// One indicator lamp with its engraved caption beneath. `lit` drives the
// colour; `caption` is drawn whatever the state, because a lamp whose meaning
// is carried by colour alone cannot be read in a greyscale photograph and is
// unreadable to about one man in twelve.
void drawBenchLamp(ImDrawList* dl, const ImVec2& centre, float radius, ImU32 colour,
                   bool lit, const char* caption);

// One analogue meter: cream tombstone face, brass bezel, a tick arc and a
// needle, with a caption above and a value line below.
//
// `frac01` is the needle's position on its own travel and `haveReading` says
// whether there is one at all. WITH NO READING NO NEEDLE IS DRAWN - a needle
// resting at zero claims "we measured, and it is nothing", which is the
// opposite of "we have not measured", and this product has been bitten by
// exactly that conflation before.
//
// The caller must drive frac01 from the SAME figure it prints in valueLine. A
// moving needle beside a number it disagrees with is worse than no meter: the
// reference artboard does precisely that - its PROCESSOR needle animates
// through 44-62% of arc while the text beside it reads 22% - and it is the one
// thing from that file that must not be copied.
void drawBenchMeter(ImDrawList* dl, const ImVec2& tl, float width, float height,
                    const char* caption, float frac01, bool haveReading,
                    const char* valueLine);

// The chip and lamp that finish a rail section's plate - the state of that
// section, read without opening it.
void drawRailChip(ImDrawList* dl, const ImVec2& headerMin, const ImVec2& headerMax,
                  const char* chipText, ImU32 lampColour, bool lampLit);

// The POWER button: the RECEIVER's run state, lit while it runs. Not the
// application's power and not the way out of the mode - a button that quit
// FoxSDR, or silently left the scope, would be lying about what it controls.
// Returns true when pressed.
bool drawScopePowerButton(ImDrawList* dl, const ImVec2& centre, float radius,
                          bool running);

}  // namespace cascade::gui

#endif  // CASCADE_GUI_SCOPE_FACE_HPP
