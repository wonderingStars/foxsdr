// The keyboard table. See key_bindings.hpp for why it exists and why the
// defaults are HDSDR's spirit rather than HDSDR's letter.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/key_bindings.hpp"

#include <cstdio>
#include <cstring>

// The definition behind the header's opaque declaration. This is the one
// translation unit that needs the enumerators themselves.
#include <imgui.h>

namespace cascade::gui {

static_assert(kChordKeyNone == ImGuiKey_None,
              "the header's stand-in for ImGuiKey_None must be the real one");

namespace {

struct ActionRow {
    KeyAction action;
    const char* id;
    const char* name;
    const char* group;
};

// ONE ROW PER ACTION, in enum order, and the table is asserted to be complete
// and in order at the bottom of this file - a row added to the enum without a
// row here would otherwise surface as an empty name in the settings list
// rather than as a build failure.
constexpr ActionRow kActions[kKeyActionCount] = {
    {KeyAction::StartStop, "startStop", "Start / stop the receiver", "Receiver"},
    {KeyAction::Mute, "mute", "Mute the sound", "Receiver"},
    {KeyAction::VolumeUp, "volumeUp", "Volume up", "Receiver"},
    {KeyAction::VolumeDown, "volumeDown", "Volume down", "Receiver"},
    {KeyAction::SquelchUp, "squelchUp", "Squelch up", "Receiver"},
    {KeyAction::SquelchDown, "squelchDown", "Squelch down", "Receiver"},
    {KeyAction::ModeNFM, "modeNFM", "Mode: NFM (narrow FM)", "Mode"},
    {KeyAction::ModeWFM, "modeWFM", "Mode: WFM (broadcast FM)", "Mode"},
    {KeyAction::ModeAM, "modeAM", "Mode: AM", "Mode"},
    {KeyAction::ModeDSB, "modeDSB", "Mode: DSB", "Mode"},
    {KeyAction::ModeUSB, "modeUSB", "Mode: USB", "Mode"},
    {KeyAction::ModeCW, "modeCW", "Mode: CW", "Mode"},
    {KeyAction::ModeLSB, "modeLSB", "Mode: LSB", "Mode"},
    {KeyAction::ModeRAW, "modeRAW", "Mode: RAW", "Mode"},
    {KeyAction::TuneStepUp, "tuneStepUp", "Tune up one step (10 kHz)", "Tuning"},
    {KeyAction::TuneStepDown, "tuneStepDown", "Tune down one step (10 kHz)", "Tuning"},
    {KeyAction::TuneSpanUp, "tuneSpanUp", "Tune up one screen width", "Tuning"},
    {KeyAction::TuneSpanDown, "tuneSpanDown", "Tune down one screen width", "Tuning"},
    {KeyAction::QuickTune, "quickTune", "Type a frequency", "Tuning"},
    {KeyAction::ZoomIn, "zoomIn", "Zoom the spectrum in", "Picture"},
    {KeyAction::ZoomOut, "zoomOut", "Zoom the spectrum out", "Picture"},
    {KeyAction::ZoomReset, "zoomReset", "Zoom back to the full span", "Picture"},
    {KeyAction::Record, "record", "Record the audio", "Picture"},
    {KeyAction::Screenshot, "screenshot", "Save a screenshot", "Picture"},
    {KeyAction::Fullscreen, "fullscreen", "Maximise / restore the window", "Picture"},
    {KeyAction::BankSignal, "bankSignal", "Bank: SIGNAL PATH", "Rail"},
    {KeyAction::BankDecode, "bankDecode", "Bank: DECODE", "Rail"},
    {KeyAction::BankView, "bankView", "Bank: VIEW", "Rail"},
    {KeyAction::BankExtend, "bankExtend", "Bank: EXTEND", "Rail"},
    {KeyAction::BankSystem, "bankSystem", "Bank: SYSTEM", "Rail"},
    {KeyAction::OpenSettings, "openSettings", "Settings and key bindings", "Rail"},
};

struct KeyRow {
    ImGuiKey key;
    const char* name;
};

// EVERY KEY A CHORD MAY USE, and no modifier key among them: Ctrl, Shift, Alt
// and Super are what the three flags carry, so a chord whose KEY was Ctrl
// could never be pressed as anything but a modifier. The gamepad and mouse
// halves of ImGuiKey are absent for the same reason - this is a keyboard.
//
// The NAMES are ours, not ImGui::GetKeyName's: that call needs a live context
// (the settings list and the config loader both run without one in the tests),
// and its spellings are free to change with the vendored version, which a file
// format cannot be.
constexpr KeyRow kKeys[] = {
    {ImGuiKey_A, "A"}, {ImGuiKey_B, "B"}, {ImGuiKey_C, "C"}, {ImGuiKey_D, "D"},
    {ImGuiKey_E, "E"}, {ImGuiKey_F, "F"}, {ImGuiKey_G, "G"}, {ImGuiKey_H, "H"},
    {ImGuiKey_I, "I"}, {ImGuiKey_J, "J"}, {ImGuiKey_K, "K"}, {ImGuiKey_L, "L"},
    {ImGuiKey_M, "M"}, {ImGuiKey_N, "N"}, {ImGuiKey_O, "O"}, {ImGuiKey_P, "P"},
    {ImGuiKey_Q, "Q"}, {ImGuiKey_R, "R"}, {ImGuiKey_S, "S"}, {ImGuiKey_T, "T"},
    {ImGuiKey_U, "U"}, {ImGuiKey_V, "V"}, {ImGuiKey_W, "W"}, {ImGuiKey_X, "X"},
    {ImGuiKey_Y, "Y"}, {ImGuiKey_Z, "Z"},
    {ImGuiKey_0, "0"}, {ImGuiKey_1, "1"}, {ImGuiKey_2, "2"}, {ImGuiKey_3, "3"},
    {ImGuiKey_4, "4"}, {ImGuiKey_5, "5"}, {ImGuiKey_6, "6"}, {ImGuiKey_7, "7"},
    {ImGuiKey_8, "8"}, {ImGuiKey_9, "9"},
    {ImGuiKey_F1, "F1"}, {ImGuiKey_F2, "F2"}, {ImGuiKey_F3, "F3"},
    {ImGuiKey_F4, "F4"}, {ImGuiKey_F5, "F5"}, {ImGuiKey_F6, "F6"},
    {ImGuiKey_F7, "F7"}, {ImGuiKey_F8, "F8"}, {ImGuiKey_F9, "F9"},
    {ImGuiKey_F10, "F10"}, {ImGuiKey_F11, "F11"}, {ImGuiKey_F12, "F12"},
    {ImGuiKey_Tab, "Tab"},
    {ImGuiKey_LeftArrow, "Left"},
    {ImGuiKey_RightArrow, "Right"},
    {ImGuiKey_UpArrow, "Up"},
    {ImGuiKey_DownArrow, "Down"},
    {ImGuiKey_PageUp, "PageUp"},
    {ImGuiKey_PageDown, "PageDown"},
    {ImGuiKey_Home, "Home"},
    {ImGuiKey_End, "End"},
    {ImGuiKey_Insert, "Insert"},
    {ImGuiKey_Delete, "Delete"},
    {ImGuiKey_Backspace, "Backspace"},
    {ImGuiKey_Space, "Space"},
    {ImGuiKey_Enter, "Enter"},
    {ImGuiKey_Escape, "Escape"},
    {ImGuiKey_Menu, "Menu"},
    {ImGuiKey_Apostrophe, "Apostrophe"},
    {ImGuiKey_Comma, "Comma"},
    // THE TWO HDSDR WRITES AS '+' AND '-'. The physical keys are Equal and
    // Minus on a US keyboard and ImGui names them that way; the chord text
    // says Plus and Minus because that is what the volume keys DO and what
    // every shortcut list in this class of application prints. "Equal" is
    // accepted on the way in (see chordKeyFromName) so a hand-edited file
    // spelling it the other way still loads.
    {ImGuiKey_Minus, "Minus"},
    {ImGuiKey_Equal, "Plus"},
    {ImGuiKey_Period, "Period"},
    {ImGuiKey_Slash, "Slash"},
    {ImGuiKey_Semicolon, "Semicolon"},
    {ImGuiKey_LeftBracket, "LeftBracket"},
    {ImGuiKey_Backslash, "Backslash"},
    {ImGuiKey_RightBracket, "RightBracket"},
    {ImGuiKey_GraveAccent, "GraveAccent"},
    {ImGuiKey_CapsLock, "CapsLock"},
    {ImGuiKey_ScrollLock, "ScrollLock"},
    {ImGuiKey_NumLock, "NumLock"},
    {ImGuiKey_PrintScreen, "PrintScreen"},
    {ImGuiKey_Pause, "Pause"},
    {ImGuiKey_Keypad0, "Num0"}, {ImGuiKey_Keypad1, "Num1"},
    {ImGuiKey_Keypad2, "Num2"}, {ImGuiKey_Keypad3, "Num3"},
    {ImGuiKey_Keypad4, "Num4"}, {ImGuiKey_Keypad5, "Num5"},
    {ImGuiKey_Keypad6, "Num6"}, {ImGuiKey_Keypad7, "Num7"},
    {ImGuiKey_Keypad8, "Num8"}, {ImGuiKey_Keypad9, "Num9"},
    {ImGuiKey_KeypadDecimal, "NumDecimal"},
    {ImGuiKey_KeypadDivide, "NumDivide"},
    {ImGuiKey_KeypadMultiply, "NumMultiply"},
    {ImGuiKey_KeypadSubtract, "NumMinus"},
    {ImGuiKey_KeypadAdd, "NumPlus"},
    {ImGuiKey_KeypadEnter, "NumEnter"},
    {ImGuiKey_KeypadEqual, "NumEqual"},
};

constexpr int kKeyCount = static_cast<int>(sizeof(kKeys) / sizeof(kKeys[0]));

// The spellings a person or an older file might use for a key this table
// names differently. One direction only - they are read, never written - so
// formatChord still has exactly one answer per key.
constexpr KeyRow kKeyAliases[] = {
    {ImGuiKey_Equal, "Equal"},
    {ImGuiKey_Escape, "Esc"},
    {ImGuiKey_Delete, "Del"},
    {ImGuiKey_Enter, "Return"},
    {ImGuiKey_LeftArrow, "LeftArrow"},
    {ImGuiKey_RightArrow, "RightArrow"},
    {ImGuiKey_UpArrow, "UpArrow"},
    {ImGuiKey_DownArrow, "DownArrow"},
    {ImGuiKey_PageUp, "PgUp"},
    {ImGuiKey_PageDown, "PgDn"},
};

constexpr int kKeyAliasCount = static_cast<int>(sizeof(kKeyAliases) / sizeof(kKeyAliases[0]));

// Case-insensitive compare over ASCII only. std::tolower with a char argument
// is undefined for negative values, which every byte above 0x7F is on a signed
// -char build, and a chord name is ASCII by construction.
char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsIgnoreCase(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (lowerAscii(*a) != lowerAscii(*b)) { return false; }
        ++a;
        ++b;
    }
    return *a == *b;
}

// Both ends trimmed of the spaces a hand-edited file is entitled to contain.
std::string trimmed(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) { ++b; }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) {
        --e;
    }
    return s.substr(b, e - b);
}

}  // namespace

