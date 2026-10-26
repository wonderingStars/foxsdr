// app_window_plugin_settings.cpp - A PLUGIN'S SETTINGS FORM in the window, and the rows of its panel that are keys (stub campaign B9, PLUGIN-SETTINGS-UI
// and PLUGIN-ROW-KEYS; master 0.99.43 - 0.99.77).
//
// THE FORM. A plugin that declares CASCADE_CAP_SETTINGS_UI (a callsign, a power, a checkbox, a path to a program the user installed) gets its fields
// drawn by the host, folded under a "Plugin settings" line on the DECODE bank, on the plugin's own windows and on its node's inspector on the patch page.
// The drawing is gui/plugin_settings_form.cpp and holds no plugin API (the fence denies it). This file is the window's side of it: the list
// PLUGIN_SETTINGS read when listSeq moves (what each module declares, and the number its value is read by), the sink the form reads and edits through
// (fact.plugin.setting.<id>; FOXAPP_OP_PLUGIN_SETTING_EDIT, whose ANSWER the form takes from the drain that runs once a frame), and the per-frame hooks
// the commit-on-leave rule needs. An edit goes when the box is LEFT; the engine judges it and writes it through PluginApiCore::settingsUiSet - the one
// path that advances the plugin's settings_seq - so the plugin sees it move once for each ACCEPTED edit and never for a keystroke.
//
// THE ROWS. A row in a plugin's panel that carries CASCADE_ROW_FLAG_KEY and the id of a command the plugin registered is drawn as a key (one
// selectable spanning the row). The rule - which rows ask, which the host will press - is core/plugin_ui_data.hpp's (hoisted from plugin_api.hpp, which
// the fence denies); the registered ids are PLUGIN_COMMANDS; a press is FOXAPI_OP_PLUGIN_COMMAND, and the plugin takes it with poll_command from wherever
// it runs. Nothing of the plugin's runs here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/app_commands.hpp"
#include "core/i18n.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_ui_data.hpp"
#include "gui/app_window.hpp"
#include "gui/plugin_settings_form.hpp"
#include "gui/theme.hpp"
#include "gui/ui_census.hpp"

namespace cascade::gui {

using cascade::i18n::tr;
using cascade::i18n::trId;

// THE SINK THE WINDOW'S FORMS READ AND EDIT THROUGH: the table, and the commands on the window's session.
class PluginSettingsSink final : public SettingsEditSink {
public:
    explicit PluginSettingsSink(AppWindow& w) : w_(w) {}

    // What the field shows: fact.plugin.setting.<id> (the stored text, else the plugin's default), read when listSeq moves - never per frame. A field
    // the table does not list (the form was drawn from a list that has moved on) shows its default.
    std::string value(const std::string& owner, const core::SettingField& f) override {
        const PluginSettingsSet* set = w_.settingsLists().find(owner);
        if (set == nullptr) { return f.defaultValue; }
        for (std::size_t i = 0; i < set->fields.size(); ++i) {
            if (set->fields[i].key != f.key) { continue; }
            char key[64];
            std::snprintf(key, sizeof(key), "fact.plugin.setting.%llu", static_cast<unsigned long long>(set->ids[i]));
            return w_.uiFact(key);
        }
        return f.defaultValue;
    }

