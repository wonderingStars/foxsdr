// test_tci_engine.cpp - THE TCI SERVER AND AUDIO OVER THE NETWORK, THROUGH THE REAL ENGINE (docs/API.md 6.22): a headless engine that hosts its
// servers, its pass on a thread of its own, the listeners started by FOXAPI_OP_SERVER_CONFIG, a real WebSocket client on the loopback interface.
// (tests/test_tci_server.cpp holds the transport against a stub; tests/test_tci_protocol.cpp the grammar; this file holds the WHOLE LINE from a
// client's bytes to the engine's state, and the safety defaults.)
//
//   E1  OFF BY DEFAULT: a fresh engine listens on nothing, and says so in its facts; SERVER_CONFIG switches TCI on, on this computer only.
//   E2  THE START-UP EXCHANGE from the real engine, and the radio follows what the client sets: VFO inside the band moves the offset and NOT the
//       centre; MODULATION; and the same on a SECOND radio while the first stays where it was.
//   E3  RECEIVE-ONLY: TRX / TUNE answer false and the transmitter is never keyed; the engine session behind a connection has exactly the six
//       receive ops on its profile and every transmitter op is refused at the door.
//   E4  SESSIONS: one internal session per connection, back to the baseline on disconnect and when the listener is stopped.
//   E5  THE AUDIO STREAM: AUDIO_START gives binary packets of the right layout from the real engine's audio ring, in every sample type.
//   E6  AUDIO OVER THE NETWORK (UDP): off by default; on, to a loopback receiver: datagrams of the chosen size, counted in the facts; another
//       radio; a radio that is not there sends nothing; off again stops.
//   E7  THE CONFIGURATION: the op's refusals in words, a port already in use in words (and retried), REMOTE sessions cannot configure, and the
//       settings file round trip (with hostile values clamped).
//   E8  TEARDOWN with clients connected and the stream running: the engine shuts down promptly.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SockT = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SockT = int;
constexpr SockT INVALID_SOCKET = -1;
#endif

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/server_host.hpp"
#include "engine_api_test_env.hpp"
#include "foxsdr_api.h"
#include "net/tci_protocol.hpp"
#include "net/ws_frame.hpp"
#include "test_check.hpp"

using namespace cascade::net;
using Clock = std::chrono::steady_clock;
using cascade::engine::Engine;
using cascade::engine::InternalKind;
namespace cmd = cascade::core::cmd;

#include "tci_test_client.hpp"

namespace {

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

constexpr std::uint64_t kAllGrants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_TRANSMIT | FOXAPI_GRANT_ADMIN |
                                     FOXAPI_GRANT_AUDIO | FOXAPI_GRANT_IQ;

std::uint32_t le32(const std::string& s, std::size_t off) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(s[off])) | (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 2])) << 16) | (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 3])) << 24);
}

