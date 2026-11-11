// net/ws_frame.hpp - the smallest WebSocket (RFC 6455) SERVER layer the engine needs, with no sockets in it.
//
// WHY THIS EXISTS. The TCI server (net/tci_server.*) is a WebSocket server. The vendored cpp-httplib (0.53.1) does contain one
// (Server::WebSocket, ws::WebSocket), and it compiles (tests/test_ws_frame.cpp drives this server with httplib's WebSocketClient), but
// it is not what the server uses: its handler model is one blocking thread per connection with a blocking read, a send that waits on a
// client that never reads (up to the socket's write timeout, with the write mutex held), a close() that reads the stream from a second
// thread, and on Windows a listener option (SO_REUSEADDR) that lets a second program bind a port already in use. A control port that
// must drop a slow or hostile client WITHOUT waiting for it, and say "port in use" in words, is simpler to get right as its own polled
// loop over these pure pieces. httplib's WebSocketClient is what the TESTS use as an independent client, which proves this server
// against a second implementation of the RFC.
//
// WRITTEN FROM RFC 6455 (https://www.rfc-editor.org/rfc/rfc6455): section 1.3 (the opening handshake and the accept key), 4.2.1 (what a
// server checks in the request), 5.2 (the frame layout), 5.3 (client-to-server masking), 5.4 (fragmentation), 5.5 (control frames),
// 7.4 (close codes), 8.1 (text must be UTF-8). No code from another program.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_WS_FRAME_HPP
#define CASCADE_NET_WS_FRAME_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cascade::net::ws {

// SHA-1 (FIPS 180-1) of `data`: 20 raw bytes. Used only for the handshake's accept key, where RFC 6455 requires it.
std::string sha1(std::string_view data);
// Standard base64 (RFC 4648 section 4) with padding.
std::string base64Encode(std::string_view bytes);
// False for anything that is not canonical padded base64.
bool base64Decode(std::string_view text, std::string& out);
// Sec-WebSocket-Accept for a Sec-WebSocket-Key (RFC 6455 section 1.3).
std::string acceptKey(std::string_view clientKey);

// The largest request head (request line and headers) read before the handshake is refused.
inline constexpr std::size_t kMaxHandshakeBytes = 8192;

struct Handshake {
    bool ok = false;
    int status = 400;         // the HTTP status to refuse with when !ok
    std::string key;          // Sec-WebSocket-Key
    std::string path;         // the request target
    std::string error;        // why it was refused, in words (never echoes a header value)
};
// `head` is everything up to and including the blank line. Requires a GET, HTTP/1.1, "Upgrade: websocket", a Connection header that lists
// upgrade, Sec-WebSocket-Version 13 and a Sec-WebSocket-Key that is the base64 of 16 bytes (RFC 6455 section 4.2.1).
Handshake parseHandshake(std::string_view head);
std::string handshakeAcceptResponse(std::string_view clientKey);
// A refusal as an HTTP response (the connection is closed after it).
std::string handshakeRefusal(int status, std::string_view why);

enum class Opcode : std::uint8_t { Continuation = 0, Text = 1, Binary = 2, Close = 8, Ping = 9, Pong = 10 };

// One server-to-client frame (never masked, section 5.1), FIN set. A control frame's payload is cut to 125 bytes.
std::string encodeFrame(Opcode op, std::string_view payload);
// A close frame: the 2-byte status code then a reason cut to 123 bytes.
std::string encodeClose(std::uint16_t code, std::string_view reason);

// Close codes (section 7.4.1) this layer produces.
inline constexpr std::uint16_t kCloseNormal = 1000;
inline constexpr std::uint16_t kCloseGoingAway = 1001;
inline constexpr std::uint16_t kCloseProtocolError = 1002;
inline constexpr std::uint16_t kCloseUnsupportedData = 1003;
inline constexpr std::uint16_t kCloseInvalidPayload = 1007;
inline constexpr std::uint16_t kCloseMessageTooBig = 1009;
inline constexpr std::uint16_t kCloseTryAgainLater = 1013;

// True when `s` is well-formed UTF-8 (no overlongs, no surrogates, nothing above U+10FFFF).
bool isValidUtf8(std::string_view s);

struct Message {
    Opcode op = Opcode::Text;   // Text or Binary (a whole, reassembled message), Close, Ping or Pong
    std::string payload;        // a Close's payload still carries its 2-byte code
};

// The client-to-server side, incrementally: feed bytes as they arrive, pull whole messages. It buffers at most one header and one
// message (never more than `maxMessageBytes` of payload: a frame that DECLARES more is refused before its payload is read), and a
// violation is final (failed()).
class FrameDecoder {
public:
    explicit FrameDecoder(std::size_t maxMessageBytes) : maxMessage_(maxMessageBytes) {}
    void feed(const char* data, std::size_t n);
    // true: `out` is the next message. false: nothing complete yet, or failed().
    bool next(Message& out);
    bool failed() const { return code_ != 0; }
    std::uint16_t errorCode() const { return code_; }
    const std::string& errorText() const { return text_; }
    std::size_t buffered() const { return buf_.size(); }

private:
    bool fail(std::uint16_t code, const char* why);
    std::string buf_;
    std::string assembled_;      // a fragmented data message in progress
    Opcode assembledOp_ = Opcode::Text;
    bool assembling_ = false;
    std::size_t maxMessage_;
    std::uint16_t code_ = 0;
    std::string text_;
};

}  // namespace cascade::net::ws

#endif  // CASCADE_NET_WS_FRAME_HPP
