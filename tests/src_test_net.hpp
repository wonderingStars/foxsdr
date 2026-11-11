// src_test_net.hpp - loopback socket helpers shared by the radio-source tests of the
// feat/radio-sources lane (SoapyRemote, SDRconnect, HPSDR): a TCP listener that runs a handler
// per connection, UDP sockets, and a scripted transport. Header only; 127.0.0.1 only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "source/iiod_client.hpp"

namespace srctest {

#if defined(_WIN32)
using Sock = SOCKET;
constexpr Sock kBadSock = INVALID_SOCKET;
inline void closeSock(Sock s) {
    if (s != kBadSock) { ::closesocket(s); }
}
inline bool startNet() {
    static const bool ok = [] {
        WSADATA d{};
        return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}
using SockLen = int;
#else
using Sock = int;
constexpr Sock kBadSock = -1;
inline void closeSock(Sock s) {
    if (s != kBadSock) { ::close(s); }
}
inline bool startNet() { return true; }
using SockLen = socklen_t;
#endif

using Bytes = std::vector<std::uint8_t>;

inline void put32(Bytes& b, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) { b.push_back(static_cast<std::uint8_t>(v >> s)); }
}
inline std::uint32_t get32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

// Waits for readability with a stop flag; false on stop or error.
inline bool waitRead(Sock s, const std::atomic<bool>& run, int ms = 50) {
    while (run.load()) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        timeval tv{};
        tv.tv_usec = ms * 1000;
#if defined(_WIN32)
        const int r = ::select(0, &rf, nullptr, nullptr, &tv);
#else
        const int r = ::select(s + 1, &rf, nullptr, nullptr, &tv);
#endif
        if (r < 0) { return false; }
        if (r > 0) { return true; }
    }
    return false;
}

// Exactly n bytes, or false on close / stop.
inline bool recvExact(Sock s, const std::atomic<bool>& run, void* dst, std::size_t n) {
    char* p = static_cast<char*>(dst);
    std::size_t got = 0;
    while (got < n) {
        if (!waitRead(s, run)) { return false; }
        const int r = ::recv(s, p + got, static_cast<int>(n - got), 0);
        if (r <= 0) { return false; }
        got += static_cast<std::size_t>(r);
    }
    return true;
}

inline bool sendExact(Sock s, const void* src, std::size_t n) {
    const char* p = static_cast<const char*>(src);
    std::size_t sent = 0;
    while (sent < n) {
        const int r = ::send(s, p + sent, static_cast<int>(n - sent), 0);
        if (r <= 0) { return false; }
        sent += static_cast<std::size_t>(r);
    }
    return true;
}

// A TCP listener on an ephemeral loopback port; each accepted connection runs `handler` on a
// thread of its own, with the run flag. stop() ends and joins everything.
class TcpServer {
public:
    using Handler = std::function<void(Sock, std::size_t /*index*/, const std::atomic<bool>&)>;
    ~TcpServer() { stop(); }

    // `port` 0: an ephemeral loopback port; otherwise that port (a test that needs a TCP and a UDP socket
    // under one number picks the number first).
    bool start(Handler h, std::uint16_t port = 0) {
        if (!startNet()) { return false; }
        handler_ = std::move(h);
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == kBadSock) { return false; }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        a.sin_port = ::htons(port);
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { return false; }
        SockLen len = sizeof(a);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ::ntohs(a.sin_port);
        if (::listen(listener_, 16) != 0) { return false; }
        run_.store(true);
        acceptor_ = std::thread([this] {
            while (run_.load()) {
                if (!waitRead(listener_, run_)) { continue; }
                const Sock c = ::accept(listener_, nullptr, nullptr);
                if (c == kBadSock) { continue; }
                const std::size_t idx = count_.fetch_add(1);
                std::lock_guard<std::mutex> lk(m_);
                workers_.emplace_back([this, c, idx] {
                    handler_(c, idx, run_);
                    closeSock(c);
                });
            }
        });
        return true;
    }
    void stop() {
        run_.store(false);
        if (acceptor_.joinable()) { acceptor_.join(); }
        std::vector<std::thread> w;
        {
            std::lock_guard<std::mutex> lk(m_);
            w.swap(workers_);
        }
        for (std::thread& t : w) {
            if (t.joinable()) { t.join(); }
        }
        if (listener_ != kBadSock) {
            closeSock(listener_);
            listener_ = kBadSock;
        }
    }
    std::uint16_t port() const { return port_; }
    std::size_t connections() const { return count_.load(); }
    const std::atomic<bool>& running() const { return run_; }

