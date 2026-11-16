// net/tcp_audio.hpp - "Audio over TCP": one radio's finished audio served to programs that connect, in the same format as the UDP audio
// (net/udp_audio.hpp): headerless raw PCM, 48 000 Hz, signed 16-bit little-endian, ONE channel (the format GQRX documents, which command-line
// tools read). A program connects and receives the audio from that moment on; nothing is sent back and nothing a client sends is read (the
// shared fan-out reads it and throws it away, bounded per turn).
//
// SAFETY (screen lane S4b, docs/multi-radio-records/screen-s4b.md): OFF BY DEFAULT, binds 127.0.0.1 unless the person chose to listen on
// every address (allowAllInterfaces, a deliberate setting); at most `maxClients` clients (a connection past it is closed at once); a client
// that cannot keep up or stops reading is dropped (the fan-out's own bounds: a queue per client and a stall time), the producer is never
// slowed and memory stays bounded however many clients connect and however they behave. The audio is read like every other reader's, from the
// engine's audio ring, by a worker of this class; it never touches the DSP thread or the engine's pass.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_TCP_AUDIO_HPP
#define CASCADE_NET_TCP_AUDIO_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "net/tcp_fanout.hpp"
#include "net/udp_audio.hpp"

namespace cascade::net {

inline constexpr int kDefaultTcpAudioPort = 7356;

struct TcpAudioConfig {
    std::string bindAddress = "127.0.0.1";   // dotted-quad IPv4
    bool allowAllInterfaces = false;         // must be true for 0.0.0.0
    int port = kDefaultTcpAudioPort;         // 1..65535 (0 = the OS picks, for the tests)
    int packetFrames = kUdpPacketFramesShort;   // 480 or 960 samples a write
    int maxClients = 4;                      // 1..kFanoutMaxClientsCap
};

class TcpAudioServer {
public:
    TcpAudioServer() = default;
    ~TcpAudioServer();
    TcpAudioServer(const TcpAudioServer&) = delete;
    TcpAudioServer& operator=(const TcpAudioServer&) = delete;

    // Binds, then starts the worker. False with a sentence in `error` (a bad setting, the bind policy, a port in use).
    bool start(const TcpAudioConfig& cfg, UdpAudioSender::Reader reader, std::string& error);
    void stop();
    bool running() const { return running_.load(std::memory_order_relaxed); }
    int port() const { return fan_ ? fan_->port() : 0; }
    FanoutStats stats() const { return fan_ ? fan_->stats() : FanoutStats{}; }
    std::uint64_t noRadio() const { return noRadio_.load(std::memory_order_relaxed); }

private:
    void run();

    std::unique_ptr<TcpFanout> fan_;
    UdpAudioSender::Reader reader_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> noRadio_{0};
    int packetFrames_ = kUdpPacketFramesShort;
};

}  // namespace cascade::net

#endif  // CASCADE_NET_TCP_AUDIO_HPP
