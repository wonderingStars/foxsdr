// kiwisdr_fake_server.hpp - a scripted stand-in for a KiwiSDR receiver, on a
// real loopback socket, for the KiwiSDR client tests.
//
// WHAT IT IS. A small WebSocket server written for the tests: it listens on
// 127.0.0.1 on a port the OS chooses, completes the RFC 6455 upgrade
// (httplib's own accept-key function does the SHA-1 and base64), and then runs
// a SCRIPT on a thread of its own for each connection. The script says what
// the "receiver" does: send frames in any chunking (including a frame split
// across several TCP segments or sent as several WebSocket fragments), say
// nothing, send a hostile length, drop the connection abruptly. Meanwhile a
// helper thread reads what the client sends (masked text frames) and keeps it,
// with arrival times, for the test to inspect. Modelled on
// tests/iiod_fake_server.hpp.
//
// WHERE THE BYTES COME FROM, AND WHAT THIS CANNOT PROVE. The frames it sends
// are built from the same outline the client was written from
// (src/source/kiwisdr_protocol.hpp): "SND" + flags + a little-endian sequence
// + a big-endian S-meter + audio. A test that agrees with the client therefore
// proves the client is SELF-CONSISTENT with that outline, not that a real
// KiwiSDR sends these bytes. Nothing here has talked to a real receiver.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#if !defined(_WIN32)
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#endif
#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace kiwifake {

#if defined(_WIN32)
using Sock = SOCKET;
constexpr Sock kBadSock = INVALID_SOCKET;
inline void closeSock(Sock s) {
    if (s != kBadSock) { ::closesocket(s); }
}
inline void shutSock(Sock s) { ::shutdown(s, SD_BOTH); }
#else
using Sock = int;
constexpr Sock kBadSock = -1;
inline void closeSock(Sock s) {
    if (s != kBadSock) { ::close(s); }
}
inline void shutSock(Sock s) { ::shutdown(s, SHUT_RDWR); }
#endif

using Clock = std::chrono::steady_clock;

struct Command {
    std::string text;
    Clock::time_point at;
};

// One accepted connection, as the script sees it.
class Conn {
public:
    Conn(Sock s, const std::atomic<bool>& stopping) : s_(s), stopping_(stopping) {}
    ~Conn() {
        if (reader_.joinable()) {
            shutSock(s_);
            reader_.join();
        }
    }

    std::string path;  // the request path of the upgrade

    // Called on the helper thread for every text frame the client sends. Set it
    // BEFORE upgrade().
    std::function<void(const Command&)> onCommand;

    // Reads the HTTP upgrade request. answer = false: read it and say nothing
    // (a receiver that accepts TCP and then stalls the handshake). Starts the
    // helper that records the client's text frames.
    bool upgrade(bool answer = true) {
        std::string req;
        char buf[512];
        while (req.find("\r\n\r\n") == std::string::npos) {
            if (req.size() > 16384) { return false; }
            const int n = ::recv(s_, buf, sizeof buf, 0);
            if (n <= 0) { return false; }
            req.append(buf, static_cast<std::size_t>(n));
        }
        const std::size_t eol = req.find("\r\n");
        const std::string line = req.substr(0, eol);
        const std::size_t a = line.find(' ');
        const std::size_t b = line.find(' ', a + 1);
        if (line.compare(0, 4, "GET ") != 0 || a == std::string::npos || b == std::string::npos) {
            return false;
        }
        path = line.substr(a + 1, b - a - 1);
        std::string key;
        std::size_t pos = 0;
        while (pos < req.size()) {
            std::size_t e = req.find("\r\n", pos);
            if (e == std::string::npos) { break; }
            std::string h = req.substr(pos, e - pos);
            pos = e + 2;
            std::string low = h;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (low.compare(0, 18, "sec-websocket-key:") == 0) {
                key = h.substr(18);
                const std::size_t f = key.find_first_not_of(" \t");
                key = f == std::string::npos ? std::string() : key.substr(f);
            }
        }
        if (key.empty()) { return false; }
        if (answer) {
            const std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                     "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                                     httplib::detail::websocket_accept_key(key) + "\r\n\r\n";
            if (!sendRaw(resp)) { return false; }
        }
        reader_ = std::thread([this] { readClient(); });
        return true;
    }

    // --- sending ---------------------------------------------------------------

    bool sendRaw(const std::string& bytes) {
        std::size_t off = 0;
        while (off < bytes.size()) {
            const int n = ::send(s_, bytes.data() + off, static_cast<int>(bytes.size() - off), 0);
            if (n <= 0) { return false; }
            off += static_cast<std::size_t>(n);
        }
        return true;
    }

