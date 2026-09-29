// source_row_status.hpp - what the receiver's Source row says about radios
// (0.99.49 beta feedback).
//
// THE REPORT. A tester on a small laptop with a NooElec NESDR SMArt v5 had a
// patch running that radio perfectly, while the Function Select panel said
// "No radio hardware found" and the Source row's chip said "Signal gen". Two
// separate faults, both reading as "FoxSDR cannot see my radio":
//
//   - The "No radio hardware found" block was gated on the SoapySDR list
//     alone. The native drivers (RTL-SDR, HackRF, Airspy, ...) fill a second
//     list, and the dongle was right there in the combo above the warning as
//     "NESDR SMArt v5 (native)" - but SoapySDR had no module for it, so the
//     warning appeared anyway.
//   - While a patch runs it takes the radios, and the receiver is moved to the
//     signal generator (app_window_patch_radios.cpp, the owner's fourth rule).
//     The Source row then named only the generator, with nothing to say where
//     the radio had gone.
//
// Both rules are here, pure, so tests/test_source_row_status.cpp can hold them
// without a window or a radio, and tests/test_source_row_app.cpp proves the
// real AppWindow asks them.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/i18n.hpp"
#include "core/patch_devices.hpp"
#include "core/utf8_text.hpp"
#include "source/device_source.hpp"

namespace cascade::gui {

// --- 1. "No radio hardware found" ------------------------------------------
//
// The radios the native drivers found. The ADALM-Pluto row is NOT one of them:
// it is always in the list (a network cannot be walked, so it is a row that
// asks for an address - see AppWindow::scanNative) and says nothing about what
// is plugged in. `plutoDriver` is that row's driver key.
inline std::size_t nativeRadiosFound(const std::vector<cascade::source::NativeDeviceInfo>& native,
                                     const char* plutoDriver) {
    std::size_t n = 0;
    for (const cascade::source::NativeDeviceInfo& d : native) {
        if (plutoDriver == nullptr || d.driver != plutoDriver) { ++n; }
    }
    return n;
}

// Whether the "No radio hardware found" block is shown: only after a complete
// SoapySDR scan has finished (so it never flashes up during the first one),
// and only when NEITHER list has a radio in it. A dongle the native driver
// found is hardware found, whatever SoapySDR thinks.
inline bool noRadioHardwareFound(bool soapyScanned, bool soapyScanPartial, bool busy,
                                 std::size_t soapyRadios, std::size_t nativeRadios) {
    return soapyScanned && !soapyScanPartial && !busy && soapyRadios == 0 && nativeRadios == 0;
}

// --- 2. the radio the patch has ----------------------------------------------
//
// One radio a running patch holds open: the device key it was opened as
// (core/patch_devices.hpp) and the label it opened under.
struct PatchHeldRadio {
    std::string deviceKey;
    std::string label;
};

// The names of the HARDWARE radios a running patch holds, in order. The
// generator and a recording are not radios the receiver could have had, so
// they are left out; a label is cut before " (serial ...)", which identifies
// the radio to nobody reading a one-line row. Empty while the patch is not
// running.
inline std::vector<std::string> patchHardwareNames(bool patchRunning,
                                                   const std::vector<PatchHeldRadio>& held) {
    std::vector<std::string> out;
    if (!patchRunning) { return out; }
    for (const PatchHeldRadio& r : held) {
        if (r.deviceKey.empty() || cascade::core::patch::isGeneratorKey(r.deviceKey) ||
            cascade::core::patch::isIqFileKey(r.deviceKey)) {
            continue;
        }
        std::string name = r.label;
        if (const std::size_t at = name.find(" (serial "); at != std::string::npos) {
            name.resize(at);
        }
        if (name.empty()) { continue; }
        out.push_back(std::move(name));
    }
    return out;
}

// What the Source combo shows: the receiver's own source, and - when the patch
// has radios - which ones, so "Signal generator" is never the whole story
// while a radio is streaming on the patch page. "Signal generator - the patch
// has NESDR SMArt v5".
inline std::string sourceRowText(const std::string& receiverSource,
                                 const std::vector<std::string>& patchRadios) {
    if (patchRadios.empty()) { return receiverSource; }
    std::string names;
    for (const std::string& n : patchRadios) { names += (names.empty() ? "" : ", ") + n; }
    std::string out;
    cascade::core::formatUtf8(out, cascade::i18n::tr("%s - the patch has %s"),
                              receiverSource.c_str(), names.c_str());
    return out;
}

// The folded row's chip. The chip is ten characters wide, so it cannot carry
// the sentence above; it says the radio is on the patch rather than naming the
// generator alone, which is what read as "no radio".
inline std::string sourceChipWithPatch(const std::string& receiverChip,
                                       const std::vector<std::string>& patchRadios) {
    if (patchRadios.empty()) { return receiverChip; }
    return cascade::i18n::tr("ON PATCH");
}

}  // namespace cascade::gui
