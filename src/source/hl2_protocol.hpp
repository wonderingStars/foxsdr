// hl2_protocol.hpp - what a RECEIVE-ONLY client needs of the Hermes-Lite 2 extensions to the openHPSDR protocol 1:
// the extended discovery reply, the receive-side command words, the direct LNA gain, the watchdog bit of the
// Start/Stop packet, and the status words the radio puts in the C&C bytes of every data frame.
//
// SOURCE, USED AS FACTS ONLY: the Hermes-Lite 2 project's wiki page "Protocol"
// (https://github.com/softerhardware/Hermes-Lite2/wiki/Protocol , read 2026-11-12). THE REPOSITORY HAS NO LICENCE
// FILE and its gateware headers say GNU GPL v2 or later: no code, no gateware, no file of the repository was read
// or used; what is taken from the wiki page is the content of an interface description - which bit means what
// and which address holds what - written out again here in this repository's own words and code. The page states
// no licence either. The page is itself "based on the original protocol1 from openHPSDR" (the USB protocol and
// Metis documents the earlier lanes already used, see hpsdr1_frames.hpp and hpsdr_protocol.hpp), which is what the
// brief means by "facts only".
//
// BUILT (receive side): Board_ID 0x06 recognition; the discovery-reply fields (gateware major and minor, number of
// hardware receivers, wide-band format, the status bytes); the C0 layout (RQST bit 7, ADDR[5:0] in bits 6..1, MOX
// bit 0 - never set here); RX NCO addresses for receivers 1..12; the direct LNA gain (address 0x0A bit 6 with
// LNA[5:0] = -12 dB .. +48 dB); the Start/Stop watchdog bit; the ACK==0 status words (addresses 0..2).
//
// NOT BUILT, ON PURPOSE: everything that writes to the radio's own hardware or settings - the I2C buses (0x3C,
// 0x3D), the AD9866 SPI window (0x3B), the PA bias values, the EEPROM (fixed IP, MAC, valid flags), "reset on
// disconnect", the clock-generator, synchronisation and master commands (0x39), the PureSignal and predistortion
// words, CW/PTT/TX drive words (this client never transmits and never sets MOX), the wide-band (ADC) data stream.
// Receivers 2..12 are not streamed to the pipeline (only receiver 1), as in Hpsdr1Source.
//
// UNVERIFIED ON A HERMES-LITE 2 (none is here): every bit position below is the wiki's word; the wiki's own text is
// not consistent in places (it calls the filter-board bits Alex bits in one paragraph and says MOX in another);
// whether the units of temperature, power and current counts are volts, amps or raw counts is NOT stated by the page and
// is therefore not claimed: the values are carried as raw counts.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace cascade::source::hl2 {

constexpr std::uint8_t kBoardId = 0x06;  // "The Hermes-Lite2 will use Board_ID 0x06" (discovery byte 0x0A in its own table)
constexpr std::size_t kDiscoveryBytes = 60;

// ---- the discovery reply (offsets are the wiki table's "Address" column) ---------------------------------------------
struct Info {
    bool valid = false;               // a 60-byte reply that says Board_ID 6
    std::uint8_t gatewareMajor = 0;   // 0x09
    std::uint8_t gatewareMinor = 0;   // 0x15
    std::uint8_t hardwareReceivers = 0;  // 0x13
    bool wideband16Bit = false;       // 0x14 bits 7:6 = 01 (16-bit two's complement); 00 = 12-bit sign extended
    std::uint8_t buildBoard = 0;      // 0x14 bits 5:0 ("Board ID: 5, 3 or 2 for build")
    bool externalCwKey = false;       // 0x1B bit 7
    bool ptt = false;                 // 0x1B bit 6
    std::uint8_t adcClipCount = 0;    // 0x1B bits 1:0
    std::uint16_t temperature = 0;    // 0x1C (4 msb bits) 0x1D: raw counts
    std::uint16_t forwardPower = 0;   // 0x1E 0x1F
    std::uint16_t reversePower = 0;   // 0x20 0x21
    std::uint16_t biasCurrent = 0;    // 0x22 0x23
};

// `d` is the whole datagram, `n` its length (60 expected: shorter replies carry no extension and are refused with
// valid == false; longer are not trusted beyond 60). The Board_ID is read where the base protocol puts it (byte 10).
inline Info parseDiscovery(const std::uint8_t* d, std::size_t n) {
    Info i;
    if (d == nullptr || n < kDiscoveryBytes) { return i; }
    if (d[0] != 0xEF || d[1] != 0xFE || (d[2] != 0x02 && d[2] != 0x03)) { return i; }
    if (d[10] != kBoardId) { return i; }
    i.valid = true;
    i.gatewareMajor = d[9];
    i.hardwareReceivers = d[0x13];
    i.wideband16Bit = ((d[0x14] >> 6) & 3) == 1;
    i.buildBoard = static_cast<std::uint8_t>(d[0x14] & 0x3F);
    i.gatewareMinor = d[0x15];
    i.externalCwKey = (d[0x1B] & 0x80) != 0;
    i.ptt = (d[0x1B] & 0x40) != 0;
    i.adcClipCount = static_cast<std::uint8_t>(d[0x1B] & 3);
    i.temperature = static_cast<std::uint16_t>(((d[0x1C] & 0x0F) << 8) | d[0x1D]);
    i.forwardPower = static_cast<std::uint16_t>(((d[0x1E] & 0x0F) << 8) | d[0x1F]);
    i.reversePower = static_cast<std::uint16_t>(((d[0x20] & 0x0F) << 8) | d[0x21]);
    i.biasCurrent = static_cast<std::uint16_t>(((d[0x22] & 0x0F) << 8) | d[0x23]);
    return i;
}

