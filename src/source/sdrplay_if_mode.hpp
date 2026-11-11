// sdrplay_if_mode.hpp - the SDRplay IF-mode CHOICE (survey row R13): zero-IF (today's
// behaviour and the default) or low-IF, as a rate plan the SdrPlaySource can adopt later.
//
// STAND-ALONE: nothing in sdrplay_source.cpp calls this yet. sdrPlayRatePlan() in
// sdrplay_source.cpp is untouched and stays the zero-IF plan; the wiring lane decides how
// the setting reaches it (HAND-OFF in docs/feature-records/radio-sources.md).
//
// WHERE THE NUMBERS COME FROM (all project-own, no outside source read in this lane):
//  * The low-IF combinations are the ones the SDRplay API specification lists (section 3.15,
//    "Conditions for LIF down-conversion to be enabled", as transcribed in
//    sdrplay_probe.cpp stepIfModes): fs 2 MHz + IF 450 kHz + BW 0.2 or 0.6; fs 6 MHz + IF
//    1620 kHz + BW 1.536; fs 8 MHz or 8.192 MHz + IF 2048 kHz + BW 1.536 (8 MHz also with
//    BW 5.0). The specification PDF itself was NOT reachable in this lane (the re-check of
//    2026-11-11 saw 404): the table is the earlier transcription, NOT re-verified.
//  * The output-rate arithmetic is fsHz / decimation, the one rule sdrplay_source.cpp holds
//    every plan to. WHAT A LOW-IF STREAM ACTUALLY DELIVERS is the open question that made
//    0.99.44 retire low-IF: two RSP2 field logs showed the ADC rate (6 MS/s) arriving
//    where the old plan expected 2 MS/s. So a plan here carries the rate it EXPECTS, and
//    sdrPlayLowIfRateVerdict() classifies a MEASURED rate against it - the check the wiring
//    lane must run on the first real stream, and refuse low-IF on a mismatch.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>

#include "source/sdrplay_source.hpp"

namespace cascade::source {

enum class SdrPlayIfMode { ZeroIf, LowIf };

const char* sdrPlayIfModeName(SdrPlayIfMode m);  // "zero-IF", "low-IF"
// "zero", "zero-if", "low", "low-if" in any case; false for anything else (mode untouched).
bool parseSdrPlayIfMode(const std::string& text, SdrPlayIfMode& mode);

// The plan for one output rate in one mode. ZeroIf is exactly sdrPlayRatePlan(). LowIf takes
// the rates a listed combination can produce (62.5k..1M, 2M from fs 2 MHz; 2M and 3M from
// 6 MHz; 2M, 4M and 8M from 8 MHz) and refuses the rest (false, `out` default): a rate with
// no listed combination is never guessed at.
bool sdrPlayRatePlanForMode(double outputRateHz, SdrPlayIfMode mode, SdrPlayRatePlan& out);

// The output rates a mode offers, ascending. ZeroIf: sdrPlaySupportedRatesHz().
std::vector<double> sdrPlaySupportedRatesForMode(SdrPlayIfMode mode);

enum class LowIfRateVerdict {
    AsPlanned,       // measured within 2% of fsHz/decM
    DeliversAdcRate, // measured within 2% of fsHz while decM != 1: the 0.99.43 field behaviour
    Other            // neither
};
LowIfRateVerdict sdrPlayLowIfRateVerdict(const SdrPlayRatePlan& plan, double measuredHz);
const char* sdrPlayLowIfRateVerdictSentence(LowIfRateVerdict v);

}  // namespace cascade::source
