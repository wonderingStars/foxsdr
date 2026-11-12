// sdrplay_dual_tuner.cpp - see sdrplay_dual_tuner.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/sdrplay_dual_tuner.hpp"

#include <algorithm>
#include <cmath>
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

}  // namespace cascade::source
