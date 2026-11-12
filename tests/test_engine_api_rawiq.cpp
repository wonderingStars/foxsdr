// test_engine_api_rawiq.cpp - A RAW I/Q STREAM AS A RADIO'S SOURCE THROUGH THE ENGINE (engine serial lane A, row R5): RADIO_ADD / SELECT_SOURCE "open-rawiq:<address>".
//   A  radio 2 on a stand-in TCP sender (127.0.0.1, FXIQ header, a tone 25 kHz from the centre, with noise): the radio's source fact says rawiq, its rate is the header's, and its spectrum has the tone where it
//      belongs; radio 1 is not disturbed;  B  the refusals: an address that is not one (BAD_ARGUMENT, nothing made), the same stream twice (BUSY), a REMOTE session (DENIED), a stream with no sender (the radio stays on the generator
//      with a sentence);  C  a headerless stream (u8, with the rate in the address) plays too.
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

namespace {

void put32(std::string& v, std::uint32_t x) { for (int i = 0; i < 4; ++i) { v.push_back(static_cast<char>((x >> (8 * i)) & 0xFFu)); } }

std::string fxiqHeader(std::uint32_t rate, double centre) {
    std::string h = {'F', 'X', 'I', 'Q', 1, 0, 3, 0};
    put32(h, rate);
    const std::uint64_t b = std::bit_cast<std::uint64_t>(centre);
    put32(h, static_cast<std::uint32_t>(b & 0xFFFFFFFFu));
    put32(h, static_cast<std::uint32_t>(b >> 32));
    put32(h, 0);
    put32(h, 0);
    return h;
}

// A sender that keeps a stand-in server fed: 10 ms of tone (and noise) every 10 ms, phase continuous.
struct Feeder {
    nettest::StandInServer& srv;
    double rate, toneHz;
    bool u8;
    std::atomic<bool> go{true};
    std::thread th;
    Feeder(nettest::StandInServer& s, double r, double t, bool asU8) : srv(s), rate(r), toneHz(t), u8(asU8) {
        th = std::thread([this] {
            std::mt19937_64 rng(3);
            std::normal_distribution<double> noise(0.0, 0.002);
            std::uint64_t k = 0;
            const std::size_t per = static_cast<std::size_t>(rate / 100.0);
            while (go.load()) {
                std::string block;
                for (std::size_t i = 0; i < per; ++i, ++k) {
                    const double ph = 6.283185307179586 * toneHz * static_cast<double>(k) / rate;
                    const double re = 0.4 * std::cos(ph) + noise(rng), im = 0.4 * std::sin(ph) + noise(rng);
                    if (u8) {
                        block.push_back(static_cast<char>(static_cast<unsigned char>(std::lround(re * 127.5 + 127.5))));
                        block.push_back(static_cast<char>(static_cast<unsigned char>(std::lround(im * 127.5 + 127.5))));
                    } else {
                        put32(block, std::bit_cast<std::uint32_t>(static_cast<float>(re)));
                        put32(block, std::bit_cast<std::uint32_t>(static_cast<float>(im)));
                    }
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

void testA() {
    std::printf("A: a header stream becomes radio 2\n");
    Rig r;
    r.pump(10);
    nettest::StandInServer srv;
    CHECK(srv.start());
    srv.setGreeting(fxiqHeader(240000, 0.0));
    Feeder feed(srv, 240000.0, 25000.0, false);
    const testing::Outcome o = r.add(2, "open-rawiq:127.0.0.1:" + std::to_string(srv.port()) + "?hdr=1");
    CHECK_EQ(o.status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.2.source") == "rawiq" && r.radio(2).sampleRateHz == 240000.0; }, 30000));
    CHECK_EQ(r.fact("radio.2.source"), std::string("rawiq"));
    CHECK_EQ(r.radio(2).sampleRateHz, 240000.0);
    CHECK(r.until([&] { return peakAt(r.spectrum(2), 25000.0, 4); }, 20000));
    CHECK(peakAt(r.spectrum(2), 25000.0, 4));
    CHECK_EQ(r.radio(1).sampleRateHz > 0.0, true);   // radio 1 is where it was
    // the same stream twice: the ledger says so
    const testing::Outcome twice = r.add(3, "open-rawiq:127.0.0.1:" + std::to_string(srv.port()) + "?hdr=1");
    CHECK_EQ(twice.status(), FOXAPI_BUSY);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RADIO_REMOVE, 1, 0.0, 2)).status(), FOXAPI_OK);
}

void testB() {
    std::printf("B: refusals\n");
    Rig r;
    r.pump(10);
    CHECK_EQ(r.add(2, "open-rawiq:nonsense").status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.add(2, "open-rawiq:127.0.0.1:5555").status(), FOXAPI_BAD_ARGUMENT);        // no rate, no header
    CHECK_EQ(r.add(2, "open-rawiq:127.0.0.1:5555?rate=48000&fmt=f64").status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.fact("radio.count"), std::string("1"));
    // a REMOTE session may not make the engine connect or listen anywhere
    {
        testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
        CHECK(remote.ok());
        FoxCommand c = testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0.0, 0, "open-rawiq:127.0.0.1:5555?rate=48000");
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const int32_t n = api()->submit(remote.raw(), &c, 1, &sr);
        std::int32_t status = n < 0 ? n : sr.status;
        for (int pass = 0; pass < 40 && status == FOXAPI_OK; ++pass) {
            r.pump(1);
            FoxCommandResult res[8];
            for (auto& x : res) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(remote.raw(), res, 8);
            for (int32_t i = 0; i < got; ++i) {
                if (res[i].ticket == sr.ticket) {
                    status = res[i].status;
                    pass = 99;
                }
            }
        }
        CHECK_EQ(status, FOXAPI_DENIED);
    }
    // a stream with nobody sending: the open fails with a sentence and the radio stays on the generator
    {
        nettest::StandInServer srv;
        CHECK(srv.start());   // accepts, then says nothing: the header never comes
        CHECK_EQ(r.add(2, "open-rawiq:127.0.0.1:" + std::to_string(srv.port()) + "?hdr=1&timeout=600").status(), FOXAPI_OK);
        CHECK(r.until([&] { return r.fact("radio.2.refusal") != "<absent>"; }, 30000));
        CHECK_EQ(r.fact("radio.2.source"), std::string("siggen"));
        std::printf("  no sender: %s\n", r.fact("radio.2.refusal").c_str());
        CHECK(r.fact("radio.2.refusal").find("header") != std::string::npos);
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RADIO_REMOVE, 1, 0.0, 2)).status(), FOXAPI_OK);
    }
}

void testC() {
    std::printf("C: a headerless u8 stream\n");
    Rig r;
    r.pump(10);
    nettest::StandInServer srv;
    CHECK(srv.start());
    Feeder feed(srv, 240000.0, -30000.0, true);
    CHECK_EQ(r.add(2, "open-rawiq:127.0.0.1:" + std::to_string(srv.port()) + "?rate=240000&fmt=u8&freq=7100000").status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.2.source") == "rawiq" && r.radio(2).sampleRateHz == 240000.0; }, 30000));
    CHECK_EQ(r.radio(2).centreHz, 7100000.0);
    CHECK(r.until([&] { return peakAt(r.spectrum(2), -30000.0, 4); }, 20000));
    CHECK(peakAt(r.spectrum(2), -30000.0, 4));
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RADIO_REMOVE, 1, 0.0, 2)).status(), FOXAPI_OK);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_rawiq\n");
    engine_api_test::hermetic("rawiq");
    Access::installHooks();
    testA();
    testB();
    testC();
    return check::finish("test_engine_api_rawiq");
}
