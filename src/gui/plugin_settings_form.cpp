// plugin_settings_form.cpp - see plugin_settings_form.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/plugin_settings_form.hpp"

#include <cfloat>
#include <cstring>

#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "foxsdr_api.h"
#include "gui/theme.hpp"
#include "gui/ui_census.hpp"
#include "imgui.h"

using cascade::i18n::tr;
using cascade::i18n::trId;

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

// What handing an edit to the sink came to, for the callers that count.
enum class Handed { Refused, Accepted, Sent };

// HANDS `typed` TO THE SINK AND FILES WHAT CAME BACK. A verdict known now (an adapter over the store) is the field's `last` at once; otherwise the
// edit is in flight: the box is PENDING - it keeps the text typed, and `expect` is what the store will hold if the edit is accepted, which the pure
// core::prepareSettingEdit says (the NUMBER trimmed, the checkbox "1" or ""). The old message goes: "until the next edit of it".
Handed handOver(SettingsEditSink& sink, const std::string& owner, const core::SettingField& f, SettingFieldState& fs,
                const std::string& typed) {
    std::string stored;
    const core::SettingEditResult pre = core::prepareSettingEdit(f, typed, stored);
    const SettingsCommit c = sink.commit(owner, f, typed);
    fs.hold = 0;
    if (c.verdict.has_value()) {
        fs.pending = false;
        fs.ticket = 0;
        fs.last = *c.verdict;
        return fs.last == core::SettingEditResult::Accepted ? Handed::Accepted : Handed::Refused;
    }
    fs.pending = true;
    fs.ticket = c.ticket;
    fs.last = core::SettingEditResult::Accepted;
    fs.expect = pre == core::SettingEditResult::Accepted ? stored : typed;
    return Handed::Sent;
}

}  // namespace

bool PluginSettingsFormState::deliver(std::uint64_t ticket, core::SettingEditResult verdict) {
    if (ticket == 0u) { return false; }
    bool any = false;
    for (auto& kv : fields) {
        SettingFieldState& fs = kv.second;
        if (!fs.pending || fs.ticket != ticket) { continue; }
        fs.pending = false;
        fs.ticket = 0;
        fs.last = verdict;
        // An accepted edit keeps showing what it was accepted as until the store reads it back; a refused one goes back to what the store holds
        // from the next frame.
        fs.hold = verdict == core::SettingEditResult::Accepted ? kSettingHoldFrames : 0;
        any = true;
    }
    return any;
}

core::SettingEditResult settingVerdictFromAnswer(int status, double applied0) {
    if (status == FOXAPI_OK || status == FOXAPI_NO_CHANGE) { return core::SettingEditResult::Accepted; }
    const int v = static_cast<int>(applied0);
    if (v >= static_cast<int>(core::SettingEditResult::TooLong) && v <= static_cast<int>(core::SettingEditResult::Refused)) {
        return static_cast<core::SettingEditResult>(v);
    }
    return status == FOXAPI_NOT_FOUND ? core::SettingEditResult::Refused : core::SettingEditResult::StoreError;
}

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

