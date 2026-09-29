// app_window_patch_presets.cpp - the patch page's PRESETS panel.
//
// WHAT IT IS FOR. The patch page holds one patch, and a user with more than
// one set-up - the airband on one radio, a pager and the marine channels on
// another - rebuilt each by hand. A preset is a whole patch kept under a
// name (core/patch_presets.hpp has the rules); this panel is where they are
// saved, loaded, renamed and deleted.
//
// A PAGE, NOT A POPUP. It opens from the PRESETS key in the page's own control
// row, beside START/STOP and ALL OFF, and is drawn with beginPage like every
// other torn-off window, so it can sit beside the canvas while the patch is
// looked at. It is drawn only while the patch face is showing: with the
// receiver up there is no patch in front of the user to save.
//
// LOADING IS SAFE BY CONSTRUCTION, and none of that safety is here: the panel
// calls loadPatchPreset, which stops a running patch through its own STOP,
// leaves the new one stopped, and keeps the replaced one in the "(previous
// patch)" slot (see app_window.hpp). The panel only says so.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <string>

#include <imgui.h>

#include "core/i18n.hpp"
#include "core/patch_presets.hpp"
#include "core/utf8_text.hpp"
#include "gui/theme.hpp"

namespace cascade::gui {
using cascade::core::PatchPresetStatus;
using cascade::i18n::tr;
using cascade::i18n::trId;

std::string AppWindow::patchPresetSentence(PatchPresetStatus st, const std::string& name) const {
    std::string out;
    switch (st) {
        case PatchPresetStatus::Saved:
        case PatchPresetStatus::Overwritten:
            cascade::core::formatUtf8(out, tr("Saved \"%s\"."), name.c_str());
            break;
        case PatchPresetStatus::Renamed:
            cascade::core::formatUtf8(out, tr("Renamed to \"%s\"."), name.c_str());
            break;
        case PatchPresetStatus::Deleted:
            cascade::core::formatUtf8(out, tr("Deleted \"%s\"."), name.c_str());
            break;
        case PatchPresetStatus::Exists:
            out = tr("Another preset already has that name.");
            break;
        case PatchPresetStatus::EmptyName:
            out = tr("Type a name for the preset first.");
            break;
        case PatchPresetStatus::ReservedName:
            out = tr("That name is kept for the previous patch. Choose another.");
            break;
        case PatchPresetStatus::ListFull:
            cascade::core::formatUtf8(out, tr("There are already %zu presets. Delete one to make room."),
                                      cascade::core::kMaxPatchPresets);
            break;
        case PatchPresetStatus::TooLarge:
            cascade::core::formatUtf8(
                out, tr("This patch is too large to keep as a preset (over %zu KB)."),
                cascade::core::kMaxPatchPresetTextBytes / 1024u);
            break;
        case PatchPresetStatus::NotAPatch:
            out = tr("That preset is not a patch this version can read.");
            break;
        case PatchPresetStatus::NotFound:
            out = tr("That preset is no longer there.");
            break;
    }
    return out;
}

void AppWindow::loadSelectedPatchPreset() {
    // Copied, not referenced: the load refills the previous-patch slot, and
    // loading the slot itself must not read a string it is overwriting.
    std::string text;
    std::string label;
    if (patchPresetSelPrevious_) {
        text = patchPresets_.previous();
        label = tr("(previous patch)");
    } else {
        const int i = patchPresets_.find(patchPresetSel_);
        if (i < 0) {
            patchPresetNote_ = patchPresetSentence(PatchPresetStatus::NotFound, patchPresetSel_);
            return;
        }
        text = patchPresets_.list()[static_cast<std::size_t>(i)].text;
        label = patchPresets_.list()[static_cast<std::size_t>(i)].name;
    }
    if (!loadPatchPreset(std::move(text))) {
        patchPresetNote_ = patchPresetSentence(PatchPresetStatus::NotAPatch, label);
        return;
    }
    cascade::core::formatUtf8(patchPresetNote_, tr("Loaded \"%s\". Press START to run it."),
                              label.c_str());
}

void AppWindow::drawPatchPresetsPage() {
    if (!patchPresetsOpen_ || !patchOpen_) { return; }
    constexpr float kW = 440.0f;
    constexpr float kH = 460.0f;
    ImGui::SetNextWindowSize(ImVec2(kW, kH), ImGuiCond_FirstUseEver);
    if (beginPage("Patch presets###patchpresetswindow", tr("PATCH PRESETS"), &patchPresetsOpen_, 0,
                  kW, kH)) {
        drawPatchPresetsBody();
    }
    endPage();
}

void AppWindow::drawPatchPresetsBody() {
    namespace th = cascade::gui::theme;
    const std::vector<cascade::core::PatchPreset>& list = patchPresets_.list();
    const bool hasPrevious = !patchPresets_.previous().empty();
    // A selection whose row has gone (deleted, or renamed from under it) is
    // forgotten rather than pointing at nothing.
    if (!patchPresetSel_.empty() && patchPresets_.find(patchPresetSel_) < 0) {
        patchPresetSel_.clear();
    }
    if (patchPresetSelPrevious_ && !hasPrevious) { patchPresetSelPrevious_ = false; }

    // --- the list -----------------------------------------------------------
    // Whatever height the keys, the name field and the note leave.
    const float below = ImGui::GetFrameHeightWithSpacing() * 2.0f +
                        ImGui::GetTextLineHeightWithSpacing() * 3.0f;
    const float listH = std::max(80.0f, ImGui::GetContentRegionAvail().y - below);
    bool loadNow = false;
    ImGui::BeginChild("##patchpresetlist", ImVec2(0.0f, listH), true);
    // One row. The name is LETTERED over an unlabelled Selectable rather than
    // passed as its label: a name is the user's own text, and one holding
    // "##" would otherwise lose everything after it - and change its id.
    const auto row = [&](const std::string& label, bool selected, bool muted) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::Selectable("##row", selected,
                                               ImGuiSelectableFlags_AllowDoubleClick);
        const bool doubled = pressed && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        ImGui::GetWindowDrawList()->AddText(
            at, muted ? th::kInkMuted : ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
        if (doubled) { loadNow = true; }
        return pressed;
    };
    if (hasPrevious) {
        // THE PREVIOUS-PATCH SLOT, first and muted: not one of the user's
        // presets, but the one mis-click it exists to undo.
        ImGui::PushID("previous");
        if (row(tr("(previous patch)"), patchPresetSelPrevious_, true)) {
            patchPresetSelPrevious_ = true;
            patchPresetSel_.clear();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("The patch the last load replaced. Load it to undo that load."));
        }
        ImGui::PopID();
    }
    for (std::size_t i = 0; i < list.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const bool selected =
            !patchPresetSelPrevious_ && cascade::core::patchPresetNamesMatch(list[i].name, patchPresetSel_);
        if (row(list[i].name, selected, false)) {
            patchPresetSel_ = list[i].name;
            patchPresetSelPrevious_ = false;
        }
        ImGui::PopID();
    }
    if (list.empty() && !hasPrevious) {
        ImGui::PushStyleColor(ImGuiCol_Text, th::vec(th::kInkMuted));
        ImGui::TextWrapped("%s", tr("No presets yet. SAVE AS... keeps the patch on the canvas under a name."));
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    // --- the keys -----------------------------------------------------------
    const bool anySelected = patchPresetSelPrevious_ || !patchPresetSel_.empty();
    const bool namedSelected = !patchPresetSelPrevious_ && !patchPresetSel_.empty();
    ImGui::BeginDisabled(!anySelected);
    if (ImGui::Button(trId("LOAD###presetload"))) { loadNow = true; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(trId("SAVE AS...###presetsaveas"))) {
        patchPresetEdit_ = PatchPresetEdit::SaveAs;
        cascade::core::formatUtf8(patchPresetName_, sizeof(patchPresetName_), "%s",
                                  namedSelected ? patchPresetSel_.c_str() : "");
        patchPresetFocusName_ = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!namedSelected);
    if (ImGui::Button(trId("RENAME###presetrename"))) {
        patchPresetEdit_ = PatchPresetEdit::Rename;
        cascade::core::formatUtf8(patchPresetName_, sizeof(patchPresetName_), "%s",
                                  patchPresetSel_.c_str());
        patchPresetFocusName_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(trId("DELETE###presetdelete"))) {
        patchPresetPending_ = patchPresetSel_;
        patchPresetAskDelete_ = true;
    }
    ImGui::EndDisabled();
    if (loadNow && anySelected) { loadSelectedPatchPreset(); }

    // --- the name field, for SAVE AS... and RENAME -----------------------------
    if (patchPresetEdit_ != PatchPresetEdit::None) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(tr("Name"));
        ImGui::SameLine();
        if (patchPresetFocusName_) {
            ImGui::SetKeyboardFocusHere();
            patchPresetFocusName_ = false;
        }
        // The buffer is the name's byte budget: ImGui stops at a whole
        // character, and the store cleans and trims what arrives.
        ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x * 0.5f));
        const bool entered = ImGui::InputText("##presetname", patchPresetName_,
                                              sizeof(patchPresetName_),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        const bool saving = patchPresetEdit_ == PatchPresetEdit::SaveAs;
        const bool go = ImGui::Button(saving ? trId("SAVE###presetsavego")
                                             : trId("RENAME###presetrenamego")) ||
                        entered;
        ImGui::SameLine();
        if (ImGui::Button(trId("CANCEL###presetcancel"))) { patchPresetEdit_ = PatchPresetEdit::None; }
        if (go) {
            const std::string name = cascade::core::cleanPatchPresetName(patchPresetName_);
            if (saving) {
                // Never an overwrite from here: a name already taken asks.
                const PatchPresetStatus st = savePatchPreset(name, false);
                if (st == PatchPresetStatus::Exists) {
                    patchPresetPending_ = name;
                    patchPresetAskOverwrite_ = true;
                } else {
                    patchPresetNote_ = patchPresetSentence(st, name);
                    if (st == PatchPresetStatus::Saved) {
                        patchPresetEdit_ = PatchPresetEdit::None;
                        patchPresetSel_ = name;
                        patchPresetSelPrevious_ = false;
                    }
                }
            } else {
                const PatchPresetStatus st = patchPresets_.rename(patchPresetSel_, name);
                patchPresetNote_ = patchPresetSentence(st, name);
                if (st == PatchPresetStatus::Renamed) {
                    patchPresetEdit_ = PatchPresetEdit::None;
                    patchPresetSel_ = name;
                }
            }
        }
    }

    // --- what the last action did ----------------------------------------------
    if (!patchPresetNote_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, th::vec(th::kInkMuted));
        ImGui::TextWrapped("%s", patchPresetNote_.c_str());
        ImGui::PopStyleColor();
    }

    // --- the two questions -------------------------------------------------------
    // Modal, like the map's "Stop following?": the answer decides whether a
    // saved patch is lost, so nothing else happens until it is given.
    if (patchPresetAskOverwrite_) {
        patchPresetAskOverwrite_ = false;
        ImGui::OpenPopup(trId("Replace preset?"));
    }
    if (ImGui::BeginPopupModal(trId("Replace preset?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::string q;
        cascade::core::formatUtf8(
            q, tr("A preset called \"%s\" already exists.\nReplace it with the patch on the canvas?"),
            patchPresetPending_.c_str());
        ImGui::TextUnformatted(q.c_str());
        ImGui::Separator();
        if (ImGui::Button(trId("REPLACE###presetreplace"))) {
            const PatchPresetStatus st = savePatchPreset(patchPresetPending_, true);
            patchPresetNote_ = patchPresetSentence(st, patchPresetPending_);
            if (st == PatchPresetStatus::Overwritten || st == PatchPresetStatus::Saved) {
                patchPresetEdit_ = PatchPresetEdit::None;
                patchPresetSel_ = patchPresetPending_;
                patchPresetSelPrevious_ = false;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(trId("CANCEL###presetkeep"))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (patchPresetAskDelete_) {
        patchPresetAskDelete_ = false;
        ImGui::OpenPopup(trId("Delete preset?"));
    }
    if (ImGui::BeginPopupModal(trId("Delete preset?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::string q;
        cascade::core::formatUtf8(q, tr("Delete the preset \"%s\"? This cannot be undone."),
                                  patchPresetPending_.c_str());
        ImGui::TextUnformatted(q.c_str());
        ImGui::Separator();
        if (ImGui::Button(trId("DELETE###presetdeletego"))) {
            const PatchPresetStatus st = patchPresets_.remove(patchPresetPending_);
            patchPresetNote_ = patchPresetSentence(st, patchPresetPending_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(trId("CANCEL###presetdeletecancel"))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

}  // namespace cascade::gui
