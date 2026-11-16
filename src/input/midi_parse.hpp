// midi_parse.hpp - MIDI 1.0 bytes and short messages to InputEvents. PURE: no Windows header, no thread, no clock (the caller passes the time).
//
// SOURCES (all cited in docs/feature-records/knob-input.md): the MIDI 1.0 Detailed Specification (message set, running status, real-time bytes inside
// messages, note-on velocity 0 = note off, CC 0-31 MSB / 32-63 LSB pairs, pitch bend LSB first); Microsoft's MIM_DATA page (the packed layout winmm hands
// a callback, running status already expanded). The RELATIVE encodings are a controller-vendor convention, not part of the MIDI spec; their sign
// direction is UNVERIFIED against a real controller, so every control has an `invert` flag.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "input_event.hpp"

namespace cascade::input {

enum class MidiEncoding : std::uint8_t {
    Absolute7 = 0,          // CC value / 127
    Absolute14 = 1,         // CC n (0..31) MSB + CC n+32 LSB, reported on the MSB's control
    RelTwosComplement = 2,  // 1..63 = +1..+63, 127..65 = -1..-63, 64 = -64, 0 = none
    RelSignedBit = 3,       // bit 6 = negative, bits 0-5 magnitude (sign direction UNVERIFIED)
    RelBinaryOffset = 4     // value - 64
};

const char* midiEncodingName(MidiEncoding e);  // "absolute7", "absolute14", "twos_complement", "signed_bit", "binary_offset"
bool midiEncodingFromName(const std::string& s, MidiEncoding& out);
bool midiEncodingIsRelative(MidiEncoding e);

// Signed detents for a 7-bit value under a relative encoding (0 for the absolute encodings or v > 127).
int decodeRelative(MidiEncoding e, unsigned v7);

// Control ids (16 bits): kind in the top nibble, channel (0-15) in bits 8-11, note / controller number in the low byte.
constexpr std::uint16_t midiNoteControl(unsigned channel, unsigned note) { return static_cast<std::uint16_t>(0x1000u | ((channel & 15u) << 8) | (note & 127u)); }
constexpr std::uint16_t midiCcControl(unsigned channel, unsigned cc) { return static_cast<std::uint16_t>(0x2000u | ((channel & 15u) << 8) | (cc & 127u)); }
constexpr std::uint16_t midiBendControl(unsigned channel) { return static_cast<std::uint16_t>(0x3000u | ((channel & 15u) << 8)); }

struct MidiMessage {
    std::uint8_t status = 0;  // full status byte
    std::uint8_t d1 = 0, d2 = 0;
    std::uint8_t length = 0;  // bytes in the message including status (1..3)
};

// winmm's MIM_DATA dwParam1: status in the low byte, then data 1, data 2. False for a non-status first byte, a data byte above 0x7F, a system-exclusive
// or end-of-exclusive status (those arrive as MIM_LONGDATA, which this layer never asks for), or an undefined status.
bool unpackShortMessage(std::uint32_t packed, MidiMessage& out);

// Raw byte stream (USB-MIDI, a serial port, a test): running status, real-time bytes (0xF8-0xFF) in the middle of a message, system exclusive skipped.
class MidiStreamParser {
public:
    // Feed one byte; true when `out` holds a complete message. A real-time byte completes a one-byte message and leaves a message in progress alone.
    bool feed(std::uint8_t b, MidiMessage& out);
    void reset();

private:
    std::uint8_t running_ = 0;  // 0 or a channel status
    std::uint8_t status_ = 0;   // message in progress
    std::uint8_t need_ = 0, have_ = 0;
    std::uint8_t data_[2] = {0, 0};
    bool inSysex_ = false;
};

struct MidiControlConfig {
    MidiEncoding encoding = MidiEncoding::Absolute7;
    bool invert = false;  // negate a relative result
};

// One decoder per open port. handle() is called on the port's device thread; setControl()/clear() from any thread.
class MidiDecoder {
public:
    explicit MidiDecoder(std::uint32_t deviceId) : device_(deviceId) {}
    void setControl(std::uint16_t control, const MidiControlConfig& cfg);
    void clearControls();
    // Writes 0, 1 or 2 events to out[0..1]; returns how many. Unknown messages give 0.
    std::size_t handle(const MidiMessage& m, std::int64_t timeUs, InputEvent* out);

private:
    std::uint32_t device_;
    std::mutex m_;
    std::map<std::uint16_t, MidiControlConfig> cfg_;
    std::map<std::uint16_t, std::uint8_t> msb_, lsb_;  // 14-bit pairs by MSB control id
};

// "Learn": guess the encoding of one CC from the 7-bit values the knob sent while the user turned it both ways. Says how sure it is.
struct LearnResult {
    MidiEncoding encoding = MidiEncoding::Absolute7;
    double confidence = 0.0;  // 0..1
    int samples = 0;
    bool ambiguous = false;  // the data cannot tell two encodings apart (usually: only one turning direction was seen)
    const char* note = "";
};

class EncodingLearner {
public:
    void add(unsigned v7);  // values above 127 are ignored
    void reset();
    LearnResult guess() const;

private:
    int count_[128] = {};
    int total_ = 0;
};

}  // namespace cascade::input
