// See ws_frame.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/ws_frame.hpp"

#include <cctype>
#include <cstring>

namespace cascade::net::ws {
namespace {

std::uint32_t rol(std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return r;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) { s.remove_prefix(1); }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) { s.remove_suffix(1); }
    return s;
}

// True when the comma-separated header value lists `token` (case-insensitive).
bool listsToken(std::string_view value, std::string_view token) {
    std::size_t pos = 0;
    while (pos <= value.size()) {
        std::size_t comma = value.find(',', pos);
        if (comma == std::string_view::npos) { comma = value.size(); }
        if (lower(trim(value.substr(pos, comma - pos))) == token) { return true; }
        pos = comma + 1;
    }
    return false;
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

std::string sha1(std::string_view data) {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::string msg(data);
    const std::uint64_t bitLen = static_cast<std::uint64_t>(data.size()) * 8u;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) { msg.push_back('\0'); }
    for (int i = 7; i >= 0; --i) { msg.push_back(static_cast<char>((bitLen >> (8 * i)) & 0xFFu)); }
    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(msg.data()) + off + 4 * i;
            w[i] = (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
                   (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
        }
        for (int i = 16; i < 80; ++i) { w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1); }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::string out;
    for (int i = 0; i < 5; ++i) {
        for (int s = 24; s >= 0; s -= 8) { out.push_back(static_cast<char>((h[i] >> s) & 0xFFu)); }
    }
    return out;
}

std::string base64Encode(std::string_view in) {
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(static_cast<unsigned char>(in[i])) << 16) |
                                (static_cast<std::uint32_t>(static_cast<unsigned char>(in[i + 1])) << 8) |
                                static_cast<unsigned char>(in[i + 2]);
        out.push_back(kB64[(v >> 18) & 63]); out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]); out.push_back(kB64[v & 63]);
    }
    if (i + 1 == in.size()) {
        const std::uint32_t v = static_cast<std::uint32_t>(static_cast<unsigned char>(in[i])) << 16;
        out.push_back(kB64[(v >> 18) & 63]); out.push_back(kB64[(v >> 12) & 63]); out += "==";
    } else if (i + 2 == in.size()) {
        const std::uint32_t v = (static_cast<std::uint32_t>(static_cast<unsigned char>(in[i])) << 16) |
                                (static_cast<std::uint32_t>(static_cast<unsigned char>(in[i + 1])) << 8);
        out.push_back(kB64[(v >> 18) & 63]); out.push_back(kB64[(v >> 12) & 63]); out.push_back(kB64[(v >> 6) & 63]); out.push_back('=');
    }
    return out;
}

bool base64Decode(std::string_view text, std::string& out) {
    out.clear();
    if (text.size() % 4 != 0) { return false; }
    const auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') { return c - 'A'; }
        if (c >= 'a' && c <= 'z') { return c - 'a' + 26; }
        if (c >= '0' && c <= '9') { return c - '0' + 52; }
        if (c == '+') { return 62; }
        if (c == '/') { return 63; }
        return -1;
    };
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = text[i + k];
            if (c == '=') {
                if (!last || k < 2) { return false; }
                ++pad;
                v[k] = 0;
            } else {
                if (pad != 0) { return false; }
                v[k] = val(c);
                if (v[k] < 0) { return false; }
            }
        }
        const std::uint32_t n = (static_cast<std::uint32_t>(v[0]) << 18) | (static_cast<std::uint32_t>(v[1]) << 12) |
                                (static_cast<std::uint32_t>(v[2]) << 6) | static_cast<std::uint32_t>(v[3]);
        out.push_back(static_cast<char>((n >> 16) & 0xFF));
        if (pad < 2) { out.push_back(static_cast<char>((n >> 8) & 0xFF)); }
        if (pad < 1) { out.push_back(static_cast<char>(n & 0xFF)); }
    }
    return true;
}

std::string acceptKey(std::string_view clientKey) {
    static const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";  // RFC 6455 section 1.3
    return base64Encode(sha1(std::string(clientKey) + kGuid));
}

