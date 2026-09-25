// bias_tee_memory.hpp - WHICH RADIO a remembered bias tee belongs to.
//
// Since the deck's BIAS TEE key (2026-09-25, repair round 1) the bias tee is
// remembered PER RADIO for every driver family, in AppConfig::biasTee: a map
// from this key to "on" or "off". It replaced two memories that were each
// wrong in their own way - nativeBiasT, ONE bool that every HackRF, Airspy,
// Airspy HF+, SDRplay, Mirics and RX888 opened with (so an "on" confirmed
// for a mast-head amplifier on one radio put power on the next radio of any
// of those families, silently, at its open), and rtlBiasTArgs / rtlBiasT,
// which remembered exactly one RTL-SDR (so turning a second dongle off
// forgot the first dongle's "on").
//
// THE KEY IS THE DRIVER AND THE SERIAL: "hackrf|serial=0000...457863c8". The
// serial is what names a radio rather than a place in a USB walk ("index=0"
// is whichever radio the walk finds first, a different one the day another
// is plugged in), which is the same argument gui::rtlArgsNameADongle makes.
// A radio opened with no serial gets "<kind>|<args>" - but only an OFF is ever
// kept under such a key (gui::biasTeeRemember): an ON is never remembered for
// a radio that cannot be told apart from the next one in the same socket.
//
// Here, with no ImGui, because core/config.cpp needs it to migrate the old
// RTL-SDR memory and gui/bias_tee.hpp needs it for everything else.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>

#include "source/device_source.hpp"

namespace cascade::core {

// Whether these args name ONE radio (by its serial).
//
// A SERIAL THAT IS ALL ZEROS NAMES NOTHING (review round 2, 2026-09-25): it is
// what a board with no serial programmed reports, and it is the serial EVERY
// RX888 bootloader reports ("0000000000000000"), so two such radios cannot be
// told apart by it - exactly the position-not-radio case "index=0" is. An
// empty serial is the same. "00000001" is NOT treated this way: it is the
// factory serial of many RTL-SDRs, and the shared-serial residual it carries
// is documented beside gui::rtlBiasTeeAtOpen rather than taken from a single
// Blog V4 owner whose restore depends on it.
inline bool biasTeeArgsNameARadio(const std::string& args) {
    const std::string serial = cascade::source::argValue(args, "serial");
    return serial.find_first_not_of('0') != std::string::npos;
}

// The memory key for the radio `kind` (the driver key, "rtlsdr", "hackrf"...)
// opened with `args`.
inline std::string biasTeeRadioKey(const std::string& kind, const std::string& args) {
    const std::string serial = cascade::source::argValue(args, "serial");
    if (!serial.empty()) { return kind + "|serial=" + serial; }
    return kind + "|" + args;
}

// How many radios the memory keeps at most: a file edited by hand (or by
// something hostile) cannot grow the config without bound. Far more radios
// than any bench has.
inline constexpr std::size_t kBiasTeeMemoryCap = 64;

}  // namespace cascade::core
