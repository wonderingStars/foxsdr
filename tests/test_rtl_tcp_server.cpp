// Tests for net/rtl_tcp_server.hpp (screen lane S4b, G3-24: the rtl_tcp server). Every test talks to the server over a REAL loopback socket from a stand-in client built from the
// protocol description in the header (a real client program is UNVERIFIED); nothing here reaches an outside host and no radio is opened.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/rtl_tcp_server.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "net_test_support.hpp"
#include "test_check.hpp"

using namespace cascade::net;
using nettest::Client;
using nettest::msSince;

namespace {

RtlTcpConfig cfg0() {
    RtlTcpConfig c;
    c.port = 0;
    return c;
}

struct Seen {
    std::mutex m;
    std::vector<std::pair<int, std::uint32_t>> cmds;
    RtlTcpServer::CommandFn fn() {
        return [this](std::uint8_t c, std::uint32_t p) {
            std::lock_guard<std::mutex> g(m);
            cmds.emplace_back(static_cast<int>(c), p);
        };
    }
    std::size_t count() {
        std::lock_guard<std::mutex> g(m);
        return cmds.size();
    }
};

// feeds the server with a constant sample until told to stop (a stand-in radio)
struct Feeder {
    RtlTcpServer& s;
    float i, q;
    std::atomic<bool> stop{false};
    std::thread th;
    Feeder(RtlTcpServer& srv, float ii, float qq, int sleepMs = 1) : s(srv), i(ii), q(qq) {
        th = std::thread([this, sleepMs] {
            std::vector<float> buf(2 * 2048);
            for (std::size_t k = 0; k < 2048; ++k) { buf[2 * k] = i; buf[2 * k + 1] = q; }
            while (!stop.load()) {
                s.submit(buf.data(), 2048);
                if (sleepMs > 0) { std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs)); } else { std::this_thread::yield(); }
            }
        });
    }
    ~Feeder() { stop.store(true); if (th.joinable()) { th.join(); } }
};

void testRefusals() {
    std::string err;
    Seen seen;
    {
        RtlTcpConfig c = cfg0();
        c.bindAddress = "0.0.0.0";
        RtlTcpServer s(c);
        CHECK(!s.start(seen.fn(), err) && err.find("every network interface") != std::string::npos && !s.running());
    }
    {
        RtlTcpConfig c = cfg0();
        c.bindAddress = "localhost";
        RtlTcpServer s(c);
        CHECK(!s.start(seen.fn(), err) && !s.running());
    }
    {
        RtlTcpConfig c = cfg0();
        c.clientQueueBytes = 100;
        RtlTcpServer s(c);
        CHECK(!s.start(seen.fn(), err) && !s.running());
    }
    {
        RtlTcpConfig c = cfg0();
        c.pollMs = 0;
        RtlTcpServer s(c);
        CHECK(!s.start(seen.fn(), err) && !s.running());
    }
    {
        RtlTcpServer s(cfg0());
        CHECK(s.start(seen.fn(), err) && s.running() && s.port() > 0);
        CHECK(!s.start(seen.fn(), err) && s.running());   // twice
        s.stop();
        CHECK(!s.running());
        s.stop();
    }
}

