// test_source_row_status.cpp - gui/source_row_status.hpp: when the Source row
// says "No radio hardware found", and what it says while a running patch has
// the radio (0.99.49 beta feedback - a NESDR SMArt v5 streaming on the patch
// page while the receiver said it had no radio). tests/test_source_row_app.cpp
// proves the real AppWindow asks these.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>
#include <vector>

#include "gui/source_row_status.hpp"
#include "test_check.hpp"

using cascade::gui::PatchHeldRadio;
using cascade::source::NativeDeviceInfo;

namespace {

NativeDeviceInfo row(const char* driver, const char* label) {
    NativeDeviceInfo d;
    d.driver = driver;
    d.label = label;
    return d;
}

}  // namespace

int main() {
    std::printf("test_source_row_status\n");
    using cascade::gui::nativeRadiosFound;
    using cascade::gui::noRadioHardwareFound;
    using cascade::gui::patchHardwareNames;
    using cascade::gui::sourceChipWithPatch;
    using cascade::gui::sourceRowText;

    // --- the Pluto row is not a radio found ----------------------------------
    CHECK(nativeRadiosFound({}, "pluto") == 0u);
    CHECK(nativeRadiosFound({row("pluto", "ADALM-Pluto (network)")}, "pluto") == 0u);
    CHECK(nativeRadiosFound({row("rtlsdr", "NESDR SMArt v5"), row("pluto", "ADALM-Pluto")},
                            "pluto") == 1u);
    CHECK(nativeRadiosFound({row("rtlsdr", "a"), row("hackrf", "b")}, "pluto") == 2u);
    // ...and neither is the rtl_tcp server's (0.99.70), when its key is passed
    // as the second address row. Passing only the Pluto's key still counts it,
    // which is what the default of "no second row" has always meant.
    CHECK(nativeRadiosFound({row("pluto", "ADALM-Pluto"), row("rtltcp", "rtl_tcp server")}, "pluto",
                            "rtltcp") == 0u);
    CHECK(nativeRadiosFound({row("rtlsdr", "a"), row("pluto", "p"), row("rtltcp", "r")}, "pluto",
                            "rtltcp") == 1u);
    CHECK(nativeRadiosFound({row("rtltcp", "rtl_tcp server")}, "pluto") == 1u);

    // --- "No radio hardware found" -------------------------------------------
    // The tester's case: SoapySDR found nothing, the native driver found one.
    CHECK(!noRadioHardwareFound(true, false, false, 0, 1));
    // SoapySDR found one and the native drivers nothing: not shown (as before).
    CHECK(!noRadioHardwareFound(true, false, false, 1, 0));
    // Neither found anything, after a whole scan: shown.
    CHECK(noRadioHardwareFound(true, false, false, 0, 0));
    // Never before the first scan, during one, or after a partial one.
    CHECK(!noRadioHardwareFound(false, false, false, 0, 0));
    CHECK(!noRadioHardwareFound(true, true, false, 0, 0));
    CHECK(!noRadioHardwareFound(true, false, true, 0, 0));

    // --- the radios the patch holds ------------------------------------------
    const std::vector<PatchHeldRadio> held = {
        {"siggen", "Signal generator"},
        {"iqfile|path=/tmp/a.wav", "Recording: a.wav"},
        {"rtlsdr|serial=00000001", "NESDR SMArt v5 (serial 00000001)"},
        {"soapy|driver=airspy", "Airspy R2"},
        {"", "nothing chosen"},
    };
    // Not running: none, whatever is in the map.
    CHECK(patchHardwareNames(false, held).empty());
    const std::vector<std::string> names = patchHardwareNames(true, held);
    CHECK(names.size() == 2u);
    CHECK(names.size() == 2u && names[0] == "NESDR SMArt v5");  // serial left off
    CHECK(names.size() == 2u && names[1] == "Airspy R2");
    // A patch on the generator and a recording only holds no radio.
    CHECK(patchHardwareNames(true, {{"siggen", "Signal generator"}}).empty());

    // --- what the row says ---------------------------------------------------
    CHECK(sourceRowText("Signal generator", {}) == "Signal generator");
    CHECK(sourceRowText("Signal generator", {"NESDR SMArt v5"}) ==
          "Signal generator - the patch has NESDR SMArt v5");
    CHECK(sourceRowText("Signal generator", {"NESDR SMArt v5", "Airspy R2"}) ==
          "Signal generator - the patch has NESDR SMArt v5, Airspy R2");
    CHECK(sourceChipWithPatch("Signal gen", {}) == "Signal gen");
    CHECK(sourceChipWithPatch("Signal gen", {"NESDR SMArt v5"}) == "ON PATCH");

    return testSummary("test_source_row_status");
}
