// msi2500.hpp - the Mirics MSi2500 bridge chip: its USB vendor requests, its
// register writes, the sample-rate arithmetic and the sample formats that come
// back off the bulk pipe. Numbers and pure functions, with nothing in it that
// can touch a device.
//
// WHY IT IS A SEPARATE HEADER FROM THE DRIVER, the same reason
// hackrf_protocol.hpp and airspy_protocol.hpp are: everything here can be
// checked without a radio, and there is NO MIRICS DEVICE ON THIS BENCH. Every
// number below is arithmetic that the silicon and every driver that speaks to
// it agree on, and tests/test_mirisdr_source.cpp names the reference function
// each expectation came from.
//
// THE LICENCE POSTURE, WHICH IS THE SAME ONE THE RTL-SDR DRIVER TOOK.
// libmirisdr-4 is GPL-2.0, and this product never links copyleft code. NO
// CODE, COMMENT, STRUCTURE OR IDENTIFIER FROM IT WAS COPIED. What it was used
// for is DOCUMENTATION: the MSi2500 and the MSi001 have no publicly published
// register map, so the only available description of how the silicon behaves -
// which vendor request reaches which block, how a register write is encoded
// into a control transfer, what the ADC initialisation values are, how the
// sample clock divides and which packing each rate produces - is the behaviour
// of the driver that already speaks to it. Those are facts about hardware, and
// facts are not copyrightable. This implementation is written from them in
// this codebase's own style, with its own structure and its own comments
// explaining WHY each step exists. installer/THIRD-PARTY-LICENSES.txt carries
// the same statement.
//
// ONE THING THIS HEADER IS HONEST ABOUT. The reference pack read for this work
// contains libmirisdr's registers, its rate arithmetic, its band plan and its
// gain map, but NOT its convert/*.c - the files that unpack the bit-packed
// formats. The packing below is therefore DERIVED from the reference's own
// size arithmetic (see kBlockBytes) rather than read off, and the twelve- and
// ten-bit BIT ORDERS in particular are the conventional little-endian packing
// rather than a measured fact. They are marked where they appear, and the
// first Mirics device on a bench is what settles them. The sixteen- and
// eight-bit formats are not in doubt: one byte or two per component leaves
// nothing to order.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <chrono>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "usb/usb_device.hpp"

