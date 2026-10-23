// The keyboard table - see gui/key_bindings.hpp for why the application has
// one at all.
//
// WHAT IS PINNED HERE, and why each of them is worth a check:
//
//   The action table. Every action has an id, a name and a group, the ids are
//   unique, and every id reads back to the action it came from. The ids are a
//   FILE FORMAT - config.json stores them - so a duplicate or a missing one is
//   a user's rebind silently landing on the wrong control.
//
//   The chord round trip. Every key the table names, under all eight modifier
//   combinations, must survive being written and read back unchanged. This is
//   the property the config file depends on and the settings list prints, and
//   it is the one that a new key added to the name table could quietly break.
//
//   The defaults. All bound, no two on one chord, and none of them on Escape -
//   which the frequency editor and scope mode both answer as "cancel", and
//   which therefore cannot be handed to anything else.
//
//   Conflict detection and the frame lookup, including the case that matters
//   most in practice: modifiers match EXACTLY, so Ctrl+M is not M.
//
//   The config round trip, including the lines a hand-edit produces - an
//   unknown id, no '=', an unreadable chord, the same action twice.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstring>
#include <string>
#include <vector>

// The enumerators themselves: key_bindings.hpp declares ImGuiKey opaquely (see
// the note at its top), so a test that names ImGuiKey_PageUp asks for the
// definition itself.
#include <imgui.h>
#include <imgui_internal.h>

#include "gui/fonts.hpp"
#include "gui/key_bindings.hpp"
#include "test_check.hpp"

using cascade::gui::Chord;
using cascade::gui::KeyAction;
using cascade::gui::KeyBindings;
using cascade::gui::chordKeyFromName;
using cascade::gui::chordKeyName;
using cascade::gui::chordKeyTable;
using cascade::gui::defaultKeyBindings;
using cascade::gui::findKeyBindingConflict;
using cascade::gui::formatChord;
using cascade::gui::kKeyActionCount;
using cascade::gui::keyActionForChord;
using cascade::gui::keyActionFromId;
using cascade::gui::keyActionGroup;
using cascade::gui::keyActionId;
using cascade::gui::keyActionName;
using cascade::gui::keyBindingConflicts;
using cascade::gui::keyBindingsFromConfig;
using cascade::gui::keyBindingsToConfig;
using cascade::gui::parseChord;

namespace {

Chord chordOf(const char* text) {
    Chord c;
    CHECK(parseChord(text, c));
    return c;
}

}  // namespace

// --- a text field that holds the keys ------------------------------------------
//
// Headless ImGui, as test_plugin_settings_ui drives the settings form: a
// context with no renderer, one window, a READ-ONLY multiline box (the report
// page's diagnostics preview, drawn with the same flag) and an editable box
// beside it. The question is asked where dispatchKeyBindings asks it - right
// after NewFrame, before any widget of the frame is submitted - about the item
// the previous frame left active.
//
// THE 0.99.84 FAULT, pinned: with the read-only box active, io.WantTextInput
// is false (ImGui raises it only for a field that could take typing), and the
// dispatcher that asked only that let Ctrl+A and Ctrl+C through to the AM and
// CW chords. textFieldHoldsKeys must say true there.
void textFieldHoldsKeysTest() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    cascade::gui::fonts::load();

    char preview[128] = "FoxSDR diagnostics bundle\nversion: 0.99.84\nmode: AM\n";
    char contact[64] = "M0XYZ";
    bool focusPreview = false;
    bool focusContact = false;
    bool held = false;      // textFieldHoldsKeys() at the dispatcher's moment
    bool wantText = false;  // io.WantTextInput at the same moment
    auto frame = [&]() {
        ImGui::NewFrame();
        held = cascade::gui::textFieldHoldsKeys();
        wantText = io.WantTextInput;
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(600.0f, 400.0f), ImGuiCond_Always);
        ImGui::Begin("report", nullptr, ImGuiWindowFlags_NoTitleBar);
        if (focusPreview) {
            ImGui::SetKeyboardFocusHere();
            focusPreview = false;
        }
        ImGui::InputTextMultiline("##preview", preview, sizeof(preview), ImVec2(-1.0f, 140.0f),
                                  ImGuiInputTextFlags_ReadOnly);
        if (focusContact) {
            ImGui::SetKeyboardFocusHere();
            focusContact = false;
        }
        ImGui::InputText("##contact", contact, sizeof(contact));
        ImGui::Button("Send");
        ImGui::End();
        ImGui::Render();
    };
    auto frames = [&](int n) {
        for (int i = 0; i < n; ++i) { frame(); }
    };

    // Nothing active: the keys are free.
    frames(2);
    CHECK(ImGui::GetActiveID() == 0);
    CHECK(!wantText);
    CHECK(!held);

    // The read-only preview takes the focus. It IS the active item (text can
    // be selected and copied from it), ImGui does not ask for text input, and
    // the keys are held all the same.
    focusPreview = true;
    frames(3);
    CHECK(ImGui::GetActiveID() != 0);
    CHECK(!wantText);
    CHECK(held);

    // Ctrl+A in that state - the keystroke of GitHub issue 6 - leaves the
    // field active and the keys held.
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_A, true);
    frame();
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    frame();
    CHECK(ImGui::GetActiveID() != 0);
    CHECK(held);

    // The editable field: held, and this time ImGui asks for text input too.
    focusContact = true;
    frames(3);
    CHECK(ImGui::GetActiveID() != 0);
    CHECK(wantText);
    CHECK(held);

    // Nothing active again: free again. (Escape is what a user presses; the
    // test clears the item directly so a change to Escape's meaning in a
    // text field cannot mask a change here.)
    ImGui::ClearActiveID();
    frames(2);
    CHECK(ImGui::GetActiveID() == 0);
    CHECK(!wantText);
    CHECK(!held);

    ImGui::DestroyContext();
}