void testHeaderAndStream() {
    std::uint8_t h[kRtlTcpHeaderBytes];
    RtlTcpServer::header(5, 29, h);
    CHECK(std::memcmp(h, "RTL0", 4) == 0 && h[4] == 0 && h[5] == 0 && h[6] == 0 && h[7] == 5 && h[8] == 0 && h[9] == 0 && h[10] == 0 && h[11] == 29);
    std::string err;
    Seen seen;
    RtlTcpServer s(cfg0());
    CHECK(s.start(seen.fn(), err));
    // nothing is queued while nobody is connected
    float z[8] = {};
    CHECK(s.submit(z, 4) == 0 && s.stats().samplesIn == 0);
    Client c;
    CHECK(c.connect(static_cast<std::uint16_t>(s.port())));
    Feeder f(s, 0.0f, -1.0f);
    CHECK(c.waitBytes(kRtlTcpHeaderBytes + 20000, 4000));
    // the header: "RTL0", tuner 0, gains 0 (it does not pretend to be an RTL-SDR)
    CHECK(c.got.size() >= kRtlTcpHeaderBytes && std::memcmp(c.got.data(), "RTL0", 4) == 0);
    bool zeroes = c.got.size() >= kRtlTcpHeaderBytes;
    for (std::size_t k = 4; k < kRtlTcpHeaderBytes && k < c.got.size(); ++k) { zeroes = zeroes && c.got[k] == 0; }
    CHECK(zeroes);
    // the samples: I = 0 is 128, Q = -1 is 0 (unsigned, zero at 128), interleaved, nothing between
    std::size_t good = 0, total = 0;
    for (std::size_t k = kRtlTcpHeaderBytes; k + 1 < c.got.size() && total < 5000; k += 2, ++total) {
        if ((c.got[k] == 128 || c.got[k] == 127) && c.got[k + 1] == 0) { ++good; }
    }
    CHECK(total > 4000 && good == total);
    const RtlTcpStats st = s.stats();
    CHECK(st.accepted == 1 && st.clients == 1 && st.samplesIn > 0 && st.bytesSent >= kRtlTcpHeaderBytes + 20000);
    f.stop.store(true);
    f.th.join();
    const auto t0 = nettest::Clock::now();
    s.stop();
    CHECK(msSince(t0) < 2000);
    CHECK(c.waitEof(3000));
}

