// ascp_protocol.hpp - the control-and-data message format shared by RFspace's network radios (NetSDR,
// SDR-IP, Cloud-IQ) and by the Afedri SDR-Net family, which speaks the same "SDR-IP" protocol: framing of
// the TCP control messages, the control-item numbers this client uses, and the UDP I/Q packet layout. No
// sockets here.
//
// PROVENANCE (all opened and read 2026-11-11; nothing downloaded except the documents themselves, into a
// scratch folder outside the repository):
//  * CuteSDR 1.20, Moe Wheatley, Simplified BSD licence, files interface/ascpmsg.h, protocoldefs.h,
//    sdrinterface.cpp, netiobase.cpp, dataprocess.cpp, gui/sdrsetupdlg.cpp, read at
//    https://sources.debian.org/src/cutesdr/1.20-4/interface/ . The licence was checked FIRST in
//    https://sources.debian.org/data/main/c/cutesdr/1.20-4/debian/copyright : "interface/*" and "gui/*"
//    are "MOEBSD", the Simplified BSD licence ("Redistribution and use in source and binary forms, with
//    or without modification, are permitted provided that ..." and the disclaimer); the same licence
//    text sits at the top of each file read. FoxSDR's reproduction of the NOTICE required by that licence
//    is the paragraph you are reading plus docs/feature-records/radio-families.md: "Copyright 2010 Moe
//    Wheatley. All rights reserved. THIS SOFTWARE IS PROVIDED BY Moe Wheatley AS IS AND ANY EXPRESS OR
//    IMPLIED WARRANTIES ... ARE DISCLAIMED." Only the byte layouts and numbers (facts of the interface)
//    are used; no code is copied. The programme's siqs/* directory (GPL) and the Debian packaging (GPL)
//    were not read.
//  * Afedri, "AFE822x SDR-Net protocol extensions" (8 pages, https://afedri-sdr.com/index.php/downloads/
//    category/2-documentation?download=242), manufacturer's document, no licence stated: the SDR-IP
//    control header examples, the frequency / RF-gain / main-clock items and the UDP data stream picture.
//  * Afedri, "SDR Network Control Box software description" rev 6 (23 pages, download=193): default
//    destination port 50000 for both TCP and UDP, the "Multi Stream" second port (+1), sample-rate rules.
//  * RFspace, NetSDR User's Guide (Jan 2012, http://www.rfspace.com/support_files/NetSDR_Guide_0112.pdf):
//    "RFSPACE supplies a fully documented protocol" and "Please contact info@rfspace.com for the latest
//    version of the NetSDR Interface Specification Document". The specification itself is only on
//    request (http://www.rfspace.com/RFSPACE/Support.html: "SDR-IP protocol specifications, NetSDR
//    protocol specifications" are listed as available on request); it was NOT obtained. Everything below
//    that is not in the Afedri documents comes from CuteSDR's implementation, which is a working client.
//
// MESSAGE FORMAT (little-endian everywhere). A control message is a 16-bit header, then (for items) a
// 16-bit control-item number, then the item's parameters. Header bits 0..12 = the message length in
// bytes INCLUDING the header; bits 13..15 = the type: host->radio 0 set, 1 request current, 2 request
// range, 3 data-item ACK (3 bytes: header + channel), 4..7 data items; radio->host 0 response, 1
// unsolicited, 2 range response, 3 ACK, 4..7 data items. A radio answers an item it does not implement
// with the two-byte message 02 00 (a NAK; it does not say which request it answers). Length field 0 on a
// data message means 8194 bytes. The afedri example headers decode the same way: 0A 00 = set, length 10;
// 04 20 = request, length 4; 84 04 (UDP data) = type 4, length 0x404 = 1028.
//
// UDP I/Q PACKETS: header (2 bytes, type 4 / length = the datagram's length), a 16-bit sequence number
// (little-endian), then interleaved I,Q: 16-bit signed (datagram 1028 bytes "large" or 516 "small") or
// 24-bit signed little-endian (1444 or 388). Sequence: the first packet after a start is 0, then 1, 2 ...
// 65535, and after 65535 the counter goes to 1 (it skips 0).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cascade::source::ascp {

