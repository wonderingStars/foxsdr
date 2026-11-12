// websocket_client.cpp - see websocket_client.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/websocket_client.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace cascade::source::ws {

namespace {

std::uint32_t rotl(std::uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) { ++a; }
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) { --b; }
    return s.substr(a, b - a);
}

}  // namespace

std::array<std::uint8_t, 20> sha1(const std::uint8_t* data, std::size_t n) {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::vector<std::uint8_t> m(data, data + n);
    m.push_back(0x80);
    while (m.size() % 64 != 56) { m.push_back(0); }
    const std::uint64_t bits = static_cast<std::uint64_t>(n) * 8u;
    for (int s = 56; s >= 0; s -= 8) { m.push_back(static_cast<std::uint8_t>(bits >> s)); }
    for (std::size_t off = 0; off < m.size(); off += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(m[off + 4 * i]) << 24) |
                   (static_cast<std::uint32_t>(m[off + 4 * i + 1]) << 16) |
                   (static_cast<std::uint32_t>(m[off + 4 * i + 2]) << 8) | m[off + 4 * i + 3];
        }
        for (int i = 16; i < 80; ++i) { w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1); }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const std::uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::array<std::uint8_t, 20> out{};
    for (int i = 0; i < 5; ++i) {
        out[4 * i] = static_cast<std::uint8_t>(h[i] >> 24);
        out[4 * i + 1] = static_cast<std::uint8_t>(h[i] >> 16);
        out[4 * i + 2] = static_cast<std::uint8_t>(h[i] >> 8);
        out[4 * i + 3] = static_cast<std::uint8_t>(h[i]);
    }
    return out;
}

std::string base64(const std::uint8_t* d, std::size_t n) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < n; i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(d[i]) << 16) |
                                (i + 1 < n ? static_cast<std::uint32_t>(d[i + 1]) << 8 : 0u) |
                                (i + 2 < n ? d[i + 2] : 0u);
        out += tab[(v >> 18) & 63];
        out += tab[(v >> 12) & 63];
        out += i + 1 < n ? tab[(v >> 6) & 63] : '=';
        out += i + 2 < n ? tab[v & 63] : '=';
    }
    return out;
}

std::string handshakeRequest(const std::string& host, std::uint16_t port, const std::string& path,
                             const std::array<std::uint8_t, 16>& nonce) {
    std::string r = "GET " + (path.empty() ? std::string("/") : path) + " HTTP/1.1\r\n";
    r += "Host: " + host + ":" + std::to_string(port) + "\r\n";
    r += "Upgrade: websocket\r\nConnection: Upgrade\r\n";
    r += "Sec-WebSocket-Key: " + base64(nonce.data(), nonce.size()) + "\r\n";
    r += "Sec-WebSocket-Version: 13\r\n\r\n";
    return r;
}

std::string checkHandshakeResponse(const std::string& response, const std::string& key) {
    std::size_t lineEnd = response.find("\r\n");
    if (lineEnd == std::string::npos) { return "the server's reply is not an HTTP response"; }
    const std::string status = response.substr(0, lineEnd);
    if (status.compare(0, 9, "HTTP/1.1 ") != 0 || status.size() < 12 || status.compare(9, 3, "101") != 0) {
        return "the server did not agree to a WebSocket connection (" + status.substr(0, 60) + ")";
    }
    bool upgrade = false, connection = false, accepted = false;
    const std::string want = [&key] {
        std::string k = key + kGuid;
        const auto d = sha1(reinterpret_cast<const std::uint8_t*>(k.data()), k.size());
        return base64(d.data(), d.size());
    }();
    std::size_t pos = lineEnd + 2;
    while (pos < response.size()) {
        std::size_t e = response.find("\r\n", pos);
        if (e == std::string::npos) { e = response.size(); }
        const std::string line = response.substr(pos, e - pos);
        pos = e + 2;
        if (line.empty()) { break; }
        const std::size_t c = line.find(':');
        if (c == std::string::npos) { return "a malformed header line in the server's reply"; }
        const std::string name = lower(trim(line.substr(0, c)));
        const std::string val = trim(line.substr(c + 1));
        if (name == "upgrade") { upgrade = lower(val) == "websocket"; }
        else if (name == "connection") { connection = lower(val).find("upgrade") != std::string::npos; }
        else if (name == "sec-websocket-accept") { accepted = val == want; }
        else if (name == "sec-websocket-extensions") { return "the server asked for a WebSocket extension that was not offered"; }
        else if (name == "sec-websocket-protocol") { return "the server named a WebSocket subprotocol that was not offered"; }
    }
    if (!upgrade || !connection) { return "the server's reply is not a WebSocket upgrade"; }
    if (!accepted) { return "the server's WebSocket accept value is wrong"; }
    return std::string();
}

