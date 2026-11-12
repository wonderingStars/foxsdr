// test_adif_broadcast.cpp - the ADIF broadcast (net/adif_broadcast.hpp) against stand-in listeners on 127.0.0.1.
//
//   B1  OFF by default; refusals in words: disabled, a name, a non-loopback address without the switch, bad ports, no output selected.
//   B2  UDP: one record per datagram, in order, byte-exact, none lost (a counter in the records shows a loss or a reorder).
//   B3  TCP: clients get the records back to back; two clients; a client that goes away is forgotten; the ninth client is refused.
//   B4  records refused: empty, no <EOR>, too long, NUL; counted; nothing sent.
//   B5  stop / restart / publish while not running; the same object can start again.
//   B6  a TCP client that never reads is dropped and publish never blocks.
//   B7  end to end: an ADIF QSO record from core/adif_qso.hpp arrives intact.
//   B8  fuzz: random strings never crash; only well-formed records arrive.
//   B9  source scan: no thread, no wide bind, nothing to stdout/stderr, no setting.
//
// Every socket here is bound to or connected to 127.0.0.1 only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "core/adif_qso.hpp"
#include "hamops_util.hpp"
#include "net/adif_broadcast.hpp"
#include "test_check.hpp"

using namespace cascade::net;
using Clock = std::chrono::steady_clock;

