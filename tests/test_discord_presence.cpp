// test_discord_presence.cpp - Discord rich presence (net/discord_presence.hpp) against a STAND-IN Discord: a named-pipe server in this file that
// speaks the frame protocol from Discord's documentation. No network is used: the pipe is local, and no real Discord client is involved.
//
//   [A] pure: frame bytes, the parser, JSON escaping, the 128-byte cap on a character boundary, client IDs, reply parsing, frequency text,
//       what the profile line says by default and with the box ticked, hostile mode names; fuzz runs.
//   [B] live: OFF with no client ID (no connection is even attempted) and with a bad one; the handshake and READY; the default line (no
//       frequency, no mode); the ticked line; ping answered; coalescing and not resending an unchanged line; clear; clear at the end; a refusal and
//       the retry; a CLOSE frame and the reconnect that says the line again; garbage from the server; Discord starting late.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "net/discord_presence.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace cascade::net;
using Clock = std::chrono::steady_clock;

namespace {

std::string bytesToHex(const std::vector<std::uint8_t>& b) {
    std::string s;
    char t[4];
    for (auto x : b) { std::snprintf(t, sizeof t, "%02X", x); s += t; }
    return s;
}

void testPure() {
    using namespace discord;
    CHECK(bytesToHex(encodeFrame(Op::Handshake, "{}")) == "00000000" "02000000" "7B7D");
    CHECK(bytesToHex(encodeFrame(Op::Pong, "")) == "04000000" "00000000");
    {   // a frame fed a byte at a time, and two in one piece
        auto f = encodeFrame(Op::Frame, "{\"a\":1}");
        const auto g = encodeFrame(Op::Ping, "xy");
        f.insert(f.end(), g.begin(), g.end());
        FrameParser p;
        std::vector<FrameParser::Frame> out;
        for (auto b : f) { CHECK(p.feed(&b, 1, out)); }
        CHECK(out.size() == 2 && out[0].op == Op::Frame && out[0].payload == "{\"a\":1}" && out[1].op == Op::Ping && out[1].payload == "xy");
        FrameParser p2;
        std::vector<FrameParser::Frame> out2;
        CHECK(p2.feed(f.data(), f.size(), out2) && out2.size() == 2);
    }
    {   // violations are permanent
        auto bad = encodeFrame(Op::Frame, "x");
        bad[0] = 9;
        FrameParser p;
        std::vector<FrameParser::Frame> out;
        CHECK(!p.feed(bad.data(), bad.size(), out) && !p.error().empty());
        const auto good = encodeFrame(Op::Frame, "x");
        CHECK(!p.feed(good.data(), good.size(), out));
        std::vector<std::uint8_t> big = {1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0x7F};
        FrameParser p2;
        CHECK(!p2.feed(big.data(), big.size(), out));
        std::vector<std::uint8_t> edge = {1, 0, 0, 0, 0, 0, 1, 0};   // exactly 64 KiB: accepted, waiting for the rest
        FrameParser p3;
        CHECK(p3.feed(edge.data(), edge.size(), out));
    }
    CHECK(handshakePayload("123456789012345678") == "{\"v\":1,\"client_id\":\"123456789012345678\"}");
    CHECK(setActivityPayload(42, "7", activityJson("Listening to the radio", "")) ==
          "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":42,\"activity\":{\"type\":2,\"details\":\"Listening to the radio\"}},\"nonce\":\"7\"}");
    CHECK(setActivityPayload(42, "8", "") == "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":42},\"nonce\":\"8\"}");
    CHECK(activityJson("a", "b") == "{\"type\":2,\"details\":\"a\",\"state\":\"b\"}");
    CHECK(jsonEscape("a\"b\\c\nd\te\x01" "f") == "a\\\"b\\\\c\\nd\\te\\u0001f");
    CHECK(jsonEscape("caf\xC3\xA9 \xF0\x9F\x93\xBB") == "caf\xC3\xA9 \xF0\x9F\x93\xBB");   // UTF-8 passes through
    CHECK(capUtf8("abc", 128) == "abc" && capUtf8(std::string(200, 'a')).size() == 128);
    {   // the cut never lands inside a character: 127 ASCII bytes then a two-byte character straddling byte 128
        const std::string s = std::string(127, 'a') + "\xC3\xA9" + "tail";
        const std::string c = capUtf8(s);
        CHECK(c == std::string(127, 'a'));
        const std::string s4 = std::string(126, 'a') + "\xF0\x9F\x93\xBB";   // a four-byte character at 126..129
        CHECK(capUtf8(s4) == std::string(126, 'a'));
        const std::string exact = std::string(126, 'a') + "\xC3\xA9";        // ends exactly at 128: kept
        CHECK(capUtf8(exact) == exact);
        CHECK(capUtf8("", 0).empty() && capUtf8("abc", 0).empty());
    }
    CHECK(validClientId("123456789012345678") && validClientId("12345678901234567") && validClientId("12345678901234567890"));
    CHECK(!validClientId("") && !validClientId("1234567890123456") && !validClientId("123456789012345678901") && !validClientId("12345678901234567x") &&
          !validClientId("1234567890123456 8") && !validClientId("-23456789012345678"));
    CHECK(parseReply("{\"cmd\":\"DISPATCH\",\"data\":{},\"evt\":\"READY\",\"nonce\":null}").kind == Reply::Ready);
    CHECK(parseReply("{ \"evt\" :  \"READY\" }").kind == Reply::Ready);
    CHECK(parseReply("{\"evt\":\"ERROR\",\"data\":{\"code\":4000,\"message\":\"x\"}}").kind == Reply::Error);
    CHECK(parseReply("{\"evt\":\"ERROR\",\"data\":{\"code\":4000,\"message\":\"x\"}}").code == 4000);
    CHECK(parseReply("{\"evt\":\"ERROR\",\"data\":{\"code\": 4006}}").code == 4006);
    CHECK(parseReply("{\"evt\":\"ERROR\"}").code == 0);
    CHECK(parseReply("{\"cmd\":\"SET_ACTIVITY\",\"evt\":null}").kind == Reply::Other && parseReply("").kind == Reply::Other && parseReply("garbage").kind == Reply::Other);
    CHECK(parseReply("{\"evt\":\"ERROR\",\"data\":{\"code\":99999999999999999999}}").code >= 0);   // no overflow
    CHECK(formatFrequency(7050000.0) == "7.050 MHz" && formatFrequency(162550000.0) == "162.550 MHz" && formatFrequency(1.0e6) == "1.000 MHz");
    CHECK(formatFrequency(810000.0) == "810 kHz" && formatFrequency(198500.0) == "198.5 kHz" && formatFrequency(500.0) == "500 Hz");
    CHECK(formatFrequency(0.0).empty() && formatFrequency(-5.0).empty() && formatFrequency(std::nan("")).empty() && formatFrequency(INFINITY).empty());
    {   // what the line says
        PresenceInput in;
        in.listening = true;
        in.frequencyHz = 7050000.0;
        in.mode = "USB";
        const auto d = composePresence(in, false);
        CHECK(d.show && d.details == "Listening to the radio" && d.state.empty());   // the default: only that the user is listening
        const auto t = composePresence(in, true);
        CHECK(t.show && t.state == "7.050 MHz USB");
        in.listening = false;
        CHECK(!composePresence(in, true).show && !composePresence(in, false).show);
        in.listening = true;
        in.mode = "USB\"}; C:\\Users\\steve\\secret.wav";
        const auto h = composePresence(in, true);
        CHECK(h.state.find('"') == std::string::npos && h.state.find('\\') == std::string::npos && h.state.find("secret") == std::string::npos);
        in.mode = std::string(500, 'X');
        CHECK(composePresence(in, true).state.size() <= 9 + 1 + 16 + 1);
        in.frequencyHz = 0.0;
        in.mode = "NFM";
        CHECK(composePresence(in, true).state == "NFM");
        in.mode = "";
        CHECK(composePresence(in, true).state.empty());
    }
    {   // FUZZ: the parser and the reply reader on random and mutated frames
        std::mt19937 rng(1234);
        const auto seed = encodeFrame(Op::Frame, "{\"cmd\":\"DISPATCH\",\"evt\":\"ERROR\",\"data\":{\"code\":4000}}");
        for (int i = 0; i < 30000; ++i) {
            std::vector<std::uint8_t> b = seed;
            if (i % 4 == 0) { b.resize(rng() % 90); for (auto& x : b) { x = static_cast<std::uint8_t>(rng()); } }
            else { for (int f = 0, n = 1 + static_cast<int>(rng() % 5); f < n; ++f) { b[rng() % b.size()] = static_cast<std::uint8_t>(rng()); } }
            FrameParser p;
            std::vector<FrameParser::Frame> out;
            std::size_t at = 0;
            bool ok = true;
            while (at < b.size() && ok) {
                const std::size_t take = std::min<std::size_t>(1 + rng() % 25, b.size() - at);
                ok = p.feed(b.data() + at, take, out);
                at += take;
            }
            for (const auto& f : out) { (void)parseReply(f.payload); }
            (void)parseReply(std::string(b.begin(), b.end()));
            (void)capUtf8(std::string(b.begin(), b.end()), rng() % 40);
        }
        CHECK(true);
    }
}

