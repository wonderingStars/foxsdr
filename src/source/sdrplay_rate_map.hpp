// sdrplay_rate_map.hpp - the RSPduo (and single-tuner RSP) OUTPUT-RATE MAP: which output sample rate is
// reached with which ADC rate, IF type, IF bandwidth and decimation factor, per operating mode, and the
// validation of a setting against it. Pure logic over sdrplay_abi types; nothing here loads or calls the
// SDRplay API (the device itself stays on the user-installed API, as everywhere in FoxSDR), and nothing
// calls this yet (wiring hand-off in docs/feature-records/oss-radios.md).
//
// SOURCES AND LICENCES (opened 2026-11-12, commit read in THIRD-PARTY-NOTICES.md):
//   * pothosware/SoapySDRPlay3, Settings.cpp (MIT, "Copyright (c) 2015 Charles J. Cliffe, Copyright (c) 2020
//     Franco Venturi"; LICENSE.txt and the file header both opened). Ported as FACTS in our own code: the
//     functions getInputSampleRateAndDecimation, listSampleRates, getSampleRate and getBwEnumForRate, and the
//     RSPduo mode strings ("ST", "DT", "MA", "MA8", "SL"). No file was pasted.
//   * SDRplay API Specification v3.15 (sdrplay.com, no licence stated, opened): the six single-tuner LIF
//     down-conversion conditions, "In RSPduo master/slave mode, down-conversion is always enabled", the
//     DecimationT structure (enable, decimationFactor, wideBandSignal) - which lists NO factor values: the
//     factor list below is SoapySDRPlay3's.
//
// WHAT THE MAP SAYS (all from SoapySDRPlay3):
//   * LOW IF (LIF) path, used by dual tuner, master/slave and, for output rates up to 2 MS/s, single tuner:
//     ADC 6 MHz with IF 1.620 MHz, or (master only, "MA8") ADC 8 MHz with IF 2.048 MHz. The API's down-conversion
//     delivers 2 MS/s (getSampleRate in SoapySDRPlay3 reports 2.0e6 for both pairs); the decimation stage then
//     divides by 1, 2, 4, 8, 16 or 32, giving 2 M, 1 M, 500 k, 250 k, 125 k and 62.5 k.
//   * single tuner, zero IF, output below 2 MS/s: 96 k, 192 k, 384 k, 768 k use ADC 3.072 MS/s with decimation
//     32, 16, 8, 4 (output x factor = 3.072 MS/s in each case).
//   * single tuner, zero IF, above 2 MS/s: ADC = output, no decimation, wideBandSignal 1.
//   * the IF bandwidth follows the output rate (getBwEnumForRate).
// WHERE THIS FILE DEPARTS FROM SoapySDRPlay3: at an 8 MHz ADC rate SoapySDRPlay3 sets the same per-rate
// bandwidth as at 6 MHz (200 kHz and up). The API specification's low-IF list allows 8 MHz only with 1.536 MHz
// (and 5 MHz, single tuner), and the RSPduo introduction (read by the earlier lane, see sdrplay_dual_tuner.hpp)
// says the same for two-tuner use, so `bw` is 1.536 MHz at 8 MHz - the combination both documents agree on -
// and `soapyBw` carries what SoapySDRPlay3 would have set. UNVERIFIED ON A REAL RSPduo which one the hardware
// prefers.
// A CORRECTION TO THE EARLIER LANE: sdrplay_dual_tuner.hpp's `documentedLowIfOutputRatesHz` calls 6 and 8 MHz
// the dual-tuner "output rates"; those are ADC rates. The stream the API hands over in LIF mode is 2 MS/s
// before decimation (SoapySDRPlay3 getSampleRate). checkDuoRate there is also stricter than the map below
// (decimation 1 only): this map is the one for the rate menu.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>
#include <vector>

#include "source/sdrplay_api_decl.hpp"
#include "source/sdrplay_dual_tuner.hpp"

namespace cascade::source {

// One resolved setting: everything the API needs for a requested output rate.
struct DuoRateSetting {
    double outputHz = 0.0;   // rate the client sees
    double fsHz = 0.0;       // sdrplay_api_DevParamsT fsFreq.fsHz
    sdrplay_abi::IfKHzT ifType = sdrplay_abi::IF_Undefined;
    sdrplay_abi::BwMHzT bw = sdrplay_abi::BW_Undefined;      // what to set (see the header: documents agree)
    sdrplay_abi::BwMHzT soapyBw = sdrplay_abi::BW_Undefined; // what SoapySDRPlay3 would set
    unsigned decimation = 1;        // decimationFactor
    bool decimationEnable = false;  // decimation.enable
    bool wideBandSignal = false;    // decimation.wideBandSignal (1 for zero IF)
};

struct DuoRateResult {
    bool ok = false;
    DuoRateSetting setting;
    std::string sentence;  // the refusal, empty when ok
};

// The IF bandwidth for an output rate, as SoapySDRPlay3's getBwEnumForRate: < 300 k -> 200 kHz, < 600 k -> 300
// kHz, < 1.536 M -> 600 kHz, < 5 M -> 1.536 MHz, < 6 M -> 5 MHz, < 7 M -> 6 MHz, < 8 M -> 7 MHz, else 8 MHz.
sdrplay_abi::BwMHzT bandwidthForOutputRate(double outputHz);

// The ADC rate of the two-tuner and master/slave modes: 6 MHz or 8 MHz (the master dictates it).
bool isDuoMasterFsHz(double fsHz);

// The output rates a mode offers, ascending. `masterFsHz` (6 MHz or 8 MHz) is used only by the two-tuner
// modes. Dual tuner and master/slave: 62.5 k, 125 k, 250 k, 500 k, 1 M, 2 M. Single tuner: the 19 rates
// SoapySDRPlay3 lists. Empty when masterFsHz is not 6 or 8 MHz for a two-tuner mode.
std::vector<double> duoOutputRatesHz(DuoMode mode, double masterFsHz = 6000000.0);

// The output ranges of the single tuner's zero-IF side as SoapySDRPlay3 states them: 2 MHz up to 10.66 MHz.
constexpr double kDuoSingleZeroIfMinHz = 2000000.0;
constexpr double kDuoSingleZeroIfMaxHz = 10660000.0;

// Resolves an output rate (whole Hz, +-0.5 Hz tolerated) to a setting, or says why not.
DuoRateResult resolveDuoRate(DuoMode mode, double masterFsHz, double outputHz);

// Checks a complete setting (as it would be written into the API structures) against the map: ok when it is
// one the map could produce for `mode` (and the same output rate), otherwise the sentence says which field is
// wrong. This is what a settings-restore path uses before it hands stored values to the API.
DuoRateResult validateDuoSetting(DuoMode mode, double masterFsHz, const DuoRateSetting& s);

}  // namespace cascade::source
