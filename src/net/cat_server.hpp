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
// THREADING. One acceptor thread, one thread per connection, capped. Set
// requests are QUEUED for the GUI thread exactly as the browser's are: this
// code never touches the pipeline.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_CAT_SERVER_HPP
#define CASCADE_NET_CAT_SERVER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/cat_protocol.hpp"

namespace cascade::net {

// The port Hamlib's rigctld uses, and therefore the one every client already
// expects. Changing it means configuring every client, so it is the default.
inline constexpr std::uint16_t kDefaultCatPort = 4532;

// A client is allowed a generous but finite line; anything longer is a client
// that has lost sync, and reading it forever would be a memory leak driven
// from the network.
inline constexpr std::size_t kMaxCatLineBytes = 1024;

// Concurrent connections. Small because real stations run one or two clients;
// the cap exists so a misbehaving script cannot exhaust threads.
inline constexpr int kMaxCatClients = 4;

// Bound on the queue the GUI thread drains, and deliberately the same number
// and the same rule as the browser's (WebServer::kMaxQueuedControls): an
// application that stops draining — a stalled GUI thread — must not let a
// client grow this without limit, and past the cap the OLDEST request is
// dropped, because for a control surface the most recent instruction is the
// one that matters.
inline constexpr std::size_t kMaxQueuedCatControls = 64;

class CatServer {
public:
    using StatusProvider = std::function<RadioStatus()>;

    CatServer() = default;
    ~CatServer();
    CatServer(const CatServer&) = delete;
    CatServer& operator=(const CatServer&) = delete;

    // Must be set before start(); the server calls it on its own threads, so
    // the provider has to be safe to call from any thread (the application
    // hands out a snapshot taken under the pipeline's lock).
    void setStatusProvider(StatusProvider fn);

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

    // Requests accumulated since the last call, in arrival order. The GUI
    // thread drains this each frame and applies them.
    std::vector<ControlRequest> takePendingControls();

    // Diagnostics for the panel: how many clients are connected and how many
    // commands have been served since start.
    int clientCount() const { return clients_.load(std::memory_order_relaxed); }
    std::uint64_t commandCount() const {
        return commands_.load(std::memory_order_relaxed);
    }

private:
    void acceptLoop();
    void serveClient(std::intptr_t sock);

    StatusProvider status_;
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

    std::mutex controlsMu_;
    std::vector<ControlRequest> controls_;
};

}  // namespace cascade::net

#endif  // CASCADE_NET_CAT_SERVER_HPP
