// mapicons_logic.cpp - see mapicons_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "mapicons_logic.hpp"

#include <cstring>

#include "core/plugin_abi.h"

namespace foxsdr::slinthost {

namespace {
bool singleEngineHelicopter(const char* icaoType) {
    if (icaoType == nullptr || icaoType[0] == '\0') { return false; }
    static constexpr const char* kSingle[] = {"R22", "R44", "R66", "B06", "B407", "B505", "AS50", "EC20", "EC30", "H500", "H269", "EN28", "EN48", "G2CA", "A119", "B47G"};
    char norm[8] = {};
    std::size_t n = 0;
    for (const char* p = icaoType; *p != '\0'; ++p) {
        if (*p == ' ') { continue; }
        if (n + 1 >= sizeof(norm)) { return false; }
        char c = *p;
        if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 'a' + 'A'); }
        norm[n++] = c;
    }
    for (const char* t : kSingle) { if (std::strcmp(norm, t) == 0) { return true; } }
    return false;
}
}  // namespace

int aircraftIconIndex(std::uint32_t category, const char* icaoType) {
    int base = 9;
    switch (category) {
        case CASCADE_AIRCRAFT_LIGHT: base = 2; break;
        case CASCADE_AIRCRAFT_MEDIUM1:
        case CASCADE_AIRCRAFT_MEDIUM2:
        case CASCADE_AIRCRAFT_HIGH_VORTEX:
        case CASCADE_AIRCRAFT_HIGH_PERF:
        case CASCADE_AIRCRAFT_SPACE: base = 0; break;
        case CASCADE_AIRCRAFT_HEAVY: base = 1; break;
        case CASCADE_AIRCRAFT_ROTORCRAFT: base = 8; break;
        case CASCADE_AIRCRAFT_GLIDER: base = 3; break;
        case CASCADE_AIRCRAFT_LIGHTER_THAN_AIR:
        case CASCADE_AIRCRAFT_PARACHUTIST: base = 6; break;
        case CASCADE_AIRCRAFT_ULTRALIGHT: base = 4; break;
        case CASCADE_AIRCRAFT_UAV: base = 5; break;
        case CASCADE_AIRCRAFT_SURFACE_EMERGENCY:
        case CASCADE_AIRCRAFT_SURFACE_SERVICE:
        case CASCADE_AIRCRAFT_GROUND_OBSTRUCTION: base = 7; break;
        default: base = 9; break;
    }
    if (base == 8 && singleEngineHelicopter(icaoType)) { return 10; }
    return base;
}

}  // namespace foxsdr::slinthost
