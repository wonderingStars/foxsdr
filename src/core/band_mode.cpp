// band_mode.cpp - the band-plan mode convention (see band_mode.hpp for the
// whole design and both of its deliberate limits).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/band_mode.hpp"

#include <cmath>
#include <cstring>

namespace cascade::core {

const char* conventionalModeForBand(const std::string& service, double hz) {
    if (!std::isfinite(hz) || hz <= 0.0) { return ""; }
    if (service == "amateur") {
        if (hz < 10.0e6) {
            // The 60 m allocation is channelised USB by its licensing
            // conditions everywhere it exists; every other phone convention
            // below 10 MHz is LSB (160/80/40 m).
            if (hz >= 5.25e6 && hz <= 5.45e6) { return "USB"; }
            return "LSB";
        }
        if (hz < 30.0e6) { return "USB"; }
        return "NFM";
    }
    if (service == "broadcast") {
        if (hz <= 30.0e6) { return "AM"; }
        // One span for the OIRT (65.8-74), Japanese (76-95) and CCIR
        // (87.5-108) FM bands; what broadcasts above 108 MHz (DAB, DRM+) is
        // not a demodulator this receiver has, so no suggestion there.
        if (hz >= 64.0e6 && hz <= 108.0e6) { return "WFM"; }
        return "";
    }
    if (service == "aviation") {
        // HF air-route voice is USB; VHF airband and UHF military voice are
        // both AM, and the navaids inside the VHF band (VOR, ILS) are AM too.
        return hz < 30.0e6 ? "USB" : "AM";
    }
    if (service == "marine") {
        // HF working channels are USB; the VHF marine channels are NFM.
        return hz < 30.0e6 ? "USB" : "NFM";
    }
    if (service == "iss") {
        // Voice, APRS and SSTV downlinks are all FM.
        return "NFM";
    }
    // satellite (SSB transponders and FM birds both exist), mobile (analogue
    // PMR beside digital trunking), other, and anything unrecognised: no
    // convention confident enough to move the user's mode.
    return "";
}

bool modeFollowsTypedBandChange(const BandEntry* from, const BandEntry* to,
                                const char* currentMode, const char* suggested) {
    if (to == nullptr) { return false; }               // typed into a gap
    if (suggested == nullptr || suggested[0] == '\0') { return false; }
    if (currentMode == nullptr) { return false; }
    if (std::strcmp(currentMode, "RAW") == 0) { return false; }  // diagnostics hold
    if (from == to) { return false; }                  // same band: the mode is the user's
    return std::strcmp(currentMode, suggested) != 0;
}

}  // namespace cascade::core
