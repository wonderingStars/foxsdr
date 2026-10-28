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
// ON THE ENGINE-EXTRACTION BRANCH (the 0.99.97 merge, docs/engine-merge-0.99.97.md): master wrote this file against the window that owned the plugin host,
// the frequency manager and the patch graph. Here they are the engine's and the window reads the table: the catalogue is the PATCH_PARTS copy, the
// presets are PRESETS, the tower groups are BOOKMARKS' (the window's one copy) with the receiver's position from the state, and the workflow is placed
// on the page's own draft (patchDraft_) and committed to the engine as every edit is. The decisions - what is built, in what order, with what names -
// are core/patch_blueprints.hpp's and are master's, untouched.
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
#include "engine/tune_control.hpp"
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

// HOW MANY NEAREST AIRPORTS the list offers a tower for: the Airband
// section's own "Nearest airports" count.
constexpr std::size_t kTowerNearest = 6;

// THE AIRPORTS A "LISTEN TO THE TOWER" BLUEPRINT IS OFFERED FOR (2026-10-27):
// every airport the user has looked up in the Airband section - its group is
// in the frequency list, named airportGroupName() - and, with a receiver
// position, the kTowerNearest nearest it. Each once, looked-up first.
std::vector<const cascade::core::Airport*> towerAirports(
    const std::vector<cascade::gui::BookmarkRow>& list, bool rxSet, double lat, double lon) {
    namespace cc = cascade::core;
    std::vector<const cc::Airport*> out;
    const auto have = [&out](const cc::Airport* a) {
        return std::find(out.begin(), out.end(), a) != out.end();
    };
    std::vector<std::string> groups;
    for (const cascade::gui::BookmarkRow& b : list) {
        if (!b.group.empty() &&
            std::find(groups.begin(), groups.end(), b.group) == groups.end()) {
            groups.push_back(b.group);
        }
    }
    for (const std::string& g : groups) {
        // The group starts with the airport's ident: "KORD Chicago O'Hare ...".
        const std::string ident = g.substr(0, g.find(' '));
        for (const cc::Airport* a : cc::findAirports(cc::airbandTable(), ident)) {
            if (cc::airportGroupName(*a) == g && !have(a)) { out.push_back(a); }
        }
    }
    if (rxSet) {
        for (const auto& [a, km] : cc::nearestAirports(cc::airbandTable(), lat, lon, kTowerNearest)) {
            (void)km;
            if (!have(a)) { out.push_back(a); }
        }
    }
    return out;
}

// An airport's tower frequencies, as a listen blueprint's channels: its TWR
// frequencies, or - a field with no tower - its CTAF, where the traffic
// there talks. Named as the frequency list names them, type then channel.
std::vector<cascade::core::patch::ListenChannel> towerChannels(const cascade::core::Airport& a) {
    std::vector<cascade::core::patch::ListenChannel> out;
    for (const char* type : {"TWR", "CTAF"}) {
        for (const cascade::core::AirbandFreq& f : a.freqs) {
            if (f.type != type) { continue; }
            std::string name;
            cascade::core::formatUtf8(name, "%s %.3f", f.type.c_str(),
                                      static_cast<double>(f.channelKhz) / 1000.0);
            out.push_back({f.hz, name});
        }
        if (!out.empty()) { break; }
    }
    return out;
}
}  // namespace

std::vector<pc::Blueprint> AppWindow::patchBlueprints() {
    // Each plugin's presets, from the table's PRESETS list - which is the engine's validatedPresets, the same filter and cap every other consumer of a
    // preset table uses - keyed by the module key the catalogue names parts by. A PRESETS entry names its plugin by the plugin's NAME (detail), as
    // the parts do (DecoderInfo::name), so the module key of every part whose name it is gets the preset. value[0] is the frequency, value[1] the
    // ABI demod code, value[3] the preset's own sample rate (0 when it names none).
    const PatchListCache& lists = patchLists();
    struct Row {
        std::string plugin;
        pc::BlueprintTune tune;
    };
    std::vector<Row> rows;
    {
        const auto size = [this](std::uint32_t list) {
            return std::max<std::int32_t>(0, engineApi_->read_list(uiSession_, list, 0, nullptr));
        };
        const std::int32_t n = size(FOXAPI_LIST_PRESETS);
        for (std::int32_t i = 0; i < n; ++i) {
            FoxListItem it{};
            it.structSize = sizeof(it);
            if (engineApi_->read_list(uiSession_, FOXAPI_LIST_PRESETS, static_cast<std::uint32_t>(i), &it) <= i) { break; }
            Row r;
            r.plugin = it.detail;
            r.tune.label = cascade::gui::presetLabel(it.name, r.plugin);
            r.tune.freqHz = it.value[0];
            r.tune.mode = pc::demodIndexForAbi(static_cast<std::uint32_t>(it.value[1]));
            r.tune.rateHz = it.value[3] > 0.0 ? it.value[3] : 0.0;
            rows.push_back(std::move(r));
        }
    }
    std::map<std::string, std::vector<pc::BlueprintTune>> tunes;
    for (const pc::DecoderInfo& d : lists.parts) {
        std::string module = d.key;
        if (d.image) {
            const std::string suffix = pc::kImageKeySuffix;
            if (module.size() >= suffix.size() && module.compare(module.size() - suffix.size(), suffix.size(), suffix) == 0) {
                module.erase(module.size() - suffix.size());
            }
        }
        if (tunes.count(module) != 0) { continue; }
        std::vector<pc::BlueprintTune>& list = tunes[module];
        for (const Row& r : rows) {
            if (r.plugin == d.name) { list.push_back(r.tune); }
        }
    }
    std::vector<pc::Blueprint> out = pc::listBlueprints(lists.parts, tunes);
    // ...then LISTEN TO THE TOWER, one per airport, after the plugins.
    for (const cascade::core::Airport* a : towerAirports(tuningLists().markers, receiverPositionKnown(), uiState_.rxLatDeg, uiState_.rxLonDeg)) {
        pc::Blueprint bp;
        bp.listen = towerChannels(*a);
        if (bp.listen.empty()) { continue; }
        cascade::core::formatUtf8(bp.title, tr("Listen to the tower: %s"),
                                  cascade::core::airportGroupName(*a).c_str());
        out.push_back(std::move(bp));
    }
    return out;
}

