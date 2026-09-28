// plugin_settings_ui.hpp - CASCADE_CAP_SETTINGS_UI, the host half that needs
// no graphics context: reading a plugin's declared form into host-owned
// fields, deciding what each field shows, and deciding whether an edit the
// user made may go into the settings store.
//
// The drawing is gui/plugin_settings_form.{hpp,cpp}. Everything that can be
// wrong about a field - a spec this host cannot read, a kind it does not know,
// a value too long, a number that is not one - is decided HERE, as plain
// functions over plain data, so it is tested with expected answers rather than
// with screenshots (tests/test_plugin_settings_ui.cpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/plugin_abi.h"

namespace cascade::core {

class PluginApiCore;

// One field the host will draw, COPIED out of the plugin's static table so the
// record outlives the module image and so nothing downstream reads a
// plugin-owned char array without a bound.
struct SettingField {
    std::string key;           // valid store key (validSettingKey), never empty
    std::string label;         // plugin-authored: shown as-is, never translated
    std::string placeholder;   // plugin-authored hint; "" for none
    std::string defaultValue;  // shown while the store has no value
    std::uint32_t kind = CASCADE_SETTING_TEXT;
    // TEXT: the plugin's limit in UTF-8 bytes, clamped to
    // 1..CASCADE_SETTING_VALUE_BYTES-1. NUMBER: kMaxNumberChars. BOOL: 1.
    std::size_t maxLength = 0;
};

// Longest decimal text a NUMBER field accepts. A double needs at most 24
// characters written out in full ("-1.7976931348623157e+308"); 32 leaves room
// for a leading sign and spaces the user typed and we trimmed.
constexpr std::size_t kMaxNumberChars = 32;

// The plugin's form, as fields this host can draw, in the plugin's order.
//
// FORGIVING PER ENTRY, like everything in the header: an entry whose
// structSize this host does not recognise, whose kind it does not know, whose
// key the store could never hold, or whose key repeats an earlier entry's is
// SKIPPED - the rest of the form is still drawn. Null, a table of the wrong
// size, null specs, or a count of 0 or above CASCADE_MAX_SETTING_SPECS yield
// no fields at all (the loader has already refused a plugin with such a
// table; this is the second line, for a record built any other way).
std::vector<SettingField> settingFieldsFrom(const CascadeSettingsUiApi* api);

// What an attempted edit came to. Anything but Accepted is shown beside the
// field, and the field goes back to showing what the store holds.
enum class SettingEditResult {
    Accepted,    // stored (or already the stored value)
    TooLong,     // TEXT longer than the field's maxLength
    NotANumber,  // NUMBER text that is not a finite decimal number
    BadText,     // not valid UTF-8, or holds a control character
    // The three below are told apart because they are told to the user apart
    // (settingEditMessage, gui/plugin_settings_form.cpp) - "the plugin's
    // settings are full" is a real, common, explainable state; a bare
    // "refused" covering every other store failure as well would be true and
    // useless.
    StoreFull,   // settingsUiSet returned CASCADE_API_LIMIT: this key would be
                 // the plugin's 65th, past CASCADE_MAX_SETTINGS_PER_PLUGIN
    StoreError,  // settingsUiSet refused for some OTHER reason - defensive:
                 // every value reaching it has already passed the checks
                 // above, so this should not happen in practice
    Refused,     // the field's own `kind` is not TEXT/NUMBER/BOOL - internal:
                 // settingFieldsFrom already filters these out, so a caller
                 // going through the normal path never sees this either
};

// Decides whether `typed` may be stored in `f`, and if so, exactly what text
// is stored (`stored`). Pure - it touches no store.
//
//   TEXT:   at most maxLength bytes of valid UTF-8 with no control
//           characters; stored exactly as typed. "" is a real value (the user
//           cleared the field), not a deletion - a deletion would bring a
//           non-empty default straight back.
//   NUMBER: leading/trailing spaces trimmed; "" is accepted as "no number";
//           otherwise [+-] digits [. digits] [e|E [+-] digits], finite,
//           stored as the trimmed text the user typed.
//   BOOL:   "1" stays "1"; ANYTHING else is stored as "" (unchecked). Never a
//           deletion, for the same reason as TEXT: a spec defaulting to "1"
//           must be able to be switched off and stay off.
SettingEditResult prepareSettingEdit(const SettingField& f, const std::string& typed,
                                     std::string& stored);

// Validates `typed` and, when accepted, writes it through
// PluginApiCore::settingsUiSet - the one path that advances settings_seq.
SettingEditResult commitSettingEdit(PluginApiCore& api, const std::string& pluginName,
                                    const SettingField& f, const std::string& typed);

// What the field shows: the stored value, or the spec's defaultValue when the
// store has none (CASCADE_API_NOT_FOUND) - the exact rule the header gives a
// plugin reading the same key, so the host and the plugin cannot disagree.
std::string settingFieldValue(const PluginApiCore& api, const std::string& pluginName,
                              const SettingField& f);

// A BOOL's stored text as a checkbox state: "1" is checked, everything else
// ("" / "0" / anything a plugin wrote itself) is not.
bool settingBoolChecked(const std::string& value);

}  // namespace cascade::core
