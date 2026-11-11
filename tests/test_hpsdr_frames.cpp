// test_hpsdr_frames.cpp - the openHPSDR protocol-1 frame layer and the receive path built on it: 24-bit I/Q
// unpacking proven bit-exact against hand-built frames, every receiver count, the C&C status bytes, the
// control frames FoxSDR sends (tuning, sample rate, attenuator), and the whole source against a STAND-IN
// radio on 127.0.0.1 UDP that generates I/Q with a frequency offset, noise and a sample-clock error.
//
// SOURCE OF TRUTH FOR THE LITERAL NUMBERS: "HPSDR - USB Data Protocol" (openHPSDR; revision list to
// 23 Jan 19), cited in src/source/hpsdr_frame.hpp, and "Metis - How it works" V1.33 for the envelope. The
// stand-in is written from the same reading with literal byte positions and its own parsing (it does not
// call the code under test's frame functions), so it cannot show that the reading was right: NOTHING HERE
// IS PROVEN ON A REAL HPSDR RADIO (none on the desk).
//
// Network: UDP to 127.0.0.1 only. Nothing is broadcast; no radio is opened.
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

#include "source/hpsdr_frame.hpp"
#include "source/hpsdr_protocol.hpp"
#include "source/hpsdr_source.hpp"
#include "src_test_net.hpp"
#include "test_check.hpp"

using namespace srctest;
using cascade::source::HpsdrDevice;
using cascade::source::HpsdrSource;
using cascade::source::NetSampleSource;
namespace hp = cascade::source::hpsdr;