void testCommands() {
    std::string err;
    Seen seen;
    RtlTcpServer s(cfg0());
    CHECK(s.start(seen.fn(), err));
    Client c;
    CHECK(c.connect(static_cast<std::uint16_t>(s.port())));
    // a whole command, then one split across two writes, then two in one write
    const std::uint8_t setf[5] = {0x01, 0x05, 0xF5, 0xE1, 0x00};   // 100000000 Hz
    CHECK(c.sendBytes(setf, 5) == 5);
    const std::uint8_t rate[5] = {0x02, 0x00, 0x1E, 0x84, 0x80};    // 2000000
    CHECK(c.sendBytes(rate, 3) == 3);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(c.sendBytes(rate + 3, 2) == 2);
    std::uint8_t two[10] = {0x03, 0, 0, 0, 1, 0x0E, 0, 0, 0, 1};
    CHECK(c.sendBytes(two, 10) == 10);
    for (int i = 0; i < 100 && seen.count() < 4; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    {
        std::lock_guard<std::mutex> g(seen.m);
        CHECK(seen.cmds.size() == 4);
        CHECK(seen.cmds.size() >= 1 && seen.cmds[0] == std::make_pair(0x01, 100000000u));
        CHECK(seen.cmds.size() >= 2 && seen.cmds[1] == std::make_pair(0x02, 2000000u));
        CHECK(seen.cmds.size() >= 4 && seen.cmds[2].first == 0x03 && seen.cmds[3].first == 0x0E);
    }
    // an unknown command byte is counted and not delivered
    const std::uint8_t bad[5] = {0x77, 1, 2, 3, 4};
    CHECK(c.sendBytes(bad, 5) == 5);
    for (int i = 0; i < 100 && s.stats().unknownCommands < 1; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    CHECK(s.stats().unknownCommands == 1 && seen.count() == 4);
    CHECK(s.stats().commands == 5);
    // a command left half-sent when the client goes does not leak into the next client
    c.sendBytes(setf, 3);
    c.close();
    for (int i = 0; i < 100 && s.stats().clients != 0; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    Client d;
    CHECK(d.connect(static_cast<std::uint16_t>(s.port())));
    const std::uint8_t setf2[5] = {0x01, 0x08, 0x00, 0x00, 0x00};   // 134217728
    CHECK(d.sendBytes(setf2, 5) == 5);
    for (int i = 0; i < 100 && seen.count() < 5; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    {
        std::lock_guard<std::mutex> g(seen.m);
        CHECK(seen.cmds.size() == 5 && seen.cmds[4] == std::make_pair(0x01, 134217728u));
    }
    s.stop();
}

void testHostileClients() {
    std::string err;
    Seen seen;
    RtlTcpConfig cfg = cfg0();
    cfg.stallMs = 700;
    RtlTcpServer s(cfg);
    CHECK(s.start(seen.fn(), err));
    // a second client while the first is connected is closed at once, and the first is not disturbed
    Client first, second;
    CHECK(first.connect(static_cast<std::uint16_t>(s.port())));
    for (int i = 0; i < 100 && s.stats().clients != 1; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(second.connect(static_cast<std::uint16_t>(s.port())));
    CHECK(second.waitEof(2000));
    CHECK(s.stats().refusedBusy >= 1 && s.stats().clients == 1);
    // garbage: 4 MiB of arbitrary bytes is read in bounded turns; the server stays up and keeps the stream
    {
        std::vector<std::uint8_t> junk(256 * 1024);
        for (std::size_t k = 0; k < junk.size(); ++k) { junk[k] = static_cast<std::uint8_t>(k * 131u + 7u); }
        Feeder f(s, 0.5f, 0.5f);
        for (int k = 0; k < 16; ++k) { (void)first.sendBytes(junk.data(), junk.size()); }
        CHECK(first.waitBytes(kRtlTcpHeaderBytes + 10000, 4000));
        CHECK(s.running() && s.stats().bytesRead > 0);
    }
    first.close();
    for (int i = 0; i < 200 && s.stats().clients != 0; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    // a client that connects and never reads is dropped (the stall), and memory stays bounded while the producer keeps feeding
    {
        Client idle;
        CHECK(idle.connect(static_cast<std::uint16_t>(s.port())));
        cascade::net::sock::setBuffers(idle.s, 4096, 0);
        Feeder f(s, 0.25f, -0.25f);
        const auto t0 = nettest::Clock::now();
        while (msSince(t0) < 20000 && s.stats().droppedStalled < 1) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        CHECK(s.stats().droppedStalled >= 1);
    }
    // a stuck client that is never dropped holds at most its queue: past it the samples are dropped and counted, the client stays
    {
        RtlTcpConfig cfg2 = cfg0();
        cfg2.clientQueueBytes = 64 * 1024;
        cfg2.stallMs = 0;
        RtlTcpServer t(cfg2);
        CHECK(t.start(seen.fn(), err));
        Client idle;
        CHECK(idle.connect(static_cast<std::uint16_t>(t.port())));
        cascade::net::sock::setBuffers(idle.s, 4096, 0);
        Feeder f(t, 0.25f, -0.25f, 0);
        const auto t0 = nettest::Clock::now();
        while (msSince(t0) < 20000 && t.stats().samplesDropped < 100000) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        CHECK(t.stats().samplesDropped >= 100000 && t.stats().clients == 1 && t.stats().droppedStalled == 0);
    }
    // the producer is not slowed by a stuck client: a million complex samples are offered in well under the time a wait would take
    {
        Client idle;
        CHECK(idle.connect(static_cast<std::uint16_t>(s.port())));
        std::vector<float> buf(2 * 4096, 0.1f);
        const auto t0 = nettest::Clock::now();
        for (int k = 0; k < 250; ++k) { s.submit(buf.data(), 4096); }
        CHECK(msSince(t0) < 1500);
    }
    // a thousand connections, one after the other: nothing grows, the server still serves a good client
    for (int k = 0; k < 1000; ++k) {
        Client c;
        if (c.connect(static_cast<std::uint16_t>(s.port()), 300)) { if (k % 3 == 0) { std::uint8_t g[7] = {1, 2, 3, 4, 5, 6, 7}; c.sendBytes(g, 7); } }
    }
    for (int i = 0; i < 300 && s.stats().clients != 0; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(s.stats().clients == 0);
    Client ok;
    bool served = false;
    for (int attempt = 0; attempt < 30 && !served; ++attempt) {
        if (ok.connect(static_cast<std::uint16_t>(s.port())) && ok.waitBytes(kRtlTcpHeaderBytes, 1000)) { served = true; }
        else { ok.close(); ok.got.clear(); ok.eof = false; std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    }
    CHECK(served && std::memcmp(ok.got.data(), "RTL0", 4) == 0);
    const auto t0 = nettest::Clock::now();
    s.stop();
    CHECK(msSince(t0) < 2000);
}

}  // namespace

int main() {
    testRefusals();
    testHeaderAndStream();
    testCommands();
    testHostileClients();
    return testSummary("test_rtl_tcp_server");
}
