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
// cascade::source::AirspySource* engine_.asAirspyDevice() used to hand out
// (a pointer into engine-owned, mutable object state - the one query of this
// shape the Engine ever returned) onto the PUBLISHED state
// (PublishedState::app's airspy* fields, receiver_snapshot.hpp) - the same
// snapshot every other reader of the receiver answers from, and the one 3b's
// control thread can keep publishing safely. tests/test_airspy_app.cpp is
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

bool AppWindow::drawAirspyControls() {
    // THE PUBLISHED STATE, ONCE, for the whole panel - never the raw device
    // pointer. A failed lock-free read (vanishingly rare - see
    // receiver_snapshot.hpp) falls back to the installed block, exactly as
    // catStatusNow() does; either way this is at most one frame old, the same
    // bound every other gain control in this section already accepts.
    cascade::core::PublishedState ps;
    if (!engine_.receiverSnapshot_->read(ps)) { ps = engine_.receiverSnapshot_->readFull()->state; }
    const cascade::core::AppStateExt& e = ps.app;
    if (!e.airspyOpen) { return false; }

    // --- DECIMATION ------------------------------------------------------
    const unsigned current = e.airspyDecimation;
    const auto decimationText = [](unsigned d) {
        return d == 1 ? std::string(tr("None")) : std::to_string(d);
    };
    ImGui::SetNextItemWidth(120.0f);
    const std::string preview = decimationText(current);
    if (ImGui::BeginCombo(labelAboveIfNeeded(trId("Decimation")), preview.c_str())) {
        for (std::uint32_t i = 0; i < e.airspyDecimationChoiceCount; ++i) {
            const unsigned d = e.airspyDecimationChoices[i];
            const bool sel = d == current;
            if (ImGui::Selectable(decimationText(d).c_str(), sel) && !sel) {
                engine_.submitCommand(cascade::core::cmd::makeInt(FOXAPP_OP_AIRSPY_DECIMATION,
                                                                  static_cast<std::int64_t>(d)));
            }
            if (sel) { ImGui::SetItemDefaultFocus(); }
        }
        ImGui::EndCombo();
    }
    tooltipIfHovered(
        tr("Divides the sample rate by a power of two before anything else sees it: a narrower "
           "span, less work for the computer, and less noise in each sample - about 3 dB for "
           "every halving. The same as decimation in Airspy's own software."));
    if (e.airspyDecimation > 1) {
        std::string line;
        cascade::core::formatUtf8(line, tr("radio at %.4g MS/s, decimated by %u"),
                                  e.airspyHardwareSampleRateHz / 1.0e6, e.airspyDecimation);
        ImGui::TextDisabled("%s", line.c_str());
    }

    // --- THE GAIN MODE -------------------------------------------------------
    // One of three, in the reference application's order, and only the
    // chosen mode's controls below it.
    ImGui::TextUnformatted(tr("Gain mode"));
    const AirspyMode mode = static_cast<AirspyMode>(e.airspyGainMode);
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
            engine_.submitCommand(cascade::core::cmd::makeInt(
                FOXAPP_OP_AIRSPY_GAIN_MODE, static_cast<std::int64_t>(buttons[i].mode)));
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
    if (mode != AirspyMode::Free) {
        // ONE slider: the table's index, 0 (quietest) to 21.
        slider(0, trId("Gain"));
        return true;
    }
    // Free: the three stages by hand, the first two each with its own AGC
    // switch, and a stage its AGC is driving greyed rather than hidden - its
    // number is what it goes back to when the AGC is switched off.
    bool lnaAgc = e.airspyLnaAgc;
    if (ImGui::Checkbox(trId("LNA AGC"), &lnaAgc)) {
        engine_.submitCommand(cascade::core::cmd::makeInt(FOXAPP_OP_AIRSPY_AGC, 0, lnaAgc ? 1 : 0));
    }
    ImGui::SameLine();
    bool mixerAgc = e.airspyMixerAgc;
    if (ImGui::Checkbox(trId("Mixer AGC"), &mixerAgc)) {
        engine_.submitCommand(cascade::core::cmd::makeInt(FOXAPP_OP_AIRSPY_AGC, 1, mixerAgc ? 1 : 0));
    }
    for (std::size_t i = 0; i < engine_.deviceGainNames_.size(); ++i) {
        const std::string& name = engine_.deviceGainNames_[i];
        const bool driven = (name == "LNA" && e.airspyLnaAgc) || (name == "MIXER" && e.airspyMixerAgc);
        ImGui::BeginDisabled(driven);
        slider(i, name.c_str());
        ImGui::EndDisabled();
    }
    return true;
}

}  // namespace cascade::gui
