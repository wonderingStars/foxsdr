// s5b_logic.hpp - SCREEN LANE S5b (wave 8; docs/multi-radio-records/screen-s5b.md): the pure rules behind the radio and source controls lane S5b adds on top of lane S5's panel - no Slint, no Win32, no engine call.
//
//   analogue IF bandwidth   the RTL-SDR tuner's own filter, in kHz, 0 = automatic (G1-14)
//
// The typed-field helpers (parseNumber, trim, numberText, Built) are lane L's.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>
#include <vector>

#include "lanel_logic.hpp"

namespace foxsdr::slints5b {

// ============================================================ analogue IF bandwidth (G1-14) ============================================================
// A bandwidth typed in kHz: empty or 0 = automatic (the filter follows the sample rate); otherwise 300 to 8000 (the driver's own range, which judges again). `hz` is 0 for automatic.
bool parseIfBandwidthKHz(const std::string& text, long long& hz, std::string& err);
// The engine's fact ("radio.N.ifBandwidth", Hz) in words: "Automatic (from the sample rate)" or "400 kHz"; "" for anything that is not a number.
std::string ifBandwidthWords(const std::string& fact);

}  // namespace foxsdr::slints5b
