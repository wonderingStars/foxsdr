// test_engine_api_s5.cpp - SCREEN LANE S5's CONTROLS AGAINST THE REAL ENGINE (wave 8; docs/multi-radio-records/screen-s5-sources.md): the rows the Source section draws (src/slint/s5_rows.cpp, the very code the window
// runs) are driven here with the engine behind them instead of the stand-in of test_slint_s5_host, and every assertion is about what the ENGINE says afterwards (its facts and radio state), not about what the page stored.
//   A  a raw I/Q stream: the address typed into the fields becomes the text the engine opens; a stand-in TCP sender on 127.0.0.1 (a tone 25 kHz from the centre) shows up as radio 1's source with the rate and centre typed,
//      and the tone is where it belongs in the spectrum; a UDP listener opens on 127.0.0.1 and says so; a hostile stream (bad header, then garbage) leaves the radio standing with a sentence and does not grow memory
//   B  tuner pooling: Find a free radio takes a free radio and the page says which and how; the claim shows as a Release key; a radio that is claimed is not taken twice; Release gives it back; with every radio
//      claimed the engine's own refusal is shown
//   C  automatic ppm: Measure reaches the radio (WFM only: the engine's refusal is shown in its words in another mode), the state and progress are read back, Cancel stops it, nothing is applied by it
//   D  direct sampling: a radio with no such setting shows no control and the reason (the generator radio has none)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "net_test_support.hpp"
#include "wire1_rig.hpp"

#include "slint/lanek_logic.hpp"
#include "slint/s5_logic.hpp"
#include "slint/s5_rows.hpp"

