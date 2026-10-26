// plugin_settings_form.hpp - draws a plugin's CASCADE_CAP_SETTINGS_UI form
// (0.99.43): one editable field per core::SettingField, wherever that plugin's
// other controls already appear - its row under "Turn on and off plugins", its
// own window, and its node's inspector on the patch page.
//
// The rules (what a field shows, what an edit may store) are all in
// core/plugin_settings_ui.hpp; this file only turns them into widgets, so the
// state it keeps is the one thing ImGui needs across frames - the text being
// typed - and it holds no ImGui types itself (app_window.hpp, which owns one,
// does not include imgui.h).
//
// THE FORM DOES NOT TOUCH THE PLUGIN API (stub campaign B9). It drew through core::PluginApiCore - reading the stored value and committing an edit
// with settingsUiSet, the one path that advances settings_seq - and the interface fence denies core/plugin_api.hpp to everything under src/gui
// (cmake/interface_fence.cmake): the engine owns the store, and a headless engine has no window. The form draws against a SettingsEditSink instead:
// what a field shows, and where an edit goes. The window's sink reads fact.plugin.setting.<id> and sends FOXAPP_OP_PLUGIN_SETTING_EDIT (the ENGINE
// judges the edit and writes it through the same two core calls master's window made); a test gives the form an adapter over a PluginApiCore, whose
// answer is known at once. The two differ in one thing the form must be written for: an edit made through a command is ANSWERED a frame or two later.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_PLUGIN_SETTINGS_FORM_HPP
#define CASCADE_GUI_PLUGIN_SETTINGS_FORM_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/file_pick.hpp"
#include "core/plugin_settings_ui.hpp"

namespace cascade::gui {

// WHAT AN EDIT CAME TO WHEN IT WAS HANDED OVER: the verdict when it is known now, otherwise the ticket the answer will carry.
struct SettingsCommit {
    std::optional<core::SettingEditResult> verdict;  // decided at once (an adapter over the store; a send that could not be made)
    std::uint64_t ticket = 0;                        // else: the edit was sent, and PluginSettingsFormState::deliver(ticket, ...) is its answer
};

// WHERE THE FORM READS AND WRITES. `owner` is what the surface calls the plugin - the module's file name in the window, the plugin's name in the
// adapter a test builds over a store - and is only ever handed back.
class SettingsEditSink {
public:
    virtual ~SettingsEditSink() = default;
    // What the field shows now: the stored text, else the field's default (core::settingFieldValue's rule).
    virtual std::string value(const std::string& owner, const core::SettingField& f) = 0;
    // Hands an edit over. The text is exactly what was typed or chosen; judging it is the store's side.
    virtual SettingsCommit commit(const std::string& owner, const core::SettingField& f, const std::string& typed) = 0;
};

// How many frames a box keeps showing an accepted edit while the store still reads the old value (the answer is handled a frame before the facts
// it changed are read again: the box must not flash the old text). Bounded so a plugin that overwrites the value at once is not held to ours.
constexpr int kSettingHoldFrames = 30;

// One field's memory between frames.
struct SettingFieldState {
    // The box's text. Refilled from the STORE on every frame the box is not
    // being typed in, so what the user reads after leaving a field is what
    // was actually kept - a refused edit visibly snaps back rather than
    // sitting in the box looking saved.
    std::vector<char> buf;
    bool editing = false;
    // The last edit's answer; anything but Accepted is shown under the field
    // until the next edit of it.
    core::SettingEditResult last = core::SettingEditResult::Accepted;

    // AN EDIT SENT AND NOT YET ANSWERED (a sink whose commit crosses a command round trip). The box keeps what was typed - it must not snap back to
    // the old value for the frames before the answer, and a refused edit snaps back only when the answer says so. `expect` is the text the store
    // will hold if the edit is accepted (core::prepareSettingEdit's), `hold` the frames left in which an ACCEPTED edit's box still follows `expect`
    // instead of a store that has not caught up.
    bool pending = false;
    std::uint64_t ticket = 0;
    std::string expect;
    int hold = 0;

    // --- For flushUntouchedSettingsEdits() only; drawPluginSettingsForm()
    // keeps these current on every call, nothing else writes them. ---------

    // Who owns this box and what it is, copied fresh on every draw so a
    // pending edit can still be committed on the exact frame its surface
    // stops drawing it - a window closing, a plugin row disappearing, the
    // patch page selecting a different node - none of which fire ImGui's own
    // deactivation on the box, because the box is simply never drawn again.
    std::string pluginName;
    core::SettingField field;
    // Set true by every drawPluginSettingsForm() call that reaches this
    // field; cleared by beginSettingsFormFrame() at the top of the frame. A
    // field left false AND still `editing` after a frame's drawing is done
    // is exactly the case above.
    bool touchedThisFrame = false;
};

// Every field of every form, keyed by surface + plugin + key, so the same
// plugin drawn in two places at once keeps two independent boxes.
struct PluginSettingsFormState {
    std::map<std::string, SettingFieldState> fields;

