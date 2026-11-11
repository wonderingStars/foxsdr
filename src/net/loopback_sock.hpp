// net/loopback_sock.hpp - thin non-blocking IPv4 socket helpers for the ham-operations network pieces (ADIF broadcast, X-Plane UDP source,
// Antenna Genius client) and their tests. Header-only, no thread: every call returns at once, the caller decides which thread uses it.
//
// SAFETY: a socket here is bound to, or connected to, an address the CALLER passes as a host-order IPv4 number; the pieces built on it pass 127.0.0.1
// unless the user switched a non-loopback destination on deliberately (a wider bind is what raises a firewall prompt on Windows: none of this lane's
// tests binds anything but 127.0.0.1). Nothing here prints anything.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cascade::net::lo {

using Sock = std::intptr_t;
inline constexpr Sock kBad = -1;
inline constexpr std::uint32_t kLoopbackHost = 0x7F000001u;   // 127.0.0.1, host byte order

inline bool initSockets() {
#if defined(_WIN32)
    static const bool ok = [] {
        WSADATA d{};
        return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
#else
    return true;
#endif
}

// Parses a dotted IPv4 number (no name resolution, ever). False on anything else.
inline bool parseIpv4(const std::string& s, std::uint32_t& hostOrder) {
    if (s.empty() || s.size() > 15) { return false; }
    in_addr a{};
    if (::inet_pton(AF_INET, s.c_str(), &a) != 1) { return false; }
    hostOrder = ntohl(a.s_addr);
    return true;
}
inline bool isLoopbackHost(std::uint32_t hostOrder) { return (hostOrder >> 24) == 127u; }

inline void closeSock(Sock s) {
    if (s == kBad) { return; }
#if defined(_WIN32)
    ::closesocket(static_cast<SOCKET>(s));
#else
    ::close(static_cast<int>(s));
#endif
}

inline void setNonBlocking(Sock s) {
#if defined(_WIN32)
    u_long on = 1;
    ::ioctlsocket(static_cast<SOCKET>(s), FIONBIO, &on);
#else
    const int fl = ::fcntl(static_cast<int>(s), F_GETFL, 0);
    ::fcntl(static_cast<int>(s), F_SETFL, fl | O_NONBLOCK);
#endif
}

inline bool wouldBlock() {
#if defined(_WIN32)
    const int e = ::WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS;
#endif
}

inline sockaddr_in addrOf(std::uint32_t hostOrder, int port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<std::uint16_t>(port));
    a.sin_addr.s_addr = htonl(hostOrder);
    return a;
}

inline int boundPortOf(Sock s) {
    sockaddr_in a{};
#if defined(_WIN32)
    int len = sizeof(a);
    if (::getsockname(static_cast<SOCKET>(s), reinterpret_cast<sockaddr*>(&a), &len) != 0) { return -1; }
#else
    socklen_t len = sizeof(a);
    if (::getsockname(static_cast<int>(s), reinterpret_cast<sockaddr*>(&a), &len) != 0) { return -1; }
#endif
    return ntohs(a.sin_port);
}