namespace cascade::source::msi2500 {

// --- identity -------------------------------------------------------------

// The devices built around an MSi2500 + MSi001 pair. Mirics' own id first,
// then the television sticks and the early SDRplay units that shipped the same
// silicon behind a different label. A device not in this list is not opened:
// the vendor requests below would be sent to something that has never heard of
// them.
struct DeviceModel {
    std::uint16_t vid;
    std::uint16_t pid;
    const char* label;   // what the Source section shows
    bool sdrPlayFlavour;  // which band plan the tuner uses (see tuner_msi001.hpp)
};

// Kept as a function rather than a namespace-scope array so there is exactly
// one definition of it in the program, and so a caller cannot extend it.
const std::vector<DeviceModel>& deviceModels();

// Null when the pair is not one of ours.
const DeviceModel* modelFor(std::uint16_t vid, std::uint16_t pid);

// The VID/PID list enumeration asks the transport for, built from the above.
std::vector<cascade::usb::UsbId> usbIds();

// --- the control endpoint -------------------------------------------------

// THE REQUEST TYPE IS 0x42, NOT THE 0x40 EVERY OTHER DRIVER HERE USES, and
// that is deliberate rather than a typo: bits 6:5 say vendor, but the
// recipient field is 2 (endpoint) instead of 0 (device). The MSi2500 answers
// its register writes addressed that way and this driver has no hardware to
// discover a second acceptable spelling on, so it sends exactly what the
// silicon is known to take. usb_device.hpp's kRequestTypeVendorOut is 0x40 and
// is deliberately NOT used here.
constexpr std::uint8_t kRequestTypeVendorOutEndpoint = 0x42;

// The vendor requests. Only three of them are ever sent by this driver - a
// request we cannot name is a request we cannot send - but the numbers around
// them are recorded because the gaps are what make a number plausible.
enum class VendorRequest : std::uint8_t {
    Reset = 0x40,           // not sent: our transport has no device reset
    WriteRegister = 0x41,   // sent, for every register in the chip and the tuner
    ReadRegister = 0x42,    // not sent
    StartStreaming = 0x43,  // sent
    Download = 0x44,        // not sent
    StopStreaming = 0x45,   // sent
    ReadEeprom = 0x46,      // not sent
    WriteEeprom = 0x47,     // not sent
    WriteGpio = 0x49,       // not sent: the band switch rides in register 8
};

constexpr std::uint8_t requestByte(VendorRequest r) { return static_cast<std::uint8_t>(r); }

// The reference's CTRL_TIMEOUT. A std::chrono duration rather than a bare
// unsigned so tests/test_shutdown_budget.cpp's scan can DISCOVER it - a wait
// that file has not been introduced to is a wait nothing holds the shutdown
// budget to. The transport takes milliseconds as an unsigned, so the call
// sites convert.
//
// It is TWENTY TIMES the HackRF's 100 ms and four times the Airspy's 500, and
// that is the reference's number rather than a hedge. It is also why this
// driver sends at most ONE control transfer on its teardown path: see the
// kKnownWaits rows for the arithmetic that keeps the shutdown budget.
constexpr std::chrono::milliseconds kControlTimeout{2000};
constexpr unsigned kControlTimeoutMs = static_cast<unsigned>(kControlTimeout.count());

// THE ONE EXCEPTION, AND IT IS ARITHMETIC RATHER THAN TASTE. Exactly one
// control transfer is spent on the GUI thread during a teardown - the
// stop-streaming command in MiriSdrSource::stopStreamingLocked - and at 2000
// ms it would make this driver's shutdown column 2000 + kReaderJoinWait 1000 +
// the transport's kAbortDrainWait 250 = 3250 ms, longer than the 3000 ms
// SoapySDR column that tests/test_shutdown_budget.cpp charges the budget for.
// One driver's teardown would then be the worst case and the whole budget
// would have to be re-derived around it.
//
// So the teardown write gets its own bound, and the reason it is SAFE to
// shorten is what happens when it expires: we stop anyway. A device that has
// not accepted a 500 ms setup packet is not going to accept a 2000 ms one
// either, and everything behind this transfer - the reader join, the ring
// teardown - is bounded whatever it answers. 500 ms matches what the Airspy
// pair spend in the same place, which puts this driver's column at 1750 ms.
constexpr std::chrono::milliseconds kTeardownControlTimeout{500};
constexpr unsigned kTeardownControlTimeoutMs =
    static_cast<unsigned>(kTeardownControlTimeout.count());

// --- register writes ------------------------------------------------------
//
// EVERY register in this chip, and every register in the MSi001 tuner behind
// it, is written by one control transfer with NO DATA STAGE: the register
// number and the low byte of the value ride in wValue, and the upper sixteen
// bits of the value ride in wIndex. A register is therefore 24 bits wide as
// far as the host is concerned, and the tuner's own registers are reached by
// writing the whole 24-bit tuner word to register 9 (the tuner's register
// number is the low nibble of that word - see tuner_msi001.hpp).
struct RegWrite {
    std::uint16_t value = 0;
    std::uint16_t index = 0;
};

constexpr RegWrite encodeRegWrite(std::uint8_t reg, std::uint32_t val) {
    return RegWrite{static_cast<std::uint16_t>(((val & 0xFFu) << 8) | reg),
                    static_cast<std::uint16_t>((val >> 8) & 0xFFFFu)};
}

// --- the registers this driver writes -------------------------------------

constexpr std::uint8_t kRegReset = 0x00;
constexpr std::uint8_t kRegTunerClock = 0x02;   // named for what the init sets, not measured
constexpr std::uint8_t kRegClockDivider = 0x03;  // the sample clock, and the ADC sleep bit
constexpr std::uint8_t kRegClockFraction = 0x04;
constexpr std::uint8_t kRegAdc = 0x05;
constexpr std::uint8_t kRegFormat = 0x07;
constexpr std::uint8_t kRegBandSwitch = 0x08;  // and the bias-T, bit 11
constexpr std::uint8_t kRegTunerWord = 0x09;   // everything the MSi001 is told

// One register write in a scripted sequence.
struct RegSet {
    std::uint8_t reg;
    std::uint32_t val;
};

// The static initialisation the chip needs before it will clock its ADC, in
// order. Five writes, none of which depends on the rate or the frequency -
// which is why they are a table rather than a function.
const std::vector<RegSet>& adcInitSequence();

// What puts the USB interface and the ADC back to sleep. Sent when a running
// radio has to be made quiet for a rate change, and once at open before
// anything is programmed: a device that was left streaming by whatever ran
// before us is a device whose first transfer would be somebody else's.
constexpr std::uint32_t kAdcStopValue = 0x010000u;

// The bias-T rides in register 8 alongside the band switch, so the two can
// never be written independently - hence one function that takes both.
//
// AND IT COLLIDES WITH TWO OF THE BAND SWITCH'S OWN WORDS, which is recorded
// here rather than smoothed over: the band plan's L-band and upper Band III
// rows carry 0xFA80 and 0xFF80, both of which ALREADY have bit 11 set. In
// those bands switching the bias-T on changes nothing and switching it off
// cannot turn it off. That is the reference's own arrangement and it is
// reproduced faithfully, because the alternative is inventing a different bit
// for a control that puts voltage on somebody's antenna. It is the first thing
// to measure on the first Mirics device that reaches a bench.
constexpr std::uint32_t kBiasTeeBit = 1u << 11;

constexpr std::uint32_t bandSwitchValue(std::uint32_t bandSelectWord, bool biasT) {
    return biasT ? (bandSelectWord | kBiasTeeBit) : bandSelectWord;
}

// --- the sample formats ---------------------------------------------------
//
// WHERE THESE COME FROM, since the packing is the one thing the reference pack
// did not carry. The reference names four formats after the number of COMPLEX
// PAIRS each USB block holds - 252, 336, 384, 504 - and records the top rate
// each one supports as 6.048, 8.064, 9.216 and 12.096 MS/s, every one of them
// at "24.576 MB/s". Those five numbers only agree with each other if a block
// is 1024 bytes and the device sends 24000 of them a second: 6.048e6 / 252 =
// 24000, and 24000 * 1024 = 24.576e6. Every other format divides out to the
// same 24000. So the block size is 1024 and it is fixed, and the sample rate
// is carried entirely by how many pairs are packed into one.
//
// From there the component width is arithmetic: 252 pairs of two components in
// 1008 bytes is sixteen bits each, 336 pairs is twelve, 504 pairs is eight.
// 1008 is what is left of 1024 after a 16-byte header, which is the only
// header size that makes all three land exactly. The 384 format is the odd one
// - 768 components at ten bits is 960 bytes, forty-eight short of the payload
// - so it is packed with the payload's tail unused. THAT ONE IS THE LEAST
// CERTAIN THING IN THIS FILE and is why supportedSampleRatesHz() offers no
// rate that selects it.
enum class Format {
    Bits16,  // the reference's "252": 252 pairs a block, one int16 a component
    Bits12,  // "336": 336 pairs, twelve bits a component
    Bits10,  // "384": 384 pairs, ten bits a component
    Bits8,   // "504": 504 pairs, one int8 a component
};

constexpr std::size_t kBlockBytes = 1024;
constexpr std::size_t kBlockHeaderBytes = 16;
constexpr std::size_t kBlockPayloadBytes = kBlockBytes - kBlockHeaderBytes;  // 1008

// Pairs per 1024-byte block, which IS the format's name in the reference.
constexpr std::size_t pairsPerBlock(Format f) {
    return f == Format::Bits16   ? 252
           : f == Format::Bits12 ? 336
           : f == Format::Bits10 ? 384
                                 : 504;
}

// Bits per component, and the full-scale divisor that follows from it.
constexpr int componentBits(Format f) {
    return f == Format::Bits16 ? 16 : f == Format::Bits12 ? 12 : f == Format::Bits10 ? 10 : 8;
}

// SCALED BY THE CONTAINER, not by the converter's 14 bits. The ADC behind all
// four formats is fourteen bits, so the sixteen-bit format's codes may well
// occupy only the middle of their container and read about 12 dB low in
// absolute terms. Scaling by the container can only ever be quiet; scaling by
// a guess at the alignment could CLIP, and a clipped sample is a defect that
// looks like a strong signal. Every stage downstream of here is relative.
constexpr float fullScale(Format f) {
    return static_cast<float>(1 << (componentBits(f) - 1));
}

// The format register's own value, and the value the reference writes for each
// (hard.c mirisdr_set_hard). Kept as a function of the format so the two can
// never drift apart.
constexpr std::uint32_t formatRegisterValue(Format f) {
    return f == Format::Bits16   ? 0x000094u
           : f == Format::Bits12 ? 0x000085u
           : f == Format::Bits10 ? 0x0000A5u
                                 : 0x000C94u;
}

// The nibble the clock register carries for each format (hard.c, the switch
// that ORs bits 12-15 of register 3). The reference's comment calls it AGC.
constexpr std::uint32_t formatClockNibble(Format f) {
    return f == Format::Bits16 ? 0x01u : f == Format::Bits12 ? 0x05u : f == Format::Bits10 ? 0x09u
                                                                                           : 0x0Du;
}

// --- unpacking ------------------------------------------------------------

// One 1024-byte block into complex samples in [-1, 1). `dst` must have room
// for pairsPerBlock(f). Returns what it wrote, which is always that count: a
// short block is not this function's problem, because its caller only ever
// hands it whole ones.
//
// The header is SKIPPED AND NOT INTERPRETED. Sixteen bytes is what the payload
// arithmetic leaves for it (see Format above); what is in them - a sequence
// number, most likely, since that is what such a header is usually for - is
// not something this driver can claim to know, and a driver that acted on a
// field it had guessed would be worse than one that ignores a field it has.
std::size_t unpackBlock(Format f, const std::uint8_t* block, std::complex<float>* dst);

// Whole blocks out of one bulk transfer. A partial tail is DROPPED rather than
// half-decoded: the transfer size is a multiple of the block size, so a tail
// means the device gave us a short read, and half a block is noise with the
// shape of signal.
std::size_t unpackTransfer(Format f, const std::uint8_t* src, std::size_t bytes,
                           std::complex<float>* dst, std::size_t dstCap);

// --- the sample rate ------------------------------------------------------

// The reference's own bounds (hard.h). Below the floor the divider N would
// fall under 2, which the chip does not accept; above the ceiling it enters a
// mode the reference documents as one it cannot get back out of.
constexpr double kMinSampleRateHz = 1.3e6;
constexpr double kMaxSampleRateHz = 15.0e6;

// Everything one rate becomes: the format it selects, and the three register
// writes that program it.
struct RateSetting {
    std::uint32_t rateHz = 0;  // after the clamp, which is what was programmed
    Format format = Format::Bits16;
    std::uint32_t formatRegister = 0;  // register 7
    std::uint32_t clockFraction = 0;   // register 4
    std::uint32_t clockDivider = 0;    // register 3
    // The working values, exposed because they are what a test can pin against
    // the reference's own printout rather than against a second copy of this
    // arithmetic.
    std::uint64_t vcoHz = 0;
    std::uint64_t n = 0;
    std::uint64_t fraction = 0;
};

// hard.c mirisdr_set_hard, the rate half. The VCO is walked up in even
// multiples until it clears 202 MHz, which is what fixes the divider; the
// leftover becomes a 21-bit fraction whose top bit is a half-step flag in the
// clock register and whose low twenty bits are a register of their own.
//
// OUT-OF-RANGE RATES ARE CLAMPED HERE, not refused, because this is
// arithmetic: the driver above decides whether a clamp is acceptable or
// whether the caller should be told no (MiriSdrSource::setSampleRateHz
// refuses below the floor and clamps above the ceiling, for the reasons
// hackrf_source.hpp argues).
RateSetting computeRate(double hz);

// Blocks a second, which is the same 24000 for every rate and every format -
// see Format. Exposed because it is what turns a transfer size into a duration
// when sizing the ring.
constexpr double kBlocksPerSecond = 24000.0;

// --- the bulk stream ------------------------------------------------------

// The reference fills its bulk transfers from endpoint 1 IN (sync.c, async.c).
constexpr std::uint8_t kRxEndpoint = 0x81;

// Sixty-four blocks a transfer. A multiple of 1024 so blocks never straddle a
// transfer, and therefore a multiple of the 512 WinUSB requires. At the fixed
// 24000 blocks a second that is 2.67 ms of signal WHATEVER the rate, which is
// the pleasant consequence of a format that carries the rate in its packing
// rather than in its block rate.
constexpr std::size_t kTransferBytes = kBlockBytes * 64;  // 65536
constexpr std::size_t kBlocksPerTransfer = kTransferBytes / kBlockBytes;

// Eight in flight, so the host may be 21 ms late without the device having
// nowhere to put a block.
constexpr std::size_t kTransferCount = 8;

// The most complex samples one transfer can become - the widest format, every
// block full. What the reader's conversion buffer is sized to.
constexpr std::size_t kMaxSamplesPerTransfer = kBlocksPerTransfer * 504;

}  // namespace cascade::source::msi2500