const char* keyActionId(KeyAction a) {
    const int i = static_cast<int>(a);
    if (i < 0 || i >= kKeyActionCount) { return ""; }
    return kActions[i].id;
}

const char* keyActionName(KeyAction a) {
    const int i = static_cast<int>(a);
    if (i < 0 || i >= kKeyActionCount) { return ""; }
    return kActions[i].name;
}

const char* keyActionGroup(KeyAction a) {
    const int i = static_cast<int>(a);
    if (i < 0 || i >= kKeyActionCount) { return ""; }
    return kActions[i].group;
}

bool keyActionFromId(const char* id, KeyAction& out) {
    if (id == nullptr) { return false; }
    for (int i = 0; i < kKeyActionCount; ++i) {
        // EXACT, not case-insensitive: the id is a file format key, and two
        // spellings of one key is how a file ends up with two entries for one
        // action and no way to tell which the user meant.
        if (std::strcmp(kActions[i].id, id) == 0) {
            out = kActions[i].action;
            return true;
        }
    }
    return false;
}

const char* chordKeyName(ImGuiKey key) {
    for (int i = 0; i < kKeyCount; ++i) {
        if (kKeys[i].key == key) { return kKeys[i].name; }
    }
    return "";
}

bool chordKeyFromName(const char* name, ImGuiKey& out) {
    if (name == nullptr || name[0] == '\0') { return false; }
    for (int i = 0; i < kKeyCount; ++i) {
        if (equalsIgnoreCase(kKeys[i].name, name)) {
            out = kKeys[i].key;
            return true;
        }
    }
    for (int i = 0; i < kKeyAliasCount; ++i) {
        if (equalsIgnoreCase(kKeyAliases[i].name, name)) {
            out = kKeyAliases[i].key;
            return true;
        }
    }
    return false;
}