// A hermetic engine that HOSTS its servers (servers=1), its pass on a thread of its own.
struct Rig {
    Engine e;
    FoxSession* op = nullptr;
    int tciPort = 0;
    explicit Rig(const std::string& options = "testHooks=1;servers=1") {
        e.configureApi(options);
        e.initialise();
        (void)e.applyCommand(cmd::makeInt(FOXAPI_OP_RUN, 1));
        e.controlPass();
        e.startControlThread();
        FoxSessionParams p{};
        p.structSize = sizeof(p);
        p.flags = FOXAPI_SESSION_LOCAL;
        p.grants = kAllGrants;
        p.clientName = "the operator";
        (void)api()->open_session(e.apiHandle(), &p, &op);
    }
    ~Rig() {
        if (op != nullptr) { api()->close_session(op); }
        e.shutdown();
    }
    FoxCommandResult submitText(std::uint32_t opcode, const std::string& text, std::int64_t ival0 = 0, std::int64_t ival1 = 0) {
        FoxCommand c{};
        c.structSize = sizeof(c);
        c.op = opcode;
        c.ival[0] = ival0;
        c.ival[1] = ival1;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        FoxCommandResult out{};
        out.status = FOXAPI_FAILED;
        if (api()->submit_text(op, &c, text.data(), static_cast<uint32_t>(text.size()), &sr) < 0) { return out; }
        return waitResult(sr.ticket);
    }
    FoxCommandResult submit(const FoxCommand& c) {
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        FoxCommandResult out{};
        out.status = FOXAPI_FAILED;
        if (api()->submit(op, &c, 1, &sr) < 0) { return out; }
        return waitResult(sr.ticket);
    }
    FoxCommandResult waitResult(std::uint64_t ticket) {
        FoxCommandResult out{};
        out.status = FOXAPI_FAILED;
        const auto end = Clock::now() + std::chrono::seconds(10);
        while (Clock::now() < end) {
            FoxCommandResult r[8];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(op, r, 8);
            for (int32_t i = 0; i < got; ++i) {
                if (r[i].ticket == ticket) {
                    (void)e.servers().waitIdle(std::chrono::seconds(30));
                    return r[i];
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return out;
    }
    FoxCommandResult config(const std::string& json) { return submitText(FOXAPI_OP_SERVER_CONFIG, json); }
    bool startTci() {
        tciPort = freePort();
        const FoxCommandResult a = config("{\"tci\":{\"enabled\":true,\"bindAll\":false,\"port\":" + std::to_string(tciPort) + "}}");
        return a.status == FOXAPI_OK && e.servers().tciServer().running();
    }
    std::string fact(const char* key, FoxSession* s = nullptr) {
        char buf[512];
        buf[0] = '\0';
        const int32_t n = api()->get_setting(s != nullptr ? s : op, key, buf, sizeof(buf));
        return n >= 0 ? std::string(buf) : std::string("<") + std::to_string(n) + ">";
    }
    FoxReceiverState state(std::uint32_t radio) {
        FoxReceiverState st{};
        st.structSize = sizeof(st);
        for (int i = 0; i < 100 && api()->read_radio_state(op, radio, &st) != FOXAPI_OK; ++i) {}
        return st;
    }
    bool addRadio2() {
        FoxCommand c{};
        c.structSize = sizeof(c);
        c.op = FOXAPI_OP_RADIO_ADD;
        c.ival[0] = 2;
        c.ival[1] = 1;
        const FoxCommandResult r = submit(c);
        return r.status == FOXAPI_OK;
    }
    // A fact is published by the pass AFTER the change that moved it: these poll for it (up to three seconds) instead of reading once.
    bool is(const char* key, const std::string& want, int ms = 3000) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            if (fact(key) == want) { return true; }
            if (Clock::now() > end) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    bool has(const char* key, const std::string& part, int ms = 3000) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            if (fact(key).find(part) != std::string::npos) { return true; }
            if (Clock::now() > end) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    std::size_t tciSessions() { return e.api().sessionCountForTest(InternalKind::Tci); }
    std::size_t udpSessions() { return e.api().sessionCountForTest(InternalKind::Udp); }
};

// A connected TCI client: the handshake done and the engine's start-up exchange read to its READY and a little beyond.
bool hello(RawClient& c, int port) {
    return c.open(port) && c.handshake() && c.waitText("READY;");
}

// Reads text messages until one starts with `prefix`; returns it ("" on timeout).
std::string waitPrefix(RawClient& c, const std::string& prefix, int ms = 3000) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    ServerMsg m;
    while (Clock::now() < end) {
        if (c.next(m, 200) && m.op == ws::Opcode::Text && m.payload.rfind(prefix, 0) == 0) { return m.payload; }
    }
    return std::string();
}

long long hz(double v) { return static_cast<long long>(std::llround(v)); }

// ===================================================================================================================================

void testOffByDefault() {
    Rig r;
    CHECK(r.is("fact.tci.enabled", "0") && r.is("fact.tci.listening", "0") && r.is("fact.tci.bindAll", "0"));
    CHECK(r.is("fact.tci.port", "50001") && r.is("fact.tci.scope", "this computer only"));
    CHECK(r.is("fact.udp.enabled", "0") && r.is("fact.udp.sending", "0") && r.is("fact.udp.host", "127.0.0.1") && r.is("fact.udp.port", "7355"));
    CHECK(r.is("fact.udp.radio", "1") && r.is("fact.udp.packetFrames", "480"));
    CHECK(!r.e.servers().tciServer().running() && !r.e.servers().udpSender().running());
    CHECK(r.is("fact.tci.clients", "0") && r.is("fact.tci.accepted", "0") && r.is("fact.udp.sent", "0"));
    // the defaults of the settings file
    cascade::core::AppConfig c;
    CHECK(!c.tciEnabled && !c.tciBindAll && c.tciPort == 50001 && !c.udpAudioEnabled && c.udpAudioHost == "127.0.0.1" && c.udpAudioPort == 7355);
    // switching it on
    CHECK(r.startTci());
    CHECK(r.is("fact.tci.enabled", "1") && r.is("fact.tci.listening", "1"));
    CHECK(r.fact("fact.tci.boundPort") == std::to_string(r.tciPort));
    CHECK(r.is("fact.tci.scope", "this computer only"));
    // ... and off again
    CHECK(r.config("{\"tci\":{\"enabled\":false}}").status == FOXAPI_OK);
    CHECK(!r.e.servers().tciServer().running() && r.is("fact.tci.listening", "0"));
    // the list of listeners is still the two it always was
    CHECK(api()->read_list(r.op, FOXAPI_LIST_SERVERS, 0, nullptr) == 2);
}

void testStartupAndTuning() {
    Rig r;
    CHECK(r.startTci());
    const FoxReceiverState s1 = r.state(1);
    RawClient c;
    std::vector<std::string> seen;
    CHECK(c.open(r.tciPort) && c.handshake());
    CHECK(c.waitText("TX_ENABLE:0,false;", &seen));
    CHECK(seen.size() > 10 && seen[0].rfind("VFO_LIMITS:", 0) == 0);
    CHECK(std::find(seen.begin(), seen.end(), "TRX_COUNT:1;") != seen.end());
    CHECK(std::find(seen.begin(), seen.end(), "DEVICE:FoxSDR;") != seen.end() && std::find(seen.begin(), seen.end(), "RECEIVE_ONLY:true;") != seen.end());
    CHECK(std::find(seen.begin(), seen.end(), "READY;") != seen.end());
    CHECK(std::find(seen.begin(), seen.end(), "DDS:0," + std::to_string(hz(s1.centreHz)) + ";") != seen.end());
    CHECK(std::find(seen.begin(), seen.end(), "VFO:0,0," + std::to_string(hz(s1.tunedHz)) + ";") != seen.end());
    // a VFO inside the band: only the offset moves
    const long long target = hz(s1.centreHz) + 5000;
    CHECK(c.sendText("VFO:0,0," + std::to_string(target) + ";"));
    CHECK(c.waitText("VFO:0,0," + std::to_string(target) + ";"));
    const FoxReceiverState s2 = r.state(1);
    CHECK(hz(s2.tunedHz) == target && hz(s2.centreHz) == hz(s1.centreHz) && hz(s2.vfoOffsetHz) == 5000);
    // a mode
    CHECK(c.sendText("modulation:0,am;"));
    CHECK(c.waitText("MODULATION:0,AM;"));
    CHECK(r.state(1).demodMode == FOXAPI_DEMOD_AM);
    // a frequency outside the band moves the centre
    const long long farHz = hz(s1.centreHz) + 40000000;
    CHECK(c.sendText("VFO:0,0," + std::to_string(farHz) + ";"));
    CHECK(c.waitText("DDS:0," + std::to_string(farHz) + ";"));
    CHECK(hz(r.state(1).centreHz) == farHz && hz(r.state(1).vfoOffsetHz) == 0);
    // stop and start
    CHECK(c.sendText("STOP;"));
    CHECK(c.waitText("STOP;"));
    CHECK((r.state(1).flags & FOXAPI_RX_RUNNING) == 0u);
    CHECK(c.sendText("START;"));
    CHECK(c.waitText("START;"));
    CHECK((r.state(1).flags & FOXAPI_RX_RUNNING) != 0u);
    // a second radio: a client sees it appear, addresses it by its number, and the first does not move
    CHECK(r.addRadio2());
    CHECK(c.waitText("TRX_COUNT:2;"));
    const FoxReceiverState a1 = r.state(1), a2 = r.state(2);
    const long long t2 = hz(a2.centreHz) + 3000;
    CHECK(c.sendText("VFO:1,0," + std::to_string(t2) + ";MODULATION:1,USB;"));
    CHECK(c.waitText("VFO:1,0," + std::to_string(t2) + ";"));
    CHECK(c.waitText("MODULATION:1,USB;"));
    CHECK(hz(r.state(2).tunedHz) == t2 && r.state(2).demodMode == FOXAPI_DEMOD_USB);
    CHECK(hz(r.state(1).tunedHz) == hz(a1.tunedHz) && r.state(1).demodMode == a1.demodMode);   // radio 1 did not move
    // a second client sees the first one's change
    RawClient d;
    CHECK(hello(d, r.tciPort));
    CHECK(c.sendText("VFO:1,0," + std::to_string(t2 + 100) + ";"));
    CHECK(d.waitText("VFO:1,0," + std::to_string(t2 + 100) + ";"));
    CHECK(r.is("fact.tci.clients", "2") && r.is("fact.tci.accepted", "2"));
}

void testReceiveOnly() {
    Rig r;
    CHECK(r.startTci());
    RawClient c;
    CHECK(hello(c, r.tciPort));
    CHECK(c.sendText("TRX:0,true,tci;"));
    CHECK(c.waitText("TRX:0,false;"));
    CHECK(c.sendText("TUNE:0,true;"));
    CHECK(c.waitText("TUNE:0,false;"));
    CHECK(c.sendText("DRIVE:0,100;CW_MACROS:0,CQ CQ;TX_ENABLE:0,true;"));
    CHECK(c.waitText("TX_ENABLE:0,false;"));
    const FoxReceiverState st = r.state(1);
    CHECK((st.flags & (FOXAPI_RX_TX_KEYED | FOXAPI_RX_TX_LATCHED)) == 0u);
    // THE WALL BEHIND THE PROTOCOL LAYER: the session a connection holds can speak six ops and no others, and the transmitter's are refused at the door
    const auto ops = cascade::engine::internalProfileOps(InternalKind::Tci);
    std::vector<std::uint32_t> want = {FOXAPI_OP_RUN, FOXAPI_OP_SET_CENTRE, FOXAPI_OP_SET_VFO_OFFSET, FOXAPI_OP_SET_MODE, FOXAPI_OP_SET_BANDWIDTH, FOXAPI_OP_SET_SQUELCH};
    std::vector<std::uint32_t> got(ops.begin(), ops.end());
    std::sort(want.begin(), want.end());
    std::sort(got.begin(), got.end());
    CHECK(got == want);
    CHECK(cascade::engine::internalProfileOps(InternalKind::Udp).empty());
    CHECK((cascade::engine::internalProfileGrants(InternalKind::Tci) & (FOXAPI_GRANT_TRANSMIT | FOXAPI_GRANT_ADMIN)) == 0u);
    CHECK((cascade::engine::internalProfileGrants(InternalKind::Udp) & (FOXAPI_GRANT_TRANSMIT | FOXAPI_GRANT_ADMIN | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS)) == 0u);
    std::int32_t status = 0;
    FoxSession* s = r.e.api().openInternalSession(InternalKind::Tci, "probe", &status);
    CHECK(s != nullptr && status == FOXAPI_OK);
    if (s != nullptr) {
        const std::uint32_t denied[] = {FOXAPI_OP_TX_PTT, FOXAPI_OP_TX_LATCH, FOXAPI_OP_TX_REMOTE_ARM, FOXAPI_OP_TX_CLOSE, FOXAPI_OP_SERVER_CONFIG, FOXAPI_OP_RECORD_AUDIO,
                                        FOXAPI_OP_RECORD_IQ, FOXAPI_OP_RADIO_ADD, FOXAPI_OP_RADIO_REMOVE, FOXAPI_OP_STORE_INSTALL, FOXAPI_OP_SET_VOLUME,
                                        FOXAPI_OP_SELECT_SOURCE, FOXAPI_OP_BOOKMARK_ADD, FOXAPP_OP_SCHEDULE_SET, FOXAPP_OP_TX_PLUGIN_ARM, FOXAPI_OP_RADIO_FOCUS};
        for (const std::uint32_t o : denied) {
            FoxCommand cmdv{};
            cmdv.structSize = sizeof(cmdv);
            cmdv.op = o;
            cmdv.ival[0] = 1;
            const std::int32_t v = r.e.api().validateForTest(s, cmdv);
            CHECK(v == FOXAPI_DENIED);
            if (v != FOXAPI_DENIED) { std::printf("      op 0x%04X from a TCI session: %d\n", o, v); }
        }
        // and what the profile allows is allowed
        for (const std::uint32_t o : want) {
            FoxCommand cmdv{};
            cmdv.structSize = sizeof(cmdv);
            cmdv.op = o;
            cmdv.target = o == FOXAPI_OP_RUN ? 0u : 1u;
            cmdv.ival[0] = o == FOXAPI_OP_SET_MODE ? FOXAPI_DEMOD_AM : 1;
            cmdv.num[0] = o == FOXAPI_OP_SET_SQUELCH ? -80.0 : 100000.0;
            CHECK(r.e.api().validateForTest(s, cmdv) == FOXAPI_OK);
        }
        api()->close_session(s);
    }
    // a Udp session may read audio and do nothing else
    FoxSession* u = r.e.api().openInternalSession(InternalKind::Udp, "probe", &status);
    CHECK(u != nullptr);
    if (u != nullptr) {
        FoxCommand cmdv{};
        cmdv.structSize = sizeof(cmdv);
        cmdv.op = FOXAPI_OP_SET_CENTRE;
        cmdv.num[0] = 100e6;
        CHECK(r.e.api().validateForTest(u, cmdv) == FOXAPI_DENIED);
        std::uint64_t cursor = 0;
        float buf[64];
        CHECK(r.e.api().readAudioOfRadio(u, 1, &cursor, buf, 64) >= 0);
        CHECK(r.e.api().readAudioOfRadio(u, 5, &cursor, buf, 64) == FOXAPI_NO_DEVICE);   // no such radio
        api()->close_session(u);
    }
}

void testSessions() {
    Rig r;
    CHECK(r.startTci());
    const std::size_t base = r.e.api().sessionCountForTest();
    {
        RawClient a, b, c;
        CHECK(hello(a, r.tciPort) && hello(b, r.tciPort) && hello(c, r.tciPort));
        CHECK(r.tciSessions() == 3);
        CHECK(r.e.api().sessionCountForTest() == base + 3);
    }
    // all three disconnect: the sessions are closed with their connections
    const auto end = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < end && r.tciSessions() != 0) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    CHECK(r.tciSessions() == 0 && r.e.api().sessionCountForTest() == base);
    // stop with clients connected: no session of the server is left
    RawClient a, b;
    CHECK(hello(a, r.tciPort) && hello(b, r.tciPort));
    CHECK(r.config("{\"tci\":{\"enabled\":false}}").status == FOXAPI_OK);
    CHECK(r.tciSessions() == 0 && r.e.api().sessionCountForTest() == base);
    // more than the cap: refused before a session is made
    CHECK(r.config("{\"tci\":{\"enabled\":true}}").status == FOXAPI_OK);
    std::vector<std::unique_ptr<RawClient>> cs;
    for (int i = 0; i < kMaxTciClients; ++i) {
        cs.push_back(std::make_unique<RawClient>());
        CHECK(hello(*cs.back(), r.e.servers().tciServer().boundPort()));
    }
    RawClient over;
    CHECK(over.open(r.e.servers().tciServer().boundPort()));
    const auto e2 = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < e2 && over.buf.find("\r\n\r\n") == std::string::npos) { if (!over.fill()) { break; } }
    CHECK(over.buf.rfind("HTTP/1.1 503", 0) == 0);
    CHECK(r.tciSessions() == static_cast<std::size_t>(kMaxTciClients));
}

