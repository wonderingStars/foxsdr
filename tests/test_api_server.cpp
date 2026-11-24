// Tests for net/api_server.hpp and net/control_hub.hpp (screen lane S4b, G3-31: the WebSocket and SSE API). Every test talks to the server over a REAL loopback socket from a stand-in client
// (plain sockets, a hand-made masked WebSocket frame writer); nothing here reaches an outside host.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/api_server.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "net/ws_frame.hpp"
#include "net_test_support.hpp"
#include "test_check.hpp"

using namespace cascade::net;
using nettest::Client;
using nettest::msSince;
using nlohmann::json;

namespace {

ApiConfig cfg0() {
    ApiConfig c;
    c.port = 0;
    c.statusIntervalMs = 100;
    return c;
}

bool waitFor(Client& c, const std::string& marker, int ms = 3000) {
    const auto t0 = nettest::Clock::now();
    while (msSince(t0) < ms) {
        if (std::string(c.got.begin(), c.got.end()).find(marker) != std::string::npos) { return true; }
        if (c.eof) { return false; }
        c.pump(30);
    }
    return std::string(c.got.begin(), c.got.end()).find(marker) != std::string::npos;
}

std::string httpGet(std::uint16_t port, const std::string& request, std::string* head = nullptr) {
    Client c;
    if (!c.connect(port)) { return "<no connection>"; }
    c.sendBytes(request.data(), request.size());
    c.waitEof(3000);
    const std::string all(c.got.begin(), c.got.end());
    if (head != nullptr) { *head = all.substr(0, all.find("\r\n\r\n")); }
    const std::size_t b = all.find("\r\n\r\n");
    return b == std::string::npos ? all : all.substr(b + 4);
}

int statusOf(const std::string& response) {
    return response.size() > 12 ? std::atoi(response.substr(9, 3).c_str()) : 0;
}

std::string wsFrame(std::uint8_t opcode, const std::string& payload, bool mask = true) {
    std::string f;
    f.push_back(static_cast<char>(0x80 | opcode));
    const std::uint8_t mbit = mask ? 0x80 : 0;
    if (payload.size() < 126) { f.push_back(static_cast<char>(mbit | payload.size())); }
    else if (payload.size() < 65536) { f.push_back(static_cast<char>(mbit | 126)); f.push_back(static_cast<char>(payload.size() >> 8)); f.push_back(static_cast<char>(payload.size())); }
    else { f.push_back(static_cast<char>(mbit | 127)); for (int i = 7; i >= 0; --i) { f.push_back(static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> (8 * i)) & 0xFF)); } }
    const std::uint8_t key[4] = {0x37, 0xfa, 0x21, 0x3d};
    if (mask) { f.append(reinterpret_cast<const char*>(key), 4); }
    for (std::size_t i = 0; i < payload.size(); ++i) { f.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ (mask ? key[i % 4] : 0))); }
    return f;
}

const char* kWsRequest = "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

// Opens a WebSocket; true when the server said 101 (and has sent the first frame).
bool wsOpen(Client& c, std::uint16_t port, const std::string& request = kWsRequest) {
    if (!c.connect(port)) { return false; }
    c.sendBytes(request.data(), request.size());
    return waitFor(c, "\r\n\r\n") && std::string(c.got.begin(), c.got.end()).rfind("HTTP/1.1 101", 0) == 0;
}

// Reads the next whole server frame (unmasked) from c.got starting at `pos`; moves `pos`. Returns false when none is complete.
bool nextFrame(Client& c, std::size_t& pos, std::uint8_t& opcode, std::string& payload) {
    if (pos == 0) {
        const std::string all(c.got.begin(), c.got.end());
        const std::size_t b = all.find("\r\n\r\n");
        if (b == std::string::npos) { return false; }
        pos = b + 4;
    }
    if (c.got.size() < pos + 2) { return false; }
    opcode = c.got[pos] & 0x0F;
    std::size_t len = c.got[pos + 1] & 0x7F, hdr = 2;
    if (len == 126) { if (c.got.size() < pos + 4) { return false; } len = (static_cast<std::size_t>(c.got[pos + 2]) << 8) | c.got[pos + 3]; hdr = 4; }
    else if (len == 127) { return false; }
    if (c.got.size() < pos + hdr + len) { return false; }
    payload.assign(c.got.begin() + static_cast<std::ptrdiff_t>(pos + hdr), c.got.begin() + static_cast<std::ptrdiff_t>(pos + hdr + len));
    pos += hdr + len;
    return true;
}

