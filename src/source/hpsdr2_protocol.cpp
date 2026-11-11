// hpsdr2_protocol.cpp - see hpsdr2_protocol.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/hpsdr2_protocol.hpp"

#include <cmath>
#include <cstdio>

namespace cascade::source::hpsdr2 {

BoardInfo boardInfo(std::uint8_t type) {
    // Appendix A of the document has tables for boards 1 to 4. ADC counts: "one on Hermes (ANAN-10/10E/100)
    // and two on Angelia and Orion", DDC Specific packet, byte 4; DSP clock 122.88 MHz in each table.
    switch (type) {
        case 1: return {true, "Hermes (ANAN-10, 100)", 1, 122880000.0};
        case 2: return {true, "Hermes (ANAN-10E, 100B)", 1, 122880000.0};
        case 3: return {true, "Angelia (ANAN-100D)", 2, 122880000.0};
        case 4: return {true, "Orion (ANAN-200D)", 2, 122880000.0};
        case 0: return {false, "Atlas", 0, 0.0};
        case 5: return {false, "Orion Mk II (ANAN-7000DLE, 8000DLE)", 0, 0.0};
        case 6: return {false, "Hermes Lite", 0, 0.0};
        case 10: return {false, "Saturn (ANAN-G2)", 0, 0.0};
        default: break;
    }
    return {};
}

namespace {

inline void put32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}
inline std::uint32_t get32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
inline void put16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}

}  // namespace

std::vector<std::uint8_t> buildDiscovery() {
    std::vector<std::uint8_t> v(kDiscoveryBytes, 0);
    v[4] = 0x02;
    return v;
}

std::string DiscoveryReply::macText() const {
    char b[24];
    std::snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return b;
}

bool parseDiscoveryReply(const std::uint8_t* d, std::size_t n, DiscoveryReply& out) {
    if (d == nullptr || n < 24) { return false; }
    if (get32(d) != 0) { return false; }
    if (d[4] != 0x02 && d[4] != 0x03) { return false; }
    DiscoveryReply r;
    r.busy = d[4] == 0x03;
    for (std::size_t i = 0; i < 6; ++i) { r.mac[i] = d[5 + i]; }
    r.boardType = d[11];
    r.protocolVersion = d[12];
    r.firmwareVersion = d[13];
    r.numDdcs = d[20];
    r.phaseWord = d[21] != 0;
    r.formatModes = d[22];
    r.beta = d[23] != 0;
    out = r;
    return true;
}

std::vector<std::uint8_t> buildGeneral(std::uint32_t seq, const PortPlan& ports, bool phaseWord) {
    std::vector<std::uint8_t> v(kGeneralBytes, 0);
    put32(v.data(), seq);
    v[4] = 0x00;
    put16(&v[5], ports.ddcSpecific);
    // bytes 7-8 DUC specific port, 13-14 DDC audio, 15-16 DUC I&Q, 19-20 mic, 21-22 wideband: zero = default
    put16(&v[9], ports.highPriorityToHardware);
    put16(&v[11], ports.highPriorityFromHardware);
    put16(&v[17], ports.ddc0);
    v[37] = static_cast<std::uint8_t>(phaseWord ? 0x08 : 0x00);
    v[38] = 0x01;  // hardware reset timer enabled
    v[39] = 0x00;  // big-endian, 3-byte I&Q
    return v;
}

const std::array<int, 6>& supportedRates() {
    static const std::array<int, 6> r = {48000, 96000, 192000, 384000, 768000, 1536000};
    return r;
}

bool rateToKsps(int rateHz, std::uint16_t& ksps) {
    for (const int r : supportedRates()) {
        if (r == rateHz) { ksps = static_cast<std::uint16_t>(r / 1000); return true; }
    }
    return false;
}

std::vector<std::uint8_t> buildDdcSpecific(std::uint32_t seq, int adcs, int rateHz) {
    std::uint16_t ksps = 0;
    if (adcs < 1 || adcs > 8 || !rateToKsps(rateHz, ksps)) { return {}; }
    std::vector<std::uint8_t> v(kDdcSpecificBytes, 0);
    put32(v.data(), seq);
    v[4] = static_cast<std::uint8_t>(adcs);
    v[7] = 0x01;      // DDC0 enabled
    v[17] = 0x00;     // DDC0 on ADC0
    put16(&v[18], ksps);
    v[22] = 24;       // bits per sample
    return v;
}

