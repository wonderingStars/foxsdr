// icr8600_protocol.hpp - the Icom IC-R8600 [I/Q OUT] USB port: USB ids, endpoints, the CI-V framing and
// commands that start and stop the stream, the I/Q block format with its sync markers, and the reader of the
// firmware file the USER supplies. Pure byte logic; nothing here opens a device.
//
// SOURCES AND LICENCES (opened 2026-11-12, commits in THIRD-PARTY-NOTICES.md):
//   * ibelinp/ic-r8600-usb-iq (MIT, "Copyright (c) 2026 Pieter Ibelings": LICENSE opened, with its note that the
//     licence does NOT cover Icom's firmware). README.md, firmware/README.md, tools/spt_to_json.py and
//     examples/linux/r8600_iq.c were read: the five-phase protocol, the CI-V start-up order, the rate codes, the
//     sync-marker rule and the Cypress "CSPT" script layout. Facts ported; no file pasted.
//   * dmitryelj/SoapyICR8600 (MIT, "Copyright (c) 2018 D. Eliuseev": LICENSE opened; no file header names a GPL or
//     other origin): WinUSBDevice.cpp / .h, Settings.cpp, Streaming.cpp read as a cross-check. They agree with
//     the above on the endpoints (0x02 CI-V out, 0x88 CI-V answers, 0x86 I/Q), the frame bytes, the BCD
//     frequency, the BCD RF gain, the attenuator and the rate codes. They add: the answer frames
//     FE FE E0 96 FB FD (OK) / FA FD (NG), the antenna and pre-amp commands, and that antenna commands only
//     work on HF (the radio answers NG elsewhere).
//   Both are reverse-engineered and say so; Icom's own "I/Q Output Reference Guide" was NOT opened (it sits
//   behind Icom's agreement page). Everything below that the two projects state differently or not at all is
//   marked UNVERIFIED.
//
// WHAT THE USER MUST SUPPLY, AND WHAT IS NEVER SHIPPED OR FETCHED: the [I/Q OUT] port is a Cypress FX2 with no
// firmware in ROM. It enumerates as 0C26:0022 and needs Icom's firmware loaded into its RAM on every connect;
// then it re-enumerates as 0C26:0023 and streams. Icom's firmware is Icom's copyrighted work. FoxSDR contains
// none of it and fetches none: the user either (a) installs Icom's own "USB I/Q Package for HDSDR", whose driver
// loads the firmware itself so that the port already shows as 0C26:0023 (nothing is needed from us), or (b)
// gives the path of the file Icom's installer places on their machine (IC-R8600_usb_iq.spt) or of the JSON
// sequence made from it, and FoxSDR replays it. A firmware file is parsed defensively (see parseFirmware): only
// the vendor RAM-write request, only the FX2's RAM and CPU-control addresses.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace cascade::source::icr8600 {

constexpr std::uint16_t kVid = 0x0C26;
constexpr std::uint16_t kPidLoader = 0x0022;  // FX2 with no firmware
constexpr std::uint16_t kPidStream = 0x0023;  // after the firmware load
constexpr std::uint8_t kEpCommand = 0x02;     // bulk OUT: CI-V commands
constexpr std::uint8_t kEpAnswer = 0x88;      // bulk IN: CI-V answers
constexpr std::uint8_t kEpIq = 0x86;          // bulk IN: I/Q

// ---- CI-V ------------------------------------------------------------------------------------------------
constexpr std::uint8_t kRadioAddress = 0x96;
constexpr std::uint8_t kControllerAddress = 0xE0;

// FE FE 96 E0 <payload> FD, plus one FF when the total length would be odd (the bulk packets are padded to an
// even length). Empty for an empty payload or one over 32 bytes.
inline std::vector<std::uint8_t> civFrame(const std::vector<std::uint8_t>& payload) {
    if (payload.empty() || payload.size() > 32) { return {}; }
    std::vector<std::uint8_t> f{0xFE, 0xFE, kRadioAddress, kControllerAddress};
    f.insert(f.end(), payload.begin(), payload.end());
    f.push_back(0xFD);
    if (f.size() & 1) { f.push_back(0xFF); }
    return f;
}

enum class Answer { Ok, Refused, Other, Garbage };

