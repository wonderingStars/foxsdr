// mcp_http.hpp - the loopback listener for net/mcp_server.hpp, on the project's vendored cpp-httplib. LOOPBACK ONLY, by construction: the
// only way to start it takes a bind address and refuses anything that is not 127.0.0.0/8, ::1 or "localhost" before opening a socket (the
// web server's own bind policy, net/web_policy.hpp, is a separate matter and is not touched). Every request goes through McpHttpPolicy
// (Host / Origin / token / method / headers checks), which is itself OFF unless the wiring lane enables it.
//
// Limits: body 65 KiB at the library level (413 before a handler runs), four worker threads, 5 s read and write time-outs, at most 8
// requests per kept-alive connection. As in every translation unit that includes httplib.h on non-Windows, CPPHTTPLIB_OPENSSL_SUPPORT is
// defined first (see net/web_server.cpp); no TLS is used or offered here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <memory>
#include <thread>
#include <string>

#if !defined(_WIN32)
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#endif
#include <httplib.h>

#include "net/mcp_server.hpp"

namespace cascade::net::mcp {

enum class ListenResult { Started, NotLoopback, BindFailed, AlreadyRunning };

// "127.x.y.z", "::1" or "localhost" (mapped to 127.0.0.1). Anything else, including "", "0.0.0.0", "::" and any LAN address, is not.
inline bool loopbackBindAddress(const std::string& addr, std::string& resolved) {
    if (addr == "localhost") { resolved = "127.0.0.1"; return true; }
    if (addr == "::1") { resolved = addr; return true; }
    if (detail::loopbackOrigin("http://" + addr) && addr.find(':') == std::string::npos && addr.find('[') == std::string::npos) {
        resolved = addr;
        return true;
    }
    return false;
}

class McpHttpServer {
public:
    explicit McpHttpServer(McpHttpPolicy& policy) : policy_(policy) {}
    ~McpHttpServer() { stop(); }
    McpHttpServer(const McpHttpServer&) = delete;
    McpHttpServer& operator=(const McpHttpServer&) = delete;

    ListenResult start(const std::string& bindAddress, int port, const std::string& path = "/mcp") {
        if (running_) return ListenResult::AlreadyRunning;
        std::string host;
        if (!loopbackBindAddress(bindAddress, host)) return ListenResult::NotLoopback;
        if (port < 1 || port > 65535) return ListenResult::BindFailed;
        svr_ = std::make_unique<httplib::Server>();
        svr_->new_task_queue = [] { return new httplib::ThreadPool(4); };
        svr_->set_payload_max_length(66 * 1024);
        svr_->set_read_timeout(5, 0);
        svr_->set_write_timeout(5, 0);
        svr_->set_keep_alive_max_count(8);
        auto handler = [this](const httplib::Request& req, httplib::Response& res) {
            HttpRequest r;
            r.method = req.method;
            r.path = req.path;
            r.body = req.body;
            for (const auto& h : req.headers) r.headers[detail::lowerStr(h.first)] = h.second;
            const Reply rep = policy_.handle(r);
            res.status = rep.status;
            std::string contentType;
            for (const auto& h : rep.headers) {
                if (detail::lowerStr(h.first) == "content-type") contentType = h.second;
                else res.set_header(h.first, h.second);
            }
            if (rep.hasBody) res.set_content(rep.body, contentType.empty() ? "application/json" : contentType);
        };
        svr_->Post(path, handler);
        svr_->Get(path, handler);
        svr_->Put(path, handler);
        svr_->Delete(path, handler);
        svr_->Patch(path, handler);
        svr_->Options(path, handler);
        if (!svr_->bind_to_port(host, port)) { svr_.reset(); return ListenResult::BindFailed; }
        boundHost_ = host;
        port_ = port;
        running_ = true;
        thread_ = std::thread([this] { svr_->listen_after_bind(); });
        svr_->wait_until_ready();
        return ListenResult::Started;
    }

    void stop() {
        if (!running_) return;
        svr_->stop();
        if (thread_.joinable()) thread_.join();
        svr_.reset();
        running_ = false;
    }

    bool running() const { return running_; }
    int port() const { return port_; }
    const std::string& boundHost() const { return boundHost_; }

private:
    McpHttpPolicy& policy_;
    std::unique_ptr<httplib::Server> svr_;
    std::thread thread_;
    std::string boundHost_;
    int port_ = 0;
    bool running_ = false;
};

}  // namespace cascade::net::mcp