    // The bytes of one WebSocket frame (server frames are not masked).
    static std::string frameBytes(std::uint8_t opcode, const std::string& payload, bool fin = true) {
        std::string f;
        f.push_back(static_cast<char>((fin ? 0x80 : 0x00) | opcode));
        const std::uint64_t n = payload.size();
        if (n < 126) {
            f.push_back(static_cast<char>(n));
        } else if (n <= 0xFFFF) {
            f.push_back(126);
            f.push_back(static_cast<char>(n >> 8));
            f.push_back(static_cast<char>(n & 0xFF));
        } else {
            f.push_back(127);
            for (int i = 7; i >= 0; --i) { f.push_back(static_cast<char>((n >> (8 * i)) & 0xFF)); }
        }
        f += payload;
        return f;
    }

    bool sendBinary(const std::string& payload) { return sendRaw(frameBytes(0x2, payload)); }
    bool sendMsg(const std::string& text) { return sendBinary("MSG " + text); }

    // The payload of one SND message (tag, header, audio).
    static std::string sndPayload(std::uint8_t flags, std::uint32_t seq, std::uint16_t smeter,
                                  const std::string& audio) {
        std::string p = "SND";
        p.push_back(static_cast<char>(flags));
        for (int i = 0; i < 4; ++i) { p.push_back(static_cast<char>((seq >> (8 * i)) & 0xFF)); }
        p.push_back(static_cast<char>(smeter >> 8));
        p.push_back(static_cast<char>(smeter & 0xFF));
        return p + audio;
    }
    bool sendSnd(std::uint8_t flags, std::uint32_t seq, std::uint16_t smeter, const std::string& audio) {
        return sendBinary(sndPayload(flags, seq, smeter, audio));
    }

    // Sends `bytes` in TCP writes of the given sizes (cycled), pausing between
    // them so each is its own segment.
    bool sendChunked(const std::string& bytes, const std::vector<std::size_t>& sizes, int pauseMs = 3) {
        std::size_t off = 0;
        std::size_t k = 0;
        while (off < bytes.size()) {
            const std::size_t want = sizes.empty() ? bytes.size() : sizes[k++ % sizes.size()];
            const std::size_t n = std::min(want == 0 ? std::size_t(1) : want, bytes.size() - off);
            if (!sendRaw(bytes.substr(off, n))) { return false; }
            off += n;
            if (off < bytes.size() && !sleep(pauseMs)) { return false; }
        }
        return true;
    }

    // One message as WebSocket fragments (binary start, continuations, FIN last).
    bool sendFragmented(const std::string& payload, std::size_t pieces) {
        const std::size_t step = std::max<std::size_t>(1, payload.size() / std::max<std::size_t>(1, pieces));
        std::size_t off = 0;
        bool first = true;
        while (off < payload.size()) {
            const std::size_t n = std::min(step, payload.size() - off);
            const bool last = off + n >= payload.size();
            if (!sendRaw(frameBytes(first ? 0x2 : 0x0, payload.substr(off, n), last))) { return false; }
            // A ping between the fragments is legal and must not disturb the message.
            if (!last && !sendRaw(frameBytes(0x9, "p"))) { return false; }
            first = false;
            off += n;
        }
        return true;
    }

    // --- waiting -------------------------------------------------------------------

    // False once the server is stopping or the client has gone.
    bool sleep(int ms) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < end) {
            if (stopping_.load() || peerGone()) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return !(stopping_.load() || peerGone());
    }

    bool peerGone() const { return gone_.load(); }

    std::vector<Command> commands() const {
        std::lock_guard<std::mutex> lk(m_);
        return cmds_;
    }
    std::size_t commandCount() const {
        std::lock_guard<std::mutex> lk(m_);
        return cmds_.size();
    }
    bool waitCommands(std::size_t n, int timeoutMs) {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return cmds_.size() >= n; });
    }
    int unmaskedClientFrames() const { return unmasked_.load(); }

    // --- ending ----------------------------------------------------------------------

    // Abrupt: a reset, no close frame.
    void reset() {
        linger l{};
        l.l_onoff = 1;
        l.l_linger = 0;
        ::setsockopt(s_, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof l);
        shutSock(s_);
    }
    // Orderly: a close frame, then the end of the stream.
    void goodbye() {
        sendRaw(frameBytes(0x8, std::string("\x03\xE8", 2)));
        shutSock(s_);
    }

