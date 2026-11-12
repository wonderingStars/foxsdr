// sdrplay_rate_map.cpp - see sdrplay_rate_map.hpp for sources, licence and the one departure from
// SoapySDRPlay3 (the 8 MHz bandwidth).
//
// Ported as facts from pothosware/SoapySDRPlay3 Settings.cpp (MIT, Copyright (c) 2015 Charles J. Cliffe,
// Copyright (c) 2020 Franco Venturi); the notice is reproduced in THIRD-PARTY-NOTICES.md.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/sdrplay_rate_map.hpp"

#include <algorithm>
#include <cmath>

namespace cascade::source {

namespace abi = sdrplay_abi;

namespace {

constexpr double kLifOutputHz = 2000000.0;  // 6 MHz / 3 and 8 MHz / 4 as the API delivers after down-conversion
constexpr double kZeroIfDecimatedFsHz = 3072000.0;

bool near(double a, double b) { return std::fabs(a - b) < 0.5; }

// the six rates the LIF path offers, with their decimation factor
struct LifRow { double outHz; unsigned dec; };
constexpr LifRow kLif[] = {{62500.0, 32}, {125000.0, 16}, {250000.0, 8}, {500000.0, 4}, {1000000.0, 2}, {2000000.0, 1}};

// the four decimated zero-IF rates of the single tuner
struct ZeroRow { double outHz; unsigned dec; };
constexpr ZeroRow kZero[] = {{96000.0, 32}, {192000.0, 16}, {384000.0, 8}, {768000.0, 4}};

// SoapySDRPlay3 listSampleRates() for a single tuner
constexpr double kSingleRates[] = {62500.0,   96000.0,   125000.0,  192000.0,  250000.0,  384000.0,  500000.0,
                                   768000.0,  1000000.0, 2000000.0, 2048000.0, 3000000.0, 4000000.0, 5000000.0,
                                   6000000.0, 7000000.0, 8000000.0, 9000000.0, 10000000.0};

bool twoTuner(DuoMode m) { return m == DuoMode::DualTuner || m == DuoMode::MasterSlave; }

DuoRateResult refuse(const std::string& s) {
    DuoRateResult r;
    r.sentence = s;
    return r;
}

}  // namespace

abi::BwMHzT bandwidthForOutputRate(double r) {
    if (r < 300000.0) { return abi::BW_0_200; }
    if (r < 600000.0) { return abi::BW_0_300; }
    if (r < 1536000.0) { return abi::BW_0_600; }
    if (r < 5000000.0) { return abi::BW_1_536; }
    if (r < 6000000.0) { return abi::BW_5_000; }
    if (r < 7000000.0) { return abi::BW_6_000; }
    if (r < 8000000.0) { return abi::BW_7_000; }
    return abi::BW_8_000;
}

bool isDuoMasterFsHz(double fsHz) { return near(fsHz, 6000000.0) || near(fsHz, 8000000.0); }

std::vector<double> duoOutputRatesHz(DuoMode mode, double masterFsHz) {
    std::vector<double> v;
    if (twoTuner(mode)) {
        if (!isDuoMasterFsHz(masterFsHz)) { return v; }
        for (const LifRow& r : kLif) { v.push_back(r.outHz); }
        std::sort(v.begin(), v.end());
        return v;
    }
    v.assign(std::begin(kSingleRates), std::end(kSingleRates));
    return v;
}

DuoRateResult resolveDuoRate(DuoMode mode, double masterFsHz, double outputHz) {
    if (!std::isfinite(outputHz) || outputHz <= 0.0) {
        return refuse("That is not a valid sample rate.");
    }
    const bool two = twoTuner(mode);
    if (two && !isDuoMasterFsHz(masterFsHz)) {
        return refuse(std::string("In ") + duoModeName(mode) + " mode the ADC rate is 6 MHz or 8 MHz.");
    }
    // the low-IF path: used by every mode for the six rates up to 2 MS/s
    for (const LifRow& row : kLif) {
        if (!near(outputHz, row.outHz)) { continue; }
        const bool eight = two && near(masterFsHz, 8000000.0);
        DuoRateResult r;
        r.ok = true;
        DuoRateSetting& s = r.setting;
        s.outputHz = row.outHz;
        s.fsHz = eight ? 8000000.0 : 6000000.0;
        s.ifType = eight ? abi::IF_2_048 : abi::IF_1_620;
        s.soapyBw = bandwidthForOutputRate(row.outHz);
        // 8 MHz: only 1.536 MHz is a combination both the specification and the introduction list
        s.bw = eight ? abi::BW_1_536 : s.soapyBw;
        s.decimation = row.dec;
        s.decimationEnable = row.dec != 1;
        s.wideBandSignal = false;
        return r;
    }
    if (two) {
        return refuse(std::string("In ") + duoModeName(mode) +
                      " mode the output rate must be 62.5, 125, 250, 500 kS/s or 1 or 2 MS/s.");
    }
    // single tuner, zero IF, decimated
    for (const ZeroRow& row : kZero) {
        if (!near(outputHz, row.outHz)) { continue; }
        DuoRateResult r;
        r.ok = true;
        DuoRateSetting& s = r.setting;
        s.outputHz = row.outHz;
        s.fsHz = kZeroIfDecimatedFsHz;
        s.ifType = abi::IF_Zero;
        s.bw = s.soapyBw = bandwidthForOutputRate(row.outHz);
        s.decimation = row.dec;
        s.decimationEnable = true;
        s.wideBandSignal = true;
        return r;
    }
    if (outputHz > kLifOutputHz && outputHz <= kDuoSingleZeroIfMaxHz) {
        DuoRateResult r;
        r.ok = true;
        DuoRateSetting& s = r.setting;
        s.outputHz = outputHz;
        s.fsHz = outputHz;
        s.ifType = abi::IF_Zero;
        s.bw = s.soapyBw = bandwidthForOutputRate(outputHz);
        s.decimation = 1;
        s.decimationEnable = false;
        s.wideBandSignal = true;
        return r;
    }
    if (outputHz > kDuoSingleZeroIfMaxHz) {
        return refuse("The RSP's sample rate goes up to 10.66 MS/s.");
    }
    return refuse("Below 2 MS/s the single-tuner rates are 62.5, 96, 125, 192, 250, 384, 500, 768 kS/s and 1 MS/s.");
}

DuoRateResult validateDuoSetting(DuoMode mode, double masterFsHz, const DuoRateSetting& s) {
    DuoRateResult want = resolveDuoRate(mode, masterFsHz, s.outputHz);
    if (!want.ok) { return want; }
    const DuoRateSetting& w = want.setting;
    if (!near(s.fsHz, w.fsHz)) {
        return refuse("The ADC rate does not match this output rate.");
    }
    if (s.ifType != w.ifType) {
        return refuse("The IF type does not match this output rate.");
    }
    if (s.decimation != w.decimation || s.decimationEnable != w.decimationEnable) {
        return refuse("The decimation does not match this output rate.");
    }
    if (s.wideBandSignal != w.wideBandSignal) {
        return refuse("The wide-band flag does not match this output rate.");
    }
    if (s.bw != w.bw) {
        return refuse("The IF bandwidth does not match this output rate.");
    }
    return want;
}

}  // namespace cascade::source
