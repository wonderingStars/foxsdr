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

    return testSummary("test_slint_s5b_host");
}
