// iq_out.hpp - network I/Q out: the raw samples of the tuned channel or of the full band over TCP (TcpFanout) or UDP (UdpSink), as 8-bit
// unsigned, 16-bit signed or 32-bit float, with an optional small header or none.
//
// WHAT IS SENT. Interleaved I, Q, I, Q ... in the chosen format, little-endian, nothing else (HEADERLESS, the default - what a program
// reading "raw IQ" from a socket expects), or with the 28-byte FXIQ header below. Which samples (the channel or the whole band) and at
// what rate is the CALLER's choice: submit() takes the floats it is given and, in the header, the rate and centre frequency it was told.
//
//   8-bit unsigned   u = round(x * 127.5 + 127.5), clamped to 0..255; x = (u - 127.5) / 127.5 on the way back. Zero maps to 128.
//                    This is the usual raw-file convention of 8-bit receivers; WHICH programs use exactly this is UNVERIFIED.
//   16-bit signed    s = round(x * 32767), clamped to -32767..32767, little-endian.
//   32-bit float     the IEEE-754 single, little-endian, unchanged.
//   A non-finite input sample (NaN, infinity) is sent as zero in every format: no receiver is handed a NaN.
//
//   HEADER (28 bytes, little-endian; only when `header` is on)
//     0  "FXIQ"            4 bytes, ASCII
//     4  version           u16 = 1
//     6  format            u16: 1 = u8, 2 = s16, 3 = f32
//     8  sampleRateHz      u32
//     12 centreHz          f64 (IEEE-754 double)
//     20 sequence          u32: 0 in the TCP greeting; in UDP the datagram counter, which counts a datagram even when the ring then
//                          drops it, so a receiver sees a gap in the sequence wherever samples were lost
//     24 flags             u32 = 0 (reserved)
//   TCP: the header is sent ONCE to every new client (its greeting), then the sample stream. UDP: every datagram starts with the header.
//
// DROPS. The producer is never slowed. When the network is slower than the radio the ring (or a slow TCP client's queue) overflows and the
// samples are dropped and COUNTED (stats().samplesDropped; the TCP fan-out's own counters say which client was dropped). A drop leaves a
// discontinuity in the stream at a whole-sample boundary. In UDP the sequence number shows it; headerless streams cannot.
//
// COST. The producer-thread cost is the conversion plus one wait-free publish per chunk; measured in tests/test_iq_out.cpp and recorded in
// docs/feature-records/net-outputs.md (2.4 MS/s).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_IQ_OUT_HPP
#define CASCADE_NET_IQ_OUT_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "net/tcp_fanout.hpp"
#include "net/udp_sink.hpp"

namespace cascade::net {

enum class IqFormat : std::uint16_t { U8 = 1, S16 = 2, F32 = 3 };

inline constexpr std::size_t kIqHeaderBytes = 28;
inline constexpr std::size_t kIqDefaultTcpChunkSamples = 4096;
inline constexpr std::uint16_t kIqDefaultTcpPort = 1234;  // rtl_tcp's customary port: a courtesy default, UNVERIFIED as a convention here

struct IqHeader {
    IqFormat format = IqFormat::U8;
    std::uint32_t sampleRateHz = 0;
    double centreHz = 0.0;
    std::uint32_t sequence = 0;
};
// Writes the 28 bytes. `out` must have room for kIqHeaderBytes.
void iqWriteHeader(const IqHeader& h, std::uint8_t* out) noexcept;
// Reads and validates a header (magic, version 1, format 1..3, flags 0); false otherwise.
bool iqReadHeader(const std::uint8_t* in, std::size_t n, IqHeader& h) noexcept;

std::size_t iqBytesPerSample(IqFormat f) noexcept;  // one complex sample: I and Q together (2, 4 or 8)

// Converts `nComplex` interleaved float pairs (2 x nComplex floats) into `out` (iqBytesPerSample x nComplex bytes). No allocation.
void iqConvert(IqFormat f, const float* iq, std::size_t nComplex, std::uint8_t* out) noexcept;

struct IqOutConfig {
    IqFormat format = IqFormat::U8;
    bool header = false;                  // false: headerless
    std::uint32_t sampleRateHz = 0;       // required (non-zero) when `header` is on
    double centreHz = 0.0;                // written into the header
    bool udp = false;                     // false: TCP
    std::size_t chunkSamples = 0;         // complex samples per message; 0 = the default (TCP 4096; UDP: as many as fit one datagram)
    FanoutConfig tcp = defaultTcp();
    UdpConfig udpCfg;

    static FanoutConfig defaultTcp() {
        FanoutConfig c;
        c.port = kIqDefaultTcpPort;
        c.clientQueueBytes = 1 << 20;     // about 200 ms of 8-bit samples at 2.4 MS/s
        c.ringBytes = 4 << 20;
        return c;
    }
};

struct IqStats {
    std::uint64_t samplesIn = 0;        // complex samples handed to submit()
    std::uint64_t samplesQueued = 0;    // complex samples that went into the ring / the sender
    std::uint64_t samplesDropped = 0;   // complex samples the ring could not take
    std::uint64_t chunksDropped = 0;
    FanoutStats tcp;                    // when TCP
    UdpStats udp;                       // when UDP
};

class IqOut {
public:
    explicit IqOut(const IqOutConfig& cfg);
    bool start(std::string& error);
    void stop();
    // PRODUCER THREAD ONLY. `iq` holds 2 x nComplex floats, I then Q. Returns the number of complex samples queued (nComplex when
    // nothing was dropped). Wait-free; allocation-free.
    std::size_t submit(const float* iq, std::size_t nComplex) noexcept;
    std::uint16_t tcpPort() const { return tcp_.port(); }
    std::size_t chunkSamples() const { return chunk_; }
    IqStats stats() const;

private:
    static IqOutConfig fix(const IqOutConfig& c);

    IqOutConfig cfg_;
    TcpFanout tcp_;
    UdpSink udp_;
    std::size_t chunk_ = 0;
    std::size_t bps_ = 0;
    std::vector<std::uint8_t> scratch_;  // one message: header (UDP) + a chunk of samples
    std::uint32_t seq_ = 0;              // producer thread only
    std::atomic<std::uint64_t> in_{0}, queued_{0}, dropped_{0}, chunksDropped_{0};  // written by the producer, read by stats()
};

}  // namespace cascade::net

#endif
