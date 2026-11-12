// hpsdr_frame.hpp - the openHPSDR protocol-1 "USB frame" layer: what sits INSIDE the 512-byte frames that
// the Metis UDP data packets carry (hpsdr_protocol.hpp has the envelope). Pure functions and small value
// classes; no sockets, no threads.
//
// PROVENANCE. "HPSDR - USB Data Protocol", the openHPSDR project's own protocol document, revision list to
// "23 Jan 19" (Phil Harman VK6APH; the file's metadata says author Phil Harman, last saved 2019-01-23),
// published in the project's repository https://github.com/TAPR/OpenHPSDR-SVN as
// "Documentation/USB_protocol_V1.60.doc" (Word 97-2003 file, 153088 bytes; read 2026-11-11 with the text
// pulled out of the file by a short script, whitespace kept, so the bit diagrams could be read column by
// column). The TAPR "Metis - How it works" V1.33 (hpsdr_protocol.hpp) names this document as "the latest
// version of this protocol". The older "HPSDR USB Data Protocol" V1.0 (8 June 2007, 6-page PDF in the same
// repository, vu3rdd/doc/HPSDR_USB_protocol_V1_0.pdf) was read as well for the first frame diagram. LICENCE:
// neither document states one (the repository states none at its top level and holds GPL programs in other
// folders, none of which was opened); they are read as the project's published protocol description, which
// is what the lane brief directs. No program, firmware or port of one (PowerSDR, Thetis, piHPSDR, ...) and
// not the Hermes-Lite 2 wiki (no licence) was read.
//
// WHAT THE DOCUMENT GIVES, AS USED HERE
//   * A frame is 512 bytes: 0x7F 0x7F 0x7F, five command-and-control bytes C0..C4, then 504 bytes of
//     samples. Device to PC (end point 6): per receiver 3-byte I then 3-byte Q (24 bit, big-endian: bits
//     23-16 first), and after the last receiver ONE 2-byte microphone sample (16 bit, 48 kHz whatever the
//     I/Q rate); a group is therefore 6*n+2 bytes for n receivers (n = 1..8, the C&C field "Number of
//     Receivers", 000 = 1 ... 111 = 8). Groups are packed back to back; "where there are insufficient
//     samples to exactly fill a 512 byte frame then the end of the frame is padded with 0s" (the padding
//     table gives 0, 10, 24, 10, 20, 4 for 1, 4, 5, 6, 7, 8 receivers and the three-receiver example ends
//     in four zero bytes: all equal to 504 mod (6n+2)).
//   * Device-to-PC C&C: the address is C0 bits [7:3]. Address 0: C0 bits 2..0 = DOT, DASH, PTT; C1 bit 0
//     overflow (the diagram's text); C4 = software serial number of Ozy/Metis/Hermes (0..255). Address 4
//     (C0 = 00100xxx): C1..C4 each carry, in bit 0, the overflow of ADC 1..4 ("1 = active"), and in bits
//     7..1 the Mercury 1..4 software version. Addresses 1, 2, 3 carry 12-bit analog readings (forward
//     power, reverse power, AIN3, AIN4, supply); they are not decoded here.
//   * PC-to-device (end point 2): a frame of the same shape with 63 groups of eight bytes (left audio 2,
//     right audio 2, I 2, Q 2 - all 16 bit) after the C&C; C0 bit 0 is MOX and bits [7:1] the address of
//     the setting the four bytes C1..C4 carry. Used here (receive only): address 0 (C1 bits 1..0 speed
//     00/01/10/11 = 48/96/192/384 kHz, bits 3..2 10 MHz reference, bit 4 122.88 MHz source, bits 6..5
//     config, bit 7 mic source; C2 mode/open collector; C3 Alex attenuator, preamp, dither, random, Rx
//     antenna, Rx out; C4 Alex Tx relay bits 1..0, duplex bit 2, NUMBER OF RECEIVERS minus one in bits
//     5..3, time stamp bit 6, common Mercury frequency bit 7), address 2.. (C0 = 0x04 + 2*(k-1) for
//     receiver k = 1..7: "NCO Frequency in Hz", a 32-bit binary value, MSB in C1), address 0x0A (C0 =
//     0001010x: C1 preamps Rx1..Rx4 in bits 0..3; C4 bits 4..0 ADC1 input attenuator 0..31 dB, bit 5
//     "Hermes/Angelia attenuator enable ... if disabled then Preamp On/Off bit is used").
//
// WHAT THE DOCUMENT DOES NOT SAY, SO IT IS A STATED CHOICE, NOT A FACT
//   * the numeric format of the 24-bit samples (the protocol-2 document, not this one, says 2's
//     complement): they are read as signed two's complement and scaled by 2^-23. UNVERIFIED ON HARDWARE.
//   * whether the I and Q order or sign need a swap on receive: none is applied (the document's only swap
//     remark is about the transmit direction).
//   * how often the PC must send frames, and any frequency or rate limit of a board. The sender here sends
//     every setting once at Start, on every change, and refreshes them periodically (a choice).
//   * a receiver 8 frequency address (the document lists NCO addresses for the transmitter and receivers
//     1..7 only): eight receivers can be UNPACKED, only seven can be TUNED.
//   * what a board does with a C&C bit it does not implement ("Ignored by Hermes" is stated for the
//     clock-source bits only).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cascade::source::hpsdr {

