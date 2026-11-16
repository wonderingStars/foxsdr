// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "midi_parse.hpp"

#include <algorithm>

namespace cascade::input {

const char* midiEncodingName(MidiEncoding e) {
    switch (e) {
        case MidiEncoding::Absolute7: return "absolute7";
        case MidiEncoding::Absolute14: return "absolute14";
        case MidiEncoding::RelTwosComplement: return "twos_complement";
        case MidiEncoding::RelSignedBit: return "signed_bit";
        case MidiEncoding::RelBinaryOffset: return "binary_offset";
    }
    return "absolute7";
}

bool midiEncodingFromName(const std::string& s, MidiEncoding& out) {
    for (MidiEncoding e : {MidiEncoding::Absolute7, MidiEncoding::Absolute14, MidiEncoding::RelTwosComplement, MidiEncoding::RelSignedBit,
                           MidiEncoding::RelBinaryOffset}) {
        if (s == midiEncodingName(e)) { out = e; return true; }
    }
    return false;
}

bool midiEncodingIsRelative(MidiEncoding e) {
    return e == MidiEncoding::RelTwosComplement || e == MidiEncoding::RelSignedBit || e == MidiEncoding::RelBinaryOffset;
}

int decodeRelative(MidiEncoding e, unsigned v) {
    if (v > 127) { return 0; }
    switch (e) {
        case MidiEncoding::RelTwosComplement: return v < 64 ? static_cast<int>(v) : static_cast<int>(v) - 128;
        case MidiEncoding::RelSignedBit: {
            const int mag = static_cast<int>(v & 0x3F);
            return (v & 0x40) ? -mag : mag;
        }
        case MidiEncoding::RelBinaryOffset: return static_cast<int>(v) - 64;
        default: return 0;
    }
}

namespace {
// Data bytes a status takes; -1 for undefined / not handled here (F0, F7 and the undefined F4, F5, F9, FD).
int dataBytesFor(std::uint8_t s) {
    if (s >= 0x80 && s < 0xF0) {
        const std::uint8_t hi = s & 0xF0;
        return (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
    }
    switch (s) {
        case 0xF1: case 0xF3: return 1;
        case 0xF2: return 2;
        case 0xF6: return 0;
        case 0xF8: case 0xFA: case 0xFB: case 0xFC: case 0xFE: case 0xFF: return 0;
        default: return -1;
    }
}
}  // namespace

bool unpackShortMessage(std::uint32_t packed, MidiMessage& out) {
    const std::uint8_t s = static_cast<std::uint8_t>(packed & 0xFF);
    const std::uint8_t d1 = static_cast<std::uint8_t>((packed >> 8) & 0xFF);
    const std::uint8_t d2 = static_cast<std::uint8_t>((packed >> 16) & 0xFF);
    const int n = dataBytesFor(s);
    if (n < 0) { return false; }
    if (n >= 1 && d1 > 0x7F) { return false; }
    if (n >= 2 && d2 > 0x7F) { return false; }
    out.status = s;
    out.d1 = n >= 1 ? d1 : 0;
    out.d2 = n >= 2 ? d2 : 0;
    out.length = static_cast<std::uint8_t>(1 + n);
    return true;
}

void MidiStreamParser::reset() { *this = MidiStreamParser(); }

bool MidiStreamParser::feed(std::uint8_t b, MidiMessage& out) {
    if (b >= 0xF8) {  // real-time: complete by itself, never disturbs the message in progress or running status
        if (dataBytesFor(b) < 0) { return false; }
        out = MidiMessage{b, 0, 0, 1};
        return true;
    }
    if (b & 0x80) {  // a status byte
        if (b == 0xF0) { inSysex_ = true; running_ = 0; need_ = 0; return false; }
        if (b == 0xF7) { inSysex_ = false; running_ = 0; need_ = 0; return false; }
        inSysex_ = false;  // any other status ends exclusive too
        const int n = dataBytesFor(b);
        if (n < 0) { running_ = 0; need_ = 0; return false; }
        running_ = b < 0xF0 ? b : std::uint8_t{0};  // system common cancels running status
        status_ = b;
        need_ = static_cast<std::uint8_t>(n);
        have_ = 0;
        if (n == 0) { out = MidiMessage{b, 0, 0, 1}; return true; }
        return false;
    }
    // a data byte
    if (inSysex_) { return false; }
    if (need_ == 0) {  // between messages: running status restarts one
        if (running_ == 0) { return false; }
        status_ = running_;
        need_ = static_cast<std::uint8_t>(dataBytesFor(running_));
        have_ = 0;
    }
    data_[have_++] = b;
    if (have_ < need_) { return false; }
    out = MidiMessage{status_, data_[0], need_ >= 2 ? data_[1] : std::uint8_t{0}, static_cast<std::uint8_t>(1 + need_)};
    need_ = 0;
    have_ = 0;
    return true;
}

void MidiDecoder::setControl(std::uint16_t control, const MidiControlConfig& cfg) {
    std::lock_guard<std::mutex> lk(m_);
    cfg_[control] = cfg;
}

void MidiDecoder::clearControls() {
    std::lock_guard<std::mutex> lk(m_);
    cfg_.clear();
    msb_.clear();
    lsb_.clear();
}

std::size_t MidiDecoder::handle(const MidiMessage& m, std::int64_t t, InputEvent* out) {
    if (m.status < 0x80 || m.status >= 0xF0) { return 0; }
    const unsigned hi = m.status & 0xF0, ch = m.status & 0x0F;
    InputEvent e;
    e.device = device_;
    e.timeUs = t;
    switch (hi) {
        case 0x80:
        case 0x90: {
            e.control = midiNoteControl(ch, m.d1 & 127);
            const bool on = hi == 0x90 && m.d2 != 0;  // MIDI 1.0: note-on with velocity 0 is note-off
            e.kind = on ? EventKind::ButtonDown : EventKind::ButtonUp;
            e.raw = m.d2;
            e.value = on ? static_cast<float>(m.d2) / 127.0f : 0.0f;
            out[0] = e;
            return 1;
        }
        case 0xE0: {
            const int v14 = (m.d1 & 127) | ((m.d2 & 127) << 7);  // LSB first
            e.control = midiBendControl(ch);
            e.kind = EventKind::Absolute;
            e.raw = v14;
            e.value = static_cast<float>(v14) / 16383.0f;
            out[0] = e;
            return 1;
        }
        case 0xB0: {
            const unsigned cc = m.d1 & 127, v = m.d2 & 127;
            if (cc >= 120) { return 0; }  // channel-mode messages (all sound off, reset controllers, ...), not knobs
            std::lock_guard<std::mutex> lk(m_);
            if (cc >= 32 && cc < 64) {  // the LSB half of a configured 14-bit pair
                const std::uint16_t msbId = midiCcControl(ch, cc - 32);
                const auto it = cfg_.find(msbId);
                if (it != cfg_.end() && it->second.encoding == MidiEncoding::Absolute14) {
                    lsb_[msbId] = static_cast<std::uint8_t>(v);
                    const int v14 = (msb_[msbId] << 7) | static_cast<int>(v);
                    e.control = msbId;
                    e.kind = EventKind::Absolute;
                    e.raw = v14;
                    e.value = static_cast<float>(v14) / 16383.0f;
                    out[0] = e;
                    return 1;
                }
            }
            e.control = midiCcControl(ch, cc);
            MidiControlConfig cfg;
            if (const auto it = cfg_.find(e.control); it != cfg_.end()) { cfg = it->second; }
            if (cfg.encoding == MidiEncoding::Absolute14 && cc < 32) {
                msb_[e.control] = static_cast<std::uint8_t>(v);
                const int v14 = (static_cast<int>(v) << 7) | lsb_[e.control];
                e.kind = EventKind::Absolute;
                e.raw = v14;
                e.value = static_cast<float>(v14) / 16383.0f;
                out[0] = e;
                return 1;
            }
            if (midiEncodingIsRelative(cfg.encoding)) {
                int d = decodeRelative(cfg.encoding, v);
                if (cfg.invert) { d = -d; }
                if (d == 0) { return 0; }
                e.kind = EventKind::Relative;
                e.raw = d;
                out[0] = e;
                return 1;
            }
            e.kind = EventKind::Absolute;  // Absolute7 (also a 14-bit config on a controller number >= 32, which has no pair)
            e.raw = static_cast<int>(v);
            e.value = static_cast<float>(v) / 127.0f;
            out[0] = e;
            return 1;
        }
        default: return 0;  // poly pressure, program change, channel pressure
    }
}

void EncodingLearner::add(unsigned v) {
    if (v > 127) { return; }
    ++count_[v];
    ++total_;
}

void EncodingLearner::reset() { *this = EncodingLearner(); }

LearnResult EncodingLearner::guess() const {
    LearnResult r;
    r.samples = total_;
    if (total_ < 4) {
        r.note = "not enough values: turn the control a few clicks each way";
        return r;
    }
    auto sum = [&](int a, int b) {
        int s = 0;
        for (int i = a; i <= b; ++i) { s += count_[i]; }
        return s;
    };
    const int nz = sum(1, 15), nh = sum(113, 127), nb = sum(48, 63), na = sum(65, 80), mid = sum(16, 47) + sum(81, 112);
    int distinct = 0;
    for (int i = 0; i < 128; ++i) { distinct += count_[i] > 0 ? 1 : 0; }
    if (mid * 4 > total_ || distinct > 20) {
        r.encoding = MidiEncoding::Absolute7;
        r.confidence = 0.6 + 0.4 * std::min(1.0, distinct / 40.0);
        r.note = "values sweep the whole range: an absolute control";
        return r;
    }
    if (nb > 0 && na > 0 && nz == 0 && nh == 0) {
        r.encoding = MidiEncoding::RelBinaryOffset; r.confidence = 0.95; r.note = "values on both sides of 64";
        return r;
    }
    if (nz > 0 && nh > 0 && nb == 0 && na == 0) {
        r.encoding = MidiEncoding::RelTwosComplement; r.confidence = 0.95; r.note = "small values and values near 127";
        return r;
    }
    if (nz > 0 && na > 0 && nh == 0 && nb == 0) {
        r.encoding = MidiEncoding::RelSignedBit; r.confidence = 0.9; r.note = "small values and values just above 64";
        return r;
    }
    r.ambiguous = true;
    if (nz > 0 && nh == 0 && nb == 0 && na == 0) {
        r.encoding = MidiEncoding::RelTwosComplement; r.confidence = 0.4;
        r.note = "one direction only (twos complement or signed bit): turn the other way";
        return r;
    }
    if (na > 0 && nz == 0 && nh == 0 && nb == 0) {
        r.encoding = MidiEncoding::RelBinaryOffset; r.confidence = 0.4;
        r.note = "one direction only (binary offset or signed bit): turn the other way";
        return r;
    }
    if (nh > 0 && nz == 0 && nb == 0 && na == 0) {
        r.encoding = MidiEncoding::RelTwosComplement; r.confidence = 0.6;
        r.note = "only values near 127: twos complement, turn the other way to confirm";
        return r;
    }
    if (nb > 0 && nz == 0 && nh == 0 && na == 0) {
        r.encoding = MidiEncoding::RelBinaryOffset; r.confidence = 0.6;
        r.note = "only values just below 64: binary offset, turn the other way to confirm";
        return r;
    }
    r.encoding = MidiEncoding::RelTwosComplement;
    r.confidence = 0.1;
    r.note = "values fit no single encoding";
    return r;
}

}  // namespace cascade::input
