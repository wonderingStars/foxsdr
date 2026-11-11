// feedsignals.hpp - the per-plugin signal builders of the feed fixtures (lane feedplan). See feedgen.hpp for what a generated fixture proves and does not prove.
// Each builder returns a ready I/Q file (Iq) and states, in its comment, the tuning a receiver needs and the text the plugin is expected to print.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <map>
#include <string>
#include <vector>

#include "feedgen.hpp"

namespace feedgen {

constexpr double kRate48 = 48000.0;
constexpr double kUsbOffset = 10000.0;   // the audio decoders' carrier sits 10 kHz above the file centre, as the converted real copies do (VFO = centre + 10 kHz)

// ---- DTMF (ITU-T Q.23 frequency table, Q.24 timing) ---------------------------------------------------------------------------------------------
// Keypad row-major "123A456B789C*0#D"; 70 ms tone, 70 ms pause (well above the 40 ms minimum), the high tone 1 dB above the low (a normal twist), 1 % frequency error on the lot.
// Tuning: USB, VFO = centre + 10 kHz, 3 kHz passband.
inline Iq dtmf(const Opt& o = Opt()) {
    static const double lo[4] = {697.0, 770.0, 852.0, 941.0}, hi[4] = {1209.0, 1336.0, 1477.0, 1633.0};
    const char* pad = "123A456B789C*0#D";
    Audio a;
    silence(a, 0.8, kRate48);
    for (int rep = 0; rep < 2; ++rep) {
        for (int k = 0; k < 16; ++k) {
            const int r = k / 4, c = k % 4;
            Audio t;
            tone(t, lo[r] * 1.004, 0.07, kRate48, 0.30, 0.003);
            Audio u;
            tone(u, hi[c] * 0.996, 0.07, kRate48, 0.30 * std::pow(10.0, 1.0 / 20.0), 0.003);
            for (std::size_t i = 0; i < t.size(); ++i) { a.push_back(t[i] + u[i]); }
            silence(a, 0.07, kRate48);
        }
        silence(a, 1.2, kRate48);   // a gap long enough to end the digit string
    }
    normalise(a, 0.8);
    (void)pad;
    return usb(a, kRate48, kUsbOffset, o);
}

// ---- ILS localiser (ICAO Annex 10: 90 Hz and 150 Hz tones on the carrier; DDM = (depth 90 - depth 150) as a fraction) ----------------------------
// 90 Hz 25 %, 150 Hz 15 %: ratio 20 log10(25/15) = +4.4 dB, DDM +0.100 (the receiver's AM demodulator may remove the carrier, then only the ratio is printed).
// Tuning: AM, VFO = centre + 10 kHz, 12 kHz passband.
inline Iq ilsLoc(const Opt& o = Opt()) {
    Audio a;
    const std::size_t n = static_cast<std::size_t>(12.0 * kRate48);
    a.resize(n);
    for (std::size_t k = 0; k < n; ++k) {
        const double t = static_cast<double>(k) / kRate48;
        a[k] = 0.25 * std::sin(2.0 * kPi * 90.0 * t) + 0.15 * std::sin(2.0 * kPi * 150.0 * t + 0.7);
    }
    return am(a, kRate48, kUsbOffset, 1.0, o);
}

// ---- CCIR / ITU-R M.257 selective calling: sequential tones, 100 ms each, figures 1..0 and the repetition tone ----------------------------------------
// Call "12345" sent twice (M.257 Figure 1: the second send follows the first). Tuning: USB, VFO = centre + 10 kHz, 3 kHz passband.
inline Iq ccirSelcall(const Opt& o = Opt()) {
    const double fig[10] = {1981.0, 1124.0, 1197.0, 1275.0, 1358.0, 1446.0, 1540.0, 1640.0, 1747.0, 1860.0};   // index = figure (0..9)
    const double rep = 2110.0;
    const std::string digits = "12345";
    std::vector<double> f;
    bool lastRep = false;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && digits[i] == digits[i - 1] && !lastRep) {
            f.push_back(rep);
            lastRep = true;
        } else {
            f.push_back(fig[digits[i] - '0']);
            lastRep = false;
        }
    }
    Audio a;
    silence(a, 0.8, kRate48);
    for (int pass = 0; pass < 2; ++pass) {
        for (double hz : f) {
            tone(a, hz, 0.097, kRate48, 0.5, 0.0015);
            silence(a, 0.003, kRate48);
        }
        silence(a, pass == 0 ? 0.9 : 1.5, kRate48);
    }
    return usb(a, kRate48, kUsbOffset, o);
}

