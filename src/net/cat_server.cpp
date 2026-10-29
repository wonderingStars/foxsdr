// TCP transport for the CAT server. See the header for the bind policy and
// why this file is as small as it is.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/cat_server.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
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
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "core/thread_beats.hpp"
#include "net/control_ops.hpp"

namespace cascade::net {
namespace {

#if defined(_WIN32)
using SockLen = int;
constexpr std::intptr_t kInvalidSock = static_cast<std::intptr_t>(INVALID_SOCKET);

void closeSock(std::intptr_t s) {
    if (s != kInvalidSock) ::closesocket(static_cast<SOCKET>(s));
}

// Tells the peer we are done, without closing the descriptor: the owning
// thread still has to close its own socket, and closing a descriptor another
// thread is using invites the handle being reused underneath it.
//
// This does NOT interrupt a recv() that is ALREADY in progress on Windows —
// measured directly, `after shutdown(SD_BOTH): recv unblocked = False` — so
// nothing here may depend on it doing so. See serveClient's poll timeout,
// which is what actually bounds a parked connection thread.
void shutdownSock(std::intptr_t s) {
    if (s != kInvalidSock) ::shutdown(static_cast<SOCKET>(s), SD_BOTH);
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

// See the Windows comment above: shutdown, not close, from another thread.
void shutdownSock(std::intptr_t s) {
    if (s != kInvalidSock) ::shutdown(static_cast<int>(s), SHUT_RDWR);
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

// A receive that expired, or was interrupted, rather than a connection that
// failed. Telling them apart is the whole point of the poll timeout below: a
// timeout means "nothing yet from an idle client", and a server that read it
// as a closed connection would hang up on healthy clients.
bool isRecvTimeout(int err) {
#if defined(_WIN32)
    return err == WSAETIMEDOUT || err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
#endif
}

// How often a connection thread comes up for air to notice a stop request,
// and the same figure the acceptor uses. It bounds how long stop() waits.
constexpr int kSocketPollMs = 200;

// The bound on stop()'s wait for connection threads, generously above the poll
// interval. It exists so that a thread which somehow never wakes degrades into
// a logged warning instead of freezing the caller for ever; stop() is called
// from the GUI thread, so "for ever" would be a frozen application.
constexpr int kClientDrainTimeoutMs = 5000;

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

void CatServer::setSessionOpener(CatSessionOpener fn) { opener_ = std::move(fn); }

void CatServer::setUsageCallback(std::function<void()> fn) { usageCallback_ = std::move(fn); }

bool CatServer::start(std::uint16_t port, bool bindAll, std::string& error) {
    error.clear();
    if (running_.load(std::memory_order_relaxed)) {
        error = "the CAT server is already running";
        return false;
    }
    if (!opener_) {
        error = "the CAT server has no engine session to answer from";
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

    // BIND EXCLUSIVITY, and note that the two platforms need OPPOSITE options
    // to get the same behaviour.
    //
    // On Windows SO_REUSEADDR does not mean what it means on Unix: it lets a
    // SECOND process bind a port this one is already listening on, and new
    // connections then land on whichever socket the stack picks. For a control
    // port that retunes the radio that is a local hijack — any program on the
    // machine can quietly take over the CAT port and answer the operator's
    // client. SO_EXCLUSIVEADDRUSE is the Windows way to say "this port is
    // mine", and a second bind then fails cleanly through the error path below
    // instead of silently succeeding.
    //
    // On Unix SO_REUSEADDR does NOT allow that hijack; it only waives the
    // TIME_WAIT wait, without which a restart fails to bind and reads to a
    // user as "the port is taken" when nothing holds it.
    int yes = 1;
    ::setsockopt(
#if defined(_WIN32)
        static_cast<SOCKET>(s), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
#else
        static_cast<int>(s), SOL_SOCKET, SO_REUSEADDR,
#endif
        reinterpret_cast<const char*>(&yes), sizeof(yes));

    // An accept timeout, so the acceptor wakes periodically and can notice
    // that it has been asked to stop.
    //
    // This is NOT belt and braces. Closing a listening socket does not reliably
    // interrupt another thread already blocked in accept() — on Linux it simply
    // does not, and stop() would then join a thread that never returns, hanging
    // the application on exit. The timeout is what makes shutdown terminate.
#if defined(_WIN32)
    DWORD acceptTimeoutMs = kSocketPollMs;
    ::setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&acceptTimeoutMs),
                 sizeof(acceptTimeoutMs));
#else
    timeval acceptTimeout{};
    acceptTimeout.tv_sec = 0;
    acceptTimeout.tv_usec = kSocketPollMs * 1000;
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

    // Read the port back rather than echoing the requested one: a request for
    // port 0 means "any free port", and only the OS knows which it picked.
    std::uint16_t actualPort = port;
    sockaddr_in bound{};
    SockLen boundLen = sizeof(bound);
    if (::getsockname(
#if defined(_WIN32)
            static_cast<SOCKET>(s),
#else
            static_cast<int>(s),
#endif
            reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
        actualPort = ::ntohs(bound.sin_port);
    }

    listener_ = s;
    boundPort_.store(actualPort, std::memory_order_relaxed);
    stopping_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    acceptor_ = std::thread([this] { acceptLoop(); });
    return true;
}

void CatServer::stop() {
    if (!running_.load(std::memory_order_relaxed)) return;
    stopping_.store(true, std::memory_order_relaxed);
    // Closing the listener is what wakes the blocking accept(); there is no
    // portable way to interrupt it otherwise. Clearing listener_ waits until
    // the acceptor has been joined, so the acceptor never reads it torn.
    closeSock(listener_);
    if (acceptor_.joinable()) acceptor_.join();
    listener_ = kInvalidSock;

    // The acceptor is gone, so no new connection can appear. Now wind up every
    // client thread and WAIT for the last one to let go.
    //
    // Connection threads are detached and capture `this`, so a thread still
    // parked in recv() after stop() returns will dereference this object at
    // whatever moment its peer next sends a byte or hangs up — long after the
    // object may have been destroyed. That is a use-after-free any connected
    // peer can fire at will. The counter is decremented as the LAST thing a
    // connection thread touches, so seeing zero means no thread can reach
    // `this` again.
    //
    // What ends the wait is the poll timeout on each accepted socket (see
    // serveClient): stopping_ is already set, so each thread notices within
    // one poll interval. The shutdown below is only a courtesy that tells the
    // peer at once and wakes an idle recv() early where the platform allows
    // it — on Windows it does NOT interrupt a recv() already in progress, and
    // waiting on the peer to hang up instead would park this caller until the
    // FIN_WAIT_2 timer expired, roughly two minutes. stop() runs on the GUI
    // thread (unticking "Accept CAT connections", changing the port, closing
    // the app), so that would be a two-minute freeze of the application with
    // one idle CAT client connected — the normal state of a CAT client.
    {
        std::lock_guard<std::mutex> lk(clientsMu_);
        for (const std::intptr_t c : clientSocks_) shutdownSock(c);
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kClientDrainTimeoutMs);
    while (clients_.load(std::memory_order_acquire) != 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            // Bounded degradation, and a visible one: an unbounded wait here
            // would turn any future failure to wake a connection thread into
            // an indefinite hang of whoever called stop().
            std::fprintf(stderr,
                         "cat: %d connection thread(s) still running after %d ms; "
                         "giving up waiting\n",
                         clients_.load(std::memory_order_acquire),
                         kClientDrainTimeoutMs);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    boundPort_.store(0, std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
}

void CatServer::acceptLoop() {
    // THE HEARTBEAT (core/thread_beats.hpp, 6.27). accept() is a declared wait: a CAT server nobody has connected to is idle,
    // and stop() ends the wait by closing the listener.
    cascade::core::beats::Registration beat(cascade::core::beats::kThreadCatAccept);
    while (!stopping_.load(std::memory_order_relaxed)) {
        sockaddr_in peer{};
        SockLen len = sizeof(peer);
        beat.wait(cascade::core::beats::kWaitAccept);
        const std::intptr_t c = static_cast<std::intptr_t>(::accept(
#if defined(_WIN32)
            static_cast<SOCKET>(listener_),
#else
            static_cast<int>(listener_),
#endif
            reinterpret_cast<sockaddr*>(&peer), &len));
        beat.beat("accepted");
        if (c == kInvalidSock) {
            if (stopping_.load(std::memory_order_relaxed)) break;
            continue;
        }
        if (clients_.load(std::memory_order_relaxed) >= kMaxCatClients) {
            closeSock(c);
            continue;
        }
        // THE CONNECTION'S SESSION, opened here - after the cap, on the acceptor thread - and handed to the connection's thread, which
        // closes it. An engine that will not give one (its session limits are reached) refuses the connection as the cap does.
        std::unique_ptr<CatSession> session = opener_("cat:" + std::to_string(connections_.fetch_add(1, std::memory_order_relaxed) + 1));
        if (!session) {
            closeSock(c);
            continue;
        }
        clients_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(clientsMu_);
            clientSocks_.push_back(c);
        }
        // TESTER USAGE: "CAT control" was used - a client actually connected,
        // not merely that the server was switched on.
        if (usageCallback_) { usageCallback_(); }
        // Detached: a client thread owns its socket and nothing joins it.
        // stop() reaches these threads through clientSocks_ instead, and the
        // decrement below is deliberately the LAST thing this thread does with
        // `this` — stop() waits on that counter reaching zero, so anything
        // after it would be a use-after-free.
        std::thread([this, c, session = std::move(session)]() mutable {
            int outstanding = 0;  // commands handed to the session whose results have not been collected
            serveClient(c, *session, outstanding);
            // What this client sent is applied before its session goes: a session that closes with commands queued has them refused.
            drainSession(*session, outstanding);
            // THE SESSION CLOSES HERE, before the count below drops: stop() seeing zero clients means no session of this server is left.
            session.reset();
            {
                std::lock_guard<std::mutex> lk(clientsMu_);
                clientSocks_.erase(
                    std::remove(clientSocks_.begin(), clientSocks_.end(), c),
                    clientSocks_.end());
            }
            // Closed only after the socket is off the list, so stop() can
            // never shut down a handle that has already been recycled.
            closeSock(c);
            clients_.fetch_sub(1, std::memory_order_release);
        }).detach();
    }
}

void CatServer::serveClient(std::intptr_t sock, CatSession& session, int& outstanding) {
    // FIRST, set this socket's timeouts explicitly rather than inheriting
    // whatever the listener carries.
    //
    // The receive timeout is a POLL interval, not a deadline for the client: a
    // timeout means "nothing yet", the loop below goes round and blocks again,
    // and only stopping_ ends it. Two things depend on that, and they pull in
    // opposite directions:
    //
    //   - An idle client must survive. A CAT client is idle almost all the
    //     time, polling a frequency every second or two, so a timeout treated
    //     as a closed connection would hang up on every healthy client the
    //     moment it paused — the feature useless in exactly its normal state
    //     while looking fine under a burst of traffic. Hence isRecvTimeout().
    //   - stop() must not block. Nothing else can wake a thread parked here:
    //     on Windows shutdown() does not interrupt a recv() already in
    //     progress, so without this timeout stop() would wait on the peer,
    //     which for an idle client means the ~2-minute FIN_WAIT_2 timer — on
    //     the GUI thread. This timeout is what bounds stop() to one interval.
    //
    // The send timeout is the same argument for the other direction: replies
    // are a handful of bytes, so a send that cannot complete in seconds means
    // a peer that has stopped reading, and a thread parked in send() would be
    // just as unwakeable.
#if defined(_WIN32)
    DWORD recvTimeoutMs = kSocketPollMs;
    ::setsockopt(static_cast<SOCKET>(sock), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&recvTimeoutMs),
                 sizeof(recvTimeoutMs));
    DWORD sendTimeoutMs = kClientDrainTimeoutMs;
    ::setsockopt(static_cast<SOCKET>(sock), SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char*>(&sendTimeoutMs),
                 sizeof(sendTimeoutMs));
#else
    timeval recvTimeout{};
    recvTimeout.tv_sec = 0;
    recvTimeout.tv_usec = kSocketPollMs * 1000;
    ::setsockopt(static_cast<int>(sock), SOL_SOCKET, SO_RCVTIMEO, &recvTimeout,
                 sizeof(recvTimeout));
    timeval sendTimeout{};
    sendTimeout.tv_sec = kClientDrainTimeoutMs / 1000;
    sendTimeout.tv_usec = 0;
    ::setsockopt(static_cast<int>(sock), SOL_SOCKET, SO_SNDTIMEO, &sendTimeout,
                 sizeof(sendTimeout));
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

    // The results of what this connection submitted are collected as it goes (see CatSession::pollResults): after every request, and on
    // every idle tick below - a session that never collects them is answered BUSY past 256 outstanding commands.
    const auto collect = [&] {
        outstanding -= session.pollResults();
        if (outstanding < 0) outstanding = 0;
    };

    // The newest figures the session gave. A read can fail (the engine's publish was being replaced twice running); the last good ones
    // stand in, as the old lock-free read fell back to the installed block rather than answer nothing.
    CatRadio lastRadio;

    // THE HEARTBEAT of this connection's thread (6.27): a beat every receive-poll interval; a session call that never returns
    // (the 120 s freeze that started this feature was a CAT client being shut down) is named by the phase it was in.
    cascade::core::beats::Registration beat(cascade::core::beats::kThreadCatClient);
    std::string pending;
    char buf[512];
    for (;;) {
        if (stopping_.load(std::memory_order_relaxed)) break;
        beat.beat("poll");
        const int n =
#if defined(_WIN32)
            ::recv(static_cast<SOCKET>(sock), buf, static_cast<int>(sizeof(buf)), 0);
#else
            static_cast<int>(::recv(static_cast<int>(sock), buf, sizeof(buf), 0));
#endif
        if (n == 0) break;  // peer closed
        if (n < 0) {
            // An expired poll interval is an idle client, not a broken one:
            // go round, re-read stopping_, and block again.
            if (isRecvTimeout(lastSocketError())) {
                if (outstanding > 0) collect();  // the idle tick
                continue;
            }
            break;  // the socket failed
        }
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

            // The figures come from the session's own read_state, asked afresh for every line (as they always were). BUSY is the engine
            // saying the read overlapped a publish: ask once more, then keep the last good ones.
            beat.beat("read-state");
            CatRadio radio = lastRadio;
            if (session.readState(radio) || session.readState(radio)) lastRadio = radio;
            const CatResult result = executeCatLine(line, lastRadio);
            commands_.fetch_add(1, std::memory_order_relaxed);

            if (result.hasControl) {
                // F is SET_CENTRE and SET_VFO_OFFSET in one batch, never SET_FREQUENCY: cat_protocol's planTune decided whether the
                // centre moves (outside the band; the offset returns to 0) or only the VFO (inside it - no retune, no gap in the
                // waterfall), and the translation keeps that order - the mode before the bandwidth, the centre before the offset.
                std::vector<FoxCommand> batch;
                for (const cascade::core::cmd::QueuedCommand& q : controlRequestToCommands(result.control, ControlOpsContext{})) {
                    batch.push_back(q.c);
                }
                // A command the session answers BUSY (256 outstanding: the engine is not applying) is not taken, and the client is told
                // what it was always told: the set was accepted. The old queue dropped its OLDEST past 64; the session refuses the NEWEST.
                beat.beat("submit");
                if (!batch.empty()) outstanding += session.submit(batch);
            }
            beat.beat("collect");
            collect();
            // Returning is enough: the caller deregisters the socket and then
            // closes it, which is the only ordering under which stop() can
            // never shut down a handle that has already been closed here.
            if (!result.reply.empty() && !sendAll(sock, result.reply)) return;
            if (result.quit) return;
        }
    }
}

// Waits - bounded, and not at all when the server is stopping - for what this connection handed over to be applied and its results
// collected, so the session can be closed without having its commands refused (see kCatSessionDrainMs).
void CatServer::drainSession(CatSession& session, int& outstanding) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kCatSessionDrainMs);
    while (outstanding > 0 && !stopping_.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < deadline) {
        outstanding -= session.pollResults();
        if (outstanding <= 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

}  // namespace cascade::net
