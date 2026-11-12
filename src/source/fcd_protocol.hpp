// fcd_protocol.hpp - FUNcube Dongle Pro (0x04D8:0xFB56, 96 kS/s) and Pro+ (0x04D8:0xFB31, 192 kS/s): the HID
// control commands, the gain models and the tuning rules. The I/Q itself is a USB sound device (left = I,
// right = Q); only the tuner is controlled through HID, which is what this file and fcd_control.* are about.
//
// SOURCE AND LICENCE (opened 2026-11-12, commit in THIRD-PARTY-NOTICES.md): pothosware/SoapyFCDPP (Boost
// Software License 1.0: LICENSE_1_0.txt opened, README states BSL-1.0, SoapyFCDPP/SoapyFCDPP.cpp carries
// "SPDX-License-Identifier: BSL-1.0"). Read: SoapyFCDPP/fcdcmd.h and fcd.c (headers "Created by Albin Stigo,
// Copyright 2016 / 2018 Albin Stigo" - no statement of any other origin), fcd.h, SoapyFCDPP.cpp / .hpp. The
// command numbers, the 64-byte report layout (command in the first byte, parameters after it), the gain
// ranges and the frequency ranges below are facts of the device's interface taken from there. PROVENANCE
// CAVEAT, recorded in the feature record: the macro names in fcdcmd.h match the ones the (GPL) libfcd/qthid use;
// that file carries no header of its own, the repository's licence is BSL-1.0, and no GPL source was opened
// here to compare. The manufacturer's own command document was not found (see the record).
// fcddirect.cpp, the file the brief names, is only a Soapy test program and holds no command.
//
// NEVER SENT: command 255 ("reset to bootloader"). The firmware-update path of the dongle is not touched.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cascade::source::fcd {

constexpr std::uint16_t kVid = 0x04D8;
constexpr std::uint16_t kPidPro = 0xFB56;      // FUNcube Dongle Pro: 96 kS/s
constexpr std::uint16_t kPidProPlus = 0xFB31;  // FUNcube Dongle Pro+: 192 kS/s
constexpr double kRateProHz = 96000.0;
constexpr double kRateProPlusHz = 192000.0;

constexpr std::size_t kReportBytes = 64;  // SoapyFCDPP: BUF_LEN 64

namespace cmd {
constexpr std::uint8_t Query = 1;           // answers with a string ("FCDAPP version ...")
constexpr std::uint8_t SetFrequencyKhz = 100;
constexpr std::uint8_t SetFrequencyHz = 101;  // 4 bytes little endian
constexpr std::uint8_t GetFrequencyHz = 102;
constexpr std::uint8_t SetLna = 110;          // 1 on, 0 off
constexpr std::uint8_t SetMixer = 114;
constexpr std::uint8_t SetIfGain = 117;       // Pro+: one byte 0..59 dB
constexpr std::uint8_t SetBiasTee = 126;      // Pro+: 1 on, 0 off
constexpr std::uint8_t GetLna = 150;
constexpr std::uint8_t GetMixer = 154;
constexpr std::uint8_t GetIfGain = 157;
constexpr std::uint8_t GetBiasTee = 166;
// Pro (V1.x): the six IF amplifier stages are set one by one
constexpr std::uint8_t SetIfStage[6] = {117, 120, 121, 123, 124, 125};
constexpr std::uint8_t GetIfStage[6] = {157, 160, 161, 163, 164, 165};
constexpr std::uint8_t ResetToBootloader = 255;  // NEVER SENT by this code
}  // namespace cmd

// The 64-byte report: [command][parameters...][zero padding]. (The HID report id byte, 0, is the transport's.)
inline std::array<std::uint8_t, kReportBytes> report(std::uint8_t command, const std::vector<std::uint8_t>& params = {}) {
    std::array<std::uint8_t, kReportBytes> r{};
    r[0] = command;
    for (std::size_t i = 0; i < params.size() && i + 1 < kReportBytes; ++i) { r[1 + i] = params[i]; }
    return r;
}