// ---- PSK31 (BPSK, 31.25 baud, Varicode; the plugin's presets are USB 14.069 MHz etc.) ------------------------------------------------------------------
// "CQ CQ DE FOXSDR FIXTURE K" on an audio carrier at 1500 Hz, raised-cosine phase transitions, idle (phase reversals) before and the tail of carrier after.
// Tuning: USB, VFO = centre + 10 kHz, 3 kHz passband.
inline const char* const* psk31Varicode() {
    static const char* const t[128] = {
        "1010101011", "1011011011", "1011101101", "1101110111", "1011101011", "1101011111", "1011101111", "1011111101", "1011111111", "11101111", "11101", "1101101111",
        "1011011101", "11111", "1101110101", "1110101011", "1011110111", "1011110101", "1110101101", "1110101111", "1101011011", "1101101011", "1101101101", "1101010111",
        "1101111011", "1101111101", "1110110111", "1101010101", "1101011101", "1110111011", "1011111011", "1101111111", "1", "111111111", "101011111", "111110101", "111011011",
        "1011010101", "1010111011", "101111111", "11111011", "11110111", "101101111", "111011111", "1110101", "110101", "1010111", "110101111", "10110111", "10111101",
        "11101101", "11111111", "101110111", "101011011", "101101011", "110101101", "110101011", "110110111", "11110101", "110111101", "111101101", "1010101", "111010111",
        "1010101111", "1010111101", "1111101", "11101011", "10101101", "10110101", "1110111", "11011011", "11111101", "101010101", "1111111", "111111101", "101111101",
        "11010111", "10111011", "11011101", "10101011", "11010101", "111011101", "10101111", "1101111", "1101101", "101010111", "110110101", "101011101", "101110101",
        "101111011", "1010101101", "111110111", "111101111", "111111011", "1010111111", "101101101", "1011011111", "1011", "1011111", "101111", "101101", "11", "111101",
        "1011011", "101011", "1101", "111101011", "10111111", "11011", "111011", "1111", "111", "111111", "110111111", "10101", "10111", "101", "110111", "1111011", "1101011",
        "11011111", "1011101", "111010101", "1010110111", "110111011", "1010110101", "1011010111", "1110110101"};
    return t;
}
inline Iq psk31(const Opt& o = Opt()) {
    const char* const* code = psk31Varicode();
    const std::string text = "CQ CQ DE FOXSDR FIXTURE K";
    std::vector<int> bits;
    for (int i = 0; i < 160; ++i) { bits.push_back(0); }   // idle: continuous phase reversals (the receiver needs about three seconds to find and lock the carrier)
    for (char ch : text) {
        for (const char* p = code[static_cast<unsigned char>(ch) & 127]; *p; ++p) { bits.push_back(*p == '1'); }
        bits.push_back(0);
        bits.push_back(0);
    }
    for (int i = 0; i < 24; ++i) { bits.push_back(0); }
    for (int i = 0; i < 16; ++i) { bits.push_back(1); }
    const std::size_t sps = static_cast<std::size_t>(kRate48 / 31.25 + 0.5);   // 1536
    std::vector<double> theta(bits.size() + 1, 0.0);
    double th = 0.0;
    for (std::size_t k = 0; k < bits.size(); ++k) {
        if (bits[k] == 0) { th += kPi; }
        theta[k] = th;
    }
    theta[bits.size()] = th;
    const std::size_t lead = static_cast<std::size_t>(0.6 * kRate48), tail = static_cast<std::size_t>(1.2 * kRate48);
    Audio a(lead + bits.size() * sps + tail, 0.0);
    double ph = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        ph += 2.0 * kPi * 1500.0 / kRate48;
        if (i < lead) { continue; }
        const std::size_t j = i - lead, k = j / sps;
        if (k >= bits.size()) { continue; }
        const double u = static_cast<double>(j % sps) / static_cast<double>(sps);
        const double c2 = std::cos(kPi * u / 2.0) * std::cos(kPi * u / 2.0), s2 = 1.0 - c2;
        const cd bb = std::polar(1.0, theta[k]) * c2 + std::polar(1.0, k + 1 < bits.size() ? theta[k + 1] : theta[k]) * s2;
        a[i] = 0.6 * (bb * std::polar(1.0, ph)).real();
    }
    return usb(a, kRate48, kUsbOffset, o);
}