// waits for a text frame whose JSON has "type" == type (skipping others); returns it
json waitType(Client& c, std::size_t& pos, const std::string& type, int ms = 3000) {
    const auto t0 = nettest::Clock::now();
    while (msSince(t0) < ms) {
        std::uint8_t op = 0;
        std::string p;
        while (nextFrame(c, pos, op, p)) {
            if (op == 1) {
                const json j = json::parse(p, nullptr, false);
                if (j.is_object() && j.value("type", "") == type) { return j; }
            }
        }
        if (c.eof) { break; }
        c.pump(30);
    }
    return json();
}

void testHub() {
    ControlHub hub;
    CHECK(ControlHub::canonicalMode("usb") == "USB" && ControlHub::canonicalMode("Wfm") == "WFM" && ControlHub::canonicalMode("") == "" && ControlHub::canonicalMode("FM").empty() && ControlHub::canonicalMode("USBX").empty());
    CHECK(hub.requestTune(100e6).ok && hub.takeTune() == 100e6 && !hub.takeTune());
    CHECK(!hub.requestTune(0).ok && !hub.requestTune(999).ok && !hub.requestTune(7.3e9).ok && !hub.requestTune(-5).ok && !hub.requestTune(std::nan("")).ok && !hub.requestTune(INFINITY).ok);
    CHECK(hub.requestTune(1000.0).ok && hub.requestTune(7.2e9).ok);
    // only the latest request is kept
    hub.takeTune();
    CHECK(hub.requestTune(101e6).ok && hub.requestTune(102e6).ok && hub.takeTune() == 102e6 && !hub.takeTune());
    CHECK(hub.requestMode("lsb").ok && hub.takeMode() == "LSB" && !hub.takeMode());
    CHECK(!hub.requestMode("SSB").ok && !hub.requestMode(std::string(500, 'U')).ok && !hub.requestMode("").ok);
    // the rate limit: a burst of 20, then ten a second
    {
        ControlHub h2;
        int ok = 0;
        for (int i = 0; i < 100; ++i) { if (h2.requestTune(100e6 + i).ok) { ++ok; } }
        CHECK(ok >= 20 && ok <= 25 && h2.counts().rateLimited >= 70);
    }
    // decodes: cleaned, bounded, numbered
    for (int i = 0; i < 250; ++i) { hub.addDecode(1.0 * i, 100e6, "dec", "line " + std::to_string(i)); }
    CHECK(hub.recentDecodes(1000).size() == kHubMaxDecodes && hub.recentDecodes(3).size() == 3 && hub.recentDecodes(3).back().text == "line 249");
    std::uint64_t cur = hub.newestSeq() - 2;
    CHECK(hub.decodesSince(cur, 10).size() == 2 && cur == hub.newestSeq() && hub.decodesSince(cur, 10).empty());
    hub.addDecode(0, 0, "d\x01x", std::string("a\x01") + "b\xC3\x28" + std::string(2000, 'z'));
    const HubDecode last = hub.recentDecodes(1).back();
    CHECK(last.text.size() <= kHubTextBytes && last.text.find('\x01') == std::string::npos && last.text.find('?') != std::string::npos && last.decoder.find('\x01') == std::string::npos);
    std::vector<HubBookmark> many(2000);
    hub.publishBookmarks(many);
    CHECK(hub.bookmarks().size() == kHubMaxBookmarks);
}

void testStartRefusals() {
    ControlHub hub;
    std::string err;
    {
        ApiConfig c = cfg0();
        c.bindAddress = "0.0.0.0";
        ApiServer s(c, hub, nullptr);
        CHECK(!s.start(err) && err.find("every network interface") != std::string::npos && !s.running());
    }
    {
        ApiConfig c = cfg0();
        c.maxConnections = 0;
        ApiServer s(c, hub, nullptr);
        CHECK(!s.start(err) && !s.running());
    }
    {
        ApiServer s(cfg0(), hub, nullptr);
        CHECK(s.start(err) && s.running() && s.port() > 0 && !s.start(err));
        s.stop();
        CHECK(!s.running());
    }
}

