// websocket_client.hpp - the pieces of a WebSocket CLIENT (RFC 6455) as plain functions and
// small classes with no sockets in them: SHA-1 and Base64 for the opening handshake, the
// handshake request and its check, frame encoding (client frames are masked), and an incremental
// frame parser for what the server sends.
//
// PROVENANCE. RFC 6455 (https://www.rfc-editor.org/rfc/rfc6455, IETF, freely readable): section
// 1.3 (the handshake and its worked example: key "dGhlIHNhbXBsZSBub25jZQ==" gives accept
// "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", the GUID 258EAFA5-E914-47DA-95CA-C5AB0DC85B11), section 5.2
// (the frame layout), 5.3 (masking), 5.4 (fragmentation), 5.5 (control frames). SHA-1 is FIPS
// 180-4 / RFC 3174; Base64 is RFC 4648. All code here is this project's own; no library was
// consulted. Not implemented, on purpose: extensions and compression (none is offered in the
// handshake, so a server that uses them is refused), subprotocols, TLS (the SDRconnect API as
// documented is a plain ws:// service on the local network).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cascade::source::ws {

std::array<std::uint8_t, 20> sha1(const std::uint8_t* data, std::size_t n);
std::string base64(const std::uint8_t* data, std::size_t n);

// The opening request for `host:port` and path, with the 16-byte nonce given (base64 of it is
// the Sec-WebSocket-Key). Ends with the blank line.
std::string handshakeRequest(const std::string& host, std::uint16_t port, const std::string& path,
                             const std::array<std::uint8_t, 16>& nonce);

// What a response must be: status 101, "Upgrade: websocket", "Connection: Upgrade" (header names
// and those values case-insensitive), Sec-WebSocket-Accept equal to base64(sha1(key + GUID)),
// and no Sec-WebSocket-Extensions or -Protocol header (none was asked for). `response` is the
// header block up to and including the blank line. Empty string when acceptable, else a
// sentence.
std::string checkHandshakeResponse(const std::string& response, const std::string& key);

enum Opcode : std::uint8_t { kContinuation = 0, kText = 1, kBinary = 2, kClose = 8, kPing = 9, kPong = 10 };

// A client frame, FIN set, masked with `mask`.
std::vector<std::uint8_t> encodeClientFrame(Opcode op, const std::uint8_t* payload, std::size_t n,
                                            const std::array<std::uint8_t, 4>& mask);

struct Message {
    Opcode op = kText;   // kText, kBinary, kClose, kPing or kPong (a reassembled data message
                         // carries the opcode of its first fragment)
    std::vector<std::uint8_t> payload;
};

// Incremental server-frame parser. feed() takes bytes as they arrive (any split) and queues
// complete messages; next() pops them. A protocol violation makes failed() true with a sentence
// and stops the parser: a masked server frame, reserved bits set, an unknown opcode, a fragmented
// or oversized control frame, a continuation with nothing to continue, a data frame while a
// fragmented message is open, a length over `maxMessage`, a 64-bit length with the top bit set.
class FrameParser {
public:
    explicit FrameParser(std::size_t maxMessage = 4u << 20) : maxMessage_(maxMessage) {}
    void feed(const std::uint8_t* data, std::size_t n);
    bool next(Message& out);
    bool failed() const { return failed_; }
    const std::string& error() const { return error_; }

private:
    void fail(const std::string& why) { failed_ = true; error_ = why; }
    void parse();
    std::size_t maxMessage_;
    std::vector<std::uint8_t> buf_;
    std::size_t pos_ = 0;
    bool failed_ = false;
    std::string error_;
    bool fragmenting_ = false;
    Opcode fragOp_ = kText;
    std::vector<std::uint8_t> frag_;
    std::vector<Message> ready_;
    std::size_t readyPos_ = 0;
};

}  // namespace cascade::source::ws