namespace {

using Cx = std::complex<float>;

// ---- independent helpers (written from the document, not from the code under test) ----------------

void put24(std::uint8_t* p, std::int32_t v) {
    p[0] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>(v & 0xFF);
}

// A device-to-PC frame for n receivers, written group by group from the document's diagram:
// <sync x3><C0..C4> then per group n x (I2 I1 I0 Q2 Q1 Q0) and <M1><M0>, zero padding at the end.
// value(g, r, isQ) gives the 24-bit sample.
template <typename F>
Bytes makeFrame(int n, const std::uint8_t cc[5], F value) {
    Bytes f(512, 0);
    f[0] = f[1] = f[2] = 0x7F;
    for (int i = 0; i < 5; ++i) { f[3 + i] = cc[i]; }
    const int groupBytes = 6 * n + 2;
    const int groups = 504 / groupBytes;  // the document's padding table is 504 mod (6n+2)
    for (int g = 0; g < groups; ++g) {
        std::uint8_t* p = f.data() + 8 + g * groupBytes;
        for (int r = 0; r < n; ++r) {
            put24(p + 6 * r, value(g, r, false));
            put24(p + 6 * r + 3, value(g, r, true));
        }
        p[6 * n] = 0x55;      // microphone sample: must never reach the I/Q output
        p[6 * n + 1] = 0xAA;
    }
    return f;
}

constexpr float kScale = 1.0f / 8388608.0f;

// ---- A: bit-exact unpacking ---------------------------------------------------------------------------

void testHandBuiltFrame() {
    Bytes f(512, 0);
    f[0] = f[1] = f[2] = 0x7F;
    const std::uint8_t cc[5] = {0x00, 0x01, 0x00, 0x00, 0x2A};
    for (int i = 0; i < 5; ++i) { f[3 + i] = cc[i]; }
    // group 0: I = 0x123456, Q = 0xFEDCBA (negative), mic 00 11
    const std::uint8_t g0[8] = {0x12, 0x34, 0x56, 0xFE, 0xDC, 0xBA, 0x00, 0x11};
    // group 1: I = 0x800000 (most negative), Q = 0x7FFFFF (most positive), mic 22 33
    const std::uint8_t g1[8] = {0x80, 0x00, 0x00, 0x7F, 0xFF, 0xFF, 0x22, 0x33};
    // group 2: I = 0xFFFFFF (-1), Q = 0x000001, mic 44 55
    const std::uint8_t g2[8] = {0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x01, 0x44, 0x55};
    for (int i = 0; i < 8; ++i) { f[8 + i] = g0[i]; f[16 + i] = g1[i]; f[24 + i] = g2[i]; }
    std::vector<Cx> out;
    hp::FrameStatus st;
    CHECK(hp::decodeFrame(f.data(), f.size(), 1, 1, out, st) == hp::FrameVerdict::Ok);
    CHECK(out.size() == 63);
    CHECK(out[0].real() == 1193046.0f * kScale);          // 0x123456
    CHECK(out[0].imag() == -74566.0f * kScale);            // 0xFEDCBA - 2^24
    CHECK(out[1].real() == -1.0f);                          // 0x800000 is exactly -1.0
    CHECK(out[1].imag() == 8388607.0f * kScale);            // 0x7FFFFF, just under +1.0
    CHECK(out[2].real() == -1.0f * kScale);                 // 0xFFFFFF
    CHECK(out[2].imag() == 1.0f * kScale);
    for (std::size_t i = 3; i < out.size(); ++i) { CHECK(out[i] == Cx(0.0f, 0.0f)); }
    CHECK(st.cc[0] == 0x00 && st.cc[1] == 0x01 && st.cc[4] == 0x2A);
    CHECK(st.address == 0 && st.hasSoftwareSerial && st.softwareSerial == 0x2A);
    CHECK(st.hasOverflow && st.adcOverflow[0] && !st.adcOverflow[1]);
    // the microphone bytes (00 11, 22 33, 44 55) appear nowhere in the output: change them, nothing moves
    Bytes g = f;
    for (int i = 6; i < 8; ++i) { g[8 + i] = 0xEE; g[16 + i] = 0xEE; g[24 + i] = 0xEE; }
    std::vector<Cx> out2;
    CHECK(hp::decodeFrame(g.data(), g.size(), 1, 1, out2, st) == hp::FrameVerdict::Ok && out2 == out);
    // padding bytes of a one-receiver frame: there are none (63 * 8 = 504)
    CHECK(hp::groupsPerFrame(1) * (6 + 2) == 504 && hp::paddingBytes(1) == 0);
}

void testReceiverCounts() {
    // The document's table (padding 0, 10, 24, 10, 20, 4 for 1, 4, 5, 6, 7, 8 receivers) and its
    // three-receiver example (four zero bytes at the end); 2 receivers is the same arithmetic (504 = 36 * 14).
    const int wantGroups[9] = {0, 63, 36, 25, 19, 15, 13, 11, 10};
    const int wantPad[9] = {0, 0, 0, 4, 10, 24, 10, 20, 4};
    for (int n = 1; n <= 8; ++n) {
        CHECK(hp::groupsPerFrame(n) == wantGroups[n]);
        CHECK(hp::paddingBytes(n) == wantPad[n]);
        CHECK(8 + wantGroups[n] * (6 * n + 2) + wantPad[n] == 512);
    }
    CHECK(hp::groupsPerFrame(0) == 0 && hp::groupsPerFrame(9) == 0 && hp::groupsPerFrame(-1) == 0);
    CHECK(hp::paddingBytes(0) == 0 && hp::paddingBytes(9) == 0);
    for (int n = 1; n <= 8; ++n) {
        const std::uint8_t cc[5] = {0x20, 0, 0, 0, 0};
        auto value = [](int g, int r, bool q) -> std::int32_t {
            const int m = (g * 977 + r * 61 + (q ? 13 : 1)) % 0x7FFFFF;
            return ((g + r) % 2 == 0) ? m : -m;
        };
        const Bytes f = makeFrame(n, cc, value);
        for (int r = 1; r <= n; ++r) {
            std::vector<Cx> out;
            hp::FrameStatus st;
            CHECK(hp::decodeFrame(f.data(), 512, n, r, out, st) == hp::FrameVerdict::Ok);
            CHECK(static_cast<int>(out.size()) == wantGroups[n]);
            bool ok = static_cast<int>(out.size()) == wantGroups[n];
            for (int g = 0; ok && g < wantGroups[n]; ++g) {
                ok = out[static_cast<std::size_t>(g)].real() == static_cast<float>(value(g, r - 1, false)) * kScale &&
                     out[static_cast<std::size_t>(g)].imag() == static_cast<float>(value(g, r - 1, true)) * kScale;
            }
            CHECK(ok);
        }
    }
    {  // the document's own 3-receiver example, positions as printed: bytes 8..27, 28..47, ..., 492..507, pad 508..511
        Bytes f(512, 0);
        f[0] = f[1] = f[2] = 0x7F;
        // I12 I11 I10 Q12 Q11 Q10 | I22.. Q20 | I32.. Q30 | M1 M0 -> mark receiver 2's I of the first group and last group
        f[8 + 6] = 0x01; f[8 + 7] = 0x02; f[8 + 8] = 0x03;           // receiver 2 I of group 0 (bytes 14..16)
        // group k starts at byte 8 + 20k; group 24 is the 25th and last (it ends at byte 507)
        f[8 + 24 * 20 + 6] = 0x0A; f[8 + 24 * 20 + 7] = 0x0B; f[8 + 24 * 20 + 8] = 0x0C;  // receiver 2 I of group 24 (bytes 494..496)
        std::vector<Cx> out;
        hp::FrameStatus st;
        CHECK(hp::decodeFrame(f.data(), 512, 3, 2, out, st) == hp::FrameVerdict::Ok && out.size() == 25);
        CHECK(out[0].real() == static_cast<float>(0x010203) * kScale);
        CHECK(out[24].real() == static_cast<float>(0x0A0B0C) * kScale);
        CHECK(8 + 25 * 20 == 508);  // the group ends at 507; bytes 508..511 are padding
    }
}

void testRefusals() {
    const std::uint8_t cc[5] = {0, 0, 0, 0, 0};
    auto zero = [](int, int, bool) -> std::int32_t { return 0; };
    const Bytes good = makeFrame(1, cc, zero);
    std::vector<Cx> out;
    hp::FrameStatus st;
    CHECK(hp::decodeFrame(good.data(), 512, 1, 1, out, st) == hp::FrameVerdict::Ok);
    out.clear();
    // short and long frames, null
    CHECK(hp::decodeFrame(good.data(), 511, 1, 1, out, st) == hp::FrameVerdict::WrongLength);
    CHECK(hp::decodeFrame(good.data(), 513, 1, 1, out, st) == hp::FrameVerdict::WrongLength);
    CHECK(hp::decodeFrame(good.data(), 0, 1, 1, out, st) == hp::FrameVerdict::WrongLength);
    CHECK(hp::decodeFrame(nullptr, 512, 1, 1, out, st) == hp::FrameVerdict::WrongLength);
    // sync: each of the three bytes
    for (int i = 0; i < 3; ++i) {
        Bytes b = good;
        b[static_cast<std::size_t>(i)] = 0x7E;
        CHECK(hp::decodeFrame(b.data(), 512, 1, 1, out, st) == hp::FrameVerdict::BadSync);
    }
    // receiver count and selection
    CHECK(hp::decodeFrame(good.data(), 512, 0, 1, out, st) == hp::FrameVerdict::BadReceiverCount);
    CHECK(hp::decodeFrame(good.data(), 512, 9, 1, out, st) == hp::FrameVerdict::BadReceiverCount);
    CHECK(hp::decodeFrame(good.data(), 512, 2, 0, out, st) == hp::FrameVerdict::BadSelection);
    CHECK(hp::decodeFrame(good.data(), 512, 2, 3, out, st) == hp::FrameVerdict::BadSelection);
    CHECK(out.empty());  // nothing appended by any refusal
    CHECK(std::string(hp::verdictName(hp::FrameVerdict::BadSync)).find("sync") != std::string::npos);

    // a packet is all-or-nothing: second frame bad -> the first frame's samples are not kept either
    hp::Protocol1Decoder d;
    CHECK(d.configure(1, 1, 48000));
    Bytes pk(1024, 0);
    for (int i = 0; i < 2; ++i) { std::copy(good.begin(), good.end(), pk.begin() + 512 * i); }
    std::vector<Cx> o;
    CHECK(d.decodePacket(pk.data(), o) && o.size() == 126 && d.tally().frames == 2 && d.tally().samples == 126);
    pk[512] = 0x00;  // break the second frame's sync
    std::vector<Cx> o2;
    CHECK(!d.decodePacket(pk.data(), o2) && o2.empty());
    CHECK(d.lastVerdict() == hp::FrameVerdict::BadSync && d.tally().refused == 1 && d.tally().badSync == 1);
    CHECK(d.tally().frames == 2 && d.tally().samples == 126);  // the refused packet changed no sample count
    pk[512] = 0x7F; pk[0] = 0x00;                                // first frame bad
    CHECK(!d.decodePacket(pk.data(), o2) && o2.empty() && d.tally().badSync == 2);
    // out vector with earlier content keeps it on a refusal
    std::vector<Cx> keep(3, Cx(1.0f, 2.0f));
    CHECK(!d.decodePacket(pk.data(), keep) && keep.size() == 3);
    // configure refusals leave the old setting
    CHECK(!d.configure(0, 1, 48000) && !d.configure(9, 1, 48000) && !d.configure(2, 3, 48000) && !d.configure(1, 1, 44100));
    CHECK(d.receivers() == 1 && d.deliver() == 1 && d.rateHz() == 48000);
    CHECK(d.configure(8, 8, 384000) && d.receivers() == 8 && d.rateHz() == 384000);
    d.resetTally();
    CHECK(d.tally().frames == 0 && d.tally().refused == 0);
}

void testStatusBytes() {
    auto zero = [](int, int, bool) -> std::int32_t { return 0; };
    std::vector<Cx> out;
    hp::FrameStatus st;
    {  // address 0: PTT / DASH / DOT are C0 bits 0, 1, 2; software serial in C4
        const std::uint8_t cc[5] = {0x05, 0x00, 0x00, 0x00, 0xC3};  // address 0, PTT + DOT
        const Bytes f = makeFrame(1, cc, zero);
        CHECK(hp::decodeFrame(f.data(), 512, 1, 1, out, st) == hp::FrameVerdict::Ok);
        CHECK(st.address == 0 && st.ptt && !st.dash && st.dot && st.softwareSerial == 0xC3 && !st.adcOverflow[0]);
    }
    {  // address 4 (C0 = 00100xxx): per-ADC overflow in bit 0 of C1..C4, Mercury version in bits 7..1
        const std::uint8_t cc[5] = {0x20, static_cast<std::uint8_t>((5 << 1) | 1), static_cast<std::uint8_t>(6 << 1),
                                    static_cast<std::uint8_t>((127 << 1) | 1), 0x00};
        const Bytes f = makeFrame(1, cc, zero);
        CHECK(hp::decodeFrame(f.data(), 512, 1, 1, out, st) == hp::FrameVerdict::Ok);
        CHECK(st.address == 4 && st.hasOverflow && !st.hasSoftwareSerial);
        CHECK(st.adcOverflow[0] && !st.adcOverflow[1] && st.adcOverflow[2] && !st.adcOverflow[3]);
        CHECK(st.mercuryVersion[0] == 5 && st.mercuryVersion[1] == 6 && st.mercuryVersion[2] == 127 && st.mercuryVersion[3] == 0);
    }
    for (const int addr : {1, 2, 3, 5, 31}) {  // analog readings and unknown addresses: nothing decoded
        const std::uint8_t cc[5] = {static_cast<std::uint8_t>(addr << 3), 0xFF, 0xFF, 0xFF, 0xFF};
        const Bytes f = makeFrame(1, cc, zero);
        CHECK(hp::decodeFrame(f.data(), 512, 1, 1, out, st) == hp::FrameVerdict::Ok);
        CHECK(st.address == addr && !st.hasOverflow && !st.hasSoftwareSerial);
    }
}

// ---- B: the control frames ----------------------------------------------------------------------------

void testControlBytes() {
    hp::ControlSettings s;
    s.rateHz = 192000;
    s.receivers = 1;
    s.rxFreqHz[0] = 7074000;  // 0x006BF0D0
    s.attenuatorEnable = true;
    s.attenuatorDb = 12;
    CHECK(hp::validateSettings(s).empty());
    const auto cyc = hp::controlCycle(s);
    CHECK(cyc.size() == 3);
    // address 0: C1 = speed 10b (192 kHz), clock bits 0; C4 = number of receivers - 1 = 0 in bits 5..3
    CHECK(cyc[0][0] == 0x00 && cyc[0][1] == 0x02 && cyc[0][2] == 0x00 && cyc[0][3] == 0x00 && cyc[0][4] == 0x00);
    // receiver 1 NCO: C0 = 0000010x with MOX 0 = 0x04, frequency big-endian in C1..C4
    CHECK(cyc[1][0] == 0x04 && cyc[1][1] == 0x00 && cyc[1][2] == 0x6B && cyc[1][3] == 0xF0 && cyc[1][4] == 0xD0);
    // address 0x0A (C0 = 0001010x = 0x14): C4 bit 5 attenuator enable, bits 4..0 = 12
    CHECK(cyc[2][0] == 0x14 && cyc[2][1] == 0x00 && cyc[2][4] == (0x20 | 12));
    // each rate's speed code: 48/96/192/384 = 00/01/10/11
    const int rates[4] = {48000, 96000, 192000, 384000};
    for (int i = 0; i < 4; ++i) {
        hp::ControlSettings r;
        r.rateHz = rates[i];
        CHECK(hp::controlCycle(r)[0][1] == i);
        std::uint8_t code = 99;
        int hz = 0;
        CHECK(hp::rateToSpeedCode(rates[i], code) && code == i && hp::speedCodeToRate(code, hz) && hz == rates[i]);
    }
    {
        int unused = 0;
        CHECK(!hp::speedCodeToRate(4, unused) && !hp::speedCodeToRate(255, unused));
    }
    // receivers 1..7: NCO addresses 0x04, 0x06, ... 0x10, and the receiver count field
    for (int n = 1; n <= 7; ++n) {
        hp::ControlSettings r;
        r.receivers = n;
        for (int k = 0; k < 7; ++k) { r.rxFreqHz[static_cast<std::size_t>(k)] = 1000000u * static_cast<std::uint32_t>(k + 1); }
        CHECK(hp::validateSettings(r).empty());
        const auto c = hp::controlCycle(r);
        CHECK(static_cast<int>(c.size()) == n + 2);
        CHECK(c[0][4] == ((n - 1) << 3));
        for (int k = 1; k <= n; ++k) {
            const int expectC0 = 0x04 + 2 * (k - 1);
            const std::uint32_t want = 1000000u * static_cast<std::uint32_t>(k);
            const std::uint32_t got = (static_cast<std::uint32_t>(c[static_cast<std::size_t>(k)][1]) << 24) |
                                      (static_cast<std::uint32_t>(c[static_cast<std::size_t>(k)][2]) << 16) |
                                      (static_cast<std::uint32_t>(c[static_cast<std::size_t>(k)][3]) << 8) | c[static_cast<std::size_t>(k)][4];
            CHECK(c[static_cast<std::size_t>(k)][0] == expectC0 && got == want);
        }
    }
    {  // the extremes of the 32-bit frequency
        hp::ControlSettings r;
        r.rxFreqHz[0] = 0xFFFFFFFFu;
        auto c = hp::controlCycle(r);
        CHECK(c[1][1] == 0xFF && c[1][2] == 0xFF && c[1][3] == 0xFF && c[1][4] == 0xFF);
        r.rxFreqHz[0] = 0;
        c = hp::controlCycle(r);
        CHECK(c[1][1] == 0 && c[1][2] == 0 && c[1][3] == 0 && c[1][4] == 0);
    }
    {  // hardware-configuration bits and preamp, positions as the diagram prints them
        hp::ControlSettings r;
        r.tenMhzRef = 2;  // Mercury: C1 bits 3..2 = 10
        r.clock122 = 1;   // Mercury: bit 4
        r.config = 3;     // both: bits 6..5
        r.preamp = true;
        const auto c = hp::controlCycle(r);
        CHECK(c[0][1] == (0 | (2 << 2) | (1 << 4) | (3 << 5)));
        CHECK(c[0][3] == 0x04);  // C3 bit 2 "Preamp On/Off"
        CHECK(c.back()[0] == 0x14 && c.back()[1] == 0x01);  // Rx1 pre-amp, C1 bit 0
    }
    {  // attenuator disabled: C4 carries no enable bit even with a stored value
        hp::ControlSettings r;
        r.attenuatorEnable = false;
        r.attenuatorDb = 20;
        CHECK((hp::controlCycle(r).back()[4] & 0x20) == 0);
    }
    // a control frame: sync, C&C, then 504 zero bytes (silent audio, no I/Q) - and MOX never set
    const hp::Cc cc = {0x04, 1, 2, 3, 4};
    const auto fr = hp::buildControlFrame(cc);
    CHECK(fr[0] == 0x7F && fr[1] == 0x7F && fr[2] == 0x7F && fr[3] == 0x04 && fr[4] == 1 && fr[7] == 4);
    CHECK(std::all_of(fr.begin() + 8, fr.end(), [](std::uint8_t b) { return b == 0; }));
}

void testValidation() {
    hp::ControlSettings s;
    CHECK(hp::validateSettings(s).empty());
    for (const int bad : {0, -48000, 44100, 48001, 96001, 768000, 1}) {
        s.rateHz = bad;
        CHECK(!hp::validateSettings(s).empty());
    }
    s.rateHz = 48000;
    for (const int bad : {0, -1, 8, 9, 100}) {
        s.receivers = bad;
        CHECK(!hp::validateSettings(s).empty());
    }
    s.receivers = 8;
    CHECK(hp::validateSettings(s).find("receiver 8") != std::string::npos);  // unpackable, not tunable: says why
    s.receivers = 1;
    s.attenuatorDb = 32;
    CHECK(!hp::validateSettings(s).empty());
    s.attenuatorDb = -1;
    CHECK(!hp::validateSettings(s).empty());
    s.attenuatorDb = 31;
    CHECK(hp::validateSettings(s).empty());
    s.tenMhzRef = 3;
    CHECK(!hp::validateSettings(s).empty());
    s.tenMhzRef = 0;
    s.clock122 = 2;
    CHECK(!hp::validateSettings(s).empty());
    s.clock122 = 0;
    s.config = 4;
    CHECK(!hp::validateSettings(s).empty());
    s.config = 0;
    // a refused set leaves the sender unchanged
    hp::ControlSender snd;
    std::string why;
    hp::ControlSettings ok;
    ok.rateHz = 96000;
    CHECK(snd.set(ok, why) && why.empty() && snd.settings().rateHz == 96000);
    hp::ControlSettings bad = ok;
    bad.rateHz = 12345;
    CHECK(!snd.set(bad, why) && !why.empty() && snd.settings().rateHz == 96000);
}

void testSender() {
    hp::ControlSender snd;
    std::string why;
    hp::ControlSettings s;
    s.receivers = 2;
    s.rxFreqHz[0] = 10000000;
    s.rxFreqHz[1] = 14000000;
    CHECK(snd.set(s, why));
    // cycle = address 0, rx1, rx2, address 0x0A = 4 quintuples = 2 packets of two frames
    CHECK(snd.packetsPerCycle() == 2);
    std::vector<Bytes> pk;
    for (int i = 0; i < 4; ++i) { pk.push_back(snd.nextPacket()); }
    for (std::size_t i = 0; i < pk.size(); ++i) {
        hp::DataPacket dp;
        CHECK(pk[i].size() == 1032 && hp::parseDataPacket(pk[i].data(), pk[i].size(), dp));
        CHECK(dp.endpoint == 0x02 && dp.sequence == i);  // end point 2, sequence from zero
        CHECK(dp.frames[0] == 0x7F && dp.frames[1] == 0x7F && dp.frames[2] == 0x7F);
        CHECK(dp.frames[512] == 0x7F && dp.frames[513] == 0x7F && dp.frames[514] == 0x7F);
    }
    // packet 0 carries C0 of address 0 then rx1; packet 1 rx2 then 0x0A; packet 2 wraps to address 0
    CHECK(pk[0][8 + 3] == 0x00 && pk[0][8 + 512 + 3] == 0x04);
    CHECK(pk[1][8 + 3] == 0x06 && pk[1][8 + 512 + 3] == 0x14);
    CHECK(pk[2][8 + 3] == 0x00 && pk[2][8 + 512 + 3] == 0x04);
    CHECK(snd.sequence() == 4);
    snd.restart();
    const Bytes again = snd.nextPacket();
    hp::DataPacket dp;
    CHECK(hp::parseDataPacket(again.data(), again.size(), dp) && dp.sequence == 0 && again[8 + 3] == 0x00);
    // MOX (C0 bit 0) and every audio / I/Q byte stay zero in everything sent
    for (const Bytes& b : pk) {
        for (int fr = 0; fr < 2; ++fr) {
            CHECK((b[8 + 512 * fr + 3] & 0x01) == 0);
            bool zero = true;
            for (int i = 8; i < 512; ++i) { zero = zero && b[static_cast<std::size_t>(8 + 512 * fr + i)] == 0; }
            CHECK(zero);
        }
    }
    // an odd-length cycle (3 quintuples) pairs frames across packets without losing one
    hp::ControlSender s3;
    hp::ControlSettings one;
    CHECK(s3.set(one, why) && s3.packetsPerCycle() == 2);
    std::vector<std::uint8_t> c0s;
    for (int i = 0; i < 3; ++i) {
        const Bytes b = s3.nextPacket();
        c0s.push_back(b[8 + 3]);
        c0s.push_back(b[8 + 512 + 3]);
    }
    CHECK((c0s == std::vector<std::uint8_t>{0x00, 0x04, 0x14, 0x00, 0x04, 0x14}));
}

// ---- C: the stand-in radio -----------------------------------------------------------------------------

// Written from the documents with literal bytes. It answers discovery, obeys Start/Stop, parses the PC's
// end-point-2 frames on its own (C0 & 0xFE is the address with MOX masked) and streams generated I/Q.
class P1Radio {
public:
    explicit P1Radio(std::uint8_t board = 0x01) : board_(board) {}
    ~P1Radio() { stop(); }
    bool start() {
        if (!sock_.open()) { return false; }
        run_.store(true);
        th_ = std::thread([this] { loop(); });
        return true;
    }
    void stop() {
        run_.store(false);
        if (th_.joinable()) { th_.join(); }
        stopStream();
    }
    std::string args(const std::string& extra = "") const {
        return "hpsdr=127.0.0.1:" + std::to_string(sock_.port()) + (extra.empty() ? "" : "," + extra);
    }

