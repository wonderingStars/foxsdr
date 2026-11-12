// hpsdr_protocol.hpp - the openHPSDR "protocol 1" (Metis) UDP framing as plain functions (survey row
// R7, HPSDR family): discovery, start/stop, and the data packet envelope. No sockets here.
//
// PROVENANCE. "Metis - How it works", V1.33 (revision history to 1.29, 31 Dec 2012; author VK6APH, Phil
// Harman), https://github.com/TAPR/OpenHPSDR-Firmware, file "Protocol 1/Documentation/Metis- How it
// works_V1.33.pdf" (18 pages, 76 KB, fetched and read in full 2026-11-11; the archive revisions V1.25,
// V1.31 were compared for the start/stop table). The document is freely readable on a public repository;
// the repository's LICENSE is GPL-3.0, which covers the gateware, none of which was read; the document
// itself states no licence. The Hermes User Manual V1.18 (same repository) was searched for the frame
// layout and does not hold it. Nothing from piHPSDR, Thetis, PowerSDR, linHPSDR or any other program was
// read, and the Hermes-Lite 2 wiki and the OpenHPSDR-SVN "USB protocol" .doc files were NOT read: neither
// repository states a licence, so neither could be confirmed readable.
//
// WHAT THE DOCUMENT GIVES (all integers big-endian, "network order", where it says so):
//   * everything is UDP, device port 1024; payloads start with the two bytes 0xEF 0xFE.
//   * Discovery request: EF FE 02 + 60 bytes of 00 (63 bytes). The document says the request goes to
//     the subnet broadcast address but that this "has not currently been implemented": send it to
//     255.255.255.255 port 1024.
//   * Discovery reply: EF FE <Status> <MAC 6 bytes> <Code version 1> <Board_ID 1> + 49 bytes of 00
//     (60 bytes). Status 0x02 = not sending data, 0x03 = sending. Board_ID: 0x00 Metis, 0x01 Hermes,
//     0x02 Griffin, 0x04 Angelia, 0x05 Orion, 0x06 Hermes_Lite. The device's IP is the datagram's source.
//     The device answers the "from port" of the asker; every Metis on the network answers.
//   * Start / Stop: EF FE 04 <Command> + 60 bytes of 00 (64 bytes). Command bit 0 set starts I&Q + mic
//     data, bit 1 set starts the wide bandscope data; clear stops the same. Data goes to the "from port"
//     and IP of whoever sent Start. The device stops if it receives "Destination Unreachable". The
//     sequence number the PC sends is reset to zero at each Start; the device's are reset by Stop.
//   * Data, both directions: EF FE 01 <End point 1 byte> <Sequence 4 bytes big-endian> <1024 bytes = two
//     512-byte USB-format frames> (1032 bytes). PC to device: end point 0x02. Device to PC: 0x06 for
//     I&Q + mic, 0x04 for the wide bandscope; sequence numbers count from zero per end point.
//
// WHAT THE DOCUMENT DOES NOT GIVE, so is NOT BUILT: the layout INSIDE a 512-byte frame (sync bytes, the
// command-and-control bytes that carry frequency, sample rate and gain, the sample widths and the
// receiver interleave). The guide points at a separate "USB protocol proposal" document
// (svn://svn.openhpsdr.org/svn/repos_sdr_hpsdr/trunk/Documentation) which was not opened. Receive
// sample unpacking, and the frequency, rate and gain commands, are therefore not implemented; the source
// takes a frame decoder from its caller. Fobos is left out: libfobos is LGPL-2.1 and no free API
// document was opened.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cascade::source::hpsdr {

constexpr std::uint16_t kDevicePort = 1024;
constexpr std::size_t kDiscoveryRequestBytes = 63;
constexpr std::size_t kDiscoveryReplyBytes = 60;
constexpr std::size_t kCommandBytes = 64;
constexpr std::size_t kDataPacketBytes = 1032;
constexpr std::size_t kFrameBytes = 512;
constexpr std::uint8_t kEndpointToDevice = 0x02;
constexpr std::uint8_t kEndpointIq = 0x06;
constexpr std::uint8_t kEndpointBandscope = 0x04;
constexpr std::uint8_t kStartIq = 0x01;        // bit 0
constexpr std::uint8_t kStartBandscope = 0x02;  // bit 1
constexpr std::uint8_t kStopAll = 0x00;

const char* boardName(std::uint8_t id);  // "Metis", "Hermes", ... or "unknown board"

std::vector<std::uint8_t> buildDiscoveryRequest();
// `command` = the bits to set: kStartIq, kStartBandscope or both; the stop form is command 0
// (kStopAll) or the bit cleared.
std::vector<std::uint8_t> buildStartStop(std::uint8_t command);

struct DiscoveryReply {
    bool sending = false;            // status 0x03
    std::array<std::uint8_t, 6> mac{};
    std::uint8_t codeVersion = 0;
    std::uint8_t boardId = 0;
    std::string macText() const;     // "aa:bb:cc:dd:ee:ff"
};

// Refuses (false) a datagram shorter than the 11 bytes the fields need, without the 0xEF 0xFE magic, or
// with a status other than 0x02 or 0x03. Bytes beyond the 11 are not interpreted (newer boards may use
// them).
bool parseDiscoveryReply(const std::uint8_t* d, std::size_t n, DiscoveryReply& out);

struct DataPacket {
    std::uint8_t endpoint = 0;
    std::uint32_t sequence = 0;
    const std::uint8_t* frames = nullptr;  // 1024 bytes, into the caller's buffer
};

// Refuses anything that is not exactly 1032 bytes with EF FE 01 and a known end point (0x02, 0x04, 0x06).
bool parseDataPacket(const std::uint8_t* d, std::size_t n, DataPacket& out);

// A data packet for tests and the PC-to-device direction. `frames` must be 1024 bytes.
std::vector<std::uint8_t> buildDataPacket(std::uint8_t endpoint, std::uint32_t sequence,
                                          const std::uint8_t* frames1024);

// Per-end-point sequence accounting: how many packets were lost (a gap forward) or arrived late or
// twice (a step back), without ever stopping the stream. reset() at each Start.
class SequenceTracker {
public:
    void reset() { expected_.clear(); lost_ = 0; late_ = 0; }
    // Returns the number of packets missing before this one (0 when in order).
    std::uint32_t note(std::uint8_t endpoint, std::uint32_t sequence);
    std::uint64_t lost() const { return lost_; }
    std::uint64_t late() const { return late_; }

private:
    std::map<std::uint8_t, std::uint32_t> expected_;
    std::uint64_t lost_ = 0, late_ = 0;
};

}  // namespace cascade::source::hpsdr
