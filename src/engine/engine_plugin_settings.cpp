// engine_plugin_settings.cpp - A PLUGIN'S SETTINGS FORM, EDITED (stub campaign B9, PLUGIN-SETTINGS-UI; master 0.99.43 - 0.99.74).
//
// THE FEATURE. A plugin that declares CASCADE_CAP_SETTINGS_UI gets a form drawn for it by the host - a text box, a number, a checkbox, a path with a
// BROWSE key - wherever its other controls appear. The rules (what a field shows, what an edit may store) are core/plugin_settings_ui.hpp's and pure;
// the store is PluginApiCore's, whose settingsUiSet is the ONE path that advances the plugin's settings_seq. In master the window held the form AND
// called that path. The window cannot include plugin_api.hpp (cmake/interface_fence.cmake), and a headless engine has no window, so the edit is a
// COMMAND now - FOXAPP_OP_PLUGIN_SETTING_EDIT, docs/api-0.3-additions.md 9.7 - and the engine runs the same two core calls master's window did
// (commitSettingEdit, which is prepareSettingEdit and then settingsUiSet) against the field the plugin's own table declares. What the form shows is
// the list PLUGIN_SETTINGS and the facts fact.plugin.setting.<id> (engine_api_lists.cpp).
//
// THE PLUGIN SEES settings_seq MOVE ONCE PER ACCEPTED EDIT - the command is sent when the box is LEFT (the window's commit-on-leave rule), never per
// keystroke, and a refused one (a number that is not one, a value too long) is answered and changes nothing, so nothing moves. An edit that leaves the
// stored value as it was is accepted and moves nothing either (settingsUiSet's own rule).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine.hpp"

#include <string>

#include "core/diag_log.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_settings_ui.hpp"

namespace cascade::engine {

namespace {

// The verdict's English, for the log and the result's message (the window says it in the user's language from the verdict itself).
const char* verdictWords(cascade::core::SettingEditResult r) {
    switch (r) {
        case cascade::core::SettingEditResult::Accepted: return "";
        case cascade::core::SettingEditResult::TooLong: return "not saved: longer than this field allows";
        case cascade::core::SettingEditResult::NotANumber: return "not saved: that is not a number";
        case cascade::core::SettingEditResult::BadText: return "not saved: it holds a character a setting cannot";
        case cascade::core::SettingEditResult::StoreFull: return "not saved: the plugin's settings are full";
        case cascade::core::SettingEditResult::StoreError: return "not saved: the store refused it";
        case cascade::core::SettingEditResult::Refused: return "not saved: this field is not one the host can store";
    }
    return "";
}

}  // namespace

// "<module file>\x1F<key>\x1F<value>": the value is everything after the SECOND separator, so a value that holds one is refused by the field's own
// rules (a control character) and not by this parse.
std::int32_t Engine::applyPluginSettingEdit(const std::string& text, double& verdict, std::string& why) {
    verdict = static_cast<double>(cascade::core::SettingEditResult::Refused);
    const std::size_t a = text.find('\x1F');
    const std::size_t b = a == std::string::npos ? std::string::npos : text.find('\x1F', a + 1u);
    if (b == std::string::npos || a == 0u || b == a + 1u) {
        why = "the edit names no module and field";
        return FOXAPI_BAD_ARGUMENT;
    }
    const std::string file = text.substr(0, a);
    const std::string key = text.substr(a + 1u, b - a - 1u);
    const std::string value = text.substr(b + 1u);
    for (const cascade::core::LoadedPlugin& lp : pluginHost_.plugins()) {
        if (!lp.loaded || lp.settingsUi == nullptr || cascade::core::pluginKey(lp) != file) { continue; }
        // THE FIELD IS THE PLUGIN'S OWN, from its static table: an edit names a field the module declares or it is not one.
        for (const cascade::core::SettingField& f : cascade::core::settingFieldsFrom(lp.settingsUi)) {
            if (f.key != key) { continue; }
            // The store is keyed by the descriptor name - the same identity the plugin's own settings_get / settings_set use.
            const std::uint64_t generation = pluginApi_->settingsGeneration();
            const cascade::core::SettingEditResult r = cascade::core::commitSettingEdit(*pluginApi_, lp.name, f, value);
            verdict = static_cast<double>(r);
            why = verdictWords(r);
            // ONE LINE FOR EACH EDIT THAT CHANGED WHAT THE PLUGIN WILL READ - the same edits that move its settings_seq - naming the field and the length and
            // never the text (a callsign, a path on this machine). A test counts them: typing is never an edit, so a box typed "G4" then "A" and left is ONE.
            if (r == cascade::core::SettingEditResult::Accepted && pluginApi_->settingsGeneration() != generation) {
                cascade::core::diagLogf("plugin settings: %s - %s saved (%zu bytes)", lp.name.c_str(), key.c_str(), value.size());
            }
            switch (r) {
                case cascade::core::SettingEditResult::Accepted: return FOXAPI_OK;
                case cascade::core::SettingEditResult::TooLong: return FOXAPI_OUT_OF_RANGE;
                case cascade::core::SettingEditResult::NotANumber:
                case cascade::core::SettingEditResult::BadText: return FOXAPI_BAD_ARGUMENT;
                case cascade::core::SettingEditResult::StoreFull: return FOXAPI_LIMIT;
                case cascade::core::SettingEditResult::StoreError: return FOXAPI_FAILED;
                case cascade::core::SettingEditResult::Refused: return FOXAPI_NOT_FOUND;
            }
            return FOXAPI_FAILED;
        }
        why = "that module declares no such field";
        return FOXAPI_NOT_FOUND;
    }
    why = "no loaded module has that file name and a settings form";
    return FOXAPI_NOT_FOUND;
}

}  // namespace cascade::engine
