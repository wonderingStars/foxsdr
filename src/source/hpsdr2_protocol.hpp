// hpsdr2_protocol.hpp - the openHPSDR "protocol 2" (Ethernet protocol) packets as plain functions: discovery,
// the General, DDC-Specific and High-Priority packets a receive-only host sends, and the DDC I&Q and
// High-Priority-Status packets it reads. No sockets here (hpsdr2_source.hpp has them).
//
// PROVENANCE. "openHPSDR Ethernet Protocol", V4.4 (March 2019 title page; its revision list runs to a
// 7/3/2026 entry; Phil Harman VK6PH and contributors), the project's own document, in
// https://github.com/TAPR/OpenHPSDR-Firmware, "Protocol 2/Documentation/openHPSDR Ethernet Protocol v4.4.pdf"
// (2.3 MB PDF, plain, no protection; fetched and read on 2026-11-11 with pdftotext). The repository's
// LICENSE is GPL-3.0, which covers the gateware, none of which was opened; the document states no licence
// of its own. Nothing from piHPSDR, Thetis, SDR++ or any other program was read. Sections used: Detailed
// operation (discovery, the order General -> DDC Specific -> High Priority with the run bit, "a Command &
// Control packet must be sent from the host to the hardware at least every second (every 100 mS is
// recommended)"), DISCOVERY PACKET, GENERAL PACKET TO SDR, DDC SPECIFIC PACKET, HIGH PRIORITY DATA PACKET,
// DISCOVERY REPLY PACKET, HIGH PRIORITY STATUS PACKET, DDC PACKET, and Appendix A for boards 1 to 4.
//
// WHAT THE DOCUMENT GIVES, AS USED HERE (all integers big-endian)
//   * Discovery: 60 bytes, sequence 0, byte 4 = 0x02, the rest zero, to UDP port 1024 of one device or the
//     broadcast address. Reply (from port 1024, to the asker's source port): sequence 0, byte 4 = 0x02 (0x03
//     when running and connected to another host; 0xFE / 0xFF announce a long hardware description that this
//     build does not parse), bytes 5-10 MAC, 11 board type (1 Hermes, 2 Hermes 10E/100B, 3 Angelia, 4 Orion,
//     5 Orion Mk II, 6 Hermes Lite, 10 Saturn), 12 protocol version, 13 firmware version, 20 number of DDCs,
//     21 frequency (0) or phase word (1), 22 endian/format modes, 23 beta flag.
//   * General packet (to port 1024, 60 bytes): sequence, byte 4 = 0x00, bytes 5-22 the UDP ports (zero =
//     default: DDC specific 1025, DUC specific 1026, high priority from PC 1027, high priority to PC 1025,
//     DDC audio 1028, DUC I&Q 1029, DDC0 1035 and one more per DDC, mic 1026, wideband 1027), byte 23
//     wideband enable, byte 37 bit 3 = send frequencies as a PHASE WORD, byte 38 bit 0 = hardware reset timer
//     enabled ("should normally be set"), byte 39 = 0 for big-endian 3-byte I&Q, byte 58 bit 0 = PA enable,
//     byte 59 = Alex enables.
//   * DDC Specific packet (to the DDC-specific port, 1444 bytes): byte 4 number of ADCs, 5 dither, 6 random,
//     7.. DDC enable bits (bit 0 of byte 7 = DDC0), then six bytes per DDC from byte 17: ADC (17+6k), sampling
//     rate in ksps (18+6k, 19+6k: valid 48 / 96 / 192 / 384 / 768 / 1536), two bytes of CIC "for future use",
//     sample size in bits (22+6k, default 24), and the sync bits from byte 1363.
//   * High Priority packet (to the high-priority port, 1444 bytes): byte 4 bit 0 = run, bit 1 = PTT (never
//     set here), bytes 9-12 the frequency or phase word of DDC0 (13-16 DDC1 ...), byte 345 the DUC0 drive
//     level (never set here), byte 1443 the step attenuator before ADC0 (0 to 31 dB), byte 1442 ADC1.
//   * Phase word = 2^32 * frequency / DSP clock; Appendix A gives the DSP clock as 122.88 MHz for boards 1
//     to 4 and says boards 1 to 4 take a phase word; the reply's byte 21 says it too.
//   * DDC I&Q packet (from the DDC port, default 1035 for DDC0): sequence (4), time stamp (8), bits per
//     sample (2), I&Q samples per frame (2), then I (3 bytes) Q (3 bytes) per sample, "signed integer 2's
//     Complement"; 238 samples make 1444 bytes. Sequence counts per port from 0 and restarts at a Stop.
//   * High Priority Status (from port 1025 by default, 60 bytes): sequence, byte 4 bits (PTT, dot, dash,
//     PLL locked), byte 5 ADC overload bits (bit n = ADC n), byte 30 FIFO overflow bits (bit 0 = DDC FIFO).
//
// WHAT THE DOCUMENT DOES NOT SAY (a stated choice here): the actual DDC / ADC limits of Orion Mk II, Hermes
// Lite and Saturn (Appendix A has tables for boards 1 to 4 only, so those are refused); which of the three
// CIC bytes a board wants (zero is sent: "for future use"); how the I/Q sense relates to the spectrum (the
// document says the spectrum "may appear reversed" and to swap then: no swap is applied here).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cascade::source::hpsdr2 {

