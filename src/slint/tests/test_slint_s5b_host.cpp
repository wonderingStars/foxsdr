// Screen lane S5b's pure rules and rows (src/slint/s5b_logic.hpp, s5b_rows.cpp; docs/multi-radio-records/screen-s5b.md), driven with a stand-in for the engine: the facts it would read are a table, and every
// command a control sends is RECORDED so each assertion is about what the engine would have been sent, not about what the page stored.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/app_commands.hpp"
#include "foxsdr_api.h"
#include "s5_logic.hpp"
#include "s5_rows.hpp"
#include "s5b_logic.hpp"
#include "test_check.hpp"

using namespace foxsdr::slints5;
namespace slk = foxsdr::slintlanek;
namespace b = foxsdr::slints5b;

namespace {

struct Sent { std::uint32_t op = 0; long long i0 = 0, i1 = 0; double n0 = 0, n1 = 0; std::string text; std::uint32_t radio = 0; };

struct Rig {
    std::map<std::string, std::string> facts;
    std::vector<Sent> sent;
    int selected = 1;
    int nextStatus = FOXAPI_OK;
    std::uint64_t ticket = 100;
    S5Rows rows;
    Rig() : rows(env()) {}
    S5Rows::Env env() {
        S5Rows::Env e;
        e.submit = [this](const FoxCommand& c, std::uint32_t radio, const std::string* lt) {
            Sent s; s.op = c.op; s.i0 = c.ival[0]; s.i1 = c.ival[1]; s.n0 = c.num[0]; s.n1 = c.num[1]; s.radio = radio;
            s.text = lt != nullptr ? *lt : std::string(c.text);
            sent.push_back(s);
            SubmitOutcome o; o.status = nextStatus; o.ticket = nextStatus == FOXAPI_OK ? ++ticket : 0;
            return o;
        };
        e.selectedRadio = [this] { return selected; };
        e.fact = [this](const std::string& name, std::string& v) {
            const auto it = facts.find(name);
            if (it == facts.end()) { return false; }
            v = it->second;
            return true;
        };
        return e;
    }
    std::vector<slk::Row> build() { return rows.build(); }
    const slk::Row* find(const std::vector<slk::Row>& v, const std::string& id) const { for (const auto& r : v) { if (r.id == id) { return &r; } } return nullptr; }
    bool has(const std::vector<slk::Row>& v, const std::string& label) const { for (const auto& r : v) { if (r.label == label || r.value == label) { return true; } } return false; }
    void answer(std::uint64_t t, int status, const char* msg = "", double a0 = 0, double a1 = 0) {
        FoxCommandResult r{}; r.structSize = sizeof(r); r.ticket = t; r.status = status; r.applied[0] = a0; r.applied[1] = a1;
        std::strncpy(r.message, msg, sizeof(r.message) - 1);
        rows.onAnswer(r);
    }
    void open() { build(); rows.act("s.more", ""); build(); }   // the panel is folded until asked for
};

}  // namespace

