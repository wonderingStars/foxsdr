// test_s4b_sec_peers.cpp - security lane S4b-sec, finding R7: the rtl_tcp server and the audio-over-TCP server have no password to ask for, so beyond this computer they are widened only to the
// client addresses the person typed (net/peer_allow.hpp). Real loopback sockets, stand-in clients.
//
//   P  the list: the forms it accepts, the forms it refuses (every address, a /0, a multicast address, junk, too many), what it allows
//   F  the fan-out / TCP audio: a client whose address is not listed is closed before it is sent or reads a byte; a listed one is served; no list serves every client as before
//   R  rtl_tcp: the same, and an unlisted address never takes the one place; a connection that does not begin with an rtl_tcp command (a web page's request to this port) is dropped before
//      any later 5-byte block could be read as a retune
//   S  what a client can make the rtl_tcp server do is exactly one command (the frequency): every one of the 256 command bytes is sent, and the host function sees only 0x01..0x0E
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "net/peer_allow.hpp"
#include "net/rtl_tcp_server.hpp"
#include "net/tcp_audio.hpp"
#include "net/tcp_fanout.hpp"
#include "net_test_support.hpp"
#include "test_check.hpp"

using namespace cascade::net;
using nettest::Client;
using nettest::msSince;

namespace {

bool parses(const std::string& text) {
    PeerAllow a;
    std::string why;
    return PeerAllow::parse(text, a, why);
}

std::string whyNot(const std::string& text) {
    PeerAllow a;
    std::string why;
    return PeerAllow::parse(text, a, why) ? std::string() : why;
}

void testList() {
    PeerAllow a;
    std::string why;
    CHECK(PeerAllow::parse("192.168.1.20, 192.168.1.0/30;10.0.0.0/8\n127.0.0.1", a, why) && a.size() == 4 && why.empty());
    CHECK(a.allowsText("192.168.1.20") && a.allowsText("192.168.1.2") && a.allowsText("10.200.3.4") && a.allowsText("127.0.0.1"));
    CHECK(!a.allowsText("192.168.1.21") && !a.allowsText("192.168.1.4") && !a.allowsText("11.0.0.1") && !a.allowsText("8.8.8.8"));
    CHECK(!a.allowsText("") && !a.allowsText("not an address") && !a.allowsText("192.168.1.2.3"));
    CHECK(PeerAllow::parse("192.168.1.77/24", a, why) && a.allowsText("192.168.1.1") && a.allowsText("192.168.1.254") && !a.allowsText("192.168.2.1"));   // host bits are ignored
    CHECK(PeerAllow::parse("  192.168.1.20  ", a, why) && a.size() == 1);
    // refused: nothing that means "every address", nothing that is not a computer, nothing that is not an address
    for (const char* bad : {"", "   ", ",,;", "0.0.0.0", "0.0.0.0/0", "10.0.0.0/0", "10.0.0.0/7", "10.0.0.0/33", "10.0.0.0/", "10.0.0.0/x", "10.0.0.0/+8", "*", "any", "all", "0.1.2.3", "255.255.255.255",
                            "224.0.0.1", "239.255.255.250", "192.168.1", "192.168.1.256", "192.168.01.1", "192.168.1.1.1", "localhost", "example.com", "::1", "192.168.1.20 evil", "1.2.3.4/8/8"}) {
        CHECK(!parses(bad));
        CHECK(!whyNot(bad).empty());
    }
    // a failed parse leaves nothing behind that could be mistaken for a list
    PeerAllow b;
    std::string w2;
    CHECK(!PeerAllow::parse("192.168.1.20, nonsense", b, w2) && b.empty() && !b.allowsText("192.168.1.20"));
    // bounds: 16 entries, 400 characters
    std::string sixteen, seventeen;
    for (int i = 1; i <= 16; ++i) { sixteen += "10.0.0." + std::to_string(i) + ","; }
    seventeen = sixteen + "10.0.0.99";
    CHECK(parses(sixteen) && !parses(seventeen));
    CHECK(!parses(std::string(401, ' ') + "10.0.0.1"));
}

bool eofWithin(Client& c, int ms) { return c.waitEof(ms); }

void testFanoutAndAudio() {
    // not listed: closed before anything is sent
    {
        FanoutConfig fc;
        fc.port = 0;
        fc.greeting = {'H', 'I'};
        std::string why;
        PeerAllow none;
        CHECK(PeerAllow::parse("10.9.9.9", fc.allowedPeers, why));
        TcpFanout f(fc);
        std::string err;
        CHECK(f.start(err));
        Client c;
        CHECK(c.connect(static_cast<std::uint16_t>(f.port())));
        CHECK(eofWithin(c, 2000));
        CHECK(c.got.empty());                                   // not even the greeting
        CHECK(f.stats().refusedPeer == 1 && f.stats().accepted == 0 && f.stats().clients == 0);
        const char junk[] = "x";
        f.publish(junk, 1);                                     // and nothing was queued for it
        f.stop();
    }
    // listed (the loopback network): served
    {
        FanoutConfig fc;
        fc.port = 0;
        fc.greeting = {'H', 'I'};
        std::string why;
        CHECK(PeerAllow::parse("127.0.0.0/8", fc.allowedPeers, why));
        TcpFanout f(fc);
        std::string err;
        CHECK(f.start(err));
        Client c;
        CHECK(c.connect(static_cast<std::uint16_t>(f.port())));
        CHECK(c.waitBytes(2, 2000) && c.got[0] == 'H' && c.got[1] == 'I');
        CHECK(f.stats().refusedPeer == 0 && f.stats().accepted == 1);
        f.stop();
    }
    // no list: every client is served, as before
    {
        FanoutConfig fc;
        fc.port = 0;
        fc.greeting = {'H', 'I'};
        TcpFanout f(fc);
        std::string err;
        CHECK(f.start(err));
        Client c;
        CHECK(c.connect(static_cast<std::uint16_t>(f.port())));
        CHECK(c.waitBytes(2, 2000));
        f.stop();
    }
    // the audio server passes the list on
    {
        TcpAudioConfig tc;
        tc.port = 0;
        std::string why;
        CHECK(PeerAllow::parse("10.9.9.9", tc.allowedPeers, why));
        TcpAudioServer s;
        std::string err;
        CHECK(s.start(tc, [](std::uint64_t&, float* dst, std::size_t n) { for (std::size_t i = 0; i < n; ++i) { dst[i] = 0.1f; } return static_cast<int>(n); }, err));
        Client c;
        CHECK(c.connect(static_cast<std::uint16_t>(s.port())));
        c.pump(400);
        CHECK(c.got.empty() && c.eof);
        CHECK(s.stats().refusedPeer == 1 && s.stats().clients == 0);
        s.stop();
    }
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
    std::vector<std::pair<int, std::uint32_t>> all() {
        std::lock_guard<std::mutex> g(m);
        return cmds;
    }
};

std::string cmdBytes(std::uint8_t c, std::uint32_t p) {
    std::string s;
    s.push_back(static_cast<char>(c));
    s.push_back(static_cast<char>(p >> 24));
    s.push_back(static_cast<char>(p >> 16));
    s.push_back(static_cast<char>(p >> 8));
    s.push_back(static_cast<char>(p));
    return s;
}

void testRtl() {
    // an address that is not listed never gets the place, nor the header, and is never read
    {
        RtlTcpConfig rc;
        rc.port = 0;
        std::string why;
        CHECK(PeerAllow::parse("10.9.9.9", rc.allowedPeers, why));
        RtlTcpServer s(rc);
        Seen seen;
        std::string err;
        CHECK(s.start(seen.fn(), err));
        Client c;
        CHECK(c.connect(static_cast<std::uint16_t>(s.port())));
        const std::string cmd = cmdBytes(0x01, 145000000u);
        c.sendBytes(cmd.data(), cmd.size());
        CHECK(eofWithin(c, 2000));
        CHECK(c.got.empty());                                   // no 12-byte header
        CHECK(s.stats().refusedPeer == 1 && s.stats().accepted == 0 && s.stats().clients == 0 && s.stats().commands == 0 && seen.count() == 0);
        s.stop();
    }
    // listed: served, its command heard
    {
        RtlTcpConfig rc;
        rc.port = 0;
        std::string why;
        CHECK(PeerAllow::parse("127.0.0.1", rc.allowedPeers, why));
        RtlTcpServer s(rc);
        Seen seen;
        std::string err;
        CHECK(s.start(seen.fn(), err));
        Client c;
        CHECK(c.connect(static_cast<std::uint16_t>(s.port())));
        CHECK(c.waitBytes(12, 2000) && c.got[0] == 'R');
        const std::string cmd = cmdBytes(0x01, 145000000u);
        c.sendBytes(cmd.data(), cmd.size());
        for (int i = 0; i < 100 && seen.count() < 1; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        CHECK(seen.count() == 1 && s.stats().refusedPeer == 0);
        s.stop();
    }
    // a web page's request to this port: the first thing on the connection is not an rtl_tcp command, so it is dropped; a block that lines up after the headers is never read as a retune
    {
        RtlTcpConfig rc;
        rc.port = 0;
        RtlTcpServer s(rc);
        Seen seen;
        std::string err;
        CHECK(s.start(seen.fn(), err));
        Client c;
        CHECK(c.connect(static_cast<std::uint16_t>(s.port())));
        std::string req = "POST /x HTTP/1.1\r\n\r\n";   // 20 bytes: four whole 5-byte blocks, and then the page's body starts on a block boundary
        CHECK(req.size() % 5 == 0);
        req += cmdBytes(0x01, 7000000u);
        req += cmdBytes(0x01, 7000001u);
        c.sendBytes(req.data(), req.size());
        for (int i = 0; i < 100 && s.stats().refusedProtocol < 1; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        CHECK(s.stats().refusedProtocol == 1);
        CHECK(seen.count() == 0);
        c.pump(300);
        CHECK(c.eof);
        // the server closes the socket and THEN counts the client out: the client can see the end of the stream a moment before the count drops (engine merge 13: this failed alone in a repeat run).
        // The expectation is unchanged (no client is left); it is waited for, up to two seconds.
        for (int i = 0; i < 100 && s.stats().clients != 0; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        CHECK(s.stats().clients == 0);
        // and the next, honest client is served at once
        Client d;
        CHECK(d.connect(static_cast<std::uint16_t>(s.port())));
        CHECK(d.waitBytes(12, 2000));
        const std::string good = cmdBytes(0x01, 100000000u);
        d.sendBytes(good.data(), good.size());
        for (int i = 0; i < 100 && seen.count() < 1; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        CHECK(seen.count() == 1 && seen.all()[0].second == 100000000u);
        s.stop();
    }
}

// What a client can make the server do: send every command byte there is, each with a parameter that would be a mistake to honour; the host function is called for the 14 numbers the
// protocol has and for nothing else, with the parameter the client sent (the engine's function then uses 0x01 alone: see test_engine_api_s4b_rtltcp and test_s4b_sec_servers).
void testSurface() {
    RtlTcpConfig rc;
    rc.port = 0;
    RtlTcpServer s(rc);
    Seen seen;
    std::string err;
    CHECK(s.start(seen.fn(), err));
    Client c;
    CHECK(c.connect(static_cast<std::uint16_t>(s.port())));
    CHECK(c.waitBytes(12, 2000));
    std::string all = cmdBytes(0x01, 100000000u);   // an honest first command
    for (int b = 0; b < 256; ++b) { all += cmdBytes(static_cast<std::uint8_t>(b), 0xDEADBEEFu); }
    c.sendBytes(all.data(), all.size());
    for (int i = 0; i < 200 && s.stats().commands < 257; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    CHECK(s.stats().commands == 257);
    std::set<int> heard;
    for (const auto& p : seen.all()) { heard.insert(p.first); }
    CHECK(heard.size() == 14);
    for (int b = 1; b <= 14; ++b) { CHECK(heard.count(b) == 1); }
    CHECK(heard.count(0) == 0 && heard.count(15) == 0 && heard.count(0x80) == 0 && heard.count(255) == 0);
    CHECK(s.stats().unknownCommands == 256 - 14);
    s.stop();
}

}  // namespace

int main() {
    testList();
    testFanoutAndAudio();
    testRtl();
    testSurface();
    return testSummary("test_s4b_sec_peers");
}
