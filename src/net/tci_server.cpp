// TCP and WebSocket transport of the TCI server. See the header for what it promises.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/tci_server.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
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
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "core/health_events.hpp"
#include "core/thread_beats.hpp"
#include "net/ws_frame.hpp"

namespace cascade::net {
namespace {

using Sock = std::intptr_t;

#if defined(_WIN32)
using SockLen = int;
constexpr Sock kInvalid = static_cast<Sock>(INVALID_SOCKET);
SOCKET raw(Sock s) { return static_cast<SOCKET>(s); }
void closeSock(Sock s) { if (s != kInvalid) { ::closesocket(raw(s)); } }
// shutdown(), not close, from another thread (the owning thread closes; see CatServer).
void shutdownSock(Sock s) { if (s != kInvalid) { ::shutdown(raw(s), SD_BOTH); } }
bool ensureWinsock() {
    static const bool ok = [] {
        WSADATA d{};
        return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}
int lastError() { return ::WSAGetLastError(); }
bool wouldBlock(int e) { return e == WSAEWOULDBLOCK || e == WSAEINTR; }
bool addrInUse(int e) { return e == WSAEADDRINUSE; }
bool accessDenied(int e) { return e == WSAEACCES; }
void setNonBlocking(Sock s) { u_long on = 1; ::ioctlsocket(raw(s), FIONBIO, &on); }
#else
using SockLen = socklen_t;
constexpr Sock kInvalid = -1;
int raw(Sock s) { return static_cast<int>(s); }
void closeSock(Sock s) { if (s != kInvalid) { ::close(raw(s)); } }
void shutdownSock(Sock s) { if (s != kInvalid) { ::shutdown(raw(s), SHUT_RDWR); } }
bool ensureWinsock() { return true; }
int lastError() { return errno; }
bool wouldBlock(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == EINTR; }
bool addrInUse(int e) { return e == EADDRINUSE; }
bool accessDenied(int e) { return e == EACCES; }
void setNonBlocking(Sock s) { const int fl = ::fcntl(raw(s), F_GETFL, 0); ::fcntl(raw(s), F_SETFL, fl | O_NONBLOCK); }
#endif

constexpr int kAcceptPollMs = 200;
constexpr int kClientDrainTimeoutMs = 5000;

// -1: would block, 0: the peer closed, -2: failed, > 0: bytes.
int recvSome(Sock s, char* buf, std::size_t n) {
#if defined(_WIN32)
    const int r = ::recv(raw(s), buf, static_cast<int>(n), 0);
#else
    const int r = static_cast<int>(::recv(raw(s), buf, n, 0));
#endif
    if (r > 0) { return r; }
    if (r == 0) { return 0; }
    return wouldBlock(lastError()) ? -1 : -2;
}

// -1: would block, -2: failed, >= 0: bytes accepted.
int sendSome(Sock s, const char* buf, std::size_t n) {
#if defined(_WIN32)
    const int r = ::send(raw(s), buf, static_cast<int>(std::min<std::size_t>(n, 1u << 20)), 0);
#else
    const int r = static_cast<int>(::send(raw(s), buf, std::min<std::size_t>(n, 1u << 20), MSG_NOSIGNAL));
#endif
    if (r >= 0) { return r; }
    return wouldBlock(lastError()) ? -1 : -2;
}

// Waits up to `ms` for the socket to be readable. Returns true when it is (or has closed).
bool waitReadable(Sock s, int ms) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(raw(s), &rf);
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
#if defined(_WIN32)
    return ::select(0, &rf, nullptr, nullptr, &tv) > 0;
#else
    return ::select(raw(s) + 1, &rf, nullptr, nullptr, &tv) > 0;
#endif
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A tiny blocking-with-deadline send for the acceptor's refusals (a few hundred bytes into a fresh socket's empty buffer).
void sendRefusal(Sock s, const std::string& text) {
    setNonBlocking(s);
    (void)sendSome(s, text.data(), text.size());
}

}  // namespace

TciServer::~TciServer() { stop(); }

void TciServer::setBackendOpener(TciBackendOpener fn) { opener_ = std::move(fn); }
void TciServer::setUsageCallback(std::function<void()> fn) { usageCallback_ = std::move(fn); }

bool TciServer::start(std::uint16_t port, bool bindAll, std::string& error) {
    error.clear();
    if (running_.load(std::memory_order_relaxed)) {
        error = "the TCI server is already running";
        return false;
    }
    if (!opener_) {
        error = "the TCI server has no engine session to answer from";
        return false;
    }
    if (!ensureWinsock()) {
        error = "could not initialise the network stack";
        return false;
    }
    const Sock s = static_cast<Sock>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (s == kInvalid) {
        error = "could not create a socket";
        return false;
    }
    // BIND EXCLUSIVITY (see CatServer::start): on Windows SO_REUSEADDR lets a second program bind a port already in use, so the exclusive
    // option is used instead and a second bind fails cleanly; on POSIX SO_REUSEADDR only waives TIME_WAIT.
    int yes = 1;
    ::setsockopt(raw(s), SOL_SOCKET,
#if defined(_WIN32)
                 SO_EXCLUSIVEADDRUSE,
#else
                 SO_REUSEADDR,
#endif
                 reinterpret_cast<const char*>(&yes), sizeof(yes));
    // An accept timeout so the acceptor wakes to notice a stop request (closing a listener does not reliably interrupt accept()).
#if defined(_WIN32)
    DWORD to = kAcceptPollMs;
    ::setsockopt(raw(s), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
#else
    timeval to{};
    to.tv_sec = 0;
    to.tv_usec = kAcceptPollMs * 1000;
    ::setsockopt(raw(s), SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    addr.sin_addr.s_addr = bindAll ? ::htonl(INADDR_ANY) : ::htonl(INADDR_LOOPBACK);
    if (::bind(raw(s), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int err = lastError();
        closeSock(s);
        const std::string where = std::string(bindAll ? "all interfaces" : "127.0.0.1") + " port " + std::to_string(port);
        if (addrInUse(err)) {
            error = "port " + std::to_string(port) + " is already in use by another program; the TCI server cannot listen on " + where +
                    " (close that program, or pick another port)";
        } else if (accessDenied(err)) {
            error = "this account is not allowed to listen on " + where + " (error " + std::to_string(err) + ")";
        } else {
            error = "could not listen on " + where + " (error " + std::to_string(err) + ")";
        }
        return false;
    }
    if (::listen(raw(s), 8) != 0) {
        const int err = lastError();
        closeSock(s);
        error = "could not listen on port " + std::to_string(port) + " (error " + std::to_string(err) + ")";
        return false;
    }
    std::uint16_t actual = port;
    sockaddr_in bound{};
    SockLen blen = sizeof(bound);
    if (::getsockname(raw(s), reinterpret_cast<sockaddr*>(&bound), &blen) == 0) { actual = ::ntohs(bound.sin_port); }
    listener_ = s;
    boundPort_.store(actual, std::memory_order_relaxed);
    stopping_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    acceptor_ = std::thread([this] { acceptLoop(); });
    return true;
}

void TciServer::stop() {
    if (!running_.load(std::memory_order_relaxed)) { return; }
    stopping_.store(true, std::memory_order_relaxed);
    closeSock(listener_);
    if (acceptor_.joinable()) { acceptor_.join(); }
    listener_ = kInvalid;
    {
        std::lock_guard<std::mutex> lk(clientsMu_);
        for (const Sock c : clientSocks_) { shutdownSock(c); }
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kClientDrainTimeoutMs);
    while (clients_.load(std::memory_order_acquire) != 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "tci: %d connection thread(s) still running after %d ms; giving up waiting\n",
                         clients_.load(std::memory_order_acquire), kClientDrainTimeoutMs);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    boundPort_.store(0, std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
}

void TciServer::acceptLoop() {
    cascade::core::beats::Registration beat(cascade::core::beats::kThreadTciAccept);
    while (!stopping_.load(std::memory_order_relaxed)) {
        sockaddr_in peer{};
        SockLen len = sizeof(peer);
        beat.wait(cascade::core::beats::kWaitAccept);
        const Sock c = static_cast<Sock>(::accept(raw(listener_), reinterpret_cast<sockaddr*>(&peer), &len));
        beat.beat("accepted");
        if (c == kInvalid) {
            if (stopping_.load(std::memory_order_relaxed)) { break; }
            continue;
        }
        if (clients_.load(std::memory_order_relaxed) >= kMaxTciClients) {
            refusedOverCap_.fetch_add(1, std::memory_order_relaxed);
            sendRefusal(c, ws::handshakeRefusal(503, "too many TCI clients are connected (" + std::to_string(kMaxTciClients) + "); close one and try again"));
            closeSock(c);
            continue;
        }
        std::unique_ptr<TciBackend> backend = opener_("tci:" + std::to_string(connections_.fetch_add(1, std::memory_order_relaxed) + 1));
        if (!backend) {
            sendRefusal(c, ws::handshakeRefusal(503, "the engine has no session free for another TCI client"));
            closeSock(c);
            continue;
        }
        clients_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(clientsMu_);
            clientSocks_.push_back(c);
        }
        if (usageCallback_) { usageCallback_(); }
        std::thread([this, c, backend = std::move(backend)]() mutable {
            serveClient(c, *backend);
            // THE SESSION CLOSES HERE, before the count below drops (stop() seeing zero means no session of this server is left).
            backend.reset();
            {
                std::lock_guard<std::mutex> lk(clientsMu_);
                clientSocks_.erase(std::remove(clientSocks_.begin(), clientSocks_.end(), c), clientSocks_.end());
            }
            closeSock(c);
            clients_.fetch_sub(1, std::memory_order_release);
        }).detach();
    }
}

void TciServer::serveClient(Sock sock, TciBackend& backend) {
    cascade::core::beats::Registration beat(cascade::core::beats::kThreadTciClient);
    setNonBlocking(sock);
    int yes = 1;
    ::setsockopt(raw(sock), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
    ::setsockopt(raw(sock), SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&yes), sizeof(yes));

    // ---- the opening handshake (RFC 6455 4.2.1), bounded in size and in time ----
    std::string head;
    const std::int64_t t0 = nowMs();
    std::string carry;   // bytes that arrived after the blank line
    for (;;) {
        if (stopping_.load(std::memory_order_relaxed)) { return; }
        beat.beat("handshake");
        if (nowMs() - t0 > kTciHandshakeMs) { return; }
        if (!waitReadable(sock, 50)) { continue; }
        char b[1024];
        const int n = recvSome(sock, b, sizeof(b));
        if (n == 0 || n == -2) { return; }
        if (n < 0) { continue; }
        head.append(b, static_cast<std::size_t>(n));
        const std::size_t end = head.find("\r\n\r\n");
        if (end != std::string::npos) {
            carry = head.substr(end + 4);
            head.resize(end + 4);
            break;
        }
        if (head.size() > ws::kMaxHandshakeBytes) {
            protocolDropped_.fetch_add(1, std::memory_order_relaxed);
            sendRefusal(sock, ws::handshakeRefusal(431, "the request head is too long"));
            return;
        }
    }
    const ws::Handshake hs = ws::parseHandshake(head);
    if (!hs.ok) {
        protocolDropped_.fetch_add(1, std::memory_order_relaxed);
        sendRefusal(sock, ws::handshakeRefusal(hs.status, hs.error));
        return;
    }

    // ---- the connection ----
    std::string out;                 // bytes waiting to go (bounded)
    std::size_t sentOff = 0;         // out[0, sentOff) has gone
    std::int64_t lastProgress = nowMs();
    const auto queue = [&](const std::string& frame) { out += frame; };
    const auto backlog = [&] { return out.size() - sentOff; };
    // False when the connection must end now (a send failed, or the peer stalled or let the queue grow past its bound).
    const auto flush = [&]() -> bool {
        while (sentOff < out.size()) {
            const int r = sendSome(sock, out.data() + sentOff, out.size() - sentOff);
            if (r == -2) { return false; }
            if (r < 0) { break; }
            if (r == 0) { break; }
            sentOff += static_cast<std::size_t>(r);
            lastProgress = nowMs();
        }
        if (sentOff == out.size()) {
            out.clear();
            sentOff = 0;
            lastProgress = nowMs();
        } else if (sentOff > (1u << 16)) {
            out.erase(0, sentOff);
            sentOff = 0;
        }
        if (backlog() > kMaxTciOutBytes || (backlog() > 0 && nowMs() - lastProgress > kTciStallMs)) {
            slowDropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    };

    queue(ws::handshakeAcceptResponse(hs.key));
    TciConnection conn(backend);
    for (const std::string& m : conn.begin()) { queue(ws::encodeFrame(ws::Opcode::Text, m)); }
    if (!flush()) { return; }

    ws::FrameDecoder dec(kMaxTciMessageBytes);
    dec.feed(carry.data(), carry.size());
    std::int64_t lastTick = nowMs();
    std::uint64_t seenCommands = 0;
    std::map<int, std::uint64_t> cursors;   // per radio audio cursor
    std::vector<float> abuf(4800);
    bool closing = false;
    for (;;) {
        if (stopping_.load(std::memory_order_relaxed)) {
            queue(ws::encodeClose(ws::kCloseGoingAway, "the TCI server is stopping"));
            (void)flush();
            return;
        }
        beat.beat("poll");
        const bool wantWrite = backlog() > 0;
        // Sleep until data arrives, or a tick is due; never longer than the tick, so audio and pushes stay timely. With a backlog the
        // wait is short: the socket's writability is polled by flush() each turn.
        if (!closing) { (void)waitReadable(sock, wantWrite ? 5 : 10); } else { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        char b[4096];
        for (int i = 0; i < 8 && !closing; ++i) {
            const int n = recvSome(sock, b, sizeof(b));
            if (n == 0 || n == -2) { return; }
            if (n < 0) { break; }
            dec.feed(b, static_cast<std::size_t>(n));
        }
        ws::Message m;
        while (!closing && dec.next(m)) {
            switch (m.op) {
                case ws::Opcode::Text:
                    for (const std::string& r : conn.onText(m.payload)) { queue(ws::encodeFrame(ws::Opcode::Text, r)); }
                    break;
                case ws::Opcode::Ping: queue(ws::encodeFrame(ws::Opcode::Pong, m.payload)); break;
                case ws::Opcode::Close: {
                    const std::uint16_t code = m.payload.size() >= 2
                        ? static_cast<std::uint16_t>((static_cast<unsigned char>(m.payload[0]) << 8) | static_cast<unsigned char>(m.payload[1]))
                        : ws::kCloseNormal;
                    queue(ws::encodeClose(code, ""));
                    closing = true;
                    break;
                }
                default: break;   // a binary message (a TX audio block: this server is receive-only) or a pong: dropped
            }
        }
        if (dec.failed() && !closing) {
            protocolDropped_.fetch_add(1, std::memory_order_relaxed);
            queue(ws::encodeClose(dec.errorCode(), dec.errorText()));
            closing = true;
        }
        const std::int64_t now = nowMs();
        if (!closing && now - lastTick >= kTciTickMs) {
            lastTick = now;
            beat.beat("tick");
            for (const std::string& r : conn.tick(now)) { queue(ws::encodeFrame(ws::Opcode::Text, r)); }
            // Audio: read what the engine has for each started stream, but not while this client is behind.
            for (const int radio : conn.audioRadios()) {
                if (backlog() > kTciAudioHighWater) { break; }
                std::uint64_t& cursor = cursors[radio];
                for (int round = 0; round < 4; ++round) {
                    const int got = backend.readAudio(radio, cursor, abuf.data(), abuf.size());
                    if (got <= 0) { break; }
                    for (const std::string& pkt : conn.audioPackets(radio, abuf.data(), static_cast<std::size_t>(got))) {
                        queue(ws::encodeFrame(ws::Opcode::Binary, pkt));
                        audioPackets_.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (static_cast<std::size_t>(got) < abuf.size()) { break; }
                }
            }
        }
        const std::uint64_t cmds = conn.counters().commands;
        if (cmds != seenCommands) {
            commands_.fetch_add(cmds - seenCommands, std::memory_order_relaxed);
            seenCommands = cmds;
        }
        if (!flush()) { return; }
        if (closing && backlog() == 0) { return; }
        if (closing && nowMs() - lastProgress > 1000) { return; }
    }
}

}  // namespace cascade::net
