// key_bindings.hpp - the keyboard, as a table a user can rewrite.
//
// WHY THIS EXISTS. Until now the application answered five keys, all of them
// hard-wired where they were needed: F1-F5 chose a rail bank inside
// drawRailBankKeys, F12 took a screenshot inside the render loop, Escape
// cancelled the frequency editor and left scope mode. Nothing else on the
// deck had a key at all - starting the receiver, muting it, changing mode or
// moving the tuning meant finding the control with the mouse - and nothing
// could be rebound, because there was no table to rebind. The owner asked for
// "key bindings similar to [HDSDR] for the basic stuff, also have a section in
// the settings menu where you can change the key bindings", so the keyboard
// becomes one table: an action list, a chord per action, and the two
// conversions (chord to text, text to chord) that let the table live in
// config.json and be drawn as key caps in the SYSTEM bank.
//
// WHY THE DEFAULTS ARE NOT HDSDR's EXACTLY. HDSDR's F2 starts and stops the
// radio; on this deck F1-F5 are ENGRAVED on the five FUNCTION SELECT keys and
// named in their tooltips ("SIGNAL PATH (F1)"), so a default that stole F2
// would make the panel lie about itself. Start/stop therefore takes Ctrl+F2 -
// HDSDR's key with the one modifier that is free - and every other default
// follows HDSDR where FoxSDR has the same control to offer. Each departure is
// stated beside the default it changes, in defaultKeyBindings().
//
// PURE, AND IT DOES NOT INCLUDE imgui.h. ImGuiKey is the vocabulary a chord is
// written in - it is the enum the frame will ask about - but this header is
// held by value inside AppWindow, and app_window.hpp is included by the tests
// under a standing rule that it pulls in neither GLFW nor ImGui (see its own
// note, and rail_banks.hpp / freq_scale.hpp / tune_control.hpp, which are
// ImGui-free for the same reason). So the enum is OPAQUELY DECLARED below -
// legal since C++11 for an enum with a fixed underlying type, and a
// redeclaration imgui.h is free to complete wherever both are included - and
// nothing here needs a context, an io or a widget. Everything is a value
// conversion, so tests/test_key_bindings.cpp can pin the whole keyboard -
// every default, every round trip, every conflict - without a GL context or an
// open frame. The window owns the dispatch (AppWindow::dispatchKeyBindings)
// and the drawing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_KEY_BINDINGS_HPP
#define CASCADE_GUI_KEY_BINDINGS_HPP

#include <cstddef>
#include <string>
#include <vector>

// The opaque declaration. imgui.h spells this `enum ImGuiKey : int { ... }`;
// naming the underlying type here makes this a complete type with no
// enumerators, which is all a chord's storage needs, and leaves imgui.h free to
// define the enumerators in any translation unit that also includes it.
enum ImGuiKey : int;

namespace cascade::gui {

// ImGuiKey_None, spelled without imgui.h. Asserted against the real enumerator
// in key_bindings.cpp, which does include it, so a vendored ImGui that ever
// renumbered its "no key" value would fail the build rather than quietly make
// every chord unbound.
inline constexpr ImGuiKey kChordKeyNone = static_cast<ImGuiKey>(0);

// THE BASIC OPERATIONS, and deliberately only those. Every entry here is a
// control a user reaches for while listening - the transport, the sound, the
// demodulator, the tuning, the picture, the five banks - and every one of them
// already has a mouse path on the deck that this list calls rather than
// reimplements. Nothing that needs a dialog, a file or a device is in it.
//
// The ORDER is the order the settings section lists them in, grouped the way a
// hand works: transport and sound, then modes, then tuning, then the picture,
// then the banks. The numeric values are NOT a contract - config.json stores
// the id string from keyActionId(), never the number - so this list may be
// reordered or extended without touching a single saved file.
enum class KeyAction : int {
    StartStop = 0,
    Mute,
    VolumeUp,
    VolumeDown,
    SquelchUp,
    SquelchDown,
    ModeNFM,
    ModeWFM,
    ModeAM,
    ModeDSB,
    ModeUSB,
    ModeCW,
    ModeLSB,
    ModeRAW,
    TuneStepUp,
    TuneStepDown,
    TuneSpanUp,
    TuneSpanDown,
    QuickTune,
    ZoomIn,
    ZoomOut,
    ZoomReset,
    Record,
    Screenshot,
    Fullscreen,
    BankSignal,
    BankDecode,
    BankView,
    BankExtend,
    BankSystem,
    OpenSettings,
    Count
};

inline constexpr int kKeyActionCount = static_cast<int>(KeyAction::Count);

// The id written to config.json. STABLE: renaming one silently unbinds every
// user who had rebound that action, so treat these as the file format they are.
const char* keyActionId(KeyAction a);

// The words in the settings list. Plain language, because the list is read by
// somebody looking for "the one that mutes it", not by somebody who knows what
// an enumerator is called.
const char* keyActionName(KeyAction a);

// The group heading the settings section files this action under.
const char* keyActionGroup(KeyAction a);

// id string -> action. False for anything the id table does not carry, which
// is how a config written by a newer build (or hand-edited) loses only the
// lines it cannot understand.
bool keyActionFromId(const char* id, KeyAction& out);

// ONE KEY AND ITS MODIFIERS. Super/Windows is deliberately absent: it is the
// desktop's modifier on both platforms this ships to, and a binding that
// fights the window manager is a binding that works on one machine.
struct Chord {
    ImGuiKey key = kChordKeyNone;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;