const ImGuiKey* chordKeyTable(int& count) {
    // A static copy of the enum column, because kKeys is an array of pairs and
    // the caller wants the keys themselves. Built once, never written again.
    static ImGuiKey table[kKeyCount] = {};
    static bool filled = false;
    if (!filled) {
        for (int i = 0; i < kKeyCount; ++i) { table[i] = kKeys[i].key; }
        filled = true;
    }
    count = kKeyCount;
    return table;
}

void formatChord(const Chord& c, char* out, std::size_t cap) {
    if (out == nullptr || cap == 0) { return; }
    out[0] = '\0';
    const char* key = c.bound() ? chordKeyName(c.key) : "";
    // A chord whose key this build does not name is NOT bound to anything a
    // frame could ever match, so it prints as unbound rather than as a
    // half-written "Ctrl+" that looks like a key.
    if (key[0] == '\0') {
        std::snprintf(out, cap, "None");
        return;
    }
    std::snprintf(out, cap, "%s%s%s%s", c.ctrl ? "Ctrl+" : "", c.shift ? "Shift+" : "",
                  c.alt ? "Alt+" : "", key);
}

std::string formatChord(const Chord& c) {
    char buf[64];
    formatChord(c, buf, sizeof(buf));
    return std::string(buf);
}

bool parseChord(const char* text, Chord& out) {
    if (text == nullptr) { return false; }
    const std::string all = trimmed(std::string(text));
    out = Chord{};
    if (all.empty() || equalsIgnoreCase(all.c_str(), "None")) { return true; }

    std::size_t pos = 0;
    bool haveKey = false;
    while (pos <= all.size()) {
        const std::size_t plus = all.find('+', pos);
        const std::string token =
            trimmed(all.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos));
        if (token.empty()) { return false; }
        if (equalsIgnoreCase(token.c_str(), "Ctrl") || equalsIgnoreCase(token.c_str(), "Control")) {
            if (out.ctrl) { return false; }  // stated twice: a typo, not a chord
            out.ctrl = true;
        } else if (equalsIgnoreCase(token.c_str(), "Shift")) {
            if (out.shift) { return false; }
            out.shift = true;
        } else if (equalsIgnoreCase(token.c_str(), "Alt")) {
            if (out.alt) { return false; }
            out.alt = true;
        } else {
            ImGuiKey k = ImGuiKey_None;
            if (!chordKeyFromName(token.c_str(), k)) { return false; }
            // TWO KEYS IS NOT A CHORD. "Ctrl+A+B" is a file somebody wrote by
            // hand and got wrong; taking the last one would bind a key they
            // did not ask for.
            if (haveKey) { return false; }
            out.key = k;
            haveKey = true;
        }
        if (plus == std::string::npos) { break; }
        pos = plus + 1;
    }
    // Modifiers with no key ("Ctrl+") is not an unbound chord, it is an
    // unfinished one.
    return haveKey;
}

