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

// ---- MPT 1327 forward control channel (1200 baud FFSK: 1200 Hz = 1, 1800 Hz = 0), codewords by the standard's BCH(63,48) + parity ---------------------
// Burst: ALOHA, CCSC, GTC (go to channel 321, prefix 23 ident 4567 calling 4568), CCSC, ACK, CCSC, broadcast (channel 640, adjacent site 3), CCSC, ALOHA; system 0x2BCD.
// Tuning: NFM, VFO = centre + 10 kHz, 12.5 kHz passband.
namespace mpt {
using Bits = std::vector<std::uint8_t>;
inline void put(Bits& b, std::uint32_t v, int n) {
    for (int i = n - 1; i >= 0; --i) { b.push_back(static_cast<std::uint8_t>((v >> i) & 1u)); }
}
inline Bits str(const char* s) {
    Bits b;
    for (; *s != '\0'; ++s) { b.push_back(static_cast<std::uint8_t>(*s == '1')); }
    return b;
}
// 3.2.3 by long division: 48 information bits, 15 check bits (generator X15+X14+X13+X11+X4+X2+1), the last check bit inverted, then even parity of the 64 bits.
inline Bits complete(const Bits& info48) {
    Bits w(63, 0);
    for (int i = 0; i < 48; ++i) { w[static_cast<std::size_t>(i)] = info48[static_cast<std::size_t>(i)]; }
    const Bits g = str("1110100000010101");
    for (std::size_t i = 0; i < 48; ++i) {
        if (w[i] == 0) { continue; }
        for (std::size_t j = 0; j < g.size(); ++j) { w[i + j] ^= g[j]; }
    }
    Bits cw = info48;
    for (std::size_t i = 48; i < 63; ++i) { cw.push_back(w[i]); }
    cw[62] ^= 1u;
    unsigned par = 0;
    for (std::uint8_t b : cw) { par ^= b; }
    cw.push_back(static_cast<std::uint8_t>(par & 1u));
    return cw;
}
// Appendix 3: the completion sequence that makes the system codeword's 64-bit word end in the standard's way.
inline Bits ccsc(std::uint32_t sys15) {
    Bits info = str("1010101010101010");
    const Bits sy = str("1100010011010100");
    info.insert(info.end(), sy.begin(), sy.end());
    put(info, sys15, 15);
    Bits ccs;
    for (int assume = 0; assume < 2; ++assume) {
        Bits t = info;
        t.push_back(static_cast<std::uint8_t>(assume));
        const Bits cw = complete(t);
        if (cw[63] == 1) {
            ccs.assign(cw.begin() + 47, cw.begin() + 63);
            ccs[15] ^= 1u;
            break;
        }
    }
    Bits b;
    b.push_back(0);
    put(b, sys15, 15);
    b.insert(b.end(), ccs.begin(), ccs.end());
    const Bits pre = str("1010101010101010"), sync = str("1100010011010111");
    b.insert(b.end(), pre.begin(), pre.end());
    b.insert(b.end(), sync.begin(), sync.end());
    return b;
}
inline Bits general(std::uint32_t pfix, std::uint32_t ident1, std::uint32_t cat, std::uint32_t type, std::uint32_t func, const Bits& rest) {
    Bits b;
    b.push_back(1);
    put(b, pfix, 7);
    put(b, ident1, 13);
    b.push_back(1);
    put(b, cat, 3);
    put(b, type, 2);
    put(b, func, 3);
    b.insert(b.end(), rest.begin(), rest.end());
    return complete(b);
}
inline Bits gtc(std::uint32_t pfix, std::uint32_t ident1, bool data, std::uint32_t chan, std::uint32_t ident2) {
    Bits b;
    b.push_back(1);
    put(b, pfix, 7);
    put(b, ident1, 13);
    b.push_back(0);
    b.push_back(data ? 1 : 0);
    put(b, chan, 10);
    put(b, ident2, 13);
    put(b, 0, 2);
    return complete(b);
}
}  // namespace mpt