    // An UNBOUND action - what Backspace in the capture box leaves behind, and
    // what a garbage config line degrades to rather than a key nobody pressed.
    bool bound() const { return key != kChordKeyNone; }
};

inline bool operator==(const Chord& a, const Chord& b) {
    return a.key == b.key && a.ctrl == b.ctrl && a.shift == b.shift && a.alt == b.alt;
}
inline bool operator!=(const Chord& a, const Chord& b) { return !(a == b); }

// The key's name in a chord string - "PageUp", "F2", "Plus", "A". Returns an
// empty string for a key the table does not name, which includes every
// modifier key: Ctrl is a modifier, never the key of a chord, so a capture box
// that saw one has seen nothing yet.
const char* chordKeyName(ImGuiKey key);

// Name -> key, case-insensitively, accepting the aliases a hand-edited file or
// a person is likely to write ("Esc", "Del", "Equal", "LeftArrow", "Return").
// False when nothing matches.
bool chordKeyFromName(const char* name, ImGuiKey& out);

// Every key a chord may use, in the order the name table carries them. The
// capture box walks this to find which key went down, and the round-trip test
// walks it to prove every one of them survives being written and read back.
const ImGuiKey* chordKeyTable(int& count);

// "Ctrl+Shift+PageUp". Modifiers always in that order so two chords that are
// the same chord always produce the same text - the settings list, the config
// file and the README would otherwise be able to disagree. An unbound chord
// writes "None".
void formatChord(const Chord& c, char* out, std::size_t cap);
std::string formatChord(const Chord& c);

// Text -> chord. Tolerant of spaces and of case, strict about everything else:
// a token that is neither a modifier nor a named key fails the whole parse
// rather than producing a chord the user did not write. "None" (and the empty
// string) parse to an unbound chord, which is a SUCCESS - being unbound is a
// state a user can choose.
bool parseChord(const char* text, Chord& out);

// The whole keyboard: one chord per action, indexed by the enum.
struct KeyBindings {
    Chord chords[kKeyActionCount];

    const Chord& operator[](KeyAction a) const { return chords[static_cast<int>(a)]; }
    Chord& operator[](KeyAction a) { return chords[static_cast<int>(a)]; }
};

// The shipped table. See the note at the top of the file for why it is
// HDSDR's spirit rather than HDSDR's letter, and key_bindings.cpp for the
// reasoning on each individual departure.
KeyBindings defaultKeyBindings();

// TWO ACTIONS ON ONE CHORD. Reported rather than prevented: the settings
// section shows it in red beside the offending row and lets the user decide,
// because the alternative - silently refusing the key they just pressed - is
// the behaviour people describe as "the rebind didn't work". Unbound actions
// never conflict with each other. Returns the FIRST pair in enum order.
bool findKeyBindingConflict(const KeyBindings& kb, KeyAction& first, KeyAction& second);

// Would assigning `c` to `self` collide with something else? `other` names the
// action it would collide with. The capture box asks this about the key under
// the cursor before it is committed.
bool keyBindingConflicts(const KeyBindings& kb, KeyAction self, const Chord& c, KeyAction& other);

// THE LOOKUP THE FRAME USES. A chord the user just pressed -> the action it
// means. Modifiers match EXACTLY: M is not Ctrl+M, so a user pressing Ctrl+M
// for something else never mutes the radio by accident. With a conflicting
// table the lowest action in enum order wins, deterministically, so a rig in
// that state behaves the same way every launch.
bool keyActionForChord(const KeyBindings& kb, const Chord& c, KeyAction& out);

// --- config.json's side ------------------------------------------------------
//
// A list of "actionId=chord" lines rather than an object, because AppConfig
// already carries four string lists and sanitises them all through one rule
// (see sanitisePluginNames in core/config.cpp); a fifth costs nothing and a
// bespoke object would need its own.
//
// ONLY WHAT DIFFERS FROM THE DEFAULTS IS WRITTEN. A file that records every
// binding freezes the defaults the day it was written: a later build that
// improves one would improve it for nobody who had ever run the old one. This
// is the same argument config.hpp makes for pluginMuteOverride, and it has the
// same consequence - a default that CHANGES moves under a user who never
// rebound it, which is the correct behaviour for "I never touched that one".
std::vector<std::string> keyBindingsToConfig(const KeyBindings& kb);

// Lines -> table. Starts from the defaults and overwrites only what the lines
// can be understood to say: an unknown id, a missing '=', an unparseable chord
// or a duplicate line for one action are each dropped on their own, never
// taking the rest of the table with them.
KeyBindings keyBindingsFromConfig(const std::vector<std::string>& lines);

}  // namespace cascade::gui

#endif  // CASCADE_GUI_KEY_BINDINGS_HPP