// The radio's answer frame: FE FE E0 96 <FB|FA> FD (an FF pad may follow). Ok for FB, Refused for FA, Other for a
// well-formed frame from the radio that carries something else (a read-back), Garbage for anything that is not a
// frame addressed to the controller from the radio.
inline Answer parseAnswer(const std::uint8_t* d, std::size_t n, std::vector<std::uint8_t>* body = nullptr) {
    if (body) { body->clear(); }
    if (d == nullptr || n < 6) { return Answer::Garbage; }
    if (d[0] != 0xFE || d[1] != 0xFE || d[2] != kControllerAddress || d[3] != kRadioAddress) { return Answer::Garbage; }
    std::size_t end = 4;
    while (end < n && d[end] != 0xFD) { ++end; }
    if (end >= n) { return Answer::Garbage; }
    if (end == 5 && d[4] == 0xFB) { return Answer::Ok; }
    if (end == 5 && d[4] == 0xFA) { return Answer::Refused; }
    if (end == 4) { return Answer::Garbage; }
    if (body) { body->assign(d + 4, d + end); }
    return Answer::Other;
}

// Five bytes, little-endian BCD pairs (10 Hz|1 Hz first). Hz up to 9,999,999,999; false above that.
inline bool frequencyBcd(std::uint64_t hz, std::uint8_t out[5]) {
    if (hz > 9999999999ull) { return false; }
    for (int i = 0; i < 5; ++i) {
        const unsigned lo = static_cast<unsigned>(hz % 10);
        hz /= 10;
        const unsigned hi = static_cast<unsigned>(hz % 10);
        hz /= 10;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

// No frequency range is enforced here: neither project states the radio's limits, so the radio's own refusal (an
// NG answer) is what the user is told. The only limit is what ten BCD digits can hold.

inline std::vector<std::uint8_t> cmdFrequency(std::uint64_t hz) {
    std::uint8_t b[5];
    if (!frequencyBcd(hz, b)) { return {}; }
    return civFrame({0x05, b[0], b[1], b[2], b[3], b[4]});
}
// RF gain as the radio's 0..255 code in three BCD digits: 14 02 <hundreds> <tens|units>. Per SoapyICR8600 the
// code is 4 x (dB + 63.75), so 255 = 0 dB: "from observation, have not found this specified" - the code is
// exposed as steps, not decibels.
inline std::vector<std::uint8_t> cmdRfGain(unsigned code) {
    if (code > 255) { return {}; }
    return civFrame({0x14, 0x02, static_cast<std::uint8_t>(code / 100),
                     static_cast<std::uint8_t>(((code % 100) / 10) << 4 | (code % 10))});
}
// Attenuator 0, 10, 20 or 30 dB: 11 00 | 10 | 20 | 30.
inline std::vector<std::uint8_t> cmdAttenuator(unsigned db) {
    if (db != 0 && db != 10 && db != 20 && db != 30) { return {}; }
    return civFrame({0x11, static_cast<std::uint8_t>((db / 10) << 4)});
}
inline std::vector<std::uint8_t> cmdAntenna(unsigned index) {  // 0, 1, 2 = ANT 1..3, HF only
    if (index > 2) { return {}; }
    return civFrame({0x12, static_cast<std::uint8_t>(index)});
}
inline std::vector<std::uint8_t> cmdPreamp(bool on) { return civFrame({0x16, 0x02, static_cast<std::uint8_t>(on ? 1 : 0)}); }
inline std::vector<std::uint8_t> cmdIpPlusOff() { return civFrame({0x16, 0x65, 0x00}); }
inline std::vector<std::uint8_t> cmdIqOff() { return civFrame({0x1A, 0x13, 0x01, 0x00}); }
inline std::vector<std::uint8_t> cmdIqModeOn() { return civFrame({0x1A, 0x13, 0x00, 0x01}); }
inline std::vector<std::uint8_t> cmdIqModeOff() { return civFrame({0x1A, 0x13, 0x00, 0x00}); }
inline std::vector<std::uint8_t> cmdIqPrepare() { return civFrame({0x1A, 0x13, 0x02, 0x01}); }  // sent by HDSDR right before ON
inline std::vector<std::uint8_t> cmdIqOn(std::uint8_t rateCode) { return civFrame({0x1A, 0x13, 0x01, 0x01, 0x00, rateCode}); }

// ---- rates -----------------------------------------------------------------------------------------------
struct RateRow {
    std::uint8_t code;
    std::uint32_t hz;
    std::uint32_t markerPairs;  // sync marker interval in I/Q pairs (README)
};
constexpr RateRow kRates[6] = {{0x06, 240000, 512},  {0x05, 480000, 1024},  {0x04, 960000, 2048},
                               {0x03, 1920000, 4096}, {0x02, 3840000, 8192}, {0x01, 5120000, 10923}};

inline bool rateCodeFor(double hz, std::uint8_t& code) {
    for (const RateRow& r : kRates) {
        if (hz >= r.hz - 0.5 && hz <= r.hz + 0.5) { code = r.code; return true; }
    }
    return false;
}

// ---- the I/Q stream ----------------------------------------------------------------------------------------
// Little-endian interleaved int16 I, Q; the value 0x8000 never occurs in data; a marker is two 0x8000 samples
// (00 80 00 80) inserted periodically and ALWAYS followed by an I sample. The aligner strips every 0x8000
// sample, locks I/Q phase to the markers, forwards whole pairs only, and counts what it did.
class IqAligner {
public:
    // Feeds raw bytes; appends complete complex samples (full scale 1.0) to `out`.
    void feed(const std::uint8_t* b, std::size_t n, std::vector<std::complex<float>>& out) {
        std::size_t i = 0;
        if (haveCarry_) {
            if (n == 0) { return; }
            sample(static_cast<std::int16_t>(static_cast<std::uint16_t>(carry_ | (b[0] << 8))), out);
            i = 1;
            haveCarry_ = false;
        }
        for (; i + 1 < n; i += 2) {
            sample(static_cast<std::int16_t>(static_cast<std::uint16_t>(b[i] | (b[i + 1] << 8))), out);
        }
        if (i < n) {
            carry_ = b[i];
            haveCarry_ = true;
        }
    }
    void reset() { *this = IqAligner(); }
    std::uint64_t markers() const { return markers_; }
    std::uint64_t realigned() const { return realigned_; }
    std::uint64_t pairs() const { return pairs_; }
    bool locked() const { return locked_; }

private:
    void sample(std::int16_t v, std::vector<std::complex<float>>& out) {
        if (static_cast<std::uint16_t>(v) == 0x8000) {
            ++marks_;
            inMarker_ = true;
            if ((marks_ & 1) == 0) { ++markers_; }  // two marker samples make one marker
            return;
        }
        if (inMarker_) {
            inMarker_ = false;  // this real sample is an I
            if (!locked_) {
                locked_ = true;
            } else if (haveI_) {
                // an I is waiting for its Q but a marker says the next sample is an I: data was lost. Complete
                // the dangling pair with a zero Q (the reference implementation's choice) and count it.
                out.emplace_back(pendingI_, 0.0f);
                ++pairs_;
                ++realigned_;
                haveI_ = false;
            }
        }
        if (!locked_) { return; }  // nothing before the first marker is trusted
        const float f = static_cast<float>(v) / 32768.0f;
        if (!haveI_) {
            pendingI_ = f;
            haveI_ = true;
        } else {
            out.emplace_back(pendingI_, f);
            ++pairs_;
            haveI_ = false;
        }
    }
    bool haveCarry_ = false, locked_ = false, inMarker_ = false, haveI_ = false;
    std::uint8_t carry_ = 0;
    float pendingI_ = 0.0f;
    std::uint64_t marks_ = 0, markers_ = 0, realigned_ = 0, pairs_ = 0;
};

// ---- the user's firmware file ---------------------------------------------------------------------------
struct FwRecord {
    std::uint16_t wValue = 0;
    std::vector<std::uint8_t> data;
};

constexpr std::size_t kMaxFirmwareRecords = 256;
constexpr std::size_t kMaxFirmwareBytes = 64 * 1024;

// The FX2's loadable space: its 16 KiB program/data RAM, 512 bytes of scratch RAM at 0xE000, and the CPU
// control register CPUCS at 0xE600 (hold in reset = 1, run = 0). A record anywhere else is refused.
inline bool firmwareAddressOk(std::uint32_t address, std::size_t len) {
    if (len == 0) { return false; }
    const std::uint32_t end = address + static_cast<std::uint32_t>(len);
    if (end > 0x10000u) { return false; }
    if (address == 0xE600u && len == 1) { return true; }
    if (end <= 0x4000u) { return true; }
    if (address >= 0xE000u && end <= 0xE200u) { return true; }
    return false;
}

// Parses either a binary Cypress "CSPT" script (the file Icom's installer places, IC-R8600_usb_iq.spt: records of
// a 32-byte header - 'CSPT', bmRequestType in the top byte of the dword at 12, bRequest at 16, wValue at 18,
// payload length at 28 - and a payload) or the JSON sequence [{"wValue": N, "data": "hex"}, ...]. Refuses, with
// `error` set: any other format; a record that is not the vendor RAM write (0x40 / 0xA0); an address outside the
// FX2's loadable space; more than 256 records or 64 KiB; a sequence that does not begin by holding the CPU in
// reset (0xE600 <- 1) and end by releasing it (0xE600 <- 0).
bool parseFirmware(const std::vector<std::uint8_t>& file, std::vector<FwRecord>& out, std::string& error);

}  // namespace cascade::source::icr8600
