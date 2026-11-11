// net_test_support.hpp - loopback client / stand-in-server helpers shared by the network output tests (test_tcp_fanout, test_aircraft_out,
// test_ais_out, test_aprs_is, test_iq_out). Everything talks to 127.0.0.1; nothing here can reach an outside service.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/net_sock.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace nettest {

namespace sock = cascade::net::sock;
using Clock = std::chrono::steady_clock;

inline double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// A blocking-style loopback TCP client built on the non-blocking shim.
struct Client {
    sock::Handle s = sock::kInvalid;
    std::vector<std::uint8_t> got;
    bool eof = false;

    bool connect(std::uint16_t port, int timeoutMs = 2000) {
        std::uint32_t ip = 0;
        sock::parseIpv4("127.0.0.1", ip);
        s = sock::beginConnectTcp(ip, port);
        if (s == sock::kInvalid) { return false; }
        if (!sock::waitConnect(s, timeoutMs)) {
            sock::closeHandle(s);
            s = sock::kInvalid;
            return false;
        }
        return true;
    }
    // Reads whatever arrives within `ms` (until quiet for that long); returns bytes added.
    std::size_t pump(int ms) {
        std::size_t added = 0;
        const auto t0 = Clock::now();
        std::uint8_t buf[65536];
        while (s != sock::kInvalid && !eof && msSince(t0) < ms) {
            sock::PollItem it;
            it.s = s;
            it.want = sock::kReadable;
            const int left = ms - static_cast<int>(msSince(t0));
            if (sock::pollMany(&it, 1, left > 0 ? left : 0) <= 0) { break; }
            const long r = sock::recvSome(s, buf, sizeof(buf));
            if (r > 0) {
                got.insert(got.end(), buf, buf + r);
                added += static_cast<std::size_t>(r);
            } else if (r == 0) {
                eof = true;
            } else if (!sock::wouldBlock()) {
                eof = true;
            }
        }
        return added;
    }
    // Reads until `n` bytes are in `got`, EOF, or `ms` pass.
    bool waitBytes(std::size_t n, int ms) {
        const auto t0 = Clock::now();
        while (got.size() < n && !eof && msSince(t0) < ms) { pump(20); }
        return got.size() >= n;
    }
    bool waitEof(int ms) {
        const auto t0 = Clock::now();
        while (!eof && msSince(t0) < ms) { pump(20); }
        return eof;
    }
    long sendBytes(const void* d, std::size_t n) { return sock::sendSome(s, d, n); }
    void close() {
        sock::closeHandle(s);
        s = sock::kInvalid;
    }
    ~Client() { close(); }
};

// A one-shot stand-in TCP server: accepts connections on an ephemeral loopback port, records every byte received per connection, can send
// scripted bytes, and can drop the connection. Used by the APRS-IS tests.
class StandInServer {
public:
    struct Conn {
        sock::Handle s = sock::kInvalid;
        std::string received;
    };

    bool start() {
        std::string err;
        std::uint32_t ip = 0;
        sock::parseIpv4("127.0.0.1", ip);
        listener_ = sock::listenTcp(ip, 0, 8, err);
        if (listener_ == sock::kInvalid) { return false; }
        port_ = sock::boundPort(listener_);
        stop_.store(false);
        th_ = std::thread([this] { loop(); });
        return true;
    }
    ~StandInServer() { stop(); }
    void stop() {
        if (!th_.joinable()) { return; }
        stop_.store(true);
        th_.join();
        for (Conn& c : conns_) { sock::closeHandle(c.s); }
        conns_.clear();
        sock::closeHandle(listener_);
        listener_ = sock::kInvalid;
    }
    std::uint16_t port() const { return port_; }
    int accepted() const { return accepted_.load(); }
    // Text received from connection `i` (0 = the first), all of it so far.
    std::string received(std::size_t i) {
        std::lock_guard<std::mutex> g(m_);
        return i < log_.size() ? log_[i] : std::string();
    }
    // Queues bytes to send to connection `i`.
    void sendTo(std::size_t i, const std::string& data) {
        std::lock_guard<std::mutex> g(m_);
        if (i < outq_.size()) { outq_[i] += data; }
    }
    void dropConn(std::size_t i) {
        std::lock_guard<std::mutex> g(m_);
        if (i < drop_.size()) { drop_[i] = true; }
    }
    // Sends this text to every connection as it arrives (before any other output): the stand-in's greeting.
    void setGreeting(const std::string& g) {
        std::lock_guard<std::mutex> gg(m_);
        greeting_ = g;
    }
    // Refuse (accept then close at once) the next `n` connections.
    void refuseNext(int n) { refuse_.store(n); }

private:
    void loop() {
        std::uint8_t buf[4096];
        while (!stop_.load()) {
            sock::Handle h = sock::acceptOne(listener_);
            if (h != sock::kInvalid) {
                if (refuse_.load() > 0) {
                    refuse_.fetch_sub(1);
                    sock::closeHandle(h);
                    accepted_.fetch_add(1);
                } else {
                    std::lock_guard<std::mutex> g(m_);
                    Conn c;
                    c.s = h;
                    conns_.push_back(c);
                    log_.emplace_back();
                    outq_.emplace_back(greeting_);
                    drop_.push_back(false);
                    accepted_.fetch_add(1);
                }
            }
            {
                std::lock_guard<std::mutex> g(m_);
                for (std::size_t i = 0; i < conns_.size(); ++i) {
                    Conn& c = conns_[i];
                    if (c.s == sock::kInvalid) { continue; }
                    if (drop_[i]) {
                        sock::closeHandle(c.s);
                        c.s = sock::kInvalid;
                        continue;
                    }
                    if (!outq_[i].empty()) {
                        const long r = sock::sendSome(c.s, outq_[i].data(), outq_[i].size());
                        if (r > 0) { outq_[i].erase(0, static_cast<std::size_t>(r)); }
                    }
                    const long r = sock::recvSome(c.s, buf, sizeof(buf));
                    if (r > 0) {
                        log_[i].append(reinterpret_cast<char*>(buf), static_cast<std::size_t>(r));
                    } else if (r == 0) {
                        sock::closeHandle(c.s);
                        c.s = sock::kInvalid;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    sock::Handle listener_ = sock::kInvalid;
    std::uint16_t port_ = 0;
    std::thread th_;
    std::atomic<bool> stop_{true};
    std::atomic<int> accepted_{0};
    std::atomic<int> refuse_{0};
    std::mutex m_;
    std::vector<Conn> conns_;
    std::vector<std::string> log_, outq_;
    std::vector<bool> drop_;
    std::string greeting_;
};

// Process CPU seconds (user + kernel). The Windows figure ticks in ~15.6 ms steps: a bound, not a precise number.
inline double processCpuSeconds() {
#if defined(_WIN32)
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) { return 0.0; }
    auto v = [](const FILETIME& f) { return static_cast<double>((static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7; };
    return v(k) + v(u);
#else
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) + 1e-6 * static_cast<double>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
#endif
}

}  // namespace nettest