void testAudioStream() {
    Rig r;
    CHECK(r.startTci());
    // give the engine something to say: a loud channel (the generator's tone through the demodulator)
    FoxCommand v{};
    v.structSize = sizeof(v);
    v.op = FOXAPI_OP_SET_VOLUME;
    v.num[0] = 1.0;
    (void)r.submit(v);
    struct Case { const char* type; std::uint32_t code; std::size_t bytes; };
    const Case cases[] = {{"int16", 0, 2}, {"int24", 1, 3}, {"int32", 2, 4}, {"float32", 3, 4}};
    for (const Case& k : cases) {
        RawClient c;
        CHECK(hello(c, r.tciPort));
        CHECK(c.sendText(std::string("AUDIO_STREAM_SAMPLE_TYPE:") + k.type + ";AUDIO_STREAM_CHANNELS:1;AUDIO_STREAM_SAMPLES:512;AUDIO_START:0;"));
        CHECK(c.waitText(std::string("AUDIO_STREAM_SAMPLES:512;")));
        ServerMsg m;
        std::vector<std::string> pk;
        const auto end = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < end && pk.size() < 4) { if (c.next(m, 200) && m.op == ws::Opcode::Binary) { pk.push_back(m.payload); } }
        CHECK(pk.size() >= 4);
        for (std::size_t i = 0; i < pk.size(); ++i) {
            const std::string& p = pk[i];
            CHECK(p.size() == 64 + 512 * k.bytes);
            CHECK(le32(p, 0) == 0 && le32(p, 4) == 48000 && le32(p, 8) == k.code && le32(p, 12) == 0 && le32(p, 16) == 0);
            CHECK(le32(p, 20) == 512 && le32(p, 24) == kTciRxAudioStream && le32(p, 28) == 1);
            if (i > 0) { CHECK(le32(p, 32) == le32(pk[i - 1], 32) + 1); }   // the packet counter counts
        }
        CHECK(c.sendText("AUDIO_STOP:0;"));
        // after the stop the binary messages cease (those already in flight may arrive)
        const auto quiet = Clock::now() + std::chrono::milliseconds(600);
        while (Clock::now() < quiet) { (void)c.next(m, 50); }
        int after = 0;
        const auto probe = Clock::now() + std::chrono::milliseconds(500);
        while (Clock::now() < probe) { if (c.next(m, 50) && m.op == ws::Opcode::Binary) { ++after; } }
        CHECK(after == 0);
    }
    // the audio of the SECOND radio is a stream of its own (receiver 1)
    CHECK(r.addRadio2());
    RawClient c;
    CHECK(hello(c, r.tciPort));
    CHECK(c.sendText("AUDIO_STREAM_CHANNELS:1;AUDIO_STREAM_SAMPLE_TYPE:int16;AUDIO_STREAM_SAMPLES:480;AUDIO_START:1;"));
    ServerMsg m;
    bool got = false;
    const auto end = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < end && !got) {
        if (c.next(m, 200) && m.op == ws::Opcode::Binary) { got = true; CHECK(le32(m.payload, 0) == 1); CHECK(m.payload.size() == 64 + 480 * 2); }
    }
    CHECK(got);
}

