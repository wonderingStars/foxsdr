// app_window_patch_blueprints.cpp - the patch page's BLUEPRINTS list.
//
// WHAT IT IS FOR (owner, 2026-10-26: "preset blue prints for all the plugins
// ... select in the list adsb and it places a full work flow for that app on
// the patch panel"). One pick in a list on the parts band places a whole,
// wired, tuned workflow for one plugin: the radio, the channel and the
// demodulator it needs, the decoder, and a Text out, a Map and a Spectrum to
// show what it finds. core/patch_blueprints.hpp decides every node and wire;
// this file only reads each plugin's presets out of it, draws the list, and
// says what the pick did.
//
// EVERY INSTALLED PLUGIN, STOPPED OR NOT. The list is built from the parts
// bin's own catalogue, which since the same change keeps a plugin the user
// has stopped (rebuildPatchCatalogue says why), so a blueprint never waits
// for the plugin to be switched on first.
//
// PLACED WHERE THE USER WILL SEE IT. The workflow is laid out below whatever
// is already on the canvas, so the view is fitted to it - otherwise a pick on
// a busy canvas would land out of sight and read as "nothing happened", the
// 0.99.15 parts-bin report.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/i18n.hpp"
#include "core/patch_blueprints.hpp"
#include "core/utf8_text.hpp"
#include "gui/patch_view_math.hpp"
#include "gui/theme.hpp"
#include "gui/tune_control.hpp"
#include "gui/ui_census.hpp"
#include "gui/ui_scale.hpp"

namespace cascade::gui {
namespace pc = cascade::core::patch;
using cascade::i18n::tr;

namespace {
// The device rate a patch radio with none of its own runs at - the same
// number app_window_patch_radios.cpp's kPatchDefaultRateHz is, and the one
// the quarter-rate offset of an audio blueprint is measured against.
constexpr double kBlueprintDefaultRateHz = 2.0e6;

// What a blueprint is called in the list: a picture decoder says so, as its
// key in the parts bin does.
std::string shownTitle(const pc::Blueprint& bp) {
    if (!bp.decoder.image) { return bp.title; }
    std::string out;
    cascade::core::formatUtf8(out, tr("%s (picture)"), bp.title.c_str());
    return out;
}
}  // namespace

std::vector<pc::Blueprint> AppWindow::patchBlueprints() const {
    // Each loaded plugin's presets, through the same validity filter and cap
    // every other consumer of a preset table uses (validatedPresets), keyed
    // by the module key the catalogue names parts by.
    std::map<std::string, std::vector<pc::BlueprintTune>> tunes;
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (!p.loaded || p.preset == nullptr) { continue; }
        std::vector<pc::BlueprintTune>& list = tunes[cascade::core::pluginKey(p)];
        for (const cascade::gui::IndexedPreset& ip : validatedPresets(p)) {
            pc::BlueprintTune t;
            t.label = cascade::gui::presetLabel(ip.preset.label, p.name);
            t.freqHz = ip.preset.frequencyHz;
            t.mode = pc::demodIndexForAbi(ip.preset.demodMode);
            t.rateHz = ip.preset.sampleRateHz > 0.0 ? ip.preset.sampleRateHz : 0.0;
            list.push_back(std::move(t));
        }
    }
    return pc::listBlueprints(patchCatalogue_, tunes);
}

