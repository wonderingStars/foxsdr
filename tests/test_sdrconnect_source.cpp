// test_sdrconnect_source.cpp - the SDRconnect WebSocket client, proven against a STAND-IN server on
// a real loopback socket, and the WebSocket layer against RFC 6455's own worked example.
//
// WHAT THIS CAN AND CANNOT PROVE. The stand-in speaks the API as SDRplay's document describes it
// (src/source/sdrconnect_protocol.hpp lists what that document says and does not say) and plain
// RFC 6455. Its frame writer and reader are its own (literals, not the client's). The handshake
// accept value is computed with the client's SHA-1 and Base64, which are proven here against the
// RFC's worked example and FIPS test vectors first. UNPROVEN: a real SDRconnect (its port, its
// WebSocket path, the IQ stream's rate and scaling, its reply timing).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "source/sdrconnect_protocol.hpp"
#include "source/sdrconnect_source.hpp"
#include "source/websocket_client.hpp"
#include "src_test_net.hpp"
#include "test_check.hpp"

using namespace srctest;
using cascade::source::SdrConnectSource;
using cascade::source::NetSampleSource;
namespace ws = cascade::source::ws;
namespace sc = cascade::source::sdrconnect;
using nlohmann::json;

namespace {

std::string hexOf(const std::uint8_t* d, std::size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) { s += h[d[i] >> 4]; s += h[d[i] & 15]; }
    return s;
}

// --- the stand-in server ---------------------------------------------------------------

enum class Hs { Normal, Not101, WrongAccept, NotHttp, CloseNow, Silent, Extensions, EarlyPush, NoApiVersion };
enum class Sf { None, CloseFrame, Abrupt, HugeLength, MaskedFrame, ReservedOpcode, Stall, FragmentOverrun };

// Server frames: unmasked, FIN set.
Bytes frame(std::uint8_t op, const Bytes& payload, bool fin = true) {
    Bytes f;
    f.push_back(static_cast<std::uint8_t>((fin ? 0x80 : 0) | op));
    if (payload.size() < 126) {
        f.push_back(static_cast<std::uint8_t>(payload.size()));
    } else if (payload.size() <= 65535) {
        f.push_back(126);
        f.push_back(static_cast<std::uint8_t>(payload.size() >> 8));
        f.push_back(static_cast<std::uint8_t>(payload.size()));
    } else {
        f.push_back(127);
        for (int s = 56; s >= 0; s -= 8) { f.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(payload.size()) >> s)); }
    }
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}
Bytes textFrame(const std::string& s) { return frame(1, Bytes(s.begin(), s.end())); }

class FakeSdrConnect {
public:
    ~FakeSdrConnect() { stop(); }
    bool start() {
        props_ = {{"api_version", "1.0.3"},          {"device_sample_rate", "2000000"},
                  {"device_center_frequency", "100000000"}, {"lna_state_min", "0"},
                  {"lna_state_max", "9"},            {"lna_state", "4"},
                  {"valid_antennas", "Antenna A, Antenna B,Antenna C"}, {"active_antenna", "Antenna A"},
                  {"valid_devices", "RSP1A 1234,RSPdx 5678"}, {"active_device", "RSP1A 1234"},
                  {"started", "false"}};
        return srv_.start([this](Sock s, std::size_t idx, const std::atomic<bool>& run) { serve(s, idx, run); });
    }
    void stop() {
        srv_.stop();  // the connection threads first: they start and stop the stream
        streamRun_.store(false);
        std::lock_guard<std::mutex> l(streamM_);
        if (streamThread_.joinable()) { streamThread_.join(); }
    }
    std::string args(const std::string& extra = "") const {
        return "sdrconnect=127.0.0.1:" + std::to_string(srv_.port()) + (extra.empty() ? "" : "," + extra);
    }

    std::atomic<Hs> hs{Hs::Normal};
    std::atomic<Sf> sf{Sf::None};
    std::atomic<int> faultAfter{20};

    std::vector<std::string> events() { std::lock_guard<std::mutex> l(m_); return events_; }
    int count(const std::string& ev, const std::string& val = "") {
        int n = 0;
        for (const std::string& e : events()) { n += (e == ev + "|" + val) || (val.empty() && e.rfind(ev + "|", 0) == 0); }
        return n;
    }
    std::string prop(const std::string& k) { std::lock_guard<std::mutex> l(m_); return props_[k]; }
    int pongs() const { return pongs_.load(); }
    int closes() const { return closes_.load(); }
    int unmaskedClientFrames() const { return unmasked_.load(); }
    std::string requestLine() { std::lock_guard<std::mutex> l(m_); return requestLine_; }
    std::string requestHeaders() { std::lock_guard<std::mutex> l(m_); return headers_; }

private:
    bool sendB(Sock s, const Bytes& b) { std::lock_guard<std::mutex> l(sendM_); return sendExact(s, b.data(), b.size()); }