constexpr std::uint16_t kDefaultPort = 50000;  // Afedri control-box document 3.3.9.1; the NetSDR guide's own example

enum class MsgType : std::uint8_t { Set = 0, Request = 1, RequestRange = 2, DataAck = 3, Data0 = 4, Data1 = 5, Data2 = 6, Data3 = 7 };

// Control items (CuteSDR protocoldefs.h; the Afedri document confirms 0x0020, 0x0038 and 0x00B0).
namespace ci {
constexpr std::uint16_t InterfaceName = 0x0001;
constexpr std::uint16_t SerialNumber = 0x0002;
constexpr std::uint16_t HardFirmVersion = 0x0004;
constexpr std::uint16_t StatusCode = 0x0005;
constexpr std::uint16_t ProductId = 0x0009;
constexpr std::uint16_t RxState = 0x0018;
constexpr std::uint16_t RxChanSetup = 0x0019;
constexpr std::uint16_t RxFrequency = 0x0020;
constexpr std::uint16_t RxRfGain = 0x0038;
constexpr std::uint16_t RxRfFilter = 0x0044;
constexpr std::uint16_t RxAdModes = 0x008A;
constexpr std::uint16_t RxInSampleRate = 0x00B0;  // Afedri: "main clock frequency"
constexpr std::uint16_t RxSyncInMode = 0x00B4;
constexpr std::uint16_t RxPulseOutMode = 0x00B6;
constexpr std::uint16_t RxOutSampleRate = 0x00B8;
constexpr std::uint16_t RxOutputParams = 0x00C4;
}  // namespace ci

constexpr std::uint8_t kChannel1 = 0;
constexpr std::uint8_t kRxStateComplex = 0x80;   // parameter 1 of RxState: complex data
constexpr std::uint8_t kRxIdle = 0x01;
constexpr std::uint8_t kRxOn = 0x02;
constexpr std::uint8_t kModeContiguous24 = 0x80;
constexpr std::uint8_t kModeContiguous16 = 0x00;
constexpr std::uint8_t kOutputLargeMtu = 0;
constexpr std::uint8_t kOutputSmallMtu = 1;
constexpr std::uint8_t kRfFilterAuto = 0;
constexpr std::uint8_t kRfFilterBypass = 11;
constexpr std::uint8_t kAdDither = 0x01;
constexpr std::uint8_t kAdPga = 0x02;
constexpr std::uint8_t kChanSetupSingle1 = 0;
constexpr std::uint8_t kSyncInOff = 0;
constexpr std::uint8_t kPulseOutOff = 0;
constexpr std::uint8_t kUdpKeepaliveByte = 0x5A;  // the one-byte datagram CuteSDR sends to the radio's UDP port

constexpr std::size_t kMaxMessage = 8192 + 2;
constexpr std::size_t kUdpLarge16 = 1028, kUdpSmall16 = 516, kUdpLarge24 = 1444, kUdpSmall24 = 388;

inline void putLe(std::vector<std::uint8_t>& v, std::uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) { v.push_back(static_cast<std::uint8_t>(value >> (8 * i))); }
}
inline std::uint64_t getLe(const std::uint8_t* p, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) { v |= static_cast<std::uint64_t>(p[i]) << (8 * i); }
    return v;
}

// A control message: header, item, parameters. Empty when the result would not fit the 13-bit length.
inline std::vector<std::uint8_t> encodeMessage(MsgType type, std::uint16_t item, const std::vector<std::uint8_t>& params = {}) {
    const std::size_t len = 4 + params.size();
    if (len > 0x1FFF) { return {}; }
    std::vector<std::uint8_t> m;
    putLe(m, (static_cast<std::uint32_t>(type) << 13) | static_cast<std::uint32_t>(len), 2);
    putLe(m, item, 2);
    m.insert(m.end(), params.begin(), params.end());
    return m;
}

