// test_sdrplay_dual_tuner.cpp - the source-level RSPduo dual-tuner decisions (R9) over an injected device
// list of hand-built sdrplay_abi::DeviceT records. No SDRplay API is loaded, no service is touched.
// UNPROVEN on a real RSPduo. The rate table the tests install is INVENTED by the test: the specification
// does not give the dual-tuner table (see src/source/sdrplay_dual_tuner.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "source/sdrplay_dual_tuner.hpp"
#include "src_test_net.hpp"
#include "test_check.hpp"

using namespace cascade::source;
namespace abi = sdrplay_abi;

namespace {

abi::DeviceT dev(const char* serial, unsigned char hw, int modes, int tuners, unsigned char valid = 1) {
    abi::DeviceT d{};
    std::snprintf(d.SerNo, sizeof(d.SerNo), "%s", serial);
    d.hwVer = hw;
    d.rspDuoMode = static_cast<abi::RspDuoModeT>(modes);
    d.tuner = static_cast<abi::TunerSelectT>(tuners);
    d.valid = valid;
    return d;
}

class FakeList : public DuoDeviceList {
public:
    std::vector<abi::DeviceT> devices;
    bool fail = false;
    std::string failText = "the SDRplay service is not running";
    float version = 3.15f;
    bool getDevices(std::vector<abi::DeviceT>& out, std::string& error) override {
        if (fail) { error = failText; return false; }
        out = devices;
        return true;
    }
    float apiVersion() const override { return version; }
};

constexpr int kAll = abi::RspDuoMode_Single_Tuner | abi::RspDuoMode_Dual_Tuner | abi::RspDuoMode_Master;

void testAssess() {
    // a free RSPduo: every mode, both tuners (the fake API's own definition of "free")
    DualTunerOffer o = assessDualTuner(dev("1234567890", abi::kRspDuo, kAll, abi::Tuner_Both), 3.15f);
    CHECK(o.status == DualTunerStatus::Available);
    CHECK(o.tuners.size() == 2 && o.tuners[0].tuner == abi::Tuner_A && o.tuners[1].tuner == abi::Tuner_B);
    CHECK(o.tuners[0].label == "Tuner 1 (A)" && o.tuners[1].label == "Tuner 2 (B)");
    CHECK(o.label == "SDRplay RSPduo (serial ...7890)" && o.label.find("1234567890") == std::string::npos);
    // not an RSPduo, whatever its flags
    for (const unsigned char hw : {abi::kRsp1, abi::kRsp2, abi::kRsp1A, abi::kRsp1B, abi::kRspDx, abi::kRspDxR2, static_cast<unsigned char>(99)}) {
        o = assessDualTuner(dev("X", hw, kAll, abi::Tuner_Both), 3.15f);
        CHECK(o.status == DualTunerStatus::NotAnRspDuo && o.tuners.empty() && o.sentence.find("Only the RSPduo") == 0);
    }
    // only slave offered: another program is the master
    o = assessDualTuner(dev("S", abi::kRspDuo, abi::RspDuoMode_Slave, abi::Tuner_A), 3.15f);
    CHECK(o.status == DualTunerStatus::InUseAsMaster && o.sentence.find("master") != std::string::npos && o.tuners.empty());
    // no dual-tuner flag and nothing says master
    o = assessDualTuner(dev("S", abi::kRspDuo, abi::RspDuoMode_Single_Tuner, abi::Tuner_Both), 3.15f);
    CHECK(o.status == DualTunerStatus::ModeNotOffered);
    o = assessDualTuner(dev("S", abi::kRspDuo, abi::RspDuoMode_Unknown, abi::Tuner_Both), 3.15f);
    CHECK(o.status == DualTunerStatus::ModeNotOffered);
    // one tuner busy, either one, or neither
    o = assessDualTuner(dev("S", abi::kRspDuo, kAll, abi::Tuner_A), 3.15f);
    CHECK(o.status == DualTunerStatus::TunerNotFree && o.sentence.find("tuner A is free") != std::string::npos &&
          o.sentence.find("tuner B is in use") != std::string::npos);
    o = assessDualTuner(dev("S", abi::kRspDuo, kAll, abi::Tuner_B), 3.15f);
    CHECK(o.status == DualTunerStatus::TunerNotFree && o.sentence.find("tuner A is in use") != std::string::npos);
    o = assessDualTuner(dev("S", abi::kRspDuo, kAll, abi::Tuner_Neither), 3.15f);
    CHECK(o.status == DualTunerStatus::TunerNotFree);
    // `valid` is honoured from 3.08 and ignored below (as sdrplay_api_decl.hpp says)
    o = assessDualTuner(dev("S", abi::kRspDuo, kAll, abi::Tuner_Both, 0), 3.15f);
    CHECK(o.status == DualTunerStatus::NotValid);
    o = assessDualTuner(dev("S", abi::kRspDuo, kAll, abi::Tuner_Both, 0), 3.07f);
    CHECK(o.status == DualTunerStatus::Available);
    o = assessDualTuner(dev("S", abi::kRspDuo, kAll, abi::Tuner_Both, 0), 3.08f);
    CHECK(o.status == DualTunerStatus::NotValid);
    // a serial with no terminator inside the array is bounded, a short one is not shown
    abi::DeviceT full = dev("", abi::kRspDuo, kAll, abi::Tuner_Both);
    std::memset(full.SerNo, 'A', sizeof(full.SerNo));
    o = assessDualTuner(full, 3.15f);
    CHECK(o.status == DualTunerStatus::Available && o.label == "SDRplay RSPduo (serial ...AAAA)");
    CHECK(assessDualTuner(dev("12", abi::kRspDuo, kAll, abi::Tuner_Both), 3.15f).label == "SDRplay RSPduo");
}

void testEnumerate() {
    FakeList list;
    list.devices = {dev("A1", abi::kRsp1A, 0, abi::Tuner_A), dev("DUO11111", abi::kRspDuo, kAll, abi::Tuner_Both),
                    dev("DUO22222", abi::kRspDuo, abi::RspDuoMode_Slave, abi::Tuner_A), dev("DX", abi::kRspDx, 0, abi::Tuner_A)};
    std::vector<DualTunerOffer> offers;
    std::string err;
    CHECK(enumerateDualTuner(list, offers, err));
    CHECK(offers.size() == 2);  // the RSP1A and the RSPdx are skipped, not refused
    CHECK(offers[0].status == DualTunerStatus::Available && offers[1].status == DualTunerStatus::InUseAsMaster);
    list.devices.clear();
    CHECK(enumerateDualTuner(list, offers, err) && offers.empty());
    list.fail = true;
    CHECK(!enumerateDualTuner(list, offers, err) && err == "the SDRplay service is not running" && offers.empty());
    list.failText = "";
    CHECK(!enumerateDualTuner(list, offers, err) && !err.empty());
    // 50 enumerations
    list.fail = false;
    list.devices = {dev("DUO11111", abi::kRspDuo, kAll, abi::Tuner_Both)};
    for (int i = 0; i < 50; ++i) { CHECK(enumerateDualTuner(list, offers, err) && offers.size() == 1); }
}

void testPrepare() {
    const DualTunerOffer ok = assessDualTuner(dev("DUO11111", abi::kRspDuo, kAll, abi::Tuner_Both), 3.15f);
    abi::DeviceT out{};
    CHECK(prepareDualTunerSelect(ok, out));
    CHECK(out.rspDuoMode == abi::RspDuoMode_Dual_Tuner && out.tuner == abi::Tuner_Both && out.hwVer == abi::kRspDuo);
    CHECK(std::strcmp(out.SerNo, "DUO11111") == 0);
    const DualTunerOffer bad = assessDualTuner(dev("D", abi::kRspDuo, kAll, abi::Tuner_A), 3.15f);
    abi::DeviceT untouched = dev("KEEP", abi::kRsp1A, 0, abi::Tuner_Neither);
    CHECK(!prepareDualTunerSelect(bad, untouched));
    CHECK(std::strcmp(untouched.SerNo, "KEEP") == 0 && untouched.hwVer == abi::kRsp1A);
    const auto ms = masterSlaveRatesHz();
    CHECK(ms.size() == 2 && ms[0] == 6000000.0 && ms[1] == 8000000.0);
}

void testRateTable() {
    DualTunerRateTable t;
    CHECK(!t.known() && t.allowed().empty() && t.outputRatesHz().empty());
    DualTunerRate want;
    want.fsHz = 6000000.0;
    want.bw = abi::BW_1_536;
    want.ifType = abi::IF_Zero;
    // EMPTY BY DEFAULT: every dual-tuner rate is refused, and the sentence says the table is unknown
    DualTunerRateCheck c = t.check(want);
    CHECK(!c.ok && !c.tableKnown && c.sentence.find("not known to this build") != std::string::npos);
    // a table the TEST invents, to prove the checking logic
    t.setTable({{2000000.0, abi::BW_0_600, abi::IF_Zero, 1}, {2000000.0, abi::BW_0_600, abi::IF_Zero, 2}, {6000000.0, abi::BW_1_536, abi::IF_Zero, 1}});
    CHECK(t.known() && t.allowed().size() == 3);
    CHECK(t.check(want).ok);
    want.fsHz = 6000000.4;
    CHECK(t.check(want).ok);  // within 1 Hz
    want.fsHz = 6000002.0;
    c = t.check(want);
    CHECK(!c.ok && c.tableKnown && c.sentence.find("does not allow") != std::string::npos);
    want.fsHz = 6000000.0;
    want.bw = abi::BW_5_000;
    CHECK(!t.check(want).ok);
    want.bw = abi::BW_1_536;
    want.ifType = abi::IF_1_620;
    CHECK(!t.check(want).ok);
    want.ifType = abi::IF_Zero;
    want.decimation = 4;
    CHECK(!t.check(want).ok);
    want.decimation = 0;
    CHECK(!t.check(want).ok);
    want.decimation = 1;
    want.fsHz = std::nan("");
    CHECK(!t.check(want).ok);
    const auto rates = t.outputRatesHz();
    CHECK(rates.size() == 3 && rates[0] == 1000000.0 && rates[1] == 2000000.0 && rates[2] == 6000000.0);
    t.setTable({});
    CHECK(t.known() && !t.check(DualTunerRate{}).ok);  // a known, empty table allows nothing and says so differently
}

void testFuzz() {
    srctest::Rng rng(0xD00D5u);
    int available = 0;
    for (int i = 0; i < 10000; ++i) {
        abi::DeviceT d{};
        for (std::size_t k = 0; k < sizeof(d.SerNo); ++k) { d.SerNo[k] = static_cast<char>(rng.next()); }
        d.hwVer = static_cast<unsigned char>(rng.next() % 3 == 0 ? abi::kRspDuo : rng.next());
        d.rspDuoMode = static_cast<abi::RspDuoModeT>(rng.next() % 17);
        d.tuner = static_cast<abi::TunerSelectT>(rng.next() % 5);
        d.valid = static_cast<unsigned char>(rng.next());
        const float v = static_cast<float>(rng.next() % 400) / 100.0f;
        const DualTunerOffer o = assessDualTuner(d, v);
        if (o.status == DualTunerStatus::Available) {
            ++available;
            // the invariants of an offer
            CHECK(d.hwVer == abi::kRspDuo && (static_cast<int>(d.rspDuoMode) & abi::RspDuoMode_Dual_Tuner) &&
                  (static_cast<int>(d.tuner) & 3) == 3 && o.tuners.size() == 2);
            abi::DeviceT out{};
            CHECK(prepareDualTunerSelect(o, out) && out.rspDuoMode == abi::RspDuoMode_Dual_Tuner);
        } else {
            CHECK(o.tuners.empty() && !o.sentence.empty());
        }
    }
    CHECK(available > 100);
}

}  // namespace

int main() {
    testAssess();
    testEnumerate();
    testPrepare();
    testRateTable();
    testFuzz();
    return testSummary("test_sdrplay_dual_tuner");
}