std::uint32_t frequencyWord(double hz, bool phaseWord, double dspClockHz, bool& ok) {
    ok = false;
    if (!std::isfinite(hz) || hz < 0.0) { return 0; }
    if (phaseWord) {
        if (!(dspClockHz > 0.0) || hz >= dspClockHz) { return 0; }
        const double w = std::round(4294967296.0 * hz / dspClockHz);
        if (w < 0.0 || w > 4294967295.0) { return 0; }
        ok = true;
        return static_cast<std::uint32_t>(w);
    }
    if (hz > 4294967295.0) { return 0; }
    ok = true;
    return static_cast<std::uint32_t>(std::llround(hz));
}

std::vector<std::uint8_t> buildHighPriority(std::uint32_t seq, bool run, std::uint32_t ddc0Word, int attenuatorDb) {
    std::vector<std::uint8_t> v(kHighPriorityBytes, 0);
    put32(v.data(), seq);
    v[4] = static_cast<std::uint8_t>(run ? 0x01 : 0x00);  // bit 1 (PTT) is never set
    put32(&v[9], ddc0Word);
    v[1443] = static_cast<std::uint8_t>(attenuatorDb < 0 ? 0 : (attenuatorDb > 31 ? 31 : attenuatorDb));
    return v;
}

const char* verdictName(DdcVerdict v) {
    switch (v) {
        case DdcVerdict::Ok: return "ok";
        case DdcVerdict::WrongLength: return "a DDC packet whose length does not match its sample count";
        case DdcVerdict::BadSampleSize: return "a DDC packet that is not 24 bits per sample";
        case DdcVerdict::BadCount: return "a DDC packet with no samples or too many";
    }
    return "?";
}

DdcVerdict parseDdcPacket(const std::uint8_t* d, std::size_t n, DdcPacket& out) {
    if (d == nullptr || n < kDdcHeaderBytes) { return DdcVerdict::WrongLength; }
    const unsigned bits = (static_cast<unsigned>(d[12]) << 8) | d[13];
    const unsigned count = (static_cast<unsigned>(d[14]) << 8) | d[15];
    if (bits != 24) { return DdcVerdict::BadSampleSize; }
    if (count == 0 || count > static_cast<unsigned>(kMaxDdcSamples)) { return DdcVerdict::BadCount; }
    if (n != kDdcHeaderBytes + 6u * count) { return DdcVerdict::WrongLength; }
    out.sequence = get32(d);
    out.samples = static_cast<std::uint16_t>(count);
    out.data = d + kDdcHeaderBytes;
    return DdcVerdict::Ok;
}

namespace {
inline float s24(const std::uint8_t* p) {
    std::int32_t v = (static_cast<std::int32_t>(p[0]) << 16) | (static_cast<std::int32_t>(p[1]) << 8) | p[2];
    if ((v & 0x800000) != 0) { v -= 0x1000000; }
    return static_cast<float>(v) * (1.0f / 8388608.0f);
}
}  // namespace

void decodeDdcSamples(const DdcPacket& p, std::vector<std::complex<float>>& out) {
    const std::uint8_t* s = p.data;
    for (unsigned i = 0; i < p.samples; ++i, s += 6) { out.emplace_back(s24(s), s24(s + 3)); }
}

std::vector<std::uint8_t> buildDdcPacket(std::uint32_t seq, const std::int32_t* iq, int count) {
    if (count < 0) { count = 0; }
    if (count > kMaxDdcSamples) { count = kMaxDdcSamples; }
    std::vector<std::uint8_t> v(kDdcHeaderBytes + 6u * static_cast<unsigned>(count), 0);
    put32(v.data(), seq);
    put16(&v[12], 24);
    put16(&v[14], static_cast<std::uint16_t>(count));
    for (int i = 0; i < count; ++i) {
        for (int k = 0; k < 2; ++k) {
            const std::int32_t x = iq[2 * i + k];
            std::uint8_t* p = &v[kDdcHeaderBytes + 6u * static_cast<unsigned>(i) + 3u * static_cast<unsigned>(k)];
            p[0] = static_cast<std::uint8_t>((x >> 16) & 0xFF);
            p[1] = static_cast<std::uint8_t>((x >> 8) & 0xFF);
            p[2] = static_cast<std::uint8_t>(x & 0xFF);
        }
    }
    return v;
}

bool parseStatus(const std::uint8_t* d, std::size_t n, Status& out) {
    if (d == nullptr || n != kStatusBytes) { return false; }
    Status s;
    s.sequence = get32(d);
    s.pllLocked = (d[4] & 0x10) != 0;
    s.adcOverload = d[5];
    s.ddcFifoOverflow = (d[30] & 0x01) != 0;
    out = s;
    return true;
}

}  // namespace cascade::source::hpsdr2