int main() {
    // --- the action table ---------------------------------------------------
    {
        // Through a MUTABLE variable, not the constant: MSVC folds a const int
        // and warns (C4127) on the constant condition inside CHECK's own if.
        int actionCount = kKeyActionCount;
        CHECK(actionCount == 32);  // 31 + the transmit PTT (0.95.0)
        std::vector<std::string> ids;
        for (int i = 0; i < kKeyActionCount; ++i) {
            const KeyAction a = static_cast<KeyAction>(i);
            const char* id = keyActionId(a);
            CHECK(id[0] != '\0');
            CHECK(keyActionName(a)[0] != '\0');
            CHECK(keyActionGroup(a)[0] != '\0');
            for (const std::string& seen : ids) { CHECK(seen != id); }
            ids.push_back(id);

            KeyAction back = KeyAction::Count;
            CHECK(keyActionFromId(id, back));
            CHECK(back == a);
        }
        KeyAction ignored = KeyAction::StartStop;
        CHECK(!keyActionFromId("notAnAction", ignored));
        CHECK(!keyActionFromId("", ignored));
        CHECK(!keyActionFromId(nullptr, ignored));
    }

    // --- chord text: round trip, every key, every modifier combination -------
    {
        int keyCount = 0;
        const ImGuiKey* keys = chordKeyTable(keyCount);
        CHECK(keyCount > 60);
        for (int k = 0; k < keyCount; ++k) {
            CHECK(chordKeyName(keys[k])[0] != '\0');
            for (int mods = 0; mods < 8; ++mods) {
                Chord c;
                c.key = keys[k];
                c.ctrl = (mods & 1) != 0;
                c.shift = (mods & 2) != 0;
                c.alt = (mods & 4) != 0;
                const std::string text = formatChord(c);
                CHECK(text != "None");
                Chord back;
                CHECK(parseChord(text.c_str(), back));
                CHECK(back == c);
                // ...and the text is stable, not merely re-readable: two
                // spellings of one chord would let the config file and the
                // settings list disagree about the same binding.
                CHECK(formatChord(back) == text);
            }
        }
        // A modifier key is never the key of a chord - it is one of the flags.
        CHECK(chordKeyName(ImGuiKey_LeftCtrl)[0] == '\0');
        CHECK(chordKeyName(ImGuiKey_RightShift)[0] == '\0');
        ImGuiKey unused = ImGuiKey_None;
        CHECK(!chordKeyFromName("Ctrl", unused));
    }

    // --- the spellings the README and the owner's reference use --------------
    {
        const Chord pgup = chordOf("Ctrl+PageUp");
        CHECK(pgup.key == ImGuiKey_PageUp);
        CHECK(pgup.ctrl && !pgup.shift && !pgup.alt);
        CHECK(formatChord(pgup) == "Ctrl+PageUp");

        const Chord f2 = chordOf("F2");
        CHECK(f2.key == ImGuiKey_F2);
        CHECK(!f2.ctrl && !f2.shift && !f2.alt);
        CHECK(formatChord(f2) == "F2");

        const Chord plus = chordOf("Shift+Plus");
        CHECK(plus.key == ImGuiKey_Equal);
        CHECK(plus.shift && !plus.ctrl && !plus.alt);
        CHECK(formatChord(plus) == "Shift+Plus");

        // Modifier order is canonical on the way out whatever order it came in.
        CHECK(formatChord(chordOf("alt+shift+ctrl+a")) == "Ctrl+Shift+Alt+A");
        // Spaces around the tokens are a hand-edit, not an error.
        CHECK(formatChord(chordOf("  Ctrl + Delete  ")) == "Ctrl+Delete");
        // The aliases a person or an older file writes.
        CHECK(chordOf("Esc").key == ImGuiKey_Escape);
        CHECK(chordOf("Del").key == ImGuiKey_Delete);
        CHECK(chordOf("Ctrl+Equal") == chordOf("Ctrl+Plus"));
        CHECK(chordOf("Return").key == ImGuiKey_Enter);
        CHECK(chordOf("pgdn").key == ImGuiKey_PageDown);
    }

    // --- unbound, and the things that are not chords -------------------------
    {
        Chord none;
        CHECK(parseChord("None", none));
        CHECK(!none.bound());
        CHECK(formatChord(none) == "None");
        CHECK(parseChord("", none));
        CHECK(!none.bound());
        CHECK(parseChord("  ", none));
        CHECK(!none.bound());

        Chord bad;
        CHECK(!parseChord("Ctrl+", bad));        // unfinished, not unbound
        CHECK(!parseChord("Ctrl+A+B", bad));     // two keys is not a chord
        CHECK(!parseChord("Ctrl+Ctrl+A", bad));  // a modifier stated twice
        CHECK(!parseChord("Hyper+A", bad));      // a modifier this build has not got
        CHECK(!parseChord("Zork", bad));
        CHECK(!parseChord("+A", bad));
        CHECK(!parseChord(nullptr, bad));
    }

    // --- the shipped defaults ------------------------------------------------
    {
        const KeyBindings kb = defaultKeyBindings();
        for (int i = 0; i < kKeyActionCount; ++i) {
            // EVERY action has a key out of the box. An action in the list with
            // no default is one the settings section advertises and the
            // keyboard cannot do.
            CHECK(kb.chords[i].bound());
            // Escape belongs to "cancel this" - the frequency editor and scope
            // mode both answer it - so nothing here may take it.
            CHECK(kb.chords[i].key != ImGuiKey_Escape);
        }
        KeyAction a = KeyAction::Count;
        KeyAction b = KeyAction::Count;
        CHECK(!findKeyBindingConflict(kb, a, b));

        // The bindings the owner's reference named, as shipped.
        CHECK(formatChord(kb[KeyAction::StartStop]) == "Ctrl+F2");
        CHECK(formatChord(kb[KeyAction::Mute]) == "M");
        CHECK(formatChord(kb[KeyAction::VolumeUp]) == "Plus");
        CHECK(formatChord(kb[KeyAction::VolumeDown]) == "Minus");
        CHECK(formatChord(kb[KeyAction::ModeAM]) == "Ctrl+A");
        CHECK(formatChord(kb[KeyAction::ModeCW]) == "Ctrl+C");
        CHECK(formatChord(kb[KeyAction::ModeLSB]) == "Ctrl+L");
        CHECK(formatChord(kb[KeyAction::ModeUSB]) == "Ctrl+U");
        CHECK(formatChord(kb[KeyAction::ModeNFM]) == "Ctrl+F");
        CHECK(formatChord(kb[KeyAction::ModeWFM]) == "Ctrl+Shift+F");
        CHECK(formatChord(kb[KeyAction::TuneSpanUp]) == "Ctrl+PageUp");
        CHECK(formatChord(kb[KeyAction::TuneSpanDown]) == "Ctrl+PageDown");
        CHECK(formatChord(kb[KeyAction::QuickTune]) == "T");
        CHECK(formatChord(kb[KeyAction::ZoomIn]) == "Ctrl+Plus");
        CHECK(formatChord(kb[KeyAction::ZoomOut]) == "Ctrl+Minus");
        CHECK(formatChord(kb[KeyAction::ZoomReset]) == "Ctrl+Delete");
        CHECK(formatChord(kb[KeyAction::Screenshot]) == "Ctrl+W");
        // THE TRANSMIT KEY IS A BARE SPACEBAR, which is what every
        // transmitter's software PTT is and what nothing else in this table
        // would dare be. It is safe as a bare key ONLY because
        // AppWindow::drawTransmitPage reads it while the TRANSMIT page has
        // focus and dispatchKeyBindings skips it everywhere else - so if this
        // ever becomes an ordinary dispatched action, this default has to
        // change with it.
        CHECK(formatChord(kb[KeyAction::TransmitPtt]) == "Space");
        // And it is a real chord that round-trips, so a user can rebind it to
        // a foot switch's key like any other.
        CHECK(kb[KeyAction::TransmitPtt].bound());
        CHECK(!kb[KeyAction::TransmitPtt].ctrl);
        CHECK(!kb[KeyAction::TransmitPtt].shift);
        CHECK(!kb[KeyAction::TransmitPtt].alt);
        // NOTHING ELSE ANSWERS A BARE SPACE, which is the half of "no
        // conflicts" that matters here: a second action on this chord would
        // fire whenever somebody keyed up.
        {
            KeyAction other = KeyAction::Count;
            CHECK(!keyBindingConflicts(kb, KeyAction::TransmitPtt,
                                       kb[KeyAction::TransmitPtt], other));
        }
        CHECK(formatChord(kb[KeyAction::Fullscreen]) == "F11");
        // The five engraved FUNCTION SELECT keys keep the keys the panel and
        // its tooltips already claim for them.
        CHECK(formatChord(kb[KeyAction::BankSignal]) == "F1");
        CHECK(formatChord(kb[KeyAction::BankDecode]) == "F2");
        CHECK(formatChord(kb[KeyAction::BankTools]) == "F3");
        CHECK(formatChord(kb[KeyAction::BankExtend]) == "F4");
        CHECK(formatChord(kb[KeyAction::BankSystem]) == "F5");
    }

    // --- the third bank is TOOLS, and its config id is still "bankView" -------
    {
        // 0.99.74 renamed the VIEW bank TOOLS (a tester: "the various scopes,
        // the scanner and the airband are not really VIEW related"). The words
        // in the settings list say so...
        CHECK(std::strcmp(keyActionName(KeyAction::BankTools), "Bank: TOOLS") == 0);
        CHECK(std::strcmp(keyActionGroup(KeyAction::BankTools), "Rail") == 0);
        // ...but the id is the file format: a user who rebound F3 has
        // "bankView=..." in config.json, and renaming the id would unbind them
        // in silence.
        CHECK(std::strcmp(keyActionId(KeyAction::BankTools), "bankView") == 0);
        KeyAction found = KeyAction::Count;
        CHECK(keyActionFromId("bankView", found));
        CHECK(found == KeyAction::BankTools);
        // The id a rename would have invented does not exist.
        CHECK(!keyActionFromId("bankTools", found));
        // An old config still binds: "bankView=Ctrl+F3" is the TOOLS bank's key
        // now, and everything it did not mention is untouched.
        const KeyBindings old = keyBindingsFromConfig({"bankView=Ctrl+F3"});
        CHECK(old[KeyAction::BankTools] == chordOf("Ctrl+F3"));
        CHECK(old[KeyAction::BankExtend] == defaultKeyBindings()[KeyAction::BankExtend]);
        // ...and what it writes back keeps the id.
        const std::vector<std::string> written = keyBindingsToConfig(old);
        CHECK(written.size() == 1);
        if (written.size() == 1) { CHECK(written[0] == "bankView=Ctrl+F3"); }
    }

    // --- conflicts -----------------------------------------------------------
    {
        KeyBindings kb = defaultKeyBindings();
        KeyAction a = KeyAction::Count;
        KeyAction b = KeyAction::Count;
        kb[KeyAction::Mute] = kb[KeyAction::StartStop];
        CHECK(findKeyBindingConflict(kb, a, b));
        CHECK(a == KeyAction::StartStop);  // the first pair, in enum order
        CHECK(b == KeyAction::Mute);

        KeyAction other = KeyAction::Count;
        CHECK(keyBindingConflicts(kb, KeyAction::Mute, kb[KeyAction::Mute], other));
        CHECK(other == KeyAction::StartStop);
        // An action never conflicts with itself: re-pressing the key it
        // already has must not be reported as a clash.
        KeyBindings clean = defaultKeyBindings();
        CHECK(!keyBindingConflicts(clean, KeyAction::Mute, clean[KeyAction::Mute], other));
        // Nor does an unbound chord, however many of them there are.
        clean[KeyAction::Mute] = Chord{};
        clean[KeyAction::Record] = Chord{};
        CHECK(!keyBindingConflicts(clean, KeyAction::Mute, Chord{}, other));
        CHECK(!findKeyBindingConflict(clean, a, b));
    }

    // --- the lookup a frame performs ----------------------------------------
    {
        const KeyBindings kb = defaultKeyBindings();
        for (int i = 0; i < kKeyActionCount; ++i) {
            KeyAction got = KeyAction::Count;
            CHECK(keyActionForChord(kb, kb.chords[i], got));
            CHECK(got == static_cast<KeyAction>(i));
        }
        KeyAction got = KeyAction::Count;
        // MODIFIERS MATCH EXACTLY. Mute is M; Ctrl+M, Shift+M and Alt+M are
        // nothing at all, so a chord meant for something else can never mute
        // the radio on its way past.
        Chord ctrlM = kb[KeyAction::Mute];
        ctrlM.ctrl = true;
        CHECK(!keyActionForChord(kb, ctrlM, got));
        Chord shiftM = kb[KeyAction::Mute];
        shiftM.shift = true;
        CHECK(!keyActionForChord(kb, shiftM, got));
        // A key nothing is bound to, and the unbound chord itself.
        CHECK(!keyActionForChord(kb, chordOf("Ctrl+Shift+Alt+Num7"), got));
        CHECK(!keyActionForChord(kb, Chord{}, got));
    }

    // --- config.json's side --------------------------------------------------
    {
        // An untouched table writes NOTHING: only differences are stored, so a
        // later build that improves a default improves it for everybody who
        // never rebound that action.
        CHECK(keyBindingsToConfig(defaultKeyBindings()).empty());

        KeyBindings custom = defaultKeyBindings();
        custom[KeyAction::Mute] = chordOf("Ctrl+Shift+M");
        custom[KeyAction::Record] = Chord{};  // deliberately unbound
        const std::vector<std::string> lines = keyBindingsToConfig(custom);
        CHECK(lines.size() == 2);
        const KeyBindings back = keyBindingsFromConfig(lines);
        CHECK(back[KeyAction::Mute] == custom[KeyAction::Mute]);
        CHECK(!back[KeyAction::Record].bound());
        // Everything the file did not mention is still the default.
        CHECK(back[KeyAction::StartStop] == defaultKeyBindings()[KeyAction::StartStop]);
        KeyAction a = KeyAction::Count;
        KeyAction b = KeyAction::Count;
        CHECK(!findKeyBindingConflict(back, a, b));

        // Garbage is dropped LINE BY LINE - one bad line must never cost the
        // user the good ones beside it.
        const std::vector<std::string> messy = {
            "mute=Ctrl+Shift+M",       // good
            "notAnAction=Ctrl+Q",      // an id this build does not know
            "zoomIn",                  // no '='
            "zoomOut=Zork",            // a chord this build cannot read
            "quickTune=",              // explicitly unbound, which IS readable
            "  startStop = Alt+F4 ",   // spaces around both halves
            "mute=Ctrl+Shift+N",       // the same action a second time
            "",
        };
        const KeyBindings loaded = keyBindingsFromConfig(messy);
        CHECK(loaded[KeyAction::Mute] == chordOf("Ctrl+Shift+M"));  // the FIRST wins
        CHECK(loaded[KeyAction::ZoomIn] == defaultKeyBindings()[KeyAction::ZoomIn]);
        CHECK(loaded[KeyAction::ZoomOut] == defaultKeyBindings()[KeyAction::ZoomOut]);
        CHECK(!loaded[KeyAction::QuickTune].bound());
        CHECK(loaded[KeyAction::StartStop] == chordOf("Alt+F4"));

        // And the whole thing round-trips again from what it wrote.
        CHECK(keyBindingsFromConfig(keyBindingsToConfig(loaded))[KeyAction::Mute] ==
              loaded[KeyAction::Mute]);
        CHECK(keyBindingsFromConfig(keyBindingsToConfig(loaded))[KeyAction::StartStop] ==
              loaded[KeyAction::StartStop]);
    }

    textFieldHoldsKeysTest();

    return testSummary("test_key_bindings");
}