void testHttp() {
    ControlHub hub;
    HubStatus st;
    st.running = true;
    st.tunedHz = 145.5e6;
    st.centreHz = 145.4e6;
    st.mode = "NFM";
    st.source = "Test";
    hub.publishStatus(st);
    std::string err;
    std::atomic<int> authCalls{0};
    ApiConfig c = cfg0();
    c.headMs = 600;
    ApiServer s(c, hub, [&](const std::string& a, const std::string&) { ++authCalls; return a == "Basic Zm94OnNkcg==" ? ApiAuth::Allowed : (a == "Basic dG9v" ? ApiAuth::TooMany : ApiAuth::Denied); });
    CHECK(s.start(err));
    const std::uint16_t port = static_cast<std::uint16_t>(s.port());
    std::string head;
    // no credentials: 401 with the challenge; the right ones: the status
    std::string r = httpGet(port, "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", &head);
    CHECK(head.rfind("HTTP/1.1 401", 0) == 0 && head.find("WWW-Authenticate: Basic") != std::string::npos);
    r = httpGet(port, "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Basic Zm94OnNkcg==\r\n\r\n", &head);
    CHECK(statusOf(head) == 200 && head.find("application/json") != std::string::npos);
    const json j = json::parse(r, nullptr, false);
    CHECK(j.is_object() && j.value("tuned_hz", 0.0) == 145.5e6 && j.value("mode", "") == "NFM" && j.value("running", false));
    // too many attempts: 429
    httpGet(port, "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Basic dG9v\r\n\r\n", &head);
    CHECK(statusOf(head) == 429 && head.find("Retry-After") != std::string::npos);
    const std::string ok = "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Basic Zm94OnNkcg==\r\n";
    // a browser (an Origin header) is refused, whatever the credentials
    httpGet(port, ok + "Origin: http://evil.example\r\n\r\n", &head);
    CHECK(statusOf(head) == 403);
    // method, path, version, header shapes
    httpGet(port, "POST /status HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n", &head);
    CHECK(statusOf(head) == 405 && head.find("Allow: GET") != std::string::npos);
    httpGet(port, ok + "\r\n", &head);
    httpGet(port, "GET /other HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", &head);
    CHECK(statusOf(head) == 404);
    httpGet(port, "GET /status?x=1 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", &head);
    CHECK(statusOf(head) == 404);
    httpGet(port, "GET /status HTTP/2.0\r\nHost: 127.0.0.1\r\n\r\n", &head);
    CHECK(statusOf(head) == 505);
    httpGet(port, "garbage\r\n\r\n", &head);
    CHECK(statusOf(head) == 400);
    httpGet(port, "GET /status HTTP/1.1\r\nnocolon\r\n\r\n", &head);
    CHECK(statusOf(head) == 400);
    {
        std::string many = ok;
        for (int i = 0; i < 100; ++i) { many += "X-" + std::to_string(i) + ": a\r\n"; }
        httpGet(port, many + "\r\n", &head);
        CHECK(statusOf(head) == 431);
    }
    // a head that never ends: refused when it passes 8 KiB
    httpGet(port, "GET /status HTTP/1.1\r\nX-A: " + std::string(9000, 'a'), &head);
    CHECK(statusOf(head) == 431);
    // a head that is never finished is closed (slowloris)
    {
        Client slow;
        CHECK(slow.connect(port));
        slow.sendBytes("GET /sta", 8);
        CHECK(slow.waitEof(3000));
        CHECK(s.stats().droppedBad >= 1);
    }
    CHECK(s.running() && authCalls.load() > 0);
    s.stop();
}

void testRequireAuthNeverAsksTheCaller() {
    ControlHub hub;
    std::string err;
    std::atomic<int> calls{0};
    ApiConfig c = cfg0();
    c.requireAuth = true;
    ApiServer s(c, hub, [&](const std::string&, const std::string&) { ++calls; return ApiAuth::Allowed; });
    CHECK(s.start(err));
    std::string head;
    httpGet(static_cast<std::uint16_t>(s.port()), "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", &head);
    CHECK(statusOf(head) == 401 && calls.load() == 0);   // no credentials: refused before the caller is asked
    httpGet(static_cast<std::uint16_t>(s.port()), "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Basic QQ==\r\n\r\n", &head);
    CHECK(statusOf(head) == 200 && calls.load() == 1);
    s.stop();
}