int drawPluginSettingsForm(SettingsEditSink& sink, const std::string& pluginName,
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
        const std::string shown = sink.value(pluginName, f);
        // THE BOX FOLLOWS WHAT WAS TYPED, NOT THE STORE, while an edit is on its way (pending) and for the few frames after an accepted one in
        // which the store has not read back what it accepted.
        bool follow = fs.pending;
        if (fs.hold > 0) {
            if (shown == fs.expect) {
                fs.hold = 0;
            } else {
                --fs.hold;
                follow = true;
            }
        }
        ImGui::PushID(f.key.c_str());

        if (f.kind == CASCADE_SETTING_BOOL) {
            bool on = core::settingBoolChecked(follow ? fs.expect : shown);
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
                if (handOver(sink, pluginName, f, fs, on ? "1" : "") == Handed::Accepted) { ++stored; }
            }
        } else {
            // A PATH FIELD (0.99.74) IS THIS SAME BOX WITH A KEY ON ITS LINE.
            // The owner, 2026-10-12: "have you made it easy for the user to add
            // the nrsc5 path to the sdr software" - until now the only way was
            // to type or paste a full path. The key is drawn only where the
            // host has a file dialog to open (state.pick); where it has none
            // the box stands alone, never beside a key that does nothing.
            const bool browse = f.kind == CASCADE_SETTING_PATH && static_cast<bool>(state.pick);
            const char* const browseLabel = browse ? trId("BROWSE FILES") : "";
            float keyWidth = 0.0f;  // the key, and the gap before it
            if (browse) {
                const ImGuiStyle& st = ImGui::GetStyle();
                keyWidth = ImGui::CalcTextSize(browseLabel, nullptr, true).x +
                           st.FramePadding.x * 2.0f + st.ItemSpacing.x;
            }
            const std::size_t cap = f.maxLength + 1u;
            if (fs.buf.size() != cap) {
                fs.buf.assign(cap, '\0');
                fs.editing = false;
            }
            if (!fs.editing && !follow) { fillBuffer(fs.buf, shown); }
            ImGui::TextUnformatted(f.label.c_str());
            ImGui::SetNextItemWidth(browse ? -keyWidth : -FLT_MIN);
            ImGui::InputTextWithHint("##v", f.placeholder.c_str(), fs.buf.data(), fs.buf.size());
            fs.editing = ImGui::IsItemActive();
            if (cascade::gui::census::enabled()) {
                // WHERE THE BOX IS, so a scripted click can focus it (tests/test_plugin_pathpick_app).
                const ImVec2 a = ImGui::GetItemRectMin();
                const ImVec2 b = ImGui::GetItemRectMax();
                census::note("settings:box:", f.key);
                census::rect("settings:box:" + f.key, a.x, a.y, b.x, b.y);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                fs.editing = false;
                if (handOver(sink, pluginName, f, fs, std::string(fs.buf.data())) == Handed::Accepted) { ++stored; }
                // Next frame refills from the store: a refused edit snaps back (for an edit that is answered later, when the answer says so).
            }
            if (browse) {
                ImGui::SameLine();
                const bool pressed = ImGui::Button(browseLabel);
                if (census::enabled()) {
                    census::note("settings:browse:", f.key);
                    const ImVec2 a = ImGui::GetItemRectMin();
                    const ImVec2 b = ImGui::GetItemRectMax();
                    census::rect("settings:browse:" + f.key, a.x, a.y, b.x, b.y);
                }
                if (pressed) {
                    core::FilePickRequest rq;
                    rq.title = tr("Choose a file");
                    rq.programsName = tr("Programs (*.exe)");
                    rq.allFilesName = tr("All files (*.*)");
                    rq.programs = core::placeholderNamesProgram(f.placeholder);
                    // The dialog opens beside what the box says NOW: pressing the key leaves the box, which commits what was typed in it on this very
                    // frame - the store already holds it for a sink that answers at once, and the box holds it for one that has the edit on its way.
                    rq.startFrom = (fs.pending || fs.hold > 0) ? std::string(fs.buf.data()) : sink.value(pluginName, f);
                    std::string chosen;
                    // A cancel (false) changes nothing at all. A file is committed
                    // through the call a typed edit takes - the store, and
                    // settings_seq for the plugin - and the box shows it from the
                    // next frame, or snaps back if the field refused it.
                    if (state.pick(rq, chosen) && !chosen.empty()) {
                        fs.editing = false;
                        fillBuffer(fs.buf, chosen);
                        if (handOver(sink, pluginName, f, fs, chosen) == Handed::Accepted) { ++stored; }
                    }
                }
            }
        }

        if (fs.last != core::SettingEditResult::Accepted) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::bad());
            ImGui::TextWrapped("%s", settingEditMessage(fs.last));
            ImGui::PopStyleColor();
            // THE VERDICT ON SHOW, so a bounded run can see that the engine's answer reached the box: "settings:message:<key>:<SettingEditResult>".
            if (census::enabled()) { census::note("settings:message:", f.key + ":" + std::to_string(static_cast<int>(fs.last))); }
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

int flushUntouchedSettingsEdits(SettingsEditSink& sink, PluginSettingsFormState& state) {
    int flushed = 0;
    for (auto& kv : state.fields) {
        SettingFieldState& fs = kv.second;
        // Not mid-edit: a BOOL (never `editing`), a TEXT/NUMBER box the user
        // left the ordinary way already, or one that was drawn this frame and
        // is still being typed in - nothing to lose yet either way.
        if (!fs.editing || fs.touchedThisFrame) { continue; }
        fs.editing = false;
        if (handOver(sink, fs.pluginName, fs.field, fs, std::string(fs.buf.data())) != Handed::Refused) { ++flushed; }
        // No redraw follows for a surface that is gone, so there is nowhere
        // to show fs.last - it is still recorded in case the same surface
        // (a plugin re-fitted under the same name, a node reselected) comes
        // back and finds it.
    }
    return flushed;
}

}  // namespace cascade::gui