constexpr std::uint16_t kDiscoveryPort = 1024;          // also where the General packet goes
constexpr std::uint16_t kDefaultDdcSpecificPort = 1025;
constexpr std::uint16_t kDefaultHighPriorityToHardwarePort = 1027;
constexpr std::uint16_t kDefaultHighPriorityFromHardwarePort = 1025;
constexpr std::uint16_t kDefaultDdc0Port = 1035;
constexpr std::size_t kDiscoveryBytes = 60;
constexpr std::size_t kGeneralBytes = 60;
constexpr std::size_t kDdcSpecificBytes = 1444;
constexpr std::size_t kHighPriorityBytes = 1444;
constexpr std::size_t kDdcPacketBytes = 1444;           // 16 header + 238 * 6
constexpr std::size_t kDdcHeaderBytes = 16;
constexpr int kMaxDdcSamples = 238;
constexpr std::size_t kStatusBytes = 60;

struct BoardInfo {
    bool documented = false;   // Appendix A has a table for it
    const char* name = "unknown board";
    int adcs = 0;
    double dspClockHz = 0.0;
};
BoardInfo boardInfo(std::uint8_t type);

std::vector<std::uint8_t> buildDiscovery();

struct DiscoveryReply {
    bool busy = false;                 // byte 4 = 0x03
    std::array<std::uint8_t, 6> mac{};
    std::uint8_t boardType = 0;
    std::uint8_t protocolVersion = 0;
    std::uint8_t firmwareVersion = 0;
    std::uint8_t numDdcs = 0;
    bool phaseWord = false;
    std::uint8_t formatModes = 0;      // 0 = big-endian 3-byte only
    bool beta = false;
    std::string macText() const;
};
// Refuses a datagram shorter than 24 bytes, a status byte other than 0x02 / 0x03 (0xFE and 0xFF, the long
// hardware descriptions, are not parsed), a non-zero sequence number. Bytes beyond 24 are not interpreted.
bool parseDiscoveryReply(const std::uint8_t* d, std::size_t n, DiscoveryReply& out);

// The ports the host asks the device to use (zero = the document's default).
struct PortPlan {
    std::uint16_t ddcSpecific = 0;
    std::uint16_t highPriorityToHardware = 0;
    std::uint16_t highPriorityFromHardware = 0;
    std::uint16_t ddc0 = 0;
    std::uint16_t effectiveDdcSpecific() const { return ddcSpecific ? ddcSpecific : kDefaultDdcSpecificPort; }
    std::uint16_t effectiveHighPriorityToHardware() const { return highPriorityToHardware ? highPriorityToHardware : kDefaultHighPriorityToHardwarePort; }
    std::uint16_t effectiveHighPriorityFromHardware() const { return highPriorityFromHardware ? highPriorityFromHardware : kDefaultHighPriorityFromHardwarePort; }
    std::uint16_t effectiveDdc0() const { return ddc0 ? ddc0 : kDefaultDdc0Port; }
};

// 60 bytes. `phaseWord` sets byte 37 bit 3; the reset timer bit (byte 38 bit 0) is set; byte 39 = 0 (big
// endian, 3-byte I&Q); wideband, PA and Alex are off.
std::vector<std::uint8_t> buildGeneral(std::uint32_t seq, const PortPlan& ports, bool phaseWord);

// The rates byte 18/19 can carry, in samples per second: 48000 ... 1536000.
const std::array<int, 6>& supportedRates();
bool rateToKsps(int rateHz, std::uint16_t& ksps);

// 1444 bytes: `adcs` in byte 4 (1..8), DDC0 enabled, on ADC0, at `rateHz`, 24-bit samples. False (empty
// vector) for a rate outside the six or adcs outside 1..8.
std::vector<std::uint8_t> buildDdcSpecific(std::uint32_t seq, int adcs, int rateHz);

// Frequency word of DDC0: Hz as is, or 2^32 * f / clock rounded to nearest. `ok` false when the value does
// not fit 32 bits (f < 0, f >= clock for a phase word, f > 4294967295 as Hz) or the clock is not positive.
std::uint32_t frequencyWord(double hz, bool phaseWord, double dspClockHz, bool& ok);

// 1444 bytes: run bit, DDC0 word, ADC0 attenuator 0..31 dB. PTT, drive and every transmit-side byte stay 0.
std::vector<std::uint8_t> buildHighPriority(std::uint32_t seq, bool run, std::uint32_t ddc0Word, int attenuatorDb);

enum class DdcVerdict {
    Ok,
    WrongLength,   // fewer than 16 bytes, or the length is not 16 + 6 * samples
    BadSampleSize, // bits per sample is not 24
    BadCount,      // zero samples, or more than 238
};
const char* verdictName(DdcVerdict v);

struct DdcPacket {
    std::uint32_t sequence = 0;
    std::uint16_t samples = 0;
    const std::uint8_t* data = nullptr;  // the I/Q bytes, into the caller's buffer
};
DdcVerdict parseDdcPacket(const std::uint8_t* d, std::size_t n, DdcPacket& out);
// Appends the samples (I then Q, two's complement, scaled by 2^-23).
void decodeDdcSamples(const DdcPacket& p, std::vector<std::complex<float>>& out);
// A packet for tests and tools: `count` samples of 24-bit I/Q, sequence, zero time stamp.
std::vector<std::uint8_t> buildDdcPacket(std::uint32_t seq, const std::int32_t* iq, int count);

struct Status {
    std::uint32_t sequence = 0;
    bool pllLocked = false;
    std::uint8_t adcOverload = 0;     // bit n = ADC n
    bool ddcFifoOverflow = false;     // byte 30 bit 0
};
bool parseStatus(const std::uint8_t* d, std::size_t n, Status& out);  // exactly 60 bytes

}  // namespace cascade::source::hpsdr2