// ---- command words (PC to radio) ---------------------------------------------------------------------------------------
// C0 = RQST(7) | ADDR[5:0](6:1) | MOX(0); DATA[31:0] = C1..C4, MSB in C1. MOX is NEVER set by this code.
constexpr std::uint8_t c0For(int address, bool request) {
    return static_cast<std::uint8_t>((request ? 0x80 : 0x00) | ((address & 0x3F) << 1));
}
constexpr int kAddrGeneral = 0x00;  // speed [25:24], number of receivers - 1 [6:3], duplex [2]
constexpr int kAddrLna = 0x0A;      // [6] direct LNA, [5:0] LNA gain; [5] is the step-attenuator enable in Hermes mode
constexpr int kAddrWatchdogEtc = 0x39;  // listed here only so tests can prove it is never sent

// The NCO address of receiver n (1..12): receivers 1..7 at 0x02..0x08, receivers 8..12 at 0x12..0x16. -1 outside.
constexpr int rxNcoAddress(int n) { return n >= 1 && n <= 7 ? 1 + n : (n >= 8 && n <= 12 ? 0x12 + (n - 8) : -1); }

constexpr int kMaxReceivers = 12;  // number of receivers field: 0000 = 1 .. 1011 = 12

// The direct LNA gain: with bit 6 set, LNA[5:0] goes straight to the AD9866: code 0 = -12 dB .. code 60 = +48 dB.
constexpr int kLnaMinDb = -12, kLnaMaxDb = 48;
constexpr bool lnaDbValid(int db) { return db >= kLnaMinDb && db <= kLnaMaxDb; }
constexpr std::uint8_t lnaDirectByte(int db) { return static_cast<std::uint8_t>(0x40 | ((db - kLnaMinDb) & 0x3F)); }
// The Hermes-compatible form (bit 6 clear): LNA[4:0] is attenuation, 0 = no attenuation (+19 dB) .. maximum (-12 dB),
// bit 5 = step attenuator enabled. Not used by the direct mode; here for the test that proves bit 6 is what differs.
constexpr std::uint8_t lnaHermesByte(int attenuationSteps, bool enable) {
    return static_cast<std::uint8_t>((enable ? 0x20 : 0x00) | (attenuationSteps & 0x1F));
}

// Start/Stop packet command byte: bit 0 radio, bit 1 wide band (never set here), bit 7 = disable the radio's
// watchdog (the radio otherwise stops sending when the host stops sending commands; a receive-only program may
// want it off - and then a crashed program leaves the radio streaming, which is why it is off by default here).
constexpr std::uint8_t kStartRadio = 0x01, kWatchdogOff = 0x80;
constexpr std::uint8_t startCommand(bool watchdogOff) { return static_cast<std::uint8_t>(kStartRadio | (watchdogOff ? kWatchdogOff : 0)); }
constexpr std::uint8_t stopCommand(bool watchdogOff) { return static_cast<std::uint8_t>(watchdogOff ? kWatchdogOff : 0); }

// ---- status words in the C&C bytes of a radio-to-PC frame (ACK == 0) ----------------------------------------------------------
struct Telemetry {
    bool valid = false;
    int radioAddress = 0;            // RADDR[3:0] = C0[6:3]
    bool ack = false;                // C0[7]
    bool txInhibitBit = false;       // address 0, C1 bit 1, RAW: the wiki names it "Tx Inhibited (Active Low)"; its polarity is not claimed here
    bool adcOverload = false;        // address 0, C1 bit 0 ("RF ADC overload, active high")
    std::uint8_t fifoFlags = 0;      // address 0, C3[7:6]: 2 = underflow, 3 = overflow (the wiki's footnote); the table also reuses bit 6 for the FIFO count
    std::uint8_t txFifoMsb = 0;      // address 0, C3[6:0]
    std::uint8_t firmwareVersion = 0;// address 0, C4
    std::uint16_t temperature = 0, forwardPower = 0, reversePower = 0, current = 0;  // raw counts
};

// Decodes the five bytes C0..C4. Only the three addresses the wiki defines are read (0, 1, 2); another RADDR gives
// valid == false (the radio may use them for something this client does not know). A frame with ACK set answers a
// REQUEST this client never makes, so it is not interpreted.
inline Telemetry decodeStatus(std::uint8_t c0, std::uint8_t c1, std::uint8_t c2, std::uint8_t c3, std::uint8_t c4) {
    Telemetry t;
    t.ack = (c0 & 0x80) != 0;
    t.radioAddress = (c0 >> 3) & 0x0F;
    if (t.ack) { return t; }
    switch (t.radioAddress) {
        case 0:
            t.txInhibitBit = (c1 & 0x02) != 0;
            t.adcOverload = (c1 & 0x01) != 0;
            t.fifoFlags = static_cast<std::uint8_t>(c3 >> 6);
            t.txFifoMsb = static_cast<std::uint8_t>(c3 & 0x7F);
            t.firmwareVersion = c4;
            break;
        case 1:
            t.temperature = static_cast<std::uint16_t>((c1 << 8) | c2);
            t.forwardPower = static_cast<std::uint16_t>((c3 << 8) | c4);
            break;
        case 2:
            t.reversePower = static_cast<std::uint16_t>((c1 << 8) | c2);
            t.current = static_cast<std::uint16_t>((c3 << 8) | c4);
            break;
        default:
            return t;
    }
    t.valid = true;
    return t;
}

}  // namespace cascade::source::hl2