private:
    void readClient() {
        std::string buf;
        char tmp[1024];
        for (;;) {
            const int n = ::recv(s_, tmp, sizeof tmp, 0);
            if (n <= 0) { break; }
            buf.append(tmp, static_cast<std::size_t>(n));
            for (;;) {
                if (buf.size() < 2) { break; }
                const auto b0 = static_cast<std::uint8_t>(buf[0]);
                const auto b1 = static_cast<std::uint8_t>(buf[1]);
                const bool masked = (b1 & 0x80) != 0;
                std::uint64_t len = b1 & 0x7F;
                std::size_t hdr = 2;
                if (len == 126) {
                    if (buf.size() < 4) { break; }
                    len = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(buf[2])) << 8) |
                          static_cast<std::uint8_t>(buf[3]);
                    hdr = 4;
                } else if (len == 127) {
                    if (buf.size() < 10) { break; }
                    len = 0;
                    for (int i = 0; i < 8; ++i) { len = (len << 8) | static_cast<std::uint8_t>(buf[2 + i]); }
                    hdr = 10;
                }
                const std::size_t maskLen = masked ? 4 : 0;
                if (buf.size() < hdr + maskLen + len) { break; }
                std::string payload = buf.substr(hdr + maskLen, static_cast<std::size_t>(len));
                if (masked) {
                    for (std::size_t i = 0; i < payload.size(); ++i) {
                        payload[i] = static_cast<char>(payload[i] ^ buf[hdr + (i % 4)]);
                    }
                } else {
                    ++unmasked_;
                }
                const std::uint8_t op = b0 & 0x0F;
                buf.erase(0, hdr + maskLen + static_cast<std::size_t>(len));
                if (op == 0x1) {
                    Command c{payload, Clock::now()};
                    {
                        std::lock_guard<std::mutex> lk(m_);
                        cmds_.push_back(c);
                        cv_.notify_all();
                    }
                    if (onCommand) { onCommand(c); }
                } else if (op == 0x8) {
                    gone_ = true;
                }
            }
        }
        gone_ = true;
    }

    Sock s_;
    const std::atomic<bool>& stopping_;
    std::thread reader_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::vector<Command> cmds_;
    std::atomic<bool> gone_{false};
    std::atomic<int> unmasked_{0};
};

// The listener. `script` runs once per accepted connection, on its own thread,
// with the connection's index (0, 1, ...). When it returns the connection is
// closed (orderly TCP close, no close frame) unless it already ended it.
class FakeKiwi {
public:
    using Script = std::function<void(Conn&, int)>;

    explicit FakeKiwi(Script script) : script_(std::move(script)) {
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        ok_ = listen_ != kBadSock && ::bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 &&
              ::listen(listen_, 8) == 0;
        if (ok_) {
            socklen_t len = sizeof a;
            ::getsockname(listen_, reinterpret_cast<sockaddr*>(&a), &len);
            port_ = ntohs(a.sin_port);
            acceptor_ = std::thread([this] { acceptLoop(); });
        }
    }
    ~FakeKiwi() {
        stopping_ = true;
        if (listen_ != kBadSock) { shutSock(listen_); }
        {
            std::lock_guard<std::mutex> lk(m_);
            for (Sock s : live_) { shutSock(s); }
        }
        if (acceptor_.joinable()) { acceptor_.join(); }
        for (auto& t : workers_) {
            if (t.joinable()) { t.join(); }
        }
        closeSock(listen_);
    }

    bool ok() const { return ok_; }
    std::uint16_t port() const { return port_; }
    int accepted() const { return accepted_.load(); }

private:
    void acceptLoop() {
        while (!stopping_.load()) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(listen_, &rd);
            timeval tv{0, 50000};
            const int r = ::select(static_cast<int>(listen_) + 1, &rd, nullptr, nullptr, &tv);
            if (r <= 0) { continue; }
            Sock c = ::accept(listen_, nullptr, nullptr);
            if (c == kBadSock) { continue; }
            int one = 1;
            ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
            const int index = accepted_.fetch_add(1);
            {
                std::lock_guard<std::mutex> lk(m_);
                live_.push_back(c);
            }
            workers_.emplace_back([this, c, index] {
                {
                    Conn conn(c, stopping_);
                    // The script decides whether and when to upgrade (upgrade() is
                    // part of the script so a test can stall it).
                    script_(conn, index);
                    shutSock(c);
                }
                closeSock(c);
            });
        }
    }

    Script script_;
    Sock listen_ = kBadSock;
    bool ok_ = false;
    std::uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<int> accepted_{0};
    std::thread acceptor_;
    std::vector<std::thread> workers_;
    std::mutex m_;
    std::vector<Sock> live_;
};

}  // namespace kiwifake