void AppWindow::drawPatchBlueprints() {
    namespace th = cascade::gui::theme;
    if (patchCatalogue_.empty()) { return; }   // the decoder row already says where to get one

    ImGui::PushStyleColor(ImGuiCol_Text, th::vec(th::kInkMuted));
    ImGui::TextUnformatted(tr("Blueprints"));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    const float room = patchBandRight_ - ImGui::GetCursorScreenPos().x;
    ImGui::SetNextItemWidth(std::max(cascade::gui::uiscale::px(120.0f),
                                     std::min(cascade::gui::uiscale::px(340.0f), room)));
    const bool open = ImGui::BeginCombo("##patchblueprints",
                                        tr("Pick a plugin to place its whole workflow"),
                                        ImGuiComboFlags_HeightLarge);
    {
        // Where the list is, for a scripted press.
        const ImVec2 k0 = ImGui::GetItemRectMin();
        const ImVec2 k1 = ImGui::GetItemRectMax();
        cascade::gui::census::rect("patchblueprints", k0.x, k0.y, k1.x, k1.y);
    }
    if (open) {
        // Read now, while the list is open, and not on every frame of the
        // page: it asks every plugin for its presets.
        const std::vector<pc::Blueprint> list = patchBlueprints();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const pc::Blueprint& bp = list[i];
            std::string label;
            cascade::core::formatUtf8(label, "%s###blueprint%zu", shownTitle(bp).c_str(), i);
            if (ImGui::Selectable(label.c_str(), false)) { patchBlueprintPending_ = bp; }
            {
                const ImVec2 k0 = ImGui::GetItemRectMin();
                const ImVec2 k1 = ImGui::GetItemRectMax();
                cascade::gui::census::rect("patchblueprint:", static_cast<int>(i), k0.x, k0.y,
                                           k1.x, k1.y);
            }
            if (ImGui::IsItemHovered() && bp.tune.freqHz > 0.0) {
                ImGui::SetTooltip("%.4f MHz", bp.tune.freqHz / 1.0e6);
            }
        }
        ImGui::EndCombo();
    }

    if (!patchBlueprintNote_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, th::vec(th::kInkMuted));
        ImGui::PushTextWrapPos(patchBandRight_ - ImGui::GetWindowPos().x);
        ImGui::TextWrapped("%s", patchBlueprintNote_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
}

void AppWindow::placePendingPatchBlueprint(float canvasW, float canvasH) {
    if (!patchBlueprintPending_) { return; }
    const pc::Blueprint bp = std::move(*patchBlueprintPending_);
    patchBlueprintPending_.reset();
    const std::string title = shownTitle(bp);

    // A NEW radio starts on a device nothing else is using, as one from the
    // parts bin does - and finding one reads the native list, so that is done
    // only when no radio on the canvas is free to be taken over.
    std::string device;
    if (pc::unwiredRadio(patchGraph_) == pc::kNoNode &&
        patchGraph_.count(pc::NodeKind::Radio) < pc::kMaxRadios) {
        scanNative();
        device = patchDefaultDeviceKey();
    }
    const pc::Placed placed =
        pc::placeBlueprint(patchGraph_, bp, device, kBlueprintDefaultRateHz);
    if (placed.result == pc::Place::RadiosFull) {
        cascade::core::formatUtf8(
            patchBlueprintNote_,
            tr("Every radio is in use, and a patch has at most %zu. Free a radio to place \"%s\"."),
            pc::kMaxRadios, title.c_str());
        return;
    }

    // THE NEW WORKFLOW FITTED TO THE CANVAS - the Fit key's own arithmetic
    // (fitView), asked of a graph holding only what this pick placed, so the
    // whole chain from radio to Text out is in view and the user's other
    // nodes are not what decides the zoom. Nothing is selected: a selection
    // opens the inspector, which would cover the right-hand end of the very
    // chain just placed.
    if (canvasW > 0.0f && canvasH > 0.0f) {
        pc::Graph only;
        std::vector<pc::NodeId> ids = placed.added;
        if (placed.reusedRadio) { ids.push_back(placed.radio); }
        for (const pc::NodeId id : ids) {
            const pc::Node* n = patchGraph_.find(id);
            if (n == nullptr) { continue; }
            if (pc::Node* c = only.mutableNode(only.addNode(n->kind, n->name))) {
                c->x = n->x;
                c->y = n->y;
                c->w = n->w;
                c->h = n->h;
            }
        }
        patchUi_.view = cascade::gui::patch::fitView(only, canvasW, canvasH,
                                                     cascade::gui::uiscale::factor());
    }
    patchUi_.selected = pc::kNoNode;
    patchUi_.dirty = true;
    cascade::core::formatUtf8(
        patchBlueprintNote_,
        placed.noFrequency
            ? tr("Placed \"%s\". It has no preset frequency - type one in before pressing START.")
            : tr("Placed \"%s\". Press START to run it."),
        title.c_str());
}

}  // namespace cascade::gui