void AppWindow::drawPatchBlueprints() {
    namespace th = cascade::gui::theme;
    // With no plugin the decoder row already says where to get one; the list
    // still shows when it can offer a tower - a receiver position, or groups
    // in the frequency list an airport lookup may have made.
    const std::vector<cascade::gui::BookmarkRow>& marks = tuningLists().markers;
    const bool anyGroup =
        std::any_of(marks.begin(), marks.end(), [](const cascade::gui::BookmarkRow& b) { return !b.group.empty(); });
    if (patchLists().parts.empty() && !receiverPositionKnown() && !anyGroup) { return; }

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
            } else if (ImGui::IsItemHovered() && !bp.listen.empty()) {
                // A tower's channels, one a line, as they will be named.
                std::string tip;
                for (const pc::ListenChannel& c : bp.listen) {
                    if (!tip.empty()) { tip += '\n'; }
                    tip += c.name;
                }
                ImGui::SetTooltip("%s", tip.c_str());
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
    syncPatchDraft();

    // A NEW radio starts on a device nothing else is using, as one from the parts bin does - and finding one reads the native list, so that is
    // asked for only when no radio on the canvas is free to be taken over. ON THE TABLE THE SCAN IS TWO PASSES (patchAddRadioPart's reason): the
    // radio is placed NOW with no device, the scan is asked for, and finishPatchRadioParts gives it the first free device when the answer arrives -
    // by which time the list exists. Master read the list inline and chose the device before placing.
    std::uint64_t scanTicket = 0;
    const bool needsNewRadio = pc::unwiredRadio(patchDraft_) == pc::kNoNode && patchDraft_.count(pc::NodeKind::Radio) < pc::kMaxRadios;
    if (needsNewRadio) {
        scanTicket = uiSubmitTicket(cascade::core::cmd::QueuedCommand{cascade::core::cmd::make(FOXAPP_OP_SCAN_NATIVE_ONLY), {}});
    }
    const pc::Placed placed = pc::placeBlueprint(patchDraft_, bp, std::string(), kBlueprintDefaultRateHz);
    if (placed.result == pc::Place::RadiosFull) {
        cascade::core::formatUtf8(
            patchBlueprintNote_,
            tr("Every radio is in use, and a patch has at most %zu. Free a radio to place \"%s\"."),
            pc::kMaxRadios, title.c_str());
        return;
    }
    if (!placed.reusedRadio && placed.radio != pc::kNoNode) {
        if (scanTicket != 0u) {
            patchRadioWaits_.push_back({placed.radio, scanTicket});
        } else if (pc::Node* n = patchDraft_.mutableNode(placed.radio)) {
            // The session would not take the scan: the list as it is.
            n->device = patchDefaultDeviceKey();
        }
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
            const pc::Node* n = patchDraft_.find(id);
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
    // To the engine before the compile below, as the part always was.
    commitPatchDraft();
    cascade::core::formatUtf8(
        patchBlueprintNote_,
        placed.noFrequency
            ? tr("Placed \"%s\". It has no preset frequency - type one in before pressing START.")
            : tr("Placed \"%s\". Press START to run it."),
        title.c_str());
    // A tower whose frequencies span more than one radio's band: the ones
    // left out are counted, so the user knows the patch is not all of them.
    if (placed.listenDropped > 0) {
        std::string more;
        cascade::core::formatUtf8(
            more, tr("%zu of its tower frequencies are outside this radio's band and were left out."),
            placed.listenDropped);
        patchBlueprintNote_ += ' ';
        patchBlueprintNote_ += more;
    }
}

}  // namespace cascade::gui