void testWebSocket() {
    ControlHub hub;
    HubStatus st;
    st.running = true;
    st.tunedHz = 100.1e6;
    st.mode = "WFM";
    hub.publishStatus(st);
    std::vector<HubBookmark> bm = {{"BBC R4", "WFM", "radio", 93.5e6}};
    hub.publishBookmarks(bm);
    std::string err;
    ApiServer s(cfg0(), hub, nullptr);
    CHECK(s.start(err));
    const std::uint16_t port = static_cast<std::uint16_t>(s.port());
    {
        Client c;
        CHECK(wsOpen(c, port));
        const std::string all(c.got.begin(), c.got.end());
        CHECK(all.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);   // RFC 6455's worked example
        std::size_t pos = 0;
        json first = waitType(c, pos, "status");
        CHECK(first.is_object() && first.value("mode", "") == "WFM");
        // reads, with the id echoed
        c.sendBytes(wsFrame(1, R"({"op":"get_status","id":7})").data(), wsFrame(1, R"({"op":"get_status","id":7})").size());
        json r = waitType(c, pos, "status");
        // (a pushed status may come first: ask again until the one with the id)
        for (int i = 0; i < 20 && !(r.is_object() && r.contains("id")); ++i) { r = waitType(c, pos, "status"); }
        CHECK(r.is_object() && r.value("id", 0) == 7);
        const std::string lb = wsFrame(1, R"({"op":"list_bookmarks"})");
        c.sendBytes(lb.data(), lb.size());
        json b = waitType(c, pos, "bookmarks");
        CHECK(b.is_object() && b["items"].size() == 1 && b["items"][0].value("name", "") == "BBC R4" && b["items"][0].value("freq_hz", 0.0) == 93.5e6);
        // tune and mode: validated, queued for the engine
        const std::string t1 = wsFrame(1, R"({"op":"tune","hz":145500000,"id":"a"})");
        c.sendBytes(t1.data(), t1.size());
        json ok = waitType(c, pos, "ok");
        CHECK(ok.is_object() && ok.value("op", "") == "tune" && ok.value("id", "") == "a" && hub.takeTune() == 145.5e6);
        const std::string m1 = wsFrame(1, R"({"op":"set_mode","mode":"usb"})");
        c.sendBytes(m1.data(), m1.size());
        ok = waitType(c, pos, "ok");
        CHECK(ok.is_object() && hub.takeMode() == "USB");
        // refusals, each answered and nothing queued
        for (const char* bad : {R"({"op":"tune","hz":5})", R"({"op":"tune","hz":"145"})", R"({"op":"tune"})", R"({"op":"set_mode","mode":"AM2"})", R"({"op":"set_mode"})", R"({"op":"recent_decodes","max":5000})",
                                R"({"op":"transmit"})", R"({"op":"write_file","path":"x"})", R"({"op":"run","cmd":"calc"})", R"({"op":"settings"})", R"({"op":5})", R"([1,2])", "not json", R"({"op":"subscribe"})"}) {
            const std::string f = wsFrame(1, bad);
            c.sendBytes(f.data(), f.size());
            const json e = waitType(c, pos, "error");
            CHECK(e.is_object());
            if (!e.is_object()) { std::printf("  no error answer for %s\n", bad); }
        }
        CHECK(!hub.takeTune() && !hub.takeMode());
        // a deeply nested message is refused before it is parsed
        {
            std::string deep(200, '[');
            deep += std::string(200, ']');
            const std::string f = wsFrame(1, deep);
            c.sendBytes(f.data(), f.size());
            CHECK(waitType(c, pos, "error").is_object());
        }
        // subscribe to decodes: lines posted afterwards arrive, in order
        const std::string sub = wsFrame(1, R"({"op":"subscribe","decodes":true})");
        c.sendBytes(sub.data(), sub.size());
        CHECK(waitType(c, pos, "ok").is_object());
        hub.addDecode(10.0, 148e6, "pocsag", "first line");
        hub.addDecode(11.0, 148e6, "pocsag", "second line");
        const json d1 = waitType(c, pos, "decode"), d2 = waitType(c, pos, "decode");
        CHECK(d1.value("text", "") == "first line" && d2.value("text", "") == "second line" && d2.value("seq", 0) == d1.value("seq", 0) + 1);
        // a ping is answered with a pong
        const std::string ping = wsFrame(9, "hi");
        c.sendBytes(ping.data(), ping.size());
        bool pong = false;
        for (int i = 0; i < 100 && !pong; ++i) {
            std::uint8_t op = 0;
            std::string p;
            while (nextFrame(c, pos, op, p)) { if (op == 10 && p == "hi") { pong = true; } }
            if (!pong) { c.pump(30); }
        }
        CHECK(pong);
    }
    // recent decodes, bounded
    {
        Client c;
        CHECK(wsOpen(c, port));
        std::size_t pos = 0;
        for (int i = 0; i < 300; ++i) { hub.addDecode(1.0, 1.0, "d", "n" + std::to_string(i)); }
        const std::string f = wsFrame(1, R"({"op":"recent_decodes","max":200})");
        c.sendBytes(f.data(), f.size());
        const json r = waitType(c, pos, "decodes", 4000);
        CHECK(r.is_object() && r["items"].size() == 200);
    }
    CHECK(s.running());
    s.stop();
}

