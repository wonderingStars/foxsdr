// spyserver_protocol.hpp - the wire format of Airspy's SpyServer (a network radio server for Airspy One/Mini/R2,
// Airspy HF+ and RTL-SDR dongles): command encoders, the 20-byte message header, the two description messages and
// the I/Q sample bodies. No sockets here.
//
// PROVENANCE AND LICENCES (docs/feature-records/oss-net.md has the full list and the commit hashes read).
//   * spy2go, https://github.com/racerxdl/spy2go , Apache-2.0 (LICENSE file opened and read, the standard Apache 2.0
//     text), commit 14102c047be58ea987be26a4d74639f7a506b2ff : spyserver/protocol.go (the constants, command and
//     message numbers, the three struct layouts) and spyserver/SpyServer.go (the order of a session: HELLO, wait for the
//     two description messages, then the settings; the bound of 1 MiB on a message body; the sample rate of decimation
//     stage i being the maximum rate divided by 2^i; the sequence-number gap rule). Port, not paste: the code below is
//     this project's own.
//   * spyclient, https://github.com/Tubbz-alt/spyclient (a mirror of sam210723/spyclient), MIT (LICENSE file opened and
//     read, "Copyright (c) 2020 Sam"), commit ace7f8ac0b4d9287a454835c9cb655115c2a2f89 : spyclient/spyclient.py and
//     enums.py (the same layouts, the protocol version 0x020006A4, and the fact that a server that cannot give the
//     client a device answers with a closed connection).
//   Not opened: SoapySpyServer, spyserver_client, SDR++ (GPL), and anything of Airspy's. No Airspy document was found.
//   AIRSPY'S TERMS FOR THIRD-PARTY CLIENTS ARE UNKNOWN. The two open clients above exist and are public, but whether
//   Airspy allows other programs to speak the protocol is not stated on any page the lane could open; that is the
//   owner's decision before this ships (docs/feature-records/oss-net.md, "Decisions for the owner").
//
// WHAT NEITHER SOURCE SAYS, so is a HYPOTHESIS (UNVERIFIED against a real server; the stand-in in
// tests/test_spyserver_source.cpp is written from the same two sources and cannot show them right):
//   (1) the byte order of the 8-bit I/Q (offset binary around 127.5 is assumed; spy2go leaves them raw);
//   (2) that the sample rate of stage i is MaximumSampleRate / 2^i (spy2go's table; the MIT client has none);
//   (3) the unit of the GAIN setting (a gain-stage index 0..MaximumGainIndex, as spy2go checks it);
//   (4) that MinimumIQDecimation is the lowest decimation stage the server accepts for I/Q;
//   (5) 24-bit samples (message type 102) and the audio and FFT streams are not read: they are counted and skipped.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace cascade::source::spyserver {

// ((major) << 24) | ((minor) << 16) | build. The MIT client sends 2.0.1700; spy2go sends 2.0.1558. A server compares
// major and minor only (spy2go checks the same two bytes of what the SERVER sends back).
constexpr std::uint32_t kClientProtocol = (2u << 24) | (0u << 16) | 1700u;
constexpr std::uint16_t kDefaultPort = 5555;
constexpr std::uint32_t kMaxBodyBytes = 1u << 20;      // spy2go: "Server sent more than expected body size"
constexpr std::size_t kMessageHeaderBytes = 20;
constexpr std::size_t kDeviceInfoBytes = 12 * 4;
constexpr std::size_t kClientSyncBytes = 9 * 4;
constexpr const char* kClientName = "FoxSDR";

enum Command : std::uint32_t { kCmdHello = 0, kCmdGet = 1, kCmdSet = 2, kCmdPing = 3 };

enum Setting : std::uint32_t {
    kSetStreamingMode = 0,
    kSetStreamingEnabled = 1,
    kSetGain = 2,
    kSetIqFormat = 100,
    kSetIqFrequency = 101,
    kSetIqDecimation = 102,
};

enum StreamMode : std::uint32_t { kModeIqOnly = 1 };      // the only mode this client asks for

enum IqFormat : std::uint32_t { kFmtUint8 = 1, kFmtInt16 = 2, kFmtFloat = 4 };

enum MessageType : std::uint32_t {
    kMsgDeviceInfo = 0,
    kMsgClientSync = 1,
    kMsgPong = 2,
    kMsgReadSetting = 3,
    kMsgUint8Iq = 100,
    kMsgInt16Iq = 101,
    kMsgInt24Iq = 102,
    kMsgFloatIq = 103,
};

enum StreamType : std::uint32_t { kStreamStatus = 0, kStreamIq = 1 };

enum DeviceType : std::uint32_t { kDeviceInvalid = 0, kDeviceAirspyOne = 1, kDeviceAirspyHf = 2, kDeviceRtlSdr = 3 };

