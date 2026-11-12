// ais_out.hpp - AIS out as NMEA 0183 !AIVDM sentences, from raw AIS message bits, over the shared TCP fan-out and over UDP.
//
// INPUT: the message bits as the decoder has them - packed most-significant bit first, `nbits` of them (1..1008, the largest AIS message
// is five slots). OUTPUT: the sentence(s), CRLF-terminated:
//
//   !AIVDM,<fragment count>,<fragment number>,<sequential id>,<channel>,<payload>,<fill bits>*<checksum>
//
//   * PAYLOAD ARMOURING: six bits per character, value v -> the character 48 + v, plus 8 more when that is above 87 ('W'), i.e. 0..39 map
//     to '0'..'W' and 40..63 to '`'..'w'. The last character is padded with zero bits; the number of pad bits (0..5) is the fill field.
//   * MULTI-SENTENCE: a payload longer than `maxPayloadChars` (56 by default, the split the worked example uses; at most 60, which keeps
//     a sentence within the 82 characters NMEA allows) is split on character boundaries; every fragment carries the same sequential id
//     (a single digit, 0..9, rolled by AisOut), the fragment count and its own number from 1; only the last carries fill bits. A
//     single-sentence message has an EMPTY sequential id field.
//   * CHANNEL: 'A', 'B', or empty when unknown.
//   * CHECKSUM: the XOR of every character between '!' and '*', two upper-case hex digits.
//
// SOURCES (read 2026-11-11): "AIVDM/AIVDO protocol decoding", https://gpsd.gitlab.io/gpsd/AIVDM.html - a format-description page (the
// armouring table, the field layout, fill bits, the two worked examples). The page is the documentation of a GPL program; no program
// source was read. It leaves the checksum's range ambiguous in its prose; the worked examples settle it (XOR between '!' and '*',
// reproduced by the tests). Default TCP port 10110 is the usual NMEA-over-network port: UNVERIFIED (not read from a standard).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_AIS_OUT_HPP
#define CASCADE_NET_AIS_OUT_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include "net/tcp_fanout.hpp"
#include "net/udp_sink.hpp"

namespace cascade::net {

inline constexpr std::uint16_t kAisTcpPort = 10110;
inline constexpr std::size_t kAisMaxBits = 1008;
inline constexpr int kAisMaxFragments = 9;
inline constexpr int kAisDefaultPayloadChars = 56;
inline constexpr int kAisMaxPayloadChars = 60;
inline constexpr std::size_t kAisMaxSentenceBytes = 96;

struct AisSentenceOptions {
    const char* talker = "AIVDM";    // "AIVDM" (heard from others) or "AIVDO" (own vessel)
    char channel = 'A';              // 'A', 'B' or 0 for an empty field
    int sequenceId = 0;              // 0..9; used only when the message needs more than one sentence
    int maxPayloadChars = kAisDefaultPayloadChars;  // 1..60
};

struct AisSentences {
    char text[kAisMaxFragments][kAisMaxSentenceBytes];
    std::size_t length[kAisMaxFragments] = {};
    int count = 0;
};

// Encodes `nbits` bits (packed MSB first in `bits`) into sentences. Returns the count (0 on refusal: null bits, nbits 0 or above
// kAisMaxBits, bad talker, channel not A/B/0, sequence id outside 0..9, maxPayloadChars outside 1..60, or more than 9 fragments).
// Allocation-free.
int aisEncode(const std::uint8_t* bits, std::size_t nbits, const AisSentenceOptions& opt, AisSentences& out) noexcept;

// The XOR checksum of a sentence's characters between '!' (or '$') and '*'.
std::uint8_t nmeaChecksum(const char* s, std::size_t n) noexcept;

// TCP and UDP together. Either may be absent. submit() is the producer's call and is wait-free.
class AisOut {
public:
    AisOut(const FanoutConfig& tcp, const UdpConfig& udp, bool useTcp, bool useUdp) : tcp_(tcp), udp_(udp), useTcp_(useTcp), useUdp_(useUdp) {}
    bool start(std::string& error);
    void stop();
    // PRODUCER THREAD ONLY. Returns the number of sentences handed over (0 when refused). `channel` as in AisSentenceOptions.
    int submit(const std::uint8_t* bits, std::size_t nbits, char channel = 'A') noexcept;
    std::uint16_t tcpPort() const { return tcp_.port(); }
    FanoutStats tcpStats() const { return tcp_.stats(); }
    UdpStats udpStats() const { return udp_.stats(); }

private:
    TcpFanout tcp_;
    UdpSink udp_;
    bool useTcp_, useUdp_;
    int nextSeq_ = 0;  // producer thread only
};

}  // namespace cascade::net

#endif