int main() {
    std::printf("test_slint_s5b_host\n");

    // ---------------------------------------------------------------- G1-14 analogue IF bandwidth: the numbers
    {
        long long hz = -1; std::string e;
        CHECK(b::parseIfBandwidthKHz("", hz, e) && hz == 0);
        CHECK(b::parseIfBandwidthKHz("0", hz, e) && hz == 0);
        CHECK(b::parseIfBandwidthKHz("400", hz, e) && hz == 400000);
        CHECK(b::parseIfBandwidthKHz("1536", hz, e) && hz == 1536000);
        CHECK(b::parseIfBandwidthKHz("8000", hz, e) && hz == 8000000);
        CHECK(b::parseIfBandwidthKHz("300", hz, e) && hz == 300000);
        for (const char* bad : {"299", "100", "8001", "-5", "wide", "1e9"}) { hz = -1; e.clear(); CHECK(!b::parseIfBandwidthKHz(bad, hz, e) && !e.empty()); }
        CHECK(b::ifBandwidthWords("0") == "Automatic (from the sample rate)");
        CHECK(b::ifBandwidthWords("400000") == "400 kHz");
        CHECK(b::ifBandwidthWords("").empty() && b::ifBandwidthWords("x").empty() && b::ifBandwidthWords("-1").empty());
    }

    // ---------------------------------------------------------------- the rows: analogue IF bandwidth
    {
        Rig r; r.open();
        std::vector<slk::Row> v = r.build();
        CHECK(r.find(v, "b.ifbw.set") == nullptr);                                   // a radio with no such filter shows no control, only the reason
        bool said = false; for (const auto& x : v) { if (x.label.find("no adjustable analogue filter") != std::string::npos) { said = true; } }
        CHECK(said);
        r.facts["fact.radio.1.ifBandwidth"] = "0";                                   // the engine's value at start-up: automatic
        v = r.build();
        CHECK(r.has(v, "Automatic (from the sample rate)") && r.find(v, "b.ifbw.set") != nullptr);
        CHECK(r.find(v, "b.ifbw.auto") != nullptr && !r.find(v, "b.ifbw.auto")->enabled);   // already automatic: nothing to switch back
        r.rows.act("b.ifbw", "500");
        r.rows.act("b.ifbw.set", "");
        CHECK(r.sent.size() == 1 && r.sent[0].op == FOXAPI_OP_SET_DEVICE_OPTION && r.sent[0].text == "if_bandwidth" && r.sent[0].i0 == 500000);
        r.answer(101, FOXAPI_FAILED, "the radio is busy; try again");               // a refusal is shown in the radio's words
        CHECK(r.rows.note().find("the radio is busy") != std::string::npos);
        r.facts["fact.radio.1.ifBandwidth"] = "500000";                              // read back after a change (or one made elsewhere)
        v = r.build();
        CHECK(r.has(v, "500 kHz") && r.find(v, "b.ifbw.auto")->enabled);
        r.rows.act("b.ifbw.auto", "");
        CHECK(r.sent.size() == 2 && r.sent[1].i0 == 0 && r.sent[1].text == "if_bandwidth");
        // a bad number is refused here and never sent
        r.rows.act("b.ifbw", "12");
        r.rows.act("b.ifbw.set", "");
        CHECK(r.sent.size() == 2 && r.rows.note().find("300") != std::string::npos);
        // the radio selected decides which fact is read
        r.selected = 2;
        r.facts["fact.radio.2.ifBandwidth"] = "1536000";
        v = r.build();
        CHECK(r.has(v, "1536 kHz"));
        r.rows.act("b.ifbw", "");
        r.rows.act("b.ifbw.set", "");
        CHECK(r.sent.size() == 3 && r.sent[2].i0 == 0);                              // empty = automatic
    }

    // ---------------------------------------------------------------- G1-15a/b: the SoapySDR driver's bandwidth and settings, built from what the driver reported
    {
        namespace bb = foxsdr::slints5b;
        // pure rules
        std::map<std::string, std::string> f;
        const auto fact = [&f](const std::string& n) { const auto it = f.find(n); return it == f.end() ? std::string() : it->second; };
        CHECK(bb::readSoapySettings(fact, 1).empty());
        double cur = 0, lo = 0, hi = 0;
        CHECK(!bb::readSoapyBandwidth(fact, 1, cur, lo, hi));
        f["radio.1.soapy.bandwidth"] = "1000000"; f["radio.1.soapy.bandwidthLo"] = "200000"; f["radio.1.soapy.bandwidthHi"] = "8000000";
        CHECK(bb::readSoapyBandwidth(fact, 1, cur, lo, hi) && cur == 1e6 && lo == 200000 && hi == 8e6);
        double hz = 0; std::string e;
        CHECK(bb::parseSoapyBandwidthKHz("500", lo, hi, hz, e) && hz == 500000);
        CHECK(!bb::parseSoapyBandwidthKHz("100", lo, hi, hz, e) && !e.empty());
        CHECK(!bb::parseSoapyBandwidthKHz("9000", lo, hi, hz, e) && !bb::parseSoapyBandwidthKHz("wide", lo, hi, hz, e) && !bb::parseSoapyBandwidthKHz("", lo, hi, hz, e));
        bb::SoapySettingView s; s.type = "int"; s.hasRange = true; s.lo = -100; s.hi = 100;
        std::string out;
        CHECK(bb::soapyValueText(s, " 40 ", out, e) && out == "40");
        CHECK(!bb::soapyValueText(s, "500", out, e) && !bb::soapyValueText(s, "1.5", out, e) && !bb::soapyValueText(s, "x", out, e));
        s.type = "bool"; CHECK(bb::soapyValueText(s, "1", out, e) && out == "true" && bb::soapyValueText(s, "false", out, e) && out == "false" && !bb::soapyValueText(s, "maybe", out, e));
        s.type = "string"; s.options = {"a", "b"}; CHECK(bb::soapyValueText(s, "b", out, e) && !bb::soapyValueText(s, "c", out, e));
        std::string cmd;
        CHECK(bb::soapyWriteCommand("trim", "1.5", cmd, e) && cmd == "soapy:trim=1.5");
        CHECK(!bb::soapyWriteCommand("a=b", "1", cmd, e) && !bb::soapyWriteCommand("", "1", cmd, e) && !bb::soapyWriteCommand("k", std::string(300, 'x'), cmd, e));

        // the rows
        Rig r; r.open();
        std::vector<slk::Row> v = r.build();
        bool said = false; for (const auto& x : v) { if (x.label.find("names no bandwidth control") != std::string::npos) { said = true; } }
        CHECK(said && r.find(v, "b.sp.bw.set") == nullptr && r.find(v, "b.sp.0") == nullptr);   // a radio whose driver reported nothing shows no control, only the reason
        r.facts["fact.radio.1.soapy.bandwidth"] = "1000000"; r.facts["fact.radio.1.soapy.bandwidthLo"] = "200000"; r.facts["fact.radio.1.soapy.bandwidthHi"] = "8000000";
        r.facts["fact.radio.1.soapy.count"] = "4";
        const auto set = [&r](int i, const char* what, const char* val) { r.facts["fact.radio.1.soapy." + std::to_string(i) + "." + what] = val; };
        set(0, "key", "biastee"); set(0, "name", "Bias tee"); set(0, "type", "bool"); set(0, "value", "false");
        set(1, "key", "offset"); set(1, "name", "Tuner offset"); set(1, "type", "int"); set(1, "value", "5"); set(1, "units", "kHz"); set(1, "min", "-100"); set(1, "max", "100");
        set(2, "key", "clock"); set(2, "name", "Clock source"); set(2, "type", "string"); set(2, "value", "external"); set(2, "options", "internal|external|gpsdo"); set(2, "optionNames", "Internal|External 10 MHz|GPS");
        set(3, "key", "label"); set(3, "name", "Free text"); set(3, "type", "string"); set(3, "value", "hello");
        v = r.build();
        CHECK(r.has(v, "1000 kHz"));                                                           // the driver's bandwidth now
        CHECK(r.find(v, "b.sp.0") != nullptr && r.find(v, "b.sp.0")->on == false);           // a tick showing the driver's value
        CHECK(r.find(v, "b.sp.2") != nullptr && r.find(v, "b.sp.2")->pick == 1 && r.find(v, "b.sp.2")->options.size() == 3 && r.find(v, "b.sp.2")->options[1] == "External 10 MHz");
        CHECK(r.find(v, "b.sp.1") != nullptr && r.find(v, "b.sp.1")->value == "5");           // a field showing the driver's value
        CHECK(r.find(v, "b.sp.4") == nullptr && r.find(v, "b.sp.3.set") != nullptr);          // exactly the driver's four settings
        // bandwidth
        r.rows.act("b.sp.bw", "2500");
        r.rows.act("b.sp.bw.set", "");
        CHECK(r.sent.size() == 1 && r.sent[0].text == "soapy_bandwidth" && r.sent[0].n0 == 2500000.0 && r.sent[0].op == FOXAPI_OP_SET_DEVICE_OPTION);
        r.answer(101, FOXAPI_OK, "", 2400000.0);                                               // the driver rounded it: that is what the note says
        CHECK(r.rows.note().find("the driver now has") != std::string::npos && r.rows.note().find("2400 kHz") != std::string::npos);
        r.rows.act("b.sp.bw", "50");                                                           // outside the driver's range: never sent
        r.rows.act("b.sp.bw.set", "");
        CHECK(r.sent.size() == 1 && r.rows.note().find("200 - 8000 kHz") != std::string::npos);
        // each type sends the text the driver's own type wants
        r.rows.act("b.sp.0", "1");
        CHECK(r.sent.size() == 2 && r.sent[1].text == "soapy:biastee=true");
        r.rows.act("b.sp.2", "2");
        CHECK(r.sent.size() == 3 && r.sent[2].text == "soapy:clock=gpsdo");
        r.rows.act("b.sp.1", "-40");
        r.rows.act("b.sp.1.set", "");
        CHECK(r.sent.size() == 4 && r.sent[3].text == "soapy:offset=-40");
        r.rows.act("b.sp.1", "500");                                                           // outside the range the driver reported: refused here
        r.rows.act("b.sp.1.set", "");
        CHECK(r.sent.size() == 4 && r.rows.note().find("-100 to 100") != std::string::npos);
        r.rows.act("b.sp.3", "my note");
        r.rows.act("b.sp.3.set", "");
        CHECK(r.sent.size() == 5 && r.sent[4].text == "soapy:label=my note");
        // the engine's word: the driver refused
        r.answer(105, FOXAPI_FAILED, "writeSetting failed: the driver will not change that while a stream is running");
        CHECK(r.rows.note().find("will not change that while a stream is running") != std::string::npos);
        // the value after the write (the driver clamped it)
        r.answer(104, FOXAPI_OK, "10");
        CHECK(r.rows.note().find("the driver now has 10") != std::string::npos);
        // another radio's driver: the facts of the selected radio decide
        r.selected = 2;
        v = r.build();
        CHECK(r.find(v, "b.sp.0") == nullptr && r.find(v, "b.sp.bw.set") == nullptr);
    }
    // ---------------------------------------------------------------- G1-16 / G3-18a / G3-18b: CAT follow, OmniRig, virtual COM
    {
        namespace bb = foxsdr::slints5b;
        CHECK(bb::catModeWords().size() == 4 && std::string(bb::catModeKey(0)) == "off" && std::string(bb::catModeKey(3)) == "omnirig" && std::string(bb::catModeKey(9)) == "off");
        CHECK(bb::catModeIndex("follow") == 1 && bb::catModeIndex("emulate") == 2 && bb::catModeIndex("omnirig") == 3 && bb::catModeIndex("x") == 0);
        CHECK(bb::catRigWords("101100000", "USB") == "101.1 MHz, USB" && bb::catRigWords("", "USB").empty() && bb::catRigWords("x", "").empty());
        Rig r; r.open();
        std::vector<slk::Row> v = r.build();
        CHECK(r.find(v, "b.cat.mode") != nullptr && r.find(v, "b.cat.mode")->pick == 0 && r.find(v, "b.cat.port") == nullptr);   // off: nothing but the choice
        // the choice sends the engine's setting and nothing else
        r.rows.act("b.cat.mode", "1");
        CHECK(r.sent.size() == 1 && r.sent[0].op == FOXAPP_OP_WIREC_SETTING && r.sent[0].text == std::string("cat.link.mode") + char(0x1F) + "follow");
        r.facts["fact.wirec.cat.link.mode"] = "follow";                                         // the engine's value, read back
        v = r.build();
        CHECK(r.find(v, "b.cat.mode")->pick == 1 && r.find(v, "b.cat.port") != nullptr && r.find(v, "b.cat.baud") != nullptr && r.find(v, "b.cat.dialect") != nullptr && r.find(v, "b.cat.ff") != nullptr);
        // the port: COM1 to COM256 only, normalised; a path or a pipe is refused here and never sent
        r.rows.act("b.cat.port", "com7");
        r.rows.act("b.cat.port.set", "");
        CHECK(r.sent.size() == 2 && r.sent[1].text == std::string("cat.link.port") + char(0x1F) + "COM7");
        r.rows.act("b.cat.port", "LPT1");
        r.rows.act("b.cat.port.set", "");
        CHECK(r.sent.size() == 2);
        // the baud rate: given by the person, 300 to 4000000
        r.rows.act("b.cat.baud", "9600");
        r.rows.act("b.cat.baud.set", "");
        CHECK(r.sent.size() == 3 && r.sent[2].text == std::string("cat.link.baud") + char(0x1F) + "9600");
        r.rows.act("b.cat.baud", "12");
        r.rows.act("b.cat.baud.set", "");
        CHECK(r.sent.size() == 3);
        r.rows.act("b.cat.dialect", "1");
        CHECK(r.sent.size() == 4 && r.sent[3].text == std::string("cat.link.dialect") + char(0x1F) + "yaesu");
        r.rows.act("b.cat.ff", "0");
        CHECK(r.sent.size() == 5 && r.sent[4].text == std::string("cat.link.follow_frequency") + char(0x1F) + "0");
        // the link's own state is shown from the engine's facts
        r.facts["fact.cat.link.state"] = "following"; r.facts["fact.cat.link.rigHz"] = "14200000"; r.facts["fact.cat.link.rigMode"] = "USB";
        v = r.build();
        CHECK(r.has(v, "following the rig") && r.has(v, "14.2 MHz, USB"));
        r.facts["fact.cat.link.state"] = "error"; r.facts["fact.cat.link.error"] = "the port stopped answering: x";
        v = r.build();
        bool shown = false; for (const auto& x : v) { if (x.label.find("the port stopped answering") != std::string::npos) { shown = true; } }
        CHECK(shown);
        // OmniRig: a choice of rig, the needed-program words when it is absent, nothing downloaded
        r.facts["fact.wirec.cat.link.mode"] = "omnirig";
        v = r.build();
        CHECK(r.find(v, "b.cat.rig") != nullptr && r.find(v, "b.cat.port") == nullptr);
        bool needed = false; for (const auto& x : v) { if (x.label.find("does not download or install it") != std::string::npos) { needed = true; } }
        CHECK(needed);
        r.rows.act("b.cat.rig", "1");
        CHECK(r.sent.back().text == std::string("cat.link.rig") + char(0x1F) + "2");
        // the page names no setting but cat.link.* (every command it sends is a setting of that family)
        for (const auto& s : r.sent) { CHECK(s.op == FOXAPP_OP_WIREC_SETTING && s.text.rfind("cat.link.", 0) == 0); }
        // what works today, said in words
        bool ant = false, sdrpp = false;
        for (const auto& x : r.build()) { if (x.label.find("AntSDR: open it as an ADALM-Pluto") != std::string::npos) { ant = true; } if (x.label.find("SDR++ Server: start it in its rtl_tcp-compatible mode") != std::string::npos) { sdrpp = true; } }
        CHECK(ant && sdrpp);
    }
    // ---------------------------------------------------------------- G1-13: SDRplay low-IF (off by default; only on an SDRplay radio)
    {
        Rig r; r.open();
        std::vector<slk::Row> v = r.build();
        CHECK(r.find(v, "b.lowif") == nullptr);                                      // no SDRplay: nothing drawn
        r.facts["fact.radio.1.sdrplay.lowIf"] = "0";
        v = r.build();
        CHECK(r.find(v, "b.lowif") != nullptr && !r.find(v, "b.lowif")->on);         // the radio's own value at start-up: off
        r.rows.act("b.lowif", "1");
        CHECK(r.sent.size() == 1 && r.sent[0].op == FOXAPI_OP_SET_DEVICE_OPTION && r.sent[0].text == "sdrplay_lowif" && r.sent[0].i0 == 1);
        r.facts["fact.radio.1.sdrplay.lowIf"] = "0"; r.facts["fact.radio.1.sdrplay.lowIfNote"] = "The service delivers the ADC rate. Low-IF is switched off and the radio is back on zero-IF.";
        v = r.build();
        bool said = false; for (const auto& x : v) { if (x.label.find("Low-IF is switched off") != std::string::npos) { said = true; } }
        CHECK(said && !r.find(v, "b.lowif")->on);                                    // the fallback is shown and the tick follows the radio
        r.facts["fact.radio.1.sdrplay.lowIf"] = "1";
        CHECK(r.find(r.build(), "b.lowif")->on);
        r.rows.act("b.lowif", "0");
        CHECK(r.sent.size() == 2 && r.sent[1].i0 == 0);
    }
    return testSummary("test_slint_s5b_host");
}