    // THE EDIT, SENT: "<module file> US <key> US <value>" as FOXAPP_OP_PLUGIN_SETTING_EDIT (the value is the text exactly as typed or chosen; the long
    // text carries what the command's 255 bytes cannot). The answer comes back through AppWindow::settingsOnAnswer by this ticket. A command the session
    // could not take (no session, its queue full, a refusal at the door) has no ticket, and the box says the store refused it.
    SettingsCommit commit(const std::string& owner, const core::SettingField& f, const std::string& typed) override {
        SettingsCommit c;
        const std::string text = owner + '\x1F' + f.key + '\x1F' + typed;
        c.ticket = w_.uiSubmitTicket(cascade::core::cmd::makeText(FOXAPP_OP_PLUGIN_SETTING_EDIT, text));
        if (c.ticket == 0u) { c.verdict = core::SettingEditResult::StoreError; }
        return c;
    }

private:
    AppWindow& w_;
};

// THE FORMS THE TABLE LISTS (PLUGIN_SETTINGS, API 0.3 9.7), read when listSeq moves and only by a surface that is drawing one: the list holds each
// field's label, its module file, its kind and its longest value; the field's key and placeholder are fact.plugin.setting.<id>.spec ("<key> US
// <placeholder>"). A module's fields are together, in its own order; the cache keeps each module's as the vector the form draws.
const PluginSettingsLists& AppWindow::settingsLists() {
    PluginSettingsLists& c = pluginSettingsLists_;
    if (uiSession_ == nullptr || c.listSeq == uiState_.listSeq) { return c; }
    c.listSeq = uiState_.listSeq;
    c.sets.clear();
    const int32_t n = std::max<int32_t>(0, engineApi_->read_list(uiSession_, FOXAPI_LIST_PLUGIN_SETTINGS, 0, nullptr));
    FoxListItem it{};
    std::string key;
    std::string spec;
    for (int32_t i = 0; i < n; ++i) {
        it = FoxListItem{};
        it.structSize = sizeof(it);
        if (engineApi_->read_list(uiSession_, FOXAPI_LIST_PLUGIN_SETTINGS, static_cast<uint32_t>(i), &it) <= i) { break; }
        char factKey[80];
        std::snprintf(factKey, sizeof(factKey), "fact.plugin.setting.%llu.spec", static_cast<unsigned long long>(it.id));
        readFactText(factKey, spec);
        const std::size_t us = spec.find('\x1F');
        if (us == std::string::npos || us == 0u) { continue; }  // a field the table cannot name is not drawn
        if (c.sets.empty() || c.sets.back().file != it.detail) {
            c.sets.emplace_back();
            c.sets.back().file = it.detail;
        }
        core::SettingField f;
        f.key = spec.substr(0, us);
        f.placeholder = spec.substr(us + 1u);
        f.label = it.name;
        f.kind = static_cast<std::uint32_t>(it.value[0]);
        f.maxLength = static_cast<std::size_t>(it.value[1] > 0.0 ? it.value[1] : 0.0);
        c.sets.back().fields.push_back(std::move(f));
        c.sets.back().ids.push_back(it.id);
    }
    return c;
}

bool AppWindow::pluginHasSettings(const std::string& file) {
    const PluginSettingsSet* s = settingsLists().find(file);
    return s != nullptr && !s->fields.empty();
}

// THE FIELDS OF ONE MODULE, drawn where the caller stands. `scope` names the surface ("rail", "window", "patch"): the same plugin drawn in two places
// at once keeps two boxes.
void AppWindow::drawPluginSettingsFields(const std::string& file, const char* scope) {
    const PluginSettingsSet* s = settingsLists().find(file);
    if (s == nullptr || s->fields.empty()) { return; }
    PluginSettingsSink sink(*this);
    drawPluginSettingsForm(sink, file, s->fields, pluginSettingsForms_, scope);
}

// A PLUGIN'S OWN WINDOW'S FORM, folded by default: a map or a picture is what the window is for, and a callsign is typed once. One line says the form is
// there; none at all for a plugin without one, which must not read as one that is missing it.
void AppWindow::drawPluginSettingsSection(const std::string& displayName) {
    if (displayName.empty()) { return; }
    const std::string file = moduleFileForName(pluginLists().plugins, displayName);
    if (file.empty() || !pluginHasSettings(file)) { return; }
    if (ImGui::TreeNodeEx(trId("Plugin settings##pluginsettings"), 0)) {
        drawPluginSettingsFields(file, "window");
        ImGui::TreePop();
    }
    ImGui::Separator();
}

// THE DECODE BANK'S LINE (0.99.74): FOLDED BY DEFAULT. A tester scrolling the bank (2026-10-12): "if I am scrolling down through the various decoders,
// you get to the Demod Analyzer and have to scroll past a bunch of settings. It would be nice if these were inside of a sub-menu that was collapsed
// by default instead". A tree node because benchSection keeps its pending press in statics and cannot nest inside the section this is drawn in. A box
// half typed in when the fold closes is committed by the frame's flush, as it is when a window closes. The census says where the line is and whether it
// is open, so a test can press it.
void AppWindow::drawPluginSettingsRailNode(const std::string& file) {
    if (!pluginHasSettings(file)) { return; }
    const bool open = ImGui::TreeNodeEx(trId("Plugin settings##railsettings"), 0);
    if (cascade::gui::census::enabled()) {
        const ImVec2 nodeMin = ImGui::GetItemRectMin();
        const ImVec2 nodeMax = ImGui::GetItemRectMax();
        cascade::gui::census::rect("decoders:settingsnode", nodeMin.x, nodeMin.y, nodeMax.x, nodeMax.y);
        cascade::gui::census::note(open ? "decoders:settings:open" : "decoders:settings:folded");
    }
    if (open) {
        drawPluginSettingsFields(file, "rail");
        ImGui::TreePop();
    }
}

// THE PATCH PAGE'S INSPECTOR: the form on the node that runs the plugin. The node's key is the module's file key (with the picture suffix for an image
// part); the form and its store belong to the module, so the same values reach the patch's copy and the receiver's copy.
void AppWindow::drawPluginSettingsPatchNode(const std::string& nodePlugin) {
    std::string modKey = nodePlugin;
    const std::string suffix = cascade::core::patch::kImageKeySuffix;
    if (modKey.size() > suffix.size() && modKey.compare(modKey.size() - suffix.size(), suffix.size(), suffix) == 0) {
        modKey.resize(modKey.size() - suffix.size());
    }
    bool loaded = false;
    for (const PluginRow& r : pluginLists().plugins) {
        if (r.file == modKey) {
            loaded = r.loaded;
            break;
        }
    }
    if (!loaded || !pluginHasSettings(modKey)) { return; }
    ImGui::Spacing();
    ImGui::SeparatorText(tr("Plugin settings"));
    drawPluginSettingsFields(modKey, "patch");
}

// THE FRAME'S FIRST AND LAST WORD ON THE FORMS. The first clears every box's "drawn this frame" mark; the last (after every surface that could draw a
// form has had its turn) commits a box that was being typed in when its surface stopped drawing it - a window closing, a fold shutting, a node
// deselected - none of which fires ImGui's own deactivation, because the box is simply never drawn again. A box that is left the ordinary way is
// committed there; it is never committed per keystroke.
void AppWindow::settingsFormsBeginFrame() { beginSettingsFormFrame(pluginSettingsForms_); }

void AppWindow::settingsFormsFlush() {
    PluginSettingsSink sink(*this);
    flushUntouchedSettingsEdits(sink, pluginSettingsForms_);
}

// THE ENGINE'S ANSWER to an edit this window sent, taken from the once-a-frame drain: the box that sent it shows the refusal's sentence and goes back
// to what the store holds, or - accepted - keeps what it was accepted as until the table reads it back.
void AppWindow::settingsOnAnswer(const FoxCommandResult& r) {
    (void)pluginSettingsForms_.deliver(r.ticket, settingVerdictFromAnswer(r.status, r.applied[0]));
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// THE ROWS OF A PLUGIN'S PANEL THAT ARE KEYS (0.99.77)
// ---------------------------------------------------------------------------------------------------------------------------------------------

// THE COMMANDS A MODULE REGISTERED (PLUGIN_COMMANDS: the id FOXAPI_OP_PLUGIN_COMMAND takes, the label, the module file), read when listSeq moves and
// only when a table has a row that asks to be a key - most panels have none. An id nobody registered is not a key, whatever a row asks.
const std::vector<cascade::core::HostCommand>& AppWindow::registeredCommandsOf(const std::string& file) {
    static const std::vector<cascade::core::HostCommand> kNone;
    PluginCommandRegistry& c = pluginCommandRegistry_;
    if (uiSession_ == nullptr) { return kNone; }
    if (c.listSeq != uiState_.listSeq) {
        c.listSeq = uiState_.listSeq;
        c.byFile.clear();
        const int32_t n = std::max<int32_t>(0, engineApi_->read_list(uiSession_, FOXAPI_LIST_PLUGIN_COMMANDS, 0, nullptr));
        FoxListItem it{};
        for (int32_t i = 0; i < n; ++i) {
            it = FoxListItem{};
            it.structSize = sizeof(it);
            if (engineApi_->read_list(uiSession_, FOXAPI_LIST_PLUGIN_COMMANDS, static_cast<uint32_t>(i), &it) <= i) { break; }
            c.byFile[it.detail].push_back(cascade::core::HostCommand{static_cast<std::uint32_t>(it.id), it.name});
        }
    }
    const auto at = c.byFile.find(file);
    return at == c.byFile.end() ? kNone : at->second;
}

namespace {

// The rows a plugin publishes, as a table: a PANEL's whole window and an INSTRUMENT's memory beneath its face are the same feed drawn the same way,
// so one drawing serves both and the two cannot drift apart.
//
// A ROW THAT IS A KEY (0.99.77, CASCADE_ROW_FLAG_KEY). "If I click on HD2 on the screen below, it should select that program, instead of having to go
// into the plugin settings and change the Programme to 1" (a US tester, 2026-10-13): a table row whose flags carry the flag and the id of a command the
// plugin registered is drawn as ONE selectable spanning the row - the file's own idiom for a row that can be pressed (the map's target list) - with the
// cell texts drawn over it as they always were, so GOOD, WARN and MUTED still apply and the row lights under the pointer. The id of the one pressed this
// frame is RETURNED (0 when none), and the caller sends it as FOXAPI_OP_PLUGIN_COMMAND; this function touches nothing of the plugin's. `registered` is
// the plugin's commands: a key whose id is not among them is drawn as the text it was before, because the engine would refuse it and a key that does
// nothing is a lie.
//
// `plugin` names the window in the UI census: panelrow:<plugin>:<id>:key is a row that is pressable (with its rectangle, for a scripted click), :text is
// a row that asked to be a key and is not (tests/test_row_keys_app). An instrument's memory rows pass the suffix ":memory", so they are not taken for the
// panel's; the names are put together only when the census is on.
std::uint32_t drawRowTable(const std::vector<std::string>& headings, const std::vector<CascadePanelRow>& rows,
                           const std::vector<cascade::core::HostCommand>& registered, const std::string& plugin,
                           const char* censusSuffix) {
    std::uint32_t pressed = 0u;
    const int cols = static_cast<int>(headings.size());
    if (cols > 0 &&
        ImGui::BeginTable("##rows", cols,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        for (const std::string& h : headings) { ImGui::TableSetupColumn(h.c_str()); }
        ImGui::TableHeadersRow();
        int rowNo = 0;
        for (const CascadePanelRow& r : rows) {
            ++rowNo;
            if (r.kind == CASCADE_ROW_SEPARATOR) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Separator();
                continue;
            }
            ImGui::TableNextRow();
            if (r.kind == CASCADE_ROW_HEADING) {
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextUnformatted(r.cells[0], r.cells[0] + ::strnlen(r.cells[0], CASCADE_PANEL_CELL_CHARS));
                ImGui::PopStyleColor();
                continue;
            }
            const std::uint32_t asked = cascade::core::panelRowKeyId(r);
            const std::uint32_t keyId = cascade::core::panelRowPressable(r, registered);
            for (int c = 0; c < cols; ++c) {
                ImGui::TableNextColumn();
                if (c == 0 && keyId != 0u) {
                    // THE WHOLE ROW IS THE KEY. Submitted first and spanning every column, so the cell texts below are drawn over it; AllowOverlap is
                    // what keeps a later cell's item from being swallowed by it (the map's target list's reason). A press is the release over it, as
                    // for every other key here.
                    ImGui::PushID(rowNo);
                    if (ImGui::Selectable("##rowkey", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                        pressed = keyId;
                    }
                    if (ImGui::IsItemHovered()) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); }
                    if (cascade::gui::census::enabled()) {
                        const std::string name = "panelrow:" + plugin + censusSuffix + ":" + std::to_string(keyId) + ":key";
                        cascade::gui::census::note(name);
                        cascade::gui::census::rect(name, ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                                   ImGui::GetItemRectMax().y);
                    }
                    ImGui::PopID();
                    // Back onto the selectable's own line, so the first cell's text starts where the cell does.
                    ImGui::SameLine(0.0f, 0.0f);
                }
                // BOUNDED, AND UNFORMATTED. The ABI says the cells are NUL-terminated, but a plugin that fills every byte must not walk the host off the
                // end of the array, so the end is measured with strnlen. Measured rather than printed through "%.*s": under Georgia every cell grew a
                // tail of question marks the moment the bench went over to it (0.84.0), where Saira had drawn the same rows clean - the format path
                // handed the renderer bytes past the terminator, and Georgia has no silent glyph for them. TextUnformatted with an explicit end hands
                // it exactly the word and nothing after.
                const char* cell = r.cells[c];
                const char* cellEnd = cell + ::strnlen(cell, CASCADE_PANEL_CELL_CHARS);
                if ((r.flags & CASCADE_ROW_FLAG_WARN) != 0u) {
                    ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
                    ImGui::TextUnformatted(cell, cellEnd);
                    ImGui::PopStyleColor();
                } else if ((r.flags & CASCADE_ROW_FLAG_GOOD) != 0u) {
                    ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::good());
                    ImGui::TextUnformatted(cell, cellEnd);
                    ImGui::PopStyleColor();
                } else if ((r.flags & CASCADE_ROW_FLAG_MUTED) != 0u) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    ImGui::TextUnformatted(cell, cellEnd);
                    ImGui::PopStyleColor();
                } else {
                    ImGui::TextUnformatted(cell, cellEnd);
                }
                // A row that ASKED to be a key and is not one (an id the plugin never registered): said to the census with where its first cell is, so
                // a bounded run can press it and see that nothing reaches the plugin.
                if (c == 0 && asked != 0u && keyId == 0u && cascade::gui::census::enabled()) {
                    const std::string name = "panelrow:" + plugin + censusSuffix + ":" + std::to_string(asked) + ":text";
                    cascade::gui::census::note(name);
                    cascade::gui::census::rect(name, ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                               ImGui::GetItemRectMax().y);
                }
            }
        }
        ImGui::EndTable();
    }
    if (rows.empty()) { ImGui::TextDisabled("%s", tr("Nothing to show yet.")); }
    return pressed;
}

}  // namespace

