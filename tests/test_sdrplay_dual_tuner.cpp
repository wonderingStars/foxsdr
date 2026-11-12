// test_sdrplay_dual_tuner.cpp - the source-level RSPduo dual-tuner decisions (R9) over an injected device
// list of hand-built sdrplay_abi::DeviceT records. No SDRplay API is loaded, no service is touched.
// UNPROVEN on a real RSPduo. testRateTable installs a table INVENTED by the test to prove the checking logic;
// testDocumentedRates (follow-up lane) proves the rate rules built from SDRplay's documents (header comment of
// src/source/sdrplay_dual_tuner.hpp) against a STUB of the API's low-IF check written literally from the
// specification's section 3.15, and an independently written expectation over a grid of settings.
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

// A stub of what sdrplay_api_Init checks, from the specification, section 3.15: "Conditions for LIF
// down-conversion to be enabled for all RSPs in single tuner mode" (the six lines, bandwidth and IF as the
// enumerations' numbers: BW_0_300 = 300, IF_0_450 = 450 ...).
bool specLifDownConversion(double fs, int bw, int ifk) {
    return (fs == 8192000 && bw == 1536 && ifk == 2048) || (fs == 8000000 && bw == 1536 && ifk == 2048) ||
           (fs == 8000000 && bw == 5000 && ifk == 2048) || (fs == 2000000 && bw <= 300 && ifk == 450) ||
           (fs == 2000000 && bw == 600 && ifk == 450) || (fs == 6000000 && bw <= 1536 && ifk == 1620);
}

bool bwDefined(int bw) { return bw == 200 || bw == 300 || bw == 600 || bw == 1536 || bw == 5000 || bw == 6000 || bw == 7000 || bw == 8000; }

// The expectation, written again from the documents (not from the code): single tuner = zero IF 2 - 10.66 MHz
// with a defined bandwidth, or a low-IF row of the specification; dual tuner and master/slave = both tuners in
// low IF at 6 or 8 MHz, bandwidth <= 1.536 MHz, and a combination the specification lists; decimation: 1 only,
// except single tuner where 1, 2, 4, 8, 16, 32 are accepted (the shipping driver's).
bool expectedOk(DuoMode m, double fs, int bw, int ifk, unsigned dec) {
    if (!(fs > 0.0) || dec == 0) { return false; }
    if (m == DuoMode::SingleTuner) {
        if (!(dec == 1 || dec == 2 || dec == 4 || dec == 8 || dec == 16 || dec == 32)) { return false; }
        if (ifk == 0) { return bwDefined(bw) && fs >= 2000000.0 && fs <= 10660000.0; }
        return bwDefined(bw) && specLifDownConversion(fs, bw, ifk);
    }
    if (dec != 1) { return false; }
    return (fs == 6000000 || fs == 8000000) && bwDefined(bw) && bw <= 1536 && specLifDownConversion(fs, bw, ifk);
}

// The code allows 1 Hz of slack on fs; the expectation is exact, so a value within 1 Hz of a whole kHz is
// snapped to it before the comparison.
double snapFs(double fs) {
    const double k = std::round(fs / 1000.0) * 1000.0;
    return std::fabs(fs - k) < 1.0 ? k : fs;
}