inline const char* deviceName(std::uint32_t t) {
    switch (t) {
        case kDeviceAirspyOne: return "Airspy One/Mini/R2";
        case kDeviceAirspyHf: return "Airspy HF+";
        case kDeviceRtlSdr: return "RTL-SDR";
        default: return "unknown device";
    }
}

inline void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int s = 0; s < 32; s += 8) { b.push_back(static_cast<std::uint8_t>(v >> s)); }  // little-endian
}
inline std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

// A command: u32 type, u32 body length, then the body.
inline std::vector<std::uint8_t> encodeCommand(std::uint32_t type, const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> out;
    out.reserve(8 + body.size());
    put32(out, type);
    put32(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// HELLO: u32 protocol version, then the client's name (not terminated, not length-prefixed: the command's body length
// bounds it). A name is cut at 64 bytes.
inline std::vector<std::uint8_t> encodeHello(const std::string& clientName = kClientName) {
    std::vector<std::uint8_t> body;
    put32(body, kClientProtocol);
    const std::string n = clientName.substr(0, 64);
    body.insert(body.end(), n.begin(), n.end());
    return encodeCommand(kCmdHello, body);
}

// SET: u32 setting, u32 value. (The MIT client names the setting numbers and has no encoder; spy2go writes the setting
// number and then each parameter as a u32.)
inline std::vector<std::uint8_t> encodeSet(std::uint32_t setting, std::uint32_t value) {
    std::vector<std::uint8_t> body;
    put32(body, setting);
    put32(body, value);
    return encodeCommand(kCmdSet, body);
}

struct MessageHeader {
    std::uint32_t protocol = 0, type = 0, stream = 0, sequence = 0, bodySize = 0;
    int major() const { return static_cast<int>((protocol >> 24) & 0xFF); }
    int minor() const { return static_cast<int>((protocol >> 16) & 0xFF); }
    int build() const { return static_cast<int>(protocol & 0xFFFF); }
};

enum class HeaderVerdict { Ok, Version, TooBig };

inline MessageHeader decodeHeader(const std::uint8_t* p) {
    MessageHeader h;
    h.protocol = get32(p);
    h.type = get32(p + 4);
    h.stream = get32(p + 8);
    h.sequence = get32(p + 12);
    h.bodySize = get32(p + 16);
    return h;
}

// A server of another major or minor version is refused (spy2go's rule); a body over 1 MiB is refused whatever it says.
inline HeaderVerdict checkHeader(const MessageHeader& h) {
    if (h.major() != static_cast<int>((kClientProtocol >> 24) & 0xFF) || h.minor() != static_cast<int>((kClientProtocol >> 16) & 0xFF)) {
        return HeaderVerdict::Version;
    }
    if (h.bodySize > kMaxBodyBytes) { return HeaderVerdict::TooBig; }
    return HeaderVerdict::Ok;
}

struct DeviceInfo {
    std::uint32_t deviceType = 0, serial = 0, maxSampleRate = 0, maxBandwidth = 0, decimationStages = 0, gainStages = 0,
                  maxGainIndex = 0, minFrequency = 0, maxFrequency = 0, adcResolution = 0, minIqDecimation = 0, forcedIqFormat = 0;
};

// False when the body is shorter than 48 bytes; a longer one (a newer server) is accepted, the extra bytes ignored.
inline bool parseDeviceInfo(const std::uint8_t* p, std::size_t n, DeviceInfo& d) {
    if (n < kDeviceInfoBytes) { return false; }
    std::uint32_t* f[] = {&d.deviceType, &d.serial, &d.maxSampleRate, &d.maxBandwidth, &d.decimationStages, &d.gainStages,
                          &d.maxGainIndex, &d.minFrequency, &d.maxFrequency, &d.adcResolution, &d.minIqDecimation, &d.forcedIqFormat};
    for (int i = 0; i < 12; ++i) { *f[i] = get32(p + 4 * i); }
    return true;
}

struct ClientSync {
    std::uint32_t canControl = 0, gain = 0, deviceCenter = 0, iqCenter = 0, fftCenter = 0, minIqCenter = 0, maxIqCenter = 0,
                  minFftCenter = 0, maxFftCenter = 0;
};

inline bool parseClientSync(const std::uint8_t* p, std::size_t n, ClientSync& s) {
    if (n < kClientSyncBytes) { return false; }
    std::uint32_t* f[] = {&s.canControl, &s.gain, &s.deviceCenter, &s.iqCenter, &s.fftCenter, &s.minIqCenter, &s.maxIqCenter,
                          &s.minFftCenter, &s.maxFftCenter};
    for (int i = 0; i < 9; ++i) { *f[i] = get32(p + 4 * i); }
    return true;
}

// The I/Q sample rates a device offers, ascending: maximum / 2^stage for the stages the server accepts (hypotheses 2 and 4
// in the header). A device that reports no stages or a zero maximum offers none.
inline std::vector<double> sampleRatesFor(const DeviceInfo& d) {
    std::vector<double> r;
    if (d.maxSampleRate == 0 || d.decimationStages == 0) { return r; }
    const std::uint32_t first = d.minIqDecimation < d.decimationStages ? d.minIqDecimation : 0;
    for (std::uint32_t s = first; s < d.decimationStages && s < 24; ++s) { r.push_back(static_cast<double>(d.maxSampleRate) / static_cast<double>(1u << s)); }
    std::sort(r.begin(), r.end());
    return r;
}

// The stage whose rate is nearest `hz`; `rateOut` is that rate. False when the device offers none.
inline bool stageForRate(const DeviceInfo& d, double hz, std::uint32_t& stage, double& rateOut) {
    const std::uint32_t first = d.minIqDecimation < d.decimationStages ? d.minIqDecimation : 0;
    bool any = false;
    double best = 0.0;
    for (std::uint32_t s = first; s < d.decimationStages && s < 24; ++s) {
        const double r = static_cast<double>(d.maxSampleRate) / static_cast<double>(1u << s);
        if (!any || std::fabs(r - hz) < std::fabs(best - hz)) { any = true; best = r; stage = s; }
    }
    rateOut = best;
    return any;
}

// I/Q sample bodies. The count of complete pairs is returned; a body that is not a whole number of pairs is refused
// (-1) rather than guessed at.
inline long convertIq(std::uint32_t type, const std::uint8_t* p, std::size_t n, std::vector<std::complex<float>>& out) {
    std::size_t pairBytes = 0;
    switch (type) {
        case kMsgUint8Iq: pairBytes = 2; break;
        case kMsgInt16Iq: pairBytes = 4; break;
        case kMsgFloatIq: pairBytes = 8; break;
        default: return -1;
    }
    if (n % pairBytes != 0) { return -1; }
    const std::size_t pairs = n / pairBytes;
    out.resize(pairs);
    for (std::size_t i = 0; i < pairs; ++i) {
        if (type == kMsgUint8Iq) {
            out[i] = {(static_cast<float>(p[2 * i]) - 127.5f) / 127.5f, (static_cast<float>(p[2 * i + 1]) - 127.5f) / 127.5f};
        } else if (type == kMsgInt16Iq) {
            const std::int16_t a = static_cast<std::int16_t>(p[4 * i] | (p[4 * i + 1] << 8));
            const std::int16_t b = static_cast<std::int16_t>(p[4 * i + 2] | (p[4 * i + 3] << 8));
            out[i] = {static_cast<float>(a) / 32768.0f, static_cast<float>(b) / 32768.0f};
        } else {
            float a = 0.0f, b = 0.0f;
            std::memcpy(&a, p + 8 * i, 4);
            std::memcpy(&b, p + 8 * i + 4, 4);
            if (!std::isfinite(a)) { a = 0.0f; }
            if (!std::isfinite(b)) { b = 0.0f; }
            out[i] = {a, b};
        }
    }
    return static_cast<long>(pairs);
}

// Accepts bytes in any split and yields whole messages; never trusts a length before checking it.
class MessageAssembler {
public:
    enum class Status { NeedMore, Message, Error };

    // Feed bytes; call next() until it says NeedMore.
    void feed(const std::uint8_t* p, std::size_t n) { buf_.insert(buf_.end(), p, p + n); }

    Status next(MessageHeader& h, std::vector<std::uint8_t>& body, std::string& error) {
        if (!error_.empty()) { error = error_; return Status::Error; }
        if (buf_.size() < kMessageHeaderBytes) { return Status::NeedMore; }
        h = decodeHeader(buf_.data());
        switch (checkHeader(h)) {
            case HeaderVerdict::Version:
                error_ = "the server speaks protocol " + std::to_string(h.major()) + "." + std::to_string(h.minor()) + "." + std::to_string(h.build()) +
                         ", this client speaks 2.0 only";
                error = error_;
                return Status::Error;
            case HeaderVerdict::TooBig:
                error_ = "the server announced a message of " + std::to_string(h.bodySize) + " bytes, over the 1 MiB limit";
                error = error_;
                return Status::Error;
            case HeaderVerdict::Ok: break;
        }
        if (buf_.size() < kMessageHeaderBytes + h.bodySize) { return Status::NeedMore; }
        body.assign(buf_.begin() + kMessageHeaderBytes, buf_.begin() + static_cast<std::ptrdiff_t>(kMessageHeaderBytes + h.bodySize));
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(kMessageHeaderBytes + h.bodySize));
        return Status::Message;
    }

    std::size_t buffered() const { return buf_.size(); }

private:
    std::vector<std::uint8_t> buf_;
    std::string error_;
};

}  // namespace cascade::source::spyserver