    // One CLIENT frame (masked): own parser.
    bool readFrame(Sock s, const std::atomic<bool>& run, std::uint8_t& op, Bytes& payload) {
        std::uint8_t h[2];
        if (!recvExact(s, run, h, 2)) { return false; }
        op = h[0] & 0x0F;
        std::uint64_t len = h[1] & 0x7F;
        if (!(h[1] & 0x80)) { unmasked_.fetch_add(1); }
        if (len == 126) { std::uint8_t e[2]; if (!recvExact(s, run, e, 2)) { return false; } len = (e[0] << 8) | e[1]; }
        else if (len == 127) { std::uint8_t e[8]; if (!recvExact(s, run, e, 8)) { return false; } len = 0; for (int i = 0; i < 8; ++i) { len = (len << 8) | e[i]; } }
        std::uint8_t mask[4] = {0, 0, 0, 0};
        if (h[1] & 0x80) { if (!recvExact(s, run, mask, 4)) { return false; } }
        if (len > (1u << 20)) { return false; }
        payload.resize(static_cast<std::size_t>(len));
        if (len > 0 && !recvExact(s, run, payload.data(), payload.size())) { return false; }
        for (std::size_t i = 0; i < payload.size(); ++i) { payload[i] ^= mask[i % 4]; }
        return true;
    }

