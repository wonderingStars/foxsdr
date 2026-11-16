// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "hid_parse.hpp"

#include <cstdio>

namespace cascade::input {

namespace {
constexpr HidProduct kProducts[] = {
    {0x0B33, 0x0020, HidKind::ShuttleXpress, "Contour ShuttleXpress"},
    {0x077D, 0x0410, HidKind::PowerMate, "Griffin PowerMate"},
};

InputEvent makeEvent(std::uint32_t dev, std::uint16_t control, EventKind k, std::int32_t raw, float value, std::int64_t t) {
    InputEvent e;
    e.device = dev;
    e.control = control;
    e.kind = k;
    e.raw = raw;
    e.value = value;
    e.timeUs = t;
    return e;
}
}  // namespace

const HidProduct* findHidProduct(std::uint16_t vendor, std::uint16_t product) {
    for (const HidProduct& p : kProducts) {
        if (p.vendor == vendor && p.product == product) { return &p; }
    }
    return nullptr;
}

std::string stableHidId(std::uint16_t vendor, std::uint16_t product, int unitIndex) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "hid:%04x:%04x", vendor, product);
    std::string s = buf;
    if (unitIndex > 0) { s += "#" + std::to_string(unitIndex + 1); }
    return s;
}

void ShuttleXpressParser::reset() {
    haveJog_ = false;
    jog_ = 0;
    ring_ = 0;
    buttons_ = 0;
}

HidParseResult ShuttleXpressParser::parse(const std::uint8_t* d, std::size_t len, std::int64_t t, ParsedEvents& out) {
    out.n = 0;
    if (d == nullptr || len < 5) { return HidParseResult::Truncated; }
    const std::int8_t ring = static_cast<std::int8_t>(d[0]);
    if (ring < -7 || ring > 7) { return HidParseResult::Malformed; }
    const std::uint8_t jog = d[1];
    const std::uint8_t buttons = static_cast<std::uint8_t>(((d[3] >> 4) & 0x0F) | ((d[4] & 0x01) << 4));

    // Button edges first (a press and a turn in one report keep their order), then the jog, then the ring.
    const std::uint8_t changed = static_cast<std::uint8_t>(buttons ^ buttons_);
    for (int i = 0; i < 5; ++i) {
        if (changed & (1u << i)) {
            const bool down = (buttons >> i) & 1u;
            out.e[out.n++] = makeEvent(device_, static_cast<std::uint16_t>(kButtonBase + i), down ? EventKind::ButtonDown : EventKind::ButtonUp, down ? 1 : 0,
                                       down ? 1.0f : 0.0f, t);
        }
    }
    if (haveJog_) {
        const int delta = static_cast<std::int8_t>(static_cast<std::uint8_t>(jog - jog_));  // 8-bit wrap
        if (delta != 0) { out.e[out.n++] = makeEvent(device_, kJogControl, EventKind::Relative, delta, 0.0f, t); }
    }
    if (ring != ring_) {
        out.e[out.n++] = makeEvent(device_, kRingControl, EventKind::Absolute, ring, 0.5f + static_cast<float>(ring) / 14.0f, t);
    }
    haveJog_ = true;
    jog_ = jog;
    ring_ = ring;
    buttons_ = buttons;
    return HidParseResult::Ok;
}

void PowerMateParser::reset() { pressed_ = false; }

HidParseResult PowerMateParser::parse(const std::uint8_t* d, std::size_t len, std::int64_t t, ParsedEvents& out) {
    out.n = 0;
    if (d == nullptr || len < 2) { return HidParseResult::Truncated; }
    const bool down = (d[0] & 1u) != 0;
    const int delta = static_cast<std::int8_t>(d[1]);
    if (down != pressed_) {
        out.e[out.n++] = makeEvent(device_, kButtonBase, down ? EventKind::ButtonDown : EventKind::ButtonUp, down ? 1 : 0, down ? 1.0f : 0.0f, t);
        pressed_ = down;
    }
    if (delta != 0) { out.e[out.n++] = makeEvent(device_, kJogControl, EventKind::Relative, delta, 0.0f, t); }
    return HidParseResult::Ok;
}

}  // namespace cascade::input
