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

// --- the cabinet ------------------------------------------------------------
//
// The vocabulary the whole face is built from: an edge, a screw, a rail, a
// divider, a plate. Every one of them takes its colours from theme.hpp by name
// and its sizes from its arguments, so the same call draws the same object at
// any window size - the reference was measured in a 1720 x 986 artboard and
// nothing here may assume that number.
//
// WHY BEVELS ARE A FUNCTION AND NOT A HABIT. One hairline of light along the
// top and left and one of shadow along the bottom and right is the entire
// difference between a coloured rectangle and a piece of metal. Written by hand
// at each site it drifts - two pixels here, a different alpha there - and the
// panel stops reading as one machined object.

// A bevelled edge on a rounded rectangle. raised=true lights the top and left
// and shadows the bottom and right, which is how brass reads as proud of the
// panel; raised=false does the reverse, which is how a well reads as cut into
// it. Draws the edge only - the caller has already filled the shape.
void addBenchBevel(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                   float rounding, bool raised);

// A countersunk screw. slotDeg turns the slot, so four corners can differ the
// way real ones do.
void addCabinetScrew(ImDrawList* dl, const ImVec2& centre, float radius,
                     float slotDeg);

// The horizontal bevelled rail dividing one deck of the face from the next: a
// light hairline directly above a dark one.
void addBenchRail(ImDrawList* dl, float x0, float x1, float y);

// A vertical hairline separator between groups of controls in a bar: the dark
// cut with its lit far wall beside it, lit from the upper left like everything
// else on this panel.
void addBenchDivider(ImDrawList* dl, float x, float y0, float y1);

// The large round transport button: brass bezel, domed face, a specular
// highlight in the upper left, its word across the middle. Returns true on the
// frame it is pressed.
//
// IT IS A REAL ImGui ITEM. The hit area is an InvisibleButton, so the control
// hovers, takes focus, answers the keyboard and takes part in the same input
// arbitration as every other widget. A hand-drawn circle that only looks
// clickable is exactly what the top of this file forbids.
//
// THE WORD FOLLOWS THE ACTION, not the picture. `running` draws the live rust
// dome and letters it STOP; stopped draws the same dome drained of colour and
// letters it START. The reference artboard only ever shows the running state,
// and a button that said STOP while it started the receiver would be lying
// about what pressing it does.
bool drawBenchStopButton(ImDrawList* dl, const ImVec2& centre, float radius,
                         bool running);

// One small square key on the function rail, bevelled proud when off and
// pressed in when on. Returns true on the frame it is clicked, and like the
// transport button it is a real ImGui item.
//
// ITS IDENTITY COMES FROM ITS POSITION. A rail carries a dozen of these and
// they cannot share one ImGui id, so the id is derived from `tl` - two keys
// cannot occupy the same place, which makes that unique by construction and
// saves every caller from remembering to PushID.
bool drawBenchKey(ImDrawList* dl, const ImVec2& tl, float size, bool on);

// A panel plate: bevelled ground, an engraved centred title in the legend face,
// and a rule beneath it. Returns the y below the rule, so the caller lays out
// from a measurement rather than a guess.
float addBenchPlate(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                    const char* title);

// An engraved group caption inside a plate - SIGNAL PATH, DECODE and the rest.
// Small, letter-spaced, cut into the ground, with a rule carrying it out to
// `width` so the caption reads as the head of a group rather than as a stray
// word.
void addBenchGroupCaption(ImDrawList* dl, const ImVec2& at, float width,
                          const char* caption);

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
//
// `unitLabel` is the tiny legend printed beside the pivot - "MS/s", "ms" - the
// way a moving-coil meter names its own scale on its face. It is OPTIONAL and
// defaults to nothing, because a unit is a claim about what the needle
// measures: a caller with no honest unit for its scale must not be handed a
// plausible one to print.
void drawBenchMeter(ImDrawList* dl, const ImVec2& tl, float width, float height,
                    const char* caption, float frac01, bool haveReading,
                    const char* valueLine, const char* unitLabel = nullptr);

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
