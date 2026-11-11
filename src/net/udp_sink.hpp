// udp_sink.hpp - a UDP datagram output with the same hand-over discipline as TcpFanout: a wait-free send() from the producing thread, one
// sender thread that owns the socket, drops counted (never retried, never blocking the producer).
//
//   * DESTINATION POLICY. One unicast IPv4 destination (dotted quad, port 1..65535), 127.0.0.1 by default. The limited broadcast address
//     255.255.255.255 and multicast (224.0.0.0/4) are refused unless `allowBroadcast` is set; 0.0.0.0 is never a destination.
//   * A datagram is one message of the ring: send(data, n) with n <= maxDatagram (default 1400, under a typical MTU; at most 65000).
//   * Datagrams the network or the socket refuses (a full socket buffer, an unreachable host) are COUNTED and dropped: UDP has no
//     back-pressure and this output must not make any.
//   * stop() joins the sender (it wakes every millisecond), closes the socket.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_UDP_SINK_HPP
#define CASCADE_NET_UDP_SINK_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

#include "net/net_sock.hpp"
#include "net/spsc_ring.hpp"

namespace cascade::net {

inline constexpr std::size_t kUdpDefaultDatagram = 1400;
inline constexpr std::size_t kUdpMaxDatagram = 65000;

struct UdpConfig {
    std::string destAddress = "127.0.0.1";
    std::uint16_t port = 0;                 // must be 1..65535 to start
    bool allowBroadcast = false;            // needed for 255.255.255.255 and multicast destinations
    std::size_t maxDatagram = kUdpDefaultDatagram;
    std::size_t ringBytes = 1 << 20;        // producer -> sender ring (rounded up to a power of two)
    int socketSendBuffer = 0;               // SO_SNDBUF asked of the socket (0 = the OS default)
};

struct UdpStats {
    std::uint64_t sent = 0;            // datagrams handed to the OS
    std::uint64_t bytesSent = 0;
    std::uint64_t ringDropped = 0;     // datagrams the ring could not take (the sender thread is behind)
    std::uint64_t socketDropped = 0;   // datagrams the OS refused because its buffer was full
    std::uint64_t sendErrors = 0;      // any other send failure
    std::uint64_t oversize = 0;        // datagrams longer than maxDatagram (or empty)
};

class UdpSink {
public:
    explicit UdpSink(const UdpConfig& cfg);
    ~UdpSink();
    UdpSink(const UdpSink&) = delete;
    UdpSink& operator=(const UdpSink&) = delete;

    bool start(std::string& error);
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }

    // PRODUCER THREAD ONLY (one thread). Wait-free. False when not running, empty, longer than maxDatagram, or the ring is full.
    bool send(const void* data, std::size_t n) noexcept;

    UdpStats stats() const;

private:
    void run();

    UdpConfig cfg_;
    MessageRing ring_;
    std::uint32_t ip_ = 0;
    sock::Handle sock_ = sock::kInvalid;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopFlag_{false};
    std::atomic<std::uint64_t> sent_{0}, bytesSent_{0}, socketDropped_{0}, sendErrors_{0}, oversize_{0};
};

}  // namespace cascade::net

#endif
