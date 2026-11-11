// net_sock.cpp - see net_sock.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/net_sock.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cascade::net::sock {
namespace {

#if defined(_WIN32)
using Sock = SOCKET;
inline Sock S(Handle h) { return static_cast<Sock>(h); }
inline Handle H(Sock s) { return (s == INVALID_SOCKET) ? kInvalid : static_cast<Handle>(s); }
using SockLen = int;
#else
using Sock = int;
inline Sock S(Handle h) { return static_cast<Sock>(h); }
inline Handle H(Sock s) { return (s < 0) ? kInvalid : static_cast<Handle>(s); }
using SockLen = socklen_t;
#endif

sockaddr_in makeAddr(std::uint32_t ip, std::uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    return a;
}

}  // namespace

bool ensureStarted() {
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

void closeHandle(Handle s) {
    if (s == kInvalid) { return; }
#if defined(_WIN32)
    ::closesocket(S(s));
#else
    ::close(S(s));
#endif
}

bool setNonBlocking(Handle s) {
#if defined(_WIN32)
    u_long on = 1;
    return ::ioctlsocket(S(s), FIONBIO, &on) == 0;
#else
    const int fl = ::fcntl(S(s), F_GETFL, 0);
    return fl >= 0 && ::fcntl(S(s), F_SETFL, fl | O_NONBLOCK) == 0;
#endif
}

void setNoDelay(Handle s) {
    int on = 1;
    ::setsockopt(S(s), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}

void setBuffers(Handle s, int recvBytes, int sendBytes) {
    if (recvBytes > 0) {
        ::setsockopt(S(s), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&recvBytes), sizeof(recvBytes));
    }
    if (sendBytes > 0) {
        ::setsockopt(S(s), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBytes), sizeof(sendBytes));
    }
}

bool wouldBlock() {
#if defined(_WIN32)
    const int e = ::WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINTR || e == WSAEINPROGRESS;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR || errno == EINPROGRESS;
#endif
}

bool parseIpv4(const std::string& text, std::uint32_t& hostOrder) {
    std::uint32_t acc = 0;
    int fields = 0;
    std::size_t i = 0;
    for (;;) {
        std::size_t j = i;
        while (j < text.size() && text[j] != '.') { ++j; }
        const std::string f = text.substr(i, j - i);
        if (f.empty() || f.size() > 3 || (f.size() > 1 && f[0] == '0')) { return false; }
        unsigned v = 0;
        for (char c : f) {
            if (c < '0' || c > '9') { return false; }
            v = v * 10 + static_cast<unsigned>(c - '0');
        }
        if (v > 255 || ++fields > 4) { return false; }
        acc = (acc << 8) | v;
        if (j == text.size()) { break; }
        i = j + 1;
    }
    if (fields != 4) { return false; }
    hostOrder = acc;
    return true;
}

std::string formatIpv4(std::uint32_t ip) {
    return std::to_string((ip >> 24) & 255) + "." + std::to_string((ip >> 16) & 255) + "." + std::to_string((ip >> 8) & 255) + "." +
           std::to_string(ip & 255);
}