inline Iq mpt1327(const Opt& o = Opt()) {
    using namespace mpt;
    const std::uint32_t sys = 0x2BCD;
    const auto rest = [](std::initializer_list<std::pair<int, std::uint32_t>> f) {
        Bits b;
        for (const auto& x : f) { put(b, x.second, x.first); }
        return b;
    };
    const Bits aloha = general(0, 8191, 0, 0, 0, rest({{4, 7}, {3, 0}, {2, 0}, {5, 0}, {4, 0}}));
    const Bits g = gtc(23, 4567, false, 321, 4568);
    const Bits ack = general(23, 4568, 0, 1, 0, rest({{13, 4567}, {1, 0}, {4, 0}}));
    Bits bc;
    bc.push_back(1);
    put(bc, 4, 5);
    put(bc, sys, 15);
    bc.push_back(1);
    put(bc, 0, 3);
    put(bc, 3, 2);
    put(bc, 4, 3);
    put(bc, 640, 10);
    put(bc, 0, 4);
    put(bc, 3, 4);
    bc = complete(bc);
    const std::vector<Bits> seq = {aloha, ccsc(sys), g, ccsc(sys), ack, ccsc(sys), bc, ccsc(sys), aloha};
    Bits bits = str("1010101010101010101010101010101010101010");
    const Bits sync = str("1100010011010111");
    bits.insert(bits.end(), sync.begin(), sync.end());
    for (const Bits& c : seq) { bits.insert(bits.end(), c.begin(), c.end()); }
    bits.push_back(0);
    Audio a;
    silence(a, 0.4, kRate48);
    for (int rep = 0; rep < 3; ++rep) {
        double phase = 0.0;
        for (std::size_t k = 0; k < bits.size(); ++k) {
            const double f = bits[k] ? 1200.0 : 1800.0;
            const std::size_t n = static_cast<std::size_t>(std::floor(static_cast<double>(k + 1) * 40.0) - std::floor(static_cast<double>(k) * 40.0));
            for (std::size_t i = 0; i < n; ++i) {
                a.push_back(0.5 * std::sin(phase));
                phase += 2.0 * kPi * f / kRate48;
            }
        }
        silence(a, 0.8, kRate48);
    }
    return nfm(a, kRate48, kUsbOffset, 3000.0, o);
}

// ---- DCF77 (the time code of the German LF station: one second marker per second, the carrier cut to 15 % for 100 ms (0) or 200 ms (1)) -------------------
// Four consecutive minutes from Wed 2026-11-11 12:34 CET (UTC+1), no call bit, no announcements, the three parity bits and the BCD fields written from the published
// second-by-second table (minute 21-27, hour 29-34, day 36-41, weekday 42-44, month 45-49, year 50-57). The carrier is heard as a 1000 Hz tone (a receiver in USB 1 kHz off the carrier).
// Tuning: USB, VFO = centre + 4 kHz, 3 kHz passband, 16 kS/s file. The plugin prints the time after it has read consecutive frames.
namespace dcf {
struct Tm { int y, mo, d, wd /* ISO 1..7 */, h, mi; };
inline void frameBits(const Tm& t, int* b) {
    for (int i = 0; i < 60; ++i) { b[i] = 0; }
    b[17] = 0;   // CET: bit 17 = 0, bit 18 = 1
    b[18] = 1;
    b[20] = 1;   // start of time information
    static const int ms[7] = {21, 22, 23, 24, 25, 26, 27}, mw[7] = {1, 2, 4, 8, 10, 20, 40};
    static const int hs[6] = {29, 30, 31, 32, 33, 34}, hw[6] = {1, 2, 4, 8, 10, 20};
    static const int ds[6] = {36, 37, 38, 39, 40, 41}, dw[6] = {1, 2, 4, 8, 10, 20};
    static const int ws[3] = {42, 43, 44}, ww[3] = {1, 2, 4};
    static const int os[5] = {45, 46, 47, 48, 49}, ow[5] = {1, 2, 4, 8, 10};
    static const int ys[8] = {50, 51, 52, 53, 54, 55, 56, 57}, yw[8] = {1, 2, 4, 8, 10, 20, 40, 80};
    const auto bcd = [&](const int* sec, const int* w, int n, int v) {
        const int u = v % 10, tn = v / 10;
        for (int i = 0; i < n; ++i) { b[sec[i]] = (w[i] < 10) ? ((u & w[i]) ? 1 : 0) : ((tn & (w[i] / 10)) ? 1 : 0); }
    };
    bcd(ms, mw, 7, t.mi);
    bcd(hs, hw, 6, t.h);
    bcd(ds, dw, 6, t.d);
    for (int i = 0; i < 3; ++i) { b[ws[i]] = (t.wd & ww[i]) ? 1 : 0; }
    bcd(os, ow, 5, t.mo);
    bcd(ys, yw, 8, t.y % 100);
    const auto even = [&](int a, int z) {
        int c = 0;
        for (int i = a; i <= z; ++i) { c += b[i]; }
        return c % 2;
    };
    b[28] = even(21, 27);
    b[35] = even(29, 34);
    b[58] = even(36, 57);
}
}  // namespace dcf

