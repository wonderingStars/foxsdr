// test_kiwisdr_client.cpp - the KiwiSDR listen-only client against a scripted
// stand-in receiver on a real loopback socket (tests/kiwisdr_fake_server.hpp).
//
// WHAT THIS PROVES, AND WHAT IT CANNOT. The stand-in was written from the SAME
// outline as the client (src/source/kiwisdr_protocol.hpp), so these tests prove
// the client is self-consistent with that outline and that its behaviour
// (politeness, bounds, bounded close, no reconnect, no redirect-following) is
// what the design says. They do NOT prove that a real KiwiSDR speaks these
// bytes, orders the opening commands this way or accepts them. Nothing here
// has ever connected to a real receiver, and nothing in this file connects to
// anything but 127.0.0.1. Every command string and byte below is a literal.
//
// Where the outline says nothing, the expectation is marked ASSUMED and listed
// in docs/feature-records/kiwi-client.md.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kiwisdr_fake_server.hpp"
#include "source/kiwisdr_client.hpp"
#include "test_check.hpp"

using cascade::source::KiwiSdrClient;
using kiwifake::Command;
using kiwifake::Conn;
using kiwifake::FakeKiwi;
using State = KiwiSdrClient::State;
using ms = std::chrono::milliseconds;

namespace {

using Clock = std::chrono::steady_clock;

bool waitFor(const std::function<bool()>& pred, int timeoutMs) {
    const auto end = Clock::now() + ms(timeoutMs);
    while (Clock::now() < end) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(ms(5));
    }
    return pred();
}

long long since(Clock::time_point t) {
    return std::chrono::duration_cast<ms>(Clock::now() - t).count();
}

// What a script saw, for the test thread to inspect.
struct Probe {
    std::mutex m;
    std::vector<Command> cmds;
    std::string path;
    std::atomic<int> conns{0};
    std::atomic<int> goneSeen{0};
    std::atomic<int> unmasked{0};

    void attach(Conn& c) {
        c.onCommand = [this](const Command& x) {
            std::lock_guard<std::mutex> lk(m);
            cmds.push_back(x);
        };
    }
    std::vector<Command> commands() {
        std::lock_guard<std::mutex> lk(m);
        return cmds;
    }
    std::size_t count() {
        std::lock_guard<std::mutex> lk(m);
        return cmds.size();
    }
    std::vector<std::string> texts() {
        std::vector<std::string> t;
        for (const auto& c : commands()) { t.push_back(c.text); }
        return t;
    }
    std::string pathSeen() {
        std::lock_guard<std::mutex> lk(m);
        return path;
    }
};

// A script that upgrades, records, runs `body`, then (if the body returns while
// the client is still there) waits for the client to go so the probe sees it.
FakeKiwi::Script serve(Probe& p, std::function<void(Conn&, int)> body) {
    return [&p, body](Conn& c, int idx) {
        p.attach(c);
        if (!c.upgrade()) { return; }
        {
            std::lock_guard<std::mutex> lk(p.m);
            p.path = c.path;
        }
        ++p.conns;
        if (body) { body(c, idx); }
        while (c.sleep(10)) {}
        p.unmasked += c.unmaskedClientFrames();
        ++p.goneSeen;
    };
}

KiwiSdrClient::Options optsFor(const FakeKiwi& srv) {
    KiwiSdrClient::Options o;
    o.host = "127.0.0.1";
    o.port = srv.port();
    o.connectWait = ms(2000);
    o.stallWait = ms(5000);
    o.keepAliveInterval = ms(100);
    return o;
}

// 16-bit big-endian PCM (ASSUMED byte order), `n` samples all equal to `v`.
std::string pcmOf(int v, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) {
        s.push_back(static_cast<char>((v >> 8) & 0xFF));
        s.push_back(static_cast<char>(v & 0xFF));
    }
    return s;
}

std::vector<std::int16_t> drain(KiwiSdrClient& c) {
    std::vector<std::int16_t> all;
    std::int16_t buf[4096];
    for (;;) {
        const std::size_t n = c.read(buf, 4096);
        if (n == 0) { break; }
        all.insert(all.end(), buf, buf + n);
    }
    return all;
}

// --- 1. the opening commands ---------------------------------------------------------