constexpr std::uint8_t kSyncByte = 0x7F;
constexpr std::size_t kFramePayloadBytes = 504;  // 512 - 3 sync - 5 C&C
constexpr int kMaxReceivers = 8;                  // C4 bits 5..3 of the PC's address-0 setting
constexpr int kMaxTunedReceivers = 7;             // NCO addresses exist for receivers 1..7 only

// Samples per frame and padding bytes for n receivers (n outside 1..8: 0 and 0).
int groupsPerFrame(int receivers);
int paddingBytes(int receivers);

// The sample rates the speed field offers (C1 bits 1..0).
bool rateToSpeedCode(int rateHz, std::uint8_t& code);
bool speedCodeToRate(std::uint8_t code, int& rateHz);
const std::array<int, 4>& supportedRates();

// ---- receive: device to PC ----------------------------------------------------------------------

enum class FrameVerdict {
    Ok,
    WrongLength,        // not exactly 512 bytes
    BadSync,            // the three sync bytes are not 0x7F
    BadReceiverCount,   // n outside 1..8 (a caller error)
    BadSelection,       // the receiver to deliver is outside 1..n
};
const char* verdictName(FrameVerdict v);

// What a frame's five C&C bytes say, as far as the document defines it for the device-to-PC direction.
struct FrameStatus {
    std::array<std::uint8_t, 5> cc{};
    std::uint8_t address = 0;  // C0 >> 3
    bool ptt = false, dash = false, dot = false;   // address 0, C0 bits 0, 1, 2
    // address 0: C4. address 4: per ADC overflow flags and Mercury software versions.
    bool hasSoftwareSerial = false;
    std::uint8_t softwareSerial = 0;
    bool hasOverflow = false;
    std::array<bool, 4> adcOverflow{};
    std::array<std::uint8_t, 4> mercuryVersion{};
};

// Decodes ONE 512-byte device-to-PC frame carrying `receivers` receivers. Appends the samples of receiver
// `deliver` (1-based) to `out`, scaled to +-1; writes the status. Nothing is appended on a refusal.
FrameVerdict decodeFrame(const std::uint8_t* frame, std::size_t length, int receivers, int deliver,
                         std::vector<std::complex<float>>& out, FrameStatus& status);

// Running tally over the frames of a stream.
struct StreamTally {
    std::uint64_t frames = 0;
    std::uint64_t refused = 0;       // frames the decoder refused (any verdict)
    std::uint64_t badSync = 0;
    std::uint64_t samples = 0;
    std::array<std::uint64_t, 4> adcOverflowFrames{};  // frames whose address-4 block reported overflow
    bool hasSoftwareSerial = false;
    std::uint8_t softwareSerial = 0;
};