    // generated signal
    std::atomic<double> rfHz{7074200.0};     // the carrier on the air
    std::atomic<double> rateErr{0.0};        // the radio's sample clock runs this fraction fast
    std::atomic<double> amplitude{0.5};      // of full scale
    std::atomic<double> noise{0.01};         // sigma per component, of full scale
    std::atomic<bool> overflow{false};
    // faults
    std::atomic<int> dropAt{-1};             // skip this packet number's sequence number (a gap on the wire)
    std::atomic<int> badSyncAt{-1};          // corrupt the second frame's sync of this packet
    std::atomic<int> shortAt{-1};            // send a 1031-byte packet in place of this one
    std::atomic<int> stallAfter{-1};         // go silent after this many packets
    // what the PC told it
    std::atomic<std::uint32_t> nco{0};
    std::atomic<int> speedCode{-1};
    std::atomic<int> receivers{0};
    std::atomic<int> attenEnable{-1};
    std::atomic<int> attenDb{-1};
    std::atomic<int> preamp{-1};
    std::atomic<int> starts{0}, stops{0};
    std::atomic<int> ep2Packets{0};
    std::atomic<int> ep2Malformed{0};        // wrong size / sync / end point
    std::atomic<int> ep2MoxSeen{0};          // any frame with C0 bit 0 set
    std::atomic<int> ep2AudioNonZero{0};     // any non-zero byte after the C&C
    std::atomic<int> ep2SeqErrors{0};        // sequence not +1 (outside the restart at Start)
    std::atomic<int> ep2FirstSeq{-1};        // first sequence number after the latest Start
    std::atomic<int> unknownAddress{0};      // C0 address that is none of 0x00, 0x04..0x10, 0x14
    std::atomic<int> sentPackets{0};

private:
    Bytes reply(std::uint8_t status) const {
        Bytes r(60, 0);
        r[0] = 0xEF; r[1] = 0xFE; r[2] = status;
        const std::uint8_t mac[6] = {0x00, 0x1c, 0xc0, 0xa2, 0x13, 0x77};
        for (int i = 0; i < 6; ++i) { r[3 + i] = mac[i]; }
        r[9] = 32;
        r[10] = board_;
        return r;
    }
    void loop() {
        Bytes d;
        std::uint16_t from = 0;
        while (run_.load()) {
            if (!sock_.recvFrom(run_, d, from)) { continue; }
            if (d.size() >= 3 && d[0] == 0xEF && d[1] == 0xFE && d[2] == 0x02 && d.size() == 63) {
                const Bytes r = reply(streaming_.load() ? 3 : 2);
                sock_.sendTo(from, r.data(), r.size());
            } else if (d.size() == 64 && d[0] == 0xEF && d[1] == 0xFE && d[2] == 0x04) {
                if (d[3] & 1) {
                    ++starts;
                    ep2Expect_.store(0); ep2First_.store(true); ep2FirstSeq = -1;
                    nco = 0; speedCode = -1; attenEnable = -1; attenDb = -1; preamp = -1;  // a radio that forgets at Stop
                    startStream(from);
                }
                else { ++stops; stopStream(); }
            } else if (d.size() == 1032 && d[0] == 0xEF && d[1] == 0xFE && d[2] == 0x01 && d[3] == 0x02) {
                onEp2(d);
            } else {
                ++ep2Malformed;
            }
        }
    }
    void onEp2(const Bytes& d) {
        ++ep2Packets;
        const std::uint32_t seq = get32(d.data() + 4);
        if (ep2First_.exchange(false)) { ep2FirstSeq = static_cast<int>(seq); }
        else if (seq != ep2Expect_.load()) { ++ep2SeqErrors; }
        ep2Expect_.store(seq + 1);
        for (int fr = 0; fr < 2; ++fr) {
            const std::uint8_t* f = d.data() + 8 + 512 * fr;
            if (f[0] != 0x7F || f[1] != 0x7F || f[2] != 0x7F) { ++ep2Malformed; continue; }
            if (f[3] & 1) { ++ep2MoxSeen; }
            for (int i = 8; i < 512; ++i) { if (f[i] != 0) { ++ep2AudioNonZero; break; } }
            const std::uint8_t addr = f[3] & 0xFE;  // MOX masked
            const std::uint32_t word = (static_cast<std::uint32_t>(f[4]) << 24) | (static_cast<std::uint32_t>(f[5]) << 16) |
                                       (static_cast<std::uint32_t>(f[6]) << 8) | f[7];
            if (addr == 0x00) {
                speedCode = f[4] & 3;
                receivers = ((f[7] >> 3) & 7) + 1;
                preamp = (f[6] >> 2) & 1;
            } else if (addr == 0x04) {
                nco = word;
            } else if (addr == 0x14) {
                attenEnable = (f[7] >> 5) & 1;
                attenDb = f[7] & 0x1F;
            } else if (addr >= 0x06 && addr <= 0x10) {
                // receivers 2..7 NCO: allowed, not interpreted here
            } else {
                ++unknownAddress;  // the transmitter NCO (0x02), drive level (0x12), CW and everything else
            }
        }
    }
    static std::uint32_t get32(const std::uint8_t* p) {
        return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
               (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
    }
    double rateHz() const {
        static const double r[4] = {48000.0, 96000.0, 192000.0, 384000.0};
        const int c = speedCode.load();
        return c >= 0 && c <= 3 ? r[c] : 48000.0;
    }
    void startStream(std::uint16_t to) {
        stopStream();
        streamRun_.store(true);
        streaming_.store(true);
        stream_ = std::thread([this, to] {
            Rng rng(0xC0FFEEu);
            auto gauss = [&rng]() {  // sum of 12 uniforms - 6: close enough to a unit normal
                double s = 0.0;
                for (int i = 0; i < 12; ++i) { s += (rng.next() & 0xFFFFFF) / 16777216.0; }
                return s - 6.0;
            };
            std::uint32_t seq = 0;
            double phase = 0.0;
            int count = 0;
            double refRate = rateHz();
            auto t0 = std::chrono::steady_clock::now();
            long long sentSinceRef = 0;
            int addrRot = 0;
            while (streamRun_.load()) {
                if (rateHz() != refRate) { refRate = rateHz(); t0 = std::chrono::steady_clock::now(); sentSinceRef = 0; }
                const double trueRate = refRate * (1.0 + rateErr.load());
                const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                const long long due = static_cast<long long>(elapsed * trueRate / 126.0) - sentSinceRef;
                if (due <= 0 || (stallAfter.load() >= 0 && count >= stallAfter.load())) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                const long long burst = std::min<long long>(due, 20);
                for (long long b = 0; b < burst && streamRun_.load(); ++b, ++sentSinceRef) {
                    Bytes p = {0xEF, 0xFE, 0x01, 0x06, static_cast<std::uint8_t>(seq >> 24), static_cast<std::uint8_t>(seq >> 16),
                               static_cast<std::uint8_t>(seq >> 8), static_cast<std::uint8_t>(seq)};
                    p.resize(1032, 0);
                    for (int fr = 0; fr < 2; ++fr) {
                        std::uint8_t* f = p.data() + 8 + 512 * fr;
                        f[0] = f[1] = f[2] = 0x7F;
                        const int addr = addrRot++ % 5;  // addresses 0..4 round robin, as the document describes
                        f[3] = static_cast<std::uint8_t>(addr << 3);
                        if (addr == 0) { f[4] = overflow.load() ? 1 : 0; f[7] = 33; }
                        if (addr == 4) { f[4] = static_cast<std::uint8_t>((3 << 1) | (overflow.load() ? 1 : 0)); f[5] = 5 << 1; }
                        for (int g = 0; g < 63; ++g) {
                            const double off = rfHz.load() - static_cast<double>(nco.load());
                            const double step = 2.0 * 3.14159265358979323846 * off / trueRate;
                            phase += step;
                            if (phase > 3.14159265358979323846) { phase -= 2.0 * 3.14159265358979323846; }
                            if (phase < -3.14159265358979323846) { phase += 2.0 * 3.14159265358979323846; }
                            const double a = amplitude.load() * 8388607.0;
                            const double ni = noise.load() * 8388607.0;
                            auto clip = [](double v) { return static_cast<std::int32_t>(std::max(-8388608.0, std::min(8388607.0, std::round(v)))); };
                            std::uint8_t* gp = f + 8 + 8 * g;
                            put24(gp, clip(a * std::cos(phase) + ni * gauss()));
                            put24(gp + 3, clip(a * std::sin(phase) + ni * gauss()));
                            gp[6] = static_cast<std::uint8_t>(rng.next());  // microphone: random, must be ignored
                            gp[7] = static_cast<std::uint8_t>(rng.next());
                        }
                    }
                    const bool dropThis = count == dropAt.load();
                    if (count == badSyncAt.load()) { p[8 + 512] = 0x00; }
                    if (count == shortAt.load()) {
                        sock_.sendTo(to, p.data(), p.size() - 1);
                    } else if (!dropThis) {
                        sock_.sendTo(to, p.data(), p.size());
                    }
                    ++seq;
                    ++count;
                    sentPackets.store(count);
                }
            }
        });
    }
    void stopStream() {
        streamRun_.store(false);
        std::lock_guard<std::mutex> l(m_);
        if (stream_.joinable()) { stream_.join(); }
        streaming_.store(false);
    }

    std::uint8_t board_;
    UdpSock sock_;
    std::atomic<bool> run_{false}, streamRun_{false}, streaming_{false};
    std::thread th_, stream_;
    std::mutex m_;
    std::atomic<std::uint32_t> ep2Expect_{0};
    std::atomic<bool> ep2First_{true};
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

std::vector<Cx> readSamples(HpsdrSource& s, std::size_t n, int ms = 8000) {
    std::vector<Cx> out;
    std::vector<Cx> tmp(2048);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (out.size() < n && std::chrono::steady_clock::now() < end) {
        const std::size_t g = s.read(tmp.data(), tmp.size());
        out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(g));
        if (s.faulted()) { break; }
    }
    return out;
}

// Mean phase advance per sample -> frequency, valid for a single strong tone with noise.
double toneHz(const std::vector<Cx>& v, std::size_t from, std::size_t n, double rate) {
    std::complex<double> acc(0.0, 0.0);
    for (std::size_t i = from; i + 1 < from + n && i + 1 < v.size(); ++i) {
        acc += std::complex<double>(v[i + 1]) * std::conj(std::complex<double>(v[i]));
    }
    return std::arg(acc) * rate / (2.0 * 3.14159265358979323846);
}

// Waits until the tone reads `want` +- tol on two windows in a row (the ring still holds older samples after
// a retune); returns the last estimate.
bool settlesAt(HpsdrSource& s, double rate, double want, double tol, int ms = 12000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    int good = 0;
    while (std::chrono::steady_clock::now() < end) {
        const auto w = readSamples(s, 4000, 4000);
        if (w.size() < 4000) { return false; }
        if (std::fabs(toneHz(w, 0, w.size(), rate) - want) <= tol) { if (++good >= 2) { return true; } } else { good = 0; }
    }
    return false;
}

void testStandInReceive() {
    P1Radio radio;
    CHECK(radio.start());
    radio.rateErr = 0.01;  // the radio's clock is 1% fast: the tone reads 1% low at the nominal rate
    HpsdrSource s;
    CHECK(s.open(radio.args("timeout=3000")));
    CHECK(s.isOpen() && s.device().reply.boardId == 1);
    CHECK(s.sampleRateHz() == 48000.0 && s.supportedSampleRatesHz().size() == 4);
    CHECK(s.setSampleRateHz(96000.0) && s.setCenterFrequencyHz(7069200.0));  // stored before start
    CHECK(s.sampleRateHz() == 96000.0 && s.centerFrequencyHz() == 7069200.0);
    CHECK(radio.starts.load() == 0 && s.controlPacketsSent() == 0);  // nothing is sent until Start
    CHECK(s.start() && s.running());
    CHECK(waitFor([&] { return radio.nco.load() == 7069200u && radio.speedCode.load() == 1; }));
    CHECK(radio.receivers.load() == 1);
    CHECK(radio.ep2FirstSeq.load() == 0);  // the PC's sequence restarts at zero at Start
    // offset = 7074200 - 7069200 = 5000 Hz; the radio's clock 1% fast: step uses trueRate, the reader assumes 96000
    const double wantOffset = 5000.0 / 1.01;
    CHECK(settlesAt(s, 96000.0, wantOffset, 6.0));
    const auto w = readSamples(s, 20000);
    CHECK(w.size() >= 20000);
    const double est = toneHz(w, 0, w.size(), 96000.0);
    CHECK(std::fabs(est - wantOffset) < 3.0);
    CHECK(std::fabs(est - 5000.0) > 30.0);  // and it is NOT the nominal 5000: the clock error is visible
    // amplitude: 0.5 of full scale, noise 0.01
    double mag = 0.0;
    for (const Cx& c : w) { mag += std::abs(c); }
    mag /= static_cast<double>(w.size());
    CHECK(mag > 0.45 && mag < 0.56);
    // retune while streaming: carrier moves to -8000 Hz relative to the NCO
    CHECK(s.setCenterFrequencyHz(7082200.0));
    CHECK(waitFor([&] { return radio.nco.load() == 7082200u; }));
    CHECK(settlesAt(s, 96000.0, -8000.0 / 1.01, 6.0));
    // change the rate while streaming: 192 kHz
    CHECK(s.setSampleRateHz(192000.0));
    CHECK(waitFor([&] { return radio.speedCode.load() == 2; }));
    CHECK(s.sampleRateHz() == 192000.0);
    CHECK(settlesAt(s, 192000.0, -8000.0 / 1.01, 8.0));
    // attenuator on a Hermes: offered, applied
    const auto gains = s.gains();
    CHECK(gains.size() == 1 && gains[0].name == "Attenuator" && gains[0].minDb == 0.0 && gains[0].maxDb == 31.0);
    CHECK(s.setGainDb("Attenuator", 12.0));
    CHECK(waitFor([&] { return radio.attenEnable.load() == 1 && radio.attenDb.load() == 12; }));
    CHECK(s.gainDb("Attenuator") == 12.0);
    CHECK(s.setGainDb("Attenuator", 0.0));
    CHECK(waitFor([&] { return radio.attenEnable.load() == 0 && radio.attenDb.load() == 0; }));
    // the periodic refresh keeps the settings going out
    const int before = radio.ep2Packets.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK(radio.ep2Packets.load() >= before + 3);
    // nothing the PC sent was a transmit: no MOX, no audio or I/Q, no unknown address, sequence in order
    CHECK(radio.ep2MoxSeen.load() == 0 && radio.ep2AudioNonZero.load() == 0 && radio.unknownAddress.load() == 0);
    CHECK(radio.ep2Malformed.load() == 0 && radio.ep2SeqErrors.load() == 0);
    CHECK(s.packetsLost() == 0 && s.badPackets() == 0 && !s.faulted());
    // frame tally: software serial 33 as the stand-in reports it; no overflow
    const hp::StreamTally t = s.frameTally();
    CHECK(t.frames > 100 && t.refused == 0 && t.hasSoftwareSerial && t.softwareSerial == 33);
    CHECK(t.adcOverflowFrames[0] == 0 && t.adcOverflowFrames[1] == 0);
    s.stop();
    CHECK(waitFor([&] { return radio.stops.load() == 1; }));
    const int atStop = radio.ep2Packets.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(radio.ep2Packets.load() <= atStop + 1);  // no refresh after Stop
    // a second and third Start: the sequence restarts at zero each time, settings come straight back
    for (int i = 0; i < 2; ++i) {
        radio.ep2FirstSeq = -1;
        CHECK(s.start());
        CHECK(waitFor([&] { return radio.ep2FirstSeq.load() == 0; }));
        CHECK(radio.nco.load() == 7082200u && radio.speedCode.load() == 2);
        s.stop();
    }
    CHECK(radio.ep2SeqErrors.load() == 0);
    s.closeDevice();
}

void testOverflowAndOtherBoard() {
    P1Radio radio(0x06);  // Hermes_Lite: no attenuator is named for it in the document
    CHECK(radio.start());
    radio.overflow = true;
    HpsdrSource s;
    CHECK(s.open(radio.args("timeout=3000")));
    CHECK(s.gains().empty());
    CHECK(!s.setGainDb("Attenuator", 6.0) && std::string(s.lastError()).find("no gain control") != std::string::npos);
    CHECK(s.start());
    const auto w = readSamples(s, 8000);
    CHECK(w.size() >= 8000);
    CHECK(waitFor([&] { return s.frameTally().adcOverflowFrames[0] > 3; }));
    CHECK(s.frameTally().adcOverflowFrames[1] == 0);
    s.closeDevice();
    P1Radio metis(0x00);
    CHECK(metis.start());
    HpsdrSource m;
    CHECK(m.open(metis.args("timeout=3000")) && m.gains().empty());
    m.closeDevice();
}

void testBoardArgument() {
    P1Radio radio(0x01);
    CHECK(radio.start());
    {
        HpsdrSource s;
        CHECK(!s.open(radio.args("timeout=500,board=angelia")) && !s.isOpen());  // a wrong board id is refused
        CHECK(std::string(s.lastError()).find("angelia") != std::string::npos &&
              std::string(s.lastError()).find("Hermes") != std::string::npos);
    }
    {
        HpsdrSource s;
        CHECK(!s.open(radio.args("timeout=500,board=toaster")) && std::string(s.lastError()).find("board=") != std::string::npos);
        CHECK(!s.open(radio.args("timeout=500,board=unknown board")));
    }
    HpsdrSource s;
    CHECK(s.open(radio.args("timeout=500,board=HERMES")));  // case-insensitive
    s.closeDevice();
}

void testRefusedSettings() {
    P1Radio radio;
    CHECK(radio.start());
    HpsdrSource s;
    CHECK(s.open(radio.args("timeout=3000")));
    CHECK(s.start());
    CHECK(waitFor([&] { return radio.speedCode.load() == 0; }));
    const std::uint64_t sent = s.controlPacketsSent();
    for (const double bad : {44100.0, 0.0, -48000.0, 48000.5, 1.0e12, std::nan(""), std::numeric_limits<double>::infinity()}) {
        CHECK(!s.setSampleRateHz(bad));
        CHECK(std::string(s.lastError()).find("48000") != std::string::npos);
    }
    for (const double bad : {-1.0, 4294967296.0, 5.0e9, std::nan(""), -std::numeric_limits<double>::infinity()}) {
        CHECK(!s.setCenterFrequencyHz(bad));
        CHECK(std::string(s.lastError()).find("4294967295") != std::string::npos);
    }
    CHECK(!s.setGainDb("Attenuator", 32.0) && !s.setGainDb("Attenuator", -1.0) && !s.setGainDb("Attenuator", std::nan("")));
    CHECK(!s.setGainDb("Gain", 5.0));
    CHECK(s.sampleRateHz() == 48000.0 && s.centerFrequencyHz() == 0.0);  // nothing changed
    // refusals sent nothing extra beyond the periodic refresh: no packet carried a changed value
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(radio.speedCode.load() == 0 && radio.nco.load() == 0 && radio.attenDb.load() == 0 && radio.attenEnable.load() == 0);
    CHECK(s.controlPacketsSent() >= sent);
    // the limits themselves are accepted
    CHECK(s.setCenterFrequencyHz(4294967295.0));
    CHECK(waitFor([&] { return radio.nco.load() == 4294967295u; }));
    CHECK(s.setCenterFrequencyHz(0.0) && s.setSampleRateHz(384000.0) && s.setGainDb("Attenuator", 31.0));
    CHECK(waitFor([&] { return radio.speedCode.load() == 3 && radio.attenDb.load() == 31; }));
    s.closeDevice();
    // setters before open: validated and kept
    HpsdrSource u;
    CHECK(!u.setSampleRateHz(1000.0) && u.setSampleRateHz(192000.0) && u.sampleRateHz() == 192000.0);
    CHECK(u.setCenterFrequencyHz(14.2e6) && u.centerFrequencyHz() == 14200000.0);
    CHECK(!u.start());
}

void testStreamFaults() {
    {  // a lost packet: counted, never fatal, and the stream carries on (the tone stays continuous but for one jump)
        P1Radio radio;
        CHECK(radio.start());
        radio.dropAt = 40;
        HpsdrSource s;
        CHECK(s.open(radio.args("timeout=3000")));
        CHECK(s.setCenterFrequencyHz(7069200.0) && s.setSampleRateHz(96000.0));
        CHECK(s.start());
        const auto w = readSamples(s, 90 * 126, 10000);
        CHECK(w.size() >= 90 * 126);
        CHECK(waitFor([&] { return s.packetsLost() == 1; }));
        CHECK(!s.faulted());
        s.closeDevice();
    }
    {  // a corrupted frame and a short packet: refused, counted, the rest still decoded; samples = frames * 63
        P1Radio radio;
        CHECK(radio.start());
        radio.badSyncAt = 30;
        radio.shortAt = 50;
        HpsdrSource s;
        CHECK(s.open(radio.args("timeout=3000")));
        CHECK(s.start());
        CHECK(waitFor([&] { return radio.sentPackets.load() >= 120; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(s.badPackets() == 2 && !s.faulted());  // the bad-sync packet (decoder) and the short one (envelope)
        const hp::StreamTally t = s.frameTally();
        CHECK(t.refused == 1 && t.badSync == 1);
        CHECK(s.packetsLost() == 1);                 // the short packet's sequence number never arrived
        CHECK(t.samples == t.frames * 63);
        CHECK(t.frames % 2 == 0);                    // whole packets only
        s.closeDevice();
    }
    {  // a radio that goes silent while streaming is a fault, bounded
        P1Radio radio;
        CHECK(radio.start());
        radio.stallAfter = 20;
        HpsdrSource s;
        CHECK(s.open(radio.args("timeout=400")));
        CHECK(s.start());
        std::vector<Cx> tmp(512);
        const auto t0 = std::chrono::steady_clock::now();
        while (!s.faulted() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10)) { s.read(tmp.data(), tmp.size()); }
        CHECK(s.faulted() && std::string(s.lastError()).find("stopped sending") != std::string::npos);
        s.closeDevice();
    }
}

void testCyclesAndClose() {
    P1Radio radio;
    CHECK(radio.start());
    const unsigned long long before = NetSampleSource::readersAbandoned();
    HpsdrSource s;
    CHECK(s.open(radio.args("timeout=3000")));
    for (int i = 0; i < 30; ++i) {
        CHECK(s.start());
        std::vector<Cx> tmp(64);
        s.read(tmp.data(), tmp.size());
        s.stop();
    }
    CHECK(radio.ep2SeqErrors.load() == 0 && radio.ep2MoxSeen.load() == 0 && radio.ep2AudioNonZero.load() == 0);
    CHECK(s.start());
    const auto w = readSamples(s, 2000);
    CHECK(w.size() >= 2000);
    const auto t0 = std::chrono::steady_clock::now();
    s.closeDevice();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 2000);
    {
        HpsdrSource u;  // destroyed while streaming
        CHECK(u.open(radio.args("timeout=3000")));
        CHECK(u.start());
    }
    CHECK(NetSampleSource::readersAbandoned() == before);
}

// ---- D: fuzz -------------------------------------------------------------------------------------------

void testFuzz() {
    Rng rng(0xBADC0DEu);
    int ok = 0, refusedSync = 0;
    for (int i = 0; i < 20000; ++i) {
        Bytes f(rng.next() % 3 == 0 ? rng.next() % 700 : 512);
        for (auto& b : f) { b = static_cast<std::uint8_t>(rng.next()); }
        if (f.size() >= 3 && rng.next() % 4 != 0) { f[0] = f[1] = f[2] = 0x7F; }
        const int n = static_cast<int>(rng.next() % 12) - 1;       // -1..10
        const int d = static_cast<int>(rng.next() % 12) - 1;
        std::vector<Cx> out(rng.next() % 3, Cx(9.0f, 9.0f));
        const std::size_t before = out.size();
        hp::FrameStatus st;
        const hp::FrameVerdict v = hp::decodeFrame(f.data(), f.size(), n, d, out, st);
        if (f.size() != 512) { CHECK(v == hp::FrameVerdict::WrongLength && out.size() == before); continue; }
        if (n < 1 || n > 8) { CHECK(v == hp::FrameVerdict::BadReceiverCount && out.size() == before); continue; }
        if (d < 1 || d > n) { CHECK(v == hp::FrameVerdict::BadSelection && out.size() == before); continue; }
        const bool syncOk = f[0] == 0x7F && f[1] == 0x7F && f[2] == 0x7F;
        if (!syncOk) { CHECK(v == hp::FrameVerdict::BadSync && out.size() == before); ++refusedSync; continue; }
        CHECK(v == hp::FrameVerdict::Ok);
        CHECK(static_cast<int>(out.size() - before) == 504 / (6 * n + 2));
        CHECK(st.address == (f[3] >> 3));
        bool bounded = true;
        for (std::size_t k = before; k < out.size(); ++k) { bounded = bounded && std::fabs(out[k].real()) <= 1.0f && std::fabs(out[k].imag()) <= 1.0f; }
        CHECK(bounded);
        ++ok;
    }
    CHECK(ok > 1000 && refusedSync > 200);
    // single-bit flips of a good frame: only a flip in the sync bytes is refused
    auto zero = [](int, int, bool) -> std::int32_t { return 0; };
    const std::uint8_t cc[5] = {0, 0, 0, 0, 0};
    const Bytes good = makeFrame(1, cc, zero);
    for (int bit = 0; bit < 512 * 8; ++bit) {
        Bytes b = good;
        b[static_cast<std::size_t>(bit / 8)] ^= static_cast<std::uint8_t>(1u << (bit % 8));
        std::vector<Cx> out;
        hp::FrameStatus st;
        const hp::FrameVerdict v = hp::decodeFrame(b.data(), b.size(), 1, 1, out, st);
        CHECK((bit < 24) == (v == hp::FrameVerdict::BadSync));
    }
    // random packets through the packet decoder
    hp::Protocol1Decoder dec;
    for (int i = 0; i < 5000; ++i) {
        Bytes pk(1024);
        for (auto& b : pk) { b = static_cast<std::uint8_t>(rng.next()); }
        if (rng.next() % 2) { for (int k = 0; k < 3; ++k) { pk[static_cast<std::size_t>(k)] = 0x7F; pk[static_cast<std::size_t>(512 + k)] = 0x7F; } }
        dec.configure(1 + static_cast<int>(rng.next() % 8), 1, 48000);
        std::vector<Cx> out;
        const bool good2 = dec.decodePacket(pk.data(), out);
        CHECK(good2 == (!out.empty()));
        if (good2) { CHECK(static_cast<int>(out.size()) == 2 * hp::groupsPerFrame(dec.receivers())); }
    }
    // control builder: random settings, validated, and what is built reads back through an independent parser
    for (int i = 0; i < 20000; ++i) {
        hp::ControlSettings s;
        s.rateHz = (rng.next() % 4 == 0) ? static_cast<int>(rng.next() % 500000) : hp::supportedRates()[rng.next() % 4];
        s.receivers = static_cast<int>(rng.next() % 11) - 1;
        for (auto& f : s.rxFreqHz) { f = rng.next(); }
        s.tenMhzRef = static_cast<std::uint8_t>(rng.next() % 5);
        s.clock122 = static_cast<std::uint8_t>(rng.next() % 3);
        s.config = static_cast<std::uint8_t>(rng.next() % 5);
        s.attenuatorEnable = rng.next() % 2 != 0;
        s.attenuatorDb = static_cast<int>(rng.next() % 40) - 3;
        s.preamp = rng.next() % 2 != 0;
        const bool want = (s.rateHz == 48000 || s.rateHz == 96000 || s.rateHz == 192000 || s.rateHz == 384000) && s.receivers >= 1 &&
                          s.receivers <= 7 && s.tenMhzRef <= 2 && s.clock122 <= 1 && s.config <= 3 && s.attenuatorDb >= 0 && s.attenuatorDb <= 31;
        CHECK(hp::validateSettings(s).empty() == want);
        if (!want) { continue; }
        hp::ControlSender snd;
        std::string why;
        CHECK(snd.set(s, why));
        bool sawGeneral = false;
        int ncos = 0;
        for (std::size_t k = 0; k < snd.packetsPerCycle(); ++k) {
            const Bytes pk = snd.nextPacket();
            for (int fr = 0; fr < 2; ++fr) {
                const std::uint8_t* f = pk.data() + 8 + 512 * fr;
                CHECK((f[3] & 1) == 0);
                const std::uint32_t word = (static_cast<std::uint32_t>(f[4]) << 24) | (static_cast<std::uint32_t>(f[5]) << 16) | (static_cast<std::uint32_t>(f[6]) << 8) | f[7];
                if (f[3] == 0x00) { sawGeneral = true; CHECK(((f[7] >> 3) & 7) + 1 == s.receivers && (f[4] & 3) == (s.rateHz == 48000 ? 0 : s.rateHz == 96000 ? 1 : s.rateHz == 192000 ? 2 : 3)); }
                if (f[3] >= 0x04 && f[3] <= 0x10) { ++ncos; CHECK(word == s.rxFreqHz[static_cast<std::size_t>((f[3] - 0x04) / 2)]); }
            }
        }
        CHECK(sawGeneral && ncos >= s.receivers);
    }
}

}  // namespace

int main() {
    std::fprintf(stderr, "testHandBuiltFrame\n");   testHandBuiltFrame();
    std::fprintf(stderr, "testReceiverCounts\n");   testReceiverCounts();
    std::fprintf(stderr, "testRefusals\n");         testRefusals();
    std::fprintf(stderr, "testStatusBytes\n");      testStatusBytes();
    std::fprintf(stderr, "testControlBytes\n");     testControlBytes();
    std::fprintf(stderr, "testValidation\n");       testValidation();
    std::fprintf(stderr, "testSender\n");           testSender();
    std::fprintf(stderr, "testStandInReceive\n");   testStandInReceive();
    std::fprintf(stderr, "testOverflowAndOtherBoard\n"); testOverflowAndOtherBoard();
    std::fprintf(stderr, "testBoardArgument\n");    testBoardArgument();
    std::fprintf(stderr, "testRefusedSettings\n");  testRefusedSettings();
    std::fprintf(stderr, "testStreamFaults\n");     testStreamFaults();
    std::fprintf(stderr, "testCyclesAndClose\n");   testCyclesAndClose();
    std::fprintf(stderr, "testFuzz\n");             testFuzz();
    return testSummary("test_hpsdr_frames");
}