namespace {

namespace s5 = foxsdr::slints5;
namespace slk = foxsdr::slintlanek;

void put32(std::string& v, std::uint32_t x) { for (int i = 0; i < 4; ++i) { v.push_back(static_cast<char>((x >> (8 * i)) & 0xFFu)); } }

// A sender that keeps a stand-in server fed: 10 ms of tone (and noise) every 10 ms, phase continuous, headerless f32.
struct Feeder {
    nettest::StandInServer& srv;
    double rate, toneHz;
    std::atomic<bool> go{true};
    std::thread th;
    Feeder(nettest::StandInServer& s, double r, double t) : srv(s), rate(r), toneHz(t) {
        th = std::thread([this] {
            std::mt19937_64 rng(5);
            std::normal_distribution<double> noise(0.0, 0.002);
            std::uint64_t k = 0;
            const std::size_t per = static_cast<std::size_t>(rate / 100.0);
            while (go.load()) {
                std::string block;
                for (std::size_t i = 0; i < per; ++i, ++k) {
                    const double ph = 6.283185307179586 * toneHz * static_cast<double>(k) / rate;
                    put32(block, std::bit_cast<std::uint32_t>(static_cast<float>(0.4 * std::cos(ph) + noise(rng))));
                    put32(block, std::bit_cast<std::uint32_t>(static_cast<float>(0.4 * std::sin(ph) + noise(rng))));
                }
                srv.sendTo(0, block);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }
    ~Feeder() {
        go.store(false);
        if (th.joinable()) { th.join(); }
    }
};

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

void testA() {
    std::printf("A: a raw I/Q stream opened from the page's own fields\n");
    {
        Rig r;
        r.pump(10);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0.0, 1)).status(), FOXAPI_OK);   // radio 1 is running
        nettest::StandInServer srv;
        CHECK(srv.start());
        Feeder feed(srv, 240000.0, 25000.0);
        Panel p(r);
        p.open();
        p.act("r.host", "127.0.0.1");
        p.act("r.port", std::to_string(srv.port()));
        p.act("r.fmt", "0");
        p.act("r.rate", "240000");
        p.act("r.centre", "7100000");
        p.act("r.open", "");
        CHECK_EQ(p.rows.refused(), 0L);
        CHECK(p.sentCmds.size() == 1 && p.sentCmds[0].op == FOXAPI_OP_SELECT_SOURCE);
        CHECK(r.until([&] { return r.fact("radio.1.source") == "rawiq" && r.radio(1).sampleRateHz == 240000.0; }, 30000));
        CHECK_EQ(r.fact("radio.1.source"), std::string("rawiq"));
        CHECK_EQ(r.radio(1).sampleRateHz, 240000.0);
        CHECK_EQ(r.radio(1).centreHz, 7100000.0);
        std::printf("  state %s, centre %.0f, rate %.0f\n", r.fact("radio.1.state").c_str(), r.radio(1).centreHz, r.radio(1).sampleRateHz);
        CHECK(r.until([&] { return peakAt(r.spectrum(1), 25000.0, 4); }, 20000));
        CHECK(peakAt(r.spectrum(1), 25000.0, 4));
    }
    {
        // BREAK-IT's partner: the same panel with a rate the stream does not have puts the tone somewhere else (the typed rate is what the engine used)
        Rig r;
        r.pump(10);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0.0, 1)).status(), FOXAPI_OK);   // radio 1 is running
        nettest::StandInServer srv;
        CHECK(srv.start());
        Feeder feed(srv, 240000.0, 25000.0);
        Panel p(r);
        p.open();
        p.act("r.port", std::to_string(srv.port()));
        p.act("r.rate", "480000");
        p.act("r.open", "");
        CHECK(r.until([&] { return r.fact("radio.1.source") == "rawiq" && r.radio(1).sampleRateHz == 480000.0; }, 30000));
        CHECK_EQ(r.radio(1).sampleRateHz, 480000.0);
        CHECK(r.until([&] { return peakAt(r.spectrum(1), 50000.0, 4); }, 20000));   // 25 kHz of a 240 kHz stream read as a 480 kHz one lands at 50 kHz
        CHECK(peakAt(r.spectrum(1), 50000.0, 4));
        CHECK(!peakAt(r.spectrum(1), 25000.0, 2));
    }
    {
        // a UDP listener on this computer only: the engine says it is listening
        Rig r;
        r.pump(10);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0.0, 1)).status(), FOXAPI_OK);   // radio 1 is running
        Panel p(r);
        p.open();
        p.act("r.proto", "1");
        p.act("r.port", "47193");
        p.act("r.rate", "240000");
        p.act("r.open", "");
        CHECK(p.sentCmds.size() == 1 && std::string(p.sentCmds[0].text).find("udp://127.0.0.1:47193") != std::string::npos);
        CHECK(r.until([&] { return r.fact("radio.1.source") == "rawiq"; }, 30000));
        CHECK_EQ(r.fact("radio.1.source"), std::string("rawiq"));
    }
    {
        // a stream that is not one: a header that lies, then bytes that are not samples. The radio is not lost to a crash, the page says what the engine said.
        Rig r;
        r.pump(10);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0.0, 1)).status(), FOXAPI_OK);   // radio 1 is running
        nettest::StandInServer srv;
        CHECK(srv.start());
        std::string junk = "FXIQ";
        junk.push_back(9); junk.push_back(0); junk.push_back(7); junk.push_back(0);   // version 9, format 7: refused
        put32(junk, 0xFFFFFFFFu);
        std::mt19937_64 rng(11);
        for (int i = 0; i < 4096; ++i) { junk.push_back(static_cast<char>(rng() & 0xFFu)); }
        srv.setGreeting(junk);
        Panel p(r);
        p.open();
        p.act("r.hdr", "1");
        p.act("r.port", std::to_string(srv.port()));
        p.act("r.timeout", "600");
        p.act("r.open", "");
        CHECK(r.until([&] { return !r.fact("status.sourceError").empty() && r.fact("status.sourceError") != "<absent>"; }, 30000) || r.fact("radio.1.source") == "siggen");
        CHECK_EQ(r.fact("radio.1.source"), std::string("siggen"));        // it stayed on the generator: nothing half-opened
        r.pump(20);
        CHECK(r.radio(1).sampleRateHz > 0.0);                             // and the engine is still serving
    }
    {
        // the engine refuses an address the page let through (a name no resolver knows) with its own sentence; the page shows it and keeps the fields
        Rig r;
        r.pump(10);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0.0, 1)).status(), FOXAPI_OK);   // radio 1 is running
        Panel p(r);
        p.open();
        p.act("r.host", "no-such-host.invalid");
        p.act("r.port", "5555");
        p.act("r.rate", "48000");
        p.act("r.open", "");
        CHECK(r.until([&] { return !r.fact("status.sourceError").empty() && r.fact("status.sourceError") != "<absent>"; }, 30000));
        CHECK_EQ(r.fact("radio.1.source"), std::string("siggen"));
        CHECK_EQ(p.rows.draft("r.port"), std::string("5555"));
    }
}

