// tcp_fanout.hpp - a small shared TCP fan-out server: one producer, many listeners, nobody can hold the producer up.
//
// WHAT IT IS FOR. The network output parts (Beast and SBS aircraft feeds, AIS NMEA, raw I/Q) all do the same thing: a thread that has
// data hands it over, and whoever has connected receives a copy. This class is that, once.
//
//   * WAIT-FREE HAND-OVER. publish() copies the record into a MessageRing (spsc_ring.hpp) and returns. No lock, no allocation, no socket
//     call, no wait. When the ring is full the record is dropped and counted; the caller is never slowed. ONE producer thread per object.
//   * ONE IO THREAD owns every socket: accept, send, discard what a client sends, close. It wakes every `pollMs` (or sooner when a socket
//     is ready), so a record reaches a client within a couple of milliseconds plus the OS timer tick.
//   * A SLOW CLIENT IS DROPPED, THE PRODUCER IS NOT. Each client has a bounded queue (`clientQueueBytes`). A record that would not fit
//     into it disconnects THAT client (droppedSlow). A client that has bytes pending and makes no progress for `stallMs` is disconnected
//     too (droppedStalled): a client that never reads cannot hold memory or a slot for ever.
//   * A CAP ON CLIENTS (`maxClients`): a connection past it is accepted and closed at once (refusedFull) so the peer sees a refusal, not
//     a hang.
//   * BIND POLICY. The default address is 127.0.0.1. 0.0.0.0 (every interface) is refused unless `allowAllInterfaces` is set - exactly
//     like the web and CAT servers: these streams carry no authentication, so exposing them is a deliberate act. Only dotted-quad IPv4
//     is accepted (no name lookup can stall start()).
//   * HOSTILE PEERS. Whatever a client sends is read and thrown away (bounded per turn so a flood cannot starve the others); nothing a
//     peer sends is ever parsed or echoed.
//   * CLEAN STOP. stop() raises a flag and joins the IO thread, which exits its loop within one poll (`pollMs`), closes every socket and
//     returns. Measured in the tests, with clients attached and the producer still publishing; the project's shutdown budget is spent in
//     whole seconds, this is milliseconds. No std::chrono wait constant is declared here on purpose: the waits are plain integer
//     milliseconds (kDefaultPollMs, `stallMs`), compared against a clock by the IO thread, never waited on by the thread that calls stop().
//
// WHAT IT IS NOT: no TLS, no authentication, no framing of its own (the bytes you publish are the bytes a client gets), no replay for a
// late client (the optional `greeting` is the one thing every new client gets first, e.g. a stream header).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_TCP_FANOUT_HPP
#define CASCADE_NET_TCP_FANOUT_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "net/net_sock.hpp"
#include "net/spsc_ring.hpp"

namespace cascade::net {

inline constexpr int kDefaultPollMs = 2;
inline constexpr int kFanoutMaxClientsCap = 64;

struct FanoutConfig {
    std::string bindAddress = "127.0.0.1";  // dotted-quad IPv4
    bool allowAllInterfaces = false;        // must be true for 0.0.0.0
    std::uint16_t port = 0;                 // 0 = the OS picks (read it back with port())
    int maxClients = 8;                     // 1..kFanoutMaxClientsCap
    std::size_t clientQueueBytes = 256 * 1024;  // per-client bound; also the longest record that can be published
    std::size_t ringBytes = 2 * 1024 * 1024;    // producer -> IO thread ring (rounded up to a power of two)
    int stallMs = 10000;                    // a client with pending bytes and no progress this long is dropped (0 = never)
    int pollMs = kDefaultPollMs;            // IO thread wake interval
    int socketSendBuffer = 0;               // SO_SNDBUF asked of each accepted socket (0 = the OS default)
    std::vector<std::uint8_t> greeting;     // sent first to every new client (may be empty)
};

struct FanoutStats {
    std::uint64_t accepted = 0;
    std::uint64_t refusedFull = 0;        // closed at once: the client cap was reached
    std::uint64_t droppedSlow = 0;        // disconnected: its queue could not take a record
    std::uint64_t droppedStalled = 0;     // disconnected: pending bytes, no progress for stallMs
    std::uint64_t closedByPeer = 0;       // the client went away (or its connection failed)
    std::uint64_t published = 0;          // records accepted into the ring
    std::uint64_t ringDropped = 0;        // records the ring could not take (consumer behind)
    std::uint64_t oversize = 0;           // records refused for being longer than maxMessage()
    std::uint64_t bytesSent = 0;
    std::uint64_t bytesIgnored = 0;       // bytes clients sent that were thrown away
    int clients = 0;                      // connected now
};

class TcpFanout {
public:
    explicit TcpFanout(const FanoutConfig& cfg);
    ~TcpFanout();
    TcpFanout(const TcpFanout&) = delete;
    TcpFanout& operator=(const TcpFanout&) = delete;

    // Validates the configuration, binds and starts the IO thread. False and `error` (plain words) otherwise. Not callable twice without
    // a stop() between.
    bool start(std::string& error);
    // Idempotent. Returns after the IO thread has closed every socket.
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }
    // The port in force once start() has succeeded (the OS's choice when the config said 0).
    std::uint16_t port() const { return port_.load(std::memory_order_acquire); }

    // PRODUCER THREAD ONLY (one thread). Wait-free. False when the output is not running, the record is empty or longer than
    // maxMessage(), or the ring is full (counted).
    bool publish(const void* data, std::size_t n) noexcept;
    std::size_t maxMessage() const;

    FanoutStats stats() const;

private:
    struct Client {
        sock::Handle s = sock::kInvalid;
        std::vector<std::uint8_t> out;
        std::size_t off = 0;
        std::int64_t lastProgressMs = 0;
        std::size_t pending() const { return out.size() - off; }
    };

    void run();
    void dispatch(const std::uint8_t* data, std::size_t n);
    void acceptAll(std::int64_t nowMs);
    bool flush(Client& c, std::int64_t nowMs);  // false: the client is gone
    void drop(std::size_t index, std::atomic<std::uint64_t>& why);

    FanoutConfig cfg_;
    MessageRing ring_;
    std::uint32_t bindIp_ = 0;
    sock::Handle listener_ = sock::kInvalid;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopFlag_{false};
    std::atomic<std::uint16_t> port_{0};
    std::vector<Client> clients_;  // IO thread only

    std::atomic<std::uint64_t> accepted_{0}, refusedFull_{0}, droppedSlow_{0}, droppedStalled_{0}, closedByPeer_{0}, published_{0},
        bytesSent_{0}, bytesIgnored_{0}, oversize_{0};
    std::atomic<int> clientCount_{0};
};

}  // namespace cascade::net

#endif
