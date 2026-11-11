// test_tci_server.cpp - THE TCI SERVER'S TRANSPORT over real sockets on the loopback interface (net/tci_server.*), against a stub backend
// that behaves like an engine. The independent client is cpp-httplib's WebSocketClient (a second implementation of RFC 6455); the hostile
// clients are raw sockets.
//
//   V1  THE START-UP EXCHANGE through httplib's client: the initialisation commands, READY, the state; a set is echoed; a second client sees
//       the first one's change pushed; a ping is answered; a close is answered.
//   V2  THE CAP: a fifth client is refused with an HTTP 503 that says why, and the refusal is BEFORE a session is made for it.
//   V3  PORT IN USE, in words (a program that holds the port exclusively); a restart on a free port works.
//   V4  HOSTILE CLIENTS: garbage, a plain HTTP request, an over-long request head, a handshake never finished, an unmasked frame, an
//       over-size message, text that is not UTF-8 - each answered or dropped, and the server serves the next client.
//   V5  A CLIENT THAT NEVER READS is dropped (not waited for) while it is being sent audio, and the engine side is never held up by it:
//       the longest backend call is timed, and another client is served promptly the whole time.
//   V6  A CLIENT THAT DISAPPEARS MID-STREAM: the connection and its session are gone, the server is still up.
//   V7  RECEIVE-ONLY over the wire; binary messages (a transmit audio block) are dropped without ending the connection.
//   V8  stop() with clients connected returns promptly and leaves no session.
//   V9  FUZZ: two hundred random connections; the server still answers.
//   V10 LOOPBACK BY DEFAULT: with bindAll off the server cannot be reached on this machine's other address; with it on, it can.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#if !defined(_WIN32)
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#endif
#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <ws2tcpip.h>
using SockT = SOCKET;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SockT = int;
constexpr SockT INVALID_SOCKET = -1;
#endif

#include "net/tci_server.hpp"
#include "net/ws_frame.hpp"
#include "test_check.hpp"

using namespace cascade::net;
using Clock = std::chrono::steady_clock;

#include "tci_test_client.hpp"