#if defined(_WIN32)

// ---- the stand-in Discord -------------------------------------------------------------------------------------------------------------------
enum class Mode { Normal, Refuse, CloseOnce, Garbage };

class FakeDiscord {
public:
    FakeDiscord(std::string base, int index, std::string expectId, Mode mode) : base_(std::move(base)), index_(index), id_(std::move(expectId)), mode_(mode) {}
    ~FakeDiscord() { stop(); }
    void start() { thread_ = std::thread([this] { loop(); }); }
    void stop() {
        stop_ = true;
        if (thread_.joinable()) {
            for (int i = 0; i < 100 && !done_; ++i) {
                ::CancelSynchronousIo(thread_.native_handle());
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            thread_.join();
        }
    }
    std::vector<std::string> activities() const { std::lock_guard<std::mutex> l(m_); return acts_; }
    int connections() const { return conns_.load(); }
    int handshakesOk() const { return okHandshakes_.load(); }
    int pongs() const { return pongs_.load(); }
    std::string lastHandshake() const { std::lock_guard<std::mutex> l(m_); return lastHandshake_; }

private:
    bool readFrame(HANDLE h, std::uint32_t& op, std::string& payload) {
        std::uint8_t hdr[8];
        if (!readExact(h, hdr, 8)) { return false; }
        std::memcpy(&op, hdr, 4);
        std::uint32_t len = 0;
        std::memcpy(&len, hdr + 4, 4);
        payload.assign(len, '\0');
        return len == 0 || readExact(h, reinterpret_cast<std::uint8_t*>(payload.data()), len);
    }
    bool readExact(HANDLE h, std::uint8_t* p, std::size_t n) {
        std::size_t at = 0;
        while (at < n) {
            if (stop_) { return false; }
            DWORD avail = 0;
            if (!::PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) { return false; }
            if (avail == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(3)); continue; }
            DWORD got = 0;
            if (!::ReadFile(h, p + at, std::min<DWORD>(avail, static_cast<DWORD>(n - at)), &got, nullptr) || got == 0) { return false; }
            at += got;
        }
        return true;
    }
    bool send(HANDLE h, std::uint32_t op, const std::string& payload) {
        std::vector<std::uint8_t> b(8 + payload.size());
        std::memcpy(b.data(), &op, 4);
        const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
        std::memcpy(b.data() + 4, &len, 4);
        std::memcpy(b.data() + 8, payload.data(), payload.size());
        DWORD w = 0;
        return ::WriteFile(h, b.data(), static_cast<DWORD>(b.size()), &w, nullptr) != 0;
    }
    void loop() {
        const std::string name = base_ + std::to_string(index_);
        const std::wstring wname(name.begin(), name.end());
        while (!stop_) {
            HANDLE h = ::CreateNamedPipeW(wname.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
            const BOOL ok = ::ConnectNamedPipe(h, nullptr);
            if (!(ok || ::GetLastError() == ERROR_PIPE_CONNECTED) || stop_) { ::CloseHandle(h); continue; }
            const int n = ++conns_;
            std::uint32_t op = 99;
            std::string payload;
            if (readFrame(h, op, payload) && op == 0) {
                { std::lock_guard<std::mutex> l(m_); lastHandshake_ = payload; }
                const bool idOk = payload.find("\"client_id\":\"" + id_ + "\"") != std::string::npos && payload.find("\"v\":1") != std::string::npos;
                if (idOk) { ++okHandshakes_; }
                if (mode_ == Mode::Refuse) {
                    send(h, 1, "{\"cmd\":\"DISPATCH\",\"evt\":\"ERROR\",\"data\":{\"code\":4000,\"message\":\"Invalid Client ID\"}}");
                } else if (mode_ == Mode::Garbage && n == 1) {
                    send(h, 99, "junk");
                } else {
                    send(h, 1, "{\"cmd\":\"DISPATCH\",\"data\":{\"v\":1,\"config\":{\"cdn_host\":\"cdn.discordapp.com\"},\"user\":{\"id\":\"1\"}},\"evt\":\"READY\",\"nonce\":null}");
                    if (mode_ == Mode::CloseOnce && n == 1) {
                        send(h, 2, "{\"code\":4000,\"message\":\"bye\"}");
                    } else {
                        send(h, 3, "ping-payload");
                        while (!stop_ && readFrame(h, op, payload)) {
                            if (op == 4) { ++pongs_; }
                            else if (op == 1 && payload.find("SET_ACTIVITY") != std::string::npos) {
                                { std::lock_guard<std::mutex> l(m_); acts_.push_back(payload); }
                                const std::size_t at = payload.find("\"nonce\":\"");
                                std::string nonce = at == std::string::npos ? "0" : payload.substr(at + 9, payload.find('"', at + 9) - at - 9);
                                send(h, 1, "{\"cmd\":\"SET_ACTIVITY\",\"data\":{},\"evt\":null,\"nonce\":\"" + nonce + "\"}");
                            }
                        }
                    }
                }
            }
            ::FlushFileBuffers(h);
            ::DisconnectNamedPipe(h);
            ::CloseHandle(h);
        }
        done_ = true;
    }

    std::string base_;
    int index_;
    std::string id_;
    Mode mode_;
    std::thread thread_;
    std::atomic<bool> stop_{false}, done_{false};
    std::atomic<int> conns_{0}, okHandshakes_{0}, pongs_{0};
    mutable std::mutex m_;
    std::vector<std::string> acts_;
    std::string lastHandshake_;
};

const char* kId = "123456789012345678";

std::string pipeBase() { return "\\\\.\\pipe\\foxsdr-test-discord-" + std::to_string(::GetCurrentProcessId()) + "-"; }

template <class F>
bool waitFor(F&& f, int ms = 5000) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end) {
        if (f()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return f();
}

DiscordOptions fastOptions() {
    DiscordOptions o;
    o.clientId = kId;
    o.pipeBase = pipeBase();
    o.maxPipes = 3;
    o.minIntervalMs = 250;
    o.retryMs = 100;
    o.maxRetryMs = 300;
    o.tickMs = 10;
    return o;
}
PresenceInput listening(double hz = 7050000.0, const char* mode = "USB") {
    PresenceInput in;
    in.listening = true;
    in.frequencyHz = hz;
    in.mode = mode;
    return in;
}

void testOff() {
    FakeDiscord srv(pipeBase(), 0, kId, Mode::Normal);
    srv.start();
    {
        DiscordOptions o = fastOptions();
        o.clientId = "";
        DiscordPresence p(o);
        CHECK(p.state() == "off");
        p.update(listening());
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        CHECK(srv.connections() == 0 && p.framesSent() == 0 && p.state() == "off");
    }
    {
        DiscordOptions o = fastOptions();
        o.clientId = "not-a-snowflake";
        DiscordPresence p(o);
        CHECK(p.state() == "bad-id");
        p.update(listening());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(srv.connections() == 0 && p.framesSent() == 0);
    }
}

void testDefaultLine() {
    FakeDiscord srv(pipeBase(), 1, kId, Mode::Normal);   // not at index 0: the client scans for it
    srv.start();
    std::vector<std::string> acts;
    {
        DiscordPresence p(fastOptions());
        p.update(listening());
        CHECK(waitFor([&] { return srv.activities().size() >= 1; }));
        CHECK(p.state() == "connected" && p.connects() == 1);
        CHECK(srv.handshakesOk() == 1);
        CHECK(srv.lastHandshake() == "{\"v\":1,\"client_id\":\"123456789012345678\"}");
        CHECK(waitFor([&] { return srv.pongs() >= 1; }));   // the server's PING was answered
        acts = srv.activities();
        CHECK(!acts.empty());
        if (!acts.empty()) {
            const std::string a = acts[0];
            CHECK(a.find("\"cmd\":\"SET_ACTIVITY\"") != std::string::npos);
            CHECK(a.find("\"pid\":" + std::to_string(::GetCurrentProcessId())) != std::string::npos);
            CHECK(a.find("\"type\":2") != std::string::npos);                     // Listening
            CHECK(a.find("\"details\":\"Listening to the radio\"") != std::string::npos);
            CHECK(a.find("\"state\"") == std::string::npos);                      // nothing more than "listening" by default
            CHECK(a.find("7.050") == std::string::npos && a.find("USB") == std::string::npos);
        }
    }
    // the destructor took the line down
    acts = srv.activities();
    CHECK(waitFor([&] { return srv.activities().size() >= 2; }, 2000));
    acts = srv.activities();
    CHECK(acts.size() >= 2 && acts.back().find("\"activity\"") == std::string::npos);
}

void testTickedLineAndCoalescing() {
    FakeDiscord srv(pipeBase(), 0, kId, Mode::Normal);
    srv.start();
    DiscordOptions o = fastOptions();
    o.showFrequencyMode = true;
    DiscordPresence p(o);
    p.update(listening(7050000.0, "USB"));
    CHECK(waitFor([&] { return srv.activities().size() >= 1; }));
    CHECK(srv.activities()[0].find("\"state\":\"7.050 MHz USB\"") != std::string::npos);
    // an unchanged line is not sent again
    for (int i = 0; i < 10; ++i) { p.update(listening(7050000.0, "USB")); }
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    CHECK(srv.activities().size() == 1);
    // twenty changes in about 100 ms: coalesced to the first interval's worth, and the last one wins
    const auto t0 = Clock::now();
    for (int i = 0; i < 20; ++i) {
        p.update(listening(14000000.0 + i * 1000.0, "USB"));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(waitFor([&] { const auto a = srv.activities(); return !a.empty() && a.back().find("14.019 MHz USB") != std::string::npos; }));
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    const std::size_t sent = srv.activities().size() - 1;
    CHECK(sent >= 1 && sent <= static_cast<std::size_t>(secs / 0.25) + 1);   // never faster than minIntervalMs allows
    CHECK(sent <= 4);
    // not listening: the line is removed
    const std::size_t before = srv.activities().size();
    p.update(PresenceInput{});
    CHECK(waitFor([&] { return srv.activities().size() > before; }));
    CHECK(srv.activities().back().find("\"activity\"") == std::string::npos);
    // listening again, then clear()
    p.update(listening(162550000.0, "NFM"));
    CHECK(waitFor([&] { return srv.activities().back().find("162.550 MHz NFM") != std::string::npos; }));
    const std::size_t n2 = srv.activities().size();
    p.clear();
    CHECK(waitFor([&] { return srv.activities().size() > n2; }));
    CHECK(srv.activities().back().find("\"activity\"") == std::string::npos);
    // clearing again when nothing is shown sends nothing
    const std::size_t n3 = srv.activities().size();
    p.clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    CHECK(srv.activities().size() == n3);
}

void testRefusedAndRetry() {
    FakeDiscord srv(pipeBase(), 0, kId, Mode::Refuse);
    srv.start();
    DiscordPresence p(fastOptions());
    p.update(listening());
    CHECK(waitFor([&] { return p.state() == "refused"; }));
    CHECK(waitFor([&] { return srv.connections() >= 3; }, 6000));   // it keeps looking, with a wait between tries
    CHECK(p.connects() == 0 && p.framesSent() == 0);
    CHECK(srv.activities().empty());
}

void testCloseAndReconnect() {
    FakeDiscord srv(pipeBase(), 0, kId, Mode::CloseOnce);
    srv.start();
    DiscordPresence p(fastOptions());
    p.update(listening());
    CHECK(waitFor([&] { return p.connects() >= 2 && srv.activities().size() >= 1; }, 8000));
    CHECK(srv.connections() >= 2);
    CHECK(srv.activities()[0].find("Listening to the radio") != std::string::npos);   // the second connection says the line again
    CHECK(p.state() == "connected");
}

void testGarbage() {
    FakeDiscord srv(pipeBase(), 0, kId, Mode::Garbage);
    srv.start();
    DiscordPresence p(fastOptions());
    p.update(listening());
    CHECK(waitFor([&] { return p.connects() >= 1 && srv.activities().size() >= 1; }, 8000));   // dropped the junk, tried again, the second connection was fine
    CHECK(srv.connections() >= 2);
}

void testLateDiscord() {
    DiscordPresence p(fastOptions());
    p.update(listening());
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(p.state() == "searching" && p.connects() == 0);
    FakeDiscord srv(pipeBase(), 2, kId, Mode::Normal);
    srv.start();
    CHECK(waitFor([&] { return srv.activities().size() >= 1; }, 8000));
    CHECK(p.state() == "connected");
}

void testStopsPromptly() {
    // destroying the object while it is still looking, and while waiting for a READY that never comes, returns without waiting out the timeouts
    const auto t0 = Clock::now();
    { DiscordPresence p(fastOptions()); p.update(listening()); std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    CHECK(std::chrono::duration<double>(Clock::now() - t0).count() < 2.0);
}

#endif  // _WIN32

}  // namespace

int main() {
    testPure();
#if defined(_WIN32)
    testOff();
    testDefaultLine();
    testTickedLineAndCoalescing();
    testRefusedAndRetry();
    testCloseAndReconnect();
    testGarbage();
    testLateDiscord();
    testStopsPromptly();
#endif
    return testSummary("test_discord_presence");
}
