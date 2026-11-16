// Screen lane S5's pure rules and rows (src/slint/s5_logic.hpp, s5_rows.hpp; docs/multi-radio-records/screen-s5-sources.md): the raw I/Q stream's address text, automatic ppm, tuner pooling, the RTL-SDR's direct-sampling
// choice. The rows are driven with a stand-in for the engine: the facts it would read are a table, and every command a control sends is RECORDED so each assertion is about what the engine would have been sent,
// not about what the page stored.
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
#include "test_check.hpp"

using namespace foxsdr::slints5;
namespace slk = foxsdr::slintlanek;

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
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    double d = 0; std::string e;

    // ---------------------------------------------------------------- G1-05 raw I/Q: the address text
    {
        RawIqFields f; f.port = "5555"; f.rateHz = "2000000";
        Built b = rawIqOpenText(f);
        CHECK(b.ok && b.text == "tcp://127.0.0.1:5555?fmt=f32&rate=2000000&freq=0&timeout=3000");
        f.udp = 1; f.format = 1; f.host = "0.0.0.0"; f.centreHz = "100000000"; f.timeoutMs = "500";
        b = rawIqOpenText(f);
        CHECK(b.ok && b.text == "udp://0.0.0.0:5555?fmt=s16&rate=2000000&freq=100000000&timeout=500");
        f.format = 2; f.header = true;   // a header says the format, rate and centre: the typed ones are not sent
        b = rawIqOpenText(f);
        CHECK(b.ok && b.text == "udp://0.0.0.0:5555?hdr=1&timeout=500");
        RawIqFields g; g.port = "5555";   // no rate, no header: refused, with the field named
        b = rawIqOpenText(g);
        CHECK(!b.ok && b.text.empty() && b.err.find("Sample rate") != std::string::npos);
        g.rateHz = "999"; CHECK(!rawIqOpenText(g).ok);                   // below 1000 Hz
        g.rateHz = "1000"; CHECK(rawIqOpenText(g).ok);
        g.rateHz = "100000000"; CHECK(rawIqOpenText(g).ok);
        g.rateHz = "100000001"; CHECK(!rawIqOpenText(g).ok);
        g.rateHz = "nan"; CHECK(!rawIqOpenText(g).ok);
        g.rateHz = "2e6"; CHECK(!rawIqOpenText(g).ok || true);   // whatever it says, never a crash
        g.rateHz = "2000000";
        for (const char* p : {"0", "65536", "-1", "abc", "", "5555.5"}) { g.port = p; CHECK(!rawIqOpenText(g).ok); }
        g.port = "1"; CHECK(rawIqOpenText(g).ok);
        g.port = "65535"; CHECK(rawIqOpenText(g).ok);
        g.port = "5555";
        for (const char* h : {"", "a b", "host?x=1", "host&y", "host/path", "tcp://x", "a\tb"}) { g.host = h; CHECK(!rawIqOpenText(g).ok); }
        g.host = std::string(254, 'a'); CHECK(!rawIqOpenText(g).ok);
        g.host = "[::1]"; CHECK(rawIqOpenText(g).ok);
        g.host = "stream-box.local"; CHECK(rawIqOpenText(g).ok);
        g.host = "127.0.0.1"; g.timeoutMs = "99"; CHECK(!rawIqOpenText(g).ok);
        g.timeoutMs = "600001"; CHECK(!rawIqOpenText(g).ok);
        g.timeoutMs = ""; CHECK(rawIqOpenText(g).ok);    // empty = the default
        CHECK(rawIqProtocolNote(RawIqFields{}).find("TCP") != std::string::npos);
        RawIqFields u; u.udp = 1;
        CHECK(rawIqProtocolNote(u).find("this computer only") != std::string::npos);
        u.host = "0.0.0.0";
        CHECK(rawIqProtocolNote(u).find("other computers") != std::string::npos);
        CHECK(rawIqFormatWords().size() == 3);
    }

    // ---------------------------------------------------------------- G1-17 automatic ppm: the numbers
    {
        CHECK(parsePpmSeconds("", d, e) && d == 0.0);
        CHECK(parsePpmSeconds("0", d, e) && d == 0.0);
        CHECK(parsePpmSeconds("10", d, e) && d == 10.0);
        CHECK(parsePpmSeconds("300", d, e) && d == 300.0);
        CHECK(!parsePpmSeconds("9", d, e) && !e.empty());
        CHECK(!parsePpmSeconds("301", d, e) && !parsePpmSeconds("-5", d, e) && !parsePpmSeconds("soon", d, e));
        CHECK(ppmReading(3.2, 0.12, 48).find("+3.20 ppm") == 0 && ppmReading(3.2, 0.12, 48).find("48") != std::string::npos);
        CHECK(ppmReading(-1.0, 0.1, 0).empty());
        CHECK(ppmStateWords("measuring") == "measuring" && ppmStateWords("done") == "done" && ppmStateWords("other").empty());
        CHECK(!replacesTypedPpm(PpmInForce{false, 5.0}, 3.0));      // nothing switched on: nothing to replace
        CHECK(!replacesTypedPpm(PpmInForce{true, 3.0}, 3.0));       // the same number: nothing to fight
        CHECK(replacesTypedPpm(PpmInForce{true, 5.0}, 3.0));
        CHECK(ppmReference().find("19 kHz") != std::string::npos);
    }

    // ---------------------------------------------------------------- G1-18 tuner pooling: the numbers
    {
        CHECK(parseClaimMHz("145.5", d, e) && std::fabs(d - 145.5e6) < 1.0);
        CHECK(!parseClaimMHz("0", d, e) && !parseClaimMHz("-1", d, e) && !parseClaimMHz("", d, e) && !parseClaimMHz("x", d, e) && !parseClaimMHz("1e9", d, e));
        CHECK(parseClaimKHz("", d, e) && d == 0.0);
        CHECK(parseClaimKHz("12.5", d, e) && d == 12500.0);
        CHECK(!parseClaimKHz("-1", d, e) && !parseClaimKHz("1e7", d, e));
        CHECK(claimModeWords().size() == 9 && claimModeWords()[1] == "NFM" && claimModeWords()[8] == "RAW");   // index k is FOXAPI_DEMOD_* k
        CHECK(claimAnswerText(2, 1).find("Radio 2") == 0 && claimAnswerText(2, 1).find("tuning") != std::string::npos);
        CHECK(claimAnswerText(3, 2).find("centre") != std::string::npos);
    }

    // ---------------------------------------------------------------- G1-11 direct sampling: the words
    {
        CHECK(directSamplingWords().size() == 4);
        CHECK(directSamplingIndex("0") == 0 && directSamplingIndex("3") == 3 && directSamplingIndex("4") == -1 && directSamplingIndex("") == -1 && directSamplingIndex("12") == -1);
    }

    // ---------------------------------------------------------------- the rows: folded, then open
    {
        Rig r;
        std::vector<slk::Row> v = r.build();
        CHECK(v.size() == 1 && v[0].id == "s.more");                   // nothing but the one key until asked for
        CHECK(r.sent.empty());                                         // and nothing sent
        r.rows.act("s.more", "");
        v = r.build();
        CHECK(v.size() > 20 && r.find(v, "r.open") != nullptr && r.find(v, "m.start") != nullptr && r.find(v, "t.claim") != nullptr);
        CHECK(r.sent.empty());                                         // opening the panel sends nothing
        r.rows.act("s.more", "");
        CHECK(r.build().size() == 1);                                  // and it folds again
    }

    // ---------------------------------------------------------------- the rows: raw I/Q reaches the engine as the text built
    {
        Rig r; r.open();
        r.rows.act("r.port", "5555");
        r.rows.act("r.rate", "2400000");
        r.rows.act("r.fmt", "2");
        r.rows.act("r.host", "192.168.1.20");
        CHECK(r.rows.act("r.open", ""));
        CHECK(r.sent.size() == 1 && r.sent[0].op == FOXAPI_OP_SELECT_SOURCE);
        CHECK(r.sent[0].text == "open-rawiq:tcp://192.168.1.20:5555?fmt=u8&rate=2400000&freq=0&timeout=3000");
        CHECK(r.sent[0].radio == 0);                                   // the selected radio's source
        // a field that is wrong sends nothing, and says which
        Rig q; q.open();
        q.rows.act("r.open", "");
        CHECK(q.sent.empty() && q.rows.refused() == 1 && q.rows.note().find("Port") != std::string::npos);
        // UDP, header: the format, rate and centre fields go away
        Rig u; u.open();
        u.rows.act("r.proto", "1"); u.rows.act("r.hdr", "1"); u.rows.act("r.port", "40000");
        std::vector<slk::Row> v = u.build();
        CHECK(u.find(v, "r.rate") == nullptr && u.find(v, "r.fmt") == nullptr);
        u.rows.act("r.open", "");
        CHECK(u.sent.size() == 1 && u.sent[0].text == "open-rawiq:udp://127.0.0.1:40000?hdr=1&timeout=3000");
        // the engine refusing the open is shown in its own words
        Rig w; w.open(); w.rows.act("r.port", "5555"); w.rows.act("r.rate", "1000000");
        w.rows.act("r.open", "");
        w.answer(101, FOXAPI_BAD_ARGUMENT, "the address is host:port");
        CHECK(w.rows.note().find("the address is host:port") != std::string::npos && w.rows.refused() == 1);
        // the engine not taking the command at all is a refusal too
        Rig x; x.open(); x.nextStatus = FOXAPI_BUSY; x.rows.act("r.port", "5555"); x.rows.act("r.rate", "1000000");
        x.rows.act("r.open", "");
        CHECK(x.rows.refused() == 1 && x.rows.note().find("Busy") != std::string::npos);
    }

    // ---------------------------------------------------------------- the rows: automatic ppm
    {
        Rig r; r.open();
        r.selected = 2;
        std::vector<slk::Row> v = r.build();
        CHECK(r.find(v, "m.use") != nullptr && !r.find(v, "m.use")->enabled);       // nothing measured: nothing to use
        CHECK(r.find(v, "m.start")->enabled && !r.find(v, "m.cancel")->enabled);
        r.rows.act("m.secs", "60");
        r.rows.act("m.start", "");
        CHECK(r.sent.size() == 1 && r.sent[0].op == FOXAPP_OP_PPM_MEASURE && r.sent[0].i0 == 1 && r.sent[0].n0 == 60.0);
        r.rows.act("m.secs", "5");                                                  // too short: refused here, not sent
        r.rows.act("m.start", "");
        CHECK(r.sent.size() == 1 && r.rows.refused() == 1);
        // measuring (radio 2's facts, the selected one): the cancel key lives, the start key rests, progress shows
        r.facts["fact.radio.2.ppmauto.state"] = "measuring"; r.facts["fact.radio.2.ppmauto.progress"] = "0.500";
        v = r.build();
        CHECK(!r.find(v, "m.start")->enabled && r.find(v, "m.cancel")->enabled && r.has(v, "50 %"));
        r.rows.act("m.cancel", "");
        CHECK(r.sent.back().op == FOXAPP_OP_PPM_MEASURE && r.sent.back().i0 == 0);
        // another radio's measurement does not show on this one
        r.facts.clear(); r.facts["fact.radio.1.ppmauto.state"] = "done"; r.facts["fact.radio.1.ppmauto.readings"] = "40"; r.facts["fact.radio.1.ppmauto.ppm"] = "2.50";
        v = r.build();
        CHECK(!r.find(v, "m.use")->enabled);
        // done, nothing in force: one press applies the measured number and switches the correction on
        Rig d1; d1.open();
        d1.facts["fact.radio.1.ppmauto.state"] = "done"; d1.facts["fact.radio.1.ppmauto.readings"] = "40"; d1.facts["fact.radio.1.ppmauto.ppm"] = "2.50"; d1.facts["fact.radio.1.ppmauto.uncertainty"] = "0.10";
        v = d1.build();
        CHECK(d1.find(v, "m.use")->enabled && d1.has(v, "+2.50 ppm, plus or minus 0.10 (40 readings)"));
        d1.rows.act("m.use", "");
        CHECK(d1.sent.size() == 1 && d1.sent[0].op == FOXAPP_OP_SET_PPM && d1.sent[0].i0 == 1 && d1.sent[0].n0 == 2.5);
        // a correction the person set (5 ppm, on) is NOT replaced by one press: the key asks, and only the second press sends
        Rig d2; d2.open();
        d2.facts["fact.radio.1.ppmauto.state"] = "done"; d2.facts["fact.radio.1.ppmauto.readings"] = "40"; d2.facts["fact.radio.1.ppmauto.ppm"] = "2.50";
        d2.facts["fact.ppm.on"] = "1"; d2.facts["fact.ppm.value"] = "5";
        d2.build();
        d2.rows.act("m.use", "");
        CHECK(d2.sent.empty() && d2.rows.note().find("press the key again") != std::string::npos);
        v = d2.build();
        CHECK(d2.find(v, "m.use")->label.find("Press again") == 0);
        d2.rows.act("m.use", "");
        CHECK(d2.sent.size() == 1 && d2.sent[0].op == FOXAPP_OP_SET_PPM && d2.sent[0].n0 == 2.5);
        // any other action in between disarms it
        Rig d3; d3.open();
        d3.facts = d2.facts;
        d3.build(); d3.rows.act("m.use", ""); d3.rows.act("m.secs", "30"); d3.build(); d3.rows.act("m.use", "");
        CHECK(d3.sent.empty());
        // the same number already in force: no second press needed
        Rig d4; d4.open();
        d4.facts = d2.facts; d4.facts["fact.ppm.value"] = "2.5";
        d4.build(); d4.rows.act("m.use", "");
        CHECK(d4.sent.size() == 1);
    }

    // ---------------------------------------------------------------- the rows: tuner pooling
    {
        Rig r; r.open();
        r.rows.act("t.claim", "");                                                  // no frequency typed
        CHECK(r.sent.empty() && r.rows.refused() == 1);
        r.rows.act("t.freq", "145.5"); r.rows.act("t.mode", "1"); r.rows.act("t.bw", "12.5");
        r.rows.act("t.claim", "");
        CHECK(r.sent.size() == 1 && r.sent[0].op == FOXAPP_OP_CHANNEL_CLAIM && std::fabs(r.sent[0].n0 - 145.5e6) < 1.0 && r.sent[0].i0 == 1 && r.sent[0].n1 == 12500.0);
        r.answer(101, FOXAPI_OK, "", 3, 2);                                         // radio 3, its centre moved
        CHECK(r.rows.note().find("Radio 3") != std::string::npos && r.rows.note().find("centre") != std::string::npos);
        r.answer(999, FOXAPI_OK, "", 7, 1);                                         // an answer that is not ours changes nothing
        CHECK(r.rows.note().find("Radio 3") != std::string::npos);
        // no radio free: the engine's own sentence
        r.rows.act("t.claim", "");
        r.answer(102, FOXAPI_LIMIT, "every radio is busy: radio 1 is recording");
        CHECK(r.rows.note().find("every radio is busy") != std::string::npos);
        // a release key exists for the claimed radios only, and sends that radio's number
        std::vector<slk::Row> v = r.build();
        CHECK(r.find(v, "t.rel2") == nullptr && r.find(v, "t.rel3") == nullptr);
        r.facts["fact.radio.2.claimed"] = "1"; r.facts["fact.radio.3.claimed"] = "0"; r.facts["fact.radio.5.claimed"] = "1";
        v = r.build();
        CHECK(r.find(v, "t.rel2") != nullptr && r.find(v, "t.rel3") == nullptr && r.find(v, "t.rel5") != nullptr);
        r.rows.act("t.rel5", "");
        CHECK(r.sent.back().op == FOXAPP_OP_CHANNEL_RELEASE && r.sent.back().i0 == 5);
        // a bandwidth that is not a number is refused here
        r.rows.act("t.bw", "wide");
        const size_t before = r.sent.size();
        r.rows.act("t.claim", "");
        CHECK(r.sent.size() == before);
    }

    // ---------------------------------------------------------------- the rows: direct sampling
    {
        Rig r; r.open();
        std::vector<slk::Row> v = r.build();
        CHECK(r.find(v, "d.mode") == nullptr);                                      // a radio with no such setting shows no control, only the reason
        CHECK(r.has(v, "This radio has no direct-sampling mode. Only an RTL-SDR opened by FoxSDR's own driver has one.") || [&] { for (const auto& x : v) { if (x.label.find("no direct-sampling mode") != std::string::npos) { return true; } } return false; }());
        r.facts["fact.radio.1.directSampling"] = "3";
        v = r.build();
        CHECK(r.find(v, "d.mode") != nullptr && r.find(v, "d.mode")->pick == 3 && r.find(v, "d.mode")->options.size() == 4);   // the engine's current value, not a default
        r.rows.act("d.mode", "2");
        CHECK(r.sent.size() == 1 && r.sent[0].op == FOXAPI_OP_SET_DEVICE_OPTION && r.sent[0].text == "direct_sampling" && r.sent[0].i0 == 2);
        r.answer(101, FOXAPI_FAILED, "7.1 MHz is below this tuner's range");        // a refusal is shown in the radio's words
        CHECK(r.rows.note().find("below this tuner's range") != std::string::npos);
        r.facts["fact.radio.1.directSampling"] = "2";                               // the value read back after a change made elsewhere
        v = r.build();
        CHECK(r.find(v, "d.mode")->pick == 2);
    }

    return testSummary("test_slint_s5_host");
}
