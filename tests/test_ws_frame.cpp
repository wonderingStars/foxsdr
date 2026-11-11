// test_ws_frame.cpp - THE WEBSOCKET LAYER (net/ws_frame.*), pure: no sockets.
//
//   W1  SHA-1 and base64 against published vectors (FIPS 180-1 "abc" and the two-block message; RFC 4648 section 10), and the accept key
//       of RFC 6455 section 1.3 ("dGhlIHNhbXBsZSBub25jZQ==" -> "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=").
//   W2  THE HANDSHAKE: a good request is accepted; every way to be wrong is refused with the right status and a sentence.
//   W3  FRAMES: a masked client frame of every length class decodes; fragmentation, interleaved control frames, byte-at-a-time feeding.
//   W4  HOSTILE FRAMES: reserved bits, reserved opcodes, unmasked, non-minimal lengths, control frames that are fragmented or long, a
//       declared length over the cap (refused BEFORE its payload is read), a continuation with no start, a data frame inside a
//       fragmented message, invalid UTF-8 - each a final failure with the right close code.
//   W5  FUZZ: random bytes and random mutations of good frames never crash, never buffer more than the cap, and never loop.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

#include "net/ws_frame.hpp"
#include "test_check.hpp"

using namespace cascade::net::ws;

namespace {

std::string hex(const std::string& s) {
    static const char* d = "0123456789abcdef";
    std::string r;
    for (unsigned char c : s) { r.push_back(d[c >> 4]); r.push_back(d[c & 15]); }
    return r;
}

// A client-to-server frame, masked with `key`.
std::string clientFrame(Opcode op, const std::string& payload, bool fin = true, const unsigned char key[4] = nullptr, bool mask = true,
                        unsigned rsv = 0) {
    static const unsigned char k0[4] = {0x37, 0xfa, 0x21, 0x3d};
    const unsigned char* k = key != nullptr ? key : k0;
    std::string f;
    f.push_back(static_cast<char>((fin ? 0x80 : 0) | (rsv << 4) | static_cast<unsigned>(op)));
    const unsigned m = mask ? 0x80u : 0u;
    if (payload.size() < 126) {
        f.push_back(static_cast<char>(m | payload.size()));
    } else if (payload.size() <= 0xFFFF) {
        f.push_back(static_cast<char>(m | 126));
        f.push_back(static_cast<char>(payload.size() >> 8));
        f.push_back(static_cast<char>(payload.size() & 0xFF));
    } else {
        f.push_back(static_cast<char>(m | 127));
        for (int i = 7; i >= 0; --i) { f.push_back(static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> (8 * i)) & 0xFF)); }
    }
    if (mask) {
        f.append(reinterpret_cast<const char*>(k), 4);
        for (std::size_t i = 0; i < payload.size(); ++i) { f.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^ k[i & 3])); }
    } else {
        f += payload;
    }
    return f;
}

std::string goodRequest(const std::string& extra = "") {
    return "GET /tci HTTP/1.1\r\nHost: 127.0.0.1:50001\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n" + extra + "\r\n";
}