// The decoder of a data packet (two frames). Thread rule: used from the reader thread only; the receiver
// count and selection are set from the control thread through setters that are plain atomic stores.
class Protocol1Decoder {
public:
    // False (and nothing changes) outside 1..8, or `deliver` outside 1..receivers.
    bool configure(int receivers, int deliver, int rateHz);
    int receivers() const { return receivers_; }
    int deliver() const { return deliver_; }
    int rateHz() const { return rateHz_; }

    // Both frames of a 1024-byte data packet. All-or-nothing: if either frame is refused nothing is
    // appended and false is returned.
    bool decodePacket(const std::uint8_t* frames1024, std::vector<std::complex<float>>& out);
    FrameVerdict lastVerdict() const { return last_; }
    const StreamTally& tally() const { return tally_; }
    void resetTally() { tally_ = StreamTally{}; }

private:
    int receivers_ = 1, deliver_ = 1, rateHz_ = 48000;
    FrameVerdict last_ = FrameVerdict::Ok;
    StreamTally tally_;
};

// ---- send: PC to device (the C&C carrier; no audio, no transmit) ---------------------------------

// One command-and-control quintuple.
using Cc = std::array<std::uint8_t, 5>;

struct ControlSettings {
    int rateHz = 48000;                   // 48000, 96000, 192000, 384000
    int receivers = 1;                    // 1..7 tuned (8 can be unpacked, not tuned)
    std::array<std::uint32_t, kMaxTunedReceivers> rxFreqHz{};  // NCO frequency of receivers 1..7, in Hz
    // Address 0 hardware-configuration bits. The document names them; the right values depend on the
    // boards in the box and are not guessed: all zero by default (zero is the document's first named
    // value of each). "Ignored by Hermes" is stated for the three clock fields.
    std::uint8_t tenMhzRef = 0;           // C1 bits 3..2: 00 Atlas/Excalibur, 01 Penelope, 10 Mercury
    std::uint8_t clock122 = 0;            // C1 bit 4: 0 Penelope, 1 Mercury
    std::uint8_t config = 0;              // C1 bits 6..5: 00 nil, 01 Penelope, 10 Mercury, 11 both
    // Input attenuation of ADC 1, address 0x0A C4: 0..31 dB, enabled only when attenuatorEnable.
    bool attenuatorEnable = false;
    int attenuatorDb = 0;
    bool preamp = false;                  // address 0 C3 bit 2 ("Preamp On/Off") and address 0x0A C1 bit 0
};

// A sentence for the first problem, or empty when the settings are encodable. receivers 8 and a rate
// outside the four are refused; a frequency above 2^32-1 cannot be represented (a double check is made by
// the caller; this takes the unsigned values).
std::string validateSettings(const ControlSettings& s);

// Every C&C quintuple one full cycle of the settings needs, in the order they are sent: address 0, then
// the NCO of receivers 1..n, then address 0x0A. MOX (C0 bit 0) is always 0; no drive-level, CW or
// transmit-side address is ever produced.
std::vector<Cc> controlCycle(const ControlSettings& s);

// A 512-byte PC-to-device frame: sync, `cc`, and 63 groups of eight zero bytes (silent audio, no I/Q).
std::array<std::uint8_t, 512> buildControlFrame(const Cc& cc);

// Round-robin sender state: a cycle position and the end-point-2 sequence number.
class ControlSender {
public:
    // False with `why` when the settings are not encodable (the old ones stay).
    bool set(const ControlSettings& s, std::string& why);
    const ControlSettings& settings() const { return settings_; }
    // The sequence number restarts at zero at each Start (the Metis guide).
    void restart() { seq_ = 0; pos_ = 0; }
    // The next 1032-byte end-point-2 packet: two frames, the next two C&C quintuples of the cycle.
    std::vector<std::uint8_t> nextPacket();
    // A packet count that covers one full cycle (so every setting goes out once).
    std::size_t packetsPerCycle() const;
    std::uint32_t sequence() const { return seq_; }

private:
    ControlSettings settings_;
    std::vector<Cc> cycle_ = controlCycle(ControlSettings{});
    std::size_t pos_ = 0;
    std::uint32_t seq_ = 0;
};

}  // namespace cascade::source::hpsdr