Handshake parseHandshake(std::string_view head) {
    Handshake h;
    const auto refuse = [&](int status, const char* why) {
        h.ok = false;
        h.status = status;
        h.error = why;
        return h;
    };
    if (head.size() > kMaxHandshakeBytes) { return refuse(431, "the request head is too long"); }
    if (head.find('\0') != std::string_view::npos) { return refuse(400, "the request holds a NUL byte"); }
    const std::size_t lineEnd = head.find("\r\n");
    if (lineEnd == std::string_view::npos) { return refuse(400, "no request line"); }
    const std::string_view line = head.substr(0, lineEnd);
    const std::size_t sp1 = line.find(' ');
    const std::size_t sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) { return refuse(400, "malformed request line"); }
    if (line.substr(0, sp1) != "GET") { return refuse(405, "a WebSocket opens with GET"); }
    h.path = std::string(line.substr(sp1 + 1, sp2 - sp1 - 1));
    if (line.substr(sp2 + 1) != "HTTP/1.1") { return refuse(400, "a WebSocket needs HTTP/1.1"); }
    bool upgrade = false, connection = false, versionOk = false;
    int keys = 0;
    std::size_t pos = lineEnd + 2;
    while (pos < head.size()) {
        const std::size_t e = head.find("\r\n", pos);
        if (e == std::string_view::npos) { return refuse(400, "unterminated header"); }
        if (e == pos) { break; }  // the blank line
        const std::string_view hl = head.substr(pos, e - pos);
        pos = e + 2;
        const std::size_t colon = hl.find(':');
        if (colon == std::string_view::npos || colon == 0) { return refuse(400, "malformed header"); }
        const std::string name = lower(hl.substr(0, colon));
        const std::string_view value = trim(hl.substr(colon + 1));
        if (name == "upgrade") { upgrade = upgrade || listsToken(value, "websocket"); }
        else if (name == "connection") { connection = connection || listsToken(value, "upgrade"); }
        else if (name == "sec-websocket-version") { versionOk = versionOk || listsToken(value, "13"); }
        else if (name == "sec-websocket-key") { ++keys; h.key = std::string(value); }
    }
    if (!upgrade || !connection) { return refuse(400, "this port speaks WebSocket only (no Upgrade request)"); }
    if (!versionOk) { return refuse(426, "only WebSocket version 13 is spoken"); }
    std::string raw;
    if (keys != 1 || !base64Decode(h.key, raw) || raw.size() != 16) { return refuse(400, "no valid Sec-WebSocket-Key"); }
    h.ok = true;
    h.status = 101;
    return h;
}

std::string handshakeAcceptResponse(std::string_view clientKey) {
    return "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
           acceptKey(clientKey) + "\r\n\r\n";
}

std::string handshakeRefusal(int status, std::string_view why) {
    const char* reason = status == 405   ? "Method Not Allowed"
                         : status == 426 ? "Upgrade Required"
                         : status == 431 ? "Request Header Fields Too Large"
                         : status == 503 ? "Service Unavailable"
                                         : "Bad Request";
    std::string body(why);
    body += "\n";
    std::string r = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: text/plain\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\nConnection: close\r\n";
    if (status == 426) { r += "Sec-WebSocket-Version: 13\r\n"; }
    return r + "\r\n" + body;
}

std::string encodeFrame(Opcode op, std::string_view payload) {
    const bool control = static_cast<std::uint8_t>(op) >= 8;
    if (control && payload.size() > 125) { payload = payload.substr(0, 125); }
    std::string f;
    f.push_back(static_cast<char>(0x80 | static_cast<std::uint8_t>(op)));
    if (payload.size() < 126) {
        f.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 0xFFFF) {
        f.push_back(126);
        f.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        f.push_back(static_cast<char>(payload.size() & 0xFF));
    } else {
        f.push_back(127);
        for (int i = 7; i >= 0; --i) { f.push_back(static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> (8 * i)) & 0xFF)); }
    }
    f.append(payload.data(), payload.size());
    return f;
}

std::string encodeClose(std::uint16_t code, std::string_view reason) {
    std::string p;
    p.push_back(static_cast<char>((code >> 8) & 0xFF));
    p.push_back(static_cast<char>(code & 0xFF));
    p.append(reason.substr(0, 123));
    return encodeFrame(Opcode::Close, p);
}