    // WHAT A PATH FIELD'S BROWSE KEY DOES (0.99.74, CASCADE_SETTING_PATH): asks the
    // person for a file and returns true with its path, or false when they
    // cancelled (or nothing could be shown). EMPTY MEANS THIS MACHINE HAS NO FILE
    // DIALOG (Linux with neither zenity nor kdialog): a PATH field is then its text
    // box alone - never a key that does nothing. The application sets it once
    // (AppWindow::pickFile: the system's dialog under the watchdog's pause, or the
    // FOXSDR_FILE_PICK test seam); a test sets its own. It is called from inside
    // drawPluginSettingsForm, on the GUI thread, and may block until the dialog
    // closes.
    std::function<bool(const core::FilePickRequest&, std::string&)> pick;

    // THE ANSWER to an edit a sink sent (SettingsCommit::ticket). The box that sent it - every box that did, if two surfaces drew one field - takes
    // the verdict: shown under the field when it is a refusal, and the box goes back to what the store holds from the next frame. An accepted edit
    // keeps the box on the text it was accepted as for a few frames (kSettingHoldFrames). False when no box was waiting for that ticket - an answer
    // to an edit since replaced by a newer one, or to a field whose form is gone - which is nothing to do.
    bool deliver(std::uint64_t ticket, core::SettingEditResult verdict);
};

// One module's fields as the window's cache holds them, in the plugin's order: the field (key, label, placeholder, kind, the longest value) and the
// number of the PLUGIN_SETTINGS entry that carries it, which is what its value is read by (fact.plugin.setting.<id>).
struct PluginSettingsSet {
    std::string file;
    std::vector<core::SettingField> fields;
    std::vector<std::uint64_t> ids;
};

// Every form the engine's table lists, read when listSeq moves (AppWindow::settingsLists).
struct PluginSettingsLists {
    std::uint64_t listSeq = ~std::uint64_t{0};
    std::vector<PluginSettingsSet> sets;
    // The module's form, or nullptr when it declares none.
    const PluginSettingsSet* find(const std::string& file) const {
        for (const PluginSettingsSet& s : sets) {
            if (s.file == file) { return &s; }
        }
        return nullptr;
    }
};

// The verdict an ANSWER to FOXAPP_OP_PLUGIN_SETTING_EDIT carries: Accepted for OK (and NO_CHANGE), otherwise the core::SettingEditResult in
// applied[0], and for an answer that carries none (the command was refused before the engine judged it: a grant, a busy queue) the store refused it.
core::SettingEditResult settingVerdictFromAnswer(int status, double applied0);

// Draws the fields into the current ImGui window, full width, in the plugin's
// order. `scope` names the surface ("rail", "window", "patch") and becomes
// part of every widget id. An edit goes to the sink - for the window, a
// FOXAPP_OP_PLUGIN_SETTING_EDIT, which the engine writes through
// PluginApiCore::settingsUiSet, the path that advances settings_seq. A TEXT
// or NUMBER edit is committed when the box is LEFT (Enter, Tab, or a click
// elsewhere), never per keystroke: a plugin reporting a callsign must never
// see "G4", then "G4A", as the user types. A checkbox commits on the click.
// A PATH field is the text box shortened by a BROWSE key (when state.pick is
// set): the key asks state.pick for a file and commits what comes back through
// the same call a typed edit takes, so the plugin sees settings_seq move; a
// cancel changes nothing.
// Returns how many edits were stored this frame - the ones the sink answered
// at once; an edit that is answered later (SettingsCommit::ticket) is not
// counted.
int drawPluginSettingsForm(SettingsEditSink& sink, const std::string& pluginName,
                           const std::vector<core::SettingField>& fields,
                           PluginSettingsFormState& state, const char* scope);

// The sentence shown under a field whose edit was not kept. Translated; never
// null; "" for Accepted.
const char* settingEditMessage(core::SettingEditResult r);

// The key a field's state is kept under (exposed for the tests).
std::string settingFieldStateKey(const char* scope, const std::string& pluginName,
                                 const std::string& key);

// Called ONCE per frame, before ANY drawPluginSettingsForm() call for that
// frame - clears every field's touchedThisFrame, so this frame's draws can
// tell a field they reach from one nobody reaches any more.
void beginSettingsFormFrame(PluginSettingsFormState& state);

// Called ONCE per frame, AFTER every surface that might draw a settings form
// has had its turn (the same "safe point" rule as consumePendingPresetRequest:
// every loop that could have touched `state` this frame has finished).
// Commits any TEXT/NUMBER field that is still `editing` but was not touched
// this frame: its surface stopped drawing it without the box ever losing
// focus the ordinary way, so IsItemDeactivatedAfterEdit could not have fired
// for it and the typed text would otherwise be silently lost. A BOOL field is
// never `editing` (it commits on the click itself), so this is a no-op for
// one. Returns how many edits were flushed this way - the ones the sink
// answered at once and accepted, or sent.
int flushUntouchedSettingsEdits(SettingsEditSink& sink, PluginSettingsFormState& state);

}  // namespace cascade::gui

#endif  // CASCADE_GUI_PLUGIN_SETTINGS_FORM_HPP
