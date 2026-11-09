// FoxSDR Slint front end - THE KEYBOARD'S DISPATCH (docs/engine-stage4.md 6.56): which shortcut a key press answers, by the SAVED key table.
//
// The window's root FocusScope (ui/main.slint) no longer knows a single chord: it names the key that was pressed (ui/keys.slint KeyNames, the
// same names the Key bindings capture stores) and asks the host, which asks this unit. The answer is the config id of the action that holds the
// chord in the table the user has saved - src/gui/app_window.cpp AppWindow::dispatchKeyBindings is the reference - so a rebinding takes effect
// the moment the capture saves it, and a cleared action answers nothing.
//
// Pure: no Slint, no engine, no config file. The table comes from keyTableFromConfig (system_logic.hpp) over the interface half's lines.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>

#include "system_logic.hpp"

namespace foxsdr::slintsystem {

// The ids the window answers that are NOT rows of the key table (the classic window hard-wires them too): the rest are the table's own ids.
constexpr const char* kShortcutEscape = "escape";          // Esc cancels what is being typed, a dialog, the full-screen scope
constexpr const char* kShortcutPalette = "palette";        // Ctrl+K opens the command palette
constexpr const char* kShortcutUiSizeAuto = "uiSizeAuto";  // Ctrl+0

// One key press as the window names it. `key` is the chord key's canonical name ("F2", "PageUp", "Plus", "M"); the modifiers are the event's.
// `glyphShifted` says the character typed needs Shift on a standard keyboard ("+" is Shift+"=", "_" Shift+"-"): Shift is then part of how
// the glyph was made, not necessarily of the chord (see shortcutForPress).
struct KeyPress {
    std::string key;
    bool ctrl = false, shift = false, alt = false;
    bool glyphShifted = false;
};

// The id of the action that holds exactly this chord, "" when none does. The FIRST action in the table's order wins a clash (the classic window's
// one-action-per-press rule, keyActionForChord); an unbound chord answers nothing; the PTT key (transmitPtt) is never answered here - it is held,
// not pressed, and the TRANSMIT page reads it itself.
std::string shortcutFor(const Chord& chord, const KeyTable& table);

// What the window does with a press, in this order:
//   1. F12 and Escape with NO modifier: "screenshot" and "escape", whatever the table says (the Key bindings footer promises both);
//   2. the saved table, the chord exactly as pressed; a glyph typed with Shift ("+") that finds nothing is tried again without the Shift;
//   3. what is fixed and not in the table: Ctrl+K (palette), Ctrl+0 (uiSizeAuto), F12 and Escape with modifiers.
// "" = the press is not answered (the window rejects it, so a field or a parent may have it).
std::string shortcutForPress(const KeyPress& press, const KeyTable& table);

}  // namespace foxsdr::slintsystem
