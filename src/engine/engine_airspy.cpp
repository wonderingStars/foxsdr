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

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/bias_tee_memory.hpp"
#include "core/diag_log.hpp"
#include "engine/airspy_panel.hpp"

namespace cascade::engine {

using AirspyMode = cascade::source::AirspySource::GainMode;

// The open radio AS an Airspy, or null for any other radio (or none open).
// A query, like currentAbsoluteHz/carriedAirCentre (kControlMayCall): the
// window's drawAirspyControls reads it once a frame to decide whether to
// draw its own panel in place of the generic one.
cascade::source::AirspySource* Engine::asAirspyDevice() const {
    return cascade::gui::asAirspy(rx().device_);
}

void Engine::refreshDeviceGainMirrors() {
    if (rx().device_ == nullptr) { return; }
    rx().deviceGainRanges_ = rx().device_->gains();
    rx().deviceGainNames_.clear();
    rx().deviceGainsDb_.clear();
    for (const cascade::source::GainInfo& g : rx().deviceGainRanges_) {
        rx().deviceGainNames_.push_back(g.name);
        rx().deviceGainsDb_.push_back(static_cast<float>(rx().device_->gainDb(g.name)));
    }
    rx().deviceAgc_ = rx().device_->autoGain();
}

void Engine::airspyRememberOpen() {
    if (cascade::source::AirspySource* a = cascade::gui::asAirspy(rx().device_)) {
        cascade::gui::airspyRemember(airspyMemory_, rx().deviceArgs_, *a);
    }
}

bool Engine::chooseAirspyDecimation(unsigned factor) {
    cascade::source::AirspySource* a = cascade::gui::asAirspy(rx().device_);
    if (a == nullptr) { return false; }
    if (!a->setDecimation(factor)) {
        rx().sourceError_ = a->lastError();
        return false;
    }
    // A divider on what the radio delivers: the Rate combo lists DELIVERED
    // rates, so it is rebuilt, and the whole chain - spectrum span, channel,
    // a recording's rate, every decoder - follows the new rate exactly as it
    // follows a rate change.
    rx().deviceRatesHz_ = a->supportedSampleRatesHz();
    rx().deviceRateLabels_.clear();
    for (const double r : rx().deviceRatesHz_) { rx().deviceRateLabels_.push_back(cascade::gui::airspyRateLabel(r)); }
    int best = 0;
    for (int i = 1; i < static_cast<int>(rx().deviceRatesHz_.size()); ++i) {
        if (std::fabs(rx().deviceRatesHz_[static_cast<std::size_t>(i)] - a->sampleRateHz()) <
            std::fabs(rx().deviceRatesHz_[static_cast<std::size_t>(best)] - a->sampleRateHz())) {
            best = i;
        }
    }
    rx().deviceRateIndex_ = best;
    followInputRate();
    airspyRememberOpen();
    return true;
}

bool Engine::chooseAirspyGainMode(AirspyMode mode) {
    cascade::source::AirspySource* a = cascade::gui::asAirspy(rx().device_);
    if (a == nullptr) { return false; }
    const bool ok = a->setGainMode(mode);
    if (ok) {
        airspyRememberOpen();
    } else {
        rx().sourceError_ = a->lastError();
    }
    refreshDeviceGainMirrors();
    return ok;
}

bool Engine::chooseAirspyAgc(bool lna, bool on) {
    cascade::source::AirspySource* a = cascade::gui::asAirspy(rx().device_);
    if (a == nullptr) { return false; }
    const bool ok = lna ? a->setLnaAgc(on) : a->setMixerAgc(on);
    if (ok) {
        airspyRememberOpen();
    } else {
        rx().sourceError_ = a->lastError();
    }
    refreshDeviceGainMirrors();
    return ok;
}

// ---- each radio's Auto gain switch and manual gain (0.99.73 / 0.99.75; master's AppWindow members, here because the radio is the engine's) ----

bool Engine::changeAutoGain(bool want) {
    if (rx().device_ == nullptr || !rx().deviceAgcSupported_) { return false; }
    const bool wasAgc = rx().deviceAgc_;
    if (!rx().device_->setAutoGain(want)) {
        rx().sourceError_ = rx().device_->lastError();
        return false;  // the device did not change mode: the box stays where it was
    }
    rx().deviceAgc_ = want;
    // An "off" is remembered as well as an "on": a radio set back to manual
    // gain stays on manual gain at its next open. The debounced save notices
    // through configsEqual (autoGainByRadio); this calls no save of its own.
    rememberAutoGain(*rx().device_, rx().deviceArgs_, want);
    if (want == wasAgc) { return true; }  // nothing changed mode: nothing for the gains to follow

    // THE GAINS FOLLOW THE SWITCH (0.99.74, found live on the B200 by the verifier of the top
    // bar's GAIN dial). Leaving automatic gain a radio may fall to the BOTTOM of every stage (a
    // B200 under SoapyUHD sat about 35 dB below the 76 dB the mirror, and so the dial, still
    // showed), and this window used to read nothing back for any radio but an Airspy: the dial
    // lied until the hand touched it. So:
    //   ON   the mirrors are read back, the dial shows what the radio reports - after keeping the
    //        numbers the user had set (manualGainsBeforeAgc_), because what a radio reports while
    //        it gains itself is not what they will want back;
    //   OFF  every stage is given its manual value again (the kept numbers, else the mirror's: a
    //        radio opened already on automatic gain has only what the open read), a step the radio
    //        turns down is dropped without a word as the dial's own steps are, and then every stage
    //        is read back, so the mirror is what the radio really holds.
    const bool airspy = cascade::gui::asAirspy(rx().device_) != nullptr;
    if (want) {
        manualGainsBeforeAgc_.clear();
        for (std::size_t i = 0; i < rx().deviceGainNames_.size() && i < rx().deviceGainsDb_.size(); ++i) {
            manualGainsBeforeAgc_.emplace_back(rx().deviceGainNames_[i], rx().deviceGainsDb_[i]);
        }
        refreshDeviceGainMirrors();
    } else {
        std::size_t restored = 0;
        for (std::size_t i = 0; i < rx().deviceGainNames_.size() && i < rx().deviceGainsDb_.size(); ++i) {
            float db = rx().deviceGainsDb_[i];
            for (const auto& kept : manualGainsBeforeAgc_) {
                if (kept.first == rx().deviceGainNames_[i]) {
                    db = kept.second;
                    break;
                }
            }
            if (rx().device_->setGainDb(rx().deviceGainNames_[i], static_cast<double>(db))) { ++restored; }
        }
        manualGainsBeforeAgc_.clear();
        refreshDeviceGainMirrors();
        if (restored > 0) { cascade::core::diagLogf("source: manual gain restored after auto gain off"); }
    }
    // The box is what the radio ACCEPTED, as it has always been for every radio but an Airspy (whose
    // two AGC switches the readback above is the authority on): refreshDeviceGainMirrors re-read
    // autoGain(), which a driver need not keep in step.
    if (!airspy) { rx().deviceAgc_ = want; }
    return true;
}

void Engine::rememberManualGain(std::size_t stage) {
    if (rx().device_ == nullptr || stage >= rx().deviceGainNames_.size()) { return; }
    // An Airspy's gains are part of its gain mode, remembered in airspyMemory_ by
    // airspyRememberOpen; a second record here would be a second witness that could disagree.
    if (cascade::gui::asAirspy(rx().device_) != nullptr) { return; }
    // A radio gaining itself reports ITS choice, not the user's: the user's numbers are the
    // ones kept from before the switch went on (manualGainsBeforeAgc_) and from the last
    // hand on a slider, which this memory already holds.
    if (rx().deviceAgc_) { return; }
    const std::string& name = rx().deviceGainNames_[stage];
    // THE READBACK, not the request: the radio quantises, and the number to put back at the
    // next open is the one the radio actually held.
    const double db = rx().device_->gainDb(name);
    if (!std::isfinite(db)) { return; }
    const std::string key = cascade::core::biasTeeRadioKey(rx().device_->driverKey(), rx().deviceArgs_);
    auto it = manualGainMemory_.find(key);
    if (it == manualGainMemory_.end()) {
        // A new radio past the cap is not remembered (the bias tee's rule); one already in
        // the map is always updated.
        if (manualGainMemory_.size() >= cascade::core::kBiasTeeMemoryCap) { return; }
        it = manualGainMemory_.emplace(key, std::vector<std::pair<std::string, double>>{}).first;
    }
    auto& stages = it->second;
    // Kept sorted by stage name, which is the order a saved file hands it back in.
    const auto at = std::lower_bound(
        stages.begin(), stages.end(), name,
        [](const std::pair<std::string, double>& p, const std::string& n) { return p.first < n; });
    if (at != stages.end() && at->first == name) {
        at->second = db;
        return;
    }
    if (stages.size() >= cascade::core::AppConfig::kMaxManualGainStages) { return; }
    stages.insert(at, std::make_pair(name, db));
}

void Engine::restoreManualGain(cascade::source::DeviceSource& dev, const std::string& args) {
    const auto mem = manualGainMemory_.find(cascade::core::biasTeeRadioKey(dev.driverKey(), args));
    if (mem == manualGainMemory_.end()) { return; }
    std::string restored;
    for (const auto& [stage, wantDb] : mem->second) {
        std::size_t i = 0;
        while (i < rx().deviceGainNames_.size() && rx().deviceGainNames_[i] != stage) { ++i; }
        if (i >= rx().deviceGainNames_.size()) { continue; }  // the radio no longer has it
        double db = wantDb;
        if (i < rx().deviceGainRanges_.size() && rx().deviceGainRanges_[i].maxDb > rx().deviceGainRanges_[i].minDb) {
            db = std::min(std::max(db, rx().deviceGainRanges_[i].minDb), rx().deviceGainRanges_[i].maxDb);
        }
        if (!dev.setGainDb(stage, db)) { continue; }
        const double took = dev.gainDb(stage);
        if (i < rx().deviceGainsDb_.size()) { rx().deviceGainsDb_[i] = static_cast<float>(took); }
        char one[96];
        std::snprintf(one, sizeof one, "%s%s %.1f", restored.empty() ? "" : ", ", stage.c_str(), took);
        restored += one;
    }
    if (!restored.empty()) {
        cascade::core::diagLogf("source: manual gain restored - %s", restored.c_str());
    }
}

void Engine::rememberAutoGain(cascade::source::DeviceSource& dev, const std::string& args, bool on) {
    // An Airspy's AGC switches are part of its gain mode, remembered in
    // airspyMemory_ by airspyRememberOpen; a second record here would be a
    // second witness that could disagree.
    if (cascade::gui::asAirspy(&dev) != nullptr) { return; }
    const std::string key = cascade::core::biasTeeRadioKey(dev.driverKey(), args);
    // A new radio past the cap is not remembered (the bias tee's rule); one
    // already in the map is always updated.
    if (autoGainMemory_.count(key) == 0 && autoGainMemory_.size() >= cascade::core::kBiasTeeMemoryCap) { return; }
    autoGainMemory_[key] = on;
}

}  // namespace cascade::engine