// The device's answer: byte 0 echoes the command, byte 1 is a flag SoapyFCDPP never checks (UNVERIFIED what it
// means), data starts at byte 2 (SoapyFCDPP reads every value from there).
struct Reply {
    std::uint8_t command = 0;
    std::uint8_t flag = 0;
    const std::uint8_t* data = nullptr;  // points into the buffer, kReportBytes - 2 bytes
};
inline bool parseReply(const std::uint8_t* d, std::size_t n, std::uint8_t expectCommand, Reply& out) {
    if (d == nullptr || n < 3 || d[0] != expectCommand) { return false; }
    out.command = d[0];
    out.flag = d[1];
    out.data = d + 2;
    return true;
}

inline std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

// ---- tuning ----------------------------------------------------------------------------------------------------
struct Band { double loHz, hiHz; };
// SoapyFCDPP's range for both models: 150 kHz to 240 MHz and 420 MHz to 1.9 GHz.
constexpr Band kBands[2] = {{150000.0, 240000000.0}, {420000000.0, 1900000000.0}};
inline bool tunable(double hz) { return (hz >= kBands[0].loHz && hz <= kBands[0].hiHz) || (hz >= kBands[1].loHz && hz <= kBands[1].hiHz); }

// The frequency actually written for a requested one with the oscillator trim in ppm: f x (1 + 1e-6 ppm),
// truncated to 32 bits as SoapyFCDPP does.
inline std::uint32_t trimmedHz(double hz, double ppm) {
    const double v = hz * (1.0 + 1e-6 * ppm);
    if (!(v >= 0.0)) { return 0; }
    if (v >= 4294967295.0) { return 0xFFFFFFFFu; }
    return static_cast<std::uint32_t>(v);
}

// ---- gains --------------------------------------------------------------------------------------------------------
// Pro+ (comments on the manufacturer's page, as SoapyFCDPP cites): LNA 0 / +10 dB, mixer 0 / +20 dB, IF 0..59 dB
// in 1 dB steps.
constexpr int kProPlusIfMin = 0, kProPlusIfMax = 59;
// Pro (Elonics E4000): LNA index table (indices 2 and 3 are not valid), mixer 4 / 12 dB, IF 3..57 dB spread over
// six stages.
constexpr double kProLnaDb[15] = {-5.0, -2.5, -999, -999, 0.0, 2.5, 5.0, 7.5, 10.0, 12.5, 15.0, 17.5, 20.0, 25.0, 30.0};
constexpr int kProIfMin = 3, kProIfMax = 57;

// The LNA register index nearest to `db` among the valid ones; its dB value in `actual`.
inline int proLnaIndexFor(double db, double& actual) {
    int best = 0;
    double bestD = 1e9;
    for (int i = 0; i < 15; ++i) {
        if (kProLnaDb[i] < -900) { continue; }
        const double d = db > kProLnaDb[i] ? db - kProLnaDb[i] : kProLnaDb[i] - db;
        if (d < bestD) { bestD = d; best = i; }
    }
    actual = kProLnaDb[best];
    return best;
}

// The six stage register values for a requested total IF gain on the Pro: SoapyFCDPP's distribution (cutoff
// tables derived from the E4000 datasheet's "linearity mode" table, which was NOT opened here): stage k takes
// the highest register whose cutoff the gain reaches; stage 2 is reduced by 3 if stage 1 is non-zero; stage 4 is
// gain mod 3. The dB each register stands for is the gains table in stageDb().
void proIfStageRegisters(int gainDb, std::uint8_t regs[6]);
int proStageDb(int stage, std::uint8_t reg);  // 0 for an out-of-range register
// The total IF gain the six registers stand for.
int proIfTotalDb(const std::uint8_t regs[6]);

}  // namespace cascade::source::fcd
