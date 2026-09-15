// The TRANSMIT page's arithmetic. See transmit_page.hpp for what is here and
// why none of it is in app_window.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/transmit_page.hpp"

#include <cmath>
#include <cstdio>

namespace cascade::gui {

void formatTxPower(char* out, std::size_t cap, double db, double quietDb) {
    if (out == nullptr || cap == 0) { return; }
    // A NUMBER THAT IS NOT ONE READS AS QUIET, not as "nan dB". The panel's
    // job here is to say how loud the radio is; the only honest answer for a
    // value nobody can interpret is the one that is also the safe one.
    if (!(db == db)) {
        std::snprintf(out, cap, "QUIET");
        return;
    }
    // Within a quarter of a decibel of the bottom of the board's own span -
    // one step of an AD9361's attenuator - is the bottom.
    if (db <= quietDb + 0.25) {
        std::snprintf(out, cap, "QUIET");
        return;
    }
    std::snprintf(out, cap, "%.1f dB", db);
}

float txMeterFraction(float peak01, double floorDb) {
    if (!(peak01 > 0.0f)) { return 0.0f; }
    if (!(floorDb < 0.0)) { return peak01 > 1.0f ? 1.0f : peak01; }
    double p = static_cast<double>(peak01);
    if (p > 1.0) { p = 1.0; }
    const double db = 20.0 * std::log10(p);
    if (db <= floorDb) { return 0.0f; }
    return static_cast<float>(1.0 - db / floorDb);
}

}  // namespace cascade::gui
