// patch_view.hpp - the patch canvas: the drawing and the pointer, nothing else.
//
// The geometry is in patch_view_math.hpp and the rules are in
// core/patch_graph.hpp; this is the third layer, and the only one that needs a
// graphics context. Keeping it this thin is what lets the other two be tested
// with expected numbers instead of screenshots.
//
// It is a FREE FUNCTION over a graph and an interaction state rather than a
// class, and AppWindow owns both. A canvas that owned the patch would make the
// patch reachable only while the page is open, and the page is closed far more
// often than the radios are.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_PATCH_VIEW_HPP
#define CASCADE_GUI_PATCH_VIEW_HPP

#include "core/patch_graph.hpp"
#include <vector>

#include "core/patch_plan.hpp"
#include "gui/patch_view_math.hpp"
#include "imgui.h"

namespace cascade::gui::patch {

// Interaction lives in patch_view_math.hpp, because it holds no ImGui types
// and app_window.hpp - which owns one - deliberately does not include imgui.h.

// What a refusal should say, in words a person can act on. Never returns null.
const char* refusalText(core::patch::Connect why);

// The word engraved on a node's title bar: RADIO, CHANNEL, DEMOD and so on.
const char* kindCaption(core::patch::NodeKind k);

// A colour per port type, taken from the bench palette rather than invented:
// phosphor is what the radio received, so it carries I/Q and audio; text and
// control are lettering colours because they carry no samples.
ImU32 portColour(core::patch::PortType t);

// Draws the canvas into the current window, filling `size` from `origin`
// (screen coordinates), and handles the pointer. The graph may be modified:
// nodes move, wires are made and cut.
// `plan` is this frame's compile() of the same graph. The canvas marks the
// nodes it names; it never computes one itself, because the plan needs the
// radio's rate and centre and the canvas has no business knowing those.
void drawPatchCanvas(core::patch::Graph& g, Interaction& ui,
                     const core::patch::Plan& plan,
                     const std::vector<NodeReading>& readings, ImVec2 origin,
                     ImVec2 size);

// The patch a page opens with when the user has none: the receiver they are
// already using, with nothing wired. An empty canvas gives no clue what a node
// even is; one node does, and one node is not an opinion about what they want.
void seedDefaultPatch(core::patch::Graph& g, const char* radioName);

}  // namespace cascade::gui::patch

#endif  // CASCADE_GUI_PATCH_VIEW_HPP
