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
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_PLUGIN_SETTINGS_FORM_HPP
#define CASCADE_GUI_PLUGIN_SETTINGS_FORM_HPP

#include <map>
#include <string>
#include <vector>

#include "core/plugin_settings_ui.hpp"

namespace cascade::core {
class PluginApiCore;
}

namespace cascade::gui {

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
};

// Draws the fields into the current ImGui window, full width, in the plugin's
// order. `scope` names the surface ("rail", "window", "patch") and becomes
// part of every widget id. Accepted edits go through core::commitSettingEdit
// - PluginApiCore::settingsUiSet, the path that advances settings_seq. A TEXT
// or NUMBER edit is committed when the box is LEFT (Enter, Tab, or a click
// elsewhere), never per keystroke: a plugin reporting a callsign must never
// see "G4", then "G4A", as the user types. A checkbox commits on the click.
// Returns how many edits were stored this frame.
int drawPluginSettingsForm(core::PluginApiCore& api, const std::string& pluginName,
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
// one. Returns how many edits were flushed this way.
int flushUntouchedSettingsEdits(core::PluginApiCore& api, PluginSettingsFormState& state);

}  // namespace cascade::gui

#endif  // CASCADE_GUI_PLUGIN_SETTINGS_FORM_HPP
