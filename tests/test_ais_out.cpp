// Tests for net/ais_out.hpp and net/udp_sink.hpp: AIS !AIVDM sentences against the worked examples of the AIVDM/AIVDO format page
// (https://gpsd.gitlab.io/gpsd/AIVDM.html, read 2026-11-11), a round trip through a decoder written here independently of the encoder,
// and both transports over real loopback sockets.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/ais_out.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "net_test_support.hpp"
#include "test_check.hpp"

using namespace cascade::net;
using nettest::Client;

namespace {

using Bits = std::vector<std::uint8_t>;  // one bit per element in the independent decoder (0 or 1)

// ---- an independent decoder: shares no code with the encoder -----------------------------------------------------------------------
struct Decoded {
    bool ok = false;
    std::string talker;
    int count = 0, number = 0;
    std::string seq, channel, payload;
    int fill = -1;
    Bits bits;  // payload bits with the fill removed
    int type = -1;
    bool padZero = true;  // the fill bits that were removed are all zero
};

Decoded decode(const std::string& line) {
    Decoded d;
    if (line.size() < 12 || line[0] != '!' || line.compare(line.size() - 2, 2, "\r\n") != 0) { return d; }
    const std::string s = line.substr(0, line.size() - 2);
    const std::size_t star = s.rfind('*');
    if (star == std::string::npos || star + 3 != s.size()) { return d; }
    unsigned x = 0;
    for (std::size_t i = 1; i < star; ++i) { x ^= static_cast<unsigned char>(s[i]); }
    unsigned want = 0;
    if (std::sscanf(s.c_str() + star + 1, "%2x", &want) != 1 || want != x) { return d; }
    if (s.substr(star + 1) != [&] { char b[8]; std::snprintf(b, sizeof(b), "%02X", x); return std::string(b); }()) { return d; }  // upper case
    std::vector<std::string> f;
    std::string cur;
    for (std::size_t i = 0; i < star; ++i) {
        if (s[i] == ',') { f.push_back(cur); cur.clear(); } else { cur.push_back(s[i]); }
    }
    f.push_back(cur);
    if (f.size() != 7) { return d; }
    d.talker = f[0];
    d.count = std::atoi(f[1].c_str());
    d.number = std::atoi(f[2].c_str());
    d.seq = f[3];
    d.channel = f[4];
    d.payload = f[5];
    d.fill = std::atoi(f[6].c_str());
    for (char c : d.payload) {
        int v = c - 48;
        if (v > 40) { v -= 8; }
        if (v < 0 || v > 63) { return d; }
        for (int b = 5; b >= 0; --b) { d.bits.push_back(static_cast<std::uint8_t>((v >> b) & 1)); }
    }
    if (d.fill < 0 || d.fill > 5 || static_cast<std::size_t>(d.fill) > d.bits.size()) { return d; }
    for (std::size_t k = d.bits.size() - static_cast<std::size_t>(d.fill); k < d.bits.size(); ++k) { if (d.bits[k]) { d.padZero = false; } }
    d.bits.resize(d.bits.size() - static_cast<std::size_t>(d.fill));
    if (d.bits.size() >= 6) {
        d.type = 0;
        for (int b = 0; b < 6; ++b) { d.type = d.type * 2 + d.bits[static_cast<std::size_t>(b)]; }
    }
    d.ok = true;
    return d;
}

std::vector<std::uint8_t> pack(const Bits& b) {
    std::vector<std::uint8_t> out((b.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < b.size(); ++i) {
        if (b[i]) { out[i >> 3] = static_cast<std::uint8_t>(out[i >> 3] | (0x80 >> (i & 7))); }
    }
    return out;
}

std::vector<std::string> sentences(const std::vector<std::uint8_t>& packed, std::size_t nbits, char channel, int seq, int maxChars = kAisDefaultPayloadChars) {
    AisSentenceOptions o;
    o.channel = channel;
    o.sequenceId = seq;
    o.maxPayloadChars = maxChars;
    AisSentences s;
    const int n = aisEncode(packed.data(), nbits, o, s);
    std::vector<std::string> v;
    for (int i = 0; i < n; ++i) { v.emplace_back(s.text[i], s.length[i]); }
    return v;
}

// ---- the published examples ----------------------------------------------------------------------------------------------------
const char* kOne = "!AIVDM,1,1,,B,177KQJ5000G?tO`K>RA1wUbN0TKH,0*5C";
const char* kTwoA = "!AIVDM,2,1,3,B,55P5TL01VIaAL@7WKO@mBplU@<PDhh000000001S;AJ::4A80?4i@E53,0*3E";
const char* kTwoB = "!AIVDM,2,2,3,B,1@0000000000000,2*55";

void testPublishedExamples() {
    // 1. single sentence: decode with the independent decoder, re-encode, the sentence is reproduced exactly (checksum 5C included)
    const Decoded one = decode(std::string(kOne) + "\r\n");
    CHECK(one.ok);
    CHECK(one.count == 1 && one.number == 1 && one.seq.empty() && one.channel == "B" && one.fill == 0);
    CHECK(one.bits.size() == 168);  // 28 characters x 6 bits: a position report is 168 bits
    CHECK(one.type == 1);
    {
        const auto v = sentences(pack(one.bits), one.bits.size(), 'B', 0);
        CHECK(v.size() == 1);
        CHECK(v.size() == 1 && v[0] == std::string(kOne) + "\r\n");
    }
    // 2. two sentences, sequential id 3, fill bits 2 on the second (checksums 3E and 55)
    const Decoded a = decode(std::string(kTwoA) + "\r\n");
    const Decoded b = decode(std::string(kTwoB) + "\r\n");
    CHECK(a.ok && b.ok);
    CHECK(a.count == 2 && a.number == 1 && a.seq == "3" && a.fill == 0 && b.number == 2 && b.fill == 2);
    CHECK(a.type == 5);
    Bits whole = a.bits;
    whole.insert(whole.end(), b.bits.begin(), b.bits.end());
    CHECK(whole.size() == 424);  // 71 characters x 6 - 2 fill bits: a static and voyage report is 424 bits
    {
        const auto v = sentences(pack(whole), whole.size(), 'B', 3);
        CHECK(v.size() == 2);
        if (v.size() == 2) {
            CHECK(v[0] == std::string(kTwoA) + "\r\n");
            CHECK(v[1] == std::string(kTwoB) + "\r\n");
        }
    }
    // the page's armouring samples: '0' = 0, 'W' = 39, '`' = 40, 'w' = 63
    const Decoded arm = decode("!AIVDM,1,1,,A,0W`w,0*" + [] {
        unsigned x = 0;
        for (char c : std::string("AIVDM,1,1,,A,0W`w,0")) { x ^= static_cast<unsigned char>(c); }
        char b2[8];
        std::snprintf(b2, sizeof(b2), "%02X", x);
        return std::string(b2);
    }() + "\r\n");
    CHECK(arm.ok);
    CHECK(arm.bits.size() == 24);
    auto val = [&](int k) { int v = 0; for (int i = 0; i < 6; ++i) { v = v * 2 + arm.bits[static_cast<std::size_t>(k * 6 + i)]; } return v; };
    CHECK(val(0) == 0 && val(1) == 39 && val(2) == 40 && val(3) == 63);
    // and the encoder agrees on those four characters
    {
        const auto v = sentences(pack(arm.bits), arm.bits.size(), 'A', 0);
        CHECK(v.size() == 1 && v[0].find(",0W`w,0*") != std::string::npos);
    }
}

void testFillBitsAndBoundaries() {
    // payload bit counts 1..13: fill = (6 - n % 6) % 6, characters = ceil(n / 6), the padding bits are zero
    for (std::size_t n = 1; n <= 13; ++n) {
        Bits ones(n, 1);
        const auto v = sentences(pack(ones), n, 'A', 0);
        CHECK(v.size() == 1);
        const Decoded d = decode(v[0]);
        CHECK(d.ok);
        CHECK(d.payload.size() == (n + 5) / 6);
        CHECK(d.fill == static_cast<int>((6 - n % 6) % 6));
        CHECK(d.padZero);
        CHECK(d.bits == ones);
    }
    // the split point: 56 characters (336 bits) is one sentence, 57 characters (342 bits) two
    Bits b336(336, 1), b337(337, 1), b342(342, 1);
    CHECK(sentences(pack(b336), 336, 'A', 0).size() == 1);
    CHECK(sentences(pack(b337), 337, 'A', 0).size() == 2);
    CHECK(sentences(pack(b342), 342, 'A', 0).size() == 2);
    const auto two = sentences(pack(b337), 337, 'A', 4);
    const Decoded t1 = decode(two[0]), t2 = decode(two[1]);
    CHECK(t1.payload.size() == 56 && t2.payload.size() == 1 && t1.fill == 0 && t2.fill == 5);
    CHECK(t1.seq == "4" && t2.seq == "4" && t1.count == 2 && t2.number == 2);
    // a one-sentence message has an EMPTY sequence field even when a sequence id was given
    CHECK(sentences(pack(b336), 336, 'A', 7)[0].find("!AIVDM,1,1,,A,") == 0);
    // the whole size range: every length 1..1008 round-trips through the decoder, every sentence within the 82-character limit
    std::mt19937 rng(99);
    std::size_t maxLine = 0;
    for (std::size_t n = 1; n <= kAisMaxBits; ++n) {
        Bits bits(n);
        for (auto& x : bits) { x = static_cast<std::uint8_t>(rng() & 1); }
        const auto v = sentences(pack(bits), n, (n & 1) ? 'A' : 'B', static_cast<int>(n % 10));
        CHECK(!v.empty());
        Bits back;
        int expectNum = 1;
        for (const std::string& s : v) {
            maxLine = std::max(maxLine, s.size());
            const Decoded d = decode(s);
            CHECK(d.ok && d.padZero);
            CHECK(d.count == static_cast<int>(v.size()) && d.number == expectNum++);
            CHECK(d.channel == ((n & 1) ? "A" : "B"));
            back.insert(back.end(), d.bits.begin(), d.bits.end());
            if (d.number != d.count) { CHECK(d.fill == 0); }
        }
        CHECK(back == bits);
    }
    CHECK(maxLine <= 82);  // incl. CRLF
    std::printf("longest sentence at the default split: %zu bytes with CRLF\n", maxLine);
    // the 60-character split, the longest the encoder allows, still fits 82
    Bits big(kAisMaxBits, 1);
    std::size_t maxLine60 = 0;
    for (const std::string& s : sentences(pack(big), big.size(), 'A', 9, 60)) { maxLine60 = std::max(maxLine60, s.size()); }
    CHECK(maxLine60 <= 82);
}

void testRefusals() {
    AisSentences s;
    const std::uint8_t one[1] = {0x80};
    AisSentenceOptions o;
    CHECK(aisEncode(nullptr, 6, o, s) == 0);
    CHECK(aisEncode(one, 0, o, s) == 0);
    std::vector<std::uint8_t> big(200, 0);
    CHECK(aisEncode(big.data(), kAisMaxBits + 1, o, s) == 0);
    CHECK(aisEncode(big.data(), kAisMaxBits, o, s) > 0);
    o.channel = 'C';
    CHECK(aisEncode(one, 6, o, s) == 0);
    o.channel = '1';  // codes seen in the wild are NOT produced
    CHECK(aisEncode(one, 6, o, s) == 0);
    o.channel = 0;  // unknown channel: an empty field
    CHECK(aisEncode(one, 6, o, s) == 1);
    CHECK(std::string(s.text[0], s.length[0]).find("!AIVDM,1,1,,,") == 0);
    o = AisSentenceOptions();
    o.sequenceId = 10;
    CHECK(aisEncode(one, 6, o, s) == 0);
    o.sequenceId = -1;
    CHECK(aisEncode(one, 6, o, s) == 0);
    o = AisSentenceOptions();
    o.maxPayloadChars = 0;
    CHECK(aisEncode(one, 6, o, s) == 0);
    o.maxPayloadChars = 61;
    CHECK(aisEncode(one, 6, o, s) == 0);
    o.maxPayloadChars = 20;  // 168 payload characters need 9 sentences at 20: exactly the limit of nine
    CHECK(aisEncode(big.data(), 1008, o, s) == 9);
    o.maxPayloadChars = 18;  // 10 fragments: the format has one digit for the count
    CHECK(aisEncode(big.data(), 1008, o, s) == 0);
    o = AisSentenceOptions();
    o.talker = "GPGGA";
    CHECK(aisEncode(one, 6, o, s) == 0);
    o.talker = nullptr;
    CHECK(aisEncode(one, 6, o, s) == 0);
    o.talker = "AIVDO";
    CHECK(aisEncode(one, 6, o, s) == 1 && std::string(s.text[0], 6) == "!AIVDO");
    // a refused call leaves count 0 (a caller that ignores the return value sends nothing)
    o.channel = 'Z';
    s.count = 5;
    CHECK(aisEncode(one, 6, o, s) == 0 && s.count == 0);
    // checksum helper on its own, from the page's single-sentence example
    const std::string body = "AIVDM,1,1,,B,177KQJ5000G?tO`K>RA1wUbN0TKH,0";
    CHECK(nmeaChecksum(body.data(), body.size()) == 0x5C);
    CHECK(nmeaChecksum("", 0) == 0);
}

// ---- transports ------------------------------------------------------------------------------------------------------------------
struct UdpListener {
    sock::Handle s = sock::kInvalid;
    std::uint16_t port = 0;
    bool open() {
        std::string err;
        std::uint32_t ip = 0;
        sock::parseIpv4("127.0.0.1", ip);
        s = sock::bindUdp(ip, 0, err);
        if (s == sock::kInvalid) { return false; }
        port = sock::boundPort(s);
        return true;
    }
    // Collects datagrams until `n` have arrived or `ms` pass.
    std::vector<std::string> collect(std::size_t n, int ms) {
        std::vector<std::string> got;
        const auto t0 = nettest::Clock::now();
        char buf[70000];
        while (got.size() < n && nettest::msSince(t0) < ms) {
            sock::PollItem it;
            it.s = s;
            it.want = sock::kReadable;
            if (sock::pollMany(&it, 1, 20) <= 0) { continue; }
            const long r = sock::recvSome(s, buf, sizeof(buf));
            if (r > 0) { got.emplace_back(buf, static_cast<std::size_t>(r)); }
        }
        return got;
    }
    ~UdpListener() { sock::closeHandle(s); }
};

void testUdpSinkPolicy() {
    std::string err;
    {
        UdpConfig c;
        c.port = 0;
        CHECK(!UdpSink(c).start(err) && err.find("port") != std::string::npos);
        c.port = 4000;
        c.destAddress = "localhost";
        CHECK(!UdpSink(c).start(err) && err.find("not an IPv4") != std::string::npos);
        c.destAddress = "0.0.0.0";
        CHECK(!UdpSink(c).start(err));
        c.destAddress = "255.255.255.255";
        CHECK(!UdpSink(c).start(err) && err.find("broadcast") != std::string::npos);
        c.destAddress = "239.1.2.3";
        CHECK(!UdpSink(c).start(err) && err.find("multicast") != std::string::npos);
        c.allowBroadcast = true;  // asked for explicitly
        {
            UdpSink ok(c);
            CHECK(ok.start(err));
        }
        c.allowBroadcast = false;
        c.destAddress = "127.0.0.1";
        c.maxDatagram = 8;
        CHECK(!UdpSink(c).start(err));
        c.maxDatagram = kUdpMaxDatagram + 1;
        CHECK(!UdpSink(c).start(err));
        c.maxDatagram = 100;
        UdpSink s(c);
        CHECK(!s.send("x", 1));  // not running
        CHECK(s.start(err));
        CHECK(!s.start(err) && err == "already running");
        std::string big(101, 'x');
        CHECK(!s.send(big.data(), big.size()));  // longer than maxDatagram
        CHECK(!s.send("", 0));                    // empty
        CHECK(s.stats().oversize == 2);
    }
}

void testUdpDelivery() {
    UdpListener rx;
    CHECK(rx.open());
    UdpConfig c;
    c.port = rx.port;
    UdpSink sink(c);
    std::string err;
    CHECK(sink.start(err));
    std::vector<std::string> sent;
    for (int i = 0; i < 200; ++i) {
        std::string m = "datagram " + std::to_string(i) + std::string(static_cast<std::size_t>(i % 50), '#');
        CHECK(sink.send(m.data(), m.size()));
        sent.push_back(m);
    }
    const auto got = rx.collect(sent.size(), 3000);
    CHECK(got.size() == sent.size());
    CHECK(got == sent);  // loopback: all of them, in order, each datagram whole
    CHECK(sink.stats().sent == 200 && sink.stats().ringDropped == 0);
    sink.stop();
    CHECK(!sink.send("x", 1));
    // stop is idempotent and restart works
    sink.stop();
    CHECK(sink.start(err));
    CHECK(sink.send("again", 5));
    CHECK(rx.collect(1, 2000).size() == 1);
}

void testUdpStopWhileSending() {
    UdpListener rx;
    CHECK(rx.open());
    double worst = 0;
    for (int round = 0; round < 5; ++round) {
        UdpConfig c;
        c.port = rx.port;
        UdpSink sink(c);
        std::string err;
        CHECK(sink.start(err));
        std::atomic<bool> stop{false};
        std::thread prod([&] {
            char b[500];
            std::memset(b, 'u', sizeof(b));
            while (!stop.load()) { sink.send(b, sizeof(b)); }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto t0 = nettest::Clock::now();
        sink.stop();
        worst = std::max(worst, nettest::msSince(t0));
        stop.store(true);
        prod.join();
        CHECK(sink.stats().sent > 0);
    }
    std::printf("udp stop() while the producer sends flat out: worst of 5 = %.1f ms\n", worst);
    CHECK(worst < 500.0);
}

void testUdpDropsCounted() {
    // A flood far faster than the sender thread can drain: the ring overflows, the drops are COUNTED, the producer is never blocked.
    UdpListener rx;
    CHECK(rx.open());
    UdpConfig c;
    c.port = rx.port;
    c.ringBytes = 8192;
    UdpSink sink(c);
    std::string err;
    CHECK(sink.start(err));
    char b[1000];
    std::memset(b, 'd', sizeof(b));
    int accepted = 0;
    const auto t0 = nettest::Clock::now();
    for (int i = 0; i < 100000; ++i) { accepted += sink.send(b, sizeof(b)) ? 1 : 0; }
    const double ms = nettest::msSince(t0);
    CHECK(accepted < 100000);
    CHECK(sink.stats().ringDropped + static_cast<std::uint64_t>(accepted) == 100000);
    CHECK(ms < 2000.0);
    std::printf("udp flood: %d of 100000 accepted by an 8 KiB ring, %llu counted as dropped, producer took %.1f ms\n", accepted,
                static_cast<unsigned long long>(sink.stats().ringDropped), ms);
}

void testAisOverTcpAndUdp() {
    UdpListener rx;
    CHECK(rx.open());
    FanoutConfig tc;
    tc.port = 0;
    tc.ringBytes = 256 * 1024;
    UdpConfig uc;
    uc.port = rx.port;
    AisOut out(tc, uc, true, true);
    std::string err;
    CHECK(out.start(err));
    Client cl;
    CHECK(cl.connect(out.tcpPort()));
    for (int spins = 0; out.tcpStats().clients < 1 && spins < 200; ++spins) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    const Decoded one = decode(std::string(kOne) + "\r\n");
    Bits whole = decode(std::string(kTwoA) + "\r\n").bits;
    const Bits b2 = decode(std::string(kTwoB) + "\r\n").bits;
    whole.insert(whole.end(), b2.begin(), b2.end());
    CHECK(out.submit(pack(one.bits).data(), one.bits.size(), 'B') == 1);
    CHECK(out.submit(pack(whole).data(), whole.size(), 'B') == 2);   // sequence id 0 for the first multi-sentence message
    CHECK(out.submit(pack(whole).data(), whole.size(), 'A') == 2);   // ... then 1
    CHECK(out.submit(nullptr, 5, 'A') == 0);
    CHECK(out.submit(pack(one.bits).data(), one.bits.size(), 'X') == 0);
    const auto udp = rx.collect(5, 3000);
    CHECK(udp.size() == 5);
    CHECK(cl.waitBytes(1, 2000));
    std::string tcp;
    for (int spins = 0; spins < 100; ++spins) {
        cl.pump(30);
        tcp.assign(cl.got.begin(), cl.got.end());
        if (std::count(tcp.begin(), tcp.end(), '\n') >= 5) { break; }
    }
    // the TCP stream is the five sentences, one per line, in order; the UDP datagrams are the same five, one sentence each
    std::string joined;
    for (const std::string& u : udp) { joined += u; }
    CHECK(tcp == joined);
    CHECK(udp.size() == 5 && udp[0] == std::string(kOne) + "\r\n");
    if (udp.size() == 5) {
        const Decoded s1 = decode(udp[1]), s3 = decode(udp[3]);
        CHECK(s1.ok && s1.seq == "0" && s1.count == 2);
        CHECK(s3.ok && s3.seq == "1" && s3.channel == "A");
    }
    out.stop();
}

void testAisSequenceRolls() {
    FanoutConfig tc;
    tc.port = 0;
    UdpConfig uc;
    UdpListener rx;
    CHECK(rx.open());
    uc.port = rx.port;
    AisOut out(tc, uc, false, true);
    std::string err;
    CHECK(out.start(err));
    Bits big(400, 1);
    const auto p = pack(big);
    for (int i = 0; i < 25; ++i) { CHECK(out.submit(p.data(), big.size(), 'A') == 2); }
    const auto got = rx.collect(50, 4000);
    CHECK(got.size() == 50);
    for (std::size_t i = 0; i + 1 < got.size(); i += 2) {
        const Decoded a = decode(got[i]), b = decode(got[i + 1]);
        CHECK(a.ok && b.ok && a.seq == b.seq);
        CHECK(a.seq == std::to_string((i / 2) % 10));  // 0..9 then back to 0
    }
}

}  // namespace

int main() {
    testPublishedExamples();
    testFillBitsAndBoundaries();
    testRefusals();
    testUdpSinkPolicy();
    testUdpDelivery();
    testUdpStopWhileSending();
    testUdpDropsCounted();
    testAisOverTcpAndUdp();
    testAisSequenceRolls();
    return testSummary("test_ais_out");
}
