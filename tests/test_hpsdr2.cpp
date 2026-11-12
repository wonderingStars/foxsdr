// test_hpsdr2.cpp - the openHPSDR protocol-2 (Ethernet protocol) receive client: packet bytes as literals from
// the project's document (V4.4, cited in src/source/hpsdr2_protocol.hpp), every refusal, fuzz, and the whole
// source against a STAND-IN radio on 127.0.0.1 UDP that answers discovery, reads the General / DDC-Specific /
// High-Priority packets with its own parsing and streams generated DDC I&Q (tone at a frequency offset, noise,
// a sample-clock error) from its own source port, with status packets from another.
//
// The stand-in is written from the same reading as the code, so it cannot show the reading was right:
// NOTHING HERE IS PROVEN ON A REAL HPSDR RADIO (none on the desk). Network: 127.0.0.1 only; nothing broadcast.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "source/hpsdr2_protocol.hpp"
#include "source/hpsdr2_source.hpp"
#include "src_test_net.hpp"
#include "test_check.hpp"

using namespace srctest;
using cascade::source::Hpsdr2Device;
using cascade::source::Hpsdr2Source;
using cascade::source::NetSampleSource;
namespace h2 = cascade::source::hpsdr2;

namespace {

using Cx = std::complex<float>;

// ---- A: packet bytes -----------------------------------------------------------------------------------

void testDiscoveryBytes() {
    const auto d = h2::buildDiscovery();
    CHECK(d.size() == 60 && d[4] == 0x02);
    CHECK(std::all_of(d.begin(), d.begin() + 4, [](std::uint8_t b) { return b == 0; }));  // sequence 0
    CHECK(std::all_of(d.begin() + 5, d.end(), [](std::uint8_t b) { return b == 0; }));
    // a reply, field by field: seq 0, 0x02, MAC, board 1, protocol 39, firmware 32, ..., DDCs 4, phase, formats 0x05, beta 1
    Bytes r(60, 0);
    r[4] = 0x02;
    for (int i = 0; i < 6; ++i) { r[5 + i] = static_cast<std::uint8_t>(0xA0 + i); }
    r[11] = 1; r[12] = 39; r[13] = 32; r[20] = 4; r[21] = 1; r[22] = 0x05; r[23] = 1;
    h2::DiscoveryReply rep;
    CHECK(h2::parseDiscoveryReply(r.data(), r.size(), rep));
    CHECK(!rep.busy && rep.macText() == "a0:a1:a2:a3:a4:a5" && rep.boardType == 1 && rep.protocolVersion == 39);
    CHECK(rep.firmwareVersion == 32 && rep.numDdcs == 4 && rep.phaseWord && rep.formatModes == 0x05 && rep.beta);
    r[4] = 0x03;
    CHECK(h2::parseDiscoveryReply(r.data(), r.size(), rep) && rep.busy);
    CHECK(h2::parseDiscoveryReply(r.data(), 24, rep));    // the 24 bytes the fields need
    CHECK(!h2::parseDiscoveryReply(r.data(), 23, rep));
    CHECK(!h2::parseDiscoveryReply(nullptr, 0, rep));
    for (const int bad : {0x00, 0x01, 0x04, 0xFE, 0xFF}) {  // 0xFE / 0xFF: long descriptions, not parsed
        r[4] = static_cast<std::uint8_t>(bad);
        CHECK(!h2::parseDiscoveryReply(r.data(), r.size(), rep));
    }
    r[4] = 0x02; r[3] = 1;                                // a non-zero sequence number is not a discovery reply
    CHECK(!h2::parseDiscoveryReply(r.data(), r.size(), rep));
    // boards: the document's Appendix A has tables for 1 to 4
    for (int t = 1; t <= 4; ++t) { CHECK(h2::boardInfo(static_cast<std::uint8_t>(t)).documented && h2::boardInfo(static_cast<std::uint8_t>(t)).dspClockHz == 122880000.0); }
    CHECK(h2::boardInfo(1).adcs == 1 && h2::boardInfo(2).adcs == 1 && h2::boardInfo(3).adcs == 2 && h2::boardInfo(4).adcs == 2);
    for (const int t : {0, 5, 6, 10, 7, 254, 255}) { CHECK(!h2::boardInfo(static_cast<std::uint8_t>(t)).documented); }
    CHECK(std::string(h2::boardInfo(6).name) == "Hermes Lite" && std::string(h2::boardInfo(10).name).find("Saturn") == 0);
}

void testGeneralBytes() {
    h2::PortPlan p;
    p.ddcSpecific = 0x1234; p.highPriorityToHardware = 0x2345; p.highPriorityFromHardware = 0x3456; p.ddc0 = 0x4567;
    const auto g = h2::buildGeneral(0x01020304, p, true);
    CHECK(g.size() == 60);
    CHECK(g[0] == 1 && g[1] == 2 && g[2] == 3 && g[3] == 4 && g[4] == 0x00);       // sequence, command 0x00
    CHECK(g[5] == 0x12 && g[6] == 0x34);                                              // DDC specific port
    CHECK(g[7] == 0 && g[8] == 0);                                                    // DUC specific: default
    CHECK(g[9] == 0x23 && g[10] == 0x45 && g[11] == 0x34 && g[12] == 0x56);          // high priority from / to PC
    CHECK(g[13] == 0 && g[14] == 0 && g[15] == 0 && g[16] == 0);
    CHECK(g[17] == 0x45 && g[18] == 0x67);                                            // DDC0 port
    CHECK(g[23] == 0);                                                                // wideband off
    CHECK(g[37] == 0x08 && g[38] == 0x01 && g[39] == 0x00);                          // phase word, reset timer, big-endian 3-byte
    CHECK(g[58] == 0 && g[59] == 0);                                                  // PA off, Alex off
    CHECK(h2::buildGeneral(7, p, false)[37] == 0x00);
    // everything else is zero
    std::size_t nz = 0;
    for (std::size_t i = 0; i < g.size(); ++i) { if (g[i] != 0) { ++nz; } }
    CHECK(nz == 4 /*seq bytes 1,2,3,4*/ + 8 /*ports*/ + 2 /*byte 37, 38*/);
    const auto def = h2::buildGeneral(0, h2::PortPlan{}, true);
    CHECK(def[5] == 0 && def[6] == 0 && def[9] == 0 && def[17] == 0);                // zero = default ports
    h2::PortPlan z;
    CHECK(z.effectiveDdcSpecific() == 1025 && z.effectiveHighPriorityToHardware() == 1027 &&
          z.effectiveHighPriorityFromHardware() == 1025 && z.effectiveDdc0() == 1035);
}

void testSpecificAndHighPriority() {
    const auto s = h2::buildDdcSpecific(9, 2, 192000);
    CHECK(s.size() == 1444 && s[3] == 9 && s[4] == 2 && s[5] == 0 && s[6] == 0);
    CHECK(s[7] == 0x01 && s[8] == 0);                              // DDC0 enabled only
    CHECK(s[17] == 0 && s[18] == 0x00 && s[19] == 0xC0 && s[22] == 24);  // ADC0, 192 ksps = 0x00C0, 24 bits
    std::size_t nz = 0;
    for (const std::uint8_t b : s) { if (b != 0) { ++nz; } }
    CHECK(nz == 1 /*seq*/ + 1 /*adcs*/ + 1 /*enable*/ + 1 /*rate low*/ + 1 /*size*/);
    const int rates[6] = {48000, 96000, 192000, 384000, 768000, 1536000};
    const std::uint16_t ksps[6] = {48, 96, 192, 384, 768, 1536};
    for (int i = 0; i < 6; ++i) {
        const auto b = h2::buildDdcSpecific(0, 1, rates[i]);
        CHECK(b.size() == 1444 && ((b[18] << 8) | b[19]) == ksps[i]);
        std::uint16_t k = 0;
        CHECK(h2::rateToKsps(rates[i], k) && k == ksps[i]);
    }
    for (const int bad : {0, 1, 44100, 47999, 48001, 3072000, -48000}) { CHECK(h2::buildDdcSpecific(0, 1, bad).empty()); }
    CHECK(h2::buildDdcSpecific(0, 0, 48000).empty() && h2::buildDdcSpecific(0, 9, 48000).empty());
    std::uint16_t k = 0;
    CHECK(!h2::rateToKsps(0, k) && !h2::rateToKsps(1536001, k));
    // high priority
    const auto hp = h2::buildHighPriority(0xAABBCCDD, true, 0x0EBCCCCD, 12);
    CHECK(hp.size() == 1444 && hp[0] == 0xAA && hp[3] == 0xDD && hp[4] == 0x01);
    CHECK(hp[9] == 0x0E && hp[10] == 0xBC && hp[11] == 0xCC && hp[12] == 0xCD);
    CHECK(hp[1443] == 12 && hp[345] == 0 && hp[1442] == 0);                // drive level 0, ADC1 attenuator untouched
    CHECK((hp[4] & 0x02) == 0);                                              // PTT never set
    nz = 0;
    for (std::size_t i = 0; i < hp.size(); ++i) { if (hp[i] != 0) { ++nz; } }
    CHECK(nz == 4 /*seq*/ + 1 /*run*/ + 4 /*word*/ + 1 /*atten*/);
    const auto stopPk = h2::buildHighPriority(1, false, 0, 0);
    CHECK(stopPk[4] == 0 && stopPk[1443] == 0);
    CHECK(h2::buildHighPriority(0, true, 0, 99)[1443] == 31 && h2::buildHighPriority(0, true, 0, -5)[1443] == 0);
}

void testFrequencyWord() {
    bool ok = false;
    CHECK(h2::frequencyWord(7074000.0, true, 122880000.0, ok) == 0x0EBCCCCDu && ok);   // 2^32 * f / 122.88 MHz, rounded
    CHECK(h2::frequencyWord(14074000.0, true, 122880000.0, ok) == 0x1D522222u && ok);
    CHECK(h2::frequencyWord(0.0, true, 122880000.0, ok) == 0 && ok);
    CHECK(h2::frequencyWord(1.0, true, 122880000.0, ok) == 0x23u && ok);
    CHECK(h2::frequencyWord(61440000.0, true, 122880000.0, ok) == 0x80000000u && ok);   // Nyquist
    CHECK(h2::frequencyWord(122879999.0, true, 122880000.0, ok) == 0xFFFFFFDDu && ok);
    h2::frequencyWord(122880000.0, true, 122880000.0, ok);
    CHECK(!ok);                                                                           // the clock itself: does not fit
    h2::frequencyWord(-1.0, true, 122880000.0, ok);
    CHECK(!ok);
    h2::frequencyWord(std::nan(""), true, 122880000.0, ok);
    CHECK(!ok);
    h2::frequencyWord(1.0, true, 0.0, ok);
    CHECK(!ok);                                                                           // no clock, no phase word
    CHECK(h2::frequencyWord(7074000.0, false, 0.0, ok) == 7074000u && ok);                // plain Hz
    CHECK(h2::frequencyWord(4294967295.0, false, 0.0, ok) == 4294967295u && ok);
    h2::frequencyWord(4294967296.0, false, 0.0, ok);
    CHECK(!ok);
    h2::frequencyWord(-0.5, false, 0.0, ok);
    CHECK(!ok);
}

void testDdcPacket() {
    // literal: seq 0x00000102, time stamp 8 bytes, bits 0x0018, count 2, then I0 Q0 I1 Q1
    Bytes b(16 + 12, 0);
    b[2] = 0x01; b[3] = 0x02;
    b[12] = 0x00; b[13] = 0x18; b[14] = 0x00; b[15] = 0x02;
    const std::uint8_t s[12] = {0x12, 0x34, 0x56,  0xFE, 0xDC, 0xBA,  0x80, 0x00, 0x00,  0x7F, 0xFF, 0xFF};
    for (int i = 0; i < 12; ++i) { b[16 + static_cast<std::size_t>(i)] = s[i]; }
    h2::DdcPacket p;
    CHECK(h2::parseDdcPacket(b.data(), b.size(), p) == h2::DdcVerdict::Ok && p.sequence == 0x0102 && p.samples == 2 && p.data == b.data() + 16);
    std::vector<Cx> out;
    h2::decodeDdcSamples(p, out);
    CHECK(out.size() == 2);
    CHECK(out[0].real() == 1193046.0f / 8388608.0f && out[0].imag() == -74566.0f / 8388608.0f);   // 0x123456, 0xFEDCBA
    CHECK(out[1].real() == -1.0f && out[1].imag() == 8388607.0f / 8388608.0f);                    // 0x800000, 0x7FFFFF
    // refusals
    CHECK(h2::parseDdcPacket(b.data(), 15, p) == h2::DdcVerdict::WrongLength);
    CHECK(h2::parseDdcPacket(nullptr, 0, p) == h2::DdcVerdict::WrongLength);
    CHECK(h2::parseDdcPacket(b.data(), b.size() - 1, p) == h2::DdcVerdict::WrongLength);
    Bytes longer = b;
    longer.push_back(0);
    CHECK(h2::parseDdcPacket(longer.data(), longer.size(), p) == h2::DdcVerdict::WrongLength);
    Bytes sixteen = b;
    sixteen[13] = 16;
    CHECK(h2::parseDdcPacket(sixteen.data(), sixteen.size(), p) == h2::DdcVerdict::BadSampleSize);
    sixteen[13] = 32;
    CHECK(h2::parseDdcPacket(sixteen.data(), sixteen.size(), p) == h2::DdcVerdict::BadSampleSize);
    Bytes none = b;
    none[15] = 0;
    none.resize(16);
    CHECK(h2::parseDdcPacket(none.data(), none.size(), p) == h2::DdcVerdict::BadCount);
    Bytes many(16 + 6 * 239, 0);
    many[13] = 24; many[14] = 0; many[15] = 239;
    CHECK(h2::parseDdcPacket(many.data(), many.size(), p) == h2::DdcVerdict::BadCount);
    many[15] = 0xFF; many[14] = 0xFF;                  // 65535 claimed in a short buffer
    CHECK(h2::parseDdcPacket(many.data(), many.size(), p) == h2::DdcVerdict::BadCount);
    CHECK(std::string(h2::verdictName(h2::DdcVerdict::BadSampleSize)).find("24") != std::string::npos);
    // 238 samples = 1444 bytes; every count round-trips through the builder
    std::vector<std::int32_t> iq(2 * 238);
    for (int n = 1; n <= 238; ++n) {
        for (int i = 0; i < 2 * n; ++i) { iq[static_cast<std::size_t>(i)] = ((i * 7919 + n) % 0xFFFFFF) - 0x800000; }
        const auto pk = h2::buildDdcPacket(static_cast<std::uint32_t>(n), iq.data(), n);
        CHECK(pk.size() == 16u + 6u * static_cast<unsigned>(n));
        h2::DdcPacket q;
        CHECK(h2::parseDdcPacket(pk.data(), pk.size(), q) == h2::DdcVerdict::Ok && q.samples == n && q.sequence == static_cast<std::uint32_t>(n));
        std::vector<Cx> o;
        h2::decodeDdcSamples(q, o);
        bool same = static_cast<int>(o.size()) == n;
        for (int i = 0; same && i < n; ++i) {
            same = o[static_cast<std::size_t>(i)].real() == static_cast<float>(iq[static_cast<std::size_t>(2 * i)]) / 8388608.0f &&
                   o[static_cast<std::size_t>(i)].imag() == static_cast<float>(iq[static_cast<std::size_t>(2 * i + 1)]) / 8388608.0f;
        }
        CHECK(same);
    }
    CHECK(h2::buildDdcPacket(0, iq.data(), 238).size() == 1444);
    CHECK(h2::buildDdcPacket(0, iq.data(), 1000).size() == 1444);  // capped at 238
    CHECK(h2::buildDdcPacket(0, iq.data(), -3).size() == 16);
}

void testStatusBytes() {
    Bytes b(60, 0);
    b[3] = 5;
    b[4] = 0x10;
    b[5] = 0x01;
    b[30] = 0x01;
    h2::Status st;
    CHECK(h2::parseStatus(b.data(), b.size(), st) && st.sequence == 5 && st.pllLocked && st.adcOverload == 1 && st.ddcFifoOverflow);
    b[4] = 0; b[5] = 0x80; b[30] = 0x02;
    CHECK(h2::parseStatus(b.data(), b.size(), st) && !st.pllLocked && st.adcOverload == 0x80 && !st.ddcFifoOverflow);
    CHECK(!h2::parseStatus(b.data(), 59, st) && !h2::parseStatus(b.data(), 61, st) && !h2::parseStatus(nullptr, 60, st));
}

// ---- B: the stand-in radio -----------------------------------------------------------------------------

class P2Radio {
public:
    explicit P2Radio(std::uint8_t board = 1) : board_(board) {}
    ~P2Radio() { stop(); }
    bool start() {
        for (UdpSock* s : {&main_, &ddc_, &hpIn_, &status_, &ddc0_}) {
            if (!s->open()) { return false; }
            const int big = 1 << 20;  // a busy machine must not make the stand-in drop the PC's datagrams
            ::setsockopt(s->sock(), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&big), sizeof(big));
        }
        run_.store(true);
        lastCcMs_.store(nowMs());
        th_.emplace_back([this] { mainLoop(); });
        th_.emplace_back([this] { ddcSpecificLoop(); });
        th_.emplace_back([this] { hpLoop(); });
        th_.emplace_back([this] { statusLoop(); });
        th_.emplace_back([this] { streamLoop(); });
        return true;
    }
    void stop() {
        run_.store(false);
        for (auto& t : th_) { if (t.joinable()) { t.join(); } }
        th_.clear();
    }
    std::string args(const std::string& extra = "") const {
        return "hpsdr2=127.0.0.1:" + std::to_string(main_.port()) + ",ddcport=" + std::to_string(ddc_.port()) + ",hpport=" +
               std::to_string(hpIn_.port()) + ",statusport=" + std::to_string(status_.port()) + ",ddc0port=" + std::to_string(ddc0_.port()) +
               (extra.empty() ? "" : "," + extra);
    }
    std::uint16_t mainPort() const { return main_.port(); }

