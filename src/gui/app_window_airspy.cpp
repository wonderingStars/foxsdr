// app_window_airspy.cpp - the Source section's Airspy R2 / Mini controls
// (0.99.41): the gain mode, only that mode's sliders, Free mode's two AGC
// switches, and the software decimation - laid out the way the reference
// Airspy application offers them ("Gain: Sensitive/Linear/Free" and
// "Decimation: none, 2, 4, 8, 16, 32 and 64", SDRsharp - The Guide v2.1).
//
// The rules are engine/airspy_panel.hpp's and the driver's; this file only
// DRAWS. The choosing (chooseAirspyDecimation/GainMode/Agc) and the state it
// reads (deviceGainNames_ etc.) moved to the Engine in the engine extraction
// merge (2026-09-28, docs/engine-merge-0.99.42.md) - they are receiver state,
// like every other gain/AGC/rate mirror. This file reads engine_ as a friend
// and calls the choose* methods directly (kControlMayCall, the same reviewed
// pattern as scanSoundCards) so tests/test_airspy_app.cpp still drives the
// same code the buttons do, now through engine_.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

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

bool AppWindow::drawAirspyControls() {
    cascade::source::AirspySource* a = engine_.asAirspyDevice();
    if (a == nullptr) { return false; }

    // --- DECIMATION ------------------------------------------------------
    const unsigned current = a->decimation();
    const auto decimationText = [](unsigned d) {
        return d == 1 ? std::string(tr("None")) : std::to_string(d);
    };
    ImGui::SetNextItemWidth(120.0f);
    const std::string preview = decimationText(current);
    if (ImGui::BeginCombo(labelAboveIfNeeded(trId("Decimation")), preview.c_str())) {
        for (const unsigned d : a->decimationChoices()) {
            const bool sel = d == current;
            if (ImGui::Selectable(decimationText(d).c_str(), sel) && !sel) {
                engine_.chooseAirspyDecimation(d);
            }
            if (sel) { ImGui::SetItemDefaultFocus(); }
        }
        ImGui::EndCombo();
    }
    tooltipIfHovered(
        tr("Divides the sample rate by a power of two before anything else sees it: a narrower "
           "span, less work for the computer, and less noise in each sample - about 3 dB for "
           "every halving. The same as decimation in Airspy's own software."));
    if (a->decimation() > 1) {
        std::string line;
        cascade::core::formatUtf8(line, tr("radio at %.4g MS/s, decimated by %u"),
                                  a->hardwareSampleRateHz() / 1.0e6, a->decimation());
        ImGui::TextDisabled("%s", line.c_str());
    }

    // --- THE GAIN MODE -------------------------------------------------------
    // One of three, in the reference application's order, and only the
    // chosen mode's controls below it.
    ImGui::TextUnformatted(tr("Gain mode"));
    const AirspyMode mode = a->gainMode();
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
            engine_.chooseAirspyGainMode(buttons[i].mode);
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
        if (i >= engine_.deviceGainNames_.size() || i >= engine_.deviceGainsDb_.size()) { return; }
        const float hi = i < engine_.deviceGainRanges_.size()
                             ? static_cast<float>(engine_.deviceGainRanges_[i].maxDb)
                             : 15.0f;
        float db = engine_.deviceGainsDb_[i];
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SliderFloat(labelAboveIfNeeded(label), &db, 0.0f, hi,
                               gainSliderFormat(cascade::source::GainUnit::Steps))) {
            engine_.submitCommand(cascade::core::cmd::makeText(FOXAPI_OP_SET_GAIN,
                                                       engine_.deviceGainNames_[i], 0, 0,
                                                       static_cast<double>(db)));
        }
        ImGui::PopID();
    };
    if (a->gainMode() != AirspyMode::Free) {
        // ONE slider: the table's index, 0 (quietest) to 21.
        slider(0, trId("Gain"));
        return true;
    }
    // Free: the three stages by hand, the first two each with its own AGC
    // switch, and a stage its AGC is driving greyed rather than hidden - its
    // number is what it goes back to when the AGC is switched off.
    bool lnaAgc = a->lnaAgc();
    if (ImGui::Checkbox(trId("LNA AGC"), &lnaAgc)) { engine_.chooseAirspyAgc(true, lnaAgc); }
    ImGui::SameLine();
    bool mixerAgc = a->mixerAgc();
    if (ImGui::Checkbox(trId("Mixer AGC"), &mixerAgc)) { engine_.chooseAirspyAgc(false, mixerAgc); }
    for (std::size_t i = 0; i < engine_.deviceGainNames_.size(); ++i) {
        const std::string& name = engine_.deviceGainNames_[i];
        const bool driven = (name == "LNA" && a->lnaAgc()) || (name == "MIXER" && a->mixerAgc());
        ImGui::BeginDisabled(driven);
        slider(i, name.c_str());
        ImGui::EndDisabled();
    }
    return true;
}

}  // namespace cascade::gui
