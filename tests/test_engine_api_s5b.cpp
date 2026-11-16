// test_engine_api_s5b.cpp - SCREEN LANE S5b's SOAPY CONTROLS AGAINST THE REAL ENGINE (wave 8; docs/multi-radio-records/screen-s5b.md): the rows the Source section draws (src/slint/s5_rows.cpp + s5b_rows.cpp, the very code the
// window runs) are driven with the real engine behind them, and the engine opens a SoapySource on the in-process stand-in driver of tests/soapy_standin.hpp (the engine's soapyScan hook lists it; the rest is the real
// scan list, the real open worker, the real SoapySource and its device lock). Every assertion is about what the STAND-IN DRIVER received (its log) or what the engine's facts say afterwards, never about what the page stored.
//   A  the facts: the driver's bandwidth range and its seven settings reach radio.N.soapy.*; a radio on the generator has none and the page says why
//   B  the controls: the bandwidth, a tick, a choice, a number and a text reach the driver as writeSetting/setBandwidth calls with the right text; the driver's rounding is read back; the driver's refusal and a value
//      the page's own rules refuse are shown as refused and the driver is never asked for the second
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "net_test_support.hpp"
#include "wire1_rig.hpp"
#include "soapy_standin.hpp"

#include "slint/lanek_logic.hpp"
#include "slint/s5_logic.hpp"
#include "slint/s5_rows.hpp"

namespace {

namespace s5 = foxsdr::slints5;
namespace slk = foxsdr::slintlanek;

std::vector<cascade::source::SoapyDeviceInfo> standInScan() {
    cascade::source::SoapyDeviceInfo d;
    d.label = "Stand-in SoapySDR driver";
    d.args = "driver=standinrich";
    return {d};
}

// The Source panel's rows wired to one real engine: a command leaves through the rig exactly as the window's host sends it, the engine's answer comes back through onAnswer, the facts are read from the engine.
struct Panel {
    Rig& r;
    int selected = 1;
    s5::S5Rows rows;
    std::vector<FoxCommand> sentCmds;
    std::vector<FoxCommandResult> answers;      // the engine's answers, handed to the rows once the row's own act has returned (as the window's tick does)
    explicit Panel(Rig& rig) : r(rig), rows(env()) {}
    s5::S5Rows::Env env() {
        s5::S5Rows::Env e;
        e.submit = [this](const FoxCommand& cin, std::uint32_t radio, const std::string*) {
            FoxCommand c = cin;
            if (radio != 0) { c.target = radio; }
            sentCmds.push_back(c);
            const testing::Outcome o = r.send(c);
            s5::SubmitOutcome out;
            out.status = o.submitStatus;
            if (!o.landed) { return out; }
            out.ticket = o.result.ticket;
            out.status = FOXAPI_OK;                    // the engine took it; its verdict comes back as the answer
            answers.push_back(o.result);
            return out;
        };
        e.selectedRadio = [this] { return selected; };
        e.fact = [this](const std::string& name, std::string& v) {
            const std::string f = r.fact(name.rfind("fact.", 0) == 0 ? name.substr(5) : name);
            if (f == "<absent>") { return false; }
            v = f;
            return true;
        };
        return e;
    }
    bool act(const std::string& id, const std::string& t) {
        const bool ok = rows.act(id, t);
        for (const FoxCommandResult& a : answers) { rows.onAnswer(a); }
        answers.clear();
        return ok;
    }
    std::vector<slk::Row> build() { return rows.build(); }
    void open() { build(); rows.act("s.more", ""); build(); }
    const slk::Row* find(const std::vector<slk::Row>& v, const std::string& id) const { for (const auto& x : v) { if (x.id == id) { return &x; } } return nullptr; }
};

// Opens radio 1 on the stand-in driver the way the Source section does: scan, then select the scanned row.
bool openStandIn(Rig& r) {
    r.pump(10);
    Access::setSoapyScan(&standInScan);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SCAN_DEVICES)).status(), FOXAPI_OK);
    r.until([&] { return false; }, 1500);   // the scan runs on a worker
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, "soapy:driver=standinrich")).status(), FOXAPI_OK);
    return r.until([&] { return r.fact("radio.1.soapy.count") == "7"; }, 30000);
}

