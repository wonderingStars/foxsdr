// app_window_airspy.cpp - the Source section's Airspy R2 / Mini controls
// (0.99.41): the gain mode, only that mode's sliders, Free mode's two AGC
// switches, and the software decimation - laid out the way the reference
// Airspy application offers them ("Gain: Sensitive/Linear/Free" and
// "Decimation: none, 2, 4, 8, 16, 32 and 64", SDRsharp - The Guide v2.1).
//
// The rules are engine/airspy_panel.hpp's and the driver's; this file only
// DRAWS. The choosing (chooseAirspyDecimation/GainMode/Agc) moved to the
// Engine in the engine extraction merge (2026-09-28,
// docs/engine-merge-0.99.42.md); engine/stage3b-pre's Airspy round (OPEN 2/3)
// then turned those into COMMANDS (FOXAPP_OP_AIRSPY_DECIMATION/GAIN_MODE/AGC,
// queued like every other gain control here - the figure is read back on the
// next frame the readback lands on) and moved every READ off the raw
// cascade::source::AirspySource* engine_.asAirspyDevice() used to hand out onto
// the PUBLISHED state.
//
// SINCE THE WINDOW'S PORT (engine-stage4.md 6.5, group 4) THIS FILE DRAWS FROM
// ITS ENGINE API SESSION ONLY: the gain modes, the decimation factors and the
// gain stages are the table's lists (GAIN_MODES, DECIMATIONS, GAINS - the
// stage its own AGC is driving carries FOXAPI_ITEM_AGC, API 0.3 section 8.3),
// read when listSeq moves and kept in airspyCache_, and every control is
// AppWindow::uiSubmit with the op it always sent. tests/test_airspy_app.cpp is
// unaffected: it drives chooseAirspyDecimation/GainMode/Agc directly (a
// friend accessor), which still exist unchanged on Engine.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "engine/tune_control.hpp"
#include "gui/text_fit.hpp"