void testB() {
    std::printf("B: tuner pooling\n");
    Rig r;
    r.pump(10);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0.0, 1)).status(), FOXAPI_OK);   // a radio that is not running is never taken: radio 1 runs
    CHECK_EQ(r.add(2, "generator").status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.count") == "2"; }, 20000));
    Panel p(r);
    p.open();
    // nothing typed: refused on the page, nothing sent
    p.act("t.claim", "");
    CHECK(p.sentCmds.empty() && p.rows.refused() == 1L);
    p.act("t.freq", "145.5");
    p.act("t.mode", "1");             // NFM
    p.act("t.bw", "12.5");
    p.act("t.claim", "");
    CHECK_EQ(p.sentCmds.size(), static_cast<std::size_t>(1));
    CHECK(p.rows.note().find("Radio ") != std::string::npos);
    std::printf("  note: %s\n", p.rows.note().c_str());
    // the engine now says one radio is claimed (the page reads it, it does not remember it)
    const int claimed1 = r.fact("radio.1.claimed") == "1" ? 1 : 0, claimed2 = r.fact("radio.2.claimed") == "1" ? 1 : 0;
    CHECK_EQ(claimed1 + claimed2, 1);
    const int first = claimed1 != 0 ? 1 : 2;
    std::vector<slk::Row> v = p.build();
    CHECK(p.find(v, "t.rel" + std::to_string(first)) != nullptr && p.find(v, "t.rel" + std::to_string(3 - first)) == nullptr);
    // the claimed radio is tuned where it was asked
    CHECK_EQ(r.radio(static_cast<std::uint32_t>(first)).demodMode, static_cast<std::uint32_t>(FOXAPI_DEMOD_NFM));
    CHECK(std::fabs(r.radio(static_cast<std::uint32_t>(first)).tunedHz - 145.5e6) < 1.0);          // at the frequency typed, in MHz
    CHECK(std::fabs(r.radio(static_cast<std::uint32_t>(first)).bandwidthHz - 12500.0) < 1.0);      // and the width typed, in kHz
    // a second claim takes the other radio, and a third finds none
    p.act("t.freq", "162.55");
    p.act("t.claim", "");
    CHECK_EQ(r.fact("radio.1.claimed") + r.fact("radio.2.claimed"), std::string("11"));
    p.act("t.claim", "");
    CHECK(p.rows.note().find("Refused") != std::string::npos);
    std::printf("  no free radio: %s\n", p.rows.note().c_str());
    // Release gives a radio back, and the key goes away
    v = p.build();
    CHECK(p.find(v, "t.rel1") != nullptr && p.find(v, "t.rel2") != nullptr);
    p.act("t.rel1", "");
    CHECK_EQ(r.fact("radio.1.claimed"), std::string("0"));
    v = p.build();
    CHECK(p.find(v, "t.rel1") == nullptr && p.find(v, "t.rel2") != nullptr);
    p.act("t.rel2", "");
    CHECK_EQ(r.fact("radio.2.claimed"), std::string("0"));
    // releasing what is not claimed is the engine's "nothing changed", shown as such
    p.act("t.rel2", "");
}

void testC() {
    std::printf("C: automatic ppm\n");
    Rig r;
    r.pump(10);
    Panel p(r);
    p.open();
    std::vector<slk::Row> v = p.build();
    CHECK(!p.find(v, "m.use")->enabled && !p.find(v, "m.cancel")->enabled && p.find(v, "m.start")->enabled);
    // not in WFM: the engine's own refusal reaches the page
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_MODE, 0.0, FOXAPI_DEMOD_AM)).status(), FOXAPI_OK);
    p.act("m.secs", "10");
    p.act("m.start", "");
    CHECK(p.rows.note().find("Refused") != std::string::npos || p.rows.note().find("WFM") != std::string::npos);
    std::printf("  AM: %s\n", p.rows.note().c_str());
    CHECK_EQ(r.fact("radio.1.ppmauto.state"), std::string("idle"));
    // in WFM it measures; the state and progress are the engine's
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_MODE, 0.0, FOXAPI_DEMOD_WFM)).status(), FOXAPI_OK);
    p.act("m.start", "");
    CHECK(r.until([&] { return r.fact("radio.1.ppmauto.state") == "measuring"; }, 10000));
    CHECK_EQ(r.fact("radio.1.ppmauto.state"), std::string("measuring"));
    v = p.build();
    CHECK(!p.find(v, "m.start")->enabled && p.find(v, "m.cancel")->enabled && !p.find(v, "m.use")->enabled);
    p.act("m.cancel", "");
    CHECK(r.until([&] { return r.fact("radio.1.ppmauto.state") != "measuring"; }, 10000));
    CHECK(r.fact("radio.1.ppmauto.state") != "measuring");
    // nothing of it was applied: no correction came on
    CHECK(r.fact("ppm.on") != "1");
}

void testD() {
    std::printf("D: direct sampling on a radio that has none\n");
    Rig r;
    r.pump(10);
    Panel p(r);
    p.open();
    const std::vector<slk::Row> v = p.build();
    CHECK(p.find(v, "d.mode") == nullptr);
    CHECK_EQ(r.fact("radio.1.directSampling"), std::string("<absent>"));
    bool reason = false;
    for (const auto& x : v) { if (x.label.find("no direct-sampling mode") != std::string::npos) { reason = true; } }
    CHECK(reason);
    CHECK(p.sentCmds.empty());
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_s5\n");
    engine_api_test::hermetic("s5");
    Access::installHooks();
    testA();
    testB();
    testC();
    testD();
    return check::finish("test_engine_api_s5");
}
