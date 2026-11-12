// sdriq_protocol.hpp - the RFspace SDR-IQ message layer: item numbers, parameter encodings, the output-rate
// codes and the I/Q data block, written from RFspace/MoeTronix's own "SDR-IQ Interface Specification" Ver. 1.04
// (18 Dec 2013, https://moetronix.com/files/sdriqinterfacespec104.pdf, opened and read 2026-11-12; the document
// states no licence and is published for implementers). The framing is ASCP, the same as the network radios, so
// the header, the stream assembler and the acknowledgement are src/source/ascp_protocol.hpp's.
//
// WHAT THE DOCUMENT SAYS, AND WHERE (section numbers are the document's):
//   * 3: 13-bit length, 3-bit type, little endian; a Data Item length of 0 means 8194 bytes.
//   * 5.1: items 0x0001 name (zero-terminated "SDR-IQ"), 0x0002 serial, 0x0003 interface version x100,
//     0x0005 status codes 0x0B idle / 0x0C busy / 0x0D loading AD6620 / 0x0E boot idle / 0x0F boot busy /
//     0x20 A/D overload / 0x80 boot error.
//   * 5.2.1: 0x0018 receiver state: channel 0x81, 0x01 idle / 0x02 run, capture mode 0 contiguous (2 one shot,
//     3 hardware trigger: not used here), N blocks 1..128 (ignored for contiguous).
//   * 5.2.2: 0x0020 frequency, channel byte then five bytes in Hz, 0 to 33,333,333 (the range request answers
//     0 to 30 MHz: that answer is what this layer trusts).
//   * 5.2.4: 0x00B8 output I/Q rate, only seven values: 8138, 16276, 37793, 55556, 111111, 158730, 196078 Hz
//     (66,666,667 Hz / 8192, 4096, 1764, 1200, 600, 420, 340); "any other values will be ignored"; not while
//     running; power-up default 196078.
//   * 5.2.5: 0x0038 RF gain, mode 0 fixed: 0, -10, -20, -30 dB (as a signed byte).
//   * 5.2.6: 0x0040 IF gain, mode 0, 0 / 6 / 12 / 18 / 24 dB.
//   * 5.4.1: Data Item 0: 8194 bytes, header 00 80, then 8192 bytes = I1 Q1 I2 Q2 ... as signed 16-bit
//     little endian.
// NOT USED, ON PURPOSE: the firmware-update items (0x0300..0x0302) - this source never sends them and the test
// stand-in fails if it does; the RS232 pass-through (0x0200/0x0201, data item 2); the security code (0x000B);
// the hardware-trigger and one-shot capture modes; the AD6620 register download (data item 1: the 0x00B8 rate
// message does that job); the A/D clock calibration item (0x00B0).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

#include "source/ascp_protocol.hpp"