void testHandshakeAndCommands() {
    Probe probe;
    FakeKiwi srv(serve(probe, [](Conn& c, int) { (void)c; }));
    CHECK(srv.ok());
    KiwiSdrClient client;
    CHECK(client.open(optsFor(srv)));
    CHECK(waitFor([&] { return probe.count() >= 4; }, 3000));

    // The path: /<digits>/SND.
    const std::string path = probe.pathSeen();
    CHECK(path.size() > 5 && path.front() == '/' && path.compare(path.size() - 4, 4, "/SND") == 0);
    bool digits = path.size() > 5;
    for (std::size_t i = 1; i + 4 < path.size(); ++i) { digits = digits && path[i] >= '0' && path[i] <= '9'; }
    CHECK(digits);

    // The opening commands, in order (the ORDER is ASSUMED; the strings are the outline's).
    const auto t = probe.texts();
    CHECK(t.size() >= 4);
    if (t.size() >= 4) {
        CHECK(t[0] == "SET auth t=kiwi p=");
        CHECK(t[1] == "SET ident_user=FoxSDR");
        CHECK(t[2] == "SET AR OK in=12000 out=12000");
        CHECK(t[3] == "SET compression=1");
    }
    CHECK(client.state() == State::Connecting);  // no audio yet

    // Tune after connect.
    CHECK(client.tune("usb", 300, 2700, 7055000.0));
    CHECK(waitFor([&] { return probe.count() >= 5; }, 3000));
    // Refused arguments send nothing.
    CHECK(!client.tune("iq", 0, 0, 7e6));
    CHECK(!client.tune("usb SET x", 0, 0, 7e6));
    CHECK(!client.tune("usb", 0, 0, -5.0));
    CHECK(client.tune("lsb", -2700, -300, 3573000.0));
    CHECK(waitFor([&] {
        for (const auto& s : probe.texts()) {
            if (s == "SET mod=lsb low_cut=-2700 high_cut=-300 freq=3573.000") { return true; }
        }
        return false;
    }, 3000));
    bool sawUsb = false;
    for (const auto& s : probe.texts()) {
        if (s == "SET mod=usb low_cut=300 high_cut=2700 freq=7055.000") { sawUsb = true; }
        CHECK(s.find("mod=iq") == std::string::npos);
        CHECK(s.find("x low_cut") == std::string::npos);
    }
    CHECK(sawUsb);
    CHECK(client.mode() == "lsb");
    CHECK_NEAR(client.frequencyHz(), 3573000.0, 1e-6);
    client.close();
    CHECK(!client.tune("usb", 300, 2700, 7055000.0));  // nothing open any more
    // Everything the client sent was a masked frame (RFC 6455).
    CHECK(waitFor([&] { return probe.goneSeen.load() == 1; }, 3000));
    CHECK(probe.unmasked.load() == 0);
}

void testPasswordAndPlainPcmRequest() {
    Probe probe;
    FakeKiwi srv(serve(probe, nullptr));
    KiwiSdrClient client;
    auto o = optsFor(srv);
    o.password = "pw d";  // ASSUMED: percent-encoded on the wire
    o.identUser = "FoxSDR";
    o.adpcm = false;
    CHECK(client.open(o));
    CHECK(waitFor([&] { return probe.count() >= 4; }, 3000));
    const auto t = probe.texts();
    if (t.size() >= 4) {
        CHECK(t[0] == "SET auth t=kiwi p=pw%20d");
        CHECK(t[1] == "SET ident_user=FoxSDR");
        CHECK(t[3] == "SET compression=0");
    }
    client.close();

    // A password above the limit cannot be sent: the open fails and nothing is sent.
    Probe probe2;
    FakeKiwi srv2(serve(probe2, nullptr));
    KiwiSdrClient bad;
    auto o2 = optsFor(srv2);
    o2.password = std::string(500, 'x');
    CHECK(!bad.open(o2));
    CHECK(bad.state() == State::ConnectFailed);
    std::this_thread::sleep_for(ms(100));
    CHECK(probe2.count() == 0);
}

// --- 2. audio ------------------------------------------------------------------------------

void testPcmAudio() {
    Probe probe;
    FakeKiwi srv(serve(probe, [](Conn& c, int) {
        // 258 = 0x0102 -> RSSI -101.2 dBm.
        c.sendSnd(0x00, 10, 0x0102, std::string("\x00\x01\xFF\xFF\x80\x00\x7F\xFF", 8));
        c.sendSnd(0x00, 11, 0x0102, pcmOf(1000, 3));
        c.sendSnd(0x00, 12, 0x04D2, pcmOf(-1000, 2));
    }));
    KiwiSdrClient client;
    CHECK(client.open(optsFor(srv)));
    CHECK(waitFor([&] { return client.audioFrames() == 3; }, 3000));
    CHECK(client.state() == State::Playing);
    const auto s = drain(client);
    const std::vector<std::int16_t> want = {1, -1, -32768, 32767, 1000, 1000, 1000, -1000, -1000};
    CHECK(s == want);
    CHECK(client.haveSmeter());
    CHECK(client.smeterRaw() == 0x04D2);
    CHECK_NEAR(client.rssiDbm(), 0.1 * 1234 - 127.0, 1e-9);
    CHECK(client.sequenceGaps() == 0 && client.missedFrames() == 0);
    CHECK(client.samplesDelivered() == 9);
    CHECK(KiwiSdrClient::audioRateHz() == 12000);
    client.close();
}