// ONE PLUGIN'S ROWS DRAWN AND, WHEN ONE OF THEM IS PRESSED, DELIVERED: the commands the plugin registered are asked for only when a row asks to be a key,
// and a press is FOXAPI_OP_PLUGIN_COMMAND with the module file as its text and the id in ival[0] - once for the one click, however many frames the
// release is seen over (a selectable answers true once). `plugin` is the display name, which is what the census calls it, followed by `censusSuffix`
// (":memory" for an instrument's rows).
void AppWindow::drawPluginRows(const std::string& plugin, const char* censusSuffix, const std::vector<std::string>& headings,
                               const std::vector<CascadePanelRow>& rows) {
    const std::string file = moduleFileForName(pluginLists().plugins, plugin);
    static const std::vector<cascade::core::HostCommand> kNone;
    const std::vector<cascade::core::HostCommand>* registered = &kNone;
    if (!file.empty()) {
        for (const CascadePanelRow& r : rows) {
            if (cascade::core::panelRowKeyId(r) != 0u) {
                registered = &registeredCommandsOf(file);
                break;
            }
        }
    }
    const std::uint32_t pressed = drawRowTable(headings, rows, *registered, plugin, censusSuffix);
    if (pressed != 0u && !file.empty()) {
        uiSubmit(cascade::core::cmd::makeText(FOXAPI_OP_PLUGIN_COMMAND, file, static_cast<std::int64_t>(pressed)));
    }
}

}  // namespace cascade::gui
