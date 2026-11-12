// test_sdrplay_rate_map.cpp - the RSPduo / RSP output-rate map (src/source/sdrplay_rate_map.hpp). Expectations
// are written out LITERALLY from SoapySDRPlay3 getInputSampleRateAndDecimation (MIT) as read on 2026-11-12:
// output 62500 -> decimation 32, 125000 -> 16, 250000 -> 8, 500000 -> 4, 1000000 -> 2, 2000000 -> 1 on the LIF
// pair (6 MHz / IF 1620 or 8 MHz / IF 2048); single-tuner zero IF 96000/192000/384000/768000 -> 32/16/8/4 at
// 3.072 MS/s. No SDRplay API is loaded. UNPROVEN on a real RSPduo.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "source/sdrplay_rate_map.hpp"
#include "test_check.hpp"

using namespace cascade::source;
namespace abi = sdrplay_abi;

namespace {

struct Lit { double out; unsigned dec; };
const Lit kLifLit[] = {{62500, 32}, {125000, 16}, {250000, 8}, {500000, 4}, {1000000, 2}, {2000000, 1}};

// the stub of sdrplay_api_Init's LIF check, literally from the specification, section 3.15
bool specLif(int fs, int bw, int ifk) {
    return (fs == 8192000 && bw == 1536 && ifk == 2048) || (fs == 8000000 && bw == 1536 && ifk == 2048) ||
           (fs == 8000000 && bw == 5000 && ifk == 2048) || (fs == 2000000 && bw <= 300 && ifk == 450) ||
           (fs == 2000000 && bw == 600 && ifk == 450) || (fs == 6000000 && bw <= 1536 && ifk == 1620);
}

void testBandwidth() {
    CHECK(bandwidthForOutputRate(62500) == abi::BW_0_200);
    CHECK(bandwidthForOutputRate(299999) == abi::BW_0_200);
    CHECK(bandwidthForOutputRate(300000) == abi::BW_0_300);
    CHECK(bandwidthForOutputRate(599999) == abi::BW_0_300);
    CHECK(bandwidthForOutputRate(600000) == abi::BW_0_600);
    CHECK(bandwidthForOutputRate(1535999) == abi::BW_0_600);
    CHECK(bandwidthForOutputRate(1536000) == abi::BW_1_536);
    CHECK(bandwidthForOutputRate(4999999) == abi::BW_1_536);
    CHECK(bandwidthForOutputRate(5000000) == abi::BW_5_000);
    CHECK(bandwidthForOutputRate(6000000) == abi::BW_6_000);
    CHECK(bandwidthForOutputRate(7000000) == abi::BW_7_000);
    CHECK(bandwidthForOutputRate(8000000) == abi::BW_8_000);
    CHECK(bandwidthForOutputRate(10660000) == abi::BW_8_000);
    CHECK(bandwidthForOutputRate(0) == abi::BW_0_200);
}

void testLifRows() {
    for (const DuoMode m : {DuoMode::DualTuner, DuoMode::MasterSlave}) {
        for (const double fs : {6000000.0, 8000000.0}) {
            const auto rates = duoOutputRatesHz(m, fs);
            CHECK(rates.size() == 6);
            for (const Lit& l : kLifLit) {
                const DuoRateResult r = resolveDuoRate(m, fs, l.out);
                CHECK(r.ok && r.sentence.empty());
                const DuoRateSetting& s = r.setting;
                CHECK(s.decimation == l.dec);
                CHECK(s.decimationEnable == (l.dec != 1));
                CHECK(!s.wideBandSignal);
                CHECK(s.fsHz == fs);
                CHECK(s.ifType == (fs == 8000000.0 ? abi::IF_2_048 : abi::IF_1_620));
                CHECK(s.outputHz == l.out);
                // LIF delivers 2 MS/s before decimation (SoapySDRPlay3 getSampleRate)
                CHECK(std::fabs(2000000.0 / s.decimation - l.out) < 0.001);
                CHECK(s.soapyBw == bandwidthForOutputRate(l.out));
                // every pair this map produces is one the specification's down-conversion check accepts
                CHECK(specLif(static_cast<int>(s.fsHz), static_cast<int>(s.bw), static_cast<int>(s.ifType)));
                // and agrees with the earlier lane's per-document rows at decimation 1
                DualTunerRate want{s.fsHz, s.bw, s.ifType, 1};
                CHECK(checkDuoRate(m, want).ok);
                CHECK(validateDuoSetting(m, fs, s).ok);
            }
        }
    }
    // the 8 MHz departure from SoapySDRPlay3: 1.536 MHz everywhere, soapyBw keeps its own choice
    const DuoRateResult e = resolveDuoRate(DuoMode::MasterSlave, 8000000.0, 62500);
    CHECK(e.ok && e.setting.bw == abi::BW_1_536 && e.setting.soapyBw == abi::BW_0_200);
    const DuoRateResult s6 = resolveDuoRate(DuoMode::MasterSlave, 6000000.0, 62500);
    CHECK(s6.ok && s6.setting.bw == abi::BW_0_200 && s6.setting.bw == s6.setting.soapyBw);
    // 2 MS/s at 6 MHz: 1.536 MHz
    CHECK(resolveDuoRate(DuoMode::DualTuner, 6000000.0, 2000000).setting.bw == abi::BW_1_536);
}

void testTwoTunerRefusals() {
    for (const DuoMode m : {DuoMode::DualTuner, DuoMode::MasterSlave}) {
        for (const double bad : {96000.0, 192000.0, 384000.0, 768000.0, 2048000.0, 3000000.0, 10000000.0, 61000.0, 63000.0}) {
            const DuoRateResult r = resolveDuoRate(m, 6000000.0, bad);
            CHECK(!r.ok && !r.sentence.empty());
            CHECK(r.sentence.find(duoModeName(m)) != std::string::npos);
        }
        // ADC rate that is not 6 or 8 MHz
        for (const double fs : {0.0, 7000000.0, 8192000.0, -6000000.0, 2000000.0}) {
            CHECK(duoOutputRatesHz(m, fs).empty());
            CHECK(!resolveDuoRate(m, fs, 1000000).ok);
        }
    }
    // not-a-number, zero, negative, infinite
    for (const double bad : {0.0, -1.0, std::nan(""), HUGE_VAL, -HUGE_VAL}) {
        CHECK(!resolveDuoRate(DuoMode::SingleTuner, 0.0, bad).ok);
        CHECK(!resolveDuoRate(DuoMode::DualTuner, 6000000.0, bad).ok);
    }
}

void testSingleTuner() {
    const auto rates = duoOutputRatesHz(DuoMode::SingleTuner);
    CHECK(rates.size() == 19 && rates.front() == 62500.0 && rates.back() == 10000000.0);
    for (size_t i = 1; i < rates.size(); ++i) { CHECK(rates[i] > rates[i - 1]); }
    // every listed rate resolves
    for (const double r : rates) { CHECK(resolveDuoRate(DuoMode::SingleTuner, 0.0, r).ok); }
    // LIF rates in single tuner mode use the 6 MHz pair whatever masterFsHz says
    for (const Lit& l : kLifLit) {
        const DuoRateResult r = resolveDuoRate(DuoMode::SingleTuner, 8000000.0, l.out);
        CHECK(r.ok && r.setting.fsHz == 6000000.0 && r.setting.ifType == abi::IF_1_620 && r.setting.decimation == l.dec);
        CHECK(r.setting.bw == r.setting.soapyBw);
    }
    // decimated zero IF
    const Lit zl[] = {{96000, 32}, {192000, 16}, {384000, 8}, {768000, 4}};
    for (const Lit& l : zl) {
        const DuoRateResult r = resolveDuoRate(DuoMode::SingleTuner, 0.0, l.out);
        CHECK(r.ok && r.setting.ifType == abi::IF_Zero && r.setting.decimation == l.dec && r.setting.decimationEnable);
        CHECK(r.setting.wideBandSignal && r.setting.fsHz == 3072000.0);
        CHECK(r.setting.outputHz * r.setting.decimation == r.setting.fsHz);
        // the earlier lane's rule for single-tuner zero IF accepts this ADC rate
        CHECK(checkDuoRate(DuoMode::SingleTuner, DualTunerRate{r.setting.fsHz, r.setting.bw, r.setting.ifType, r.setting.decimation}).ok);
    }
    // above 2 MS/s: no decimation, ADC = output, boundary values
    for (const double o : {2000001.0, 2048000.0, 5500000.0, 10660000.0}) {
        const DuoRateResult r = resolveDuoRate(DuoMode::SingleTuner, 0.0, o);
        CHECK(r.ok && r.setting.fsHz == o && r.setting.decimation == 1 && !r.setting.decimationEnable && r.setting.wideBandSignal);
        CHECK(checkDuoRate(DuoMode::SingleTuner, DualTunerRate{r.setting.fsHz, r.setting.bw, r.setting.ifType, 1}).ok);
    }
    CHECK(!resolveDuoRate(DuoMode::SingleTuner, 0.0, 10660001.0).ok);
    CHECK(resolveDuoRate(DuoMode::SingleTuner, 0.0, 10660001.0).sentence.find("10.66") != std::string::npos);
    // odd rates below 2 MS/s are refused (SoapySDRPlay3 returns -1 for them)
    for (const double o : {1500000.0, 100000.0, 1.0, 1999999.0, 48000.0}) { CHECK(!resolveDuoRate(DuoMode::SingleTuner, 0.0, o).ok); }
    // whole-Hz tolerance of half a hertz
    CHECK(resolveDuoRate(DuoMode::SingleTuner, 0.0, 96000.4).ok);
    CHECK(!resolveDuoRate(DuoMode::SingleTuner, 0.0, 96001.0).ok);
}

void testValidate() {
    const DuoRateSetting good = resolveDuoRate(DuoMode::MasterSlave, 6000000.0, 250000).setting;
    CHECK(validateDuoSetting(DuoMode::MasterSlave, 6000000.0, good).ok);
    auto bad = [&](auto mutate, const char* word) {
        DuoRateSetting s = good;
        mutate(s);
        const DuoRateResult r = validateDuoSetting(DuoMode::MasterSlave, 6000000.0, s);
        CHECK(!r.ok);
        CHECK(r.sentence.find(word) != std::string::npos);
    };
    bad([](DuoRateSetting& s) { s.fsHz = 8000000.0; }, "ADC rate");
    bad([](DuoRateSetting& s) { s.ifType = abi::IF_Zero; }, "IF type");
    bad([](DuoRateSetting& s) { s.decimation = 4; }, "decimation");
    bad([](DuoRateSetting& s) { s.decimationEnable = false; }, "decimation");
    bad([](DuoRateSetting& s) { s.wideBandSignal = true; }, "wide-band");
    bad([](DuoRateSetting& s) { s.bw = abi::BW_5_000; }, "bandwidth");
    bad([](DuoRateSetting& s) { s.outputHz = 96000.0; }, "master/slave");
    // a stored 8 MHz setting is not valid against a 6 MHz master
    const DuoRateSetting e8 = resolveDuoRate(DuoMode::DualTuner, 8000000.0, 500000).setting;
    CHECK(validateDuoSetting(DuoMode::DualTuner, 8000000.0, e8).ok);
    CHECK(!validateDuoSetting(DuoMode::DualTuner, 6000000.0, e8).ok);
    // the narrower 8 MHz bandwidth SoapySDRPlay3 sets is NOT accepted by the map (the documents do not agree to it)
    DuoRateSetting soapy8 = e8;
    soapy8.bw = e8.soapyBw;
    CHECK(soapy8.bw != e8.bw && !validateDuoSetting(DuoMode::DualTuner, 8000000.0, soapy8).ok);
}

void testFuzz() {
    std::uint64_t x = 0x9E3779B97F4A7C15ull;
    auto next = [&x]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
    int ok = 0, refused = 0;
    for (int i = 0; i < 20000; ++i) {
        const DuoMode m = static_cast<DuoMode>(next() % 3);
        const double fs = (next() % 3 == 0) ? static_cast<double>(next() % 12000000) : (next() % 2 ? 6000000.0 : 8000000.0);
        const double out = (next() % 4 == 0) ? static_cast<double>(next() % 12000000) : kLifLit[next() % 6].out;
        const DuoRateResult r = resolveDuoRate(m, fs, out);
        if (r.ok) {
            ++ok;
            CHECK(r.sentence.empty() && r.setting.decimation >= 1 && r.setting.decimation <= 32);
            CHECK(r.setting.decimationEnable == (r.setting.decimation != 1));
            CHECK(validateDuoSetting(m, fs, r.setting).ok);
        } else {
            ++refused;
            CHECK(!r.sentence.empty());
        }
    }
    CHECK(ok > 1000 && refused > 1000);
    std::printf("rate-map fuzz: %d resolved, %d refused of 20000\n", ok, refused);
}

}  // namespace

int main() {
    testBandwidth();
    testLifRows();
    testTwoTunerRefusals();
    testSingleTuner();
    testValidate();
    testFuzz();
    return testSummary("test_sdrplay_rate_map");
}