void testAdpcmAudio() {
    Probe probe;
    // The hand vector from test_kiwisdr_protocol: 0x70 -> 0, 11; 0x8F -> -19, -23, in two
    // frames (the decoder state carries across frames: ASSUMED).
    FakeKiwi srv(serve(probe, [](Conn& c, int) {
        c.sendSnd(0x10, 1, 100, "\x70");
        c.sendSnd(0x10, 2, 100, "\x8F");
    }));
    KiwiSdrClient client;
    CHECK(client.open(optsFor(srv)));
    CHECK(waitFor([&] { return client.audioFrames() == 2; }, 3000));
    const auto s = drain(client);
    const std::vector<std::int16_t> want = {0, 11, -19, -23};
    CHECK(s == want);
    client.close();
}

void testSequenceGaps() {
    Probe probe;
    FakeKiwi srv(serve(probe, [](Conn& c, int) {
        for (std::uint32_t q : {5u, 6u, 9u, 10u, 0xFFFFFFFFu, 0u, 1u}) { c.sendSnd(0x00, q, 1, pcmOf(7, 1)); }
    }));
    KiwiSdrClient client;
    CHECK(client.open(optsFor(srv)));
    CHECK(waitFor([&] { return client.audioFrames() == 7; }, 3000));
    // 6 -> 9 skips two frames; 10 -> 0xFFFFFFFF jumps (backwards in serial arithmetic: a gap,
    // nothing counted as missed); 0xFFFFFFFF -> 0 -> 1 wrap with no gap.
    CHECK(client.sequenceGaps() == 2);
    CHECK(client.missedFrames() == 2);
    client.close();
}

void testChunkBoundaries() {
    // Frames split across TCP segments (after the 2-byte WebSocket header, inside the
    // payload), as WebSocket fragments with pings in between, two frames in one write,
    // and a frame cut inside its payload: every sample must arrive, in order. (A cut
    // inside the 2-byte WebSocket HEADER is a different matter: see
    // testHeaderSplitKnownDefect.)
    std::vector<std::int16_t> want;
    std::vector<std::string> payloads;
    for (int k = 0; k < 6; ++k) {
        std::string audio;
        for (int i = 0; i < 12; ++i) {
            const int v = k * 100 + i - 20;
            audio += pcmOf(v, 1);
            want.push_back(static_cast<std::int16_t>(v));
        }
        payloads.push_back(Conn::sndPayload(0x00, 100 + k, 50, audio));
    }
    Probe probe;
    FakeKiwi srv(serve(probe, [payloads](Conn& c, int) {
        c.sendChunked(Conn::frameBytes(0x2, payloads[0]), {2, 7, 1, 11, 3}, 2);
        c.sendChunked(Conn::frameBytes(0x2, payloads[1]), {4, 2, 9}, 2);
        c.sendFragmented(payloads[2], 3);
        c.sendFragmented(payloads[3], 7);
        // Two frames in one TCP write, the second cut after its header and 5 bytes.
        c.sendRaw(Conn::frameBytes(0x2, payloads[4]) + Conn::frameBytes(0x2, payloads[5]).substr(0, 7));
        c.sleep(30);
        c.sendRaw(Conn::frameBytes(0x2, payloads[5]).substr(7));
    }));
    KiwiSdrClient client;
    CHECK(client.open(optsFor(srv)));
    CHECK(waitFor([&] { return client.audioFrames() == 6; }, 5000));
    CHECK(drain(client) == want);
    CHECK(client.sequenceGaps() == 0);
    CHECK(client.state() == State::Playing);
    client.close();
}

