// netmap_stand_in.hpp - loopback stand-in servers for the webhook and MCP tests. Everything here listens on 127.0.0.1 only and is
// owned by the test that creates it; no outside service is ever contacted.
//
//  * HttpStandIn: a cpp-httplib server whose behaviour per request (status, delay, body, headers) is set by a callback; it records every
//    request (method, path, body, content type, user agent, arrival time).
//  * RawStandIn: a bare TCP listener for faults an HTTP library cannot produce: accept and close at once, read the request then close
//    without answering, answer with garbage, accept and never answer (the receiver is "slow" forever).
//  * unusedPort(): a loopback port with nothing listening (connection refused).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using NetmapSocket = SOCKET;
static constexpr NetmapSocket kNetmapBadSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using NetmapSocket = int;
static constexpr NetmapSocket kNetmapBadSocket = -1;
#endif

#if !defined(_WIN32)
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#endif
#include <httplib.h>

namespace netmap {

inline void closeSocket(NetmapSocket s) {
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
}

struct Recorded {
    std::string method, path, body, contentType, userAgent;
    std::chrono::steady_clock::time_point at;
};

class HttpStandIn {
public:
    // Called for every request after it is recorded; `index` counts requests from 0. Default: 200 with "ok".
    using Behaviour = std::function<void(int index, const httplib::Request&, httplib::Response&)>;

    explicit HttpStandIn(int firstPort = 18800) : firstPort_(firstPort) {}
    ~HttpStandIn() { stop(); }

    bool start(Behaviour b = nullptr) {
        behaviour_ = b ? b : [](int, const httplib::Request&, httplib::Response& res) { res.status = 200; res.set_content("ok", "text/plain"); };
        auto handler = [this](const httplib::Request& req, httplib::Response& res) {
            int idx;
            {
                std::lock_guard<std::mutex> lk(m_);
                idx = static_cast<int>(rec_.size());
                rec_.push_back({req.method, req.path, req.body, req.get_header_value("Content-Type"), req.get_header_value("User-Agent"),
                                std::chrono::steady_clock::now()});
            }
            behaviour_(idx, req, res);
        };
        svr_.Post(".*", handler);
        svr_.Get(".*", handler);
        svr_.Put(".*", handler);
        svr_.Delete(".*", handler);
        svr_.Options(".*", handler);
        for (int p = firstPort_; p < firstPort_ + 90; ++p)
            if (svr_.bind_to_port("127.0.0.1", p)) { port_ = p; break; }
        if (port_ == 0) return false;
        thread_ = std::thread([this] { svr_.listen_after_bind(); });
        svr_.wait_until_ready();
        return true;
    }

    void stop() {
        if (thread_.joinable()) {
            svr_.stop();
            thread_.join();
        }
    }

    int port() const { return port_; }
    std::string url(const std::string& path = "/hook") const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
    std::vector<Recorded> requests() const {
        std::lock_guard<std::mutex> lk(m_);
        return rec_;
    }
    std::size_t count() const {
        std::lock_guard<std::mutex> lk(m_);
        return rec_.size();
    }

private:
    int firstPort_;
    int port_ = 0;
    httplib::Server svr_;
    Behaviour behaviour_;
    std::thread thread_;
    mutable std::mutex m_;
    std::vector<Recorded> rec_;
};

class RawStandIn {
public:
    enum class Mode { CloseImmediately, ReadRequestThenClose, GarbageThenClose, Blackhole };

    explicit RawStandIn(Mode mode, int firstPort = 18900) : mode_(mode), firstPort_(firstPort) {}
    ~RawStandIn() { stop(); }

    bool start() {
#if defined(_WIN32)
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_ == kNetmapBadSocket) return false;
        for (int p = firstPort_; p < firstPort_ + 90; ++p) {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(static_cast<unsigned short>(p));
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (::bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) { port_ = p; break; }
        }
        if (port_ == 0 || ::listen(listen_, 16) != 0) return false;
        running_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        closeSocket(listen_);
        if (thread_.joinable()) thread_.join();
        std::lock_guard<std::mutex> lk(m_);
        for (NetmapSocket s : held_) closeSocket(s);
        held_.clear();
    }

    int port() const { return port_; }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/hook"; }
    int accepted() const { return accepted_; }
    void setMode(Mode m) { mode_ = m; }

private:
    void loop() {
        while (running_) {
            sockaddr_in peer{};
#if defined(_WIN32)
            int len = sizeof peer;
#else
            socklen_t len = sizeof peer;
#endif
            NetmapSocket c = ::accept(listen_, reinterpret_cast<sockaddr*>(&peer), &len);
            if (c == kNetmapBadSocket) break;
            ++accepted_;
            const Mode mode = mode_;
            if (mode == Mode::CloseImmediately) { closeSocket(c); continue; }
            if (mode == Mode::Blackhole) {
                std::lock_guard<std::mutex> lk(m_);
                held_.push_back(c);
                continue;
            }
            // read the request headers (and the little body that follows) with a short time-out
#if defined(_WIN32)
            DWORD tv = 1000;
            ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof tv);
#endif
            std::string got;
            char buf[2048];
            for (int i = 0; i < 20 && got.find("\r\n\r\n") == std::string::npos; ++i) {
                const int n = ::recv(c, buf, sizeof buf, 0);
                if (n <= 0) break;
                got.append(buf, static_cast<std::size_t>(n));
            }
            if (mode == Mode::GarbageThenClose) {
                const char* junk = "NOT-HTTP \x01\x02 garbage\r\n\r\n";
                ::send(c, junk, static_cast<int>(std::strlen(junk)), 0);
            }
            closeSocket(c);
        }
    }

    std::atomic<Mode> mode_;
    int firstPort_;
    int port_ = 0;
    NetmapSocket listen_ = kNetmapBadSocket;
    std::atomic<bool> running_{false};
    std::atomic<int> accepted_{0};
    std::thread thread_;
    std::mutex m_;
    std::vector<NetmapSocket> held_;
};

// A loopback port nothing listens on (found by binding and releasing one).
inline int unusedPort() {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    NetmapSocket s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = 0;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
#if defined(_WIN32)
    int len = sizeof a;
#else
    socklen_t len = sizeof a;
#endif
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    const int port = ntohs(a.sin_port);
    closeSocket(s);
    return port;
}

}  // namespace netmap