inline Iq dcf77(const Opt& o = Opt()) {
    const double rate = 16000.0;
    dcf::Tm t{2026, 11, 11, 3, 12, 34};
    Audio a;
    const double lowRatio = 0.15, edgeMs = 1.5;
    double gain = 1.0;
    const double alpha = 1.0 - std::exp(-1.0 / (edgeMs * 1e-3 * rate));
    std::size_t k = 0;
    for (int frame = 0; frame < 4; ++frame) {
        int bits[60];
        dcf::frameBits(t, bits);
        for (int sec = 0; sec < 60; ++sec) {
            const std::size_t n = static_cast<std::size_t>(rate);
            const double lowMs = (sec == 59) ? 0.0 : (bits[sec] ? 200.0 : 100.0);   // no reduction in second 59: the minute marker
            for (std::size_t i = 0; i < n; ++i, ++k) {
                const double ms = static_cast<double>(i) * 1000.0 / rate;
                const double target = ms < lowMs ? lowRatio : 1.0;
                gain += alpha * (target - gain);
                a.push_back(0.5 * gain * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(k) / rate));
            }
        }
        if (++t.mi >= 60) {   // never reached for 12:34..12:37
            t.mi = 0;
            ++t.h;
        }
    }
    Opt oo = o;
    oo.snrDb = std::min(o.snrDb, 35.0);
    return usb(a, rate, 4000.0, oo);
}

// ---- EnOcean ERP1 (868.3 MHz ASK, 125 kbit/s, the 8-to-12 line code, CRC-8 poly x^8+x^2+x+1) -------------------------------------------------------
// Three telegrams of the kinds a switch and two sensors send (RPS F6 with one data byte, 1BS D5, 4BS A5), each as three sub-telegrams 10-40 ms apart, from a made-up sender
// address (not any real device). 1 MS/s, 2 kHz carrier offset, 100 ppm bit-rate error, 30 dB SNR; a "1" is 30 dB below a "0".
// Tuning: RAW, VFO irrelevant (an I/Q decoder), the file's 1 MHz span.
namespace eno {
inline void groupBits(unsigned byte, bool last, std::vector<int>& b) {
    const auto bit = [&](int k) { return static_cast<int>((byte >> k) & 1u); };
    b.push_back(bit(7)); b.push_back(bit(6)); b.push_back(bit(5)); b.push_back(1 - bit(5));
    b.push_back(bit(4)); b.push_back(bit(3)); b.push_back(bit(2)); b.push_back(1 - bit(2));
    b.push_back(bit(1)); b.push_back(bit(0)); b.push_back(last ? 1 : 0); b.push_back(last ? 0 : 1);
}
inline std::uint8_t crc8(const std::vector<std::uint8_t>& d) {
    unsigned rem = 0;
    for (std::uint8_t v : d) {
        unsigned x = (rem ^ v) << 8;
        for (int bit = 15; bit >= 8; --bit) {
            if (x & (1u << bit)) { x ^= (0x107u << (bit - 8)); }
        }
        rem = x & 0xFFu;
    }
    return static_cast<std::uint8_t>(rem);
}
inline std::vector<int> frameBits(std::vector<std::uint8_t> body) {
    body.push_back(crc8(body));
    std::vector<int> b;
    for (int i = 0; i < 8; ++i) { b.push_back(i % 2 == 0 ? 1 : 0); }   // preamble 10101010
    static const int sof[4] = {1, 0, 0, 1};
    for (int i = 0; i < 4; ++i) { b.push_back(sof[i]); }
    for (std::size_t i = 0; i < body.size(); ++i) { groupBits(body[i], i + 1 == body.size(), b); }
    return b;
}
}  // namespace eno

