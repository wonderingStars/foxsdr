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
// WHAT THE SPECIFICATION DOES NOT SAY, so is NOT BUILT: the allowed sample-rate and bandwidth
// combinations of DUAL-TUNER mode itself. The spec points at "pages 4 and 5 of the RSPduo introduction
// document" (https://www.sdrplay.com/wp-content/uploads/2018/05/RSPduo-Introduction-V3.pdf), which
// answered with an HTML error page (404) here. So DualTunerRateTable ships EMPTY: it refuses every
// dual-tuner rate with a sentence saying the table is not known, and a caller (or a later lane that has
// the document) fills it in with setTable(); the checking logic is proven with a table the TEST invents.
// Whether the two tuners are coherent is also not stated by the specification (the product page says
// "Not stated"): UNPROVEN, and nothing here claims it.
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

}  // namespace cascade::source
