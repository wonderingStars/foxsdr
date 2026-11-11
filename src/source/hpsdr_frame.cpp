// hpsdr_frame.cpp - see hpsdr_frame.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/hpsdr_frame.hpp"

#include "source/hpsdr_protocol.hpp"

namespace cascade::source::hpsdr {

int groupsPerFrame(int receivers) {
    if (receivers < 1 || receivers > kMaxReceivers) { return 0; }
    return static_cast<int>(kFramePayloadBytes) / (6 * receivers + 2);
}

int paddingBytes(int receivers) {
    if (receivers < 1 || receivers > kMaxReceivers) { return 0; }
    return static_cast<int>(kFramePayloadBytes) % (6 * receivers + 2);
}

const std::array<int, 4>& supportedRates() {
    static const std::array<int, 4> r = {48000, 96000, 192000, 384000};
    return r;
}

bool rateToSpeedCode(int rateHz, std::uint8_t& code) {
    const auto& r = supportedRates();
    for (std::size_t i = 0; i < r.size(); ++i) {
        if (r[i] == rateHz) { code = static_cast<std::uint8_t>(i); return true; }
    }
    return false;
}

bool speedCodeToRate(std::uint8_t code, int& rateHz) {
    if (code > 3) { return false; }
    rateHz = supportedRates()[code];
    return true;
}

const char* verdictName(FrameVerdict v) {
    switch (v) {
        case FrameVerdict::Ok: return "ok";
        case FrameVerdict::WrongLength: return "a frame that is not 512 bytes";
        case FrameVerdict::BadSync: return "a frame without the 0x7F 0x7F 0x7F sync";
        case FrameVerdict::BadReceiverCount: return "a receiver count outside 1 to 8";
        case FrameVerdict::BadSelection: return "a receiver selection outside the receiver count";
    }
    return "?";
}

namespace {

// Bits 23..16 first, two's complement (a stated choice: the document does not name the format).
inline float sample24(const std::uint8_t* p) {
    std::int32_t v = (static_cast<std::int32_t>(p[0]) << 16) | (static_cast<std::int32_t>(p[1]) << 8) | p[2];
    if ((v & 0x800000) != 0) { v -= 0x1000000; }
    return static_cast<float>(v) * (1.0f / 8388608.0f);
}

}  // namespace

FrameVerdict decodeFrame(const std::uint8_t* f, std::size_t length, int receivers, int deliver,
                         std::vector<std::complex<float>>& out, FrameStatus& status) {
    if (f == nullptr || length != 512) { return FrameVerdict::WrongLength; }
    if (receivers < 1 || receivers > kMaxReceivers) { return FrameVerdict::BadReceiverCount; }
    if (deliver < 1 || deliver > receivers) { return FrameVerdict::BadSelection; }
    if (f[0] != kSyncByte || f[1] != kSyncByte || f[2] != kSyncByte) { return FrameVerdict::BadSync; }

    FrameStatus st;
    for (int i = 0; i < 5; ++i) { st.cc[static_cast<std::size_t>(i)] = f[3 + i]; }
    st.address = static_cast<std::uint8_t>(st.cc[0] >> 3);
    if (st.address == 0) {
        st.ptt = (st.cc[0] & 0x01) != 0;
        st.dash = (st.cc[0] & 0x02) != 0;
        st.dot = (st.cc[0] & 0x04) != 0;
        st.hasOverflow = true;  // "LT2208 Overflow" is C1 bit 0
        st.adcOverflow[0] = (st.cc[1] & 0x01) != 0;
        st.hasSoftwareSerial = true;
        st.softwareSerial = st.cc[4];
    } else if (st.address == 4) {
        st.hasOverflow = true;
        for (std::size_t k = 0; k < 4; ++k) {
            st.adcOverflow[k] = (st.cc[1 + k] & 0x01) != 0;
            st.mercuryVersion[k] = static_cast<std::uint8_t>(st.cc[1 + k] >> 1);
        }
    }

    const int groups = groupsPerFrame(receivers);
    const std::size_t groupBytes = static_cast<std::size_t>(6 * receivers + 2);
    const std::size_t rxOffset = static_cast<std::size_t>(6 * (deliver - 1));
    const std::uint8_t* p = f + 8;
    for (int g = 0; g < groups; ++g, p += groupBytes) {
        const std::uint8_t* s = p + rxOffset;
        out.emplace_back(sample24(s), sample24(s + 3));  // I then Q
    }
    status = st;
    return FrameVerdict::Ok;
}

bool Protocol1Decoder::configure(int receivers, int deliver, int rateHz) {
    if (receivers < 1 || receivers > kMaxReceivers || deliver < 1 || deliver > receivers) { return false; }
    std::uint8_t code = 0;
    if (!rateToSpeedCode(rateHz, code)) { return false; }
    receivers_ = receivers;
    deliver_ = deliver;
    rateHz_ = rateHz;
    return true;
}

bool Protocol1Decoder::decodePacket(const std::uint8_t* frames1024, std::vector<std::complex<float>>& out) {
    const std::size_t mark = out.size();
    FrameStatus st[2];
    for (int i = 0; i < 2; ++i) {
        const FrameVerdict v = decodeFrame(frames1024 + 512 * i, 512, receivers_, deliver_, out, st[i]);
        if (v != FrameVerdict::Ok) {
            out.resize(mark);
            last_ = v;
            ++tally_.refused;
            if (v == FrameVerdict::BadSync) { ++tally_.badSync; }
            return false;
        }
    }
    last_ = FrameVerdict::Ok;
    for (const FrameStatus& s : st) {
        ++tally_.frames;
        if (s.hasOverflow) {
            for (std::size_t k = 0; k < 4; ++k) {
                if (s.adcOverflow[k]) { ++tally_.adcOverflowFrames[k]; }
            }
        }
        if (s.hasSoftwareSerial) { tally_.hasSoftwareSerial = true; tally_.softwareSerial = s.softwareSerial; }
    }
    tally_.samples += out.size() - mark;
    return true;
}

// ---- send ---------------------------------------------------------------------------------------

std::string validateSettings(const ControlSettings& s) {
    std::uint8_t code = 0;
    if (!rateToSpeedCode(s.rateHz, code)) {
        return "the sample rate must be 48000, 96000, 192000 or 384000 Hz";
    }
    if (s.receivers < 1 || s.receivers > kMaxTunedReceivers) {
        return s.receivers == 8 ? "eight receivers can be unpacked but not tuned: the protocol document lists no frequency address for receiver 8"
                                : "the receiver count must be 1 to 7";
    }
    if (s.tenMhzRef > 2) { return "the 10 MHz reference choice must be 0 (Atlas/Excalibur), 1 (Penelope) or 2 (Mercury)"; }
    if (s.clock122 > 1) { return "the 122.88 MHz source choice must be 0 (Penelope) or 1 (Mercury)"; }
    if (s.config > 3) { return "the board configuration choice must be 0 to 3"; }
    if (s.attenuatorDb < 0 || s.attenuatorDb > 31) { return "the input attenuator must be 0 to 31 dB"; }
    return std::string();
}

std::vector<Cc> controlCycle(const ControlSettings& s) {
    std::vector<Cc> v;
    std::uint8_t speed = 0;
    rateToSpeedCode(s.rateHz, speed);
    const int n = s.receivers < 1 ? 1 : (s.receivers > kMaxTunedReceivers ? kMaxTunedReceivers : s.receivers);
    {  // address 0: general settings
        Cc c{};
        c[0] = 0x00;
        c[1] = static_cast<std::uint8_t>((speed & 3) | ((s.tenMhzRef & 3) << 2) | ((s.clock122 & 1) << 4) | ((s.config & 3) << 5));
        c[2] = 0x00;
        c[3] = static_cast<std::uint8_t>(s.preamp ? 0x04 : 0x00);
        c[4] = static_cast<std::uint8_t>(((n - 1) & 7) << 3);
        v.push_back(c);
    }
    for (int k = 1; k <= n; ++k) {  // receivers' NCO frequencies: C0 = 0x04 + 2*(k-1), MSB in C1
        Cc c{};
        c[0] = static_cast<std::uint8_t>(0x04 + 2 * (k - 1));
        const std::uint32_t f = s.rxFreqHz[static_cast<std::size_t>(k - 1)];
        c[1] = static_cast<std::uint8_t>(f >> 24);
        c[2] = static_cast<std::uint8_t>(f >> 16);
        c[3] = static_cast<std::uint8_t>(f >> 8);
        c[4] = static_cast<std::uint8_t>(f);
        v.push_back(c);
    }
    {  // address 0x0A (C0 = 0001010x): receiver preamps in C1, ADC1 attenuator in C4
        Cc c{};
        c[0] = 0x14;
        c[1] = static_cast<std::uint8_t>(s.preamp ? 0x01 : 0x00);
        c[4] = static_cast<std::uint8_t>((s.attenuatorEnable ? 0x20 : 0x00) | (s.attenuatorDb & 0x1F));
        v.push_back(c);
    }
    return v;
}

std::array<std::uint8_t, 512> buildControlFrame(const Cc& cc) {
    std::array<std::uint8_t, 512> f{};
    f[0] = f[1] = f[2] = kSyncByte;
    for (std::size_t i = 0; i < 5; ++i) { f[3 + i] = cc[i]; }
    return f;  // the 63 groups of left/right audio and I/Q stay zero: nothing is transmitted
}

bool ControlSender::set(const ControlSettings& s, std::string& why) {
    why = validateSettings(s);
    if (!why.empty()) { return false; }
    settings_ = s;
    cycle_ = controlCycle(s);
    pos_ = 0;
    return true;
}

std::size_t ControlSender::packetsPerCycle() const { return (cycle_.size() + 1) / 2; }

std::vector<std::uint8_t> ControlSender::nextPacket() {
    std::uint8_t frames[1024];
    for (int i = 0; i < 2; ++i) {
        const auto f = buildControlFrame(cycle_[pos_]);
        for (std::size_t k = 0; k < 512; ++k) { frames[512 * i + k] = f[k]; }
        pos_ = (pos_ + 1) % cycle_.size();
    }
    return buildDataPacket(kEndpointToDevice, seq_++, frames);
}

}  // namespace cascade::source::hpsdr