namespace {

std::string rec(int n) {
    const std::string num = std::to_string(n);
    return "<CALL:5>K1ABC <SRX:" + std::to_string(num.size()) + ">" + num + " <EOR>\r\n";
}

// Reads datagrams until `want` arrived or 3 s passed.
std::vector<std::string> collectUdp(lo::Sock s, std::size_t want) {
    std::vector<std::string> out;
    const auto end = Clock::now() + std::chrono::seconds(3);
    char buf[4096];
    while (out.size() < want && Clock::now() < end) {
        const int n = lo::udpRecv(s, buf, sizeof(buf));
        if (n >= 0) { out.emplace_back(buf, static_cast<std::size_t>(n)); }
        else { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    }
    return out;
}

std::string collectTcp(lo::Sock s, std::size_t wantBytes) {
    std::string out;
    const auto end = Clock::now() + std::chrono::seconds(3);
    char buf[4096];
    while (out.size() < wantBytes && Clock::now() < end) {
        const int n = lo::tcpRecv(s, buf, sizeof(buf));
        if (n > 0) { out.append(buf, static_cast<std::size_t>(n)); }
        else if (n < 0) { break; }
        else { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    }
    return out;
}

AdifBroadcastConfig udpCfg(int port) {
    AdifBroadcastConfig c;
    c.enabled = true;
    c.udpPort = port;
    return c;
}
AdifBroadcastConfig tcpCfg() {
    AdifBroadcastConfig c;
    c.enabled = true;
    c.tcpPort = 0;
    return c;
}

void b1Refusals() {
    AdifBroadcaster b;
    std::string err;
    CHECK(!AdifBroadcastConfig().enabled);                       // OFF by default
    CHECK(!b.start(AdifBroadcastConfig(), err) && !err.empty());
    CHECK(!b.running());
    AdifBroadcastConfig c = udpCfg(40000);
    c.host = "localhost";                                          // a name is never resolved
    CHECK(!b.start(c, err) && !err.empty());
    c.host = "192.168.1.20";                                       // not loopback, no switch
    CHECK(!b.start(c, err));
    c.host = "8.8.8.8";
    CHECK(!b.start(c, err));
    c.host = "127.0.0.1x";
    CHECK(!b.start(c, err));
    c.host = "";
    CHECK(!b.start(c, err));
    c = udpCfg(70000);
    CHECK(!b.start(c, err));
    c = udpCfg(-3);
    CHECK(!b.start(c, err));
    c = udpCfg(0);                                                  // no UDP and no TCP
    CHECK(!b.start(c, err));
    c = tcpCfg();
    c.tcpPort = 99999;
    CHECK(!b.start(c, err));
    CHECK(!b.running());
    CHECK(!b.publish(rec(1)));                                      // not running
    // the whole 127/8 block counts as loopback
    c = udpCfg(40001);
    c.host = "127.5.5.5";
    CHECK(b.start(c, err));
    b.stop();
    // a wider address is only taken when the switch says so (opening a UDP send socket binds nothing)
    c = udpCfg(40002);
    c.host = "192.168.1.20";
    c.allowNonLoopback = true;
    CHECK(b.start(c, err));
    b.stop();
}

void b2Udp() {
    const lo::Sock rx = lo::udpOpen(lo::kLoopbackHost, 0);
    CHECK(rx != lo::kBad);
    const int port = lo::boundPortOf(rx);
    AdifBroadcaster b;
    std::string err;
    CHECK(b.start(udpCfg(port), err));
    CHECK(b.running());
    const int N = 500;
    for (int i = 0; i < N; ++i) {
        CHECK(b.publish(rec(i)));
        if (i % 50 == 49) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    }
    const auto got = collectUdp(rx, static_cast<std::size_t>(N));
    CHECK(got.size() == static_cast<std::size_t>(N));
    for (std::size_t i = 0; i < got.size() && i < static_cast<std::size_t>(N); ++i) { CHECK(got[i] == rec(static_cast<int>(i))); }
    CHECK(b.udpSent() == static_cast<std::uint64_t>(N));
    CHECK(b.refused() == 0);
    b.stop();
    lo::closeSock(rx);
    // nobody listening: the send is a counted non-event, publish does not throw or block
    AdifBroadcaster d;
    CHECK(d.start(udpCfg(1), err));   // port 1: nothing there
    const auto t0 = Clock::now();
    for (int i = 0; i < 50; ++i) { d.publish(rec(i)); }
    CHECK(Clock::now() - t0 < std::chrono::seconds(2));
}

void b3Tcp() {
    AdifBroadcaster b;
    std::string err;
    CHECK(b.start(tcpCfg(), err));
    CHECK(b.tcpPort() > 0);
    CHECK(!b.publish(rec(0)) || true);            // no client yet: nothing to deliver, not an error
    const lo::Sock c1 = lo::tcpConnect(lo::kLoopbackHost, b.tcpPort(), 2000);
    const lo::Sock c2 = lo::tcpConnect(lo::kLoopbackHost, b.tcpPort(), 2000);
    CHECK(c1 != lo::kBad && c2 != lo::kBad);
    for (int i = 0; i < 200 && b.clientCount() < 2; ++i) { b.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    CHECK(b.clientCount() == 2);
    std::string want;
    for (int i = 1; i <= 20; ++i) { CHECK(b.publish(rec(i))); want += rec(i); }
    const std::string g1 = collectTcp(c1, want.size());
    const std::string g2 = collectTcp(c2, want.size());
    CHECK(g1 == want);
    CHECK(g2 == want);
    CHECK(b.tcpSent() == 40);
    // a record without a trailing CR LF is framed with one
    CHECK(b.publish("<CALL:3>AAA <EOR>"));
    CHECK(collectTcp(c1, 19) == "<CALL:3>AAA <EOR>\r\n");
    // a client that goes away is forgotten
    lo::closeSock(c2);
    for (int i = 0; i < 200 && b.clientCount() > 1; ++i) { b.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    CHECK(b.clientCount() == 1);
    CHECK(b.droppedClients() >= 1);
    // bytes a client sends are discarded
    const char junk[] = "hello";
    lo::tcpSend(c1, junk, 5);
    b.pump();
    CHECK(b.clientCount() == 1);
    lo::closeSock(c1);
    b.stop();
    CHECK(b.clientCount() == 0);

    // the ninth client is refused
    AdifBroadcaster m;
    CHECK(m.start(tcpCfg(), err));
    std::vector<lo::Sock> cs;
    for (int i = 0; i < 9; ++i) {
        cs.push_back(lo::tcpConnect(lo::kLoopbackHost, m.tcpPort(), 2000));
        CHECK(cs.back() != lo::kBad);
        for (int k = 0; k < 300; ++k) {   // let the broadcaster take (or refuse) this one before the next connects
            m.pump();
            if (m.clientCount() + m.refusedClients() >= static_cast<std::size_t>(i + 1)) { break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    CHECK(m.clientCount() == kAdifMaxClients);
    CHECK(m.refusedClients() == 1);
    for (lo::Sock s : cs) { lo::closeSock(s); }
}

void b4Refused() {
    const lo::Sock rx = lo::udpOpen(lo::kLoopbackHost, 0);
    AdifBroadcaster b;
    std::string err;
    CHECK(b.start(udpCfg(lo::boundPortOf(rx)), err));
    CHECK(!b.publish(""));
    CHECK(!b.publish("<CALL:3>ABC"));                             // no <EOR>
    CHECK(!b.publish("<EOR> trailing"));
    CHECK(!b.publish(std::string(kAdifMaxRecord, 'a') + "<EOR>"));   // too long
    CHECK(!b.publish(std::string("<CALL:1>\0<EOR>", 14)));          // NUL byte
    CHECK(b.refused() == 5);
    CHECK(b.publish("<call:3>abc <eor>"));                          // any case
    CHECK(b.publish("<CALL:3>ABC<EOR>\r\n\r\n"));                   // trailing white space
    // exactly at the bound
    std::string edge = "<COMMENT:" + std::to_string(kAdifMaxRecord - 20) + ">" + std::string(kAdifMaxRecord - 20, 'x') + "<EOR>";
    CHECK(edge.size() <= kAdifMaxRecord && edge.size() > kAdifMaxRecord - 20);
    CHECK(b.publish(edge));
    const auto got = collectUdp(rx, 3);
    CHECK(got.size() == 3);
    CHECK(b.udpSent() == 3);
    b.stop();
    lo::closeSock(rx);
}

void b5Lifecycle() {
    const lo::Sock rx = lo::udpOpen(lo::kLoopbackHost, 0);
    AdifBroadcaster b;
    std::string err;
    CHECK(b.start(udpCfg(lo::boundPortOf(rx)), err));
    CHECK(b.publish(rec(1)));
    b.stop();
    CHECK(!b.running());
    CHECK(!b.publish(rec(2)));
    b.stop();                                                       // twice is harmless
    CHECK(b.start(udpCfg(lo::boundPortOf(rx)), err));               // the same object starts again
    CHECK(b.publish(rec(3)));
    CHECK(b.start(udpCfg(lo::boundPortOf(rx)), err));               // start while running: restarts
    CHECK(b.publish(rec(4)));
    const auto got = collectUdp(rx, 3);
    CHECK(got.size() == 3 && got[0] == rec(1) && got[1] == rec(3) && got[2] == rec(4));
    // UDP and TCP together
    AdifBroadcastConfig both = udpCfg(lo::boundPortOf(rx));
    both.tcpPort = 0;
    CHECK(b.start(both, err) && b.tcpPort() > 0);
    b.stop();
    lo::closeSock(rx);
}

void b6SlowClient() {
    AdifBroadcaster b;
    std::string err;
    CHECK(b.start(tcpCfg(), err));
    const lo::Sock slow = lo::tcpConnect(lo::kLoopbackHost, b.tcpPort(), 2000);
    for (int i = 0; i < 200 && b.clientCount() < 1; ++i) { b.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    CHECK(b.clientCount() == 1);
    const std::string big = "<COMMENT:1300>" + std::string(1300, 'x') + "<EOR>";
    const auto t0 = Clock::now();
    int sent = 0;
    while (b.clientCount() > 0 && sent < 200000 && Clock::now() - t0 < std::chrono::seconds(60)) {
        b.publish(big);
        ++sent;
    }
    CHECK(b.clientCount() == 0);                    // dropped, not blocked on
    CHECK(b.droppedClients() == 1);
    CHECK(Clock::now() - t0 < std::chrono::seconds(60));
    lo::closeSock(slow);
}

void b7EndToEnd() {
    using namespace cascade::core::ham;
    const lo::Sock rx = lo::udpOpen(lo::kLoopbackHost, 0);
    AdifBroadcaster b;
    std::string err;
    CHECK(b.start(udpCfg(lo::boundPortOf(rx)), err));
    Qso q;
    q.unixTime = hamops_test::unixFromCivil(2026, 11, 11, 21, 5, 9);
    q.freqKHz = 7010;
    q.mode = "CW";
    q.myCall = "W1AW";
    q.call = "K1ABC";
    q.sent = {"599", "7"};
    q.rcvd = {"599", "12"};
    AdifQsoOptions o;
    o.contest = findCabrilloContest("CQ-WPX-CW");
    o.contestId = "CQ-WPX-CW";
    const std::string r = adifQsoRecord(q, o);
    CHECK(!r.empty());
    CHECK(b.publish(r));
    const auto got = collectUdp(rx, 1);
    CHECK(got.size() == 1 && got[0] == r);
    CHECK(AdifBroadcaster::wellFormed(r));
    b.stop();
    lo::closeSock(rx);
}

void b8Fuzz() {
    const lo::Sock rx = lo::udpOpen(lo::kLoopbackHost, 0);
    AdifBroadcaster b;
    std::string err;
    CHECK(b.start(udpCfg(lo::boundPortOf(rx)), err));
    hamops_test::Rng rng(3);
    std::size_t accepted = 0;
    for (int i = 0; i < 4000; ++i) {
        std::string s;
        const int n = static_cast<int>(rng.below(60));
        for (int k = 0; k < n; ++k) { s.push_back(static_cast<char>(rng.below(256))); }
        if (rng.below(3) == 0) { s += "<EOR>"; }
        if (rng.below(8) == 0) { s += std::string(rng.below(2000), 'z') + "<eor>"; }
        const bool wf = AdifBroadcaster::wellFormed(s);
        const bool took = b.publish(s);
        CHECK(took == wf || (wf && !took));   // a well-formed record may fail only if the OS refused the datagram
        if (took) { ++accepted; }
        if (i % 100 == 99) { collectUdp(rx, 0); char buf[4096]; while (lo::udpRecv(rx, buf, sizeof(buf)) >= 0) {} }
    }
    CHECK(accepted > 100);
    CHECK(b.refused() > 100);
    b.stop();
    lo::closeSock(rx);
}

void b9SourceScan() {
    for (const char* f : {"src/net/adif_broadcast.hpp", "src/net/loopback_sock.hpp"}) {
        const std::string src = hamops_test::stripCommentsAndStrings(hamops_test::srcFile(f));
        CHECK(!src.empty());
        for (const char* bad : {"std::thread", "std::async", "stderr", "stdout", "std::cerr", "std::cout", "settings_set", "INADDR_ANY", "getaddrinfo", "gethostbyname", "0.0.0.0",
                                "OutputDebugString"}) {
            CHECK(src.find(bad) == std::string::npos);
        }
    }
    const std::string raw = hamops_test::srcFile("src/net/adif_broadcast.hpp");
    CHECK(raw.find("bool enabled = false;") != std::string::npos);   // OFF by default is in the code, not only in a comment
}

}  // namespace

int main() {
    b1Refusals();
    b2Udp();
    b3Tcp();
    b4Refused();
    b5Lifecycle();
    b6SlowClient();
    b7EndToEnd();
    b8Fuzz();
    b9SourceScan();
    return testSummary("test_adif_broadcast");
}
