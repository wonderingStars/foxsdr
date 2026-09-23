// http_post_stub.hpp - the loopback HTTP stub the feature-request and
// problem-report tests POST to. Moved here VERBATIM from
// tests/test_feature_request.cpp when tests/test_problem_report.cpp became
// its second user (2026-09-23); the only change is the route, which is now a
// constructor argument defaulting to the feature request's.
//
// Header-only and in an anonymous namespace: each test binary that includes
// it is one translation unit and gets its own copy.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_TESTS_HTTP_POST_STUB_HPP
#define CASCADE_TESTS_HTTP_POST_STUB_HPP

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>

#include <windows.h>

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <condition_variable>

// Must match every other TU in the program that includes httplib.h on
// non-Windows (plugin_repo.cpp, crash_upload.cpp, telemetry.cpp,
// web_server.cpp, test_crash_upload.cpp) - or a client/server layout mismatch
// reproduces the SEGV documented in web_server.cpp's own comment on this.
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>
#endif

namespace {

// ---------------------------------------------------------------------------
// A local HTTP stub - same discipline as test_crash_upload.cpp's StubServer:
// read headers to the blank line, read exactly Content-Length body bytes,
// reply, then close cleanly - a fake that closes without draining sends an
// RST and flakes.
// ---------------------------------------------------------------------------
#if defined(_WIN32)

class StubServer {
public:
    enum class Mode { Accept200, BadRequest400, RateLimit429, Hang, Refuse };

    // The route this stub answers on. The Windows variant accepts any path
    // and only uses this to build url(); the POSIX variant registers it.
    explicit StubServer(std::string path = "/api/feature-request") : path_(std::move(path)) {}

    bool start(Mode mode) {
        mode_ = mode;
        WSADATA wsa{};
        ::WSAStartup(MAKEWORD(2, 2), &wsa);
        listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_ == INVALID_SOCKET) { return false; }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            return false;
        }
        int len = sizeof(addr);
        ::getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ::ntohs(addr.sin_port);
        if (mode_ == Mode::Refuse) {
            ::closesocket(listen_);
            listen_ = INVALID_SOCKET;
            return true;
        }
        if (::listen(listen_, 8) != 0) { return false; }
        run_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() {
        run_ = false;
        if (listen_ != INVALID_SOCKET) {
            ::closesocket(listen_);
            listen_ = INVALID_SOCKET;
        }
        if (thread_.joinable()) { thread_.join(); }
        for (SOCKET s : held_) { ::closesocket(s); }
        held_.clear();
    }

    ~StubServer() { stop(); }

    int port() const { return port_; }
    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + path_;
    }
    int connections() const { return connections_.load(); }
    std::vector<std::string> bodies() {
        std::lock_guard<std::mutex> lk(mu_);
        return bodies_;
    }
    std::vector<std::string> requests() {
        std::lock_guard<std::mutex> lk(mu_);
        return requests_;
    }

private:
    void loop() {
        while (run_) {
            fd_set rd;
            FD_ZERO(&rd);
            if (listen_ == INVALID_SOCKET) { break; }
            FD_SET(listen_, &rd);
            timeval tv{0, 100 * 1000};
            const int n = ::select(0, &rd, nullptr, nullptr, &tv);
            if (n <= 0) { continue; }
            SOCKET c = ::accept(listen_, nullptr, nullptr);
            if (c == INVALID_SOCKET) { continue; }
            connections_.fetch_add(1);
            std::string req;
            char buf[4096];
            std::size_t contentLength = 0;
            std::size_t headerEnd = std::string::npos;
            while (run_) {
                const int got = ::recv(c, buf, sizeof(buf), 0);
                if (got <= 0) { break; }
                req.append(buf, buf + got);
                if (headerEnd == std::string::npos) {
                    headerEnd = req.find("\r\n\r\n");
                    if (headerEnd != std::string::npos) {
                        const std::size_t at4 = lowerFind(req, "content-length:");
                        if (at4 != std::string::npos) {
                            contentLength = static_cast<std::size_t>(
                                std::strtoull(req.c_str() + at4 + 15, nullptr, 10));
                        }
                    }
                }
                if (headerEnd != std::string::npos &&
                    req.size() >= headerEnd + 4 + contentLength) {
                    break;
                }
            }
            if (headerEnd != std::string::npos) {
                std::lock_guard<std::mutex> lk(mu_);
                requests_.push_back(req.substr(0, headerEnd));
                bodies_.push_back(req.substr(headerEnd + 4));
            }
            if (mode_ == Mode::Hang) {
                held_.push_back(c);
                continue;
            }
            std::string resp;
            if (mode_ == Mode::RateLimit429) {
                const std::string body = "{\"ok\":false,\"error\":\"slow down, please\"}";
                resp = "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 120\r\n"
                       "Content-Type: application/json\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n" + body;
            } else if (mode_ == Mode::BadRequest400) {
                const std::string body =
                    "{\"ok\":false,\"error\":\"text must be between 10 and 2000 characters\"}";
                resp = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                       "Content-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n" + body;
            } else {
                const std::string body = "{\"ok\":true}";
                resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n" + body;
            }
            ::send(c, resp.c_str(), static_cast<int>(resp.size()), 0);
            ::shutdown(c, SD_BOTH);
            ::closesocket(c);
        }
    }

    static std::size_t lowerFind(const std::string& hay, const std::string& needle) {
        std::string l;
        l.reserve(hay.size());
        for (char ch : hay) {
            l.push_back((ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch);
        }
        return l.find(needle);
    }

    std::string path_;
    Mode mode_ = Mode::Accept200;
    SOCKET listen_ = INVALID_SOCKET;
    int port_ = 0;
    std::atomic<bool> run_{false};
    std::atomic<int> connections_{0};
    std::thread thread_;
    std::mutex mu_;
    std::vector<std::string> bodies_;
    std::vector<std::string> requests_;
    std::vector<SOCKET> held_;
};