// KNOWN DEFECT OF THE VENDORED httplib (third_party/cpp_httplib, read_websocket_frame):
// the frame HEADER is read with strm.read(buf, 2) (and 2, 8 and 4 bytes for the extended
// length and the mask), and SocketStream::read returns whatever one recv gave it - so a
// TCP segment boundary that falls inside those few bytes makes the library treat the
// connection as failed. Over a real network a segment boundary lands anywhere in the
// stream; on loopback it only happens when a sender cuts there on purpose, as this stand-in
// does. The client therefore reports such a stream as Dropped. This test PINS that
// behaviour so it cannot be forgotten: when the library is patched (docs/feature-records/
// kiwi-client.md has the three-line change), this test goes red - flip the expectation to
// "all samples arrive" and delete the note.
void testHeaderSplitKnownDefect() {
    Probe probe;
    FakeKiwi srv(serve(probe, [](Conn& c, int) {
        c.sendChunked(Conn::frameBytes(0x2, Conn::sndPayload(0x00, 1, 1, pcmOf(5, 8))), {1}, 3);
    }));
    KiwiSdrClient client;
    CHECK(client.open(optsFor(srv)));
    const bool dropped = waitFor([&] { return client.state() == State::Dropped; }, 3000);
    const bool arrived = client.audioFrames() == 1;
    std::printf("NOTE header-split frame: %s\n", arrived ? "delivered (httplib now copes with short header reads: update this test)"
                                                       : "connection dropped (known httplib defect, see the record)");
    CHECK(dropped && !arrived);
    client.close();
}

void testRingOverflowIsCounted() {
    Probe probe;
    FakeKiwi srv(serve(probe, [](Conn& c, int) {
        const std::string audio = pcmOf(3, 4000);
        for (std::uint32_t q = 0; q < 40; ++q) { c.sendSnd(0x00, q, 1, audio); }
    }));
    KiwiSdrClient client;
    CHECK(client.open(optsFor(srv)));
    CHECK(waitFor([&] { return client.audioFrames() == 40; }, 5000));
    CHECK(client.samplesDelivered() + client.droppedSamples() == 160000);
    CHECK(client.samplesDelivered() <= KiwiSdrClient::kRingSamples);
    CHECK(client.droppedSamples() > 0);
    CHECK(drain(client).size() == client.samplesDelivered());
    CHECK(client.state() == State::Playing);
    client.close();
}

void testReadWaitIsBounded() {
    Probe probe;
    std::atomic<bool> go{false};
    FakeKiwi srv(serve(probe, [&go](Conn& c, int) {
        while (!go.load() && c.sleep(5)) {}
        c.sendSnd(0x00, 1, 1, pcmOf(9, 8));
    }));
    KiwiSdrClient client;
    CHECK(client.open(optsFor(srv)));
    std::int16_t buf[64];
    auto t0 = Clock::now();
    CHECK(client.read(buf, 64, ms(150)) == 0);
    const long long waited = since(t0);
    CHECK(waited >= 120 && waited < 1000);
    go = true;
    t0 = Clock::now();
    const std::size_t got = client.read(buf, 64, ms(3000));
    CHECK(got == 8);
    CHECK(since(t0) < 1500);
    client.close();
}

// --- 3. keep-alive ----------------------------------------------------------------------------

void testKeepAliveCadence() {
    Probe probe;
    FakeKiwi srv(serve(probe, nullptr));
    KiwiSdrClient client;
    auto o = optsFor(srv);
    o.keepAliveInterval = KiwiSdrClient::kKeepAliveInterval;  // the default, about once a second
    CHECK(KiwiSdrClient::kKeepAliveInterval == ms(1000));
    CHECK(client.open(o));
    std::this_thread::sleep_for(ms(3600));
    std::vector<Command> ka;
    for (const auto& c : probe.commands()) {
        if (c.text == "SET keepalive") { ka.push_back(c); }
    }
    CHECK(ka.size() >= 3 && ka.size() <= 4);
    for (std::size_t i = 1; i < ka.size(); ++i) {
        const auto gap = std::chrono::duration_cast<ms>(ka[i].at - ka[i - 1].at).count();
        CHECK(gap >= 700 && gap <= 1400);
    }
    CHECK(client.keepAlivesSent() == ka.size() || client.keepAlivesSent() == ka.size() + 1);
    client.close();
    // After close() no more keep-alives arrive.
    const std::size_t after = probe.count();
    std::this_thread::sleep_for(ms(1300));
    CHECK(probe.count() == after);
}

// --- 4. the receiver says no -----------------------------------------------------------------------

// Common checks for a refusal: sticky state, nothing more sent, the socket shut,
// audio sent after the refusal is never delivered, tune refused.
void expectStopped(KiwiSdrClient& client, Probe& probe, State want, int code) {
    CHECK(waitFor([&] { return client.state() == want; }, 3000));
    const auto st = client.status();
    CHECK(st.state == want);
    CHECK(st.code == code);
    CHECK(!st.detail.empty());
    // The server sees the connection end, and no keep-alive follows (the interval is 100 ms).
    CHECK(waitFor([&] { return probe.goneSeen.load() == 1; }, 3000));
    const std::size_t n = probe.count();
    std::this_thread::sleep_for(ms(500));
    CHECK(probe.count() == n);
    CHECK(client.samplesDelivered() == 0);
    CHECK(drain(client).empty());
    CHECK(!client.tune("usb", 300, 2700, 7e6));
    CHECK(client.state() == want);  // sticky
}

