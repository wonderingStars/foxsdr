// plugin_settings_form.cpp - see plugin_settings_form.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/plugin_settings_form.hpp"

#include <cfloat>
#include <cstring>

#include "core/i18n.hpp"
#include "core/plugin_api.hpp"
#include "core/utf8_text.hpp"
#include "gui/theme.hpp"
#include "imgui.h"

using cascade::i18n::tr;

namespace cascade::gui {

namespace {

// Copies `v` into the box, cut at a whole UTF-8 character if it does not fit
// (a value the plugin stored itself may be longer than the field allows).
void fillBuffer(std::vector<char>& buf, const std::string& v) {
    const std::size_t cap = buf.size();
    std::size_t n = v.size() < cap - 1 ? v.size() : cap - 1;
    std::memcpy(buf.data(), v.data(), n);
    n = core::utf8Floor(buf.data(), n);
    buf[n] = '\0';
}

}  // namespace

std::string settingFieldStateKey(const char* scope, const std::string& pluginName,
                                 const std::string& key) {
    // \x1f (unit separator) cannot occur in a store key, and a plugin name
    // containing it would only ever collide with itself.
    std::string k = scope != nullptr ? scope : "";
    k += '\x1f';
    k += pluginName;
    k += '\x1f';
    k += key;
    return k;
}

const char* settingEditMessage(core::SettingEditResult r) {
    switch (r) {
        case core::SettingEditResult::Accepted:
            return "";
        case core::SettingEditResult::TooLong:
            return tr("Not saved: longer than this field allows.");
        case core::SettingEditResult::NotANumber:
            return tr("Not saved: that is not a number.");
        case core::SettingEditResult::BadText:
            return tr("Not saved: it holds a character a setting cannot.");
        case core::SettingEditResult::StoreFull:
            return tr("Not saved: the plugin's settings are full.");
        case core::SettingEditResult::StoreError:
            return tr("Not saved: the store refused it.");
        case core::SettingEditResult::Refused:
            return tr("Not saved: this field is not one the host can store.");
    }
    return "";
}

int drawPluginSettingsForm(core::PluginApiCore& api, const std::string& pluginName,
                           const std::vector<core::SettingField>& fields,
                           PluginSettingsFormState& state, const char* scope) {
    int stored = 0;
    ImGui::PushID(scope != nullptr ? scope : "");
    ImGui::PushID(pluginName.c_str());
    for (const core::SettingField& f : fields) {
        SettingFieldState& fs = state.fields[settingFieldStateKey(scope, pluginName, f.key)];
        // Kept current for flushUntouchedSettingsEdits(): whichever surface
        // drew this field LAST is the one whose commit rule applies if the
        // surface disappears before the box is left the ordinary way.
        fs.pluginName = pluginName;
        fs.field = f;
        fs.touchedThisFrame = true;
        const std::string shown = core::settingFieldValue(api, pluginName, f);
        ImGui::PushID(f.key.c_str());

        if (f.kind == CASCADE_SETTING_BOOL) {
            bool on = core::settingBoolChecked(shown);
            // The label is drawn first, with TextUnformatted, exactly the way
            // the TEXT/NUMBER branch below draws its label - and the checkbox
            // that follows on the same line has a bare id, "##v" alone.
            // Concatenating f.label with an id suffix (the previous approach)
            // is NOT safe: a label ENDING in "#" plus a leading "##" from the
            // suffix forms "###", and ImGui hides everything from the first
            // "##" onward - so the label's own trailing "#" would silently
            // vanish from what is shown, on top of the suffix. Drawing the
            // two separately means the label string is never touched,
            // whatever the plugin put in it (cleanPluginText already
            // collapsed any internal "##" sequence to a single "#" so IT
            // cannot fake an id boundary either). The checkbox is drawn LAST
            // so it is the item a caller reads back with GetItemRectMin/Max
            // (as the widget test here does) - the same rule the TEXT/NUMBER
            // branch's InputText already follows.
            ImGui::TextUnformatted(f.label.c_str());
            ImGui::SameLine();
            if (ImGui::Checkbox("##v", &on)) {
                fs.last = core::commitSettingEdit(api, pluginName, f, on ? "1" : "");
                if (fs.last == core::SettingEditResult::Accepted) { ++stored; }
            }
        } else {
            const std::size_t cap = f.maxLength + 1u;
            if (fs.buf.size() != cap) {
                fs.buf.assign(cap, '\0');
                fs.editing = false;
            }
            if (!fs.editing) { fillBuffer(fs.buf, shown); }
            ImGui::TextUnformatted(f.label.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##v", f.placeholder.c_str(), fs.buf.data(), fs.buf.size());
            fs.editing = ImGui::IsItemActive();
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                fs.editing = false;
                fs.last = core::commitSettingEdit(api, pluginName, f, std::string(fs.buf.data()));
                if (fs.last == core::SettingEditResult::Accepted) { ++stored; }
                // Next frame refills from the store: a refused edit snaps back.
            }
        }

        if (fs.last != core::SettingEditResult::Accepted) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::bad());
            ImGui::TextWrapped("%s", settingEditMessage(fs.last));
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    ImGui::PopID();
    ImGui::PopID();
    return stored;
}

void beginSettingsFormFrame(PluginSettingsFormState& state) {
    for (auto& kv : state.fields) { kv.second.touchedThisFrame = false; }
}

int flushUntouchedSettingsEdits(core::PluginApiCore& api, PluginSettingsFormState& state) {
    int flushed = 0;
    for (auto& kv : state.fields) {
        SettingFieldState& fs = kv.second;
        // Not mid-edit: a BOOL (never `editing`), a TEXT/NUMBER box the user
        // left the ordinary way already, or one that was drawn this frame and
        // is still being typed in - nothing to lose yet either way.
        if (!fs.editing || fs.touchedThisFrame) { continue; }
        fs.editing = false;
        fs.last = core::commitSettingEdit(api, fs.pluginName, fs.field, std::string(fs.buf.data()));
        if (fs.last == core::SettingEditResult::Accepted) { ++flushed; }
        // No redraw follows for a surface that is gone, so there is nowhere
        // to show fs.last - it is still recorded in case the same surface
        // (a plugin re-fitted under the same name, a node reselected) comes
        // back and finds it.
    }
    return flushed;
}

}  // namespace cascade::gui
