// hpsdr_protocol.cpp - see hpsdr_protocol.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/hpsdr_protocol.hpp"

#include <cstdio>

namespace cascade::source::hpsdr {

const char* boardName(std::uint8_t id) {
    switch (id) {
        case 0x00: return "Metis";
        case 0x01: return "Hermes";
        case 0x02: return "Griffin";
        case 0x04: return "Angelia";
        case 0x05: return "Orion";
        case 0x06: return "Hermes_Lite";
        default: break;
    }
    return "unknown board";
}

std::vector<std::uint8_t> buildDiscoveryRequest() {
    std::vector<std::uint8_t> v(kDiscoveryRequestBytes, 0);
    v[0] = 0xEF;
    v[1] = 0xFE;
    v[2] = 0x02;
    return v;
}

std::vector<std::uint8_t> buildStartStop(std::uint8_t command) {
    std::vector<std::uint8_t> v(kCommandBytes, 0);
    v[0] = 0xEF;
    v[1] = 0xFE;
    v[2] = 0x04;
    v[3] = command;
    return v;
}

std::string DiscoveryReply::macText() const {
    char b[24];
    std::snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return b;
}

bool parseDiscoveryReply(const std::uint8_t* d, std::size_t n, DiscoveryReply& out) {
    if (d == nullptr || n < 11) { return false; }
    if (d[0] != 0xEF || d[1] != 0xFE) { return false; }
    if (d[2] != 0x02 && d[2] != 0x03) { return false; }
    DiscoveryReply r;
    r.sending = d[2] == 0x03;
    for (int i = 0; i < 6; ++i) { r.mac[static_cast<std::size_t>(i)] = d[3 + i]; }
    r.codeVersion = d[9];
    r.boardId = d[10];
    out = r;
    return true;
}

bool parseDataPacket(const std::uint8_t* d, std::size_t n, DataPacket& out) {
    if (d == nullptr || n != kDataPacketBytes) { return false; }
    if (d[0] != 0xEF || d[1] != 0xFE || d[2] != 0x01) { return false; }
    const std::uint8_t ep = d[3];
    if (ep != kEndpointToDevice && ep != kEndpointBandscope && ep != kEndpointIq) { return false; }
    out.endpoint = ep;
    out.sequence = (static_cast<std::uint32_t>(d[4]) << 24) | (static_cast<std::uint32_t>(d[5]) << 16) |
                   (static_cast<std::uint32_t>(d[6]) << 8) | d[7];
    out.frames = d + 8;
    return true;
}

std::vector<std::uint8_t> buildDataPacket(std::uint8_t endpoint, std::uint32_t sequence, const std::uint8_t* frames1024) {
    std::vector<std::uint8_t> v(kDataPacketBytes, 0);
    v[0] = 0xEF;
    v[1] = 0xFE;
    v[2] = 0x01;
    v[3] = endpoint;
    v[4] = static_cast<std::uint8_t>(sequence >> 24);
    v[5] = static_cast<std::uint8_t>(sequence >> 16);
    v[6] = static_cast<std::uint8_t>(sequence >> 8);
    v[7] = static_cast<std::uint8_t>(sequence);
    for (std::size_t i = 0; i < 1024; ++i) { v[8 + i] = frames1024[i]; }
    return v;
}

std::uint32_t SequenceTracker::note(std::uint8_t endpoint, std::uint32_t sequence) {
    const auto it = expected_.find(endpoint);
    if (it == expected_.end()) {  // the first packet of an end point sets the baseline
        expected_[endpoint] = sequence + 1;
        return 0;
    }
    const std::uint32_t diff = sequence - it->second;  // modular
    if (diff == 0) {
        it->second = sequence + 1;
        return 0;
    }
    if (diff < 0x80000000u) {  // a gap forward
        lost_ += diff;
        it->second = sequence + 1;
        return diff;
    }
    ++late_;  // behind what was expected: late or repeated; the baseline does not move back
    return 0;
}

}  // namespace cascade::source::hpsdr
