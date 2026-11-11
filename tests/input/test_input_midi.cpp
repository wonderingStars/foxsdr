// MIDI: the relative encodings, the packed-short unpacker, the byte-stream parser (running status, interleaved real-time, system exclusive), the decoder
// (notes, controllers absolute 7 / 14 bit and relative, pitch bend), the Learn helper, and a fuzz loop. Everything is injected bytes: no MIDI device is attached.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <vector>

#include "input/midi_parse.hpp"
#include "test_check.hpp"

using namespace cascade::input;

static std::vector<MidiMessage> parseAll(const std::vector<int>& bytes) {
    MidiStreamParser p;
    std::vector<MidiMessage> out;
    for (int b : bytes) {
        MidiMessage m;
        if (p.feed(static_cast<std::uint8_t>(b), m)) { out.push_back(m); }
    }
    return out;
}

static std::uint32_t pack(int s, int d1, int d2) { return static_cast<std::uint32_t>(s | (d1 << 8) | (d2 << 16)); }

int main() {
    // ---- RELATIVE ENCODINGS, every 7-bit value that matters ---------------------------------------------------------------------------------------
    CHECK(decodeRelative(MidiEncoding::RelTwosComplement, 0) == 0);
    CHECK(decodeRelative(MidiEncoding::RelTwosComplement, 1) == 1);
    CHECK(decodeRelative(MidiEncoding::RelTwosComplement, 63) == 63);
    CHECK(decodeRelative(MidiEncoding::RelTwosComplement, 127) == -1);
    CHECK(decodeRelative(MidiEncoding::RelTwosComplement, 126) == -2);
    CHECK(decodeRelative(MidiEncoding::RelTwosComplement, 65) == -63);
    CHECK(decodeRelative(MidiEncoding::RelTwosComplement, 64) == -64);
    CHECK(decodeRelative(MidiEncoding::RelSignedBit, 0) == 0);
    CHECK(decodeRelative(MidiEncoding::RelSignedBit, 1) == 1);
    CHECK(decodeRelative(MidiEncoding::RelSignedBit, 63) == 63);
    CHECK(decodeRelative(MidiEncoding::RelSignedBit, 65) == -1);
    CHECK(decodeRelative(MidiEncoding::RelSignedBit, 127) == -63);
    CHECK(decodeRelative(MidiEncoding::RelSignedBit, 64) == 0);  // negative zero
    CHECK(decodeRelative(MidiEncoding::RelBinaryOffset, 64) == 0);
    CHECK(decodeRelative(MidiEncoding::RelBinaryOffset, 65) == 1);
    CHECK(decodeRelative(MidiEncoding::RelBinaryOffset, 63) == -1);
    CHECK(decodeRelative(MidiEncoding::RelBinaryOffset, 0) == -64);
    CHECK(decodeRelative(MidiEncoding::RelBinaryOffset, 127) == 63);
    CHECK(decodeRelative(MidiEncoding::Absolute7, 5) == 0);
    CHECK(decodeRelative(MidiEncoding::RelTwosComplement, 128) == 0);  // not a 7-bit value
    for (MidiEncoding e : {MidiEncoding::Absolute7, MidiEncoding::Absolute14, MidiEncoding::RelTwosComplement, MidiEncoding::RelSignedBit, MidiEncoding::RelBinaryOffset}) {
        MidiEncoding back = MidiEncoding::RelBinaryOffset;
        CHECK(midiEncodingFromName(midiEncodingName(e), back) && back == e);
    }
    {
        MidiEncoding x;
        CHECK(!midiEncodingFromName("nonsense", x));
    }

    // ---- PACKED SHORT MESSAGES (winmm MIM_DATA) ---------------------------------------------------------------------------------------------------
    {
        MidiMessage m;
        CHECK(unpackShortMessage(pack(0xB2, 7, 100), m) && m.status == 0xB2 && m.d1 == 7 && m.d2 == 100 && m.length == 3);
        CHECK(unpackShortMessage(pack(0xC1, 5, 0), m) && m.status == 0xC1 && m.d1 == 5 && m.length == 2);
        CHECK(unpackShortMessage(pack(0xF8, 0, 0), m) && m.length == 1);
        CHECK(!unpackShortMessage(pack(0x40, 1, 2), m));        // not a status byte
        CHECK(!unpackShortMessage(pack(0x90, 0x80, 1), m));     // data byte with the high bit
        CHECK(!unpackShortMessage(pack(0x90, 1, 0xFF), m));
        CHECK(!unpackShortMessage(pack(0xF0, 1, 2), m));        // system exclusive never arrives as a short message
        CHECK(!unpackShortMessage(pack(0xF7, 0, 0), m));
        CHECK(!unpackShortMessage(pack(0xF4, 0, 0), m));        // undefined
        CHECK(unpackShortMessage(pack(0xC1, 5, 0xFF), m));      // the unused second byte of a 2-byte message is ignored
    }

    // ---- STREAM PARSER ----------------------------------------------------------------------------------------------------------------------------
    {   // running status: one status byte, three messages
        auto v = parseAll({0xB0, 10, 20, 11, 21, 12, 22});
        CHECK(v.size() == 3 && v[0].d1 == 10 && v[1].d1 == 11 && v[2].d1 == 12 && v[2].d2 == 22 && v[2].status == 0xB0);
    }
    {   // a real-time byte in the middle of a message does not break it, and is itself reported
        auto v = parseAll({0xB0, 10, 0xF8, 20});
        CHECK(v.size() == 2 && v[0].status == 0xF8 && v[1].status == 0xB0 && v[1].d1 == 10 && v[1].d2 == 20);
    }
    {   // real-time between running-status messages keeps running status
        auto v = parseAll({0x90, 60, 100, 0xFE, 61, 0});
        CHECK(v.size() == 3 && v[1].status == 0xFE && v[2].status == 0x90 && v[2].d1 == 61 && v[2].d2 == 0);
    }
    {   // one-data-byte messages (program change) under running status
        auto v = parseAll({0xC3, 5, 6});
        CHECK(v.size() == 2 && v[0].d1 == 5 && v[1].d1 == 6 && v[1].length == 2);
    }
    {   // system exclusive is skipped whole, and cancels running status
        auto v = parseAll({0x90, 60, 100, 0xF0, 1, 2, 3, 0xF7, 61, 100});
        CHECK(v.size() == 1);  // the 61,100 after F7 has no running status
    }
    {   // a new status in the middle abandons the incomplete message
        auto v = parseAll({0xB0, 10, 0x90, 60, 100});
        CHECK(v.size() == 1 && v[0].status == 0x90 && v[0].d1 == 60);
    }
    {   // truncated: a lone status, data with no status at all, a half message
        CHECK(parseAll({0xB0}).empty());
        CHECK(parseAll({1, 2, 3, 4}).empty());
        CHECK(parseAll({0xB0, 5}).empty());
        CHECK(parseAll({0xF0, 0xF7}).empty());
    }
    {   // system common: F2 takes two data bytes, F6 none; neither leaves running status
        auto v = parseAll({0xF2, 1, 2, 0xF6, 5, 6});
        CHECK(v.size() == 2 && v[0].status == 0xF2 && v[0].d2 == 2 && v[1].status == 0xF6);
    }
    {   // reset() forgets a half message
        MidiStreamParser p;
        MidiMessage m;
        p.feed(0xB0, m);
        p.feed(7, m);
        p.reset();
        CHECK(!p.feed(9, m));
    }

    // ---- DECODER ----------------------------------------------------------------------------------------------------------------------------------
    MidiDecoder dec(42);
    auto run = [&](int s, int d1, int d2, InputEvent* out) { return dec.handle(MidiMessage{static_cast<std::uint8_t>(s), static_cast<std::uint8_t>(d1), static_cast<std::uint8_t>(d2), 3}, 1234, out); };
    InputEvent o[2];
    {   // notes
        CHECK(run(0x91, 60, 127, o) == 1 && o[0].kind == EventKind::ButtonDown && o[0].control == midiNoteControl(1, 60) && o[0].value == 1.0f && o[0].device == 42 && o[0].timeUs == 1234);
        CHECK(run(0x91, 60, 0, o) == 1 && o[0].kind == EventKind::ButtonUp);  // velocity 0 = off
        CHECK(run(0x81, 60, 64, o) == 1 && o[0].kind == EventKind::ButtonUp);
    }
    {   // absolute 7-bit (the default)
        CHECK(run(0xB0, 21, 127, o) == 1 && o[0].kind == EventKind::Absolute && o[0].value == 1.0f && o[0].raw == 127 && o[0].control == midiCcControl(0, 21));
        CHECK(run(0xB0, 21, 0, o) == 1 && o[0].value == 0.0f);
    }
    {   // each relative encoding through the decoder, with invert
        dec.setControl(midiCcControl(0, 30), {MidiEncoding::RelTwosComplement, false});
        CHECK(run(0xB0, 30, 3, o) == 1 && o[0].kind == EventKind::Relative && o[0].raw == 3);
        CHECK(run(0xB0, 30, 125, o) == 1 && o[0].raw == -3);
        CHECK(run(0xB0, 30, 0, o) == 0);  // no movement: no event
        dec.setControl(midiCcControl(0, 31), {MidiEncoding::RelBinaryOffset, true});
        CHECK(run(0xB0, 31, 66, o) == 1 && o[0].raw == -2);  // +2 inverted
        CHECK(run(0xB0, 31, 64, o) == 0);
        dec.setControl(midiCcControl(2, 9), {MidiEncoding::RelSignedBit, false});
        CHECK(run(0xB2, 9, 0x41, o) == 1 && o[0].raw == -1);
        CHECK(run(0xB0, 9, 0x41, o) == 1 && o[0].kind == EventKind::Absolute);  // channel 0 is a different control
    }
    {   // 14-bit pair: MSB cc 1 and LSB cc 33
        dec.setControl(midiCcControl(0, 1), {MidiEncoding::Absolute14, false});
        CHECK(run(0xB0, 1, 127, o) == 1 && o[0].control == midiCcControl(0, 1) && o[0].raw == (127 << 7));
        CHECK(run(0xB0, 33, 127, o) == 1 && o[0].control == midiCcControl(0, 1) && o[0].raw == 16383 && o[0].value == 1.0f);  // reported on the MSB's control
        CHECK(run(0xB0, 1, 0, o) == 1 && o[0].raw == 127);  // MSB 0, LSB still 127
        CHECK(run(0xB0, 33, 0, o) == 1 && o[0].raw == 0 && o[0].value == 0.0f);
        CHECK(run(0xB0, 34, 5, o) == 1 && o[0].control == midiCcControl(0, 34));  // an LSB with no configured pair is an ordinary controller
    }
    {   // pitch bend, LSB first
        CHECK(run(0xE0, 0, 64, o) == 1 && o[0].control == midiBendControl(0) && o[0].raw == 8192 && o[0].kind == EventKind::Absolute);
        CHECK(run(0xE5, 127, 127, o) == 1 && o[0].raw == 16383 && o[0].value == 1.0f && o[0].control == midiBendControl(5));
        CHECK(run(0xE0, 0, 0, o) == 1 && o[0].raw == 0);
    }
    {   // things that are not knobs
        CHECK(run(0xC0, 5, 0, o) == 0);
        CHECK(run(0xA0, 60, 10, o) == 0);
        CHECK(run(0xD0, 60, 0, o) == 0);
        CHECK(run(0xB0, 123, 0, o) == 0);  // all notes off
        CHECK(run(0xF8, 0, 0, o) == 0);
        CHECK(run(0x40, 0, 0, o) == 0);
    }
    dec.clearControls();
    CHECK(run(0xB0, 30, 3, o) == 1 && o[0].kind == EventKind::Absolute);

    // ---- LEARN ------------------------------------------------------------------------------------------------------------------------------------
    {
        EncodingLearner l;
        CHECK(l.guess().confidence == 0.0);  // nothing yet
        for (int v : {1, 1, 2, 127, 126, 127, 1}) { l.add(v); }
        LearnResult r = l.guess();
        CHECK(r.encoding == MidiEncoding::RelTwosComplement && r.confidence > 0.9 && !r.ambiguous && r.samples == 7);
    }
    {
        EncodingLearner l;
        for (int v : {65, 65, 66, 63, 62, 63, 65}) { l.add(v); }
        LearnResult r = l.guess();
        CHECK(r.encoding == MidiEncoding::RelBinaryOffset && r.confidence > 0.9 && !r.ambiguous);
    }
    {
        EncodingLearner l;
        for (int v : {1, 1, 2, 65, 66, 65, 1}) { l.add(v); }
        LearnResult r = l.guess();
        CHECK(r.encoding == MidiEncoding::RelSignedBit && r.confidence > 0.85 && !r.ambiguous);
    }
    {   // one direction only: ambiguous, low confidence, says so
        EncodingLearner l;
        for (int v : {1, 1, 2, 1, 1}) { l.add(v); }
        LearnResult r = l.guess();
        CHECK(r.ambiguous && r.confidence < 0.5);
        EncodingLearner m;
        for (int v : {65, 65, 66, 65, 65}) { m.add(v); }
        CHECK(m.guess().ambiguous && m.guess().confidence < 0.5);
        EncodingLearner n;
        for (int v : {127, 127, 126, 127}) { n.add(v); }
        CHECK(n.guess().encoding == MidiEncoding::RelTwosComplement && n.guess().confidence > 0.5 && n.guess().ambiguous);
    }
    {   // an absolute sweep
        EncodingLearner l;
        for (int v = 0; v < 128; v += 3) { l.add(v); }
        LearnResult r = l.guess();
        CHECK(r.encoding == MidiEncoding::Absolute7 && r.confidence > 0.9);
    }
    {   // too few samples; values above 127 ignored; reset
        EncodingLearner l;
        l.add(1); l.add(2); l.add(300);
        CHECK(l.guess().samples == 2 && l.guess().confidence == 0.0);
        l.reset();
        CHECK(l.guess().samples == 0);
    }

    // ---- FUZZ: random bytes through the stream parser and the decoder; no crash, every message well formed, every event in range ---------------------
    {
        std::uint64_t x = 0x9E3779B97F4A7C15ull;
        auto rnd = [&] { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
        MidiStreamParser p;
        MidiDecoder d(1);
        d.setControl(midiCcControl(0, 1), {MidiEncoding::Absolute14, false});
        d.setControl(midiCcControl(0, 2), {MidiEncoding::RelSignedBit, false});
        d.setControl(midiCcControl(0, 3), {MidiEncoding::RelTwosComplement, true});
        bool wellFormed = true, inRange = true;
        long events = 0;
        for (int i = 0; i < 2000000; ++i) {
            const std::uint8_t b = static_cast<std::uint8_t>(rnd());
            MidiMessage m;
            if (p.feed(b, m)) {
                if (m.status < 0x80 || m.d1 > 127 || m.d2 > 127 || m.length < 1 || m.length > 3) { wellFormed = false; }
                InputEvent ev[2];
                const std::size_t n = d.handle(m, i, ev);
                if (n > 2) { inRange = false; }
                for (std::size_t k = 0; k < n; ++k) {
                    ++events;
                    const InputEvent& e = ev[k];
                    if (e.kind == EventKind::Absolute && !(e.value >= 0.0f && e.value <= 1.0f)) { inRange = false; }
                    if (e.kind == EventKind::Relative && (e.raw == 0 || e.raw < -64 || e.raw > 64)) { inRange = false; }
                    if (e.kind == EventKind::Disconnected) { inRange = false; }
                }
            }
            MidiMessage u;
            if (unpackShortMessage(static_cast<std::uint32_t>(rnd()), u)) {
                if (u.status < 0x80 || u.d1 > 127 || u.d2 > 127) { wellFormed = false; }
            }
        }
        CHECK(wellFormed);
        CHECK(inRange);
        std::printf("note: midi fuzz: 2000000 bytes, %ld events\n", events);
        CHECK(events > 1000);
    }
    return testSummary("test_input_midi");
}
