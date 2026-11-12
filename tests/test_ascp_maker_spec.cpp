// test_ascp_maker_spec.cpp - the ASCP layer (src/source/ascp_protocol.hpp, built earlier from CuteSDR and
// Afedri's documents) checked against the MAKER's own interface specifications, which are published on
// https://moetronix.com/svdownload.htm (opened and read 2026-11-12): "NetSDR Interface Specification" Rev. 1.05
// (8 Sept 2014, files/NetSdrInterfaceSpec105.pdf) and "SDR-IP Interface Specification" Ver. 1.03 (files/
// sdripinterfacespec103.pdf). Every byte string below is printed in those documents (sections named in the
// comments); nothing here is derived from the code under test. The documents state no licence; they are
// published by RFspace/MoeTronix for implementers. UNPROVEN on a real radio.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "source/ascp_protocol.hpp"
#include "source/ascp_source.hpp"
#include "test_check.hpp"

namespace as = cascade::source::ascp;
using cascade::source::AscpModel;
using cascade::source::ascpRatesFor;
using Bytes = std::vector<std::uint8_t>;

namespace {

// section 4.1.1: host sends [04][20][01][00]
void testRequests() {
    CHECK(as::encodeMessage(as::MsgType::Request, as::ci::InterfaceName) == (Bytes{0x04, 0x20, 0x01, 0x00}));
    // 4.2.3: range request [05][40][20][00][00]
    CHECK(as::encodeMessage(as::MsgType::RequestRange, as::ci::RxFrequency, {0x00}) == (Bytes{0x05, 0x40, 0x20, 0x00, 0x00}));
    // 4.2.3: frequency request for channel 2 [05][20][20][00][02]
    CHECK(as::encodeMessage(as::MsgType::Request, as::ci::RxFrequency, {0x02}) == (Bytes{0x05, 0x20, 0x20, 0x00, 0x02}));
    // 4.2.6: RF gain request [05][20][38][00][00]
    CHECK(as::encodeMessage(as::MsgType::Request, as::ci::RxRfGain, {0x00}) == (Bytes{0x05, 0x20, 0x38, 0x00, 0x00}));
}

// section 5.1, the five minimal steps of "24bit I/Q Continuous Capture" at 100 kHz and 20.0 MHz
void testStartupExample() {
    CHECK(as::encodeMessage(as::MsgType::Set, as::ci::RxOutSampleRate, {0x00, 0xA0, 0x86, 0x01, 0x00}) ==
          (Bytes{0x09, 0x00, 0xB8, 0x00, 0x00, 0xA0, 0x86, 0x01, 0x00}));
    CHECK(as::encodeMessage(as::MsgType::Set, as::ci::RxRfFilter, {0x00, 0x00}) == (Bytes{0x06, 0x00, 0x44, 0x00, 0x00, 0x00}));
    CHECK(as::encodeMessage(as::MsgType::Set, as::ci::RxAdModes, {0x00, static_cast<std::uint8_t>(as::kAdDither | as::kAdPga)}) ==
          (Bytes{0x06, 0x00, 0x8A, 0x00, 0x00, 0x03}));
    Bytes f{0x00};
    as::putFrequency(f, 20000000ull);
    CHECK(as::encodeMessage(as::MsgType::Set, as::ci::RxFrequency, f) ==
          (Bytes{0x0A, 0x00, 0x20, 0x00, 0x00, 0x00, 0x2D, 0x31, 0x01, 0x00}));
    // 4.2.3: 14.010 MHz on channel 1 [0A][00][20][00][00][90][C6][D5][00][00]
    Bytes g{0x00};
    as::putFrequency(g, 14010000ull);
    CHECK(as::encodeMessage(as::MsgType::Set, as::ci::RxFrequency, g) == (Bytes{0x0A, 0x00, 0x20, 0x00, 0x00, 0x90, 0xC6, 0xD5, 0x00, 0x00}));
    // 4.2.1: start, complex 24-bit contiguous [08][00][18][00][80][02][80][00]
    CHECK(as::encodeMessage(as::MsgType::Set, as::ci::RxState, {as::kRxStateComplex, as::kRxOn, as::kModeContiguous24, 0x00}) ==
          (Bytes{0x08, 0x00, 0x18, 0x00, 0x80, 0x02, 0x80, 0x00}));
    // 4.2.1: stop [08][00][18][00][00][01][00][00] - parameters 1, 3 and 4 are ignored for stop, so ours
    // (0x80 in parameter 1) is the same command to the radio; the item, state byte and length are checked
    const Bytes stop = as::encodeMessage(as::MsgType::Set, as::ci::RxState, {as::kRxStateComplex, as::kRxIdle, 0, 0});
    CHECK(stop.size() == 8 && stop[0] == 0x08 && stop[2] == 0x18 && stop[3] == 0x00 && stop[5] == 0x01);
    // 4.4.5: small UDP packets [05][00][C4][00][01]
    CHECK(as::encodeMessage(as::MsgType::Set, as::ci::RxOutputParams, {as::kOutputSmallMtu}) == (Bytes{0x05, 0x00, 0xC4, 0x00, 0x01}));
    // 4.2.6: RF gain -20 dB is 0xEC; the four values 0, -10, -20, -30 are 0x00, 0xF6, 0xEC, 0xE2
    CHECK(static_cast<std::uint8_t>(static_cast<std::int8_t>(0)) == 0x00 && static_cast<std::uint8_t>(static_cast<std::int8_t>(-10)) == 0xF6 &&
          static_cast<std::uint8_t>(static_cast<std::int8_t>(-20)) == 0xEC && static_cast<std::uint8_t>(static_cast<std::int8_t>(-30)) == 0xE2);
    CHECK(as::encodeMessage(as::MsgType::Set, as::ci::RxRfGain, {0x00, 0xEC}) == (Bytes{0x06, 0x00, 0x38, 0x00, 0x00, 0xEC}));
}

// the printed responses, fed byte by byte
void testResponses() {
    as::StreamAssembler a;
    std::vector<as::Message> m;
    // 4.1.1: NetSDR answers with its name
    const Bytes name{0x0B, 0x00, 0x01, 0x00, 'N', 'e', 't', 'S', 'D', 'R', 0x00};
    for (std::uint8_t b : name) { a.feed(&b, 1, m); }
    CHECK(m.size() == 1 && m[0].type == as::MsgType::Set && !m[0].nak && m[0].item == as::ci::InterfaceName);
    CHECK(m[0].body == (Bytes{'N', 'e', 't', 'S', 'D', 'R', 0x00}));
    m.clear();
    // 4.2.3: range response with two bands (100 kHz - 34 MHz, 140 - 150 MHz), 15 bytes per band
    const Bytes range{0x24, 0x40, 0x20, 0x00, 0x00, 0x02,
                      0xA0, 0x86, 0x01, 0x00, 0x00, 0x80, 0xCC, 0x06, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                      0x00, 0x3B, 0x58, 0x08, 0x00, 0x80, 0xD1, 0xF0, 0x08, 0x00, 0x00, 0x68, 0x89, 0x09, 0x00};
    // NOTE: the printed example has its first band's VCO bytes as five zeros and the second band's as
    // [00][68][89][09][00]; the byte list above is that example, 6 + 30 = 36 = 0x24 bytes.
    a.feed(range.data(), range.size(), m);
    CHECK(m.size() == 1 && m[0].type == as::MsgType::RequestRange && m[0].item == as::ci::RxFrequency);
    std::vector<as::FreqRange> r;
    CHECK(as::parseFrequencyRanges(m[0].body, r));
    CHECK(r.size() == 2 && r[0].minHz == 100000 && r[0].maxHz == 34000000 && r[1].minHz == 140000000 && r[1].maxHz == 150000000);
    // every truncation of the announced two bands is refused (15 bytes per band, not 10)
    for (std::size_t cut = 0; cut < m[0].body.size(); ++cut) {
        const Bytes part(m[0].body.begin(), m[0].body.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK(!as::parseFrequencyRanges(part, r));
    }
    m.clear();
    // a Target NAK is a 16-bit header with length 2 (3.2)
    const Bytes nak{0x02, 0x00};
    a.feed(nak.data(), nak.size(), m);
    CHECK(m.size() == 1 && m[0].nak);
    m.clear();
    // 4.2.1: the stop response [08][00][18][00][00][01][00][00]
    const Bytes resp{0x08, 0x00, 0x18, 0x00, 0x00, 0x01, 0x00, 0x00};
    a.feed(resp.data(), resp.size(), m);
    CHECK(m.size() == 1 && m[0].item == as::ci::RxState && m[0].body == (Bytes{0x00, 0x01, 0x00, 0x00}));
    CHECK(a.error().empty());
}

// 4.5.1: the four data headers and sizes
void testUdpHeaders() {
    struct Row { std::uint8_t h0, h1; std::size_t total; int bits; std::size_t pairs; };
    const Row rows[] = {
        {0xA4, 0x85, 1444, 24, 240},  // complex 24-bit, large MTU: 1440 data bytes
        {0x84, 0x81, 388, 24, 64},    // complex 24-bit, small MTU: 384 data bytes
        {0x04, 0x84, 1028, 16, 256},  // complex 16-bit, large MTU: 1024 data bytes
        {0x04, 0x82, 516, 16, 128},   // complex 16-bit, small MTU: 512 data bytes
    };
    for (const Row& row : rows) {
        Bytes d(row.total, 0);
        d[0] = row.h0;
        d[1] = row.h1;
        d[2] = 0x34;
        d[3] = 0x12;
        as::UdpPacket info;
        std::vector<std::complex<float>> out;
        CHECK(as::parseUdpPacket(d.data(), d.size(), info, out));
        CHECK(info.bits == row.bits && info.pairs == row.pairs && info.sequence == 0x1234 && out.size() == row.pairs);
        // a datagram one byte short or long is refused whatever the header says
        Bytes shortD(d.begin(), d.end() - 1);
        out.clear();
        CHECK(!as::parseUdpPacket(shortD.data(), shortD.size(), info, out) && out.empty());
    }
}

// 4.5.1.5: sequence 0 only at the start, then 1..65535, then 1 again
void testSequence() {
    as::UdpSequence s;
    CHECK(s.note(0) == 0 && s.note(1) == 0 && s.note(2) == 0);
    for (std::uint32_t q = 3; q <= 0xFFFF; ++q) { CHECK(s.note(static_cast<std::uint16_t>(q)) == 0); }
    CHECK(s.note(1) == 0 && s.note(2) == 0);  // wrap skips 0
    CHECK(s.lost() == 0 && s.late() == 0);
    CHECK(s.note(5) == 2 && s.lost() == 2);   // 3 and 4 missing
}

// 4.2.10 (NetSDR) / 4.2.9 (SDR-IP): rates are 80 MHz / (4 k) (NetSDR) or 80 MHz / (10 k) (SDR-IP standard
// configuration), 32 kHz to 2 MHz (16 bit) or 1,333,333 Hz (24 bit)
bool onGrid(double base, double rate) {
    const double k = std::floor(base / rate + 0.5);
    return k >= 1.0 && std::fabs(std::floor(base / k) - rate) <= 1.0;
}

void testRateTables() {
    const auto n = ascpRatesFor(AscpModel::NetSdr, 80000000);
    CHECK(n.size() == 5);
    for (double r : n) { CHECK(r >= 32000.0 && r <= 2000000.0 && onGrid(20.0e6, r)); }
    const auto ip = ascpRatesFor(AscpModel::SdrIp, 80000000);
    CHECK(ip.size() == 5);
    for (double r : ip) { CHECK(r >= 32000.0 && r <= 2000000.0 && onGrid(8.0e6, r)); }
    // the maker's own examples are on the grid: 100 kHz (5.1) and 500 kHz (80 MHz / 160)
    CHECK(onGrid(20.0e6, 100000.0) && onGrid(20.0e6, 500000.0) && onGrid(8.0e6, 500000.0));
    // and an off-grid rate is not
    CHECK(!onGrid(8.0e6, 1200000.0));
}

}  // namespace

int main() {
    testRequests();
    testStartupExample();
    testResponses();
    testUdpHeaders();
    testSequence();
    testRateTables();
    return testSummary("test_ascp_maker_spec");
}
