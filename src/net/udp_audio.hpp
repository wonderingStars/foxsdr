// net/udp_audio.hpp - "Audio over the network": one radio's finished audio sent as UDP datagrams to one address.
//
// WIRE FORMAT (design section 8; features-external-facts.md section 7, https://gqrx.dk/doc/streaming-audio-over-udp): headerless raw PCM,
// 48 000 Hz, signed 16-bit little-endian, ONE channel, no handshake, no sequence number - the format GQRX documents, which command-line
// tools (aplay, multimon-ng, DSD, per forum posts the facts report found) read. Only mono is built. The datagram is exactly `packetFrames`
// samples (480 = 10 ms, 960 = 20 ms; 960 bytes or 1920 bytes of payload), so a receiver that counts bytes can count time.
//
// SAFETY: OFF BY DEFAULT; the default destination is the loopback address 127.0.0.1 port 7355 (a destination on another machine is a
// deliberate setting); the socket only sends, nothing is ever read from the network; the worker never touches the DSP thread or the pass
// (it reads the engine's audio ring like any other reader, which is lock-free for the DSP side); a send that would block or fails
// (nobody listening, ICMP unreachable) is COUNTED and the audio is dropped - UDP is lossy by design and nothing waits.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_UDP_AUDIO_HPP
#define CASCADE_NET_UDP_AUDIO_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace cascade::net {

inline constexpr int kDefaultUdpAudioPort = 7355;
inline constexpr int kUdpPacketFramesShort = 480;
inline constexpr int kUdpPacketFramesLong = 960;

// float mono samples -> the datagram payload (little-endian int16, clipped). Pure, exported for the tests.
std::string udpAudioPayload(const float* samples, std::size_t n);

class UdpAudioSender {
public:
    // The audio source: mono 48 kHz float frames from `cursor` (0 = join live); returns frames written (>= 0) or a negative FOXAPI_* (the
    // radio is gone: nothing is sent until it is back). Called from the worker thread only.
    using Reader = std::function<int(std::uint64_t& cursor, float* out, std::size_t capFrames)>;

    UdpAudioSender() = default;
    ~UdpAudioSender();
    UdpAudioSender(const UdpAudioSender&) = delete;
    UdpAudioSender& operator=(const UdpAudioSender&) = delete;

    // Resolves `host` (an IPv4 address or a name) and starts the worker. False with a sentence in `error`.
    bool start(const std::string& host, int port, int packetFrames, Reader reader, std::string& error);
    void stop();
    bool running() const { return running_.load(std::memory_order_relaxed); }

    std::uint64_t packetsSent() const { return packets_.load(std::memory_order_relaxed); }
    std::uint64_t bytesSent() const { return bytes_.load(std::memory_order_relaxed); }
    std::uint64_t sendErrors() const { return errors_.load(std::memory_order_relaxed); }   // datagrams the network refused or would block on
    std::uint64_t noRadio() const { return noRadio_.load(std::memory_order_relaxed); }     // worker turns the radio was not there

private:
    void run();

    Reader reader_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> packets_{0};
    std::atomic<std::uint64_t> bytes_{0};
    std::atomic<std::uint64_t> errors_{0};
    std::atomic<std::uint64_t> noRadio_{0};
    std::intptr_t sock_ = -1;
    unsigned char addr_[32] = {};   // a sockaddr_in
    int addrLen_ = 0;
    int packetFrames_ = kUdpPacketFramesShort;
};

}  // namespace cascade::net

#endif  // CASCADE_NET_UDP_AUDIO_HPP
