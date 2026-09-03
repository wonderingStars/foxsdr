// theme.hpp - the application's palette and its Dear ImGui style, in one place.
//
// WHY THIS FILE EXISTS. Until it did, FoxSDR's entire theme was three lines in
// AppWindow::run - ImGui::StyleColorsDark(), WindowRounding = 0, and WindowBg
// alpha = 1 - and every other colour in the product was a literal at the point
// it was drawn. An audit counted 172 IM_COL32 literals, 25 inline ImVec4
// colours and about fifty file-local constexpr colour constants sitting in
// anonymous namespaces where nothing could reach them. The warning amber alone
// was written out seventeen times and had already drifted into three slightly
// different hues, which is worse than one wrong colour because it implies a
// distinction that does not exist.
//
// So: one palette, named by ROLE rather than by hue, and one applyTheme() that
// sets the whole ImGuiStyle from it. A control added next year inherits the
// bench instead of arriving in default blue.
//
// WHAT THIS CANNOT REACH, and it is a lot. ImGuiStyle governs WIDGETS. Every
// instrument face in this product - the spectrum, the waterfall, the map, the
// radar scope, the counter drums, the knobs, the cabinet - is drawn straight
// into an ImDrawList, which never consults the style. Those surfaces have to
// take their colours from these constants BY NAME, one call site at a time.
// The constants are here so that work has somewhere to point.
//
// THE DESIGN. "FoxSDR Desktop - 1960s" from the design handoff: a brass and
// dark-enamel bench instrument with amber counter digits, ivory legends and a
// phosphor-green display. Every value below is measured from that reference
// rather than chosen here.
//
// ONE RULE WORTH KNOWING BEFORE USING THESE. The reference engraves its
// CAPTIONS dark-into-brass and puts its READINGS on glass - bright ink in a
// dark well. Dark-on-brass is about 2.3:1 contrast, which is fine for a label
// at rest and not acceptable for a number somebody is trying to read at arm's
// length. So: a caption may be engraved; a live figure must be on glass.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_THEME_HPP
#define CASCADE_GUI_THEME_HPP

#include "imgui.h"

namespace cascade::gui::theme {

// --- the palette, by role ----------------------------------------------------
//
// Named for what a colour MEANS, never for what it looks like. A constant
// called kBrassMid can be re-toned without every call site becoming a lie; one
// called kOlive cannot.

// The metal. Panels, keys, rails, plates - anything a hand operates.
inline constexpr ImU32 kBrassBright = IM_COL32(0x8B, 0x80, 0x69, 0xFF);  // #8B8069
inline constexpr ImU32 kBrassMid = IM_COL32(0x6E, 0x65, 0x52, 0xFF);     // #6E6552
inline constexpr ImU32 kBrassDark = IM_COL32(0x4A, 0x42, 0x34, 0xFF);    // #4A4234
inline constexpr ImU32 kBrassTint = IM_COL32(0x9C, 0x90, 0x78, 0xFF);    // #9C9078
inline constexpr ImU32 kBrassShade = IM_COL32(0x7D, 0x73, 0x60, 0xFF);   // #7D7360

// The enamel. Wells, recesses and the dark ground a reading sits on.
inline constexpr ImU32 kEnamel = IM_COL32(0x2A, 0x25, 0x1C, 0xFF);       // #2A251C
inline constexpr ImU32 kEnamelDark = IM_COL32(0x1F, 0x1B, 0x14, 0xFF);   // #1F1B14
inline constexpr ImU32 kWell = IM_COL32(0x14, 0x11, 0x0C, 0xFF);         // #14110C
inline constexpr ImU32 kVoid = IM_COL32(0x0D, 0x0B, 0x07, 0xFF);         // #0D0B07

// The ink. Engraved legends, cut into the metal.
inline constexpr ImU32 kEngraved = IM_COL32(0x3B, 0x35, 0x29, 0xFF);     // #3B3529
inline constexpr ImU32 kInkMuted = IM_COL32(0x9C, 0x90, 0x78, 0xFF);
inline constexpr ImU32 kInkFaint = IM_COL32(0x7D, 0x73, 0x60, 0xFF);

// The lettering. Ivory on metal, for anything a hand operates.
inline constexpr ImU32 kIvory = IM_COL32(0xEF, 0xE7, 0xD2, 0xFF);        // #EFE7D2
inline constexpr ImU32 kCream = IM_COL32(0xD8, 0xCF, 0xB4, 0xFF);        // #D8CFB4

// The readings. Amber is a NUMBER; nothing else may use it.
inline constexpr ImU32 kAmber = IM_COL32(0xF0, 0xA8, 0x40, 0xFF);        // #F0A840
inline constexpr ImU32 kAmberDim = IM_COL32(0x8A, 0x5A, 0x2A, 0xFF);     // #8A5A2A
inline constexpr ImU32 kGold = IM_COL32(0xD9, 0xB2, 0x3C, 0xFF);         // #D9B23C

// The display. Phosphor, for what the radio actually received.
inline constexpr ImU32 kPhosphor = IM_COL32(0x8F, 0xD9, 0xA0, 0xFF);     // #8FD9A0
inline constexpr ImU32 kPhosphorDim = IM_COL32(0x5F, 0x8A, 0x55, 0xFF);  // #5F8A55

// Trouble. Rust for a fault, and it is deliberately NOT the amber a reading
// uses - a number and a warning must never be the same colour.
inline constexpr ImU32 kAlarm = IM_COL32(0xB8, 0x55, 0x2F, 0xFF);        // #B8552F
inline constexpr ImU32 kAlarmHot = IM_COL32(0xE0, 0x7A, 0x4E, 0xFF);     // #E07A4E

// --- metrics -----------------------------------------------------------------
// A machined key has a small radius, not a round one. Everything here is in
// logical pixels at scale 1.
inline constexpr float kKeyRounding = 2.0f;
inline constexpr float kPanelRounding = 3.0f;
inline constexpr float kHairline = 1.0f;
inline constexpr float kRailThickness = 2.0f;

// --- conversions -------------------------------------------------------------
// ImGuiStyle wants ImVec4; ImDrawList wants ImU32. One converter so a colour
// cannot be transcribed differently in the two places it is used.
ImVec4 vec(ImU32 packed);
ImU32 withAlpha(ImU32 packed, float alpha);

// --- the style ---------------------------------------------------------------
//
// Sets every ImGuiCol_ and the style vars from the palette above. Call it once,
// after ImGui::CreateContext and before the first frame.
//
// TWO OVERRIDES ARE LOAD-BEARING AND MUST SURVIVE ANY EDIT HERE. This
// application runs with multi-viewport enabled and WITHOUT
// ConfigViewportsNoDecoration, so its torn-off windows - map pages, decoded
// images, the target details window, decoder output - are real operating
// system windows. WindowBg's alpha must stay 1.0 or those render translucent,
// and WindowRounding must stay 0 or the OS frame and the ImGui corner disagree
// visibly. Both were added deliberately and neither is cosmetic.
void applyTheme();

}  // namespace cascade::gui::theme

#endif  // CASCADE_GUI_THEME_HPP