void testTooBusy() {
    Probe probe;
    FakeKiwi srv(serve(probe, [](Conn& c, int) {
        c.sendMsg("too_busy=3");
        c.sendSnd(0x00, 1, 1, pcmOf(5, 16));  // must never be delivered
    }));
    KiwiSdrClient client;
    CHECK(client.open(optsFor(srv)));
    expectStopped(client, probe, State::RefusedBusy, 3);
    client.close();
    CHECK(client.state() == State::RefusedBusy);  // close() keeps the reported reason
    std::this_thread::sleep_for(ms(800));
    CHECK(srv.accepted() == 1);  // and it did not come back
}

void testBadPassword() {
    for (int code : {1, 7}) {
        Probe probe;
        FakeKiwi srv(serve(probe, [code](Conn& c, int) {
            c.sendMsg("badp=" + std::to_string(code));
            c.sendSnd(0x00, 1, 1, pcmOf(5, 16));
        }));
        KiwiSdrClient client;
        CHECK(client.open(optsFor(srv)));
        expectStopped(client, probe, State::RefusedPassword, code);
        client.close();
    }
    // badp=0 is "accepted" (ASSUMED): the stream goes on.
    Probe probe;
    FakeKiwi srv(serve(probe, [](Conn& c, int) {
        c.sendMsg("badp=0");
        c.sendSnd(0x00, 1, 1, pcmOf(5, 4));
    }));
    KiwiSdrClient ok;
    CHECK(ok.open(optsFor(srv)));
    CHECK(waitFor([&] { return ok.state() == State::Playing; }, 3000));
    CHECK(drain(ok).size() == 4);
    ok.close();
}

void testDown() {
    Probe probe;
    FakeKiwi srv(serve(probe, [](Conn& c, int) {
        c.sendMsg("down=1");
        c.sendSnd(0x00, 1, 1, pcmOf(5, 16));
    }));
    KiwiSdrClient client;
    CHECK(client.open(optsFor(srv)));
    expectStopped(client, probe, State::ServerDown, 1);
    client.close();
}

void testRedirectIsReportedNotFollowed() {
    // The redirect points at a SECOND stand-in on loopback; if the client followed it,
    // that server would see a connection.
    Probe other;
    FakeKiwi second(serve(other, nullptr));
    const std::string target = "127.0.0.1:" + std::to_string(second.port());
    Probe probe;
    FakeKiwi srv(serve(probe, [target](Conn& c, int) {
        c.sendMsg("redirect=" + target);
        c.sendSnd(0x00, 1, 1, pcmOf(5, 16));
    }));
    KiwiSdrClient client;
    CHECK(client.open(optsFor(srv)));
    expectStopped(client, probe, State::Redirected, 0);
    CHECK(client.status().redirect == target);
    std::this_thread::sleep_for(ms(700));
    CHECK(second.accepted() == 0);
    CHECK(srv.accepted() == 1);
    client.close();
}

// --- 5. the receiver sends rubbish ----------------------------------------------------------------------

void expectMalformed(const std::string& payload, const char* what) {
    Probe probe;
    FakeKiwi srv(serve(probe, [payload](Conn& c, int) {
        c.sendBinary(payload);
        c.sendSnd(0x00, 1, 1, pcmOf(5, 16));  // after the bad frame: never delivered
    }));
    KiwiSdrClient client;
    CHECK(client.open(optsFor(srv)));
    if (!waitFor([&] { return client.state() == State::Malformed; }, 3000)) {
        std::printf("not Malformed: %s (state %d)\n", what, static_cast<int>(client.state()));
    }
    CHECK(client.state() == State::Malformed);
    CHECK(waitFor([&] { return probe.goneSeen.load() == 1; }, 3000));
    CHECK(client.samplesDelivered() == 0);
    client.close();
}

