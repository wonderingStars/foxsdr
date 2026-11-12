// hpsdr1_frames.hpp - the layout INSIDE the 512-byte frames of openHPSDR protocol 1: the receive frame
// (sync, C&C status, interleaved 24-bit I/Q and 16-bit microphone samples for 1..8 receivers) and the
// transmit-direction command frame (sync + C0..C4 + zeroed audio) that carries the sample rate, the receiver
// frequencies and the attenuator. No sockets here. This is the piece the earlier hpsdr_protocol.hpp
// listed as missing; it closes the gap for Hermes-class boards and for the Red Pitaya SDR receiver/
// transceiver, which present themselves as HPSDR Hermes radios.
//
// PROVENANCE. "HPSDR - USB Data Protocol", revisions up to V1.60 (23 Jan 2019; authors VK6APH, K5SO,
// VK6PH), file Documentation/USB_protocol_V1.60.doc (V1.58 was fetched too) in the public repository
// https://github.com/TAPR/OpenHPSDR-SVN , fetched on 2026-11-11 into a scratch folder outside the repository
// and read in full (text extracted with antiword). The same document is the one Pavel Demin's page for
// the Red Pitaya SDR receiver links as "HPSDR - USB Data Protocol"
// (https://pavel-demin.github.io/red-pitaya-notes/sdr-receiver-hpsdr , which also gives: Hermes emulation,
// 48/96/192 kSPS, 0 to 61.44 MHz, "eight receivers"; opened, no licence stated). The document states no
// licence; the repository contains no top-level licence file (the page of the repository's file listing
// shows none). It is a protocol description published by the project for implementers, read as such;
// NO source code of any HPSDR program (Thetis, PowerSDR, piHPSDR, linHPSDR, HL2 firmware ...) was read.
// The Hermes-Lite 2 wiki page https://github.com/softerhardware/Hermes-Lite2/wiki/Protocol (no licence
// stated) was opened by a fetch tool BEFORE the decision to read the USB document; its summary
// (C0 bit meanings, the 48/96/192/384 kHz rate bits, "RX1 = address 0x02") agrees with the USB document;
// nothing here depends on it, and it is recorded in the feature record. HL2-specific extensions are not built.
//
// FRAME (512 bytes, from the document):
//   <7F><7F><7F><C0><C1><C2><C3><C4> then samples. Per sample set, for n receivers: n times
//   <I2><I1><I0><Q2><Q1><Q0> (24-bit signed, MOST significant byte first, I then Q) then <M1><M0> (16-bit
//   microphone, always 48 kHz). The frame is padded with zeros at the end when the sample sets do not fill
//   it: receivers 1..8 -> 0,0,4,10,24,10,20,4 bytes of padding (the document's table; it equals the left-over
//   of 504 / (6n + 2), which the tests also check). Sets per frame = 504 / (6n+2) rounded down.
//   HW->PC C&C: address in C0[7:3]; address 0: C1[0] = A/D overflow, C2 = Mercury version, C3 = Penelope
//   version, C4 = Metis/Hermes version; other addresses carry power readings and ADC 2..4 overflow
//   (address 4: C1..C4 bit 0 = overflow of ADC1..4).
//   PC->HW (end point 2): the same envelope; C0[0] = MOX, C0[7:1] = address; C1..C4 as below; then 63 sets of
//   <L1><L0><R1><R0><I1><I0><Q1><Q0> (16-bit, zero here: this client never transmits and never sends audio).
//     address 0: C1[1:0] = speed (00 48k, 01 96k, 10 192k, 11 384k); C3[2] = preamp; C4[5:3] = number of
//                receivers - 1; C4[2] = duplex.
//     address 1: transmitter NCO, 32-bit Hz, MSB in C1 (always set equal to receiver 1, never keyed).
//     addresses 2..8: receivers 1..7 NCO, 32-bit Hz, MSB in C1. The document lists no address for receiver 8.
//     address 9: C1 = Hermes drive level (always 0 here).
//     address 10: C4[4:0] = ADC1 input attenuator 0..31 dB, C4[5] = Hermes/Angelia attenuator enable.
//
// WHAT THE DOCUMENT DOES NOT SAY, so is a HYPOTHESIS (UNVERIFIED on hardware): that duplex = 1 is needed for
// the receiver frequencies to be independent of the transmitter frequency (it is set, and the transmitter
// frequency is set to receiver 1's, so either reading gives the right result); how often the command
// frames must be repeated for a board to keep running (the source repeats the full set about every
// 100 ms); the full-scale of the 24-bit samples (taken as 2^23); a Hermes board's gain in dB per
// attenuator step (the attenuator is offered as 0..31 dB as the document says "0 - 31dB").
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cascade::source::hpsdr1 {

constexpr int kFrameBytes = 512;
constexpr int kHeaderBytes = 8;
constexpr int kMaxReceivers = 8;
constexpr int kMaxFreqReceivers = 7;  // the document lists NCO addresses for receivers 1..7 only

constexpr int kAddrGeneral = 0;
constexpr int kAddrTxFreq = 1;
constexpr int kAddrRx1 = 2;  // receiver n is address 1 + n
constexpr int kAddrDrive = 9;
constexpr int kAddrAtten = 10;

// The document's padding table, index = receivers (1..8); index 0 unused.
constexpr int kPadding[kMaxReceivers + 1] = {0, 0, 0, 4, 10, 24, 10, 20, 4};

constexpr bool validReceivers(int n) { return n >= 1 && n <= kMaxReceivers; }
// Sample sets in one frame for n receivers (0 for an invalid n).
constexpr int setsPerFrame(int n) { return validReceivers(n) ? 504 / (6 * n + 2) : 0; }

// Speed code <-> rate in Hz; -1 for a rate the protocol does not have.
constexpr int speedCodeForRate(int hz) { return hz == 48000 ? 0 : hz == 96000 ? 1 : hz == 192000 ? 2 : hz == 384000 ? 3 : -1; }
constexpr int rateForSpeedCode(int code) { return code == 0 ? 48000 : code == 1 ? 96000 : code == 2 ? 192000 : code == 3 ? 384000 : 0; }

struct FrameStatus {
    std::uint8_t c0 = 0, c1 = 0, c2 = 0, c3 = 0, c4 = 0;
    int address() const { return c0 >> 3; }
    bool adcOverload() const { return address() == 0 && (c1 & 1) != 0; }   // address 0 C1[0]
    std::uint8_t boardSoftwareVersion() const { return address() == 0 ? c4 : 0; }
};

inline float s24be(const std::uint8_t* p) {
    std::int32_t v = (static_cast<std::int32_t>(p[0]) << 16) | (static_cast<std::int32_t>(p[1]) << 8) | p[2];
    if (v & 0x800000) { v -= 0x1000000; }
    return static_cast<float>(v) / 8388608.0f;
}

// Decodes one receive frame for `receivers` receivers. Appends sample sets to perReceiver[r] (resized to
// `receivers`). False, appending nothing, for a length other than 512, a missing sync, or a receiver
// count outside 1..8. The microphone samples and the padding are not interpreted.
inline bool decodeRxFrame(const std::uint8_t* f, std::size_t n, int receivers,
                          std::vector<std::vector<std::complex<float>>>& perReceiver, FrameStatus& st) {
    if (n != static_cast<std::size_t>(kFrameBytes) || !validReceivers(receivers)) { return false; }
    if (f[0] != 0x7F || f[1] != 0x7F || f[2] != 0x7F) { return false; }
    st.c0 = f[3]; st.c1 = f[4]; st.c2 = f[5]; st.c3 = f[6]; st.c4 = f[7];
    if (perReceiver.size() != static_cast<std::size_t>(receivers)) { perReceiver.resize(static_cast<std::size_t>(receivers)); }
    const int sets = setsPerFrame(receivers);
    const int stride = 6 * receivers + 2;
    for (int k = 0; k < sets; ++k) {
        const std::uint8_t* b = f + kHeaderBytes + k * stride;
        for (int r = 0; r < receivers; ++r) {
            perReceiver[static_cast<std::size_t>(r)].emplace_back(s24be(b + 6 * r), s24be(b + 6 * r + 3));
        }
    }
    return true;
}

struct CommandState {
    int speedCode = 0;           // 0..3
    int receivers = 1;           // 1..8 the stream carries
    std::uint32_t rxHz[kMaxFreqReceivers] = {0, 0, 0, 0, 0, 0, 0};
    bool preamp = false;
    std::uint8_t attenDb = 0;    // 0..31
    bool attenEnable = false;
    // Hermes-Lite 2 only (hl2_protocol.hpp): address 10 carries the DIRECT LNA gain, C4 = 0x40 | (dB + 12).
    bool lnaDirect = false;
    std::uint8_t lnaCode = 0;    // 0..60
};

// The addresses of one full refresh: general, transmitter frequency, the receivers' frequencies, drive,
// attenuator.
inline std::vector<int> refreshAddresses(const CommandState& s) {
    std::vector<int> a{kAddrGeneral, kAddrTxFreq};
    const int nf = s.receivers < kMaxFreqReceivers ? s.receivers : kMaxFreqReceivers;
    for (int r = 1; r <= nf; ++r) { a.push_back(1 + r); }
    a.push_back(kAddrDrive);
    a.push_back(kAddrAtten);
    return a;
}

// One transmit-direction frame for `address`. Never sets MOX; audio and I/Q fields are zero.
inline std::array<std::uint8_t, kFrameBytes> buildCommandFrame(const CommandState& s, int address) {
    std::array<std::uint8_t, kFrameBytes> f{};
    f[0] = f[1] = f[2] = 0x7F;
    f[3] = static_cast<std::uint8_t>((address & 0x7F) << 1);
    auto putHz = [&](std::uint32_t hz) {
        f[4] = static_cast<std::uint8_t>(hz >> 24);
        f[5] = static_cast<std::uint8_t>(hz >> 16);
        f[6] = static_cast<std::uint8_t>(hz >> 8);
        f[7] = static_cast<std::uint8_t>(hz);
    };
    if (address == kAddrGeneral) {
        f[4] = static_cast<std::uint8_t>(s.speedCode & 3);
        f[6] = static_cast<std::uint8_t>(s.preamp ? 0x04 : 0x00);
        f[7] = static_cast<std::uint8_t>((((s.receivers - 1) & 7) << 3) | 0x04);
    } else if (address == kAddrTxFreq) {
        putHz(s.rxHz[0]);
    } else if (address >= kAddrRx1 && address < kAddrRx1 + kMaxFreqReceivers) {
        putHz(s.rxHz[address - kAddrRx1]);
    } else if (address == kAddrDrive) {
        f[4] = 0;
    } else if (address == kAddrAtten) {
        f[7] = s.lnaDirect ? static_cast<std::uint8_t>(0x40 | (s.lnaCode & 0x3F))
                           : static_cast<std::uint8_t>((s.attenEnable ? 0x20 : 0x00) | (s.attenDb & 0x1F));
    }
    return f;
}

}  // namespace cascade::source::hpsdr1