void testWebSocketMalformed() {
    ControlHub hub;
    std::string err;
    ApiServer s(cfg0(), hub, nullptr);
    CHECK(s.start(err));
    const std::uint16_t port = static_cast<std::uint16_t>(s.port());
    const auto closedWith = [&](const std::string& frames, int code) {
        Client c;
        if (!wsOpen(c, port)) { return false; }
        c.sendBytes(frames.data(), frames.size());
        std::size_t pos = 0;
        const auto t0 = nettest::Clock::now();
        while (msSince(t0) < 3000) {
            std::uint8_t op = 0;
            std::string p;
            while (nextFrame(c, pos, op, p)) {
                if (op == 8 && p.size() >= 2) { return ((static_cast<std::uint8_t>(p[0]) << 8) | static_cast<std::uint8_t>(p[1])) == code; }
            }
            if (c.eof) { break; }
            c.pump(30);
        }
        // what the client really received, so a failure says "reset, no close frame" or "closed with another code" and not just false
        std::printf("  closedWith(%zu bytes sent, want %d): received %zu bytes, eof=%d, ended on a receive error (reset)=%d\n", frames.size(), code, c.got.size(), c.eof ? 1 : 0, c.recvFailed ? 1 : 0);
        return false;
    };
    CHECK(closedWith(wsFrame(2, "binary"), 1003));                       // binary is not used
    CHECK(closedWith(wsFrame(1, std::string(5000, 'x')), 1009));         // over the 4096-byte message limit
    CHECK(closedWith(wsFrame(1, std::string("\xC3\x28 bad", 6)), 1007)); // text that is not UTF-8
    CHECK(closedWith(wsFrame(1, R"({"op":"get_status"})", false), 1002)); // an unmasked client frame
    CHECK(closedWith(std::string("\x03\x80\x00\x00\x00\x00", 6), 1002)); // a reserved opcode
    // the same refusal when only the header of the oversized frame is sent: nothing is left unread, so this one never depended on the close surviving a reset (round 10's control)
    CHECK(closedWith(std::string("\x81\xFE\x13\x88\x00\x00\x00\x00", 8), 1009));
    {
        // raw garbage after a good handshake: closed, not echoed, nothing else happens
        std::string junk(2000, '\0');
        for (std::size_t i = 0; i < junk.size(); ++i) { junk[i] = static_cast<char>(i * 131u + 7u); }
        Client c;
        CHECK(wsOpen(c, port));
        c.sendBytes(junk.data(), junk.size());
        CHECK(c.waitEof(3000));
    }
    // a handshake with the wrong version or no key is refused with a status, not upgraded
    {
        std::string head;
        httpGet(port, "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 8\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n", &head);
        CHECK(statusOf(head) >= 400 && statusOf(head) < 500);
        httpGet(port, "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", &head);
        CHECK(statusOf(head) >= 400 && statusOf(head) < 500);
    }
    CHECK(hub.takeTune() == std::nullopt && s.running());
    s.stop();
}