KeyBindings defaultKeyBindings() {
    KeyBindings kb;
    const auto set = [&kb](KeyAction a, const char* text) {
        Chord c;
        // Every literal below is parsed rather than built field by field, so
        // the defaults exercise the same parser a config file does: a default
        // this build cannot read back would fail its own round-trip test.
        parseChord(text, c);
        kb[a] = c;
    };

    // --- the receiver --------------------------------------------------------
    // HDSDR starts and stops on F2. Here F1-F5 are the FUNCTION SELECT keys,
    // engraved on the panel and named in their own tooltips, so start/stop
    // takes HDSDR's key with the one free modifier on it.
    set(KeyAction::StartStop, "Ctrl+F2");
    set(KeyAction::Mute, "M");                    // HDSDR: M
    set(KeyAction::VolumeUp, "Plus");             // HDSDR: '+'
    set(KeyAction::VolumeDown, "Minus");          // HDSDR: '-'
    set(KeyAction::SquelchUp, "Ctrl+Shift+PageUp");    // HDSDR: the same pair
    set(KeyAction::SquelchDown, "Ctrl+Shift+PageDown");

    // --- the demodulator -----------------------------------------------------
    // HDSDR's mode keys are Ctrl + the initial, and four of them land on a
    // FoxSDR mode unchanged: Ctrl+A is AM, Ctrl+C is CW, Ctrl+L is LSB,
    // Ctrl+U is USB. The three that do not:
    //
    //   Ctrl+F   HDSDR has one FM; this receiver has two. The narrow one takes
    //            the bare key because it is the one a scanner listener lives
    //            in, and WFM takes Shift with it - the wider mode on the wider
    //            chord.
    //   Ctrl+D   HDSDR's DIG. There is no DIG here; DSB is the mode whose
    //            initial it is and the nearest thing to "the carrier and both
    //            sidebands, undecoded".
    //   Ctrl+R   HDSDR spends this on RF gain, which has no keyboard control
    //            here, and spends Ctrl+E on ECSS, which this receiver does not
    //            have. RAW takes Ctrl+R on its own initial rather than
    //            inheriting an unrelated letter.
    set(KeyAction::ModeNFM, "Ctrl+F");
    set(KeyAction::ModeWFM, "Ctrl+Shift+F");
    set(KeyAction::ModeAM, "Ctrl+A");
    set(KeyAction::ModeDSB, "Ctrl+D");
    set(KeyAction::ModeUSB, "Ctrl+U");
    set(KeyAction::ModeCW, "Ctrl+C");
    set(KeyAction::ModeLSB, "Ctrl+L");
    set(KeyAction::ModeRAW, "Ctrl+R");

    // --- tuning --------------------------------------------------------------
    // HDSDR: Ctrl+Up/Down step the tune frequency by the selected step,
    // Ctrl+PageUp/PageDown move the LO by the visible spectrum width, and T
    // opens the tune-frequency entry.
    set(KeyAction::TuneStepUp, "Ctrl+Up");
    set(KeyAction::TuneStepDown, "Ctrl+Down");
    set(KeyAction::TuneSpanUp, "Ctrl+PageUp");
    set(KeyAction::TuneSpanDown, "Ctrl+PageDown");
    set(KeyAction::QuickTune, "T");

    // --- the picture ---------------------------------------------------------
    // HDSDR: Ctrl+'+' / Ctrl+'-' / Ctrl+Del for the RF spectrum zoom, Ctrl+W
    // for the screenshot, F11 for full screen, Shift+R to record a WAV.
    set(KeyAction::ZoomIn, "Ctrl+Plus");
    set(KeyAction::ZoomOut, "Ctrl+Minus");
    set(KeyAction::ZoomReset, "Ctrl+Delete");
    set(KeyAction::Record, "Shift+R");
    set(KeyAction::Screenshot, "Ctrl+W");
    set(KeyAction::Fullscreen, "F11");

    // --- the rail ------------------------------------------------------------
    // Already the keyboard's way to the banks before there was a table; the
    // table is what makes them rebindable. F7 is HDSDR's Options key and lands
    // on the SYSTEM bank with the key-binding list open - which is what makes
    // it worth having beside F5, since F5 alone leaves a user who wants to
    // change a key hunting down the rail for the row.
    set(KeyAction::BankSignal, "F1");
    set(KeyAction::BankDecode, "F2");
    set(KeyAction::BankView, "F3");
    set(KeyAction::BankExtend, "F4");
    set(KeyAction::BankSystem, "F5");
    set(KeyAction::OpenSettings, "F7");
    return kb;
}