Handle listenTcp(std::uint32_t ip, std::uint16_t port, int backlog, std::string& error) {
    if (!ensureStarted()) {
        error = "network stack did not start";
        return kInvalid;
    }
    const Handle h = H(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (h == kInvalid) {
        error = "could not create a socket";
        return kInvalid;
    }
    int on = 1;
#if defined(_WIN32)
    // SO_REUSEADDR on Windows lets a second process bind the same port and steal connections; exclusive use is the safe setting.
    ::setsockopt(S(h), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on));
#else
    ::setsockopt(S(h), SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
    const sockaddr_in a = makeAddr(ip, port);
    if (::bind(S(h), reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0) {
        error = "could not listen on " + formatIpv4(ip) + ":" + std::to_string(port) + " (in use, or not an address of this machine)";
        closeHandle(h);
        return kInvalid;
    }
    if (::listen(S(h), backlog) != 0 || !setNonBlocking(h)) {
        error = "could not listen";
        closeHandle(h);
        return kInvalid;
    }
    return h;
}

std::uint16_t boundPort(Handle s) {
    sockaddr_in a{};
    SockLen len = sizeof(a);
    if (::getsockname(S(s), reinterpret_cast<sockaddr*>(&a), &len) != 0) { return 0; }
    return ntohs(a.sin_port);
}

Handle acceptOne(Handle listener) {
    const Handle h = H(::accept(S(listener), nullptr, nullptr));
    if (h == kInvalid) { return kInvalid; }
    if (!setNonBlocking(h)) {
        closeHandle(h);
        return kInvalid;
    }
    return h;
}

Handle beginConnectTcp(std::uint32_t ip, std::uint16_t port) {
    if (!ensureStarted()) { return kInvalid; }
    const Handle h = H(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (h == kInvalid) { return kInvalid; }
    if (!setNonBlocking(h)) {
        closeHandle(h);
        return kInvalid;
    }
    const sockaddr_in a = makeAddr(ip, port);
    if (::connect(S(h), reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0 && !wouldBlock()) {
        closeHandle(h);
        return kInvalid;
    }
    return h;
}

bool waitConnect(Handle s, int ms) {
    PollItem it;
    it.s = s;
    it.want = kWritable;
    if (pollMany(&it, 1, ms) <= 0) { return false; }
    int err = 0;
    SockLen len = sizeof(err);
    if (::getsockopt(S(s), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) != 0) { return false; }
    return err == 0 && (it.got & kWritable) != 0;
}

long recvSome(Handle s, void* buf, std::size_t n) {
    return static_cast<long>(::recv(S(s), static_cast<char*>(buf), static_cast<int>(n), 0));
}

long sendSome(Handle s, const void* buf, std::size_t n) {
#if defined(MSG_NOSIGNAL)
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    return static_cast<long>(::send(S(s), static_cast<const char*>(buf), static_cast<int>(n), flags));
}

int pollMany(PollItem* items, std::size_t n, int ms) {
    if (n == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms > 0 ? ms : 0));
        return 0;
    }
#if defined(_WIN32)
    WSAPOLLFD fds[256];
#else
    pollfd fds[256];
#endif
    if (n > 256) { n = 256; }
    for (std::size_t i = 0; i < n; ++i) {
        fds[i].fd = S(items[i].s);
        fds[i].events = static_cast<short>(((items[i].want & kReadable) ? POLLIN : 0) | ((items[i].want & kWritable) ? POLLOUT : 0));
        fds[i].revents = 0;
        items[i].got = 0;
    }
#if defined(_WIN32)
    const int r = ::WSAPoll(fds, static_cast<ULONG>(n), ms);
#else
    const int r = ::poll(fds, static_cast<nfds_t>(n), ms);
#endif
    if (r < 0) { return -1; }
    for (std::size_t i = 0; i < n; ++i) {
        unsigned g = 0;
        if (fds[i].revents & POLLIN) { g |= kReadable; }
        if (fds[i].revents & POLLOUT) { g |= kWritable; }
        if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { g |= kError; }
        items[i].got = g;
    }
    return r;
}

Handle openUdp(bool allowBroadcast, std::string& error) {
    if (!ensureStarted()) {
        error = "network stack did not start";
        return kInvalid;
    }
    const Handle h = H(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (h == kInvalid) {
        error = "could not create a UDP socket";
        return kInvalid;
    }
    if (allowBroadcast) {
        int on = 1;
        ::setsockopt(S(h), SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
    }
    if (!setNonBlocking(h)) {
        error = "could not set the UDP socket non-blocking";
        closeHandle(h);
        return kInvalid;
    }
    return h;
}

long sendTo(Handle s, const void* buf, std::size_t n, std::uint32_t ip, std::uint16_t port) {
    const sockaddr_in a = makeAddr(ip, port);
    return static_cast<long>(::sendto(S(s), static_cast<const char*>(buf), static_cast<int>(n), 0,
                                      reinterpret_cast<const sockaddr*>(&a), sizeof(a)));
}

}  // namespace cascade::net::sock
