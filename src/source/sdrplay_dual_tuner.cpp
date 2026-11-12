// sdrplay_dual_tuner.cpp - see sdrplay_dual_tuner.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/sdrplay_dual_tuner.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <cstdio>

namespace cascade::source {

namespace abi = sdrplay_abi;

namespace {

std::string serialOf(const abi::DeviceT& d) {
    std::size_t len = 0;
    while (len < sizeof(d.SerNo) && d.SerNo[len] != '\0') { ++len; }
    return std::string(d.SerNo, len);
}

std::string labelOf(const abi::DeviceT& d) {
    const std::string s = serialOf(d);
    std::string l = "SDRplay RSPduo";
    if (s.size() >= 4) { l += " (serial ..." + s.substr(s.size() - 4) + ")"; }
    return l;
}

}  // namespace

DualTunerOffer assessDualTuner(const abi::DeviceT& dev, float apiVersion) {
    DualTunerOffer o;
    o.device = dev;
    o.label = labelOf(dev);
    if (dev.hwVer != abi::kRspDuo) {
        o.status = DualTunerStatus::NotAnRspDuo;
        o.sentence = "Only the RSPduo has two tuners.";
        return o;
    }
    // `valid` is meaningless below API 3.08 (sdrplay_api_decl.hpp, kValidFieldSinceVersion).
    if (apiVersion >= 3.08f - 0.001f && dev.valid == 0) {
        o.status = DualTunerStatus::NotValid;
        o.sentence = "The RSPduo is not ready to be used yet.";
        return o;
    }
    const int modes = static_cast<int>(dev.rspDuoMode);
    if ((modes & abi::RspDuoMode_Dual_Tuner) == 0) {
        if ((modes & abi::RspDuoMode_Slave) != 0 && (modes & abi::RspDuoMode_Single_Tuner) == 0) {
            o.status = DualTunerStatus::InUseAsMaster;
            o.sentence = "Another program is using this RSPduo as master, so only slave mode is available, not dual-tuner mode.";
        } else {
            o.status = DualTunerStatus::ModeNotOffered;
            o.sentence = "This RSPduo does not offer dual-tuner mode right now.";
        }
        return o;
    }
    const int tuners = static_cast<int>(dev.tuner);
    if ((tuners & abi::Tuner_Both) != abi::Tuner_Both) {
        o.status = DualTunerStatus::TunerNotFree;
        o.sentence = std::string("Dual-tuner mode needs both tuners free (tuner A is ") +
                     ((tuners & abi::Tuner_A) ? "free" : "in use") + ", tuner B is " +
                     ((tuners & abi::Tuner_B) ? "free" : "in use") + ").";
        return o;
    }
    o.status = DualTunerStatus::Available;
    o.sentence = "Two tuners are available.";
    o.tuners.push_back({"Tuner 1 (A)", abi::Tuner_A});
    o.tuners.push_back({"Tuner 2 (B)", abi::Tuner_B});
    return o;
}

bool enumerateDualTuner(DuoDeviceList& list, std::vector<DualTunerOffer>& offers, std::string& error) {
    offers.clear();
    std::vector<abi::DeviceT> devs;
    if (!list.getDevices(devs, error)) {
        if (error.empty()) { error = "the SDRplay device list could not be read"; }
        return false;
    }
    const float v = list.apiVersion();
    for (const abi::DeviceT& d : devs) {
        if (d.hwVer != abi::kRspDuo) { continue; }
        offers.push_back(assessDualTuner(d, v));
    }
    return true;
}

bool prepareDualTunerSelect(const DualTunerOffer& offer, abi::DeviceT& out) {
    if (offer.status != DualTunerStatus::Available) { return false; }
    out = offer.device;
    out.rspDuoMode = abi::RspDuoMode_Dual_Tuner;
    out.tuner = abi::Tuner_Both;
    return true;
}

std::vector<double> masterSlaveRatesHz() { return {6000000.0, 8000000.0}; }

DualTunerRateCheck DualTunerRateTable::check(const DualTunerRate& want) const {
    DualTunerRateCheck c;
    c.tableKnown = known_;
    if (!known_) {
        c.sentence = "The allowed sample rates and bandwidths of the RSPduo's dual-tuner mode are not known to this build.";
        return c;
    }
    if (!std::isfinite(want.fsHz) || want.decimation == 0) {
        c.sentence = "That is not a valid sample-rate setting.";
        return c;
    }
    for (const DualTunerRate& a : allowed_) {
        if (std::fabs(a.fsHz - want.fsHz) < 1.0 && a.bw == want.bw && a.ifType == want.ifType &&
            a.decimation == want.decimation) {
            c.ok = true;
            return c;
        }
    }
    c.sentence = "The RSPduo's dual-tuner mode does not allow that sample rate and bandwidth.";
    return c;
}

std::vector<double> DualTunerRateTable::outputRatesHz() const {
    std::vector<double> v;
    for (const DualTunerRate& a : allowed_) {
        if (a.decimation > 0) { v.push_back(a.fsHz / a.decimation); }
    }
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end(), [](double x, double y) { return std::fabs(x - y) < 0.5; }), v.end());
    return v;
}