// A non-blocking UDP socket. bindPort < 0: unbound (send only); otherwise bound to host:bindPort (0 = any free port; read it with boundPortOf).
inline Sock udpOpen(std::uint32_t bindHost, int bindPort) {
    if (!initSockets()) { return kBad; }
#if defined(_WIN32)
    const SOCKET raw = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (raw == INVALID_SOCKET) { return kBad; }
#else
    const int raw = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (raw < 0) { return kBad; }
#endif
    const Sock s = static_cast<Sock>(raw);
    if (bindPort >= 0) {
        const sockaddr_in a = addrOf(bindHost, bindPort);
        if (::bind(raw, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0) {
            closeSock(s);
            return kBad;
        }
    }
    setNonBlocking(s);
    return s;
}

// One datagram to host:port. Returns bytes sent, 0 when it would block, -1 on error.
inline int udpSendTo(Sock s, std::uint32_t host, int port, const void* data, std::size_t len) {
    const sockaddr_in a = addrOf(host, port);
    const int n = static_cast<int>(::sendto(
#if defined(_WIN32)
        static_cast<SOCKET>(s), static_cast<const char*>(data), static_cast<int>(len), 0,
#else
        static_cast<int>(s), data, len, 0,
#endif
        reinterpret_cast<const sockaddr*>(&a), sizeof(a)));
    if (n < 0) { return wouldBlock() ? 0 : -1; }
    return n;
}

// One datagram, if one is waiting. Returns bytes (0 allowed: an empty datagram), -1 when nothing is waiting or on error.
inline int udpRecv(Sock s, void* buf, std::size_t cap) {
    sockaddr_in from{};
#if defined(_WIN32)
    int fl = sizeof(from);
    const int n = ::recvfrom(static_cast<SOCKET>(s), static_cast<char*>(buf), static_cast<int>(cap), 0, reinterpret_cast<sockaddr*>(&from), &fl);
#else
    socklen_t fl = sizeof(from);
    const int n = static_cast<int>(::recvfrom(static_cast<int>(s), buf, cap, 0, reinterpret_cast<sockaddr*>(&from), &fl));
#endif
    return n;
}

// A listening TCP socket on host:port (port 0 = a free one; read it with boundPortOf). Non-blocking accept.
inline Sock tcpListen(std::uint32_t host, int port) {
    if (!initSockets()) { return kBad; }
    const Sock s = static_cast<Sock>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
#if defined(_WIN32)
    if (static_cast<SOCKET>(s) == INVALID_SOCKET) { return kBad; }
    const SOCKET raw = static_cast<SOCKET>(s);
#else
    if (s < 0) { return kBad; }
    const int raw = static_cast<int>(s);
#endif
    const sockaddr_in a = addrOf(host, port);
    if (::bind(raw, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0 || ::listen(raw, 8) != 0) {
        closeSock(s);
        return kBad;
    }
    setNonBlocking(s);
    return s;
}

// Accepts one waiting connection, non-blocking socket back, or kBad when none is waiting.
inline Sock tcpAccept(Sock listener) {
#if defined(_WIN32)
    const SOCKET c = ::accept(static_cast<SOCKET>(listener), nullptr, nullptr);
    if (c == INVALID_SOCKET) { return kBad; }
#else
    const int c = ::accept(static_cast<int>(listener), nullptr, nullptr);
    if (c < 0) { return kBad; }
#endif
    const Sock s = static_cast<Sock>(c);
    setNonBlocking(s);
    return s;
}

// Connects to host:port, waiting at most timeoutMs. kBad on refusal or timeout. The returned socket is non-blocking.
inline Sock tcpConnect(std::uint32_t host, int port, int timeoutMs) {
    if (!initSockets()) { return kBad; }
    const Sock s = static_cast<Sock>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
#if defined(_WIN32)
    if (static_cast<SOCKET>(s) == INVALID_SOCKET) { return kBad; }
    const SOCKET raw = static_cast<SOCKET>(s);
#else
    if (s < 0) { return kBad; }
    const int raw = static_cast<int>(s);
#endif
    setNonBlocking(s);
    const sockaddr_in a = addrOf(host, port);
    const int rc = ::connect(raw, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    if (rc != 0 && !wouldBlock()) { closeSock(s); return kBad; }
    if (rc != 0) {
        fd_set w, e;
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(raw, &w);
        FD_SET(raw, &e);
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        const int n = ::select(static_cast<int>(raw) + 1, nullptr, &w, &e, &tv);
        if (n <= 0 || FD_ISSET(raw, &e) || !FD_ISSET(raw, &w)) { closeSock(s); return kBad; }
        int err = 0;
#if defined(_WIN32)
        int el = sizeof(err);
        ::getsockopt(raw, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &el);
#else
        socklen_t el = sizeof(err);
        ::getsockopt(raw, SOL_SOCKET, SO_ERROR, &err, &el);
#endif
        if (err != 0) { closeSock(s); return kBad; }
    }
    return s;
}

// Non-blocking send: bytes sent (possibly fewer than len), 0 when it would block, -1 when the peer is gone.
inline int tcpSend(Sock s, const void* data, std::size_t len) {
#if defined(_WIN32)
    const int n = ::send(static_cast<SOCKET>(s), static_cast<const char*>(data), static_cast<int>(len), 0);
#else
    const int n = static_cast<int>(::send(static_cast<int>(s), data, len, 0));
#endif
    if (n < 0) { return wouldBlock() ? 0 : -1; }
    return n;
}

// Non-blocking receive: bytes read, 0 when nothing is waiting, -1 when the peer closed or failed.
inline int tcpRecv(Sock s, void* buf, std::size_t cap) {
#if defined(_WIN32)
    const int n = ::recv(static_cast<SOCKET>(s), static_cast<char*>(buf), static_cast<int>(cap), 0);
#else
    const int n = static_cast<int>(::recv(static_cast<int>(s), buf, cap, 0));
#endif
    if (n == 0) { return -1; }   // orderly close
    if (n < 0) { return wouldBlock() ? 0 : -1; }
    return n;
}

}  // namespace cascade::net::lo