void testMalformedAndHostileFrames() {
    expectMalformed("SND\x00\x01", "SND cut inside its header");
    expectMalformed(Conn::sndPayload(0x00, 1, 1, "abc"), "PCM with half a sample");
    expectMalformed("MSG " + std::string(6000, 'x'), "oversized MSG text");
    expectMalformed(Conn::sndPayload(0x10, 1, 1, std::string(200 * 1024, '\x55')), "200 KiB frame");
    expectMalformed(Conn::sndPayload(0x00, 1, 1, std::string(1024 * 1024, '\x01')), "1 MiB frame");
    expectMalformed(std::string("EXT") + std::string(64 * 1024, 'x'), "EXT over the limit");

    // A header that DECLARES 20 MiB (above even the WebSocket layer's own 16 MiB cap) and then
    // says nothing: the library refuses it, the client reports a dropped connection and
    // delivers nothing.
    Probe probe;
    FakeKiwi srv(serve(probe, [](Conn& c, int) {
        std::string h;
        h.push_back(static_cast<char>(0x82));
        h.push_back(127);
        const std::uint64_t n = 20ull * 1024 * 1024;
        for (int i = 7; i >= 0; --i) { h.push_back(static_cast<char>((n >> (8 * i)) & 0xFF)); }
        c.sendRaw(h);
    }));
    KiwiSdrClient client;
    CHECK(client.open(optsFor(srv)));
    CHECK(waitFor([&] { return client.state() == State::Dropped; }, 3000));
    CHECK(client.samplesDelivered() == 0);
    client.close();

    // A frame the client does not know is ignored, not fatal; W/F and EXT likewise.
    Probe probe2;
    FakeKiwi srv2(serve(probe2, [](Conn& c, int) {
        c.sendBinary("ZZZ whatever");
        c.sendBinary(std::string("W/F") + std::string(500, '\x01'));
        c.sendBinary("EXT hello");
        c.sendMsg("audio_rate=12000 sample_rate=12001");
        c.sendSnd(0x00, 1, 1, pcmOf(5, 4));
    }));
    KiwiSdrClient fine;
    CHECK(fine.open(optsFor(srv2)));
    CHECK(waitFor([&] { return fine.audioFrames() == 1; }, 3000));
    CHECK(fine.state() == State::Playing);
    CHECK(fine.ignoredFrames() == 4);
    CHECK(drain(fine).size() == 4);
    fine.close();
}

// --- 6. the connection fails -----------------------------------------------------------------------------

void testStallReported() {
    Probe probe;
    FakeKiwi srv(serve(probe, [](Conn& c, int) { c.sendSnd(0x00, 1, 1, pcmOf(5, 4)); }));
    KiwiSdrClient client;
    auto o = optsFor(srv);
    o.stallWait = ms(500);
    const auto t0 = Clock::now();
    CHECK(client.open(o));
    CHECK(waitFor([&] { return client.state() == State::Dropped; }, 4000));
    CHECK(since(t0) < 3000);
    CHECK(client.status().detail.find("silent") != std::string::npos);
    CHECK(drain(client).size() == 4);  // what arrived before the stall is still readable
    std::this_thread::sleep_for(ms(800));
    CHECK(srv.accepted() == 1);  // no reconnect
    client.close();
}

void testDropMidStreamIsNotRetried() {
    for (int how = 0; how < 2; ++how) {
        Probe probe;
        FakeKiwi srv(serve(probe, [how](Conn& c, int) {
            c.sendSnd(0x00, 1, 1, pcmOf(5, 4));
            c.sendSnd(0x00, 2, 1, pcmOf(6, 4));
            c.sleep(100);
            if (how == 0) {
                c.reset();  // a reset, no close frame
            } else {
                c.goodbye();  // an orderly close frame
            }
        }));
        KiwiSdrClient client;
        CHECK(client.open(optsFor(srv)));
        CHECK(waitFor([&] { return client.state() == State::Dropped; }, 3000));
        CHECK(drain(client).size() == 8);
        // No reconnect, however long we look.
        std::this_thread::sleep_for(ms(1000));
        CHECK(srv.accepted() == 1);
        CHECK(client.state() == State::Dropped);
        client.close();
        CHECK(client.state() == State::Dropped);
    }
}

