// app_window_airspy.cpp - the Source section's Airspy R2 / Mini controls
// (0.99.40): the gain mode, only that mode's sliders, Free mode's two AGC
// switches, and the software decimation - laid out the way the reference
// Airspy application offers them ("Gain: Sensitive/Linear/Free" and
// "Decimation: none, 2, 4, 8, 16, 32 and 64", SDRsharp - The Guide v2.1).
//
// The rules are gui/airspy_panel.hpp's and the driver's; this file draws, and
// the choose* members hand the user's choice to them and re-read what the
// radio took (members so tests/test_airspy_app.cpp drives the same code).
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
#include "gui/airspy_panel.hpp"
#include "gui/text_fit.hpp"
#include "gui/tune_control.hpp"

namespace cascade::gui {
using cascade::i18n::tr;
using cascade::i18n::trId;
using AirspyMode = cascade::source::AirspySource::GainMode;

namespace {

// The nearest entry, as the Rate combo points at a readback.
int nearestRateIndex(const std::vector<double>& v, double x) {
    int best = 0;
    for (int i = 1; i < static_cast<int>(v.size()); ++i) {
        if (std::fabs(v[static_cast<std::size_t>(i)] - x) <
            std::fabs(v[static_cast<std::size_t>(best)] - x)) {
            best = i;
        }
    }
    return best;
}

void tooltipIfHovered(const char* text) {
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", text); }
}

}  // namespace

void AppWindow::refreshDeviceGainMirrors() {
    if (device_ == nullptr) { return; }
    deviceGainRanges_ = device_->gains();
    deviceGainNames_.clear();
    deviceGainsDb_.clear();
    for (const cascade::source::GainInfo& g : deviceGainRanges_) {
        deviceGainNames_.push_back(g.name);
        deviceGainsDb_.push_back(static_cast<float>(device_->gainDb(g.name)));
    }
    deviceAgc_ = device_->autoGain();
}

void AppWindow::airspyRememberOpen() {
    if (cascade::source::AirspySource* a = asAirspy(device_)) {
        airspyRemember(airspyMemory_, deviceArgs_, *a);
    }
}

bool AppWindow::chooseAirspyDecimation(unsigned factor) {
    cascade::source::AirspySource* a = asAirspy(device_);
    if (a == nullptr) { return false; }
    if (!a->setDecimation(factor)) {
        sourceError_ = a->lastError();
        return false;
    }
    // A divider on what the radio delivers: the Rate combo lists DELIVERED
    // rates, so it is rebuilt, and the whole chain - spectrum span, channel,
    // a recording's rate, every decoder - follows the new rate exactly as it
    // follows a rate change.
    deviceRatesHz_ = a->supportedSampleRatesHz();
    deviceRateLabels_.clear();
    for (const double r : deviceRatesHz_) { deviceRateLabels_.push_back(airspyRateLabel(r)); }
    deviceRateIndex_ = nearestRateIndex(deviceRatesHz_, a->sampleRateHz());
    followInputRate();
    airspyRememberOpen();
    return true;
}

bool AppWindow::chooseAirspyGainMode(AirspyMode mode) {
    cascade::source::AirspySource* a = asAirspy(device_);
    if (a == nullptr) { return false; }
    const bool ok = a->setGainMode(mode);
    if (ok) {
        airspyRememberOpen();
    } else {
        sourceError_ = a->lastError();
    }
    refreshDeviceGainMirrors();
    return ok;
}

bool AppWindow::chooseAirspyAgc(bool lna, bool on) {
    cascade::source::AirspySource* a = asAirspy(device_);
    if (a == nullptr) { return false; }
    const bool ok = lna ? a->setLnaAgc(on) : a->setMixerAgc(on);
    if (ok) {
        airspyRememberOpen();
    } else {
        sourceError_ = a->lastError();
    }
    refreshDeviceGainMirrors();
    return ok;
}

bool AppWindow::drawAirspyControls() {
    cascade::source::AirspySource* a = asAirspy(device_);
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
                chooseAirspyDecimation(d);
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
            chooseAirspyGainMode(buttons[i].mode);
        }
        tooltipIfHovered(buttons[i].tip);
    }

    // --- THAT MODE'S CONTROLS --------------------------------------------
    const auto slider = [&](std::size_t i, const char* label) {
        if (i >= deviceGainNames_.size() || i >= deviceGainsDb_.size()) { return; }
        const float hi = i < deviceGainRanges_.size()
                             ? static_cast<float>(deviceGainRanges_[i].maxDb)
                             : 15.0f;
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SliderFloat(labelAboveIfNeeded(label), &deviceGainsDb_[i], 0.0f, hi,
                               gainSliderFormat(cascade::source::GainUnit::Steps))) {
            if (a->setGainDb(deviceGainNames_[i], static_cast<double>(deviceGainsDb_[i]))) {
                deviceGainsDb_[i] = static_cast<float>(a->gainDb(deviceGainNames_[i]));
                airspyRememberOpen();
            } else {
                sourceError_ = a->lastError();
            }
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
    if (ImGui::Checkbox(trId("LNA AGC"), &lnaAgc)) { chooseAirspyAgc(true, lnaAgc); }
    ImGui::SameLine();
    bool mixerAgc = a->mixerAgc();
    if (ImGui::Checkbox(trId("Mixer AGC"), &mixerAgc)) { chooseAirspyAgc(false, mixerAgc); }
    for (std::size_t i = 0; i < deviceGainNames_.size(); ++i) {
        const std::string& name = deviceGainNames_[i];
        const bool driven = (name == "LNA" && a->lnaAgc()) || (name == "MIXER" && a->mixerAgc());
        ImGui::BeginDisabled(driven);
        slider(i, name.c_str());
        ImGui::EndDisabled();
    }
    return true;
}

}  // namespace cascade::gui