std::vector<std::uint8_t> encodeClientFrame(Opcode op, const std::uint8_t* payload, std::size_t n,
                                            const std::array<std::uint8_t, 4>& mask) {
    std::vector<std::uint8_t> f;
    f.push_back(static_cast<std::uint8_t>(0x80 | op));
    if (n < 126) {
        f.push_back(static_cast<std::uint8_t>(0x80 | n));
    } else if (n <= 0xFFFF) {
        f.push_back(0x80 | 126);
        f.push_back(static_cast<std::uint8_t>(n >> 8));
        f.push_back(static_cast<std::uint8_t>(n));
    } else {
        f.push_back(0x80 | 127);
        for (int s = 56; s >= 0; s -= 8) { f.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(n) >> s)); }
    }
    f.insert(f.end(), mask.begin(), mask.end());
    for (std::size_t i = 0; i < n; ++i) { f.push_back(static_cast<std::uint8_t>(payload[i] ^ mask[i % 4])); }
    return f;
}

void FrameParser::feed(const std::uint8_t* data, std::size_t n) {
    if (failed_) { return; }
    buf_.insert(buf_.end(), data, data + n);
    parse();
    if (pos_ > 0 && pos_ == buf_.size()) { buf_.clear(); pos_ = 0; }
    else if (pos_ > (1u << 16)) { buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_)); pos_ = 0; }
}

bool FrameParser::next(Message& out) {
    if (readyPos_ >= ready_.size()) { ready_.clear(); readyPos_ = 0; return false; }
    out = std::move(ready_[readyPos_++]);
    return true;
}

void FrameParser::parse() {
    for (;;) {
        const std::size_t avail = buf_.size() - pos_;
        if (avail < 2) { return; }
        const std::uint8_t b0 = buf_[pos_], b1 = buf_[pos_ + 1];
        const bool fin = (b0 & 0x80) != 0;
        if ((b0 & 0x70) != 0) { fail("a WebSocket frame with reserved bits set (an extension was not negotiated)"); return; }
        const std::uint8_t op = b0 & 0x0F;
        if (b1 & 0x80) { fail("a masked WebSocket frame from the server"); return; }
        std::uint64_t len = b1 & 0x7F;
        std::size_t hdr = 2;
        if (len == 126) {
            if (avail < 4) { return; }
            len = (static_cast<std::uint64_t>(buf_[pos_ + 2]) << 8) | buf_[pos_ + 3];
            hdr = 4;
        } else if (len == 127) {
            if (avail < 10) { return; }
            len = 0;
            for (int i = 0; i < 8; ++i) { len = (len << 8) | buf_[pos_ + 2 + i]; }
            if (len >> 63) { fail("a WebSocket frame length with the top bit set"); return; }
            hdr = 10;
        }
        const bool control = (op & 0x8) != 0;
        if (op > 2 && op < 8) { fail("a WebSocket frame with a reserved opcode"); return; }
        if (op > 10) { fail("a WebSocket frame with a reserved opcode"); return; }
        if (control && (!fin || len > 125)) { fail("a fragmented or oversized WebSocket control frame"); return; }
        if (len > maxMessage_) { fail("a WebSocket frame longer than the limit"); return; }
        if (avail < hdr + len) { return; }
        std::vector<std::uint8_t> payload(buf_.begin() + static_cast<std::ptrdiff_t>(pos_ + hdr),
                                          buf_.begin() + static_cast<std::ptrdiff_t>(pos_ + hdr + len));
        pos_ += hdr + static_cast<std::size_t>(len);
        if (control) {
            Message m;
            m.op = static_cast<Opcode>(op);
            m.payload = std::move(payload);
            ready_.push_back(std::move(m));
            continue;
        }
        if (op == kContinuation) {
            if (!fragmenting_) { fail("a WebSocket continuation frame with nothing to continue"); return; }
            if (frag_.size() + payload.size() > maxMessage_) { fail("a WebSocket message longer than the limit"); return; }
            frag_.insert(frag_.end(), payload.begin(), payload.end());
            if (fin) {
                Message m;
                m.op = fragOp_;
                m.payload = std::move(frag_);
                frag_.clear();
                fragmenting_ = false;
                ready_.push_back(std::move(m));
            }
            continue;
        }
        if (fragmenting_) { fail("a new WebSocket data frame inside a fragmented message"); return; }
        if (fin) {
            Message m;
            m.op = static_cast<Opcode>(op);
            m.payload = std::move(payload);
            ready_.push_back(std::move(m));
        } else {
            fragmenting_ = true;
            fragOp_ = static_cast<Opcode>(op);
            frag_ = std::move(payload);
        }
    }
}

}  // namespace cascade::source::ws