bool findKeyBindingConflict(const KeyBindings& kb, KeyAction& first, KeyAction& second) {
    for (int i = 0; i < kKeyActionCount; ++i) {
        if (!kb.chords[i].bound()) { continue; }
        for (int j = i + 1; j < kKeyActionCount; ++j) {
            if (!kb.chords[j].bound()) { continue; }
            if (kb.chords[i] == kb.chords[j]) {
                first = static_cast<KeyAction>(i);
                second = static_cast<KeyAction>(j);
                return true;
            }
        }
    }
    return false;
}

bool keyBindingConflicts(const KeyBindings& kb, KeyAction self, const Chord& c, KeyAction& other) {
    if (!c.bound()) { return false; }
    for (int i = 0; i < kKeyActionCount; ++i) {
        if (i == static_cast<int>(self)) { continue; }
        if (kb.chords[i].bound() && kb.chords[i] == c) {
            other = static_cast<KeyAction>(i);
            return true;
        }
    }
    return false;
}

bool keyActionForChord(const KeyBindings& kb, const Chord& c, KeyAction& out) {
    if (!c.bound()) { return false; }
    for (int i = 0; i < kKeyActionCount; ++i) {
        if (kb.chords[i].bound() && kb.chords[i] == c) {
            out = static_cast<KeyAction>(i);
            return true;
        }
    }
    return false;
}