inline Iq enocean(const Opt& o = Opt()) {
    const double fs = 1e6, bitUs = 8.0 * (1.0 + 100e-6), low = 0.03;
    const std::uint32_t id = 0x0180ABCD;   // made up
    const auto body = [&](unsigned rorg, std::initializer_list<unsigned> data, unsigned status) {
        std::vector<std::uint8_t> b;
        b.push_back(static_cast<std::uint8_t>(rorg));
        for (unsigned d : data) { b.push_back(static_cast<std::uint8_t>(d)); }
        b.push_back(static_cast<std::uint8_t>(id >> 24)); b.push_back(static_cast<std::uint8_t>(id >> 16));
        b.push_back(static_cast<std::uint8_t>(id >> 8)); b.push_back(static_cast<std::uint8_t>(id));
        b.push_back(static_cast<std::uint8_t>(status));
        return b;
    };
    std::vector<std::vector<int>> frames;
    std::vector<double> gaps;
    const std::vector<std::vector<std::uint8_t>> tel = {body(0xF6, {0x30}, 0xB0), body(0xD5, {0x09}, 0x80), body(0xA5, {0x00, 0x00, 0x55, 0x08}, 0x80)};
    Rng rng(o.seed);
    for (std::size_t t = 0; t < tel.size(); ++t) {
        for (int sub = 0; sub < 3; ++sub) {
            frames.push_back(eno::frameBits(tel[t]));
            gaps.push_back(sub == 2 ? 300000.0 : 10000.0 + 30000.0 * rng.uniform());
        }
    }
    std::vector<double> startUs;
    double cur = 40000.0, endUs = 0.0;
    for (std::size_t f = 0; f < frames.size(); ++f) {
        startUs.push_back(cur);
        endUs = cur + static_cast<double>(frames[f].size()) * bitUs;
        cur = endUs + gaps[f];
    }
    const std::size_t n = static_cast<std::size_t>((endUs + 20000.0) * fs / 1e6);
    Iq r;
    r.rate = fs;
    r.i.resize(n);
    r.q.resize(n);
    const double sigma = std::sqrt(1.0 / std::pow(10.0, std::min(o.snrDb, 30.0) / 10.0) / 2.0);
    Rng g(o.seed * 31u + 5u);
    std::size_t fi = 0;
    for (std::size_t k = 0; k < n; ++k) {
        const double us = static_cast<double>(k) * 1e6 / fs;
        while (fi + 1 < frames.size() && us >= startUs[fi] + static_cast<double>(frames[fi].size()) * bitUs + 40.0) { ++fi; }
        double a = 0.0;
        const double s = startUs[fi], e = s + static_cast<double>(frames[fi].size()) * bitUs;
        if (us >= s - 40.0 && us < s) {
            a = low;
        } else if (us >= s && us < e) {
            const double rel = (us - s) / bitUs;
            const std::size_t j = static_cast<std::size_t>(rel);
            const auto lv = [&](std::size_t q) { return frames[fi][q] == 0 ? 1.0 : low; };
            a = lv(j);
            const double fr = (rel - static_cast<double>(j)) * bitUs, half = 0.75;
            if (fr < half && j > 0) { a = lv(j - 1) + (lv(j) - lv(j - 1)) * (0.5 + fr / 1.5); }
            else if (fr > bitUs - half && j + 1 < frames[fi].size()) { a = lv(j) + (lv(j + 1) - lv(j)) * (0.5 - (bitUs - fr) / 1.5); }
        } else if (us >= e && us < e + 40.0) {
            a = low;
        }
        const double ph = 2.0 * kPi * (2000.0 + o.cfoHz) * us * 1e-6;
        r.i[k] = static_cast<float>(a * std::cos(ph) + sigma * g.gauss());
        r.q[k] = static_cast<float>(a * std::sin(ph) + sigma * g.gauss());
    }
    return r;
}

}  // namespace feedgen
