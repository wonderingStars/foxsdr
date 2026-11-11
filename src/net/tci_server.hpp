// net/tci_server.hpp - the TCI server's TCP and WebSocket transport (the protocol is net/tci_protocol, the frames net/ws_frame).
//
// SAFETY, in the order it matters:
//  * OFF BY DEFAULT and LOOPBACK BY DEFAULT: start() binds 127.0.0.1 unless bindAll says otherwise (the engine's setting is `tciBindAll`,
//    off). This port has no authentication - the protocol has none - so anything that can reach it can retune the receivers; a bind to
//    every interface is a deliberate act and is reported as such. (Same reasoning as net/cat_server.hpp.)
//  * RECEIVE-ONLY. No transmit command exists in the protocol layer's vocabulary (net/tci_protocol.hpp) and the engine session behind a
//    connection has no transmitter op on its profile (InternalKind::Tci), so this is two walls, not one.
//  * NOTHING HERE BLOCKS THE ENGINE. A connection is one thread of its own that never calls anything that waits on the DSP thread or the
//    pass: it reads the engine's published figures (read_radio_state), submits commands and collects results (all non-blocking calls of the
//    table) and reads the audio ring. A slow or dead client is DROPPED, never waited for: sends are non-blocking into a bounded
//    out-queue; a queue over kMaxTciOutBytes, or one the peer has not drained at all for kTciStallMs, ends the connection. Audio is
//    not queued behind a backlog (kTciAudioHighWater): it is skipped, and the engine's ring tells the reader how much it lost.
//  * HOSTILE INPUT IS REFUSED: a request head over 8 KiB, an invalid handshake, an unmasked or reserved frame, a message over
//    kMaxTciMessageBytes, text that is not UTF-8, a command over the protocol's limits - each closes the connection or drops the command,
//    none can crash the server or grow a buffer without bound (the bounds are the constants below).
//  * kMaxTciClients concurrent clients; one more is refused with an HTTP 503 that says so.
//
// A CONNECTION IS A SESSION (as for CAT): opened by the acceptor right after the client-count check, handed to the connection's thread and
// closed by it before the count drops, so stop()'s "no thread of this server still runs" also means "no session of this server exists".
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_TCI_SERVER_HPP
#define CASCADE_NET_TCI_SERVER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/tci_protocol.hpp"

namespace cascade::net {

inline constexpr int kMaxTciClients = 4;
inline constexpr std::size_t kMaxTciMessageBytes = 32768;       // a client's largest WebSocket message (a TX audio block of TCI 2.1 is 64 + 16384 bytes)
inline constexpr std::size_t kMaxTciOutBytes = 1u << 20;        // the most one client may have queued unsent before it is dropped
inline constexpr std::size_t kTciAudioHighWater = 256u << 10;   // no audio is read while this much is waiting to be sent
inline constexpr int kTciStallMs = 3000;                        // a peer that accepts no byte at all for this long is dropped
inline constexpr int kTciHandshakeMs = 5000;                    // a client has this long to complete the opening handshake
inline constexpr int kTciTickMs = 30;                           // the state-push tick of the specification (4.7 note 7)

// Opens the engine session for a new connection; `label` names it ("tci:<n>"). Null when the engine will not give one.
using TciBackendOpener = std::function<std::unique_ptr<TciBackend>(const std::string& label)>;

class TciServer {
public:
    TciServer() = default;
    ~TciServer();
    TciServer(const TciServer&) = delete;
    TciServer& operator=(const TciServer&) = delete;

    void setBackendOpener(TciBackendOpener fn);
    // Fired once per accepted client (after the cap and the session); must not block.
    void setUsageCallback(std::function<void()> fn);

    // Binds and starts. False with a sentence in `error` (the sentence is shown to the operator verbatim): the port already in use, or not
    // allowed. Port 0 asks the OS for a free port (boundPort()).
    bool start(std::uint16_t port, bool bindAll, std::string& error);
    // A hard stop: when it returns no thread of this server is running and every connection is closed.
    void stop();
    bool running() const { return running_.load(std::memory_order_relaxed); }
    std::uint16_t boundPort() const { return boundPort_.load(std::memory_order_relaxed); }

    int clientCount() const { return clients_.load(std::memory_order_relaxed); }
    std::uint64_t commandCount() const { return commands_.load(std::memory_order_relaxed); }
    std::uint64_t audioPackets() const { return audioPackets_.load(std::memory_order_relaxed); }
    // Connections ended because the peer could not keep up, or broke the protocol.
    std::uint64_t slowDropped() const { return slowDropped_.load(std::memory_order_relaxed); }
    std::uint64_t protocolDropped() const { return protocolDropped_.load(std::memory_order_relaxed); }
    std::uint64_t refusedOverCap() const { return refusedOverCap_.load(std::memory_order_relaxed); }

private:
    void acceptLoop();
    void serveClient(std::intptr_t sock, TciBackend& backend);

    TciBackendOpener opener_;
    std::function<void()> usageCallback_;
    std::atomic<std::uint64_t> connections_{0};
    std::thread acceptor_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<int> clients_{0};
    std::atomic<std::uint64_t> commands_{0};
    std::atomic<std::uint64_t> audioPackets_{0};
    std::atomic<std::uint64_t> slowDropped_{0};
    std::atomic<std::uint64_t> protocolDropped_{0};
    std::atomic<std::uint64_t> refusedOverCap_{0};
    std::atomic<std::uint16_t> boundPort_{0};
    std::intptr_t listener_ = -1;
    std::mutex clientsMu_;
    std::vector<std::intptr_t> clientSocks_;
};

}  // namespace cascade::net

#endif  // CASCADE_NET_TCI_SERVER_HPP