namespace {

// ---- the stub engine ---------------------------------------------------------------------------------------------------------------

struct World {
    std::mutex m;
    std::map<int, TciRadioView> v;
    std::vector<FoxCommand> all;
    std::atomic<int> backends{0};        // live backends (sessions)
    std::atomic<int> opened{0};
    std::atomic<std::int64_t> maxCallUs{0};   // the longest any backend call took
    std::atomic<int> audioFramesPerCall{480};
    std::atomic<bool> refuseSessions{false};
    World() {
        for (int r : {1, 2}) {
            TciRadioView x;
            x.running = true;
            x.centreHz = 7100000.0 * r;
            x.tunedHz = x.centreHz;
            x.sampleRateHz = 2400000.0;
            x.bandwidthHz = 2700.0;
            x.demod = FOXAPI_DEMOD_USB;
            v[r] = x;
        }
    }
};

struct StubBackend : TciBackend {
    explicit StubBackend(World& w) : w_(w) { ++w_.backends; ++w_.opened; }
    ~StubBackend() override { --w_.backends; }
    struct Timer {
        World& w;
        Clock::time_point t0 = Clock::now();
        ~Timer() {
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
            std::int64_t cur = w.maxCallUs.load();
            while (us > cur && !w.maxCallUs.compare_exchange_weak(cur, us)) {}
        }
    };
    std::vector<int> radios() override { Timer t{w_}; std::lock_guard<std::mutex> lk(w_.m); std::vector<int> r; for (auto& [n, x] : w_.v) { r.push_back(n); } return r; }
    bool readRadio(int radio, TciRadioView& out) override {
        Timer t{w_};
        std::lock_guard<std::mutex> lk(w_.m);
        const auto it = w_.v.find(radio);
        if (it == w_.v.end()) { return false; }
        out = it->second;
        return true;
    }
    int submit(const std::vector<FoxCommand>& c) override {
        Timer t{w_};
        std::lock_guard<std::mutex> lk(w_.m);
        for (const FoxCommand& x : c) {
            w_.all.push_back(x);
            auto set = [&](TciRadioView& r) {
                if (x.op == FOXAPI_OP_SET_CENTRE) { r.centreHz = x.num[0]; r.vfoOffsetHz = 0; r.tunedHz = r.centreHz; }
                if (x.op == FOXAPI_OP_SET_VFO_OFFSET) { r.vfoOffsetHz = x.num[0]; r.tunedHz = r.centreHz + r.vfoOffsetHz; }
                if (x.op == FOXAPI_OP_SET_MODE) { r.demod = static_cast<std::uint32_t>(x.ival[0]); }
            };
            if (x.target == FOXAPI_TARGET_ALL_RADIOS) { for (auto& [n, r] : w_.v) { set(r); } }
            else if (w_.v.count(static_cast<int>(x.target)) != 0) { set(w_.v[static_cast<int>(x.target)]); }
        }
        pending_ += static_cast<int>(c.size());
        return static_cast<int>(c.size());
    }
    int pollResults() override { Timer t{w_}; const int p = pending_; pending_ = 0; return p; }
    int readAudio(int, std::uint64_t& cursor, float* out, std::size_t cap) override {
        Timer t{w_};
        const std::size_t n = std::min<std::size_t>(cap, static_cast<std::size_t>(w_.audioFramesPerCall.load()));
        for (std::size_t i = 0; i < n; ++i) { out[i] = 0.25f * std::sin(0.05f * static_cast<float>(cursor + i)); }
        cursor += n;
        return static_cast<int>(n);
    }
    World& w_;
    int pending_ = 0;
};

TciBackendOpener opener(World& w) {
    return [&w](const std::string&) -> std::unique_ptr<TciBackend> {
        if (w.refuseSessions.load()) { return nullptr; }
        return std::make_unique<StubBackend>(w);
    };
}

struct Rig {
    World w;
    TciServer srv;
    std::string error;
    int port = 0;
    std::atomic<int> accepted{0};
    bool up(bool bindAll = false, int p = 0) {
        srv.setBackendOpener(opener(w));
        srv.setUsageCallback([this] { ++accepted; });
        if (!srv.start(static_cast<std::uint16_t>(p), bindAll, error)) { return false; }
        port = srv.boundPort();
        return true;
    }
};

bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

// ===================================================================================================================================

void testThroughHttplibClient() {
    Rig r;
    std::printf("   step 1\n");
    CHECK(r.up());
    std::printf("   step 2\n");
    CHECK(r.port != 0 && r.srv.running());
    std::printf("   step 3\n");
    httplib::ws::WebSocketClient a("ws://127.0.0.1:" + std::to_string(r.port) + "/");
    std::printf("   step 4\n");
    CHECK(a.is_valid());
    std::printf("   step 5\n");
    const auto res = a.connect();
    std::printf("   step 6\n");
    CHECK(static_cast<bool>(res));
    std::printf("   step 7\n");
    a.set_read_timeout(3, 0);
    std::printf("   step 8\n");
    // the start-up exchange: until READY, then the state of the receivers
    std::vector<std::string> first;
    std::printf("   step 9\n");
    std::string msg;
    std::printf("   step 10\n");
    for (int i = 0; i < 40 && a.read(msg) == httplib::ws::Text; ++i) {
        first.push_back(msg);
        if (has(first, "VFO:1,0,14200000;") && has(first, "TX_ENABLE:1,false;")) { break; }
    }
    CHECK(first.size() > 12 && first[0].rfind("VFO_LIMITS:", 0) == 0);
    std::printf("   step 11\n");
    CHECK(first[2] == "TRX_COUNT:2;" && first[4] == "DEVICE:FoxSDR;" && first[7] == "PROTOCOL:FoxSDR,2.1;" && first[8] == "READY;");
    std::printf("   step 12\n");
    CHECK(has(first, "VFO:0,0,7100000;") && has(first, "VFO:1,0,14200000;"));
    std::printf("   step 13\n");
    CHECK(r.accepted.load() == 1);
    std::printf("   step 14\n");
    CHECK(r.srv.clientCount() == 1);
    std::printf("   step 15\n");
    // a second client
    httplib::ws::WebSocketClient b("ws://127.0.0.1:" + std::to_string(r.port) + "/tci");
    std::printf("   step 16\n");
    CHECK(static_cast<bool>(b.connect()));
    std::printf("   step 17\n");
    b.set_read_timeout(3, 0);
    std::printf("   step 18\n");
    for (int i = 0; i < 40 && b.read(msg) == httplib::ws::Text; ++i) {
        if (msg == "TX_ENABLE:1,false;") { break; }
    }
    CHECK(r.srv.clientCount() == 2);
    std::printf("   step 19\n");
    // A sets receiver 1 to 14.2005 MHz (inside its band): A gets the echo, B gets the change pushed
    CHECK(a.send("vfo:1,0,14200500;"));
    std::printf("   step 20\n");
    std::vector<std::string> got;
    std::printf("   step 21\n");
    bool echoA = false, pushB = false;
    std::printf("   step 22\n");
    const auto end = Clock::now() + std::chrono::seconds(3);
    std::printf("   step 23\n");
    while (Clock::now() < end && !(echoA && pushB)) {
        if (!echoA) {
            a.set_read_timeout(0, 200000);
            if (a.read(msg) == httplib::ws::Text && msg == "VFO:1,0,14200500;") { echoA = true; }
        }
        if (!pushB) {
            b.set_read_timeout(0, 200000);
            if (b.read(msg) == httplib::ws::Text && msg == "VFO:1,0,14200500;") { pushB = true; }
        }
    }
    CHECK(echoA);
    std::printf("   step 24\n");
    CHECK(pushB);
    std::printf("   step 25\n");
    {
        std::lock_guard<std::mutex> lk(r.w.m);
        CHECK(!r.w.all.empty() && r.w.all.back().op == FOXAPI_OP_SET_VFO_OFFSET && r.w.all.back().target == 2 && r.w.all.back().num[0] == 500.0);
    }
    // an invalid-case, over-spaced command still works; a read request is answered
    CHECK(a.send("  Modulation : 0 ;"));
    std::printf("   step 26\n");
    a.set_read_timeout(2, 0);
    std::printf("   step 27\n");
    bool mod = false;
    std::printf("   step 28\n");
    for (int i = 0; i < 20 && !mod && a.read(msg) == httplib::ws::Text; ++i) { mod = msg == "MODULATION:0,USB;"; }
    CHECK(mod);
    std::printf("   step 29\n");
    // a close from the client is answered and the count drops
    a.close();
    std::printf("   step 30\n");
    CHECK(waitUntil(3000, [&] { return r.srv.clientCount() == 1; }));
    std::printf("   step 31\n");
    b.close();
    std::printf("   step 32\n");
    CHECK(waitUntil(3000, [&] { return r.srv.clientCount() == 0 && r.w.backends.load() == 0; }));
    std::printf("   step 33\n");
    r.srv.stop();
    std::printf("   step 34\n");
    CHECK(!r.srv.running());
    std::printf("   step 35\n");
}

void testCap() {
    Rig r;
    CHECK(r.up());
    std::vector<std::unique_ptr<RawClient>> cs;
    for (int i = 0; i < kMaxTciClients; ++i) {
        cs.push_back(std::make_unique<RawClient>());
        CHECK(cs.back()->open(r.port) && cs.back()->handshake());
    }
    CHECK(waitUntil(2000, [&] { return r.srv.clientCount() == kMaxTciClients; }));
    CHECK(r.w.backends.load() == kMaxTciClients);
    RawClient extra;
    CHECK(extra.open(r.port));
    // it is refused with an HTTP 503 that says why - and no session was made for it
    const auto end = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < end && extra.buf.find("\r\n\r\n") == std::string::npos) { if (!extra.fill()) { break; } }
    CHECK(extra.buf.rfind("HTTP/1.1 503", 0) == 0 && extra.buf.find("too many TCI clients") != std::string::npos);
    CHECK(r.w.opened.load() == kMaxTciClients);
    CHECK(r.srv.refusedOverCap() == 1);
    // one leaves: a new one is welcome
    cs[0].reset();
    CHECK(waitUntil(3000, [&] { return r.srv.clientCount() == kMaxTciClients - 1; }));
    RawClient again;
    CHECK(again.open(r.port) && again.handshake());
    // when the engine will not give a session, the client is refused (not hung)
    cs.clear();
    CHECK(waitUntil(3000, [&] { return r.srv.clientCount() <= 1; }));
    r.w.refuseSessions = true;
    RawClient none;
    CHECK(none.open(r.port));
    const auto e2 = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < e2 && none.buf.find("\r\n\r\n") == std::string::npos) { if (!none.fill()) { break; } }
    CHECK(none.buf.rfind("HTTP/1.1 503", 0) == 0 && none.buf.find("no session free") != std::string::npos);
    r.srv.stop();
}