const char* duoModeName(DuoMode m) {
    switch (m) {
        case DuoMode::SingleTuner: return "single tuner";
        case DuoMode::DualTuner: return "dual tuner";
        case DuoMode::MasterSlave: return "master/slave";
    }
    return "?";
}

std::vector<DualTunerRate> documentedLowIfRows(DuoMode mode) {
    std::vector<DualTunerRate> v;
    const auto add = [&v](double fs, abi::BwMHzT bw, abi::IfKHzT ifk) { v.push_back({fs, bw, ifk, 1}); };
    if (mode == DuoMode::SingleTuner) {
        // the six conditions of sdrplay_api_Init, expanded (the "<=" lines over the bandwidth enumeration)
        add(8192000.0, abi::BW_1_536, abi::IF_2_048);
        add(8000000.0, abi::BW_1_536, abi::IF_2_048);
        add(8000000.0, abi::BW_5_000, abi::IF_2_048);
        add(2000000.0, abi::BW_0_200, abi::IF_0_450);
        add(2000000.0, abi::BW_0_300, abi::IF_0_450);
        add(2000000.0, abi::BW_0_600, abi::IF_0_450);
        add(6000000.0, abi::BW_0_200, abi::IF_1_620);
        add(6000000.0, abi::BW_0_300, abi::IF_1_620);
        add(6000000.0, abi::BW_0_600, abi::IF_1_620);
        add(6000000.0, abi::BW_1_536, abi::IF_1_620);
    } else {
        // dual tuner and master/slave: ADC rate 6 or 8 MHz, bandwidth <= 1.536 MHz (Introduction, datasheet),
        // and a combination the specification's low-IF list permits
        add(6000000.0, abi::BW_0_200, abi::IF_1_620);
        add(6000000.0, abi::BW_0_300, abi::IF_1_620);
        add(6000000.0, abi::BW_0_600, abi::IF_1_620);
        add(6000000.0, abi::BW_1_536, abi::IF_1_620);
        add(8000000.0, abi::BW_1_536, abi::IF_2_048);
    }
    return v;
}

namespace {
bool powerOfTwoUpTo32(unsigned d) { return d == 1 || d == 2 || d == 4 || d == 8 || d == 16 || d == 32; }
bool definedBandwidth(abi::BwMHzT bw) {
    return bw == abi::BW_0_200 || bw == abi::BW_0_300 || bw == abi::BW_0_600 || bw == abi::BW_1_536 || bw == abi::BW_5_000 ||
           bw == abi::BW_6_000 || bw == abi::BW_7_000 || bw == abi::BW_8_000;
}
}  // namespace

DualTunerRateCheck checkDuoRate(DuoMode mode, const DualTunerRate& want) {
    DualTunerRateCheck c;
    c.tableKnown = true;
    if (!std::isfinite(want.fsHz) || want.fsHz <= 0.0 || want.decimation == 0) {
        c.sentence = "That is not a valid sample-rate setting.";
        return c;
    }
    if (mode == DuoMode::SingleTuner) {
        if (!powerOfTwoUpTo32(want.decimation)) {
            c.sentence = "In single-tuner mode the decimation must be 1, 2, 4, 8, 16 or 32.";
            return c;
        }
        if (want.ifType == abi::IF_Zero) {
            if (!definedBandwidth(want.bw)) {
                c.sentence = "That is not one of the RSPduo's IF bandwidths (200, 300, 600 kHz, 1.536, 5, 6, 7 or 8 MHz).";
                return c;
            }
            if (want.fsHz < kSingleTunerMinFsHz || want.fsHz > kSingleTunerMaxFsHz) {
                c.sentence = "In zero-IF single-tuner mode the ADC rate must be 2 to 10.66 MHz.";
                return c;
            }
            c.ok = true;
            return c;
        }
    } else if (want.decimation != 1) {
        c.sentence = std::string("In ") + duoModeName(mode) +
                     " mode only no decimation is accepted: no SDRplay document opened lists the decimation factors.";
        return c;
    }
    for (const DualTunerRate& a : documentedLowIfRows(mode)) {
        if (std::fabs(a.fsHz - want.fsHz) < 1.0 && a.bw == want.bw && a.ifType == want.ifType) {
            c.ok = true;
            return c;
        }
    }
    c.sentence = mode == DuoMode::SingleTuner
                     ? "That is not one of the low-IF combinations the SDRplay API specification lists."
                     : std::string("In ") + duoModeName(mode) +
                           " mode both tuners run in low IF at an ADC rate of 6 or 8 MHz with a bandwidth of at most 1.536 MHz.";
    return c;
}

std::vector<double> documentedLowIfOutputRatesHz(DuoMode mode) {
    std::vector<double> v;
    for (const DualTunerRate& a : documentedLowIfRows(mode)) { v.push_back(a.fsHz / a.decimation); }
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end(), [](double x, double y) { return std::fabs(x - y) < 0.5; }), v.end());
    return v;
}

DualTunerRateTable documentedDualTunerTable() {
    DualTunerRateTable t;
    t.setTable(documentedLowIfRows(DuoMode::DualTuner));
    return t;
}

}  // namespace cascade::source