bool isValidUtf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        std::size_t n;
        std::uint32_t cp;
        if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 0x1Fu; }
        else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0Fu; }
        else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07u; }
        else { return false; }
        if (i + n >= s.size()) { return false; }
        for (std::size_t k = 1; k <= n; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { return false; }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (n == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) { return false; }
        if (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) { return false; }
        i += n + 1;
    }
    return true;
}

bool FrameDecoder::fail(std::uint16_t code, const char* why) {
    code_ = code;
    text_ = why;
    buf_.clear();
    assembled_.clear();
    return false;
}

void FrameDecoder::feed(const char* data, std::size_t n) {
    if (code_ != 0) { return; }
    buf_.append(data, n);
}

bool FrameDecoder::next(Message& out) {
    while (code_ == 0) {
        if (buf_.size() < 2) { return false; }
        const unsigned char b0 = static_cast<unsigned char>(buf_[0]);
        const unsigned char b1 = static_cast<unsigned char>(buf_[1]);
        const bool fin = (b0 & 0x80) != 0;
        if ((b0 & 0x70) != 0) { return fail(kCloseProtocolError, "reserved bits set"); }
        const unsigned opc = b0 & 0x0Fu;
        const bool control = opc >= 8;
        if ((opc > 2 && opc < 8) || opc > 10) { return fail(kCloseProtocolError, "reserved opcode"); }
        if ((b1 & 0x80) == 0) { return fail(kCloseProtocolError, "client frame not masked"); }
        std::uint64_t len = b1 & 0x7Fu;
        std::size_t hdr = 2;
        if (len == 126) {
            if (buf_.size() < 4) { return false; }
            len = (static_cast<std::uint64_t>(static_cast<unsigned char>(buf_[2])) << 8) | static_cast<unsigned char>(buf_[3]);
            hdr = 4;
            if (len < 126) { return fail(kCloseProtocolError, "non-minimal length"); }
        } else if (len == 127) {
            if (buf_.size() < 10) { return false; }
            len = 0;
            for (int i = 0; i < 8; ++i) { len = (len << 8) | static_cast<unsigned char>(buf_[2 + static_cast<std::size_t>(i)]); }
            hdr = 10;
            if ((len >> 63) != 0 || len <= 0xFFFF) { return fail(kCloseProtocolError, "invalid length"); }
        }
        if (control && (!fin || len > 125)) { return fail(kCloseProtocolError, "bad control frame"); }
        if (len > maxMessage_ || (!control && assembled_.size() + len > maxMessage_)) { return fail(kCloseMessageTooBig, "message too big"); }
        if (buf_.size() < hdr + 4 + static_cast<std::size_t>(len)) { return false; }
        unsigned char mask[4];
        std::memcpy(mask, buf_.data() + hdr, 4);
        std::string payload(buf_.data() + hdr + 4, static_cast<std::size_t>(len));
        for (std::size_t i = 0; i < payload.size(); ++i) { payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i & 3]); }
        buf_.erase(0, hdr + 4 + static_cast<std::size_t>(len));
        if (control) {
            out.op = static_cast<Opcode>(opc);
            out.payload = std::move(payload);
            if (out.op == Opcode::Close && out.payload.size() == 1) { return fail(kCloseProtocolError, "bad close payload"); }
            return true;
        }
        if (opc == 0) {
            if (!assembling_) { return fail(kCloseProtocolError, "continuation without a start"); }
            assembled_ += payload;
        } else {
            if (assembling_) { return fail(kCloseProtocolError, "new message inside a fragmented one"); }
            assembledOp_ = static_cast<Opcode>(opc);
            assembled_ = std::move(payload);
            assembling_ = true;
        }
        if (fin) {
            assembling_ = false;
            if (assembledOp_ == Opcode::Text && !isValidUtf8(assembled_)) { return fail(kCloseInvalidPayload, "text is not UTF-8"); }
            out.op = assembledOp_;
            out.payload = std::move(assembled_);
            assembled_.clear();
            return true;
        }
    }
    return false;
}

}  // namespace cascade::net::ws
