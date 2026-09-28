// engine_airspy.cpp - the Engine's machinery from gui/app_window_airspy.cpp
// (0.99.41, merged into the engine extraction 2026-09-28): the gain mode,
// gains, AGC switches and software decimation for an Airspy R2 / Mini. The
// drawing (drawAirspyControls) stays in gui/app_window_airspy.cpp, which
// calls these as reviewed direct calls (kControlMayCall, the same pattern as
// scanSoundCards) rather than through a command - tests/test_airspy_app.cpp
// drives the same code the buttons do.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine.hpp"

#include <cmath>

#include "gui/airspy_panel.hpp"

namespace cascade::engine {

using AirspyMode = cascade::source::AirspySource::GainMode;

void Engine::refreshDeviceGainMirrors() {
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

void Engine::airspyRememberOpen() {
    if (cascade::source::AirspySource* a = cascade::gui::asAirspy(device_)) {
        cascade::gui::airspyRemember(airspyMemory_, deviceArgs_, *a);
    }
}

bool Engine::chooseAirspyDecimation(unsigned factor) {
    cascade::source::AirspySource* a = cascade::gui::asAirspy(device_);
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
    for (const double r : deviceRatesHz_) { deviceRateLabels_.push_back(cascade::gui::airspyRateLabel(r)); }
    int best = 0;
    for (int i = 1; i < static_cast<int>(deviceRatesHz_.size()); ++i) {
        if (std::fabs(deviceRatesHz_[static_cast<std::size_t>(i)] - a->sampleRateHz()) <
            std::fabs(deviceRatesHz_[static_cast<std::size_t>(best)] - a->sampleRateHz())) {
            best = i;
        }
    }
    deviceRateIndex_ = best;
    followInputRate();
    airspyRememberOpen();
    return true;
}

bool Engine::chooseAirspyGainMode(AirspyMode mode) {
    cascade::source::AirspySource* a = cascade::gui::asAirspy(device_);
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

bool Engine::chooseAirspyAgc(bool lna, bool on) {
    cascade::source::AirspySource* a = cascade::gui::asAirspy(device_);
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

}  // namespace cascade::engine
