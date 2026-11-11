// The two HID knob report parsers as pure functions of bytes: the ShuttleXpress (ring, 8-bit jog counter with wrap, five buttons) and the PowerMate
// (push, signed turn), on hand-built reports including truncated and malformed ones, plus a fuzz loop. NO DEVICE IS ATTACHED: the byte layouts are the
// UNVERIFIED community-sourced ones documented in hid_parse.hpp, so these tests prove the parsers do what that layout says, not that a real unit sends it.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <vector>

#include "input/hid_parse.hpp"
#include "test_check.hpp"

using namespace cascade::input;

static HidParseResult feed(ShuttleXpressParser& p, std::initializer_list<int> bytes, ParsedEvents& pe) {
    std::vector<std::uint8_t> v;
    for (int b : bytes) { v.push_back(static_cast<std::uint8_t>(b)); }
    return p.parse(v.data(), v.size(), 99, pe);
}

static int count(const ParsedEvents& pe, std::uint16_t control, EventKind k) {
    int n = 0;
    for (std::size_t i = 0; i < pe.n; ++i) { n += (pe.e[i].control == control && pe.e[i].kind == k) ? 1 : 0; }
    return n;
}

int main() {
    // ---- PRODUCT TABLE ----------------------------------------------------------------------------------------------------------------------------
    CHECK(findHidProduct(0x0B33, 0x0020) && findHidProduct(0x0B33, 0x0020)->kind == HidKind::ShuttleXpress);
    CHECK(findHidProduct(0x077D, 0x0410) && findHidProduct(0x077D, 0x0410)->kind == HidKind::PowerMate);
    CHECK(findHidProduct(0x0B33, 0x0410) == nullptr);
    CHECK(findHidProduct(0, 0) == nullptr);
    CHECK(stableHidId(0x0B33, 0x0020, 0) == "hid:0b33:0020");
    CHECK(stableHidId(0x077D, 0x0410, 1) == "hid:077d:0410#2");
    CHECK(stableHidId(0x0B33, 0x0020, 2) == "hid:0b33:0020#3");

    // ---- SHUTTLEXPRESS ----------------------------------------------------------------------------------------------------------------------------
    {
        ShuttleXpressParser p(5);
        ParsedEvents pe;
        // The first report sets the jog baseline only: no jog event however large the counter.
        CHECK(feed(p, {0, 200, 0, 0, 0}, pe) == HidParseResult::Ok && pe.n == 0);
        // A step forward and a step back.
        CHECK(feed(p, {0, 201, 0, 0, 0}, pe) == HidParseResult::Ok && pe.n == 1 && pe.e[0].kind == EventKind::Relative && pe.e[0].raw == 1 && pe.e[0].control == kJogControl);
        CHECK(pe.e[0].device == 5 && pe.e[0].timeUs == 99);
        CHECK(feed(p, {0, 200, 0, 0, 0}, pe) == HidParseResult::Ok && pe.n == 1 && pe.e[0].raw == -1);
        // No change, no event.
        CHECK(feed(p, {0, 200, 0, 0, 0}, pe) == HidParseResult::Ok && pe.n == 0);
        // Several detents in one report.
        CHECK(feed(p, {0, 207, 0, 0, 0}, pe) == HidParseResult::Ok && pe.n == 1 && pe.e[0].raw == 7);
    }
    {   // WRAP-AROUND of the 8-bit counter, both ways
        ShuttleXpressParser p(5);
        ParsedEvents pe;
        feed(p, {0, 254, 0, 0, 0}, pe);
        feed(p, {0, 255, 0, 0, 0}, pe);
        CHECK(pe.n == 1 && pe.e[0].raw == 1);
        feed(p, {0, 0, 0, 0, 0}, pe);   // 255 -> 0
        CHECK(pe.n == 1 && pe.e[0].raw == 1);
        feed(p, {0, 255, 0, 0, 0}, pe);  // 0 -> 255 is one step BACK
        CHECK(pe.n == 1 && pe.e[0].raw == -1);
        feed(p, {0, 2, 0, 0, 0}, pe);   // 255 -> 2: three steps forward across the wrap
        CHECK(pe.n == 1 && pe.e[0].raw == 3);
        feed(p, {0, 250, 0, 0, 0}, pe);  // 2 -> 250: eight steps back across the wrap
        CHECK(pe.n == 1 && pe.e[0].raw == -8);
    }
    {   // SHUTTLE RING: absolute, spring return, raw -7..7 maps to 0..1
        ShuttleXpressParser p(5);
        ParsedEvents pe;
        feed(p, {0, 0, 0, 0, 0}, pe);
        CHECK(pe.n == 0);  // centre at start: nothing to say
        CHECK(feed(p, {3, 0, 0, 0, 0}, pe) == HidParseResult::Ok && pe.n == 1 && pe.e[0].kind == EventKind::Absolute && pe.e[0].control == kRingControl && pe.e[0].raw == 3);
        CHECK(pe.e[0].value > 0.71f && pe.e[0].value < 0.72f);
        feed(p, {7, 0, 0, 0, 0}, pe);
        CHECK(pe.n == 1 && pe.e[0].raw == 7 && pe.e[0].value == 1.0f);
        feed(p, {static_cast<int>(static_cast<std::uint8_t>(-7)), 0, 0, 0, 0}, pe);
        CHECK(pe.n == 1 && pe.e[0].raw == -7 && pe.e[0].value == 0.0f);
        feed(p, {0, 0, 0, 0, 0}, pe);  // springs back to centre
        CHECK(pe.n == 1 && pe.e[0].raw == 0 && pe.e[0].value == 0.5f);
        feed(p, {0, 0, 0, 0, 0}, pe);
        CHECK(pe.n == 0);
    }
    {   // BUTTONS: 1-4 are byte 3 bits 4-7, button 5 is byte 4 bit 0; each edge once
        ShuttleXpressParser p(5);
        ParsedEvents pe;
        feed(p, {0, 0, 0, 0x00, 0x00}, pe);
        for (int i = 0; i < 4; ++i) {
            CHECK(feed(p, {0, 0, 0, 0x10 << i, 0}, pe) == HidParseResult::Ok);
            CHECK(count(pe, static_cast<std::uint16_t>(kButtonBase + i), EventKind::ButtonDown) == 1);
            // (i > 0: the previous button's release is reported too)
            CHECK(pe.n == (i == 0 ? 1u : 2u));
        }
        CHECK(feed(p, {0, 0, 0, 0x00, 0x01}, pe) == HidParseResult::Ok);
        CHECK(count(pe, kButtonBase + 4, EventKind::ButtonDown) == 1 && count(pe, kButtonBase + 3, EventKind::ButtonUp) == 1);
        CHECK(feed(p, {0, 0, 0, 0x00, 0x01}, pe) == HidParseResult::Ok && pe.n == 0);  // held: no repeat
        CHECK(feed(p, {0, 0, 0, 0x00, 0x00}, pe) == HidParseResult::Ok && count(pe, kButtonBase + 4, EventKind::ButtonUp) == 1);
        // Two buttons changing in ONE report both come out.
        CHECK(feed(p, {0, 0, 0, 0x30, 0x01}, pe) == HidParseResult::Ok && pe.n == 3);
        // The reserved low nibble of byte 3 and the high bits of byte 4 are ignored.
        CHECK(feed(p, {0, 0, 0, 0x0F, 0xFE}, pe) == HidParseResult::Ok);
        CHECK(count(pe, kButtonBase + 0, EventKind::ButtonUp) == 1 && count(pe, kButtonBase + 1, EventKind::ButtonUp) == 1 && count(pe, kButtonBase + 4, EventKind::ButtonUp) == 1 &&
              pe.n == 3);
    }
    {   // ALL AT ONCE: a press, a turn and a ring move in one report, in that order
        ShuttleXpressParser p(5);
        ParsedEvents pe;
        feed(p, {0, 10, 0, 0, 0}, pe);
        feed(p, {2, 12, 0, 0x20, 0}, pe);
        CHECK(pe.n == 3 && pe.e[0].kind == EventKind::ButtonDown && pe.e[1].kind == EventKind::Relative && pe.e[1].raw == 2 && pe.e[2].kind == EventKind::Absolute);
    }
    {   // TRUNCATED and MALFORMED: refused, no event, and the state is untouched (the next good report is judged against the last GOOD one)
        ShuttleXpressParser p(5);
        ParsedEvents pe;
        feed(p, {0, 100, 0, 0, 0}, pe);
        for (int len = 0; len < 5; ++len) {
            std::vector<std::uint8_t> v(static_cast<std::size_t>(len), 0xFF);
            CHECK(p.parse(v.data(), v.size(), 1, pe) == HidParseResult::Truncated && pe.n == 0);
        }
        CHECK(p.parse(nullptr, 5, 1, pe) == HidParseResult::Truncated);
        CHECK(feed(p, {8, 150, 0, 0xF0, 1}, pe) == HidParseResult::Malformed && pe.n == 0);   // ring +8
        CHECK(feed(p, {0x80, 150, 0, 0xF0, 1}, pe) == HidParseResult::Malformed && pe.n == 0);  // ring -128
        CHECK(feed(p, {0xF8, 150, 0, 0xF0, 1}, pe) == HidParseResult::Malformed);              // ring -8
        feed(p, {0, 101, 0, 0, 0}, pe);
        CHECK(pe.n == 1 && pe.e[0].raw == 1);  // 100 -> 101, not 100 -> 150
        // Longer reports are fine; the extra bytes are ignored.
        CHECK(feed(p, {0, 102, 0, 0, 0, 9, 9, 9}, pe) == HidParseResult::Ok && pe.n == 1);
    }
    {   // reset() forgets the baseline (a reconnect)
        ShuttleXpressParser p(5);
        ParsedEvents pe;
        feed(p, {0, 10, 0, 0, 0}, pe);
        p.reset();
        feed(p, {0, 99, 0, 0, 0}, pe);
        CHECK(pe.n == 0);
    }

    // ---- POWERMATE --------------------------------------------------------------------------------------------------------------------------------
    {
        PowerMateParser p(6);
        ParsedEvents pe;
        std::uint8_t r1[6] = {0, 1, 0, 0, 0, 0};
        CHECK(p.parse(r1, 6, 5, pe) == HidParseResult::Ok && pe.n == 1 && pe.e[0].kind == EventKind::Relative && pe.e[0].raw == 1 && pe.e[0].control == kJogControl);
        std::uint8_t r2[6] = {0, 0xFD, 0, 0, 0, 0};
        CHECK(p.parse(r2, 6, 5, pe) == HidParseResult::Ok && pe.n == 1 && pe.e[0].raw == -3);
        std::uint8_t r3[6] = {0, 7, 0, 0, 0, 0};
        CHECK(p.parse(r3, 6, 5, pe) == HidParseResult::Ok && pe.e[0].raw == 7);
        std::uint8_t down[6] = {1, 0, 0, 0, 0, 0};
        CHECK(p.parse(down, 6, 5, pe) == HidParseResult::Ok && pe.n == 1 && pe.e[0].kind == EventKind::ButtonDown && pe.e[0].control == kButtonBase);
        CHECK(p.parse(down, 6, 5, pe) == HidParseResult::Ok && pe.n == 0);   // held
        std::uint8_t downTurn[6] = {1, 2, 0, 0, 0, 0};
        CHECK(p.parse(downTurn, 6, 5, pe) == HidParseResult::Ok && pe.n == 1 && pe.e[0].kind == EventKind::Relative && pe.e[0].raw == 2);   // turn while pressed
        std::uint8_t up[6] = {0, 0, 0, 0, 0, 0};
        CHECK(p.parse(up, 6, 5, pe) == HidParseResult::Ok && pe.n == 1 && pe.e[0].kind == EventKind::ButtonUp);
        CHECK(p.parse(up, 6, 5, pe) == HidParseResult::Ok && pe.n == 0);
        // press and turn in one report: press first
        std::uint8_t both[2] = {1, 1};
        CHECK(p.parse(both, 2, 5, pe) == HidParseResult::Ok && pe.n == 2 && pe.e[0].kind == EventKind::ButtonDown && pe.e[1].kind == EventKind::Relative);
        // truncated
        CHECK(p.parse(both, 1, 5, pe) == HidParseResult::Truncated && pe.n == 0);
        CHECK(p.parse(both, 0, 5, pe) == HidParseResult::Truncated);
        CHECK(p.parse(nullptr, 6, 5, pe) == HidParseResult::Truncated);
        // only bit 0 of byte 0 is the push
        std::uint8_t other[2] = {0xFE, 0};
        p.reset();
        CHECK(p.parse(other, 2, 5, pe) == HidParseResult::Ok && pe.n == 0);
    }

    // ---- FUZZ: random bytes of random length; no crash, no out-of-range event --------------------------------------------------------------------
    {
        std::uint64_t x = 0xD1B54A32D192ED03ull;
        auto rnd = [&] { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
        ShuttleXpressParser sp(1);
        PowerMateParser pp(2);
        bool ok = true;
        long events = 0;
        for (int i = 0; i < 1000000; ++i) {
            std::uint8_t buf[16];
            for (auto& b : buf) { b = static_cast<std::uint8_t>(rnd()); }
            const std::size_t len = rnd() % 17;
            ParsedEvents pe;
            const HidParseResult r1 = sp.parse(buf, len, i, pe);
            for (int pass = 0; pass < 2; ++pass) {
                if (r1 != HidParseResult::Ok && pass == 0 && pe.n != 0) { ok = false; }
                for (std::size_t k = 0; k < pe.n; ++k) {
                    const InputEvent& e = pe.e[k];
                    ++events;
                    if (e.kind == EventKind::Relative && (e.raw == 0 || e.raw < -128 || e.raw > 127)) { ok = false; }
                    if (e.kind == EventKind::Absolute && (e.raw < -7 || e.raw > 7 || e.value < 0.0f || e.value > 1.0f || e.control != kRingControl)) { ok = false; }
                    if ((e.kind == EventKind::ButtonDown || e.kind == EventKind::ButtonUp) && (e.control < kButtonBase || e.control > kButtonBase + 4)) { ok = false; }
                    if (e.kind == EventKind::Disconnected) { ok = false; }
                }
                if (pass == 0) {
                    const HidParseResult r2 = pp.parse(buf, len, i, pe);
                    if (r2 != HidParseResult::Ok && pe.n != 0) { ok = false; }
                    for (std::size_t k = 0; k < pe.n; ++k) {
                        const InputEvent& e = pe.e[k];
                        ++events;
                        if (e.kind == EventKind::Relative && e.control != kJogControl) { ok = false; }
                        if ((e.kind == EventKind::ButtonDown || e.kind == EventKind::ButtonUp) && e.control != kButtonBase) { ok = false; }
                        if (e.kind != EventKind::Relative && e.kind != EventKind::ButtonDown && e.kind != EventKind::ButtonUp) { ok = false; }
                    }
                    break;
                }
            }
        }
        CHECK(ok);
        std::printf("note: hid fuzz: 1000000 reports, %ld events\n", events);
        CHECK(events > 1000);
    }
    return testSummary("test_input_hid_parse");
}