    // configuration
    std::atomic<bool> busy{false};
    std::atomic<int> formatModes{0};
    std::atomic<int> phaseFlag{1};
    // signal
    std::atomic<double> rfHz{7074200.0};
    std::atomic<double> rateErr{0.0};
    std::atomic<double> amplitude{0.5};
    std::atomic<double> noise{0.01};
    std::atomic<bool> overload{false};
    std::atomic<bool> fifoOverflow{false};
    // faults
    std::atomic<int> dropAt{-1};
    std::atomic<int> shortAt{-1};         // send a packet one sample short of its length
    std::atomic<int> sixteenBitAt{-1};    // claim 16 bits per sample
    std::atomic<int> stallAfter{-1};
    std::atomic<bool> junkFromOtherPort{false};
    // what the PC told it
    std::atomic<int> discoveries{0}, generals{0}, specifics{0}, hps{0};
    std::atomic<int> run{0};
    std::atomic<int> ksps{-1}, adcs{-1}, ddcEnable{-1}, ddcAdc{-1}, sampleBits{-1};
    std::atomic<std::uint32_t> word{0};
    std::atomic<int> atten{-1};
    std::atomic<int> generalPhase{-1}, generalTimer{-1}, generalFormat{-1};
    std::atomic<int> portsMatch{-1};
    std::atomic<int> pttSeen{0}, driveSeen{0}, unexpectedBytes{0};
    std::atomic<int> seqErrors{0};
    std::atomic<int> standbys{0};
    std::atomic<int> sentPackets{0};
    std::atomic<int> starts{0}, stops{0};

private:
    static long long nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    Bytes reply() const {
        Bytes r(60, 0);
        r[4] = busy.load() ? 0x03 : 0x02;
        const std::uint8_t mac[6] = {0x00, 0x1c, 0xc0, 0xa2, 0x13, 0x88};
        for (int i = 0; i < 6; ++i) { r[5 + static_cast<std::size_t>(i)] = mac[i]; }
        r[11] = board_; r[12] = 39; r[13] = 32; r[20] = 4;
        r[21] = static_cast<std::uint8_t>(phaseFlag.load());
        r[22] = static_cast<std::uint8_t>(formatModes.load());
        return r;
    }
    void checkSeq(std::atomic<std::uint32_t>& expect, std::atomic<bool>& first, std::uint32_t seq) {
        if (first.exchange(false)) { if (seq != 0) { ++seqErrors; } }
        else if (seq != expect.load()) { ++seqErrors; }
        expect.store(seq + 1);
    }
    static std::uint32_t get32(const std::uint8_t* p) {
        return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
    }
    int countOutside(const Bytes& d, std::initializer_list<std::pair<int, int>> allowed) {
        int n = 0;
        for (int i = 4; i < static_cast<int>(d.size()); ++i) {
            bool ok = false;
            for (const auto& a : allowed) { if (i >= a.first && i <= a.second) { ok = true; } }
            if (!ok && d[static_cast<std::size_t>(i)] != 0) { ++n; }
        }
        return n;
    }
    void mainLoop() {
        Bytes d;
        std::uint16_t from = 0;
        while (run_.load()) {
            if (!main_.recvFrom(run_, d, from)) { continue; }
            if (d.size() == 60 && d[4] == 0x02 && get32(d.data()) == 0) {
                ++discoveries;
                hostPort_.store(from);
                const Bytes r = reply();
                main_.sendTo(from, r.data(), r.size());
            } else if (d.size() == 60 && d[4] == 0x00) {
                ++generals;
                hostPort_.store(from);
                checkSeq(expGeneral_, firstGeneral_, get32(d.data()));
                generalPhase = (d[37] >> 3) & 1;
                generalTimer = d[38] & 1;
                generalFormat = d[39];
                const auto p16 = [&d](int i) { return (d[static_cast<std::size_t>(i)] << 8) | d[static_cast<std::size_t>(i) + 1]; };
                portsMatch = (p16(5) == ddc_.port() && p16(9) == hpIn_.port() && p16(11) == status_.port() && p16(17) == ddc0_.port()) ? 1 : 0;
                unexpectedBytes += countOutside(d, {{5, 6}, {9, 12}, {17, 18}, {37, 39}});
            } else {
                ++unexpectedBytes;
            }
        }
    }
    void ddcSpecificLoop() {
        Bytes d;
        std::uint16_t from = 0;
        while (run_.load()) {
            if (!ddc_.recvFrom(run_, d, from)) { continue; }
            if (d.size() != 1444) { ++unexpectedBytes; continue; }
            ++specifics;
            lastCcMs_.store(nowMs());
            checkSeq(expDdc_, firstDdc_, get32(d.data()));
            adcs = d[4];
            ddcEnable = d[7];
            ddcAdc = d[17];
            ksps = (d[18] << 8) | d[19];
            sampleBits = d[22];
            unexpectedBytes += countOutside(d, {{4, 4}, {7, 7}, {17, 19}, {22, 22}});
        }
    }
    void hpLoop() {
        Bytes d;
        std::uint16_t from = 0;
        while (run_.load()) {
            if (!hpIn_.recvFrom(run_, d, from)) { continue; }
            if (d.size() != 1444) { ++unexpectedBytes; continue; }
            ++hps;
            lastCcMs_.store(nowMs());
            checkSeq(expHp_, firstHp_, get32(d.data()));
            const int r = d[4] & 1;
            if ((d[4] & 0x02) != 0) { ++pttSeen; }
            if (d[345] != 0) { ++driveSeen; }
            word = get32(d.data() + 9);
            atten = d[1443];
            unexpectedBytes += countOutside(d, {{4, 4}, {9, 12}, {1443, 1443}}) + ((d[4] & 0xFE) != 0 ? 1 : 0);
            if (r && !run.load()) { ++starts; standby_.store(false); seq_.store(0); firstOut_.store(true); }
            if (!r && run.load()) { ++stops; }
            run = r;
        }
    }
    void statusLoop() {
        std::uint32_t seq = 0;
        while (run_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            const std::uint16_t hp = hostPort_.load();
            if (hp == 0) { continue; }
            Bytes b(60, 0);
            b[0] = static_cast<std::uint8_t>(seq >> 24); b[1] = static_cast<std::uint8_t>(seq >> 16);
            b[2] = static_cast<std::uint8_t>(seq >> 8); b[3] = static_cast<std::uint8_t>(seq);
            b[4] = 0x10;
            b[5] = overload.load() ? 0x01 : 0x00;
            b[30] = fifoOverflow.load() ? 0x01 : 0x00;
            status_.sendTo(hp, b.data(), b.size());
            if (junkFromOtherPort.load()) {  // a mic-port style datagram from a port the source does not listen on
                Bytes j(132, 0x55);
                main_.sendTo(hp, j.data(), j.size());
            }
            ++seq;
        }
    }
    double hzOfWord() const {  // the stand-in's own inverse of the document's formula
        return phaseFlag.load() ? static_cast<double>(word.load()) * 122880000.0 / 4294967296.0 : static_cast<double>(word.load());
    }
    double rateHz() const {
        const int k = ksps.load();
        return k > 0 ? k * 1000.0 : 48000.0;
    }
    void streamLoop() {
        Rng rng(0xFEEDu);
        auto gauss = [&rng]() {
            double s = 0.0;
            for (int i = 0; i < 12; ++i) { s += (rng.next() & 0xFFFFFF) / 16777216.0; }
            return s - 6.0;
        };
        double phase = 0.0;
        int count = 0;
        double refRate = rateHz();
        auto t0 = std::chrono::steady_clock::now();
        long long sent = 0;
        bool wasRunning = false;
        while (run_.load()) {
            const bool running = run.load() != 0 && !standby_.load();
            if (running && nowMs() - lastCcMs_.load() > 1000) {  // the document: no C&C for a second -> standby
                standby_.store(true);
                ++standbys;
                continue;
            }
            if (!running) { wasRunning = false; std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
            if (!wasRunning) { wasRunning = true; seq_.store(0); count = 0; t0 = std::chrono::steady_clock::now(); sent = 0; refRate = rateHz(); }
            if (rateHz() != refRate) { refRate = rateHz(); t0 = std::chrono::steady_clock::now(); sent = 0; }
            const double trueRate = refRate * (1.0 + rateErr.load());
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const long long due = static_cast<long long>(elapsed * trueRate / 238.0) - sent;
            if (due <= 0 || (stallAfter.load() >= 0 && count >= stallAfter.load()) || hostPort_.load() == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            const long long burst = std::min<long long>(due, 30);
            for (long long b = 0; b < burst && run_.load(); ++b, ++sent) {
                std::int32_t iq[2 * 238];
                const double off = rfHz.load() - hzOfWord();
                const double step = 2.0 * 3.14159265358979323846 * off / trueRate;
                for (int i = 0; i < 238; ++i) {
                    phase += step;
                    if (phase > 3.14159265358979323846) { phase -= 2.0 * 3.14159265358979323846; }
                    if (phase < -3.14159265358979323846) { phase += 2.0 * 3.14159265358979323846; }
                    const double a = amplitude.load() * 8388607.0;
                    const double ni = noise.load() * 8388607.0;
                    auto clip = [](double v) { return static_cast<std::int32_t>(std::max(-8388608.0, std::min(8388607.0, std::round(v)))); };
                    iq[2 * i] = clip(a * std::cos(phase) + ni * gauss());
                    iq[2 * i + 1] = clip(a * std::sin(phase) + ni * gauss());
                }
                const std::uint32_t seq = seq_.fetch_add(1);
                Bytes p = h2::buildDdcPacket(seq, iq, 238);
                if (count == sixteenBitAt.load()) { p[13] = 16; }
                const bool dropThis = count == dropAt.load();
                if (count == shortAt.load()) { p.resize(p.size() - 6); }  // length no longer matches the count field
                if (!dropThis) { ddc0_.sendTo(hostPort_.load(), p.data(), p.size()); }
                ++count;
                sentPackets.store(count);
            }
        }
    }

    std::uint8_t board_;
    UdpSock main_, ddc_, hpIn_, status_, ddc0_;
    std::atomic<bool> run_{false};
    std::vector<std::thread> th_;
    std::atomic<std::uint16_t> hostPort_{0};
    std::atomic<std::uint32_t> expGeneral_{0}, expDdc_{0}, expHp_{0}, seq_{0};
    std::atomic<bool> firstGeneral_{true}, firstDdc_{true}, firstHp_{true}, firstOut_{true}, standby_{false};
    std::atomic<long long> lastCcMs_{0};
};

template <typename Pred>
bool waitFor(Pred p, int ms = 8000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (p()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return p();
}

std::vector<Cx> readSamples(Hpsdr2Source& s, std::size_t n, int ms = 8000) {
    std::vector<Cx> out;
    std::vector<Cx> tmp(4096);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (out.size() < n && std::chrono::steady_clock::now() < end) {
        const std::size_t g = s.read(tmp.data(), tmp.size());
        out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(g));
        if (s.faulted()) { break; }
    }
    return out;
}

double toneHz(const std::vector<Cx>& v, double rate) {
    std::complex<double> acc(0.0, 0.0);
    for (std::size_t i = 0; i + 1 < v.size(); ++i) { acc += std::complex<double>(v[i + 1]) * std::conj(std::complex<double>(v[i])); }
    return std::arg(acc) * rate / (2.0 * 3.14159265358979323846);
}

bool settlesAt(Hpsdr2Source& s, double rate, double want, double tol, int ms = 12000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    int good = 0;
    while (std::chrono::steady_clock::now() < end) {
        const auto w = readSamples(s, 4760, 4000);
        if (w.size() < 4760) { return false; }
        if (std::fabs(toneHz(w, rate) - want) <= tol) { if (++good >= 2) { return true; } } else { good = 0; }
    }
    return false;
}

void testReceive() {
    P2Radio radio;
    CHECK(radio.start());
    radio.rateErr = 0.01;
    Hpsdr2Source s;
    CHECK(s.open(radio.args("timeout=3000")));
    CHECK(s.isOpen() && s.device().reply.boardType == 1 && std::string(s.name()).find("HPSDR 2: Hermes") == 0);
    CHECK(s.sampleRateHz() == 48000.0 && s.supportedSampleRatesHz().size() == 6);
    CHECK(s.setSampleRateHz(96000.0) && s.setCenterFrequencyHz(7069200.0));
    CHECK(radio.generals.load() == 0 && s.controlPacketsSent() == 0);  // nothing is sent until Start
    CHECK(s.start() && s.running());
    CHECK(waitFor([&] { return radio.run.load() == 1 && radio.ksps.load() == 96 && radio.generals.load() == 1; }));
    CHECK(radio.portsMatch.load() == 1 && radio.generalPhase.load() == 1 && radio.generalTimer.load() == 1 && radio.generalFormat.load() == 0);
    CHECK(radio.adcs.load() == 1 && radio.ddcEnable.load() == 1 && radio.ddcAdc.load() == 0 && radio.sampleBits.load() == 24);
    CHECK(radio.word.load() == static_cast<std::uint32_t>(std::llround(4294967296.0 * 7069200.0 / 122880000.0)));
    const double wantOffset = 5000.0 / 1.01;  // carrier 5 kHz above the DDC; the radio's clock is 1% fast
    CHECK(settlesAt(s, 96000.0, wantOffset, 6.0));
    const auto w = readSamples(s, 19040);
    CHECK(w.size() >= 19040);
    const double est = toneHz(w, 96000.0);
    CHECK(std::fabs(est - wantOffset) < 3.0 && std::fabs(est - 5000.0) > 30.0);
    double mag = 0.0;
    for (const Cx& c : w) { mag += std::abs(c); }
    mag /= static_cast<double>(w.size());
    CHECK(mag > 0.45 && mag < 0.56);
    // retune: the carrier is now 8 kHz below the DDC
    CHECK(s.setCenterFrequencyHz(7082200.0));
    CHECK(waitFor([&] { return radio.word.load() == static_cast<std::uint32_t>(std::llround(4294967296.0 * 7082200.0 / 122880000.0)); }));
    CHECK(settlesAt(s, 96000.0, -8000.0 / 1.01, 6.0));
    // rate change to 192 kHz: all DDC settings travel in one DDC Specific packet
    const int specBefore = radio.specifics.load();
    CHECK(s.setSampleRateHz(192000.0));
    CHECK(waitFor([&] { return radio.ksps.load() == 192 && radio.specifics.load() == specBefore + 1; }));
    CHECK(settlesAt(s, 192000.0, -8000.0 / 1.01, 8.0));
    // attenuator
    CHECK(s.gains().size() == 1 && s.gains()[0].name == "Attenuator" && s.gains()[0].maxDb == 31.0);
    CHECK(s.setGainDb("Attenuator", 17.0));
    CHECK(waitFor([&] { return radio.atten.load() == 17; }));
    CHECK(s.gainDb("Attenuator") == 17.0);
    // the periodic refresh: C&C every ~100 ms, so the radio never goes to standby over 2.5 s
    const int hpBefore = radio.hps.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    CHECK(radio.hps.load() >= hpBefore + 10 && radio.standbys.load() == 0);
    const auto more = readSamples(s, 4760, 4000);
    CHECK(more.size() >= 4760);
    // status packets arrive on the status port: counted, no overload
    CHECK(s.statusPackets() > 10 && s.adcOverloadPackets() == 0 && s.fifoOverflowPackets() == 0);
    // receive only: no PTT, no drive, no stray bytes, sequence numbers in order
    CHECK(radio.pttSeen.load() == 0 && radio.driveSeen.load() == 0 && radio.unexpectedBytes.load() == 0 && radio.seqErrors.load() == 0);
    if (s.packetsLost() != 0 || s.badPackets() != 0 || s.faulted()) {
        std::fprintf(stderr, "receive: lost=%llu late=%llu bad=%llu faulted=%d err=%s dropped=%llu sent=%d\n", static_cast<unsigned long long>(s.packetsLost()),
                     static_cast<unsigned long long>(s.packetsLate()), static_cast<unsigned long long>(s.badPackets()), s.faulted() ? 1 : 0,
                     s.lastError(), static_cast<unsigned long long>(s.droppedBuffers()), radio.sentPackets.load());
    }
    CHECK(s.packetsLost() == 0 && s.badPackets() == 0 && !s.faulted());
    s.stop();
    CHECK(waitFor([&] { return radio.run.load() == 0 && radio.stops.load() == 1; }));
    // second start: everything goes out again, in the document's order
    CHECK(s.start());
    CHECK(waitFor([&] { return radio.starts.load() == 2 && radio.generals.load() == 2; }));
    CHECK(radio.ksps.load() == 192 && radio.atten.load() == 17);
    CHECK(radio.seqErrors.load() == 0);
    s.closeDevice();
    CHECK(waitFor([&] { return radio.run.load() == 0; }));  // closing clears the run bit
}

void testStatusFlags() {
    P2Radio radio;
    CHECK(radio.start());
    radio.overload = true;
    radio.fifoOverflow = true;
    radio.junkFromOtherPort = true;
    Hpsdr2Source s;
    CHECK(s.open(radio.args("timeout=3000")));
    CHECK(s.start());
    CHECK(waitFor([&] { return s.adcOverloadPackets() > 3 && s.fifoOverflowPackets() > 3; }));
    CHECK(s.badPackets() == 0);  // datagrams from a port the source does not listen on are ignored, not counted bad
    s.closeDevice();
}

void testBoardsAndOpenRefusals() {
    for (const int b : {0, 5, 6, 10, 7}) {
        P2Radio radio(static_cast<std::uint8_t>(b));
        CHECK(radio.start());
        Hpsdr2Source s;
        CHECK(!s.open(radio.args("timeout=500")) && !s.isOpen());
        CHECK(std::string(s.lastError()).find("not supported") != std::string::npos);
    }
    for (const int b : {1, 2, 3, 4}) {
        P2Radio radio(static_cast<std::uint8_t>(b));
        CHECK(radio.start());
        Hpsdr2Source s;
        CHECK(s.open(radio.args("timeout=500")));
        s.closeDevice();
    }
    P2Radio radio;
    CHECK(radio.start());
    radio.busy = true;
    {
        Hpsdr2Source s;
        CHECK(!s.open(radio.args("timeout=500")) && std::string(s.lastError()).find("another computer") != std::string::npos);
        CHECK(s.open(radio.args("timeout=500,takeover=1")));
        s.closeDevice();
    }
    radio.busy = false;
    radio.formatModes = 0x02;  // little-endian only: not what this build reads
    {
        Hpsdr2Source s;
        CHECK(!s.open(radio.args("timeout=500")) && std::string(s.lastError()).find("3-byte") != std::string::npos);
    }
    radio.formatModes = 0x05;  // big-endian and 3-byte offered
    {
        Hpsdr2Source s;
        CHECK(s.open(radio.args("timeout=500")));
        s.closeDevice();
    }
    radio.formatModes = 0;
    {
        Hpsdr2Source s;
        CHECK(!s.open(radio.args("timeout=500,mac=de:ad:be:ef:00:01")) && std::string(s.lastError()).find("MAC") != std::string::npos);
        CHECK(s.open(radio.args("timeout=500,mac=00:1C:C0:A2:13:88")));
        s.closeDevice();
    }
    {  // port arguments: out of range and clashing ports are refused before anything is sent
        Hpsdr2Source s;
        CHECK(!s.open("hpsdr2=127.0.0.1:" + std::to_string(radio.mainPort()) + ",ddc0port=0") );
        CHECK(!s.open("hpsdr2=127.0.0.1:" + std::to_string(radio.mainPort()) + ",ddc0port=70000"));
        CHECK(!s.open("hpsdr2=127.0.0.1:" + std::to_string(radio.mainPort()) + ",ddc0port=5000,statusport=5000"));
        CHECK(!s.open("hpsdr2=127.0.0.1:" + std::to_string(radio.mainPort()) + ",ddc0port=1024"));
        CHECK(std::string(s.lastError()).find("differ") != std::string::npos);
        CHECK(!s.open("hpsdr2=127.0.0.1:" + std::to_string(radio.mainPort()) + ",timeout=5"));
        CHECK(!s.open("hpsdr2=[::1"));
    }
    const int disc = radio.discoveries.load();
    Hpsdr2Source n;
    CHECK(!n.open("hpsdr2=127.0.0.1:" + std::to_string(radio.mainPort()) + ",ddc0port=bad"));
    CHECK(radio.discoveries.load() == disc);  // refused before sending discovery
    std::vector<Hpsdr2Device> found;
    std::string err;
    CHECK(cascade::source::discoverHpsdr2("127.0.0.1", radio.mainPort(), std::chrono::milliseconds(500), found, err));
    CHECK(found.size() == 1 && found[0].reply.boardType == 1 && found[0].name().find("Hermes") == 0);
    CHECK(!cascade::source::discoverHpsdr2("no-such-host.invalid", 1024, std::chrono::milliseconds(100), found, err) && !err.empty());
}

void testRefusedSettings() {
    P2Radio radio;
    CHECK(radio.start());
    Hpsdr2Source s;
    CHECK(s.open(radio.args("timeout=3000")));
    CHECK(s.start());
    CHECK(waitFor([&] { return radio.ksps.load() == 48 && radio.atten.load() == 0; }));
    for (const double bad : {44100.0, 0.0, -48000.0, 48000.5, 3072000.0, 1.0e12, std::nan(""), std::numeric_limits<double>::infinity()}) {
        CHECK(!s.setSampleRateHz(bad) && std::string(s.lastError()).find("1536000") != std::string::npos);
    }
    for (const double bad : {-1.0, 122880000.0, 200e6, 5.0e9, std::nan(""), std::numeric_limits<double>::infinity()}) {
        CHECK(!s.setCenterFrequencyHz(bad) && std::string(s.lastError()).find("DSP clock") != std::string::npos);
    }
    CHECK(!s.setGainDb("Attenuator", 32.0) && !s.setGainDb("Attenuator", -1.0) && !s.setGainDb("Attenuator", std::nan("")) && !s.setGainDb("Gain", 3.0));
    CHECK(s.sampleRateHz() == 48000.0 && s.centerFrequencyHz() == 0.0 && s.gainDb("Attenuator") == 0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(radio.ksps.load() == 48 && radio.word.load() == 0 && radio.atten.load() == 0);  // nothing changed on the radio
    // the limits themselves
    CHECK(s.setCenterFrequencyHz(61440000.0));
    CHECK(waitFor([&] { return radio.word.load() == 0x80000000u; }));
    CHECK(s.setSampleRateHz(1536000.0));
    CHECK(waitFor([&] { return radio.ksps.load() == 1536; }));
    CHECK(s.setGainDb("Attenuator", 31.0));
    CHECK(waitFor([&] { return radio.atten.load() == 31; }));
    s.closeDevice();
    Hpsdr2Source u;  // before open: validated and kept; start refuses
    CHECK(!u.setSampleRateHz(1000.0) && u.setSampleRateHz(768000.0) && u.sampleRateHz() == 768000.0);
    CHECK(u.setCenterFrequencyHz(14.2e6) && u.centerFrequencyHz() == 14200000.0 && !u.setCenterFrequencyHz(-5.0));
    CHECK(!u.start() && u.gains().empty());
}

void testStreamFaults() {
    {  // a lost packet is counted, never fatal
        P2Radio radio;
        CHECK(radio.start());
        radio.dropAt = 25;
        Hpsdr2Source s;
        CHECK(s.open(radio.args("timeout=3000")));
        CHECK(s.start());
        const auto w = readSamples(s, 60 * 238, 10000);
        CHECK(w.size() >= 60 * 238);
        CHECK(waitFor([&] { return s.packetsLost() == 1; }));
        CHECK(!s.faulted());
        s.closeDevice();
    }
    {  // wrong length and wrong sample size: refused, counted, the stream carries on
        P2Radio radio;
        CHECK(radio.start());
        radio.shortAt = 20;
        radio.sixteenBitAt = 30;
        Hpsdr2Source s;
        CHECK(s.open(radio.args("timeout=3000")));
        CHECK(s.start());
        CHECK(waitFor([&] { return radio.sentPackets.load() >= 80; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(s.badPackets() == 2 && !s.faulted());
        CHECK(s.packetsLost() == 2);  // a refused packet never reaches the sequence tracker: its number is a gap
        s.closeDevice();
    }
    {  // a radio that goes silent while streaming is a fault, bounded
        P2Radio radio;
        CHECK(radio.start());
        radio.stallAfter = 15;
        Hpsdr2Source s;
        CHECK(s.open(radio.args("timeout=1000")));
        CHECK(s.start());
        std::vector<Cx> tmp(512);
        const auto t0 = std::chrono::steady_clock::now();
        while (!s.faulted() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10)) { s.read(tmp.data(), tmp.size()); }
        CHECK(s.faulted() && std::string(s.lastError()).find("stopped sending") != std::string::npos);
        s.closeDevice();
    }
}

void testCyclesAndClose() {
    P2Radio radio;
    CHECK(radio.start());
    const unsigned long long before = NetSampleSource::readersAbandoned();
    Hpsdr2Source s;
    CHECK(s.open(radio.args("timeout=3000")));
    for (int i = 0; i < 20; ++i) {
        CHECK(s.start());
        std::vector<Cx> tmp(64);
        s.read(tmp.data(), tmp.size());
        s.stop();
    }
    CHECK(waitFor([&] { return radio.stops.load() == 20; }, 60000));  // the stand-in may trail the PC on a busy machine
    CHECK(radio.seqErrors.load() == 0 && radio.pttSeen.load() == 0 && radio.unexpectedBytes.load() == 0);
    CHECK(s.start());
    CHECK(readSamples(s, 2000).size() >= 2000);
    const auto t0 = std::chrono::steady_clock::now();
    s.closeDevice();
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < 2500);
    {
        Hpsdr2Source u;
        CHECK(u.open(radio.args("timeout=3000")));
        CHECK(u.start());
    }
    CHECK(NetSampleSource::readersAbandoned() == before);
    Hpsdr2Source c;
    CHECK(!c.start());
    c.closeDevice();
    c.stop();
}

// ---- C: fuzz --------------------------------------------------------------------------------------------

void testFuzz() {
    Rng rng(0x2BADu);
    int ddcOk = 0, discOk = 0, statusOk = 0;
    for (int i = 0; i < 30000; ++i) {
        Bytes in(rng.next() % 4 == 0 ? rng.next() % 40 : (rng.next() % 2 ? 1444 : rng.next() % 1500));
        for (auto& b : in) { b = static_cast<std::uint8_t>(rng.next()); }
        if (in.size() >= 16 && rng.next() % 3 != 0) {
            in[12] = 0; in[13] = 24; in[14] = 0; in[15] = static_cast<std::uint8_t>(1 + rng.next() % 238);
            if (rng.next() % 2) { in.resize(16 + 6 * static_cast<std::size_t>(in[15])); }
        }
        h2::DdcPacket p;
        const h2::DdcVerdict v = h2::parseDdcPacket(in.data(), in.size(), p);
        if (v == h2::DdcVerdict::Ok) {
            ++ddcOk;
            CHECK(in.size() == 16u + 6u * p.samples && p.samples >= 1 && p.samples <= 238 && p.data == in.data() + 16);
            std::vector<Cx> o;
            h2::decodeDdcSamples(p, o);
            bool bounded = o.size() == p.samples;
            for (const Cx& c : o) { bounded = bounded && std::fabs(c.real()) <= 1.0f && std::fabs(c.imag()) <= 1.0f; }
            CHECK(bounded);
        } else {
            CHECK(in.size() < 16 || in[13] != 24 || in[12] != 0 || (static_cast<unsigned>(in[14]) << 8 | in[15]) == 0 ||
                  (static_cast<unsigned>(in[14]) << 8 | in[15]) > 238 || in.size() != 16u + 6u * (static_cast<unsigned>(in[14]) << 8 | in[15]));
        }
        h2::DiscoveryReply r;
        if (in.size() >= 24 && rng.next() % 2) { in[0] = in[1] = in[2] = in[3] = 0; in[4] = static_cast<std::uint8_t>(2 + rng.next() % 2); }
        if (h2::parseDiscoveryReply(in.data(), in.size(), r)) { ++discOk; CHECK(in.size() >= 24 && (in[4] == 2 || in[4] == 3)); }
        h2::Status st;
        if (in.size() == 60 || (in.size() > 60 && rng.next() % 2)) { in.resize(60); }
        if (h2::parseStatus(in.data(), in.size(), st)) { ++statusOk; }
    }
    CHECK(ddcOk > 2000 && discOk > 1000 && statusOk > 100);
    // builders: random settings never produce a packet of the wrong size, and the transmit-side bytes stay 0
    for (int i = 0; i < 5000; ++i) {
        const std::uint32_t word = rng.next();
        const int att = static_cast<int>(rng.next() % 80) - 20;
        const auto hp = h2::buildHighPriority(rng.next(), rng.next() % 2 != 0, word, att);
        CHECK(hp.size() == 1444 && (hp[4] & 0xFE) == 0 && hp[345] == 0 && hp[1443] <= 31);
        bool rest = true;
        for (std::size_t k = 13; k < 1443; ++k) { rest = rest && hp[k] == 0; }
        CHECK(rest);
        const auto ds = h2::buildDdcSpecific(rng.next(), 1 + static_cast<int>(rng.next() % 8), h2::supportedRates()[rng.next() % 6]);
        CHECK(ds.size() == 1444 && ds[7] == 1);
    }
}

}  // namespace

int main() {
    std::fprintf(stderr, "testDiscoveryBytes\n");         testDiscoveryBytes();
    std::fprintf(stderr, "testGeneralBytes\n");           testGeneralBytes();
    std::fprintf(stderr, "testSpecificAndHighPriority\n"); testSpecificAndHighPriority();
    std::fprintf(stderr, "testFrequencyWord\n");          testFrequencyWord();
    std::fprintf(stderr, "testDdcPacket\n");              testDdcPacket();
    std::fprintf(stderr, "testStatusBytes\n");            testStatusBytes();
    std::fprintf(stderr, "testReceive\n");                testReceive();
    std::fprintf(stderr, "testStatusFlags\n");            testStatusFlags();
    std::fprintf(stderr, "testBoardsAndOpenRefusals\n");  testBoardsAndOpenRefusals();
    std::fprintf(stderr, "testRefusedSettings\n");        testRefusedSettings();
    std::fprintf(stderr, "testStreamFaults\n");           testStreamFaults();
    std::fprintf(stderr, "testCyclesAndClose\n");         testCyclesAndClose();
    std::fprintf(stderr, "testFuzz\n");                   testFuzz();
    return testSummary("test_hpsdr2");
}