#else  // !_WIN32

class StubServer {
public:
    enum class Mode { Accept200, BadRequest400, RateLimit429, Hang, Refuse };

    // The route this stub answers on. The Windows variant accepts any path
    // and only uses this to build url(); the POSIX variant registers it.
    explicit StubServer(std::string path = "/api/feature-request") : path_(std::move(path)) {}

    bool start(Mode mode) {
        mode_ = mode;
        if (mode_ == Mode::Refuse) {
            // See test_crash_upload.cpp's identical comment: bind then
            // release a port so it is refused in microseconds rather than
            // left listening-but-unaccepted, which would hang on the READ
            // timeout instead.
            httplib::Server probe;
            port_ = probe.bind_to_any_port("127.0.0.1");
            probe.stop();
            return port_ > 0;
        }
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) { return false; }
        server_.Post(path_,
                     [this](const httplib::Request& req, httplib::Response& res) {
                         {
                             std::lock_guard<std::mutex> lk(mu_);
                             bodies_.push_back(req.body);
                             requests_.push_back(req.get_header_value("Content-Type"));
                         }
                         connections_.fetch_add(1);
                         if (mode_ == Mode::Hang) {
                             std::unique_lock<std::mutex> lk(hangMu_);
                             hangCv_.wait_for(lk, std::chrono::seconds(30),
                                              [this] { return hangStop_.load(); });
                             return;
                         }
                         if (mode_ == Mode::RateLimit429) {
                             res.status = 429;
                             res.set_header("Retry-After", "120");
                             res.set_content("{\"ok\":false,\"error\":\"slow down, please\"}",
                                            "application/json");
                         } else if (mode_ == Mode::BadRequest400) {
                             res.status = 400;
                             res.set_content(
                                 "{\"ok\":false,\"error\":\"text must be between 10 and "
                                 "2000 characters\"}",
                                 "application/json");
                         } else {
                             res.status = 200;
                             res.set_content("{\"ok\":true}", "application/json");
                         }
                     });
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
        return true;
    }

    void stop() {
        hangStop_.store(true);
        hangCv_.notify_all();
        server_.stop();
        if (thread_.joinable()) { thread_.join(); }
    }

    ~StubServer() { stop(); }

    int port() const { return port_; }
    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + path_;
    }
    int connections() const { return connections_.load(); }
    std::vector<std::string> bodies() {
        std::lock_guard<std::mutex> lk(mu_);
        return bodies_;
    }
    // On POSIX this holds the captured Content-Type, not the raw header
    // block WinHTTP's variant does - both call sites below only ever check
    // for "application/json" as a substring, which is true either way.
    std::vector<std::string> requests() {
        std::lock_guard<std::mutex> lk(mu_);
        return requests_;
    }

private:
    std::string path_;
    Mode mode_ = Mode::Accept200;
    httplib::Server server_;
    int port_ = 0;
    std::thread thread_;
    std::atomic<int> connections_{0};
    std::mutex mu_;
    std::vector<std::string> bodies_;
    std::vector<std::string> requests_;
    std::mutex hangMu_;
    std::condition_variable hangCv_;
    std::atomic<bool> hangStop_{false};
};

#endif  // _WIN32

}  // namespace

#endif  // CASCADE_TESTS_HTTP_POST_STUB_HPP