void testHashes() {
    CHECK(hex(sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");                                                  // FIPS 180-1 appendix A
    CHECK(hex(sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");  // appendix B
    CHECK(hex(sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(hex(sha1(std::string(1000, 'a'))) == "291e9a6c66994949b57ba5e650361e98fc36b1ba");   // computed with Python's hashlib in the session
    // RFC 4648 section 10 test vectors
    CHECK(base64Encode("") == "");
    CHECK(base64Encode("f") == "Zg==");
    CHECK(base64Encode("fo") == "Zm8=");
    CHECK(base64Encode("foo") == "Zm9v");
    CHECK(base64Encode("foob") == "Zm9vYg==");
    CHECK(base64Encode("fooba") == "Zm9vYmE=");
    CHECK(base64Encode("foobar") == "Zm9vYmFy");
    std::string out;
    CHECK(base64Decode("Zm9vYmFy", out) && out == "foobar");
    CHECK(base64Decode("Zm9vYg==", out) && out == "foob");
    CHECK(!base64Decode("Zm9vYg=", out));       // length
    CHECK(!base64Decode("Zm9v!mFy", out));      // alphabet
    CHECK(!base64Decode("Zg=A", out));          // data after padding
    CHECK(!base64Decode("=Zg=", out));
    CHECK(acceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");   // RFC 6455 section 1.3
}

void testHandshake() {
    Handshake h = parseHandshake(goodRequest());
    CHECK(h.ok && h.key == "dGhlIHNhbXBsZSBub25jZQ==" && h.path == "/tci");
    CHECK(handshakeAcceptResponse(h.key).find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos);
    CHECK(handshakeAcceptResponse(h.key).rfind("HTTP/1.1 101 ", 0) == 0);
    // header names and the Connection list are case-insensitive; Connection may list several tokens
    h = parseHandshake("GET / HTTP/1.1\r\nupgrade: WebSocket\r\nCONNECTION: keep-alive, Upgrade\r\nsec-websocket-key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                       "SEC-WEBSOCKET-VERSION: 13\r\n\r\n");
    CHECK(h.ok);
    struct Bad { std::string req; int status; };
    const Bad bad[] = {
        {"POST / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", 405},
        {"GET / HTTP/1.0\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", 400},
        {"GET /\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: x\r\n\r\n", 400},   // a plain HTTP GET: no upgrade
        {"GET / HTTP/1.1\r\nUpgrade: websocket\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", 400},  // no Connection
        {"GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 8\r\n\r\n", 426},
        {"GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n\r\n", 400},   // no key
        {"GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: AAAA\r\nSec-WebSocket-Version: 13\r\n\r\n", 400},  // 3 bytes
        {"GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: not base64!!!!!!!!!!!!!\r\nSec-WebSocket-Version: 13\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
         "Sec-WebSocket-Version: 13\r\n\r\n", 400},   // two keys
        {"GET / HTTP/1.1\r\nbad header line\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n", 400},
        {std::string("GET / HTTP/1.1\r\nX: a") + '\0' + "b\r\n\r\n", 400},
        {"", 400},
    };
    for (const Bad& b : bad) {
        const Handshake r = parseHandshake(b.req);
        CHECK(!r.ok);
        CHECK(r.status == b.status);
        if (r.ok || r.status != b.status) { std::printf("      request: %.60s -> ok %d status %d (%s)\n", b.req.c_str(), r.ok ? 1 : 0, r.status, r.error.c_str()); }
        CHECK(!r.error.empty());
    }
    // too long
    const Handshake big = parseHandshake(goodRequest("X-Pad: " + std::string(kMaxHandshakeBytes, 'a') + "\r\n"));
    CHECK(!big.ok && big.status == 431);
    // the refusal text is a real HTTP response that says why
    const std::string ref = handshakeRefusal(426, "only version 13");
    CHECK(ref.rfind("HTTP/1.1 426 ", 0) == 0 && ref.find("Sec-WebSocket-Version: 13") != std::string::npos && ref.find("only version 13") != std::string::npos);
    CHECK(handshakeRefusal(503, "full").rfind("HTTP/1.1 503 ", 0) == 0);
}

void testFrames() {
    FrameDecoder d(1u << 20);
    Message m;
    // every length class: 0, 1, 125, 126, 65535, 65536
    for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{125}, std::size_t{126}, std::size_t{65535}, std::size_t{65536}}) {
        std::string payload(n, 'x');
        for (std::size_t i = 0; i < n; ++i) { payload[i] = static_cast<char>('a' + i % 26); }
        d.feed(clientFrame(Opcode::Text, payload).data(), clientFrame(Opcode::Text, payload).size());
        CHECK(d.next(m) && m.op == Opcode::Text && m.payload == payload);
        CHECK(!d.next(m));
        CHECK(!d.failed());
    }
    // one byte at a time
    {
        FrameDecoder one(4096);
        const std::string f = clientFrame(Opcode::Binary, std::string("\x01\x02\x03", 3)) + clientFrame(Opcode::Text, "hello");
        int got = 0;
        for (char c : f) {
            one.feed(&c, 1);
            while (one.next(m)) {
                ++got;
                if (got == 1) { CHECK(m.op == Opcode::Binary && m.payload.size() == 3); }
                if (got == 2) { CHECK(m.op == Opcode::Text && m.payload == "hello"); }
            }
        }
        CHECK(got == 2);
    }
    // fragmentation with a ping in the middle, and a different mask per frame
    {
        FrameDecoder f(4096);
        const unsigned char k1[4] = {1, 2, 3, 4}, k2[4] = {9, 8, 7, 6}, k3[4] = {0, 0, 0, 0};
        std::string s = clientFrame(Opcode::Text, "Hel", false, k1) + clientFrame(Opcode::Ping, "p", true, k2) +
                        clientFrame(Opcode::Continuation, "lo, ", false, k3) + clientFrame(Opcode::Continuation, "world", true, k1);
        f.feed(s.data(), s.size());
        CHECK(f.next(m) && m.op == Opcode::Ping && m.payload == "p");
        CHECK(f.next(m) && m.op == Opcode::Text && m.payload == "Hello, world");
        CHECK(!f.next(m) && !f.failed());
    }
    // close with a code and a reason
    {
        FrameDecoder f(4096);
        const std::string s = clientFrame(Opcode::Close, std::string("\x03\xE8", 2) + "bye");
        f.feed(s.data(), s.size());
        CHECK(f.next(m) && m.op == Opcode::Close && m.payload.size() == 5);
    }
    // the server's own frames: layout and lengths
    CHECK(encodeFrame(Opcode::Text, "hi") == std::string("\x81\x02hi", 4));
    CHECK(encodeFrame(Opcode::Binary, std::string(300, 'z')).substr(0, 4) == std::string("\x82\x7e\x01\x2c", 4));
    CHECK(encodeFrame(Opcode::Binary, std::string(70000, 'z')).substr(0, 10) == std::string("\x82\x7f\x00\x00\x00\x00\x00\x01\x11\x70", 10));
    CHECK(encodeFrame(Opcode::Ping, std::string(200, 'p')).size() == 2 + 125);   // control payload cut to 125
    CHECK(encodeClose(1000, "ok") == std::string("\x88\x04\x03\xe8ok", 6));
    CHECK(encodeClose(1002, std::string(500, 'r')).size() == 2 + 2 + 123);
    // valid and invalid UTF-8
    CHECK(isValidUtf8("") && isValidUtf8("plain") && isValidUtf8("\xC3\xA9") && isValidUtf8("\xE2\x82\xAC") && isValidUtf8("\xF0\x9F\x98\x80"));
    CHECK(!isValidUtf8("\xC0\x80"));          // overlong
    CHECK(!isValidUtf8("\xC3"));              // truncated
    CHECK(!isValidUtf8("\xED\xA0\x80"));      // a surrogate
    CHECK(!isValidUtf8("\xF4\x90\x80\x80"));  // above U+10FFFF
    CHECK(!isValidUtf8("\x80"));              // a stray continuation byte
    CHECK(!isValidUtf8("\xE2\x82"));
}

// Feeds `bytes` to a fresh decoder (cap `cap`) and returns it after pulling everything.
struct Outcome { bool failed; std::uint16_t code; int messages; };
Outcome run(const std::string& bytes, std::size_t cap = 1024) {
    FrameDecoder d(cap);
    d.feed(bytes.data(), bytes.size());
    Message m;
    int n = 0;
    while (d.next(m)) { ++n; }
    return {d.failed(), d.errorCode(), n};
}

void testHostileFrames() {
    // reserved bits
    for (unsigned rsv = 1; rsv < 8; ++rsv) {
        const Outcome o = run(clientFrame(Opcode::Text, "x", true, nullptr, true, rsv));
        CHECK(o.failed && o.code == kCloseProtocolError);
    }
    // reserved opcodes 3..7 and 11..15
    for (unsigned op : {3u, 4u, 5u, 6u, 7u, 11u, 12u, 13u, 14u, 15u}) {
        const Outcome o = run(clientFrame(static_cast<Opcode>(op), "x"));
        CHECK(o.failed && o.code == kCloseProtocolError);
    }
    // unmasked
    {
        const Outcome o = run(clientFrame(Opcode::Text, "x", true, nullptr, false));
        CHECK(o.failed && o.code == kCloseProtocolError);
    }
    // non-minimal lengths (126 form for a short payload, 127 form for a 16-bit one)
    {
        std::string f(1, static_cast<char>(0x81));
        f.push_back(static_cast<char>(0x80 | 126));
        f.push_back(0); f.push_back(5);
        f += std::string(4, 'k') + "xxxxx";
        CHECK(run(f).failed);
        std::string g(1, static_cast<char>(0x81));
        g.push_back(static_cast<char>(0x80 | 127));
        for (int i = 0; i < 7; ++i) { g.push_back(0); }
        g.push_back(100);
        g += std::string(4, 'k') + std::string(100, 'x');
        CHECK(run(g).failed);
        // the top bit of a 64-bit length set
        std::string h(1, static_cast<char>(0x81));
        h.push_back(static_cast<char>(0x80 | 127));
        h.push_back(static_cast<char>(0x80));
        for (int i = 0; i < 7; ++i) { h.push_back(0); }
        h += std::string(4, 'k');
        CHECK(run(h).failed);
    }
    // control frames: fragmented, or too long
    CHECK(run(clientFrame(Opcode::Ping, "x", false)).failed);
    CHECK(run(clientFrame(Opcode::Close, std::string(126, 'c'))).failed);
    CHECK(run(clientFrame(Opcode::Close, "x")).failed);   // a 1-byte close payload
    // continuation with no start; a new data frame inside a fragmented message
    CHECK(run(clientFrame(Opcode::Continuation, "x")).failed);
    CHECK(run(clientFrame(Opcode::Text, "ab", false) + clientFrame(Opcode::Text, "cd")).failed);
    // the declared length decides, before any payload arrives: a header that declares 2 MiB against a 1 KiB cap
    {
        FrameDecoder d(1024);
        std::string f(1, static_cast<char>(0x82));
        f.push_back(static_cast<char>(0x80 | 127));
        const std::uint64_t len = 2u << 20;
        for (int i = 7; i >= 0; --i) { f.push_back(static_cast<char>((len >> (8 * i)) & 0xFF)); }
        f += std::string(4, 'k');
        d.feed(f.data(), f.size());
        Message m;
        CHECK(!d.next(m) && d.failed() && d.errorCode() == kCloseMessageTooBig);
        CHECK(d.buffered() == 0);
    }
    // a fragmented message that grows past the cap across frames
    {
        std::string s;
        for (int i = 0; i < 20; ++i) { s += clientFrame(i == 0 ? Opcode::Binary : Opcode::Continuation, std::string(100, 'a'), false); }
        const Outcome o = run(s, 1024);
        CHECK(o.failed && o.code == kCloseMessageTooBig);
    }
    // invalid UTF-8 in text; the same bytes as BINARY are fine
    CHECK(run(clientFrame(Opcode::Text, "\xC0\x80")).code == kCloseInvalidPayload);
    {
        const Outcome o = run(clientFrame(Opcode::Binary, "\xC0\x80"));
        CHECK(!o.failed && o.messages == 1);
    }
    // a failure is final: later good bytes are ignored
    {
        FrameDecoder d(1024);
        d.feed(clientFrame(Opcode::Text, "x", true, nullptr, false).data(), 3);
        Message m;
        CHECK(!d.next(m) && d.failed());
        const std::string good = clientFrame(Opcode::Text, "ok");
        d.feed(good.data(), good.size());
        CHECK(!d.next(m) && d.failed() && d.buffered() == 0);
    }
}

void testFuzz() {
    std::mt19937 rng(20261111);
    const std::string seeds[] = {clientFrame(Opcode::Text, "VFO:0,0,7100000;"), clientFrame(Opcode::Binary, std::string(300, 'b')),
                                 clientFrame(Opcode::Text, "Hel", false) + clientFrame(Opcode::Continuation, "lo", true),
                                 clientFrame(Opcode::Ping, "p"), clientFrame(Opcode::Close, std::string("\x03\xE8", 2))};
    std::size_t worst = 0;
    for (int iter = 0; iter < 4000; ++iter) {
        std::string s;
        if (iter % 3 == 0) {
            const std::size_t n = rng() % 400;
            for (std::size_t i = 0; i < n; ++i) { s.push_back(static_cast<char>(rng())); }
        } else {
            s = seeds[rng() % 5] + seeds[rng() % 5];
            const int flips = 1 + static_cast<int>(rng() % 4);
            for (int k = 0; k < flips && !s.empty(); ++k) { s[rng() % s.size()] = static_cast<char>(rng()); }
        }
        FrameDecoder d(2048);
        std::size_t pos = 0;
        Message m;
        int pulled = 0;
        while (pos < s.size() && pulled < 100000) {
            const std::size_t step = 1 + rng() % 17;
            d.feed(s.data() + pos, std::min(step, s.size() - pos));
            pos += step;
            while (d.next(m)) { ++pulled; }
            worst = std::max(worst, d.buffered());
        }
    }
    CHECK(worst <= 2048 + 10 + 4);   // the cap plus one header
    std::printf("fuzz: 4000 inputs, the decoder never held more than %zu bytes (cap 2048 + a 14-byte header)\n", worst);
}

}  // namespace

int main() {
    testHashes();
    testHandshake();
    testFrames();
    testHostileFrames();
    testFuzz();
    return testSummary("test_ws_frame");
}