std::vector<std::string> keyBindingsToConfig(const KeyBindings& kb) {
    const KeyBindings defaults = defaultKeyBindings();
    std::vector<std::string> lines;
    for (int i = 0; i < kKeyActionCount; ++i) {
        if (kb.chords[i] == defaults.chords[i]) { continue; }
        lines.push_back(std::string(kActions[i].id) + "=" + formatChord(kb.chords[i]));
    }
    return lines;
}

KeyBindings keyBindingsFromConfig(const std::vector<std::string>& lines) {
    KeyBindings kb = defaultKeyBindings();
    // WHICH ACTIONS THIS FILE HAS ALREADY SPOKEN FOR. A second line for one
    // action is a hand-edit that says two different things; the first wins,
    // because the alternative - the last one wins - makes the outcome depend
    // on an ordering nothing in the file guarantees.
    bool seen[kKeyActionCount] = {};
    for (const std::string& raw : lines) {
        const std::size_t eq = raw.find('=');
        if (eq == std::string::npos) { continue; }
        const std::string id = trimmed(raw.substr(0, eq));
        KeyAction a = KeyAction::StartStop;
        if (!keyActionFromId(id.c_str(), a)) { continue; }
        if (seen[static_cast<int>(a)]) { continue; }
        Chord c;
        if (!parseChord(raw.substr(eq + 1).c_str(), c)) { continue; }
        kb[a] = c;
        seen[static_cast<int>(a)] = true;
    }
    return kb;
}

namespace {
// The action table is indexed by the enum, so a row in the wrong place would
// give an action somebody else's name and id - silently, and in a file format.
// Checked at compile time rather than in a test, because a test can only fail
// after the wrong id has been written to somebody's config.
constexpr bool actionTableInOrder() {
    for (int i = 0; i < kKeyActionCount; ++i) {
        if (static_cast<int>(kActions[i].action) != i) { return false; }
    }
    return true;
}
static_assert(actionTableInOrder(), "kActions must be in KeyAction order, one row per action");
}  // namespace

}  // namespace cascade::gui