// After a close frame the server reads and discards what the client still sends (so the close frame is not lost to a reset), but only for a bounded time: a client that
// never closes, or keeps sending, must not hold the connection past it, and other connections are served meanwhile.
void testCloseLingerIsBounded() {
    ControlHub hub;
    std::string err;
    ApiServer s(cfg0(), hub, nullptr);
    CHECK(s.start(err));
    const std::uint16_t port = static_cast<std::uint16_t>(s.port());
    // (a) a client that provokes a 1009 and then neither reads nor closes
    {
        Client c;
        CHECK(wsOpen(c, port));
        const std::string big = wsFrame(1, std::string(5000, 'x'));
        c.sendBytes(big.data(), big.size());
        const auto t0 = nettest::Clock::now();
        // while it lingers, another client is served at once
        Client other;
        CHECK(wsOpen(other, port));
        CHECK(msSince(t0) < 400);
        other.close();
        while (s.stats().connections > 0 && msSince(t0) < 5000) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        CHECK(s.stats().connections == 0);
        CHECK(msSince(t0) < 3000);   // the bound is 500 ms; the rest is poll granularity and a busy machine
    }
    // (b) a client that keeps sending through the wait is let go all the same
    {
        Client c;
        CHECK(wsOpen(c, port));
        const std::string big = wsFrame(1, std::string(5000, 'x'));
        c.sendBytes(big.data(), big.size());
        const std::string chunk(4096, 'y');
        const auto t0 = nettest::Clock::now();
        while (s.stats().connections > 0 && msSince(t0) < 5000) {
            c.sendBytes(chunk.data(), chunk.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(s.stats().connections == 0);
        CHECK(msSince(t0) < 3000);
    }
    CHECK(s.running());
    s.stop();
}

void testSse() {
    ControlHub hub;
    HubStatus st;
    st.mode = "AM";
    hub.publishStatus(st);
    std::string err;
    ApiServer s(cfg0(), hub, nullptr);
    CHECK(s.start(err));
    const std::uint16_t port = static_cast<std::uint16_t>(s.port());
    Client c;
    CHECK(c.connect(port));
    const std::string req = "GET /events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    c.sendBytes(req.data(), req.size());
    CHECK(waitFor(c, "text/event-stream") && waitFor(c, "event: status\ndata: {"));
    hub.addDecode(5.0, 7.0e6, "navtex", "a line");
    CHECK(waitFor(c, "event: decode\ndata: {"));
    CHECK(waitFor(c, "\"text\":\"a line\""));
    // an SSE client cannot command anything: what it sends is thrown away
    const std::string cmd = R"({"op":"tune","hz":99000000})";
    c.sendBytes(cmd.data(), cmd.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!hub.takeTune());
    CHECK(s.stats().sseOpened == 1);
    // a stream whose client never reads is dropped, and the server carries on
    {
        ApiConfig c2 = cfg0();
        c2.clientQueueBytes = 8192;
        c2.stallMs = 500;
        c2.statusIntervalMs = 50;
        ApiServer t(c2, hub, nullptr);
        CHECK(t.start(err));
        Client idle;
        CHECK(idle.connect(static_cast<std::uint16_t>(t.port())));
        cascade::net::sock::setBuffers(idle.s, 4096, 0);
        const std::string r2 = "GET /events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        idle.sendBytes(r2.data(), r2.size());
        const auto t0 = nettest::Clock::now();
        while (msSince(t0) < 20000 && t.stats().droppedSlow + t.stats().droppedStalled < 1) {
            for (int k = 0; k < 40; ++k) { hub.addDecode(1.0, 1.0, "d", std::string(400, 'q')); }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(t.stats().droppedSlow + t.stats().droppedStalled >= 1 && t.running());
    }
    s.stop();
}

void testManyConnections() {
    ControlHub hub;
    std::string err;
    ApiConfig c = cfg0();
    c.maxConnections = 8;
    ApiServer s(c, hub, nullptr);
    CHECK(s.start(err));
    const std::uint16_t port = static_cast<std::uint16_t>(s.port());
    int connected = 0;
    {
        std::vector<std::unique_ptr<Client>> held;
        for (int i = 0; i < 1000; ++i) {
            auto cl = std::make_unique<Client>();
            if (cl->connect(port, 500)) { ++connected; }
            if (held.size() < 30) { held.push_back(std::move(cl)); }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(s.stats().connections <= 8 && s.stats().refusedFull > 0);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::string head;
    const std::string r = httpGet(port, "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", &head);
    CHECK(statusOf(head) == 200 && connected > 0);
    const auto t0 = nettest::Clock::now();
    s.stop();
    CHECK(msSince(t0) < 2000);
}

}  // namespace

int main() {
    testHub();
    testStartRefusals();
    testHttp();
    testRequireAuthNeverAsksTheCaller();
    testWebSocket();
    testWebSocketMalformed();
    testCloseLingerIsBounded();
    testSse();
    testManyConnections();
    return testSummary("test_api_server");
}