void testConnectFailures() {
    // A port nobody listens on.
    kiwifake::Sock probeSock = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ::bind(probeSock, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t len = sizeof a;
    ::getsockname(probeSock, reinterpret_cast<sockaddr*>(&a), &len);
    const std::uint16_t deadPort = ntohs(a.sin_port);
    kiwifake::closeSock(probeSock);

    KiwiSdrClient refused;
    KiwiSdrClient::Options o;
    o.host = "127.0.0.1";
    o.port = deadPort;
    o.connectWait = ms(1500);
    CHECK(!refused.open(o));
    CHECK(refused.state() == State::ConnectFailed);
    CHECK(!refused.status().detail.empty());
    CHECK(!refused.open(o));  // one open per object

    // Hosts that are not hosts.
    for (const char* h : {"", "a b", "x/y", "h?x=1", "u@h", "127.0.0.1\r\nX: y"}) {
        KiwiSdrClient bad;
        KiwiSdrClient::Options b;
        b.host = h;
        CHECK(!bad.open(b));
        CHECK(bad.state() == State::ConnectFailed);
    }
    KiwiSdrClient zero;
    KiwiSdrClient::Options z;
    z.port = 0;
    CHECK(!zero.open(z));

    // A server that answers the upgrade with HTTP 404.
    Probe probe;
    FakeKiwi srv([&probe](Conn& c, int) {
        probe.attach(c);
        if (!c.upgrade(false)) { return; }
        c.sendRaw("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
        c.sleep(300);
    });
    KiwiSdrClient notFound;
    CHECK(!notFound.open(optsFor(srv)));
    CHECK(notFound.state() == State::ConnectFailed);
    CHECK(notFound.status().detail.find("404") != std::string::npos);
    CHECK(notFound.samplesDelivered() == 0);

    // Never opened: everything is safe.
    KiwiSdrClient never;
    CHECK(never.state() == State::Idle);
    CHECK(never.status().state == State::Idle);
    std::int16_t b[4];
    CHECK(never.read(b, 4) == 0);
    CHECK(!never.tune("usb", 0, 0, 1e6));
    never.close();
    never.close();
    CHECK(never.sequenceGaps() == 0 && !never.haveSmeter());
}

// --- 7. close ---------------------------------------------------------------------------------------------------

void testCloseIsBoundedWhileTheServerStalls() {
    const auto abandonedBefore = KiwiSdrClient::threadsAbandoned();
    {
        Probe probe;
        FakeKiwi srv(serve(probe, nullptr));  // upgrades, then says nothing at all
        KiwiSdrClient client;
        auto o = optsFor(srv);
        o.stallWait = ms(30000);  // far longer than the bound: close must not wait for it
        CHECK(client.open(o));
        CHECK(waitFor([&] { return probe.count() >= 4; }, 3000));
        const auto t0 = Clock::now();
        client.close();
        const long long took = since(t0);
        std::printf("close() with a stalled server took %lld ms\n", took);
        CHECK(took < 2500);
        CHECK(client.state() == State::Closed);
        client.close();  // idempotent
        CHECK(waitFor([&] { return probe.goneSeen.load() == 1; }, 3000));
    }
    {
        // A server that accepts the TCP connection and never answers the upgrade.
        Probe probe;
        FakeKiwi srv([&probe](Conn& c, int) {
            probe.attach(c);
            if (!c.upgrade(false)) { return; }
            while (c.sleep(10)) {}
        });
        KiwiSdrClient client;
        auto o = optsFor(srv);
        o.connectWait = ms(30000);
        o.stallWait = ms(30000);
        std::atomic<int> result{-1};
        std::thread opener([&] { result = client.open(o) ? 1 : 0; });
        std::this_thread::sleep_for(ms(300));
        CHECK(result.load() == -1);  // still waiting for the 101
        const auto t0 = Clock::now();
        client.close();
        opener.join();
        const long long took = since(t0);
        std::printf("close() during a stalled upgrade took %lld ms\n", took);
        CHECK(took < 2500);
        CHECK(result.load() == 0);
        CHECK(client.state() == State::Closed);
    }
    {
        // The destructor does the same as close().
        Probe probe;
        FakeKiwi srv(serve(probe, nullptr));
        const auto t0 = Clock::now();
        {
            KiwiSdrClient client;
            auto o = optsFor(srv);
            o.stallWait = ms(30000);
            CHECK(client.open(o));
            CHECK(waitFor([&] { return probe.count() >= 4; }, 3000));
        }
        CHECK(since(t0) < 3500);
    }
    CHECK(KiwiSdrClient::threadsAbandoned() == abandonedBefore);
}

void testSecondClientIsIndependent() {
    // Two clients to ONE stand-in: connection 0 is refused, connection 1 plays. The
    // refusal of one never touches the other, and closing one leaves the other running.
    Probe probe;
    FakeKiwi srv(serve(probe, [](Conn& c, int idx) {
        if (idx == 0) {
            c.sendMsg("too_busy=1");
        } else {
            for (std::uint32_t q = 0; q < 5; ++q) { c.sendSnd(0x00, q, 7, pcmOf(222, 10)); }
        }
    }));
    KiwiSdrClient a;
    CHECK(a.open(optsFor(srv)));
    CHECK(waitFor([&] { return a.state() == State::RefusedBusy; }, 3000));
    KiwiSdrClient b;
    CHECK(b.open(optsFor(srv)));
    CHECK(waitFor([&] { return b.audioFrames() == 5; }, 3000));
    CHECK(a.state() == State::RefusedBusy && b.state() == State::Playing);
    a.close();
    const auto s = drain(b);
    CHECK(s.size() == 50);
    for (auto v : s) { CHECK(v == 222); }
    CHECK(b.state() == State::Playing);
    b.close();

    // Two playing at once, each with its own samples, sequence counters and S-meter.
    Probe probe2;
    FakeKiwi srv2(serve(probe2, [](Conn& c, int idx) {
        const int v = idx == 0 ? 1000 : 2000;
        const std::uint16_t sm = idx == 0 ? 1270 : 1370;
        for (std::uint32_t q = 0; q < 6; ++q) {
            c.sendSnd(0x00, idx == 0 ? q : q * 2, sm, pcmOf(v, 5));
        }
    }));
    KiwiSdrClient c0;
    CHECK(c0.open(optsFor(srv2)));
    CHECK(waitFor([&] { return c0.audioFrames() == 6; }, 3000));
    KiwiSdrClient c1;
    CHECK(c1.open(optsFor(srv2)));
    CHECK(waitFor([&] { return c1.audioFrames() == 6; }, 3000));
    for (auto v : drain(c0)) { CHECK(v == 1000); }
    for (auto v : drain(c1)) { CHECK(v == 2000); }
    CHECK_NEAR(c0.rssiDbm(), 0.0, 1e-9);
    CHECK_NEAR(c1.rssiDbm(), 10.0, 1e-9);
    CHECK(c0.sequenceGaps() == 0);
    CHECK(c1.sequenceGaps() == 5);  // its own counter: 0,2,4,... skips each time
    c0.close();
    c1.close();
}

// --- 8. the real httplib server, for good measure -----------------------------------------------------------------

void testAgainstHttplibServer() {
    // The client through the vendored httplib::Server's own WebSocket route, not the
    // stand-in: the upgrade, the masking and the framing are the library's on both sides.
    std::mutex m;
    std::vector<std::string> got;
    std::string pathTs;
    std::atomic<int> handlers{0};
    httplib::Server svr;
    svr.set_websocket_ping_interval(0);
    svr.WebSocket("/:ts/SND", [&](const httplib::Request& req, httplib::ws::WebSocket& ws) {
        ++handlers;
        {
            std::lock_guard<std::mutex> lk(m);
            auto it = req.path_params.find("ts");
            pathTs = it == req.path_params.end() ? std::string("<none>") : it->second;
        }
        const std::string f = Conn::sndPayload(0x00, 1, 1270, pcmOf(1234, 6));
        ws.send(f.data(), f.size());
        std::string msg;
        while (ws.read(msg) != httplib::ws::Fail) {
            std::lock_guard<std::mutex> lk(m);
            got.push_back(msg);
        }
    });
    const int port = svr.bind_to_any_port("127.0.0.1");
    CHECK(port > 0);
    if (port <= 0) { return; }
    std::thread th([&] { svr.listen_after_bind(); });
    svr.wait_until_ready();

    KiwiSdrClient client;
    KiwiSdrClient::Options o;
    o.host = "127.0.0.1";
    o.port = static_cast<std::uint16_t>(port);
    o.keepAliveInterval = ms(100);
    o.timestamp = 1234567890;
    CHECK(client.open(o));
    CHECK(waitFor([&] { return client.audioFrames() == 1; }, 3000));
    const auto s = drain(client);
    CHECK(s.size() == 6 && (s.empty() || s[0] == 1234));
    CHECK(waitFor([&] {
        std::lock_guard<std::mutex> lk(m);
        return got.size() >= 5;
    }, 3000));
    {
        std::lock_guard<std::mutex> lk(m);
        CHECK(pathTs == "1234567890");
        CHECK(got.size() >= 4 && got[0] == "SET auth t=kiwi p=");
        if (got.size() >= 4) { CHECK(got[1] == "SET ident_user=FoxSDR"); }
    }
    client.close();
    std::this_thread::sleep_for(ms(300));
    svr.stop();
    th.join();
}

}  // namespace

int main() {
    testHandshakeAndCommands();
    testPasswordAndPlainPcmRequest();
    testPcmAudio();
    testAdpcmAudio();
    testSequenceGaps();
    testChunkBoundaries();
    testHeaderSplitKnownDefect();
    testRingOverflowIsCounted();
    testReadWaitIsBounded();
    testKeepAliveCadence();
    testTooBusy();
    testBadPassword();
    testDown();
    testRedirectIsReportedNotFollowed();
    testMalformedAndHostileFrames();
    testStallReported();
    testDropMidStreamIsNotRetried();
    testConnectFailures();
    testCloseIsBoundedWhileTheServerStalls();
    testSecondClientIsIndependent();
    testAgainstHttplibServer();
    return testSummary("test_kiwisdr_client");
}