// The 3-byte data-item acknowledgement: header + channel.
inline std::vector<std::uint8_t> encodeDataAck(std::uint8_t channel) {
    std::vector<std::uint8_t> m;
    putLe(m, (static_cast<std::uint32_t>(MsgType::DataAck) << 13) | 3u, 2);
    m.push_back(channel);
    return m;
}

// One message received on the TCP connection.
struct Message {
    MsgType type = MsgType::Set;
    bool nak = false;              // the two-byte "not implemented" message
    std::uint16_t item = 0;        // control types only
    std::vector<std::uint8_t> body;  // the parameters (control types) or everything after the header (others)
};

// Cuts the TCP byte stream into messages. A malformed header makes error() true and the assembler
// stops (the connection is then not trustworthy).
class StreamAssembler {
public:
    // Appends complete messages to `out`.
    void feed(const std::uint8_t* d, std::size_t n, std::vector<Message>& out) {
        if (!error_.empty()) { return; }
        for (std::size_t i = 0; i < n; ++i) {
            buf_.push_back(d[i]);
            if (buf_.size() < 2) { continue; }
            if (!haveHeader_) {
                const std::uint16_t h = static_cast<std::uint16_t>(buf_[0] | (buf_[1] << 8));
                type_ = static_cast<MsgType>(h >> 13);
                len_ = h & 0x1FFF;
                if (len_ == 0 && static_cast<unsigned>(type_) >= 4) { len_ = kMaxMessage; }
                if (len_ < 2 || len_ > kMaxMessage) { error_ = "a message length of " + std::to_string(len_) + " bytes is impossible"; return; }
                if (len_ == 3 && type_ != MsgType::DataAck) { error_ = "a 3-byte message that is not an acknowledgement"; return; }
                haveHeader_ = true;
            }
            if (buf_.size() == len_) {
                Message m;
                m.type = type_;
                if (len_ == 2) {
                    m.nak = true;
                } else if (static_cast<unsigned>(type_) <= 2) {
                    m.item = static_cast<std::uint16_t>(buf_[2] | (buf_[3] << 8));
                    m.body.assign(buf_.begin() + 4, buf_.end());
                } else {
                    m.body.assign(buf_.begin() + 2, buf_.end());
                }
                out.push_back(std::move(m));
                buf_.clear();
                haveHeader_ = false;
            }
        }
    }
    const std::string& error() const { return error_; }

private:
    std::vector<std::uint8_t> buf_;
    bool haveHeader_ = false;
    MsgType type_ = MsgType::Set;
    std::size_t len_ = 0;
    std::string error_;
};

// A frequency value: 4 bytes little-endian plus one more byte (the Afedri document's "byte 5"); CuteSDR
// writes the fifth byte as 0.
inline void putFrequency(std::vector<std::uint8_t>& v, std::uint64_t hz) { putLe(v, hz & 0xFFFFFFFFFFull, 5); }
inline std::uint64_t getFrequency(const std::uint8_t* p) { return getLe(p, 5); }

struct FreqRange {
    std::uint64_t minHz = 0, maxHz = 0;
};

// The body of a range response for the receive frequency: channel, number of ranges, then per range the
// minimum, the maximum and the VCO frequency, each 5 bytes. False when the body is too short for what it
// announces.
inline bool parseFrequencyRanges(const std::vector<std::uint8_t>& body, std::vector<FreqRange>& out) {
    out.clear();
    if (body.size() < 2) { return false; }
    const std::size_t count = body[1];
    if (count == 0 || count > 8) { return false; }
    if (body.size() < 2 + count * 15) { return false; }
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* p = body.data() + 2 + i * 15;
        FreqRange r;
        r.minHz = getFrequency(p);
        r.maxHz = getFrequency(p + 5);
        if (r.maxHz < r.minHz) { return false; }
        out.push_back(r);
    }
    return true;
}

// ---- the UDP I/Q stream ---------------------------------------------------------------

