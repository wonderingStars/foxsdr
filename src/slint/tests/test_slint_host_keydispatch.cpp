// The keyboard's dispatch (src/slint/key_dispatch.hpp, docs 6.56): which shortcut a key press answers, by the SAVED key table. Pure: no Slint, no engine,
// no window. The reference is the ImGui window's AppWindow::dispatchKeyBindings (src/gui/app_window.cpp): the chord must match exactly (a key and its three
// modifiers), the first action in table order wins a clash, an unbound action answers nothing, and the PTT key is held, not pressed, so it is never answered.
//
// What this holds equal: the stock table reproduces EVERY chord the window answered before the dispatch moved out of main.slint; a rebound action answers its new
// chord and not the old one; a cleared action answers nothing; a clash resolves as the classic window does; F2 and Ctrl+F2 are two chords.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "key_dispatch.hpp"
#include "system_logic.hpp"
#include "host_logic.hpp"
#include "test_check.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace foxsdr::slintsystem;

namespace {

KeyPress press(const char* key, bool ctrl = false, bool shift = false, bool alt = false, bool glyphShifted = false) {
    KeyPress p;
    p.key = key;
    p.ctrl = ctrl;
    p.shift = shift;
    p.alt = alt;
    p.glyphShifted = glyphShifted;
    return p;
}

Chord chord(const char* text) {
    Chord c;
    CHECK(parseChord(text, c));
    return c;
}

// ---- the stock table answers every chord the window answered when main.slint carried them ----
void stockChords() {
    const KeyTable def = defaultKeyTable();
    struct Row { KeyPress press; const char* id; };
    const Row rows[] = {
        {press("F2", true), "startStop"},
        {press("F1"), "bankSignal"},
        {press("F2"), "bankDecode"},
        {press("F3"), "bankView"},
        {press("F4"), "bankExtend"},
        {press("F5"), "bankSystem"},
        {press("F7"), "openSettings"},
        {press("F11"), "fullscreen"},
        {press("F12"), "screenshot"},
        {press("Escape"), "escape"},
        {press("Up", true), "tuneStepUp"},
        {press("Down", true), "tuneStepDown"},
        {press("PageUp", true, true), "squelchUp"},
        {press("PageDown", true, true), "squelchDown"},
        {press("PageUp", true), "tuneSpanUp"},
        {press("PageDown", true), "tuneSpanDown"},
        {press("Delete", true), "zoomReset"},
        {press("F", true, true), "modeWFM"},
        {press("F", true), "modeNFM"},
        {press("A", true), "modeAM"},
        {press("D", true), "modeDSB"},
        {press("U", true), "modeUSB"},
        {press("C", true), "modeCW"},
        {press("L", true), "modeLSB"},
        {press("R", true), "modeRAW"},
        {press("W", true), "screenshot"},
        {press("K", true), "palette"},
        {press("Plus", true), "zoomIn"},
        {press("Minus", true), "zoomOut"},
        {press("0", true), "uiSizeAuto"},
        {press("M"), "mute"},
        {press("Plus"), "volumeUp"},
        {press("Minus"), "volumeDown"},
        {press("T"), "quickTune"},
        {press("R", false, true), "record"},
    };
    for (const Row& r : rows) {
        const std::string got = shortcutForPress(r.press, def);
        if (got != r.id) { std::printf("  stock %s%s%s%s: got '%s', wanted '%s'\n", r.press.ctrl ? "Ctrl+" : "", r.press.shift ? "Shift+" : "", r.press.alt ? "Alt+" : "", r.press.key.c_str(), got.c_str(), r.id); }
        CHECK(got == r.id);
    }
    // every row of the table answers its own stock chord with its own id - 41 of the 42 (the PTT is held, not pressed)
    int answered = 0;
    for (int i = 0; i < kKeyActionCount; ++i) {
        const std::string want = keyAction(i).id;
        const std::string got = shortcutFor(def[static_cast<std::size_t>(i)], def);
        if (want == "transmitPtt") {
            CHECK(got.empty());
            continue;
        }
        // the first holder of a chord answers; the stock table has no clash, so it is the row itself
        if (got != want) { std::printf("  table row %d (%s): got '%s'\n", i, want.c_str(), got.c_str()); }
        CHECK(got == want);
        ++answered;
    }
    CHECK(answered == 41);
    // the keys no chord holds answer nothing; modifier-only and unnamed keys too
    CHECK(shortcutForPress(press("G"), def).empty() && shortcutForPress(press("Space"), def).empty() && shortcutForPress(press(""), def).empty());
}

// ---- modifiers make a chord: F2 and Ctrl+F2, M and Ctrl+M, Shift+R and R ----
void modifiersDistinguish() {
    const KeyTable def = defaultKeyTable();
    CHECK(shortcutForPress(press("F2"), def) == "bankDecode" && shortcutForPress(press("F2", true), def) == "startStop");
    CHECK(shortcutForPress(press("F2", false, true), def).empty() && shortcutForPress(press("F2", false, false, true), def).empty() && shortcutForPress(press("F2", true, true), def).empty());
    // the classic window matches the three modifiers exactly: Shift+M is not M, Ctrl+M is not M (the Slint window used to answer both as Mute)
    CHECK(shortcutForPress(press("M"), def) == "mute" && shortcutForPress(press("M", false, true), def).empty() && shortcutForPress(press("M", true), def).empty());
    CHECK(shortcutForPress(press("R", false, true), def) == "record" && shortcutForPress(press("R"), def).empty() && shortcutForPress(press("R", true), def) == "modeRAW");
    CHECK(shortcutForPress(press("F", true), def) == "modeNFM" && shortcutForPress(press("F", true, true), def) == "modeWFM");
    CHECK(shortcutForPress(press("PageUp", true), def) == "tuneSpanUp" && shortcutForPress(press("PageUp", true, true), def) == "squelchUp" && shortcutForPress(press("PageUp"), def).empty());
    // Alt is a modifier too (a Ctrl+Alt chord cannot be captured on Windows - Slint delivers it without its modifiers - but Alt alone can)
    KeyTable t = def;
    t[1] = chord("Alt+M");
    CHECK(shortcutForPress(press("M", false, false, true), t) == "mute" && shortcutForPress(press("M"), t).empty());
}

// ---- a rebound action answers the new chord and NOT the old one ----
void rebinding() {
    KeyTable t = keyTableFromConfig({"bankSignal=F6"});
    CHECK(shortcutForPress(press("F6"), t) == "bankSignal");
    CHECK(shortcutForPress(press("F1"), t).empty());          // the old chord is dead
    CHECK(shortcutForPress(press("F2"), t) == "bankDecode");  // the others are untouched
    // a letter chord, a rebinding onto a key another action had (that action loses nothing it was not bound to - it keeps ITS chord, and the clash is the user's to see)
    t = keyTableFromConfig({"mute=Ctrl+M", "volumeUp=Ctrl+Shift+Plus"});
    CHECK(shortcutForPress(press("M", true), t) == "mute" && shortcutForPress(press("M"), t).empty());
    CHECK(shortcutForPress(press("Plus", true, true), t) == "volumeUp" && shortcutForPress(press("Plus"), t).empty());
    // the saved text is what binds: lines with a bad chord or an unknown action are dropped one by one and the stock chord stays
    t = keyTableFromConfig({"mute=Banana", "nonsense=F9", "bankView=F9"});
    CHECK(shortcutForPress(press("M"), t) == "mute" && shortcutForPress(press("F9"), t) == "bankView" && shortcutForPress(press("F3"), t).empty());
    // a stock id the old window spelled differently is the TABLE's now: mode keys answer "modeNFM", not "modeNfm"
    CHECK(shortcutForPress(press("F", true), defaultKeyTable()) == "modeNFM");
}

// ---- a cleared action answers nothing ----
void cleared() {
    KeyTable t = keyTableFromConfig({"mute=None", "bankSystem=None"});
    CHECK(shortcutForPress(press("M"), t).empty() && shortcutForPress(press("F5"), t).empty());
    CHECK(shortcutForPress(press("T"), t) == "quickTune");   // the rest is untouched
    // and "None" is not a chord a key can press: an empty key name finds nothing in a table full of unbound rows
    KeyTable none;
    CHECK(shortcutForPress(press(""), none).empty() && shortcutForPress(press("M"), none).empty());
    CHECK(shortcutFor(Chord{}, none).empty() && shortcutFor(Chord{}, defaultKeyTable()).empty());
}

// ---- two actions on one chord: the first in table order answers, as in the classic window ----
void clashes() {
    KeyTable t = keyTableFromConfig({"volumeUp=Ctrl+M", "mute=Ctrl+M"});   // mute is row 1, volumeUp row 2
    CHECK(shortcutForPress(press("M", true), t) == "mute");
    t = keyTableFromConfig({"bankView=F9", "bankSignal=F9"});               // bankSignal is row 27, bankView row 29
    CHECK(shortcutForPress(press("F9"), t) == "bankSignal");
    // a cleared FIRST holder lets the second answer
    t = keyTableFromConfig({"volumeUp=Ctrl+M", "mute=Ctrl+M"});
    t[1] = Chord{};
    CHECK(shortcutForPress(press("M", true), t) == "volumeUp");
    // the PTT is never answered even when it is the only holder of the chord
    t = keyTableFromConfig({"transmitPtt=F9"});
    CHECK(shortcutForPress(press("F9"), t).empty() && shortcutFor(chord("F9"), t).empty());
    // ...and does not shadow a later holder either
    t = keyTableFromConfig({"transmitPtt=F9", "bankExtend=F9"});
    CHECK(shortcutForPress(press("F9"), t) == "bankExtend");
}

// ---- what is fixed, and what is not in the table ----
void fixedChords() {
    const KeyTable def = defaultKeyTable();
    // F12 and Escape with no modifier are answered whatever the table says: the Key bindings footer promises both
    KeyTable t = keyTableFromConfig({"bankView=F12", "mute=Escape"});
    CHECK(shortcutForPress(press("F12"), t) == "screenshot" && shortcutForPress(press("Escape"), t) == "escape");
    // the screenshot key may be moved or cleared; F12 still shoots
    t = keyTableFromConfig({"screenshot=None"});
    CHECK(shortcutForPress(press("F12"), t) == "screenshot" && shortcutForPress(press("W", true), t).empty());
    // with a modifier they are table chords first, then the fixed fallback (what the window always did: Esc and F12 answer with any modifier)
    t = keyTableFromConfig({"bankView=Ctrl+F12"});
    CHECK(shortcutForPress(press("F12", true), t) == "bankView" && shortcutForPress(press("F12", false, true), t) == "screenshot" && shortcutForPress(press("Escape", true), t) == "escape");
    // Ctrl+K opens the palette unless the user has bound it; Ctrl+0 is the interface-size reset
    CHECK(shortcutForPress(press("K", true), def) == kShortcutPalette && shortcutForPress(press("0", true), def) == kShortcutUiSizeAuto);
    t = keyTableFromConfig({"mute=Ctrl+K"});
    CHECK(shortcutForPress(press("K", true), t) == "mute");
    CHECK(shortcutForPress(press("K"), def).empty() && shortcutForPress(press("K", true, true), def).empty());
}

// ---- the plus key: "+" is Shift+"=" on a standard keyboard, and the capture stores what it saw ----
void glyphShift() {
    const KeyTable def = defaultKeyTable();
    // the "+" typed with Shift finds no Shift+Plus chord and is tried again as Plus: the stock volume and zoom keys keep answering
    CHECK(shortcutForPress(press("Plus", false, true, false, true), def) == "volumeUp");
    CHECK(shortcutForPress(press("Plus", true, true, false, true), def) == "zoomIn");
    CHECK(shortcutForPress(press("Plus", true), def) == "zoomIn");   // "=" with Ctrl
    CHECK(shortcutForPress(press("Minus", false, true, false, true), def) == "volumeDown");   // "_"
    // a user who bound Ctrl+Shift+Plus (the capture saw Shift held) gets exactly that chord first
    KeyTable t = keyTableFromConfig({"mute=Ctrl+Shift+Plus"});
    CHECK(shortcutForPress(press("Plus", true, true, false, true), t) == "mute");
    CHECK(shortcutForPress(press("Plus", true), t) == "zoomIn");
    // a real Shift on a letter is not a glyph's: Shift+M does not fall back to M
    CHECK(shortcutForPress(press("M", false, true, false, false), def).empty());
}

// Esc takes ONE layer: the dialog on top, then the plugin page, else the small overlays (docs 6.61)
void escapeLayers() {
    EscapeState s;
    CHECK(escapeStep(s) == EscapeStep::Overlays);               // nothing open: the editor / About / bias sheet are what Esc closes
    s.storePluginPage = true;
    CHECK(escapeStep(s) == EscapeStep::StorePluginPage);        // a plugin's page: back to the browse grid
    s.importDialog = true;
    CHECK(escapeStep(s) == EscapeStep::ImportDialog);           // the dialog is above the page: it goes first
    s.storePluginPage = false;
    CHECK(escapeStep(s) == EscapeStep::ImportDialog);
    s.importDialog = false;
    CHECK(escapeStep(s) == EscapeStep::Overlays);
    // and the key that asks is still the fixed one: bare Escape answers "escape" whatever the table holds
    const KeyTable def = defaultKeyTable();
    CHECK(shortcutForPress(press("Escape"), def) == kShortcutEscape);
}

}  // namespace