    void serve(Sock s, std::size_t, const std::atomic<bool>& run) {
        const Hs mode = hs.load();
        if (mode == Hs::CloseNow) { return; }
        if (mode == Hs::Silent) { while (run.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); } return; }
        // the HTTP request
        std::string req;
        while (req.find("\r\n\r\n") == std::string::npos) {
            if (!waitRead(s, run)) { return; }
            char c[256];
            const int r = ::recv(s, c, sizeof(c), 0);
            if (r <= 0) { return; }
            req.append(c, static_cast<std::size_t>(r));
            if (req.size() > 8192) { return; }
        }
        {
            std::lock_guard<std::mutex> l(m_);
            requestLine_ = req.substr(0, req.find("\r\n"));
            headers_ = req;
        }
        std::string key;
        const std::size_t k = req.find("Sec-WebSocket-Key: ");
        if (k != std::string::npos) { key = req.substr(k + 19, req.find("\r\n", k) - k - 19); }
        if (mode == Hs::NotHttp) { const std::string j = "\x01\x02garbage\xff"; sendExact(s, j.data(), j.size()); return; }
        if (mode == Hs::Not101) {
            const std::string r = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
            sendExact(s, r.data(), r.size());
            return;
        }
        const std::string guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        const std::string src = key + guid;
        const auto d = ws::sha1(reinterpret_cast<const std::uint8_t*>(src.data()), src.size());
        std::string accept = ws::base64(d.data(), d.size());
        if (mode == Hs::WrongAccept) { accept[0] = accept[0] == 'A' ? 'B' : 'A'; }
        std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " + accept + "\r\n";
        if (mode == Hs::Extensions) { resp += "Sec-WebSocket-Extensions: permessage-deflate\r\n"; }
        resp += "\r\n";
        Bytes out(resp.begin(), resp.end());
        if (mode == Hs::EarlyPush) {
            // a push in the same write as the headers: the client must not lose it
            const Bytes e = textFrame(R"({"event_type":"property_changed","property":"started","device":"primary","value":"false"})");
            out.insert(out.end(), e.begin(), e.end());
        }
        if (!sendB(s, out)) { return; }
        for (;;) {
            std::uint8_t op = 0;
            Bytes p;
            if (!readFrame(s, run, op, p)) { return; }
            if (op == 8) { closes_.fetch_add(1); sendB(s, frame(8, p)); return; }
            if (op == 9) { sendB(s, frame(10, p)); continue; }
            if (op == 10) { pongs_.fetch_add(1); continue; }
            if (op != 1) { continue; }
            const json j = json::parse(std::string(p.begin(), p.end()), nullptr, false);
            if (j.is_discarded()) { continue; }
            const std::string ev = j.value("event_type", ""), pr = j.value("property", ""), va = j.value("value", "");
            { std::lock_guard<std::mutex> l(m_); events_.push_back(ev + "|" + (pr.empty() ? va : pr + "=" + va)); }
            if (ev == "get_property") {
                if (mode == Hs::NoApiVersion && pr == "api_version") { continue; }
                std::string v;
                { std::lock_guard<std::mutex> l(m_); v = props_[pr]; }
                json r = {{"event_type", "get_property_response"}, {"property", pr}, {"device", "primary"}, {"value", v}};
                sendB(s, textFrame(r.dump()));
            } else if (ev == "set_property") {
                { std::lock_guard<std::mutex> l(m_); props_[pr] = va; }
                json r = {{"event_type", "property_changed"}, {"property", pr}, {"device", "primary"}, {"value", va}};
                sendB(s, textFrame(r.dump()));
            } else if (ev == "iq_stream_enable") {
                if (va == "true") { startStream(s); } else { stopStream(); }
            }
        }
    }

    void startStream(Sock s) {
        stopStream();
        std::lock_guard<std::mutex> l(streamM_);
        streamRun_.store(true);
        streamThread_ = std::thread([this, s] { streamLoop(s); });
    }
    void stopStream() {
        streamRun_.store(false);
        std::lock_guard<std::mutex> l(streamM_);
        if (streamThread_.joinable()) { streamThread_.join(); }
    }

    void streamLoop(Sock s) {
        std::int64_t n = 0;
        int sent = 0;
        while (streamRun_.load()) {
            const Sf f = sf.load();
            if (f == Sf::Stall && sent >= faultAfter.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
            if (sent == faultAfter.load()) {
                if (f == Sf::CloseFrame) { sendB(s, frame(8, Bytes{0x03, 0xE8})); return; }
                if (f == Sf::Abrupt) { ::shutdown(s, 2); return; }
                if (f == Sf::HugeLength) { Bytes b = {0x82, 127, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; sendB(s, b); return; }
                if (f == Sf::MaskedFrame) { Bytes b = {0x82, 0x84, 1, 2, 3, 4, 0, 0, 0, 0}; sendB(s, b); return; }
                if (f == Sf::ReservedOpcode) { Bytes b = {0x83, 0x00}; sendB(s, b); return; }
                if (f == Sf::FragmentOverrun) { Bytes b = {0x02, 0x00}; sendB(s, b); sendB(s, frame(2, Bytes{1, 0}, true)); return; }  // data frame inside a fragmented message
            }
            Bytes pl = {2, 0};  // type 2 little endian: IQ, Primary
            for (int i = 0; i < 500; ++i, ++n) {
                const std::int16_t v = static_cast<std::int16_t>(n % 20000);
                const std::int16_t q = static_cast<std::int16_t>(-v);
                pl.push_back(static_cast<std::uint8_t>(v & 0xFF));
                pl.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
                pl.push_back(static_cast<std::uint8_t>(q & 0xFF));
                pl.push_back(static_cast<std::uint8_t>((q >> 8) & 0xFF));
            }
            if (sent % 7 == 3) {  // fragmented: first (no FIN), continuation, final continuation
                const std::size_t a = 301, b = 900;
                if (!sendB(s, frame(2, Bytes(pl.begin(), pl.begin() + a), false))) { return; }
                if (!sendB(s, frame(0, Bytes(pl.begin() + a, pl.begin() + b), false))) { return; }
                if (!sendB(s, frame(0, Bytes(pl.begin() + b, pl.end()), true))) { return; }
            } else if (!sendB(s, frame(2, pl))) {
                return;
            }
            if (sent % 5 == 0) {  // frames of other types, which a client that did not ask must ignore
                sendB(s, frame(2, Bytes{1, 0, 9, 9, 9, 9}));    // audio, primary
                sendB(s, frame(2, Bytes{5, 0, 1, 0, 2, 0}));    // IQ, secondary
                sendB(s, frame(2, Bytes{3, 0, 1, 2, 3}));       // spectrum
                sendB(s, frame(2, Bytes{77, 0, 1}));            // unknown type
                sendB(s, frame(2, Bytes{2}));                   // too short for a type
            }
            if (sent % 20 == 10) { sendB(s, frame(9, Bytes{'p'})); }
            ++sent;
            sent_.store(sent);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    TcpServer srv_;
    std::mutex m_, sendM_, streamM_;
    std::map<std::string, std::string> props_;
    std::vector<std::string> events_;
    std::string requestLine_, headers_;
    std::atomic<int> pongs_{0}, closes_{0}, unmasked_{0}, sent_{0};
    std::atomic<bool> streamRun_{false};
    std::thread streamThread_;

public:
    int framesSent() const { return sent_.load(); }
};

std::vector<std::complex<float>> readN(SdrConnectSource& s, std::size_t n, int ms = 4000) {
    std::vector<std::complex<float>> out;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    std::vector<std::complex<float>> tmp(512);
    while (out.size() < n && std::chrono::steady_clock::now() < end) {
        const std::size_t g = s.read(tmp.data(), tmp.size());
        out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(g));
        if (s.faulted()) { break; }
    }
    return out;
}

bool rampOk(const std::vector<std::complex<float>>& v, std::size_t n) {
    if (v.size() < n) { return false; }
    for (std::size_t i = 0; i < n; ++i) {
        const float a = v[i].real() * 32768.0f, b = v[i].imag() * 32768.0f;
        if (std::fabs(a + b) > 0.01f) { return false; }
        if (i > 0) {
            const float step = a - v[i - 1].real() * 32768.0f;
            if (!(std::fabs(step - 1.0f) < 0.01f || std::fabs(step + 19999.0f) < 0.01f)) { return false; }
        }
    }
    return true;
}

// --- tests -------------------------------------------------------------------------------

void testWebSocketLayer() {
    // FIPS 180 / RFC 3174 test vectors
    const auto d1 = ws::sha1(reinterpret_cast<const std::uint8_t*>("abc"), 3);
    CHECK(hexOf(d1.data(), 20) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    const auto d0 = ws::sha1(nullptr, 0);
    CHECK(hexOf(d0.data(), 20) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    const auto d2 = ws::sha1(reinterpret_cast<const std::uint8_t*>(two.data()), two.size());
    CHECK(hexOf(d2.data(), 20) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    const std::string long1(1000, 'a');
    const auto d3 = ws::sha1(reinterpret_cast<const std::uint8_t*>(long1.data()), long1.size());
    CHECK(hexOf(d3.data(), 20) == "291e9a6c66994949b57ba5e650361e98fc36b1ba");
    // RFC 4648 vectors
    CHECK(ws::base64(reinterpret_cast<const std::uint8_t*>("f"), 1) == "Zg==");
    CHECK(ws::base64(reinterpret_cast<const std::uint8_t*>("fo"), 2) == "Zm8=");
    CHECK(ws::base64(reinterpret_cast<const std::uint8_t*>("foo"), 3) == "Zm9v");
    CHECK(ws::base64(reinterpret_cast<const std::uint8_t*>("foobar"), 6) == "Zm9vYmFy");
    CHECK(ws::base64(nullptr, 0).empty());
    // RFC 6455 section 1.3: key to accept
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string ok = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    CHECK(ws::checkHandshakeResponse(ok, key).empty());
    std::string wrong = ok;
    wrong.replace(wrong.find("s3pP"), 4, "s3pQ");
    CHECK(!ws::checkHandshakeResponse(wrong, key).empty());
    CHECK(!ws::checkHandshakeResponse("HTTP/1.1 200 OK\r\n\r\n", key).empty());
    CHECK(!ws::checkHandshakeResponse("garbage", key).empty());
    CHECK(!ws::checkHandshakeResponse("HTTP/1.1 101 X\r\nUpgrade: websocket\r\n\r\n", key).empty());  // no accept, no connection
    // header names and the two values are case-insensitive
    std::string mixed = "HTTP/1.1 101 Switching Protocols\r\nupgrade: WebSocket\r\nCONNECTION: upgrade\r\n"
                        "sec-websocket-accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    CHECK(ws::checkHandshakeResponse(mixed, key).empty());
    // the request for the nonce in the RFC's example
    std::array<std::uint8_t, 16> nonce = {0x74, 0x68, 0x65, 0x20, 0x73, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x20, 0x6e, 0x6f, 0x6e, 0x63, 0x65};
    const std::string rq = ws::handshakeRequest("h", 5454, "/", nonce);
    CHECK(rq.find("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") != std::string::npos);
    CHECK(rq.find("GET / HTTP/1.1\r\n") == 0 && rq.find("Sec-WebSocket-Version: 13\r\n") != std::string::npos);
    CHECK(rq.size() > 4 && rq.compare(rq.size() - 4, 4, "\r\n\r\n") == 0);
    // RFC 6455 5.7: a masked single-frame text message "Hello" is 81 85 37 fa 21 3d 7f 9f 4d 51 58
    const std::uint8_t hello[5] = {'H', 'e', 'l', 'l', 'o'};
    const auto f = ws::encodeClientFrame(ws::kText, hello, 5, {0x37, 0xfa, 0x21, 0x3d});
    const std::uint8_t want[11] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58};
    CHECK(f.size() == 11 && std::memcmp(f.data(), want, 11) == 0);
    // the unmasked server example "Hello" 81 05 48 65 6c 6c 6f, delivered a byte at a time
    ws::FrameParser p;
    const std::uint8_t srv[7] = {0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f};
    ws::Message m;
    for (int i = 0; i < 7; ++i) {
        CHECK(!p.next(m));
        p.feed(srv + i, 1);
    }
    CHECK(p.next(m) && m.op == ws::kText && std::string(m.payload.begin(), m.payload.end()) == "Hello");
    CHECK(!p.failed());
    // RFC 6455 5.7: fragmented "Hel" + "lo"
    ws::FrameParser q;
    const std::uint8_t frag[] = {0x01, 0x03, 0x48, 0x65, 0x6c, 0x80, 0x02, 0x6c, 0x6f};
    q.feed(frag, sizeof(frag));
    CHECK(q.next(m) && m.op == ws::kText && std::string(m.payload.begin(), m.payload.end()) == "Hello");
    // 16-bit and 64-bit lengths
    for (const std::size_t len : {std::size_t(125), std::size_t(126), std::size_t(65535), std::size_t(65536), std::size_t(100000)}) {
        Bytes b = frame(2, Bytes(len, 7));
        ws::FrameParser r;
        r.feed(b.data(), b.size());
        CHECK(r.next(m) && m.payload.size() == len && !r.failed());
    }
    // violations
    const auto fails = [](const Bytes& b, std::size_t limit = 4u << 20) {
        ws::FrameParser r(limit);
        r.feed(b.data(), b.size());
        return r.failed() && !r.error().empty();
    };
    CHECK(fails({0x81, 0x80, 1, 2, 3, 4}));                       // masked from the server
    CHECK(fails({0xC1, 0x00}));                                   // RSV1
    CHECK(fails({0x83, 0x00}));                                   // reserved data opcode
    CHECK(fails({0x8B, 0x00}));                                   // reserved control opcode
    CHECK(fails({0x09, 0x00}));                                   // fragmented ping
    CHECK(fails({0x89, 126, 0x00, 0x7E}));                        // ping longer than 125
    CHECK(fails({0x80, 0x00}));                                   // continuation with nothing to continue
    CHECK(fails({0x01, 0x00, 0x02, 0x00}));                       // new data frame inside a fragmented message
    CHECK(fails({0x82, 127, 0x80, 0, 0, 0, 0, 0, 0, 0}));         // top bit of the 64-bit length
    CHECK(fails({0x82, 127, 0, 0, 0, 0, 0x7F, 0xFF, 0xFF, 0xFF})); // oversized
    CHECK(fails({0x82, 126, 0x01, 0x00}, 255));                   // over the caller's limit
    // control frames may sit between fragments
    ws::FrameParser c;
    const std::uint8_t mid[] = {0x01, 0x01, 'a', 0x89, 0x00, 0x80, 0x01, 'b'};
    c.feed(mid, sizeof(mid));
    CHECK(c.next(m) && m.op == ws::kPing);
    CHECK(c.next(m) && m.op == ws::kText && m.payload.size() == 2);
}

void testMessages() {
    sc::Envelope e;
    CHECK(sc::parseMessage(R"({"event_type":"property_changed","property":"device_center_frequency","device":"primary","value":"100000000"})", e));
    CHECK(e.eventType == "property_changed" && e.property == "device_center_frequency" && e.device == "primary" && e.value == "100000000");
    CHECK(sc::parseMessage(R"({"event_type":"get_property_response","property":"x","value":12})", e) && e.value == "12");
    CHECK(sc::parseMessage(R"({"event_type":"get_property_response","property":"x","value":true})", e) && e.value == "true");
    CHECK(!sc::parseMessage("", e));
    CHECK(!sc::parseMessage("[1,2]", e));
    CHECK(!sc::parseMessage(R"({"property":"x"})", e));
    CHECK(!sc::parseMessage(R"({"event_type":5})", e));
    CHECK(!sc::parseMessage(R"({"event_type":"x","value":{"a":1}})", e));
    // a request: field names and string values exactly as the document's examples
    const json j = json::parse(sc::buildMessage("set_property", "device_center_frequency", "101000000", "primary"));
    CHECK(j["event_type"] == "set_property" && j["property"] == "device_center_frequency" &&
          j["device"] == "primary" && j["value"] == "101000000" && j["value"].is_string());
    CHECK(!json::parse(sc::buildMessage("iq_stream_enable", "", "true")).contains("device"));
    // binary
    const std::uint8_t a[] = {2, 0, 0x00, 0x80, 0xFF, 0x7F, 1, 0, 2, 0, 9};
    CHECK(sc::classifyBinary(a, sizeof(a)).kind == sc::BinaryKind::Iq);
    std::vector<std::complex<float>> v;
    CHECK(sc::decodeIq16(a + 2, sizeof(a) - 2, v) == 2);  // 9 bytes: two pairs and a stray byte
    CHECK(v[0].real() == -1.0f && std::fabs(v[0].imag() - 32767.0f / 32768.0f) < 1e-6f);
    CHECK(std::fabs(v[1].real() - 1.0f / 32768.0f) < 1e-9f && std::fabs(v[1].imag() - 2.0f / 32768.0f) < 1e-9f);
    CHECK(sc::classifyBinary(a, 1).kind == sc::BinaryKind::TooShort);
    const std::uint8_t sec[] = {5, 0};
    CHECK(sc::classifyBinary(sec, 2).kind == sc::BinaryKind::Iq && sc::classifyBinary(sec, 2).secondary);
    const std::uint8_t unk[] = {0, 0};
    CHECK(sc::classifyBinary(unk, 2).kind == sc::BinaryKind::Unknown);
    CHECK(sc::splitList("a, b,,c ").size() == 3 && sc::splitList("").empty());
}

void testHappyPath() {
    FakeSdrConnect fake;
    CHECK(fake.start());
    SdrConnectSource src;
    CHECK(src.open(fake.args("serial=1234,mode=IQ Lite")));
    CHECK(src.isOpen() && std::string(src.driverKey()) == "sdrconnect");
    CHECK(fake.requestLine() == "GET / HTTP/1.1");
    CHECK(fake.requestHeaders().find("Upgrade: websocket") != std::string::npos);
    CHECK(src.apiVersion() == "1.0.3");
    CHECK(src.activeDevice() == "RSP1A 1234");
    CHECK(std::string(src.name()) == "SDRconnect: RSP1A 1234");
    CHECK(src.validDevices().size() == 2);
    CHECK(fake.count("selected_device_serial", "1234:IQ Lite") == 1);
    CHECK_NEAR(src.sampleRateHz(), 2000000.0, 0.001);
    CHECK_NEAR(src.centerFrequencyHz(), 100000000.0, 0.001);
    CHECK(src.supportedSampleRatesHz().size() == 1);
    CHECK(src.antennas().size() == 3 && src.antennas()[1] == "Antenna B" && src.antenna() == "Antenna A");
    CHECK(src.gains().size() == 1 && src.gains()[0].maxDb == 9.0 && src.gains()[0].unit == cascade::source::GainUnit::Steps);
    CHECK(!src.autoGainSupported());
    // controls: each is a request and a read-back
    CHECK(src.setCenterFrequencyHz(145500000.0));
    CHECK(fake.prop("device_center_frequency") == "145500000");
    CHECK_NEAR(src.centerFrequencyHz(), 145500000.0, 0.001);
    CHECK(src.setSampleRateHz(8000000.0));
    CHECK(fake.prop("device_sample_rate") == "8000000");
    CHECK(src.setGainDb("LNA state", 12.4));  // clamped to the range maximum
    CHECK(fake.prop("lna_state") == "9" && src.gainDb("LNA state") == 9.0);
    CHECK(!src.setGainDb("Nope", 1.0));
    CHECK(src.setAntenna("Antenna C") && fake.prop("active_antenna") == "Antenna C");
    CHECK(!src.setAntenna("Nope"));
    CHECK(!src.setCenterFrequencyHz(-1.0));
    CHECK(!src.setSampleRateHz(0.0));
    // stopped: nothing delivered
    std::vector<std::complex<float>> tmp(16);
    CHECK(src.read(tmp.data(), tmp.size()) == 0);
    CHECK(src.start() && src.running());
    CHECK(fake.count("iq_stream_enable", "") >= 0);
    const auto got = readN(src, 6000);
    CHECK(got.size() >= 6000);
    CHECK(rampOk(got, 6000));  // includes the fragmented frames and everything of other types ignored
    CHECK(fake.pongs() >= 1);
    CHECK(fake.unmaskedClientFrames() == 0);  // every client frame was masked
    src.stop();
    CHECK(!src.running());
    src.closeDevice();
    CHECK(!src.isOpen());
    CHECK(fake.closes() == 1);
}

void testOpenRefusals() {
    {  // connection refused
        SdrConnectSource s;
        CHECK(!s.open("sdrconnect=127.0.0.1:" + std::to_string(deadPort())));
        CHECK(!s.isOpen() && std::string(s.lastError()).size() > 0);
    }
    {  // bad arguments
        SdrConnectSource s;
        CHECK(!s.open("sdrconnect=127.0.0.1:0"));
        CHECK(!s.open("sdrconnect=127.0.0.1:99999"));
        CHECK(!s.open("sdrconnect=127.0.0.1:1,path=noslash"));
        CHECK(!s.open("sdrconnect=127.0.0.1:1,path=/a b"));
        CHECK(!s.open("sdrconnect=127.0.0.1:1,timeout=1"));
    }
    struct Case { Hs h; const char* what; };
    const Case cases[] = {
        {Hs::Not101, "HTTP 400 instead of 101"}, {Hs::WrongAccept, "wrong accept value"},
        {Hs::NotHttp, "not HTTP at all"},        {Hs::CloseNow, "closed at once"},
        {Hs::Extensions, "an extension nobody asked for"},
    };
    for (const Case& c : cases) {
        FakeSdrConnect fake;
        CHECK(fake.start());
        fake.hs.store(c.h);
        SdrConnectSource s;
        const bool ok = s.open(fake.args("timeout=800"));
        if (ok) { std::printf("UNEXPECTED open success: %s", c.what); std::fputc(10, stdout); }
        CHECK(!ok && !s.isOpen());
        CHECK(std::string(s.lastError()).size() > 0);
    }
    {  // silent: bounded
        FakeSdrConnect fake;
        CHECK(fake.start());
        fake.hs.store(Hs::Silent);
        SdrConnectSource s;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!s.open(fake.args("timeout=400")));
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
        CHECK(std::string(s.lastError()).find("timed out") != std::string::npos);
    }
    {  // a WebSocket server that is not SDRconnect: it never answers api_version
        FakeSdrConnect fake;
        CHECK(fake.start());
        fake.hs.store(Hs::NoApiVersion);
        SdrConnectSource s;
        CHECK(!s.open(fake.args("timeout=400")));
        CHECK(std::string(s.lastError()).find("not an SDRconnect") != std::string::npos);
    }
    {  // a push sent in the same write as the handshake headers is kept (api_version still answers)
        FakeSdrConnect fake;
        CHECK(fake.start());
        fake.hs.store(Hs::EarlyPush);
        SdrConnectSource s;
        CHECK(s.open(fake.args()));
        s.closeDevice();
    }
}

void testStreamFaults() {
    struct Case { Sf f; const char* what; };
    const Case cases[] = {
        {Sf::CloseFrame, "close frame mid-stream"},      {Sf::Abrupt, "connection dropped mid-stream"},
        {Sf::HugeLength, "oversized frame length"},      {Sf::MaskedFrame, "masked server frame"},
        {Sf::ReservedOpcode, "reserved opcode"},         {Sf::FragmentOverrun, "data frame inside a fragmented message"},
    };
    for (const Case& c : cases) {
        FakeSdrConnect fake;
        CHECK(fake.start());
        fake.sf.store(c.f);
        fake.faultAfter.store(15);
        SdrConnectSource s;
        CHECK(s.open(fake.args()));
        CHECK(s.start());
        std::vector<std::complex<float>> tmp(512);
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (std::chrono::steady_clock::now() < end && !s.faulted()) { s.read(tmp.data(), tmp.size()); }
        if (!s.faulted()) { std::printf("no fault for: %s", c.what); std::fputc(10, stdout); }
        CHECK(s.faulted() && s.deviceDead());
        CHECK(std::string(s.lastError()).size() > 0 && !s.faultedWhile().empty());
        s.closeDevice();
    }
    {  // stalled stream: a streaming source whose server goes silent faults, bounded
        FakeSdrConnect fake;
        CHECK(fake.start());
        fake.sf.store(Sf::Stall);
        fake.faultAfter.store(10);
        SdrConnectSource s;
        const bool opened = s.open(fake.args("timeout=400"));
        if (!opened) { std::fprintf(stderr, "stall open failed: %s", s.lastError()); std::fputc(10, stderr); }
        CHECK(opened);
        CHECK(s.start());
        std::vector<std::complex<float>> tmp(512);
        const auto t0 = std::chrono::steady_clock::now();
        while (!s.faulted() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(6)) { s.read(tmp.data(), tmp.size()); }
        CHECK(s.faulted());
        CHECK(std::string(s.lastError()).find("stopped sending") != std::string::npos);
        s.closeDevice();
    }
}

void testStartStopAndShutdown() {
    FakeSdrConnect fake;
    CHECK(fake.start());
    const unsigned long long before = NetSampleSource::readersAbandoned();
    SdrConnectSource s;
    CHECK(s.open(fake.args()));
    for (int i = 0; i < 50; ++i) {
        CHECK(s.start());
        std::vector<std::complex<float>> tmp(64);
        s.read(tmp.data(), tmp.size());
        s.stop();
    }
    for (int i = 0; i < 300 && fake.count("iq_stream_enable", "") < 100; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(fake.count("iq_stream_enable", "") >= 100);
    CHECK(s.start());
    const auto got = readN(s, 800);
    CHECK(got.size() >= 800 && rampOk(got, 800));
    const auto t0 = std::chrono::steady_clock::now();
    s.closeDevice();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "close while streaming: %lld ms", static_cast<long long>(ms));
    std::fputc(10, stderr);
    CHECK(ms < 1500);
    for (int i = 0; i < 50; ++i) {
        SdrConnectSource t;
        CHECK(t.open(fake.args()));
        if (i % 5 == 0) { CHECK(t.start()); }
        t.closeDevice();
    }
    CHECK(NetSampleSource::readersAbandoned() == before);
    {
        SdrConnectSource u;  // destroyed while streaming, no closeDevice()
        CHECK(u.open(fake.args()));
        CHECK(u.start());
    }
    CHECK(NetSampleSource::readersAbandoned() == before);
    SdrConnectSource c;
    CHECK(!c.start() && !c.setCenterFrequencyHz(1e6) && !c.setSampleRateHz(1e6) && !c.setAntenna("x"));
    c.closeDevice();
    c.stop();
}

void testCloseWithSilentServer() {
    FakeSdrConnect fake;
    CHECK(fake.start());
    fake.sf.store(Sf::Stall);
    fake.faultAfter.store(5);
    SdrConnectSource s;
    CHECK(s.open(fake.args("timeout=5000")));
    CHECK(s.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const unsigned long long before = NetSampleSource::readersAbandoned();
    const auto t0 = std::chrono::steady_clock::now();
    s.closeDevice();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "close with a silent streaming server: %lld ms", static_cast<long long>(ms));
    std::fputc(10, stderr);
    CHECK(NetSampleSource::readersAbandoned() == before);
    CHECK(ms < 1500);
}

void testFuzz() {
    Rng rng(0xBADC0DEu);
    // 10,000 random byte streams into the frame parser (random splits), random text into the
    // JSON reader, random payloads into the binary classifier and the IQ decoder.
    int failed = 0, parsed = 0;
    for (int i = 0; i < 10000; ++i) {
        Bytes in;
        const int len = static_cast<int>(rng.next() % 300);
        const int style = static_cast<int>(rng.next() % 3);
        if (style == 1) { in.push_back(static_cast<std::uint8_t>(0x80 | (rng.next() % 3))); in.push_back(static_cast<std::uint8_t>(rng.next() % 127)); }
        for (int k = 0; k < len; ++k) { in.push_back(static_cast<std::uint8_t>(rng.next())); }
        ws::FrameParser p(1 << 16);
        std::size_t off = 0;
        while (off < in.size() && !p.failed()) {
            const std::size_t n = std::min<std::size_t>(1 + rng.next() % 40, in.size() - off);
            p.feed(in.data() + off, n);
            off += n;
            ws::Message m;
            while (p.next(m)) { ++parsed; }
        }
        failed += p.failed();
        const std::string txt(in.begin(), in.end());
        sc::Envelope e;
        sc::parseMessage(txt, e);
        std::vector<std::complex<float>> v;
        const sc::BinaryFrame f = sc::classifyBinary(in.data(), in.size());
        if (f.kind == sc::BinaryKind::Iq) { sc::decodeIq16(in.data() + f.payloadOffset, in.size() - f.payloadOffset, v); }
    }
    CHECK(failed > 1000);
    CHECK(parsed > 100);
}

}  // namespace

#define STEP(call) do { std::fputs(#call, stderr); std::fputc(10, stderr); std::fflush(stderr); call; } while (0)

int main() {
    STEP(testWebSocketLayer());
    STEP(testMessages());
    STEP(testHappyPath());
    STEP(testOpenRefusals());
    STEP(testStreamFaults());
    STEP(testStartStopAndShutdown());
    STEP(testCloseWithSilentServer());
    STEP(testFuzz());
    return testSummary("test_sdrconnect_source");
}
