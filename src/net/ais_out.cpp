// ais_out.cpp - see ais_out.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/ais_out.hpp"

#include <cstring>

namespace cascade::net {
namespace {

char armour(unsigned v) {
    unsigned c = v + 48;
    if (c > 87) { c += 8; }
    return static_cast<char>(c);
}

unsigned bitAt(const std::uint8_t* bits, std::size_t i) { return (bits[i >> 3] >> (7 - (i & 7))) & 1u; }

}  // namespace

std::uint8_t nmeaChecksum(const char* s, std::size_t n) noexcept {
    std::uint8_t x = 0;
    for (std::size_t i = 0; i < n; ++i) { x = static_cast<std::uint8_t>(x ^ static_cast<std::uint8_t>(s[i])); }
    return x;
}

int aisEncode(const std::uint8_t* bits, std::size_t nbits, const AisSentenceOptions& opt, AisSentences& out) noexcept {
    out.count = 0;
    if (bits == nullptr || nbits == 0 || nbits > kAisMaxBits) { return 0; }
    if (opt.talker == nullptr || (std::strcmp(opt.talker, "AIVDM") != 0 && std::strcmp(opt.talker, "AIVDO") != 0)) { return 0; }
    if (opt.channel != 'A' && opt.channel != 'B' && opt.channel != 0) { return 0; }
    if (opt.sequenceId < 0 || opt.sequenceId > 9) { return 0; }
    if (opt.maxPayloadChars < 1 || opt.maxPayloadChars > kAisMaxPayloadChars) { return 0; }
    const std::size_t chars = (nbits + 5) / 6;
    const int fill = static_cast<int>(chars * 6 - nbits);
    const std::size_t per = static_cast<std::size_t>(opt.maxPayloadChars);
    const std::size_t frags = (chars + per - 1) / per;
    if (frags > static_cast<std::size_t>(kAisMaxFragments)) { return 0; }

    std::size_t next = 0;  // next payload character
    for (std::size_t f = 0; f < frags; ++f) {
        char* p = out.text[f];
        std::size_t n = 0;
        auto put = [&](char c) { p[n++] = c; };
        put('!');
        for (const char* t = opt.talker; *t; ++t) { put(*t); }
        put(',');
        put(static_cast<char>('0' + frags));
        put(',');
        put(static_cast<char>('0' + f + 1));
        put(',');
        if (frags > 1) { put(static_cast<char>('0' + opt.sequenceId)); }
        put(',');
        if (opt.channel != 0) { put(opt.channel); }
        put(',');
        const std::size_t take = (chars - next < per) ? chars - next : per;
        for (std::size_t k = 0; k < take; ++k, ++next) {
            unsigned v = 0;
            for (int b = 0; b < 6; ++b) {
                const std::size_t bi = next * 6 + static_cast<std::size_t>(b);
                v = (v << 1) | (bi < nbits ? bitAt(bits, bi) : 0u);
            }
            put(armour(v));
        }
        put(',');
        put(static_cast<char>('0' + ((f + 1 == frags) ? fill : 0)));
        const std::uint8_t cs = nmeaChecksum(p + 1, n - 1);
        put('*');
        static const char kHex[] = "0123456789ABCDEF";
        put(kHex[cs >> 4]);
        put(kHex[cs & 15]);
        put('\r');
        put('\n');
        out.length[f] = n;
    }
    out.count = static_cast<int>(frags);
    return out.count;
}

bool AisOut::start(std::string& error) {
    if (useTcp_ && !tcp_.start(error)) { return false; }
    if (useUdp_ && !udp_.start(error)) {
        tcp_.stop();
        return false;
    }
    return true;
}

void AisOut::stop() {
    tcp_.stop();
    udp_.stop();
}

int AisOut::submit(const std::uint8_t* bits, std::size_t nbits, char channel) noexcept {
    AisSentenceOptions o;
    o.channel = channel;
    o.sequenceId = nextSeq_;
    AisSentences s;
    const int n = aisEncode(bits, nbits, o, s);
    if (n == 0) { return 0; }
    if (n > 1) { nextSeq_ = (nextSeq_ + 1) % 10; }
    for (int i = 0; i < n; ++i) {
        if (useTcp_) { tcp_.publish(s.text[i], s.length[i]); }
        if (useUdp_) { udp_.send(s.text[i], s.length[i]); }
    }
    return n;
}

}  // namespace cascade::net
