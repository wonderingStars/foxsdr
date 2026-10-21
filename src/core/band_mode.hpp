// band_mode.hpp - the conventional demodulator for a band-plan entry, and the
// rule for when a typed frequency may adopt it.
//
// THE REQUEST (field report a09fe09722e4691e, 0.99.78): type 3.620 MHz and be
// in LSB, because 80 m phone is LSB and retyping the mode after every band
// change is toil the band plan already knows how to spare. The counter's typed
// entry is the one gesture that MEANS "take me to that station", so it is the
// one gesture that follows the plan; wheeling a digit, dragging the VFO and a
// scanner step all stay within the user's hand and never touch the mode.
//
// TWO DELIBERATE LIMITS, both of which exist because a wrong automatic mode
// is worse than a manual one:
//
//   - THE MODE MOVES ONLY WHEN THE BAND CHANGES. Digital stations sit inside
//     phone bands with the "wrong" sideband by convention - FT8 on 7.074 MHz
//     is USB inside a band whose phone convention is LSB - and a user who has
//     set that up retypes nearby frequencies all session. Within one band
//     entry the mode is therefore theirs; only CROSSING into a different
//     entry (or in from a gap) adopts the new band's convention. Jumping from
//     20 m to 7.074 still lands in LSB, and the FT8 plugin's own notice
//     ("Paused: receiver not in USB", 1.1.1) names the remedy.
//
//   - NO CONVENTION, NO CHANGE. A service whose mode cannot be said with
//     confidence (satellite transponders are split between SSB and FM;
//     "mobile" spans analogue PMR and digital trunking; DAB is not a
//     demodulator this receiver has) answers "", and the mode stays put.
//     RAW is a diagnostics hold: while the user is in RAW nothing moves it.
//
// Pure functions, no GUI types, so every row of the convention is a table
// test (tests/test_band_mode.cpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>

#include "core/band_plan.hpp"

namespace cascade::core {

// The conventional demodulator for a band of class `service` at `hz`, as one
// of the Radio section's mode names ("AM", "LSB", "USB", "NFM", "WFM"), or ""
// when no convention is confident enough to act on. The table:
//
//   amateur    below 10 MHz LSB (the HF phone convention), except the 60 m
//              channels (5.25-5.45 MHz), which are USB by their licensing
//              conditions; 10-30 MHz USB; at VHF/UHF NFM.
//   broadcast  up to 30 MHz AM (LW/MW/SW); 64-108 MHz WFM (OIRT, Japanese and
//              CCIR FM bands together); above that "" (DAB and friends are
//              not a demodulator).
//   aviation   below 30 MHz USB (HF air routes); above it AM (VHF airband and
//              UHF military voice are both AM).
//   marine     below 30 MHz USB (HF working channels); above it NFM (the VHF
//              marine channels).
//   iss        NFM (voice, APRS and SSTV downlinks are all FM).
//   satellite, mobile, other, anything unrecognised: "".
const char* conventionalModeForBand(const std::string& service, double hz);

// Whether a typed retune that moved the band-plan readout from `from` to `to`
// (either may be null: a gap) should adopt `suggested` (the answer above for
// `to`). True only when there IS a suggestion, it differs from `currentMode`,
// the band actually changed, and the user is not parked in RAW.
//
// `from` and `to` are entries of the SAME plan (BandPlan::at before and after
// the retune); identity is by pointer, which at() guarantees stable for one
// plan load.
bool modeFollowsTypedBandChange(const BandEntry* from, const BandEntry* to,
                                const char* currentMode, const char* suggested);

}  // namespace cascade::core