// ---- PER tester (a 2-FSK / GFSK signal carrying PN9, Fibonacci x^9 + x^5 + 1) -----------------------------------------------------------------------
// 9600 baud, +-2400 Hz deviation, Gaussian BT 0.5, 96 kS/s. `flips` bits are inverted after the first 2000 (spread evenly), so the answer is known: errors = flips.
// Tuning: NFM, VFO = centre + 20 kHz, bandwidth 20 kHz; the plugin's own settings (baud 9600, PN9, 256-bit packets) are its defaults.
inline Iq perPn9(int flips, const Opt& o = Opt()) {
    const double rate = 96000.0, baud = 9600.0, dev = 2400.0;
    const std::size_t nbits = 9600 * 6;
    std::vector<int> bits(nbits);
    std::uint32_t h = 0x1FF;
    for (std::size_t i = 0; i < nbits; ++i) {
        const unsigned s = ((h >> 4) ^ (h >> 8)) & 1u;   // taps 5 and 9
        bits[i] = static_cast<int>(s);
        h = (h << 1) | s;
    }
    for (int f = 0; f < flips; ++f) {
        const std::size_t at = 2000 + static_cast<std::size_t>(f) * ((nbits - 4000) / static_cast<std::size_t>(std::max(1, flips)));
        bits[at] ^= 1;
    }
    const std::size_t sps = static_cast<std::size_t>(rate / baud + 0.5);   // 10
    const std::size_t lead = 4800, tail = 4800;
    Audio nrz(lead + nbits * sps + tail, 0.0);
    for (std::size_t i = 0; i < nbits; ++i) {
        for (std::size_t k = 0; k < sps; ++k) { nrz[lead + i * sps + k] = bits[i] ? 1.0 : -1.0; }
    }
    // Gaussian filter, BT 0.5, spanning +-2 symbols
    const double bt = 0.5, sigma = std::sqrt(std::log(2.0)) / (2.0 * kPi * bt) * static_cast<double>(sps);
    std::vector<double> taps;
    const int half = static_cast<int>(2 * sps);
    double sum = 0.0;
    for (int t = -half; t <= half; ++t) {
        taps.push_back(std::exp(-0.5 * (t / sigma) * (t / sigma)));
        sum += taps.back();
    }
    for (double& t : taps) { t /= sum; }
    Audio a(nrz.size(), 0.0);
    for (std::size_t i = 0; i < nrz.size(); ++i) {
        double acc = 0.0;
        for (int t = -half; t <= half; ++t) {
            const long idx = static_cast<long>(i) + t;
            if (idx >= 0 && idx < static_cast<long>(nrz.size())) { acc += taps[static_cast<std::size_t>(t + half)] * nrz[static_cast<std::size_t>(idx)]; }
        }
        a[i] = acc;
    }
    return nfm(a, rate, 20000.0, dev, o);
}

}  // namespace feedgen
