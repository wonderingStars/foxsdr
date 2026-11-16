// hid_parse.hpp - the report layouts of the two HID knobs, as PURE functions of report bytes (no Windows header, no thread, no clock). Testable without hardware.
//
// EVERY LAYOUT HERE IS UNVERIFIED AGAINST A REAL DEVICE (none is attached to the development PC). Where the bytes come from, and how far each source can be
// trusted, is in docs/feature-records/knob-input.md section 2; in short: the Contour ShuttleXpress layout is community reverse engineering (a raw capture
// reported in AetherSDR pull request 5931, and an AutoHotkey forum thread), the Griffin PowerMate layout is the macOS driver README and the Linux
// driver's description. The vendors publish no byte-level document that was reachable. The report ID byte that Windows prepends to every ReadFile buffer
// is stripped by the reader (hid_win.cpp) BEFORE these functions see the bytes.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "input_event.hpp"

namespace cascade::input {

enum class HidKind : std::uint8_t { ShuttleXpress = 0, PowerMate = 1 };

struct HidProduct {
    std::uint16_t vendor;
    std::uint16_t product;
    HidKind kind;
    const char* name;
};

// The supported products (vendor / product ids: Contour 0x0B33:0x0020, Griffin 0x077D:0x0410), or null.
const HidProduct* findHidProduct(std::uint16_t vendor, std::uint16_t product);

// "hid:0b33:0020" for the first unit, "hid:0b33:0020#2" for a second one of the same product (order of enumeration; a serial number is never read).
std::string stableHidId(std::uint16_t vendor, std::uint16_t product, int unitIndex);

enum class HidParseResult : std::uint8_t {
    Ok = 0,
    Truncated = 1,  // shorter than the layout needs: no event, state untouched
    Malformed = 2   // a field outside its defined range: no event, state untouched
};

// Control ids. Buttons are kButtonBase + index (0-based: ShuttleXpress 0..4 = buttons 1..5, PowerMate 0 = the push).
constexpr std::uint16_t kJogControl = 1;     // ShuttleXpress inner wheel / PowerMate turn: Relative
constexpr std::uint16_t kRingControl = 2;    // ShuttleXpress outer ring: Absolute, raw -7..+7, value 0.5 + raw / 14
constexpr std::uint16_t kButtonBase = 0x10;

constexpr std::size_t kMaxEventsPerReport = 8;

struct ParsedEvents {
    InputEvent e[kMaxEventsPerReport];
    std::size_t n = 0;
};

// ShuttleXpress, 5 bytes (more are ignored):
//   [0] shuttle ring, signed, -7..+7 (0 = centre)      [1] jog counter, unsigned 8-bit, free-running (wraps 255 -> 0)
//   [2] not used (always 0 in the reported capture)    [3] bits 4-7 = buttons 1-4 (1 = down)     [4] bit 0 = button 5
// The first report only sets the jog baseline (the absolute counter means nothing until a second sample). Jog delta = (int8)(now - before), so the wrap
// is a +1 / -1 and never a 255 jump.
class ShuttleXpressParser {
public:
    explicit ShuttleXpressParser(std::uint32_t deviceId) : device_(deviceId) {}
    HidParseResult parse(const std::uint8_t* data, std::size_t len, std::int64_t timeUs, ParsedEvents& out);
    void reset();  // after a reconnect

private:
    std::uint32_t device_;
    bool haveJog_ = false;
    std::uint8_t jog_ = 0;
    std::int8_t ring_ = 0;
    std::uint8_t buttons_ = 0;  // 5 bits
};

// PowerMate, 6 bytes reported (2 are needed): [0] bit 0 = push (1 = down)   [1] signed rotation since the last report (clockwise positive, seen -7..+7).
class PowerMateParser {
public:
    explicit PowerMateParser(std::uint32_t deviceId) : device_(deviceId) {}
    HidParseResult parse(const std::uint8_t* data, std::size_t len, std::int64_t timeUs, ParsedEvents& out);
    void reset();

private:
    std::uint32_t device_;
    bool pressed_ = false;
};

}  // namespace cascade::input
