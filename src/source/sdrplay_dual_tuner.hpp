// sdrplay_dual_tuner.hpp - the SOURCE-LEVEL half of RSPduo dual-tuner support (survey row R9): deciding
// from what the SDRplay API lists whether an RSPduo can be opened in dual-tuner mode and offering its two
// tuners, preparing the device structure for sdrplay_api_SelectDevice, and holding the rate and bandwidth
// rules of that mode. Pure logic over sdrplay_abi::DeviceT, with an injected device-list interface; no
// engine, multi-radio or SdrPlaySource change, and called by nothing yet.
//
// DOCUMENT. "SDRplay API Specification" v3.15, https://sdrplay.com/wp-content/uploads/2026/04/
// SDRplay_API_Specification_v3.15.pdf (41 pages, 723 KB, fetched and read where it matters on 2026-11-11;
// found through https://www.sdrplay.com/api/, which states no licence near the link: copyright footer
// only, terms NOT VERIFIED). The re-check of the same day recorded this PDF as 404; the real address is
// the one above. What it says that this file uses:
//   * "The RSPduo can operate in single tuner mode (just like an RSP2 for example), in dual tuner mode
//     (both streams in a single instance) or in master/slave mode. If the RSPduo is already in use in
//     master mode, then accessing the device again will mean that only slave mode is available."
//   * sdrplay_api_DeviceT: `tuner` and `rspDuoMode` are "set by the API on return from
//     sdrplay_api_GetDevices() ... indicating which tuners are available / which modes are available" and
//     "set by the application and used during sdrplay_api_SelectDevice() to indicate which tuner(s) /
//     which mode is to be used". RspDuoMode values Single_Tuner 1, Dual_Tuner 2, Master 4, Slave 8;
//     TunerSelect A 1, B 2, Both 3.
//   * Stream B's callback "will only be used in dual tuner mode".
//   * Master/slave sample rates: the master sets rspDuoSampleFreq at SelectDevice (the spec's sample code
//     uses 6 MHz), and sdrplay_api_SwapRspDuoDualTunerModeSampleRate "can be called to change sample
//     rates between 6MHz and 8MHz" (master/slave mode, after Init, master only).
//   * In master/slave mode LIF down-conversion is always enabled and the default IF is zero-IF for the
//     master, 450 kHz for the slave.
//
// THE RATE RULES (follow-up lane feat/hpsdrduo, 2026-11-12). The specification's pointer, "pages 4 and 5 of the
// RSPduo introduction document" (RSPduo-Introduction-V3.pdf), still answers 404 on sdrplay.com and 403 on the
// one reseller mirror of V3 that was found (storage.eleshop.eu; not worked around). Opened instead, all read
// only, none stating a licence:
//   * SDRplay's own "RSPduo Datasheet" V0.9 (Jun 2020), https://sdrplay.com/wp-content/uploads/2026/06/
//     RSPduo_Datasheet.pdf (2 pages, fetched 2026-11-12): "Sample frequency 2 - 10.66MSPS" (14 bit native
//     2 - 6.048, 12 bit 6.048 - 8.064, 10 bit 8.064 - 9.216, 8 bit above); IF modes "Zero IF, All IF
//     bandwidths" and "Low IF, IF bandwidths <= 1.536MHz"; IF bandwidths 200, 300, 600 kHz, 1.536, 5.0, 6.0,
//     7.0, 8.0 MHz; operating modes "One receive stream, any IF mode or bandwidth ... from either tuner, or
//     two simultaneous receive streams from both tuners using low IF mode and bandwidth limited to
//     1.536MHz maximum" (the table the sheet refers to is a picture and has no text).
//   * "Introduction to the RSPduo" V2.2 (an earlier revision than the V3 the specification cites; marked
//     "New Product Announcement" and "Confidential" in its page footers though it is freely distributed),
//     https://alexsradioshop.de/wp-content/uploads/2023/05/RSPduo-Introduction-V2-2.pdf (a reseller's copy
//     of SDRplay's document), pages 4 and 5: dual-tuner operation is only possible with BOTH tuners in LOW
//     IF mode (maximum flat IF bandwidth 1.536 MHz); "In low IF mode, there will only be two sample rates
//     available: 6 MHz ... 8 MHz"; the matrix: single tuner, ADC rate 2 - 10 MHz, IF bandwidth 200 kHz - 8
//     MHz; dual tuner, ADC rate 6 / 8 MHz, IF bandwidth 200 kHz - 1.536 MHz for both tuners; the tuner set
//     up first (the master) dictates the ADC rate of the second.
//   * The specification itself, section 3.15 (sdrplay_api_Init): "Conditions for LIF down-conversion to be
//     enabled for all RSPs in single tuner mode" (six lines, repeated in the test as a stub of that check)
//     and "In RSPduo master/slave mode, down-conversion is always enabled"; section 3.19: master/slave sample
//     rate "between 6MHz and 8MHz"; the tuner-parameter comment: default ifType IF_Zero (master) or
//     IF_0_450 (slave).
// WHAT IS BUILT FROM THEM (documentedLowIfRows / checkDuoRate below): single tuner = zero IF at 2 - 10.66 MHz
// with any of the eight bandwidths, or the specification's low-IF combinations; dual tuner AND master/slave =
// low IF only, ADC rate 6 MHz or 8 MHz, bandwidth <= 1.536 MHz, each row ALSO one of the specification's
// low-IF combinations (6 MHz with IF 1.620 MHz and bandwidth 0.2 .. 1.536; 8 MHz with IF 2.048 MHz and
// bandwidth 1.536): the INTERSECTION of the two documents, so a row is refused unless both agree.
// WHAT NO DOCUMENT OPENED SAYS, so it is refused, not guessed:
//   * the decimation factors: the specification shows `decimationFactor` as a byte and the stage as "disabled
//     by default" and lists no values. In DUAL-TUNER and MASTER/SLAVE mode only factor 1 (no decimation) is
//     therefore accepted. In SINGLE-tuner mode factors 1, 2, 4, 8, 16, 32 are accepted because the shipping
//     driver already uses them (sdrplay_source.cpp, two RSP2 field logs of 0.99.44), not because a document
//     lists them.
//   * the master/slave default IF (the specification's comment says zero IF for the master, which the
//     Introduction's "both tuners in low IF" contradicts): the table follows the Introduction.
//   * whether the two tuners are coherent (the product page says "Not stated"): nothing here claims it.
//   * the V3 revision of the Introduction, and whether its pages 4 and 5 differ from V2.2's.
// DualTunerRateTable itself still ships EMPTY (known() false) as before; documentedDualTunerTable() returns it
// filled from the rows above.
//
// UNPROVEN ON A REAL RSPduo. The tests use hand-built DeviceT records.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>
#include <vector>

