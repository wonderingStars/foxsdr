// net/rtl_tcp_server.hpp - an rtl_tcp-compatible SERVER: programs that read an RTL-SDR over the network connect to FoxSDR and receive a radio's I/Q samples.
//
// THE PROTOCOL (as the programs that speak it use it; UNVERIFIED against a real client in this lane: every test here is a stand-in client built from this description).
//   * On connect the server sends a 12-byte header: the four ASCII bytes "RTL0", then two 32-bit big-endian numbers: the tuner type and the number of gain steps. This server
//     says 0 and 0 (a tuner it does not name, no gain list): it is not an RTL-SDR and does not pretend to be one.
//   * Then it streams the samples: interleaved I, Q, I, Q ... as UNSIGNED 8-bit values (zero is 128; net/iq_out.hpp's U8 conversion), at the sample rate of the radio the
//     person chose. The rate is the radio's own: a client's request to change it is counted and NOT applied.
//   * The client sends 5-byte commands: one byte for the command, then a 32-bit big-endian parameter. 0x01 is "set the centre frequency" (Hz); 0x02 sample rate, 0x03 gain mode,
//     0x04 gain, 0x05 frequency correction, 0x06 IF gain, 0x07 test mode, 0x08 AGC mode, 0x09 direct sampling, 0x0A offset tuning, 0x0B and 0x0C crystal frequencies, 0x0D gain by
//     index, 0x0E bias tee.
//
// WHAT THIS SERVER DOES WITH A COMMAND. It hands each command to the caller's function (the engine), which decides; the engine's only effect is a retune of the chosen radio through its
// ordinary tuning command (its own range checks apply). EVERY OTHER command is counted and ignored: nothing here sets a gain, a rate, a bias tee or anything that is not tuning, and
// nothing can reach a transmit path.
//
// SAFETY. One client at a time (a second connection is closed at once, counted). Binds 127.0.0.1 unless allowAllInterfaces (a deliberate setting; 0.0.0.0 is refused otherwise). A
// client that sends garbage is read in bounded turns and the bytes are not kept (only whole 5-byte commands are looked at); a client that connects and never reads is dropped after
// `stallMs` and until then holds at most `clientQueueBytes` (samples that do not fit are dropped and counted: the producer is never slowed); a client that connects over and over
// cannot grow anything: connections are closed at once past the one. submit() is wait-free and does nothing while no client is connected.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_RTL_TCP_SERVER_HPP
#define CASCADE_NET_RTL_TCP_SERVER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "net/net_sock.hpp"
#include "net/peer_allow.hpp"
#include "net/spsc_ring.hpp"

namespace cascade::net {

inline constexpr std::uint16_t kRtlTcpDefaultPort = 1234;
inline constexpr std::size_t kRtlTcpHeaderBytes = 12;
inline constexpr std::size_t kRtlTcpCommandBytes = 5;
inline constexpr std::uint8_t kRtlCmdSetFrequency = 0x01;
inline constexpr std::uint8_t kRtlCmdLast = 0x0E;   // the highest command number the protocol has

struct RtlTcpConfig {
    std::string bindAddress = "127.0.0.1";   // dotted-quad IPv4
    bool allowAllInterfaces = false;         // must be true for 0.0.0.0
    std::uint16_t port = kRtlTcpDefaultPort; // 0 = the OS picks (tests)
    std::size_t clientQueueBytes = 1 << 20;  // what one client may hold unsent
    std::size_t ringBytes = 4 << 20;         // producer -> IO thread
    int stallMs = 10000;                     // a client with unsent bytes and no progress this long is dropped (0 = never)
    int pollMs = 2;
    std::uint32_t tunerType = 0;
    std::uint32_t gainCount = 0;
    PeerAllow allowedPeers;                  // when not empty: only these client addresses are served; any other is closed before it can take the one place (net/peer_allow.hpp)
};

struct RtlTcpStats {
    std::uint64_t accepted = 0;
    std::uint64_t refusedBusy = 0;        // a second client, closed at once
    std::uint64_t refusedPeer = 0;        // an address that is not on the allow-list, closed at once
    std::uint64_t refusedProtocol = 0;    // a client whose first command was not an rtl_tcp command (a web page or another protocol talking to this port): dropped
    std::uint64_t closedByPeer = 0;
    std::uint64_t droppedStalled = 0;
    std::uint64_t bytesSent = 0;
    std::uint64_t bytesRead = 0;          // everything a client sent (read and thrown away but for whole commands)
    std::uint64_t samplesIn = 0;          // complex samples offered while a client was connected
    std::uint64_t samplesDropped = 0;     // complex samples the ring or the client's queue could not take
    std::uint64_t commands = 0;           // whole 5-byte commands read
    std::uint64_t unknownCommands = 0;    // a command byte outside 0x01..0x0E
    int clients = 0;
};

class RtlTcpServer {
public:
    // Called on the IO thread for every command with a known number; it must be quick and must not block.
    using CommandFn = std::function<void(std::uint8_t command, std::uint32_t parameter)>;

    explicit RtlTcpServer(const RtlTcpConfig& cfg) : cfg_(cfg), ring_(cfg.ringBytes < 4096 ? 4096 : cfg.ringBytes) {}
    ~RtlTcpServer();
    RtlTcpServer(const RtlTcpServer&) = delete;
    RtlTcpServer& operator=(const RtlTcpServer&) = delete;

    bool start(CommandFn onCommand, std::string& error);
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }
    int port() const { return port_.load(std::memory_order_acquire); }
    bool clientConnected() const { return clients_.load(std::memory_order_acquire) > 0; }

    // PRODUCER THREAD ONLY (one thread). `iq` holds 2 x nComplex floats, I then Q. Wait-free, allocation-free; does nothing while no client is connected.
    std::size_t submit(const float* iq, std::size_t nComplex) noexcept;

    RtlTcpStats stats() const;

    // The 12-byte header for these two numbers (the tests read it back).
    static void header(std::uint32_t tunerType, std::uint32_t gainCount, std::uint8_t out[kRtlTcpHeaderBytes]) noexcept;

private:
    void run();

    RtlTcpConfig cfg_;
    MessageRing ring_;
    CommandFn onCommand_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopFlag_{false};
    std::atomic<int> port_{0};
    std::atomic<int> clients_{0};
    sock::Handle listener_ = sock::kInvalid;
    std::uint32_t bindIp_ = 0;

    std::atomic<std::uint64_t> accepted_{0}, refusedPeer_{0}, refusedProtocol_{0}, refusedBusy_{0}, closedByPeer_{0}, droppedStalled_{0}, bytesSent_{0}, bytesRead_{0}, samplesIn_{0}, samplesDropped_{0}, commands_{0}, unknownCommands_{0};
};

}  // namespace cascade::net

#endif  // CASCADE_NET_RTL_TCP_SERVER_HPP
