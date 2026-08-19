// TCP transport for the CAT server. See the header for the bind policy and
// why this file is as small as it is.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/cat_server.hpp"

#include <cstdio>
#include <cstring>
#include <string>

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
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cascade::net {
namespace {

#if defined(_WIN32)
using SockLen = int;
constexpr std::intptr_t kInvalidSock = static_cast<std::intptr_t>(INVALID_SOCKET);

void closeSock(std::intptr_t s) {
    if (s != kInvalidSock) ::closesocket(static_cast<SOCKET>(s));
}

// Winsock needs an explicit startup, exactly once per process. A static does
// that without any ordering requirement on the caller.
bool ensureWinsock() {
    static const bool ok = [] {
        WSADATA d{};
        return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}
#else
using SockLen = socklen_t;
constexpr std::intptr_t kInvalidSock = -1;

void closeSock(std::intptr_t s) {
    if (s != kInvalidSock) ::close(static_cast<int>(s));
}

bool ensureWinsock() { return true; }
#endif

int lastSocketError() {
#if defined(_WIN32)
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

bool sendAll(std::intptr_t sock, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const int n =
#if defined(_WIN32)
            ::send(static_cast<SOCKET>(sock), data.data() + sent,
                   static_cast<int>(data.size() - sent), 0);
#else
            static_cast<int>(::send(static_cast<int>(sock), data.data() + sent,
                                    data.size() - sent, MSG_NOSIGNAL));
#endif
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

CatServer::~CatServer() { stop(); }

void CatServer::setStatusProvider(StatusProvider fn) { status_ = std::move(fn); }

bool CatServer::start(std::uint16_t port, bool bindAll, std::string& error) {
    error.clear();
    if (running_.load(std::memory_order_relaxed)) {
        error = "the CAT server is already running";
        return false;
    }
    if (!status_) {
        error = "the CAT server has no status provider";
        return false;
    }
    if (!ensureWinsock()) {
        error = "could not initialise the network stack";
        return false;
    }

    const std::intptr_t s =
        static_cast<std::intptr_t>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (s == kInvalidSock) {
        error = "could not create a socket";
        return false;
    }

    // Without SO_REUSEADDR a restart inside the TIME_WAIT window fails to bind,
    // which reads to a user as "the port is taken" when nothing holds it.
    int yes = 1;
    ::setsockopt(
#if defined(_WIN32)
        static_cast<SOCKET>(s),
#else
        static_cast<int>(s),
#endif
        SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    // An accept timeout, so the acceptor wakes periodically and can notice
    // that it has been asked to stop.
    //
    // This is NOT belt and braces. Closing a listening socket does not reliably
    // interrupt another thread already blocked in accept() — on Linux it simply
    // does not, and stop() would then join a thread that never returns, hanging
    // the application on exit. The timeout is what makes shutdown terminate.
#if defined(_WIN32)
    DWORD acceptTimeoutMs = 200;
    ::setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&acceptTimeoutMs),
                 sizeof(acceptTimeoutMs));
#else
    timeval acceptTimeout{};
    acceptTimeout.tv_sec = 0;
    acceptTimeout.tv_usec = 200 * 1000;
    ::setsockopt(static_cast<int>(s), SOL_SOCKET, SO_RCVTIMEO, &acceptTimeout,
                 sizeof(acceptTimeout));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    addr.sin_addr.s_addr = bindAll ? ::htonl(INADDR_ANY) : ::htonl(INADDR_LOOPBACK);

    if (::bind(
#if defined(_WIN32)
            static_cast<SOCKET>(s),
#else
            static_cast<int>(s),
#endif
            reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int err = lastSocketError();
        closeSock(s);
        // The overwhelmingly common cause is a real rigctld already running,
        // so the message says that rather than printing a bare number.
        error = "could not bind port " + std::to_string(port) + " (error " +
                std::to_string(err) +
                "); another program may already be using it — rigctld itself "
                "uses this port";
        return false;
    }
    if (::listen(
#if defined(_WIN32)
            static_cast<SOCKET>(s),
#else
            static_cast<int>(s),
#endif
            4) != 0) {
        const int err = lastSocketError();
        closeSock(s);
        error = "could not listen on port " + std::to_string(port) + " (error " +
                std::to_string(err) + ")";
        return false;
    }

    listener_ = s;
    stopping_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    acceptor_ = std::thread([this] { acceptLoop(); });
    return true;
}

void CatServer::stop() {
    if (!running_.load(std::memory_order_relaxed)) return;
    stopping_.store(true, std::memory_order_relaxed);
    // Closing the listener is what wakes the blocking accept(); there is no
    // portable way to interrupt it otherwise.
    closeSock(listener_);
    listener_ = kInvalidSock;
    if (acceptor_.joinable()) acceptor_.join();
    running_.store(false, std::memory_order_relaxed);
}

void CatServer::acceptLoop() {
    while (!stopping_.load(std::memory_order_relaxed)) {
        sockaddr_in peer{};
        SockLen len = sizeof(peer);
        const std::intptr_t c = static_cast<std::intptr_t>(::accept(
#if defined(_WIN32)
            static_cast<SOCKET>(listener_),
#else
            static_cast<int>(listener_),
#endif
            reinterpret_cast<sockaddr*>(&peer), &len));
        if (c == kInvalidSock) {
            if (stopping_.load(std::memory_order_relaxed)) break;
            continue;
        }
        if (clients_.load(std::memory_order_relaxed) >= kMaxCatClients) {
            closeSock(c);
            continue;
        }
        clients_.fetch_add(1, std::memory_order_relaxed);
        // Detached: a client thread owns its socket and nothing waits on it.
        // stop() closes the listener and the sockets drain as clients hang up.
        std::thread([this, c] {
            serveClient(c);
            clients_.fetch_sub(1, std::memory_order_relaxed);
        }).detach();
    }
}

void CatServer::serveClient(std::intptr_t sock) {
    // FIRST, clear the receive timeout this socket INHERITED from the listener.
    //
    // The listener carries a 200 ms SO_RCVTIMEO so accept() wakes often enough
    // to notice a stop request. On Linux an accepted socket inherits that
    // option, so without this every client would time out after 200 ms of
    // silence — and since a timeout surfaces as recv() returning -1, the loop
    // below would read it as a closed connection and hang up on a perfectly
    // healthy client the moment it paused between commands. A CAT client is
    // idle almost all the time, so this made the feature useless in exactly
    // its normal state while looking fine under a burst of traffic.
#if defined(_WIN32)
    DWORD noTimeout = 0;
    ::setsockopt(static_cast<SOCKET>(sock), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&noTimeout), sizeof(noTimeout));
#else
    timeval noTimeout{};
    noTimeout.tv_sec = 0;
    noTimeout.tv_usec = 0;
    ::setsockopt(static_cast<int>(sock), SOL_SOCKET, SO_RCVTIMEO, &noTimeout,
                 sizeof(noTimeout));
#endif

    // Nagle off: these are tiny request/response exchanges and a client that
    // waits 40 ms for its frequency reads as a sluggish radio.
    int yes = 1;
    ::setsockopt(
#if defined(_WIN32)
        static_cast<SOCKET>(sock),
#else
        static_cast<int>(sock),
#endif
        IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));

    std::string pending;
    char buf[512];
    for (;;) {
        if (stopping_.load(std::memory_order_relaxed)) break;
        const int n =
#if defined(_WIN32)
            ::recv(static_cast<SOCKET>(sock), buf, static_cast<int>(sizeof(buf)), 0);
#else
            static_cast<int>(::recv(static_cast<int>(sock), buf, sizeof(buf), 0));
#endif
        if (n <= 0) break;  // peer closed, or the socket failed
        pending.append(buf, static_cast<std::size_t>(n));

        // A client that never sends a newline must not be able to grow this
        // buffer without limit.
        if (pending.size() > kMaxCatLineBytes) {
            sendAll(sock, "RPRT -1\n");
            break;
        }

        for (;;) {
            const std::size_t nl = pending.find('\n');
            if (nl == std::string::npos) break;
            const std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);

            RadioStatus snapshot = status_ ? status_() : RadioStatus{};
            const CatResult result = executeCatLine(line, snapshot);
            commands_.fetch_add(1, std::memory_order_relaxed);

            if (result.hasControl) {
                std::lock_guard<std::mutex> lk(controlsMu_);
                controls_.push_back(result.control);
            }
            if (!result.reply.empty() && !sendAll(sock, result.reply)) {
                closeSock(sock);
                return;
            }
            if (result.quit) {
                closeSock(sock);
                return;
            }
        }
    }
    closeSock(sock);
}

std::vector<ControlRequest> CatServer::takePendingControls() {
    std::lock_guard<std::mutex> lk(controlsMu_);
    std::vector<ControlRequest> out;
    out.swap(controls_);
    return out;
}

}  // namespace cascade::net