void testA() {
    std::printf("A: the driver's bandwidth and settings become facts; a radio on the generator has none\n");
    {
        Rig r;
        r.pump(10);
        Panel p(r);
        p.open();
        const std::vector<slk::Row> v = p.build();
        CHECK_EQ(r.fact("radio.1.soapy.count"), std::string("<absent>"));
        CHECK(p.find(v, "b.sp.bw.set") == nullptr && p.find(v, "b.sp.0") == nullptr);
        bool reason = false;
        for (const auto& x : v) { if (x.label.find("names no bandwidth control") != std::string::npos) { reason = true; } }
        CHECK(reason);
    }
    {
        Rig r;
        CHECK(openStandIn(r));
        CHECK_EQ(r.fact("radio.1.soapy.bandwidth"), std::string("1000000"));
        CHECK_EQ(r.fact("radio.1.soapy.bandwidthLo"), std::string("200000"));
        CHECK_EQ(r.fact("radio.1.soapy.bandwidthHi"), std::string("8000000"));
        CHECK_EQ(r.fact("radio.1.soapy.0.key"), std::string("biastee"));
        CHECK_EQ(r.fact("radio.1.soapy.0.type"), std::string("bool"));
        CHECK_EQ(r.fact("radio.1.soapy.1.min"), std::string("-100"));
        CHECK_EQ(r.fact("radio.1.soapy.3.options"), std::string("internal|external|gpsdo"));
        CHECK_EQ(r.fact("radio.1.soapy.3.value"), std::string("internal"));
        Panel p(r);
        p.open();
        const std::vector<slk::Row> v = p.build();
        CHECK(p.find(v, "b.sp.bw.set") != nullptr && p.find(v, "b.sp.0") != nullptr && p.find(v, "b.sp.6") != nullptr && p.find(v, "b.sp.7") == nullptr);
        CHECK(p.find(v, "b.sp.3") != nullptr && p.find(v, "b.sp.3")->options.size() == 3 && p.find(v, "b.sp.3")->pick == 0);   // the driver's own value at start-up
    }
}

void testB() {
    std::printf("B: each control reaches the driver; a refusal is shown\n");
    Rig r;
    CHECK(openStandIn(r));
    Panel p(r);
    p.open();
    soapystandin::g_log.clear();
    // bandwidth: the driver rounds a small request up and says so
    p.act("b.sp.bw", "250");
    p.act("b.sp.bw.set", "");
    CHECK_EQ(p.rows.refused(), 0L);
    CHECK_EQ(soapystandin::g_log.last("setBandwidth"), std::string("setBandwidth 250000"));
    CHECK(r.until([&] { return r.fact("radio.1.soapy.bandwidth") == "300000"; }, 10000));
    CHECK(p.rows.note().find("the driver now has") != std::string::npos && p.rows.note().find("300 kHz") != std::string::npos);
    std::printf("  bandwidth: %s\n", p.rows.note().c_str());
    // outside the driver's range: refused by the page, the driver is not asked
    const std::size_t bwCalls = soapystandin::g_log.count("setBandwidth");
    p.act("b.sp.bw", "50");
    p.act("b.sp.bw.set", "");
    CHECK_EQ(soapystandin::g_log.count("setBandwidth"), bwCalls);
    // a tick, a choice, a number, a text
    p.act("b.sp.0", "1");
    CHECK_EQ(soapystandin::g_log.last("writeSetting"), std::string("writeSetting biastee=true"));
    CHECK(r.until([&] { return r.fact("radio.1.soapy.0.value") == "true"; }, 10000));
    p.act("b.sp.3", "1");
    CHECK_EQ(soapystandin::g_log.last("writeSetting"), std::string("writeSetting clock=external"));
    CHECK(r.until([&] { return r.fact("radio.1.soapy.3.value") == "external"; }, 10000));
    p.act("b.sp.1", "-40");
    p.act("b.sp.1.set", "");
    CHECK_EQ(soapystandin::g_log.last("writeSetting"), std::string("writeSetting offset=-40"));
    CHECK(r.until([&] { return r.fact("radio.1.soapy.1.value") == "-40"; }, 10000));
    p.act("b.sp.4", "my antenna");
    p.act("b.sp.4.set", "");
    CHECK_EQ(soapystandin::g_log.last("writeSetting"), std::string("writeSetting label=my antenna"));
    // the driver rounds a value: the fact is the driver's, not the request
    p.act("b.sp.6", "99");
    p.act("b.sp.6.set", "");
    CHECK(r.until([&] { return r.fact("radio.1.soapy.6.value") == "10"; }, 10000));
    CHECK(p.rows.note().find("the driver now has 10") != std::string::npos);
    // the driver refuses: shown as refused with its words, the value unchanged
    p.act("b.sp.5", "2");
    p.act("b.sp.5.set", "");
    CHECK(p.rows.note().find("will not change that while a stream is running") != std::string::npos);
    std::printf("  refused: %s\n", p.rows.note().c_str());
    CHECK_EQ(r.fact("radio.1.soapy.5.value"), std::string("1"));
    // a value outside the driver's own range never reaches it
    const std::size_t writes = soapystandin::g_log.count("writeSetting");
    p.act("b.sp.1", "500");
    p.act("b.sp.1.set", "");
    CHECK_EQ(soapystandin::g_log.count("writeSetting"), writes);
    // a command that bypasses the page is judged by the driver's own list as well: an unknown name and a value the driver does not accept are refused by the engine
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_DEVICE_OPTION, 0.0, 0, "soapy:nokey=1")).status(), FOXAPI_FAILED);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_DEVICE_OPTION, 0.0, 0, "soapy:clock=atomic")).status(), FOXAPI_FAILED);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_DEVICE_OPTION, 0.0, 0, "soapy:noequals")).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_DEVICE_OPTION, -5.0, 0, "soapy_bandwidth")).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(soapystandin::g_log.count("writeSetting"), writes);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_s5b\n");
    engine_api_test::hermetic("s5b");
    Access::installHooks();
    SoapySDR::Registry regRich("standinrich", &soapystandin::findRich, &soapystandin::makeRich, SOAPY_SDR_ABI_VERSION);
    testA();
    testB();
    return check::finish("test_engine_api_s5b");
}