private:
    Handler handler_;
    Sock listener_ = kBadSock;
    std::uint16_t port_ = 0;
    std::atomic<bool> run_{false};
    std::atomic<std::size_t> count_{0};
    std::thread acceptor_;
    std::mutex m_;
    std::vector<std::thread> workers_;
};

// A bound UDP socket on an ephemeral loopback port.
class UdpSock {
public:
    ~UdpSock() { closeSock(s_); }
    bool open() {
        if (!startNet()) { return false; }
        s_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_ == kBadSock) { return false; }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        if (::bind(s_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { return false; }
        SockLen len = sizeof(a);
        ::getsockname(s_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ::ntohs(a.sin_port);
        return true;
    }
    std::uint16_t port() const { return port_; }
    Sock sock() const { return s_; }
    bool sendTo(std::uint16_t port, const void* d, std::size_t n) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        a.sin_port = ::htons(port);
        return ::sendto(s_, static_cast<const char*>(d), static_cast<int>(n), 0,
                        reinterpret_cast<sockaddr*>(&a), sizeof(a)) == static_cast<int>(n);
    }
    // One datagram, with the sender's port; false on stop/timeout.
    bool recvFrom(const std::atomic<bool>& run, Bytes& out, std::uint16_t& fromPort) {
        if (!waitRead(s_, run)) { return false; }
        out.resize(70000);
        sockaddr_in a{};
        SockLen len = sizeof(a);
        const int r = ::recvfrom(s_, reinterpret_cast<char*>(out.data()), static_cast<int>(out.size()), 0,
                                 reinterpret_cast<sockaddr*>(&a), &len);
        if (r < 0) { return false; }
        out.resize(static_cast<std::size_t>(r));
        fromPort = ::ntohs(a.sin_port);
        return true;
    }

private:
    Sock s_ = kBadSock;
    std::uint16_t port_ = 0;
};

// A TCP port nothing listens on (bound, read, closed): connecting to it is refused.
inline std::uint16_t deadPort() {
    startNet();
    const Sock t = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    ::bind(t, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    SockLen len = sizeof(a);
    ::getsockname(t, reinterpret_cast<sockaddr*>(&a), &len);
    const std::uint16_t p = ::ntohs(a.sin_port);
    closeSock(t);
    return p;
}

// A scripted transport: recvAll serves the bytes given, then fails as a closed connection;
// sendAll collects. Timeouts are scripted by `timeoutAtEnd`.
class ScriptTransport : public cascade::source::iiod::Transport {
public:
    explicit ScriptTransport(Bytes in = {}, bool timeoutAtEnd = false)
        : in_(std::move(in)), timeoutAtEnd_(timeoutAtEnd) {}
    bool sendAll(const void* d, std::size_t n) override {
        const std::uint8_t* p = static_cast<const std::uint8_t*>(d);
        out.insert(out.end(), p, p + n);
        return true;
    }
    bool recvAll(void* d, std::size_t n) override {
        if (pos_ + n > in_.size()) {
            err_ = timeoutAtEnd_ ? "peer stopped answering (receive timed out)" : "peer closed the connection";
            pos_ = in_.size();
            return false;
        }
        std::memcpy(d, in_.data() + pos_, n);
        pos_ += n;
        return true;
    }
    bool recvSome(void* d, std::size_t max, std::size_t& got) override {
        got = 0;
        const std::size_t n = std::min(max, in_.size() - pos_);
        if (n == 0) {
            err_ = timeoutAtEnd_ ? "peer stopped answering (receive timed out)" : "peer closed the connection";
            return false;
        }
        std::memcpy(d, in_.data() + pos_, n);
        pos_ += n;
        got = n;
        return true;
    }
    void close() override {}
    const char* lastError() const override { return err_.c_str(); }
    Bytes out;

private:
    Bytes in_;
    std::size_t pos_ = 0;
    bool timeoutAtEnd_;
    std::string err_;
};

// xorshift32, fixed seed per test.
struct Rng {
    std::uint32_t s;
    explicit Rng(std::uint32_t seed) : s(seed ? seed : 1u) {}
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
};

}  // namespace srctest
