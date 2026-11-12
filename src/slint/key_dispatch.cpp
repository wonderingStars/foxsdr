// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "key_dispatch.hpp"

namespace foxsdr::slintsystem {

namespace {

// The PTT key is held while the TRANSMIT page has the focus and nowhere else; dispatching it as a press would key the radio from anywhere
// and nothing would ever unkey it (AppWindow::dispatchKeyBindings skips it for the same reason).
bool isPtt(int index) { return std::string(keyAction(index).id) == "transmitPtt"; }

}  // namespace

std::string shortcutFor(const Chord& chord, const KeyTable& table) {
    if (!chord.bound()) { return {}; }
    // Table order, first holder wins: the classic window fires ONE action per press, and "the first one wins" is its answer to a clash (keyActionForChord).
    for (int i = 0; i < kKeyActionCount; ++i) {
        if (isPtt(i)) { continue; }
        const Chord& want = table[static_cast<std::size_t>(i)];
        if (want.bound() && want == chord) { return keyAction(i).id; }
    }
    return {};
}

std::string shortcutForPress(const KeyPress& press, const KeyTable& table) {
    if (press.key.empty()) { return {}; }   // a key no chord can use (a modifier alone, an unnamed key)
    const bool bare = !press.ctrl && !press.shift && !press.alt;
    // 1. The two keys the Key bindings footer promises: F12 always saves a screenshot, Esc always cancels - whatever the table says.
    if (bare && press.key == "F12") { return keyAction(keyActionIndexOf("screenshot")).id; }
    if (bare && press.key == "Escape") { return kShortcutEscape; }
    // 2. The saved table, the chord exactly as pressed.
    Chord c;
    c.key = press.key;
    c.ctrl = press.ctrl;
    c.shift = press.shift;
    c.alt = press.alt;
    std::string id = shortcutFor(c, table);
    if (!id.empty()) { return id; }
    // ...and a glyph that needs Shift to type ("+" is Shift+"=") tried without it: the Shift made the character, it was not asked for as a modifier.
    // The capture stores the Shift it saw, so a chord the user bound WITH Shift has already answered above.
    if (press.glyphShifted && press.shift) {
        c.shift = false;
        id = shortcutFor(c, table);
        if (!id.empty()) { return id; }
    }
    // 3. What is fixed and not in the table.
    if (press.ctrl && !press.shift && !press.alt && press.key == "K") { return kShortcutPalette; }
    if (press.ctrl && !press.shift && !press.alt && press.key == "0") { return kShortcutUiSizeAuto; }
    if (press.key == "F12") { return keyAction(keyActionIndexOf("screenshot")).id; }
    if (press.key == "Escape") { return kShortcutEscape; }
    if (press.shift && !press.ctrl && !press.alt && press.key == "F1") { return kShortcutOverlay; }   // not in the table (F1 itself is "Bank: SIGNAL PATH"): Shift+F1 asks for the overlay unless the table gave it to an action above
    return {};
}

EscapeStep escapeStep(const EscapeState& open) {
    if (open.importDialog) { return EscapeStep::ImportDialog; }
    if (open.storePluginPage) { return EscapeStep::StorePluginPage; }
    return EscapeStep::Overlays;
}

}  // namespace foxsdr::slintsystem