struct UdpPacket {
    std::uint16_t sequence = 0;
    int bits = 16;  // 16 or 24
    std::size_t pairs = 0;
};

// Decodes one datagram into complex samples scaled so that full scale is 1.0 (16-bit / 32768,
// 24-bit / 8388608). False, appending nothing, for any size other than the four listed, a header whose
// length field disagrees with the datagram or whose type is not a data item.
inline bool parseUdpPacket(const std::uint8_t* d, std::size_t n, UdpPacket& info, std::vector<std::complex<float>>& out) {
    int bits = 0;
    if (n == kUdpLarge16 || n == kUdpSmall16) { bits = 16; }
    else if (n == kUdpLarge24 || n == kUdpSmall24) { bits = 24; }
    else { return false; }
    const std::uint16_t h = static_cast<std::uint16_t>(d[0] | (d[1] << 8));
    if ((h >> 13) < 4 || (h & 0x1FFF) != n) { return false; }
    info.sequence = static_cast<std::uint16_t>(d[2] | (d[3] << 8));
    info.bits = bits;
    const std::size_t step = bits == 16 ? 4 : 6;
    info.pairs = (n - 4) / step;
    for (std::size_t i = 4; i + step <= n; i += step) {
        if (bits == 16) {
            const std::int16_t a = static_cast<std::int16_t>(d[i] | (d[i + 1] << 8));
            const std::int16_t b = static_cast<std::int16_t>(d[i + 2] | (d[i + 3] << 8));
            out.emplace_back(static_cast<float>(a) / 32768.0f, static_cast<float>(b) / 32768.0f);
        } else {
            auto s24 = [&](std::size_t o) {
                std::int32_t v = static_cast<std::int32_t>(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16));
                if (v & 0x800000) { v -= 0x1000000; }
                return static_cast<float>(v) / 8388608.0f;
            };
            out.emplace_back(s24(i), s24(i + 3));
        }
    }
    return true;
}

// Sequence accounting: the first packet of a stream is 0 (or, when the first one was lost, anything);
// the counter then steps by one and skips 0 on wrap. A step forward is "lost"; a step back or a repeat
// is "late". Never stops the stream.
class UdpSequence {
public:
    void reset() { have_ = false; lost_ = 0; late_ = 0; }
    // Returns the number of packets missing before this one (0 when in order).
    std::uint32_t note(std::uint16_t seq) {
        if (!have_ || seq == 0) {
            have_ = true;
            expected_ = next(seq);
            return 0;
        }
        if (seq == expected_) { expected_ = next(seq); return 0; }
        const std::uint16_t ahead = static_cast<std::uint16_t>(seq - expected_);
        if (ahead < 32768) {
            lost_ += ahead;
            expected_ = next(seq);
            return ahead;
        }
        ++late_;
        return 0;
    }
    std::uint64_t lost() const { return lost_; }
    std::uint64_t late() const { return late_; }

private:
    static std::uint16_t next(std::uint16_t s) { return static_cast<std::uint16_t>(s == 0xFFFF ? 1 : s + 1); }
    bool have_ = false;
    std::uint16_t expected_ = 0;
    std::uint64_t lost_ = 0, late_ = 0;
};

// The Afedri document's function for the sample rate the radio really delivers for a requested one, from
// its main clock (80 MHz in most SDR-Net units, 76.8 or 72 MHz in others): decimation = clock /
// (4 * rate), rounded as the document does, rate = clock / (4 * decimation). 0 for a zero argument.
inline std::uint64_t afedriRealRate(std::uint64_t mainClockHz, std::uint64_t requestedHz) {
    if (mainClockHz == 0 || requestedHz == 0) { return 0; }
    std::uint64_t temp = mainClockHz / requestedHz;
    temp = temp / 4;
    if (temp == 0) { temp = 1; }
    if (mainClockHz > requestedHz * (4 * temp + 2)) { temp = temp + 1; }
    return mainClockHz / (4 * temp);
}

}  // namespace cascade::source::ascp
