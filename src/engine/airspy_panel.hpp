// airspy_panel.hpp - the rules behind the Source section's Airspy R2 / Mini
// controls (gain mode, Free mode's two AGCs, decimation) and the per-radio
// memory of them, with no ImGui in sight so they can be tested. The drawing is
// gui/app_window_airspy.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdio>
#include <map>
#include <string>

#include "core/airspy_settings.hpp"
#include "source/airspy_source.hpp"
#include "source/device_source.hpp"

namespace cascade::gui {

// The open radio as an Airspy R2 / Mini, or null.
inline cascade::source::AirspySource* asAirspy(cascade::source::DeviceSource* dev) {
    return dynamic_cast<cascade::source::AirspySource*>(dev);
}

// A Rate combo row for an Airspy's DELIVERED rate. Whole megasamples keep the
// "2.500 MS/s" every radio's combo uses; below one, where decimation takes an
// R2 or a Mini (625000, 156250, 78125, 46875 Hz), thousands with three
// decimals, so 312.5 and 156.25 kS/s are not rounded into each other's
// neighbours the way "0.312 MS/s" would be.
inline std::string airspyRateLabel(double hz) {
    char buf[32];
    if (hz >= 1.0e6) {
        std::snprintf(buf, sizeof(buf), "%.3f MS/s", hz / 1.0e6);
    } else {
        std::snprintf(buf, sizeof(buf), "%.3f kS/s", hz / 1.0e3);
    }
    return buf;
}

// The mode's word in the config file.
inline const char* airspyModeKey(cascade::source::AirspySource::GainMode m) {
    using M = cascade::source::AirspySource::GainMode;
    switch (m) {
        case M::Linearity: return "linear";
        case M::Sensitivity: return "sensitive";
        case M::Free:
        default: return "free";
    }
}

inline cascade::source::AirspySource::GainMode airspyModeFromKey(const std::string& k) {
    using M = cascade::source::AirspySource::GainMode;
    if (k == "linear") { return M::Linearity; }
    if (k == "sensitive") { return M::Sensitivity; }
    return M::Free;
}

// What the radio is set to now, as the memory stores it.
inline cascade::core::AirspySetting airspySettingOf(const cascade::source::AirspySource& a) {
    const cascade::source::AirspySource::GainState st = a.gainState();
    cascade::core::AirspySetting s;
    s.mode = airspyModeKey(st.mode);
    s.linearity = st.linearity;
    s.sensitivity = st.sensitivity;
    s.lna = st.lna;
    s.mixer = st.mixer;
    s.vga = st.vga;
    s.lnaAgc = st.lnaAgc;
    s.mixerAgc = st.mixerAgc;
    s.decimation = a.decimation();
    return s;
}

// WRITES THE RADIO'S CURRENT STATE INTO THE MEMORY under its own key. Every
// radio, serial or not: unlike the bias tee there is nothing here that can
// hurt the next radio in the same socket - a gain and a decimation are what
// the user chose for "the Airspy in that socket", and they are cheap to change.
// A NEW radio past the cap is not remembered; one already known always is.
inline void airspyRemember(std::map<std::string, cascade::core::AirspySetting>& memory,
                           const std::string& args, const cascade::source::AirspySource& a) {
    const std::string key = cascade::core::airspyRadioKey(args);
    if (memory.count(key) == 0 && memory.size() >= cascade::core::kMaxAirspyRadios) { return; }
    memory[key] = airspySettingOf(a);
}

// PUTS A REMEMBERED RADIO BACK, after its open and before the application
// reads the rate list: the decimation first (it changes what the rate list
// and the readback say), then every gain value and the mode, programmed once.
// Nothing remembered for this radio leaves it exactly as its driver opened it.
// Returns whether anything was applied; a refusal (a decimation this board
// cannot do) is skipped rather than fatal, and the rest still goes on.
inline void airspyApplySetting(const cascade::core::AirspySetting& remembered,
                               cascade::source::AirspySource& a) {
    const cascade::core::AirspySetting s = cascade::core::sanitiseAirspySetting(remembered);
    if (s.decimation != a.decimation()) { a.setDecimation(s.decimation); }
    cascade::source::AirspySource::GainState st;
    st.mode = airspyModeFromKey(s.mode);
    st.linearity = s.linearity;
    st.sensitivity = s.sensitivity;
    st.lna = s.lna;
    st.mixer = s.mixer;
    st.vga = s.vga;
    st.lnaAgc = s.lnaAgc;
    st.mixerAgc = s.mixerAgc;
    a.setGainState(st);
}

inline bool airspyApplyRemembered(
    const std::map<std::string, cascade::core::AirspySetting>& memory, const std::string& args,
    cascade::source::AirspySource& a) {
    const auto it = memory.find(cascade::core::airspyRadioKey(args));
    if (it == memory.end()) { return false; }
    airspyApplySetting(it->second, a);
    return true;
}

}  // namespace cascade::gui
