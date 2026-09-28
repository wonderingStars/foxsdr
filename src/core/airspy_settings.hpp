// airspy_settings.hpp - what FoxSDR remembers about each Airspy R2 / Mini: the
// gain mode the user chose (Linear / Sensitive / Free, the reference Airspy
// application's three), each mode's own values, Free mode's two AGCs and the
// software decimation. Kept PER RADIO, like the bias tee and the converter,
// under the same key (core::biasTeeRadioKey - "airspy|serial=<serial>").
//
// Pure data and rules, no ImGui and no driver: core/config.cpp loads and saves
// it, gui/airspy_panel.hpp applies it to an open radio and writes it back.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>

#include "core/bias_tee_memory.hpp"

namespace cascade::core {

struct AirspySetting {
    // "linear", "sensitive" or "free" - the words the file carries, so a
    // hand-edited config reads the way the panel does.
    std::string mode = "free";
    int linearity = 10;    // 0..21, the linearity table's index
    int sensitivity = 10;  // 0..21, the sensitivity table's index
    int lna = 8;           // 0..14
    int mixer = 8;         // 0..15
    int vga = 8;           // 0..15
    bool lnaAgc = false;
    bool mixerAgc = false;
    unsigned decimation = 1;  // 1, 2, 4 ... 64

    bool operator==(const AirspySetting&) const = default;
};

// A hand-edited or hostile file cannot grow the config without bound.
inline constexpr std::size_t kMaxAirspyRadios = 64;

inline std::string airspyRadioKey(const std::string& args) {
    return biasTeeRadioKey("airspy", args);
}

// Every value into the range the driver accepts, an unknown mode to "free"
// and a decimation that is not a power of two up to 64 to none. The driver
// would clamp the gains itself; doing it here keeps what is SAVED equal to
// what was applied.
inline AirspySetting sanitiseAirspySetting(AirspySetting s) {
    if (s.mode != "linear" && s.mode != "sensitive" && s.mode != "free") { s.mode = "free"; }
    s.linearity = std::clamp(s.linearity, 0, 21);
    s.sensitivity = std::clamp(s.sensitivity, 0, 21);
    s.lna = std::clamp(s.lna, 0, 14);
    s.mixer = std::clamp(s.mixer, 0, 15);
    s.vga = std::clamp(s.vga, 0, 15);
    const unsigned d = s.decimation;
    if (d == 0 || d > 64 || (d & (d - 1)) != 0) { s.decimation = 1; }
    return s;
}

// The map as loaded: every entry sanitised, entries with no "airspy|" radio
// key dropped, and at most kMaxAirspyRadios kept.
inline std::map<std::string, AirspySetting> sanitiseAirspySettings(
    const std::map<std::string, AirspySetting>& in) {
    std::map<std::string, AirspySetting> out;
    for (const auto& [key, s] : in) {
        if (key.rfind("airspy|", 0) != 0 || key.size() <= 7) { continue; }
        if (out.size() >= kMaxAirspyRadios) { break; }
        out[key] = sanitiseAirspySetting(s);
    }
    return out;
}

}  // namespace cascade::core