// ---- SCREEN STAGE S5: the ten rows of this window's own (docs/multi-radio-records/screen-s5.md): Ctrl+1 to Ctrl+8 select a radio, Alt+PageUp / Alt+PageDown turn the page of radios ----
Chord ch(const char* text) { Chord c; parseChord(text, c); return c; }
std::string answer(const KeyTable& t, const char* chordText, bool glyphShifted = false) {
    const Chord c = ch(chordText);
    KeyPress p;
    p.key = c.key; p.ctrl = c.ctrl; p.shift = c.shift; p.alt = c.alt; p.glyphShifted = glyphShifted;
    return shortcutForPress(p, t);
}
void radioKeys() {
{
        CHECK(kClassicKeyActionCount == 32 && kKeyActionCount == 42 && kSelectRadioActions == 8);
        for (int n = 1; n <= 8; ++n) {
            const int idx = keyActionIndexOf("selectRadio" + std::to_string(n));
            CHECK(idx == kClassicKeyActionCount + n - 1 && keyAction(idx).number == n && std::string(keyAction(idx).stock) == "Ctrl+" + std::to_string(n) && std::string(keyAction(idx).group) == "Radio");
            CHECK(keyActionName(idx) == "Select Radio " + std::to_string(n));
        }
        CHECK(keyActionIndexOf("radioPagePrev") == 40 && keyActionIndexOf("radioPageNext") == 41);
        CHECK(std::string(keyAction(40).stock) == "Alt+PageUp" && std::string(keyAction(41).stock) == "Alt+PageDown");
        CHECK(keyActionName(40) == "Previous page of radios" && keyActionName(41) == "Next page of radios" && keyActionName(0) == "Start / stop the receiver");
        // PROOF THEY ARE FREE: no other action holds any of the ten chords in the stock table (the table has no clash at all), and the fixed shortcuts are other chords
        const KeyTable def = defaultKeyTable();
        CHECK(clashCount(def) == 0);
        for (int i = 0; i < kClassicKeyActionCount; ++i) {
            for (int j = kClassicKeyActionCount; j < kKeyActionCount; ++j) { CHECK(!(def[static_cast<std::size_t>(i)] == def[static_cast<std::size_t>(j)])); }
        }
        CHECK(answer(def, "Ctrl+K") == kShortcutPalette && answer(def, "Ctrl+0") == kShortcutUiSizeAuto);
        CHECK(answer(def, "F12") == "screenshot" && answer(def, "Escape") == kShortcutEscape);
        // each chord answers its own id; the neighbours that were taken are as they were
        for (int n = 1; n <= 8; ++n) { CHECK(answer(def, ("Ctrl+" + std::to_string(n)).c_str()) == "selectRadio" + std::to_string(n)); }
        CHECK(answer(def, "Alt+PageUp") == "radioPagePrev" && answer(def, "Alt+PageDown") == "radioPageNext");
        CHECK(answer(def, "Ctrl+PageUp") == "tuneSpanUp" && answer(def, "Ctrl+PageDown") == "tuneSpanDown");
        CHECK(answer(def, "Ctrl+Shift+PageUp") == "squelchUp" && answer(def, "PageUp").empty() && answer(def, "Shift+Alt+PageUp").empty() && answer(def, "Ctrl+Alt+PageUp").empty());
        CHECK(answer(def, "Ctrl+9").empty() && answer(def, "Alt+1").empty() && answer(def, "1").empty() && answer(def, "Ctrl+Shift+1").empty());
        // rebindable and saved like every other row: a chord moved, saved as "id=chord", read back; a clash is named by the row's own name
        KeyTable t = def;
        t[static_cast<std::size_t>(keyActionIndexOf("selectRadio3"))] = ch("Ctrl+Alt+3");
        const std::vector<std::string> lines = keyTableToConfig(t);
        CHECK(lines.size() == 1 && lines[0] == "selectRadio3=Ctrl+Alt+3");
        const KeyTable back = keyTableFromConfig(lines);
        CHECK(back == t && answer(back, "Ctrl+Alt+3") == "selectRadio3" && answer(back, "Ctrl+3").empty());
        t[static_cast<std::size_t>(keyActionIndexOf("selectRadio2"))] = ch("Ctrl+4");
        CHECK(clashOf(t, keyActionIndexOf("selectRadio2")) == keyActionIndexOf("selectRadio4") && clashCount(t) == 2);
        const KeyTable unbound = keyTableFromConfig({"radioPageNext=None"});
        CHECK(answer(unbound, "Alt+PageDown").empty() && answer(unbound, "Alt+PageUp") == "radioPagePrev");
        // the saved file of a build that does not know the rows (the ImGui window's) loads: unknown ids are dropped one by one
        const KeyTable odd = keyTableFromConfig({"mute=Ctrl+M", "selectRadio9=Ctrl+9", "selectRadio1=Ctrl+Shift+1"});
        CHECK(odd[static_cast<std::size_t>(keyActionIndexOf("selectRadio1"))] == ch("Ctrl+Shift+1") && odd[1] == ch("Ctrl+M"));
        // the keys page lists them
        cascade::core::AppConfig iface;
        ReadOptions o;
        const KeyBindingsModel m = readKeyBindings(iface, o);
        CHECK(m.action_count == 42 && m.groups.back().heading == "Radio" && m.groups.back().rows.size() == 10);
        CHECK(m.groups.back().rows[0].action == "Select Radio 1" && m.groups.back().rows[0].chord == "Ctrl+1" && m.groups.back().rows[9].chord == "Alt+PageDown" && m.clash_count == 0);
    // THE CLASH TEXT of a row names the other row (a name longer than a short-string buffer, so a std::string passed where a C string is meant fails here: the host crashed at start with a saved table
    // that put two actions on F2 - found by test_slint_config_roundtrip): "mute" saved on Ctrl+Up clashes with "Tune up one step (10 kHz)" and says so, and a row of the new group names its clash the same way
    {
        cascade::core::AppConfig clash;
        clash.keyBindings = {"mute=Ctrl+Up", "selectRadio2=Ctrl+1"};
        const KeyBindingsModel cm = readKeyBindings(clash, o);
        std::string muteClash, tuneClash, radio1Clash;
        for (const KeyGroup& g : cm.groups) { for (const KeyRow& r : g.rows) { if (r.id == "mute") { muteClash = r.clash; } if (r.id == "tuneStepUp") { tuneClash = r.clash; } if (r.id == "selectRadio1") { radio1Clash = r.clash; } } }
        // THE KEY SAYS 10 kHz AND DOES 10 kHz (screen stage S6, B5): the Slint window started at 12.5 kHz while the row said 10; the host's start value is held to the row's words
        {
            std::string tuneAction;
            for (const KeyGroup& g : cm.groups) { for (const KeyRow& r : g.rows) { if (r.id == "tuneStepUp") { tuneAction = r.action; } } }
            CHECK(tuneAction.find(std::to_string(static_cast<int>(foxsdr::slinthost::kDefaultTuneStepHz / 1000.0)) + " kHz") != std::string::npos);
        }
        CHECK(muteClash == "also Tune up one step (10 kHz)" && tuneClash == "also Mute the sound" && radio1Clash == "also Select Radio 2" && cm.clash_count == 4);
    }
    }


}

int main() {
    stockChords();
    modifiersDistinguish();
    rebinding();
    cleared();
    clashes();
    fixedChords();
    glyphShift();
    escapeLayers();
    radioKeys();
    return testSummary("test_slint_host_keydispatch");
}