#include "source/sdrplay_api_decl.hpp"

namespace cascade::source {

// What GetDevices lists, as the injected interface hands it over: the raw DeviceT records the API filled
// (so a test builds them by hand, and the wiring lane adapts the real session code).
class DuoDeviceList {
public:
    virtual ~DuoDeviceList() = default;
    // False with `error` when the list could not be read (service not running, API not loaded ...).
    virtual bool getDevices(std::vector<sdrplay_abi::DeviceT>& out, std::string& error) = 0;
    // The API version the service answered (for the `valid` field, meaningless below 3.08).
    virtual float apiVersion() const = 0;
};

enum class DualTunerStatus {
    Available,
    NotAnRspDuo,
    NotValid,          // the API says the device is not ready
    ModeNotOffered,    // Dual_Tuner is not among the modes the API offers
    InUseAsMaster,     // another program holds it as master: only slave is offered
    TunerNotFree,      // not both tuners are available
};

struct DualTunerChoice {
    std::string label;                      // "Tuner 1 (A)", "Tuner 2 (B)"
    sdrplay_abi::TunerSelectT tuner;        // Tuner_A / Tuner_B
};

struct DualTunerOffer {
    DualTunerStatus status = DualTunerStatus::NotAnRspDuo;
    std::string sentence;                   // for the screen: why not, or "two tuners available"
    std::string label;                      // "SDRplay RSPduo (serial ...1234)", serial masked
    std::vector<DualTunerChoice> tuners;    // two when Available, empty otherwise
    sdrplay_abi::DeviceT device{};          // the record, for prepareDualTunerSelect
};

// Decides for one record. `apiVersion` gates the `valid` field (below 3.08 it is ignored, as in
// sdrplay_api_decl.hpp). Never reads past DeviceT::SerNo's size. Pure.
DualTunerOffer assessDualTuner(const sdrplay_abi::DeviceT& dev, float apiVersion);

// Every RSPduo in the list, each assessed (non-RSPduo records are skipped, not refused). False with
// `error` when the list could not be read.
bool enumerateDualTuner(DuoDeviceList& list, std::vector<DualTunerOffer>& offers, std::string& error);

// Sets what SelectDevice needs for dual-tuner mode on a copy of an Available offer's record:
// rspDuoMode = Dual_Tuner, tuner = Tuner_Both. False (record untouched) when the offer is not Available.
bool prepareDualTunerSelect(const DualTunerOffer& offer, sdrplay_abi::DeviceT& out);

// The master/slave sample rates the specification states (6 MHz and 8 MHz), for the master/slave modes,
// which this lane does not build; here for the wiring lane's rate menu.
std::vector<double> masterSlaveRatesHz();

// --- the dual-tuner rate rules ----------------------------------------------------------------

struct DualTunerRate {
    double fsHz = 0.0;
    sdrplay_abi::BwMHzT bw = sdrplay_abi::BW_Undefined;
    sdrplay_abi::IfKHzT ifType = sdrplay_abi::IF_Undefined;
    unsigned int decimation = 1;
};

struct DualTunerRateCheck {
    bool ok = false;
    bool tableKnown = false;
    std::string sentence;   // the refusal, or empty when ok
};

class DualTunerRateTable {
public:
    // EMPTY by default: see the header. Replacing the table is the only way a rate becomes allowed.
    void setTable(std::vector<DualTunerRate> allowed) { allowed_ = std::move(allowed); known_ = true; }
    bool known() const { return known_; }
    const std::vector<DualTunerRate>& allowed() const { return allowed_; }
    // Exact match on fs (within 1 Hz), bandwidth, IF and decimation.
    DualTunerRateCheck check(const DualTunerRate& want) const;
    // The output rates the table allows, ascending, duplicates removed (fs / decimation).
    std::vector<double> outputRatesHz() const;

private:
    std::vector<DualTunerRate> allowed_;
    bool known_ = false;
};

// --- the documented rate rules of the three modes ----------------------------------------------

enum class DuoMode { SingleTuner, DualTuner, MasterSlave };
const char* duoModeName(DuoMode m);

// The enumerated low-IF combinations of a mode, decimation 1 (see the header comment for where each comes
// from). Single tuner's zero-IF side is a range, not rows: see checkDuoRate.
std::vector<DualTunerRate> documentedLowIfRows(DuoMode mode);

// The whole rule for one request: decimation, then (single tuner, zero IF) the 2 - 10.66 MHz range with a
// defined bandwidth, or one of the documented low-IF rows. `tableKnown` is always true.
DualTunerRateCheck checkDuoRate(DuoMode mode, const DualTunerRate& want);

// Output rates (fs / decimation) the low-IF rows allow for a mode, ascending.
std::vector<double> documentedLowIfOutputRatesHz(DuoMode mode);

// The dual-tuner table filled from the documents (rows of documentedLowIfRows(DualTuner)).
DualTunerRateTable documentedDualTunerTable();

constexpr double kSingleTunerMinFsHz = 2000000.0;
constexpr double kSingleTunerMaxFsHz = 10660000.0;

}  // namespace cascade::source