namespace cascade::gui {
using cascade::i18n::tr;
using cascade::i18n::trId;
using AirspyMode = cascade::source::AirspySource::GainMode;

namespace {

void tooltipIfHovered(const char* text) {
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", text); }
}

}  // namespace

// THE PANEL'S READS OF THE TABLE'S LISTS, taken when listSeq has moved and kept
// (the same rule as AppWindow::uiFact): the lists are rebuilt when anything in
// them changes, so a frame in which nothing did costs nothing and allocates
// nothing - the strings and vectors keep their capacity between refreshes.
const AppWindow::AirspyListCache& AppWindow::airspyLists() {
    AirspyListCache& c = airspyCache_;
    if (uiSession_ == nullptr || c.listSeq == uiState_.listSeq) { return c; }
    c.listSeq = uiState_.listSeq;
    const auto entry = [](FoxListItem& it) {
        it = FoxListItem{};
        it.structSize = sizeof(it);
    };
    FoxListItem it{};
    // An Airspy is open exactly when it has gain MODES (no other radio has any).
    const int32_t modes = engineApi_->read_list(uiSession_, FOXAPI_LIST_GAIN_MODES, 0, nullptr);
    c.open = modes > 0;
    c.choices.clear();
    c.stages.clear();
    c.lnaAgc = false;
    c.mixerAgc = false;
    if (!c.open) { return c; }
    // The mode in force: the code AirspySource::GainMode / FOXAPP_OP_AIRSPY_GAIN_MODE take.
    for (int32_t i = 0; i < modes; ++i) {
        entry(it);
        if (engineApi_->read_list(uiSession_, FOXAPI_LIST_GAIN_MODES, static_cast<uint32_t>(i), &it) < 0) { break; }
        if ((it.flags & FOXAPI_ITEM_ACTIVE) != 0u) { c.gainMode = static_cast<int>(it.value[0]); }
    }
    // The decimation factors in the combo's order; the active one gives the
    // factor in force and, with the rate it gives, the board's own rate
    // (a power of two divides it exactly).
    const int32_t decimations = engineApi_->read_list(uiSession_, FOXAPI_LIST_DECIMATIONS, 0, nullptr);
    for (int32_t i = 0; i < decimations; ++i) {
        entry(it);
        if (engineApi_->read_list(uiSession_, FOXAPI_LIST_DECIMATIONS, static_cast<uint32_t>(i), &it) < 0) { break; }
        c.choices.push_back(static_cast<unsigned>(it.value[0]));
        if ((it.flags & FOXAPI_ITEM_ACTIVE) != 0u) {
            c.decimation = static_cast<unsigned>(it.value[0]);
            c.hardwareRateHz = it.value[1] * it.value[0];
        }
    }
    // The stages: the name the gain command takes, the top of the slider, the
    // radio's readback, and whether its own AGC is driving it.
    const int32_t stages = engineApi_->read_list(uiSession_, FOXAPI_LIST_GAINS, 0, nullptr);
    for (int32_t i = 0; i < stages; ++i) {
        entry(it);
        if (engineApi_->read_list(uiSession_, FOXAPI_LIST_GAINS, static_cast<uint32_t>(i), &it) < 0) { break; }
        AirspyListCache::Stage s;
        s.name = it.name;
        s.maxDb = static_cast<float>(it.value[1]);
        s.db = static_cast<float>(it.value[3]);
        s.agc = (it.flags & FOXAPI_ITEM_AGC) != 0u;
        if (s.name == "LNA") { c.lnaAgc = s.agc; }
        if (s.name == "MIXER") { c.mixerAgc = s.agc; }
        c.stages.push_back(std::move(s));
    }
    return c;
}

bool AppWindow::drawAirspyControls() {
    // THE LISTS, once, for the whole panel - from the window's engine API
    // session, at most one frame old: the same bound every other gain control
    // in this section already accepts.
    const AirspyListCache& a = airspyLists();
    if (!a.open) { return false; }

    // --- DECIMATION ------------------------------------------------------
    const unsigned current = a.decimation;
    const auto decimationText = [](unsigned d) {
        return d == 1 ? std::string(tr("None")) : std::to_string(d);
    };
    ImGui::SetNextItemWidth(120.0f);
    const std::string preview = decimationText(current);
    if (ImGui::BeginCombo(labelAboveIfNeeded(trId("Decimation")), preview.c_str())) {
        for (const unsigned d : a.choices) {
            const bool sel = d == current;
            if (ImGui::Selectable(decimationText(d).c_str(), sel) && !sel) {
                uiSubmit(cascade::core::cmd::makeInt(FOXAPP_OP_AIRSPY_DECIMATION, static_cast<std::int64_t>(d)));
            }
            if (sel) { ImGui::SetItemDefaultFocus(); }
        }
        ImGui::EndCombo();
    }
    tooltipIfHovered(
        tr("Divides the sample rate by a power of two before anything else sees it: a narrower "
           "span, less work for the computer, and less noise in each sample - about 3 dB for "
           "every halving. The same as decimation in Airspy's own software."));
    if (a.decimation > 1) {
        std::string line;
        cascade::core::formatUtf8(line, tr("radio at %.4g MS/s, decimated by %u"),
                                  a.hardwareRateHz / 1.0e6, a.decimation);
        ImGui::TextDisabled("%s", line.c_str());
    }

    // --- THE GAIN MODE -------------------------------------------------------
    // One of three, in the reference application's order, and only the
    // chosen mode's controls below it.
    ImGui::TextUnformatted(tr("Gain mode"));
    const AirspyMode mode = static_cast<AirspyMode>(a.gainMode);
    struct ModeButton {
        AirspyMode mode;
        const char* label;
        const char* tip;
    };
    const ModeButton buttons[] = {
        {AirspyMode::Sensitivity, trId("Sensitive"),
         tr("One gain slider over Airspy's sensitivity table: the most gain early in the "
            "chain, for weak signals.")},
        {AirspyMode::Linearity, trId("Linear"),
         tr("One gain slider over Airspy's linearity table: gain held back early in the chain, "
            "for when strong signals are nearby.")},
        {AirspyMode::Free, trId("Free"),
         tr("Set the LNA, mixer and VGA yourself, with the LNA's and the mixer's own "
            "automatic gain control each switched on or off.")},
    };
    for (std::size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
        if (i > 0) { ImGui::SameLine(); }
        if (ImGui::RadioButton(buttons[i].label, mode == buttons[i].mode) &&
            mode != buttons[i].mode) {
            uiSubmit(cascade::core::cmd::makeInt(FOXAPP_OP_AIRSPY_GAIN_MODE,
                                                 static_cast<std::int64_t>(buttons[i].mode)));
        }
        tooltipIfHovered(buttons[i].tip);
    }

    // --- THAT MODE'S CONTROLS --------------------------------------------
    // A COPY is edited here (the same rule the generic sliders follow, see
    // drawSourceSection): the figure shown is always the radio's readback,
    // never left holding a request the driver quantised away from. The write
    // goes through the command path - FOXAPI_OP_SET_GAIN, the SAME op the
    // generic sliders submit, NOT FOXAPP_OP_SET_GAIN_NO_READBACK (that op is
    // the radar scope's GAIN knob, which keeps its own request on purpose -
    // engine/stage3b-pre M2, 2026-09-28: this slider used the knob's op by
    // mistake, so a request the R820T's discrete steps quantised away from
    // stuck on the slider instead of being corrected, and the Airspy gain
    // mirror/memory refresh FOXAPI_OP_SET_GAIN's handler does for an open
    // Airspy never ran) - queued, not applied at once: the figure is read
    // back on the NEXT frame the readback lands on, the same as every other
    // gain slider in the Source section.
    const auto slider = [&](std::size_t i, const char* label) {
        if (i >= a.stages.size()) { return; }
        float db = a.stages[i].db;
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SliderFloat(labelAboveIfNeeded(label), &db, 0.0f, a.stages[i].maxDb,
                               gainSliderFormat(cascade::source::GainUnit::Steps))) {
            uiSubmit(cascade::core::cmd::makeText(FOXAPI_OP_SET_GAIN, a.stages[i].name, 0, 0,
                                                  static_cast<double>(db)));
        }
        ImGui::PopID();
    };
    if (mode != AirspyMode::Free) {
        // ONE slider: the table's index, 0 (quietest) to 21.
        slider(0, trId("Gain"));
        return true;
    }
    // Free: the three stages by hand, the first two each with its own AGC
    // switch, and a stage its AGC is driving greyed rather than hidden - its
    // number is what it goes back to when the AGC is switched off.
    bool lnaAgc = a.lnaAgc;
    if (ImGui::Checkbox(trId("LNA AGC"), &lnaAgc)) {
        uiSubmit(cascade::core::cmd::makeInt(FOXAPP_OP_AIRSPY_AGC, 0, lnaAgc ? 1 : 0));
    }
    ImGui::SameLine();
    bool mixerAgc = a.mixerAgc;
    if (ImGui::Checkbox(trId("Mixer AGC"), &mixerAgc)) {
        uiSubmit(cascade::core::cmd::makeInt(FOXAPP_OP_AIRSPY_AGC, 1, mixerAgc ? 1 : 0));
    }
    for (std::size_t i = 0; i < a.stages.size(); ++i) {
        ImGui::BeginDisabled(a.stages[i].agc);
        slider(i, a.stages[i].name.c_str());
        ImGui::EndDisabled();
    }
    return true;
}

}  // namespace cascade::gui