namespace cascade::source::sdriq {

namespace ci {
constexpr std::uint16_t Name = 0x0001;
constexpr std::uint16_t Serial = 0x0002;
constexpr std::uint16_t InterfaceVersion = 0x0003;
constexpr std::uint16_t Status = 0x0005;
constexpr std::uint16_t RxState = 0x0018;
constexpr std::uint16_t RxFrequency = 0x0020;
constexpr std::uint16_t RfGain = 0x0038;
constexpr std::uint16_t IfGain = 0x0040;
constexpr std::uint16_t OutSampleRate = 0x00B8;
}  // namespace ci

constexpr std::uint8_t kChannel = 0x81;      // 5.2.1: the only allowed receiver channel ID
constexpr std::uint8_t kRunIdle = 0x01;
constexpr std::uint8_t kRunRun = 0x02;
constexpr std::uint8_t kCaptureContiguous = 0x00;
constexpr std::uint8_t kBlocksIgnored = 0x01;

constexpr std::uint8_t kStatusIdle = 0x0B, kStatusBusy = 0x0C, kStatusLoading = 0x0D, kStatusBootIdle = 0x0E,
                       kStatusBootBusy = 0x0F, kStatusOverload = 0x20, kStatusBootError = 0x80;

constexpr std::size_t kDataBlockBytes = 8194;  // header + 8192
constexpr std::size_t kDataBlockPairs = 2048;

constexpr double kAdClockHz = 66666667.0;
constexpr std::uint64_t kMaxFrequencyHz = 33333333ull;  // 5.2.2 (the range request narrows it to 30 MHz)

struct RateCode {
    std::uint32_t code;     // the value the 0x00B8 message carries (Hz)
    std::uint32_t divisor;  // 66,666,667 / divisor
};
constexpr RateCode kRates[7] = {{8138, 8192},  {16276, 4096},  {37793, 1764}, {55556, 1200},
                                {111111, 600}, {158730, 420}, {196078, 340}};
constexpr std::uint32_t kPowerUpRate = 196078;

// The rate code for a requested rate (within half a hertz of the code or of 66,666,667 / divisor); false for
// anything else - the radio would ignore it.
inline bool rateCodeFor(double hz, std::uint32_t& code) {
    for (const RateCode& r : kRates) {
        const double exact = kAdClockHz / static_cast<double>(r.divisor);
        if ((hz >= r.code - 0.5 && hz <= r.code + 0.5) || (hz >= exact - 0.5 && hz <= exact + 0.5)) {
            code = r.code;
            return true;
        }
    }
    return false;
}
inline double rateHzFor(std::uint32_t code) {
    for (const RateCode& r : kRates) {
        if (r.code == code) { return kAdClockHz / static_cast<double>(r.divisor); }
    }
    return 0.0;
}

// ---- parameter builders (the bodies of Set messages) ---------------------------------------------------
inline std::vector<std::uint8_t> runParams(bool run) {
    return {kChannel, run ? kRunRun : kRunIdle, kCaptureContiguous, kBlocksIgnored};
}
inline std::vector<std::uint8_t> frequencyParams(std::uint64_t hz) {
    std::vector<std::uint8_t> p{0x00};
    ascp::putFrequency(p, hz);
    return p;
}
inline std::vector<std::uint8_t> rateParams(std::uint32_t code) {
    std::vector<std::uint8_t> p{0x00};
    ascp::putLe(p, code, 4);
    return p;
}
inline std::vector<std::uint8_t> rfGainParams(int db) { return {0x00, static_cast<std::uint8_t>(static_cast<std::int8_t>(db))}; }
inline std::vector<std::uint8_t> ifGainParams(int db) { return {0x00, static_cast<std::uint8_t>(db)}; }

inline bool validRfGainDb(int db) { return db == 0 || db == -10 || db == -20 || db == -30; }
inline bool validIfGainDb(int db) { return db == 0 || db == 6 || db == 12 || db == 18 || db == 24; }

// 5.2.2: the answer to the frequency RANGE request is the channel byte, then minimum and maximum, five bytes
// each - ten bytes, no count byte (unlike the NetSDR's list of bands).
inline bool parseRange(const std::vector<std::uint8_t>& body, ascp::FreqRange& out) {
    if (body.size() != 11) { return false; }
    out.minHz = ascp::getFrequency(body.data() + 1);
    out.maxHz = ascp::getFrequency(body.data() + 6);
    return out.maxHz >= out.minHz;
}

// The zero-terminated string of a name or serial body, printable ASCII only, at most 32 characters; false for
// anything else (a name is quoted to the user, so it is cleaned before it is believed).
inline bool parseCString(const std::vector<std::uint8_t>& body, std::string& out) {
    out.clear();
    for (std::uint8_t b : body) {
        if (b == 0) { return !out.empty(); }
        if (b < 0x20 || b > 0x7E || out.size() >= 32) { return false; }
        out.push_back(static_cast<char>(b));
    }
    return false;  // no terminator
}

// 5.4.1: the body of one Data Item 0 block (8192 bytes) as complex samples, full scale 1.0. False, appending
// nothing, for any other size.
inline bool decodeBlock(const std::vector<std::uint8_t>& body, std::vector<std::complex<float>>& out) {
    if (body.size() != kDataBlockBytes - 2) { return false; }
    out.reserve(out.size() + kDataBlockPairs);
    for (std::size_t i = 0; i + 4 <= body.size(); i += 4) {
        const std::int16_t a = static_cast<std::int16_t>(body[i] | (body[i + 1] << 8));
        const std::int16_t b = static_cast<std::int16_t>(body[i + 2] | (body[i + 3] << 8));
        out.emplace_back(static_cast<float>(a) / 32768.0f, static_cast<float>(b) / 32768.0f);
    }
    return true;
}

}  // namespace cascade::source::sdriq
