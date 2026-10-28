// CAT control: the TCP transport around cat_protocol.
//
// Deliberately thin. Every decision about what a command MEANS lives in
// cat_protocol.cpp, which has no sockets and is therefore tested as a pure
// function; this file only moves bytes and manages threads.
//
// BIND POLICY. The same reasoning as the web server, and the same default:
// loopback only. This port carries no authentication of any kind — the
// protocol has no concept of it — so anything that can reach it can retune the
// receiver. Binding it to a LAN address is a deliberate act, and it is
// reported as such rather than being the default.
//
// THREADING. One acceptor thread, one thread per connection, capped. This code
// never touches the pipeline, and since engine extraction stage 5c it keeps no
// queue either: every connection holds a SESSION of its own on the engine
// (a CatSession, which the engine implements over the C table - the server
// does not name an engine header), reads the receiver's figures through it,
// hands its set requests to it and collects their results. The engine applies
// them in its pass, after the local operator's and the API clients' commands.
//
// A CONNECTION IS A SESSION. It is opened by the acceptor thread right after the
// client-count check, handed to the connection's thread, and closed by that
// thread before the client count drops - so stop()'s "no thread of this server
// still runs" also means "no session of this server still exists". There is no
// heartbeat: a CAT client never holds a key (the profile of a CAT session has
// no transmitter op at all), and only a key holder's liveness is ever judged.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_CAT_SERVER_HPP
#define CASCADE_NET_CAT_SERVER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/server_ports.hpp"
#include "foxsdr_api.h"
#include "net/cat_protocol.hpp"

namespace cascade::net {

// ONE CONNECTION'S LINE TO THE ENGINE (stage 5c). The engine implements it over the C table (engine/server_host.cpp: a session opened with
// ApiHost::openInternalSession(InternalKind::Cat, ...)), a test implements it over a stub; the server calls it from the connection's own
// thread only, so an implementation needs no locking of its own. Closing is the destructor.
class CatSession {
public:
    virtual ~CatSession() = default;
    // The six figures of the newest publish (the session's own read_state). False when the engine could not give them - a read that kept
    // overlapping a publish, after one retry - and `out` is then untouched: the caller keeps the last it had.
    virtual bool readState(CatRadio& out) = 0;
    // Hands a request's commands over, in order. Returns how many NEW tickets the engine gave (each will produce exactly one result); a
    // command the engine answered BUSY at the door has none and is simply not taken.
    virtual int submit(const std::vector<FoxCommand>& commands) = 0;
    // Collects the results that are ready, and returns how many it took. A session that never collects them is answered BUSY past 256
    // outstanding commands, so the server calls this after every request and on every idle tick of the connection.
    virtual int pollResults() = 0;
};

// Opens the session for a new connection; `label` names it ("cat:<n>"). Null when the engine will not give one (its session limits are
// reached): the connection is then refused, as one past the client cap is. Called from the acceptor thread.
using CatSessionOpener = std::function<std::unique_ptr<CatSession>(const std::string& label)>;

// (kDefaultCatPort, the port Hamlib's rigctld uses, is core/server_ports.hpp's since stage 5b.)

// A client is allowed a generous but finite line; anything longer is a client
// that has lost sync, and reading it forever would be a memory leak driven
// from the network.
inline constexpr std::size_t kMaxCatLineBytes = 1024;

// Concurrent connections. Small because real stations run one or two clients;
// the cap exists so a misbehaving script cannot exhaust threads.
inline constexpr int kMaxCatClients = 4;

// (There is no queue here any more, and so no bound to keep: the old one - 64 deep, the OLDEST dropped past it - was this server's own.
// A connection's commands wait in its session, which holds at most FOXAPI_MAX_PENDING (256) outstanding commands and answers the NEWEST
// BUSY past it; the engine applies them every pass, and the connection collects their results as it goes.)

// How long a connection that is closing waits for the commands it has handed over to be applied, so a one-shot client - `rigctl F ...`
// sends, reads "RPRT 0" and hangs up - still retunes the radio: a session that closes with commands queued has them refused (DETACHED),
// the old queue applied them whoever had sent them. A pass is tens of milliseconds; this is a hundred of them, and it is skipped when
// the server is stopping.
inline constexpr int kCatSessionDrainMs = 1000;

class CatServer {
public:
    CatServer() = default;
    ~CatServer();
    CatServer(const CatServer&) = delete;
    CatServer& operator=(const CatServer&) = delete;

    // Must be set before start(); the server calls it from its acceptor thread, so it has to be safe to call from any thread. Every
    // accepted connection opens its session through it (see CatSession).
    void setSessionOpener(CatSessionOpener fn);

    // Fired once per accepted client, from the acceptor thread. The engine's
    // ServerHost installs a counter here (fact.cat.accepted; shell retirement
    // R3) - what the beta-tester usage record's "CAT" mark is read from. Must
    // be set before start(); must be safe to call from any thread and must
    // never block (it runs ahead of the client's own thread being spawned).
    void setUsageCallback(std::function<void()> fn);

    // `bindAll` false binds 127.0.0.1, true binds 0.0.0.0. Returns false and
    // fills `error` if the socket could not be created or bound — including
    // the common case of the port already being held by a real rigctld.
    //
    // Port 0 asks the OS for a free port; boundPort() reports which one.
    bool start(std::uint16_t port, bool bindAll, std::string& error);

    // stop() is a HARD stop: when it returns, no thread belonging to this
    // server is still running and every client connection has been closed.
    // That is what makes it safe to destroy the object — a connection left
    // alive past destruction is a use-after-free a peer can trigger at will.
    void stop();
    bool running() const { return running_.load(std::memory_order_relaxed); }

    // The port actually bound, which differs from the requested one only when
    // 0 was requested. Zero when the server is not running.
    std::uint16_t boundPort() const { return boundPort_.load(std::memory_order_relaxed); }

    // Diagnostics for the panel: how many clients are connected and how many
    // commands have been served since start.
    int clientCount() const { return clients_.load(std::memory_order_relaxed); }
    std::uint64_t commandCount() const {
        return commands_.load(std::memory_order_relaxed);
    }

private:
    void acceptLoop();
    void serveClient(std::intptr_t sock, CatSession& session, int& outstanding);
    void drainSession(CatSession& session, int& outstanding);

    CatSessionOpener opener_;
    std::function<void()> usageCallback_;
    std::atomic<std::uint64_t> connections_{0};  // the number in a session's label
    std::thread acceptor_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<int> clients_{0};
    std::atomic<std::uint64_t> commands_{0};
    std::atomic<std::uint16_t> boundPort_{0};
    std::intptr_t listener_ = -1;

    // Live client sockets, so stop() can shut them down. Connection threads
    // are detached — nothing can join them — so this list plus the clients_
    // counter is how stop() knows when the last one has let go of `this`.
    std::mutex clientsMu_;
    std::vector<std::intptr_t> clientSocks_;
};

}  // namespace cascade::net

#endif  // CASCADE_NET_CAT_SERVER_HPP