void testDocumentedRates() {
    const DuoMode modes[3] = {DuoMode::SingleTuner, DuoMode::DualTuner, DuoMode::MasterSlave};
    CHECK(std::string(duoModeName(DuoMode::SingleTuner)) == "single tuner" && std::string(duoModeName(DuoMode::DualTuner)) == "dual tuner" &&
          std::string(duoModeName(DuoMode::MasterSlave)) == "master/slave");
    // every enumerated row passes the stub of the API's check, in every mode
    for (const DuoMode m : modes) {
        const auto rows = documentedLowIfRows(m);
        CHECK(!rows.empty());
        for (const DualTunerRate& r : rows) {
            CHECK(specLifDownConversion(r.fsHz, static_cast<int>(r.bw), static_cast<int>(r.ifType)) && r.decimation == 1);
            CHECK(checkDuoRate(m, r).ok);
        }
    }
    CHECK(documentedLowIfRows(DuoMode::DualTuner).size() == 5 && documentedLowIfRows(DuoMode::SingleTuner).size() == 10);
    {  // dual tuner and master/slave have the same rows; each is 6 or 8 MHz, <= 1.536 MHz
        const auto d = documentedLowIfRows(DuoMode::DualTuner), ms = documentedLowIfRows(DuoMode::MasterSlave);
        CHECK(d.size() == ms.size());
        for (std::size_t i = 0; i < d.size() && i < ms.size(); ++i) {
            CHECK(d[i].fsHz == ms[i].fsHz && d[i].bw == ms[i].bw && d[i].ifType == ms[i].ifType);
            CHECK((d[i].fsHz == 6000000.0 || d[i].fsHz == 8000000.0) && static_cast<int>(d[i].bw) <= 1536);
        }
    }
    {
        const auto o = documentedLowIfOutputRatesHz(DuoMode::DualTuner);
        CHECK(o.size() == 2 && o[0] == 6000000.0 && o[1] == 8000000.0);
        const auto s = documentedLowIfOutputRatesHz(DuoMode::SingleTuner);
        CHECK(s.size() == 4 && s[0] == 2000000.0 && s[1] == 6000000.0 && s[2] == 8000000.0 && s[3] == 8192000.0);
    }
    // the grid: every combination, every mode, against the independent expectation
    const double fss[] = {1.0e6, 2.0e6, 2.0e6 + 0.4, 3.0e6, 6.0e6, 6.0e6 + 0.4, 6.0e6 + 2.0, 8.0e6, 8.192e6, 9.0e6, 10.66e6, 10.7e6, 12.0e6};
    const int bws[] = {0, 200, 300, 600, 1536, 5000, 6000, 7000, 8000, 1234};
    const int ifs[] = {-1, 0, 450, 1620, 2048};
    const unsigned decs[] = {0, 1, 2, 3, 4, 16, 32, 64};
    int accepted = 0, total = 0;
    for (const DuoMode m : modes) {
        for (const double fs : fss) {
            for (const int bw : bws) {
                for (const int ifk : ifs) {
                    for (const unsigned dec : decs) {
                        DualTunerRate w;
                        w.fsHz = fs;
                        w.bw = static_cast<abi::BwMHzT>(bw);
                        w.ifType = static_cast<abi::IfKHzT>(ifk);
                        w.decimation = dec;
                        const DualTunerRateCheck c = checkDuoRate(m, w);
                        const bool want = expectedOk(m, snapFs(fs), bw, ifk, dec);
                        ++total;
                        if (c.ok) { ++accepted; }
                        if (c.ok != want) {
                            std::fprintf(stderr, "mismatch mode=%s fs=%.1f bw=%d if=%d dec=%u code=%d expected=%d", duoModeName(m), fs, bw, ifk, dec, c.ok ? 1 : 0, want ? 1 : 0);
                            std::fputc(10, stderr);
                        }
                        CHECK(c.ok == want);
                        CHECK(c.ok == c.sentence.empty() && c.tableKnown);
                    }
                }
            }
        }
    }
    CHECK(total == 3 * 13 * 10 * 5 * 8 && accepted > 100);
    {  // spot checks of the refusals by name
        DualTunerRate w{6000000.0, abi::BW_1_536, abi::IF_1_620, 1};
        CHECK(checkDuoRate(DuoMode::DualTuner, w).ok && checkDuoRate(DuoMode::MasterSlave, w).ok && checkDuoRate(DuoMode::SingleTuner, w).ok);
        w.decimation = 2;
        const DualTunerRateCheck c = checkDuoRate(DuoMode::DualTuner, w);
        CHECK(!c.ok && c.sentence.find("decimation") != std::string::npos && c.sentence.find("dual tuner") != std::string::npos);
        CHECK(checkDuoRate(DuoMode::SingleTuner, w).ok);  // the shipping driver's factor 2
        w.decimation = 3;
        CHECK(!checkDuoRate(DuoMode::SingleTuner, w).ok);
        DualTunerRate zif{6000000.0, abi::BW_5_000, abi::IF_Zero, 1};
        CHECK(checkDuoRate(DuoMode::SingleTuner, zif).ok);  // zero IF at 6 MHz, 5 MHz wide: single tuner only
        CHECK(!checkDuoRate(DuoMode::DualTuner, zif).ok && !checkDuoRate(DuoMode::MasterSlave, zif).ok);
        DualTunerRate wide{8000000.0, abi::BW_5_000, abi::IF_2_048, 1};
        CHECK(checkDuoRate(DuoMode::SingleTuner, wide).ok && !checkDuoRate(DuoMode::DualTuner, wide).ok);  // 5 MHz wide: not in dual
        CHECK(!checkDuoRate(DuoMode::DualTuner, DualTunerRate{2000000.0, abi::BW_0_600, abi::IF_0_450, 1}).ok);  // 2 MHz: not a dual rate
        CHECK(!checkDuoRate(DuoMode::SingleTuner, DualTunerRate{std::nan(""), abi::BW_0_600, abi::IF_Zero, 1}).ok);
        CHECK(!checkDuoRate(DuoMode::SingleTuner, DualTunerRate{-5.0, abi::BW_0_600, abi::IF_Zero, 1}).ok);
        CHECK(!checkDuoRate(DuoMode::DualTuner, DualTunerRate{6000000.0, abi::BW_1_536, abi::IF_1_620, 0}).ok);
    }
    // the dual-tuner table filled from the documents
    {
        const DualTunerRateTable t = documentedDualTunerTable();
        CHECK(t.known() && t.allowed().size() == 5);
        CHECK(!DualTunerRateTable().known());  // the bare table is still empty
        for (const DualTunerRate& r : t.allowed()) { CHECK(t.check(r).ok); }
        const auto o = t.outputRatesHz();
        CHECK(o.size() == 2 && o[0] == 6000000.0 && o[1] == 8000000.0);
        DualTunerRate no{6000000.0, abi::BW_1_536, abi::IF_2_048, 1};
        CHECK(!t.check(no).ok);
    }
    // the whole chain with stubs: device list -> assess -> prepare -> API init check for each documented row
    {
        FakeList list;
        list.devices.push_back(dev("1234567890", abi::kRspDuo, kAll, 3));
        std::vector<DualTunerOffer> offers;
        std::string err;
        CHECK(enumerateDualTuner(list, offers, err) && offers.size() == 1 && offers[0].status == DualTunerStatus::Available);
        abi::DeviceT sel{};
        CHECK(prepareDualTunerSelect(offers[0], sel) && sel.rspDuoMode == abi::RspDuoMode_Dual_Tuner && sel.tuner == abi::Tuner_Both);
        for (const DualTunerRate& r : documentedLowIfRows(DuoMode::DualTuner)) {
            // the stub of sdrplay_api_Init for a dual-tuner selection: the mode was prepared and the LIF check passes
            const bool initOk = sel.rspDuoMode == abi::RspDuoMode_Dual_Tuner && sel.tuner == abi::Tuner_Both &&
                                specLifDownConversion(r.fsHz, static_cast<int>(r.bw), static_cast<int>(r.ifType));
            CHECK(initOk && checkDuoRate(DuoMode::DualTuner, r).ok);
        }
    }
    // random settings: the rule and the expectation agree
    srctest::Rng rng(0x5EEDu);
    for (int i = 0; i < 20000; ++i) {
        const DuoMode m = modes[rng.next() % 3];
        const double fs = (rng.next() % 3 == 0) ? static_cast<double>(rng.next() % 12000000) : fss[rng.next() % 13];
        const int bw = bws[rng.next() % 10];
        const int ifk = ifs[rng.next() % 5];
        const unsigned dec = rng.next() % 70;
        DualTunerRate w{fs, static_cast<abi::BwMHzT>(bw), static_cast<abi::IfKHzT>(ifk), dec};
        CHECK(checkDuoRate(m, w).ok == expectedOk(m, snapFs(fs), bw, ifk, dec));
    }
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
    testDocumentedRates();
    testFuzz();
    return testSummary("test_sdrplay_dual_tuner");
}
