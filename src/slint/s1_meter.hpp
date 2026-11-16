// s1_meter.hpp - how the SIGNAL page's signal strength reads in each of the three units the engine keeps (FOXAPI_OP_SET_METER_UNITS): the level in dB of full scale (the reading as it always was), S units,
// or dBm. Header-only and free of everything else, so the Radio section's model (signal_logic.cpp) and screen lane S1's controls (s1_logic.cpp) quote one place.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "foxsdr_api.h"

namespace foxsdr::slints1 {

// "S7", "S9", "S9+20 dB" for the engine's sUnits (S9 = 9.0, each 6 dB is one unit below S9, S9+20 dB = 9.333: FoxReceiverState::sUnits); "S0" under S1 and for a figure that is not a number.
inline std::string sUnitsText(double sUnits) {
    if (!std::isfinite(sUnits)) { return "S0"; }
    if (sUnits <= 9.0) {
        const int whole = static_cast<int>(std::floor(sUnits + 1e-9));
        return "S" + std::to_string(std::max(0, whole));
    }
    const int db = static_cast<int>(std::lround((sUnits - 9.0) * 60.0));   // S9 + n dB is 9 + n / 60: the engine's own scale
    return db <= 0 ? std::string("S9") : "S9+" + std::to_string(db) + " dB";
}

// The reading of a radio's meter in the unit it chose (FOXAPI_METER_*): "-62.4 dB", "S7", "-87 dBm". An S or dBm figure with no calibration behind it is an estimate and carries a leading "~".
inline std::string meterReading(unsigned units, double signalDb, double signalDbm, double sUnits, bool calibrated) {
    char b[64];
    if (units == FOXAPI_METER_S) { return (calibrated ? std::string() : std::string("~")) + sUnitsText(sUnits); }
    if (units == FOXAPI_METER_DBM) {
        std::snprintf(b, sizeof(b), "%.0f dBm", signalDbm);
        return (calibrated ? std::string() : std::string("~")) + b;
    }
    std::snprintf(b, sizeof(b), "%.1f dB", signalDb);
    return b;
}

}  // namespace foxsdr::slints1