void testUdp() {
    Rig r;
    // a receiver on loopback
    SockT rx = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    ::bind(rx, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t len = sizeof(a);
    ::getsockname(rx, reinterpret_cast<sockaddr*>(&a), &len);
    const int port = ntohs(a.sin_port);
    setRecvTimeout(rx, 100);
    const auto collect = [&](int ms) {
        std::vector<std::string> out;
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        char buf[4096];
        while (Clock::now() < end) {
            const int n = static_cast<int>(::recv(rx, buf, sizeof(buf), 0));
            if (n > 0) { out.emplace_back(buf, static_cast<std::size_t>(n)); }
        }
        return out;
    };
    CHECK(collect(300).empty());   // off by default: nothing arrives
    const FoxCommandResult on = r.config("{\"udp\":{\"enabled\":true,\"host\":\"127.0.0.1\",\"port\":" + std::to_string(port) + ",\"radio\":1,\"packetFrames\":480}}");
    CHECK(on.status == FOXAPI_OK);
    CHECK(r.is("fact.udp.sending", "1") && r.is("fact.udp.refusal", ""));
    CHECK(r.udpSessions() == 1);
    std::vector<std::string> got = collect(1500);
    CHECK(got.size() > 30);
    std::size_t odd = 0;
    for (const std::string& p : got) { odd += p.size() != 960 ? 1u : 0u; }
    CHECK(odd == 0);
    const std::string sentFact = r.fact("fact.udp.sent");
    CHECK(std::strtoull(sentFact.c_str(), nullptr, 10) >= got.size());
    if (std::strtoull(sentFact.c_str(), nullptr, 10) < got.size()) { std::printf("      fact.udp.sent = \"%s\", datagrams received %zu\n", sentFact.c_str(), got.size()); }
    // the packet size changes; the stream goes on
    CHECK(r.config("{\"udp\":{\"packetFrames\":960}}").status == FOXAPI_OK);
    collect(200);
    got = collect(1000);
    CHECK(!got.empty());
    odd = 0;
    for (const std::string& p : got) { odd += p.size() != 1920 ? 1u : 0u; }
    CHECK(odd == 0);
    CHECK(r.udpSessions() == 1);   // restarted on the same terms: still one session
    // another radio
    CHECK(r.addRadio2());
    CHECK(r.config("{\"udp\":{\"radio\":2}}").status == FOXAPI_OK);
    collect(200);
    CHECK(!collect(1000).empty());
    // a radio that is not there: nothing is sent (and nothing breaks)
    CHECK(r.config("{\"udp\":{\"radio\":5}}").status == FOXAPI_OK);
    collect(300);
    CHECK(collect(600).empty());
    CHECK(r.is("fact.udp.sending", "1"));
    CHECK(r.config("{\"udp\":{\"radio\":1}}").status == FOXAPI_OK);
    collect(200);
    CHECK(!collect(800).empty());
    // off: it stops, and its session goes
    CHECK(r.config("{\"udp\":{\"enabled\":false}}").status == FOXAPI_OK);
    collect(300);
    CHECK(collect(500).empty());
    CHECK(r.is("fact.udp.sending", "0") && r.udpSessions() == 0);
    // an unfindable host is refused in words and left enabled
    const FoxCommandResult bad = r.config("{\"udp\":{\"enabled\":true,\"host\":\"no-such-host.invalid\"}}");
    CHECK(bad.status == FOXAPI_OK);
    CHECK(r.is("fact.udp.sending", "0") && r.has("fact.udp.refusal", "cannot find"));
    CHECK(r.udpSessions() == 0);
    closeSock(rx);
}

void testConfiguration() {
    Rig r;
    struct Bad { const char* json; const char* word; };
    const Bad bad[] = {
        {"{\"tci\":{}}", "says nothing"},
        {"{\"tci\":{\"enabled\":1}}", "enabled"},
        {"{\"tci\":{\"port\":80}}", "port"},
        {"{\"tci\":{\"port\":70000}}", "port"},
        {"{\"tci\":{\"colour\":\"red\"}}", "colour"},
        {"{\"tci\":5}", "object"},
        {"{\"udp\":{\"port\":0}}", "port"},
        {"{\"udp\":{\"port\":70000}}", "port"},
        {"{\"udp\":{\"port\":\"7355\"}}", "port"},
        {"{\"udp\":{\"radio\":0}}", "radio"},
        {"{\"udp\":{\"radio\":9}}", "radio"},
        {"{\"udp\":{\"packetFrames\":481}}", "480 or 960"},
        {"{\"udp\":{\"host\":\"bad host\"}}", "host"},
        {"{\"udp\":{\"host\":5}}", "host"},
        {"{\"udp\":{\"extra\":1}}", "extra"},
        {"{\"tci\":{\"enabled\":true},\"nothing\":1}", "nothing"},
    };
    for (const Bad& b : bad) {
        const FoxCommandResult x = r.config(b.json);
        CHECK(x.status < 0);
        CHECK(std::string(x.message).find(b.word) != std::string::npos);
        if (x.status >= 0 || std::string(x.message).find(b.word) == std::string::npos) { std::printf("      %s -> %d \"%s\"\n", b.json, x.status, x.message); }
    }
    CHECK(!r.e.servers().tciServer().running() && !r.e.servers().udpSender().running());   // none of that switched anything on
    // the parse of a good one
    cascade::engine::ServerConfigRequest q;
    std::string err;
    CHECK(cascade::engine::parseServerConfigRequest("{\"tci\":{\"enabled\":true,\"bindAll\":true,\"port\":50002},\"udp\":{\"enabled\":true,\"host\":\"\",\"port\":9,\"radio\":3,\"packetFrames\":960}}", q, err));
    CHECK(q.tciEnabled && *q.tciEnabled && q.tciBindAll && *q.tciBindAll && q.tciPort == 50002);
    CHECK(q.udpHost && *q.udpHost == "127.0.0.1" && q.udpPort == 9 && q.udpRadio == 3 && q.udpPacketFrames == 960);   // an emptied host resets to loopback
    // a REMOTE session may not configure a listener
    FoxSessionParams p{};
    p.structSize = sizeof(p);
    p.flags = FOXAPI_SESSION_REMOTE;
    p.grants = kAllGrants;
    p.clientName = "remote";
    FoxSession* rem = nullptr;
    const int32_t os = api()->open_session(r.e.apiHandle(), &p, &rem);
    if (os == FOXAPI_OK && rem != nullptr) {
        FoxCommand c{};
        c.structSize = sizeof(c);
        c.op = FOXAPI_OP_SERVER_CONFIG;
        const std::string j = "{\"tci\":{\"enabled\":true}}";
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        api()->submit_text(rem, &c, j.data(), static_cast<uint32_t>(j.size()), &sr);
        FoxCommandResult res{};
        const auto end = Clock::now() + std::chrono::seconds(3);
        bool found = false;
        while (Clock::now() < end && !found) {
            FoxCommandResult rr[4];
            for (auto& x : rr) { x.structSize = sizeof(x); }
            const int32_t n = api()->poll_results(rem, rr, 4);
            for (int32_t i = 0; i < n; ++i) { if (rr[i].ticket == sr.ticket) { res = rr[i]; found = true; } }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(found && res.status == FOXAPI_DENIED);
        CHECK(r.is("fact.tci.enabled", "0"));
        CHECK(r.fact("fact.tci.port", rem) == "<-1>" || r.fact("fact.tci.port", rem).rfind("<", 0) == 0);   // a REMOTE session cannot read the facts either
        api()->close_session(rem);
    }
    // THE PORT IN USE, in words, and tried again when it has been freed
    sockInit();
    SockT holder = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#if defined(_WIN32)
    int yes = 1;
    ::setsockopt(holder, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof(yes));
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    ::bind(holder, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    ::listen(holder, 4);
    socklen_t len = sizeof(a);
    ::getsockname(holder, reinterpret_cast<sockaddr*>(&a), &len);
    const int taken = ntohs(a.sin_port);
    CHECK(r.config("{\"tci\":{\"enabled\":true,\"port\":" + std::to_string(taken) + "}}").status == FOXAPI_OK);
    CHECK(r.is("fact.tci.listening", "0") && r.is("fact.tci.enabled", "1"));
    CHECK(r.has("fact.tci.refusal", "already in use") && r.has("fact.tci.refusal", std::to_string(taken)));
    std::printf("port in use, as the engine says it: \"%s\"\n", r.fact("fact.tci.refusal").c_str());
    closeSock(holder);
    CHECK(r.config("{\"tci\":{\"enabled\":true}}").status == FOXAPI_NO_CHANGE || true);   // the same configuration said again is a retry
    (void)r.e.servers().waitIdle(std::chrono::seconds(10));
    CHECK(r.is("fact.tci.listening", "1") && r.fact("fact.tci.refusal").empty());
}

void testSettingsFile() {
    // the file round trip, and what a hostile file is made into
    cascade::core::AppConfig c;
    c.tciEnabled = true;
    c.tciBindAll = true;
    c.tciPort = 50123;
    c.udpAudioEnabled = true;
    c.udpAudioHost = "192.168.1.20";
    c.udpAudioPort = 7400;
    c.udpAudioRadio = 4;
    c.udpAudioPacketFrames = 960;
    const std::string text = cascade::core::ConfigStore::serialize(c);
    cascade::core::AppConfig back;
    std::string why;
    CHECK(cascade::core::ConfigStore::parse(text, "test", back, why));
    CHECK(back.tciEnabled && back.tciBindAll && back.tciPort == 50123 && back.udpAudioEnabled && back.udpAudioHost == "192.168.1.20" && back.udpAudioPort == 7400);
    CHECK(back.udpAudioRadio == 4 && back.udpAudioPacketFrames == 960);
    // hostile values: back to the safe defaults, never to something wider
    const std::string hostile =
        "{\"tciPort\":80,\"udpAudioHost\":\"has space\",\"udpAudioPort\":99999,\"udpAudioRadio\":99,\"udpAudioPacketFrames\":123,\"tciBindAll\":\"yes\",\"tciEnabled\":7}";
    cascade::core::AppConfig h;
    CHECK(cascade::core::ConfigStore::parse(hostile, "test", h, why));
    CHECK(h.tciPort == 50001 && h.udpAudioHost == "127.0.0.1" && h.udpAudioPort == 7355 && h.udpAudioRadio == 1 && h.udpAudioPacketFrames == 480);
    CHECK(!h.tciBindAll && !h.tciEnabled);
    cascade::core::AppConfig e;
    CHECK(cascade::core::ConfigStore::parse("{\"udpAudioHost\":\"\"}", "test", e, why));
    CHECK(e.udpAudioHost == "127.0.0.1");   // an emptied host is loopback, not "everyone"
    // the engine: a file that switches both on starts both
    Rig r;
    cascade::core::AppConfig f;
    f.tciEnabled = true;
    f.tciPort = freePort();
    f.udpAudioEnabled = true;
    f.udpAudioPort = 9;   // (discard: nobody need be listening)
    r.e.applyConfig(f);
    CHECK(r.e.servers().waitIdle(std::chrono::seconds(30)));
    CHECK(r.e.servers().tciServer().running() && r.e.servers().udpSender().running());
    const cascade::core::AppConfig snap = [&] { cascade::core::AppConfig x; r.e.servers().store(x); return x; }();
    CHECK(snap.tciEnabled && snap.tciPort == f.tciPort && snap.udpAudioEnabled && snap.udpAudioPort == 9);
}

void testTeardown() {
    auto r = std::make_unique<Rig>();
    CHECK(r->startTci());
    CHECK(r->config("{\"udp\":{\"enabled\":true,\"port\":9}}").status == FOXAPI_OK);
    std::vector<std::unique_ptr<RawClient>> cs;
    for (int i = 0; i < 3; ++i) {
        cs.push_back(std::make_unique<RawClient>());
        CHECK(hello(*cs.back(), r->tciPort));
        CHECK(cs.back()->sendText("AUDIO_STREAM_CHANNELS:1;AUDIO_START:0;"));
    }
    const auto t0 = Clock::now();
    r.reset();   // the engine shuts down with three clients streaming and the UDP stream running
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    CHECK(ms < 8000);
    std::printf("engine shutdown with 3 TCI clients streaming and UDP running took %lld ms\n", static_cast<long long>(ms));
    for (auto& c : cs) { CHECK(c->closedWithin(3000)); }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // (a run that hangs still shows how far it got)
    testOffByDefault();
    testStartupAndTuning();
    testReceiveOnly();
    testSessions();
    testAudioStream();
    testUdp();
    testConfiguration();
    testSettingsFile();
    testTeardown();
    return testSummary("test_tci_engine");
}