void testPortInUse() {
    sockInit();
    // a program that holds a port exclusively
    SockT holder = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int yes = 1;
#if defined(_WIN32)
    ::setsockopt(holder, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
    (void)yes;
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    CHECK(::bind(holder, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    CHECK(::listen(holder, 4) == 0);
    socklen_t len = sizeof(a);
    ::getsockname(holder, reinterpret_cast<sockaddr*>(&a), &len);
    const int taken = ntohs(a.sin_port);
    Rig r;
    CHECK(!r.up(false, taken));
    CHECK(r.error.find("already in use") != std::string::npos && r.error.find(std::to_string(taken)) != std::string::npos);
    std::printf("port in use: \"%s\"\n", r.error.c_str());
    CHECK(!r.srv.running() && r.srv.boundPort() == 0);
    // the same server object starts on a free port afterwards
    Rig r2;
    CHECK(r2.up(false, freePort()));
    closeSock(holder);
    // a server with no engine to answer from says so
    TciServer bare;
    std::string err;
    CHECK(!bare.start(0, false, err) && err.find("no engine session") != std::string::npos);
    // a second start on the same running server is refused
    CHECK(!r2.srv.start(0, false, err) && err.find("already running") != std::string::npos);
}

void testHostile() {
    Rig r;
    CHECK(r.up());
    // garbage instead of an HTTP request
    {
        RawClient c;
        CHECK(c.open(r.port));
        // (a TLS hello, not HTTP; the length is explicit because the bytes hold NULs)
        CHECK(sendAll(c.s, std::string("\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03garbage\r\n\r\n", 22)));
        CHECK(c.closedWithin(3000));
    }
    // a plain HTTP GET: told this port speaks WebSocket only
    {
        RawClient c;
        CHECK(c.open(r.port));
        CHECK(sendAll(c.s, "GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n"));
        const auto end = Clock::now() + std::chrono::seconds(3);
        while (Clock::now() < end && c.buf.find("\r\n\r\n") == std::string::npos) { if (!c.fill()) { break; } }
        CHECK(c.buf.rfind("HTTP/1.1 400", 0) == 0 && c.buf.find("WebSocket") != std::string::npos);
    }
    // a request head past 8 KiB
    {
        RawClient c;
        CHECK(c.open(r.port));
        sendAll(c.s, "GET / HTTP/1.1\r\nX-Pad: " + std::string(20000, 'a'));
        const auto end = Clock::now() + std::chrono::seconds(3);
        while (Clock::now() < end && c.buf.find("\r\n\r\n") == std::string::npos) { if (!c.fill()) { break; } }
        CHECK(c.buf.rfind("HTTP/1.1 431", 0) == 0);
    }
    // wrong version
    {
        RawClient c;
        CHECK(c.open(r.port));
        sendAll(c.s, "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 8\r\n\r\n");
        const auto end = Clock::now() + std::chrono::seconds(3);
        while (Clock::now() < end && c.buf.find("\r\n\r\n") == std::string::npos) { if (!c.fill()) { break; } }
        CHECK(c.buf.rfind("HTTP/1.1 426", 0) == 0);
    }
    CHECK(waitUntil(3000, [&] { return r.srv.clientCount() == 0; }));
    CHECK(r.srv.protocolDropped() >= 4);
    CHECK(r.w.backends.load() == 0);   // every one of those sessions was closed with its connection
    // frames after a good handshake
    struct Case { const char* what; std::string frame; std::uint16_t code; };
    const Case cases[] = {
        {"an unmasked frame", maskedFrame(ws::Opcode::Text, "STOP;", true, false), 1002},
        {"reserved bits", maskedFrame(ws::Opcode::Text, "STOP;", true, true, 4), 1002},
        {"a reserved opcode", maskedFrame(static_cast<ws::Opcode>(3), "x"), 1002},
        {"an oversize message", maskedFrame(ws::Opcode::Binary, std::string(kMaxTciMessageBytes + 1, 'b')), 1009},
        {"text that is not UTF-8", maskedFrame(ws::Opcode::Text, "\xC0\x80;"), 1007},
        {"a fragmented ping", maskedFrame(ws::Opcode::Ping, "p", false), 1002},
    };
    for (const Case& c : cases) {
        RawClient k;
        CHECK(k.open(r.port) && k.handshake());
        CHECK(k.waitText("READY;"));
        CHECK(sendAll(k.s, c.frame));
        std::uint16_t code = 0;
        const bool closed = k.closedWithin(3000, &code);
        CHECK(closed);
        CHECK(code == c.code);
        if (!closed || code != c.code) { std::printf("      %s: closed %d code %u (wanted %u)\n", c.what, closed ? 1 : 0, code, c.code); }
    }
    // an oversize TEXT message (past the protocol's 2 KiB, within the frame cap) is read as nothing; the connection lives
    {
        RawClient k;
        CHECK(k.open(r.port) && k.handshake());
        CHECK(sendAll(k.s, maskedFrame(ws::Opcode::Text, std::string(5000, ';'))));
        CHECK(k.sendText("DDS:0;"));
        CHECK(k.waitText("DDS:0,7100000;"));
    }
    // a handshake that is never finished is dropped after the time allowed
    {
        RawClient c;
        CHECK(c.open(r.port));
        sendAll(c.s, "GET / HTTP/1.1\r\n");
        const auto t0 = Clock::now();
        CHECK(c.closedWithin(kTciHandshakeMs + 3000));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        CHECK(ms >= kTciHandshakeMs - 500);
        std::printf("a handshake never finished was dropped after %lld ms (limit %d)\n", static_cast<long long>(ms), kTciHandshakeMs);
    }
    // ping is answered with the same payload; a binary message is dropped without ending the connection
    {
        RawClient k;
        CHECK(k.open(r.port) && k.handshake());
        CHECK(k.waitText("READY;"));
        CHECK(sendAll(k.s, maskedFrame(ws::Opcode::Ping, "are you there")));
        ServerMsg m;
        bool pong = false;
        const auto end = Clock::now() + std::chrono::seconds(3);
        while (Clock::now() < end && !pong) { if (k.next(m, 200) && m.op == ws::Opcode::Pong && m.payload == "are you there") { pong = true; } }
        CHECK(pong);
        CHECK(sendAll(k.s, maskedFrame(ws::Opcode::Binary, std::string(16448, 'x'))));   // a transmit audio block of TCI 2.1: dropped
        CHECK(k.sendText("DDS:0;"));
        CHECK(k.waitText("DDS:0,7100000;"));
        // a close is answered with the same code, then the connection ends
        CHECK(sendAll(k.s, maskedFrame(ws::Opcode::Close, std::string("\x03\xE8", 2))));
        std::uint16_t code = 0;
        CHECK(k.closedWithin(2000, &code) && code == 1000);
    }
    // the server is still serving
    RawClient ok;
    CHECK(ok.open(r.port) && ok.handshake() && ok.waitText("READY;"));
}

void testReceiveOnlyOverTheWire() {
    Rig r;
    CHECK(r.up());
    RawClient k;
    CHECK(k.open(r.port) && k.handshake() && k.waitText("READY;"));
    CHECK(k.sendText("TRX:0,true,tci;"));
    CHECK(k.waitText("TRX:0,false;"));
    CHECK(k.sendText("TUNE:1,true;"));
    CHECK(k.waitText("TUNE:1,false;"));
    CHECK(k.sendText("CW_MACROS:0,CQ;DRIVE:0,100;"));
    CHECK(k.sendText("DDS:0;"));
    CHECK(k.waitText("DDS:0,7100000;"));
    std::lock_guard<std::mutex> lk(r.w.m);
    CHECK(r.w.all.empty());
}

void testNeverReads() {
    Rig r;
    CHECK(r.up());
    r.w.audioFramesPerCall = 4800;   // a fast producer
    // the client that never reads: audio started, a small receive buffer, then nothing
    RawClient dead;
    CHECK(dead.open(r.port));
    int small = 4096;
    ::setsockopt(dead.s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&small), sizeof(small));
    CHECK(dead.handshake());
    CHECK(dead.sendText("AUDIO_STREAM_SAMPLE_TYPE:float32;AUDIO_STREAM_CHANNELS:2;AUDIO_START:0;"));
    // a healthy client works the whole time
    RawClient live;
    CHECK(live.open(r.port) && live.handshake() && live.waitText("READY;"));
    const auto t0 = Clock::now();
    long long worstEchoMs = 0;
    int round = 0;
    while (r.srv.slowDropped() == 0 && Clock::now() - t0 < std::chrono::seconds(30)) {
        const auto s0 = Clock::now();
        const std::string want = "VFO:0,0," + std::to_string(7100000 + 100 * (round % 50)) + ";";
        CHECK(live.sendText("vfo:0,0," + std::to_string(7100000 + 100 * (round % 50)) + ";"));
        if (!live.waitText(want, nullptr, 2000)) { CHECK(false); break; }
        worstEchoMs = std::max<long long>(worstEchoMs, std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s0).count());
        ++round;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto droppedAfterMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    CHECK(r.srv.slowDropped() == 1);
    CHECK(droppedAfterMs < 15000);
    CHECK(waitUntil(3000, [&] { return r.srv.clientCount() == 1; }));   // only the healthy one is left
    CHECK(r.w.backends.load() == 1);
    // the engine side was never held up by the dead client: no backend call took long, and the healthy client was served promptly throughout
    const long long maxCallMs = r.w.maxCallUs.load() / 1000;
    std::printf("a client that never read was dropped after %lld ms; the healthy client's slowest echo %lld ms; slowest backend call %lld ms\n",
                static_cast<long long>(droppedAfterMs), worstEchoMs, maxCallMs);
    CHECK(maxCallMs < 250);
    CHECK(worstEchoMs < 500);
    // and it still answers
    CHECK(live.sendText("DDS:1;"));
    CHECK(live.waitText("DDS:1,14200000;"));
}

void testDisappears() {
    Rig r;
    CHECK(r.up());
    r.w.audioFramesPerCall = 1000;
    for (int i = 0; i < 6; ++i) {
        RawClient k;
        CHECK(k.open(r.port) && k.handshake() && k.waitText("READY;"));
        CHECK(k.sendText("AUDIO_STREAM_CHANNELS:1;AUDIO_START:0;"));
        ServerMsg m;
        bool binary = false;
        const auto end = Clock::now() + std::chrono::seconds(3);
        while (Clock::now() < end && !binary) { if (k.next(m, 200) && m.op == ws::Opcode::Binary) { binary = true; CHECK(m.payload.size() >= 64); } }
        CHECK(binary);
        // vanish without a close frame (and with a reset: SO_LINGER 0)
        linger l{1, 0};
        ::setsockopt(k.s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof(l));
    }
    CHECK(waitUntil(5000, [&] { return r.srv.clientCount() == 0 && r.w.backends.load() == 0; }));
    CHECK(r.srv.audioPackets() >= 6);
    RawClient after;
    CHECK(after.open(r.port) && after.handshake() && after.waitText("READY;"));
}

void testStopWithClients() {
    Rig r;
    CHECK(r.up());
    std::vector<std::unique_ptr<RawClient>> cs;
    for (int i = 0; i < 3; ++i) {
        cs.push_back(std::make_unique<RawClient>());
        CHECK(cs.back()->open(r.port) && cs.back()->handshake() && cs.back()->waitText("READY;"));
    }
    CHECK(r.w.backends.load() == 3);
    const auto t0 = Clock::now();
    r.srv.stop();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    CHECK(ms < 2500);
    CHECK(r.w.backends.load() == 0);      // no session of the server exists any more
    CHECK(!r.srv.running() && r.srv.clientCount() == 0);
    std::printf("stop() with 3 clients connected took %lld ms\n", static_cast<long long>(ms));
    for (auto& c : cs) { CHECK(c->closedWithin(2000)); }
    // it can be started again
    std::string err;
    CHECK(r.srv.start(0, false, err));
    r.srv.stop();
}

void testFuzz() {
    Rig r;
    CHECK(r.up());
    std::mt19937 rng(8801);
    for (int i = 0; i < 200; ++i) {
        RawClient c;
        if (!c.open(r.port)) { continue; }
        std::string junk;
        const int kind = static_cast<int>(rng() % 4);
        if (kind == 0) {
            junk.resize(rng() % 600);
            for (char& ch : junk) { ch = static_cast<char>(rng()); }
        } else if (kind == 1) {
            junk = "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
            std::string frames;
            const int n = static_cast<int>(rng() % 6);
            for (int k = 0; k < n; ++k) {
                std::string p(rng() % 300, '\0');
                for (char& ch : p) { ch = static_cast<char>(rng()); }
                frames += maskedFrame(static_cast<ws::Opcode>(rng() % 3), p);
            }
            junk += frames;
        } else if (kind == 2) {
            junk = "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
            std::string cmds;
            const char* frag[] = {"VFO:", "DDS:", "0", "1", ",", ";", "99999999999", "-5", "MODULATION:0,", "AUDIO_START:", "TRX:0,true"};
            for (int k = 0; k < 20; ++k) { cmds += frag[rng() % 11]; }
            junk += maskedFrame(ws::Opcode::Text, cmds);
        } else {
            junk = "GET / HTTP/1.1\r\n";
            junk.resize(junk.size() + rng() % 200, 'x');
        }
        sendAll(c.s, junk);
        std::this_thread::sleep_for(std::chrono::milliseconds(rng() % 10));
    }
    CHECK(waitUntil(10000, [&] { return r.srv.clientCount() == 0 && r.w.backends.load() == 0; }));
    RawClient ok;
    CHECK(ok.open(r.port) && ok.handshake() && ok.waitText("READY;"));
    {
        std::lock_guard<std::mutex> lk(r.w.m);
        for (const FoxCommand& c : r.w.all) {
            CHECK(c.op == FOXAPI_OP_RUN || c.op == FOXAPI_OP_SET_CENTRE || c.op == FOXAPI_OP_SET_VFO_OFFSET || c.op == FOXAPI_OP_SET_MODE ||
                  c.op == FOXAPI_OP_SET_BANDWIDTH || c.op == FOXAPI_OP_SET_SQUELCH);
        }
    }
    std::printf("fuzz: 200 random connections survived; the server still answers; %zu engine commands were built from them, all allowed ops\n", r.w.all.size());
}

// This machine's first non-loopback IPv4 address, or "" when it has none.
std::string otherAddress() {
    sockInit();
    SockT s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in d{};
    d.sin_family = AF_INET;
    d.sin_port = ::htons(9);
    ::inet_pton(AF_INET, "192.0.2.1", &d.sin_addr);   // TEST-NET-1: nothing is sent; connect() on UDP only picks the route
    std::string out;
    if (::connect(s, reinterpret_cast<sockaddr*>(&d), sizeof(d)) == 0) {
        sockaddr_in me{};
        socklen_t len = sizeof(me);
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) {
            char buf[64];
            if (::inet_ntop(AF_INET, &me.sin_addr, buf, sizeof(buf)) != nullptr) { out = buf; }
        }
    }
    closeSock(s);
    if (out == "0.0.0.0" || out.rfind("127.", 0) == 0) { out.clear(); }
    return out;
}

void testLoopbackByDefault() {
    const std::string other = otherAddress();
    if (other.empty()) {
        std::printf("SKIP: this machine has no non-loopback IPv4 address; the bind-scope test cannot say anything here\n");
        ++g_checksSkipped;
        return;
    }
    {
        Rig r;
        CHECK(r.up(false));
        CHECK(connectTo("127.0.0.1", r.port) != INVALID_SOCKET);
        const SockT s = connectTo(other.c_str(), r.port);
        CHECK(s == INVALID_SOCKET);   // not reachable on the machine's other address
        closeSock(s);
    }
    {
        Rig r;
        CHECK(r.up(true));
        const SockT s = connectTo(other.c_str(), r.port);
        CHECK(s != INVALID_SOCKET);   // bindAll reaches it
        closeSock(s);
    }
    std::printf("bind scope checked against %s\n", other.c_str());
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // (a run that hangs still shows how far it got)
    std::printf("-- testThroughHttplibClient\n");
    testThroughHttplibClient();
    std::printf("-- testCap\n");
    testCap();
    std::printf("-- testPortInUse\n");
    testPortInUse();
    std::printf("-- testHostile\n");
    testHostile();
    std::printf("-- testReceiveOnlyOverTheWire\n");
    testReceiveOnlyOverTheWire();
    std::printf("-- testNeverReads\n");
    testNeverReads();
    std::printf("-- testDisappears\n");
    testDisappears();
    std::printf("-- testStopWithClients\n");
    testStopWithClients();
    std::printf("-- testFuzz\n");
    testFuzz();
    std::printf("-- testLoopbackByDefault\n");
    testLoopbackByDefault();
    return testSummary("test_tci_server");
}
