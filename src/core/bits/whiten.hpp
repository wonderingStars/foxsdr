// BIT-VIEW LIBRARY, part 3: whitening and scramblers.
//
// Two textbook families, both generic over the polynomial:
//
//  * ADDITIVE (synchronous) whitening: the data is XORed with the output of a free-running
//    linear-feedback shift register that starts from a known seed. Applying it twice gives the
//    data back, so the same function whitens and de-whitens. Fibonacci form: the register holds
//    `degree` bits; the output bit is stage 0; the feedback is the parity of the stages in
//    `taps`; the register shifts right and the feedback enters at the top stage.
//
//  * SELF-SYNCHRONISING (multiplicative) scrambling: each transmitted bit is the data bit XOR
//    the bits transmitted a and b places earlier; the descrambler XORs the received bit with the
//    received bits a and b places earlier. It needs no seed (it locks after max(a, b) bits).
//
// Named forms, and where each is stated:
//   PN9 = x^9 + x^5 + 1, register all ones, 8 LSBs XORed per byte, one clock per bit: the Texas
//         Instruments sub-GHz whitening (application note SWRA322 and the CC1100/CC1150 datasheet
//         text, both read through search-result text; the sequence FF E1 1D 9A ED 85 33 24 EA 7A
//         D2 39 70 97 57 0A is asserted by test_bits_whiten as the first sixteen bytes and was
//         recalled, then matched by this implementation: its source page was NOT opened).
//   G3RUH = 1 + x^12 + x^17, self-synchronising, each bit XOR the bits sent 12 and 17 earlier
//         (James Miller G3RUH, amsat.org/articles/g3ruh/109.html, opened 2026-11-11).
//   V.27ter 1 + x^-6 + x^-7 and V.29 1 + x^-18 + x^-23: stated in two US patents quoted by a search
//         result; the ITU texts were NOT opened, so they are offered as plain tap pairs, not as
//         verified forms. No "CCITT" form is named here: no opened source states one under that name.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <vector>

#include "core/bits/bits.hpp"

namespace cascade::bits {

struct LfsrSpec {
    unsigned degree = 0;   // 1..63
    uint64_t taps = 0;     // stage i set = stage i feeds the feedback parity (stage 0 is the output)
};

inline constexpr LfsrSpec kPn9{9, (1ull << 0) | (1ull << 5)};         // x^9 + x^5 + 1
inline constexpr uint64_t kPn9Seed = 0x1FF;                            // all ones

inline bool lfsrValid(const LfsrSpec& s, uint64_t seed) {
    return s.degree >= 1 && s.degree <= 63 && s.taps != 0 && (s.taps >> s.degree) == 0 && (seed & ((1ull << s.degree) - 1)) != 0;
}

// The first n output bits of the register (empty for an invalid spec or an all-zero seed, which
// would stay at zero forever).
inline Bits lfsrSequence(const LfsrSpec& s, uint64_t seed, size_t n) {
    Bits out;
    if (!lfsrValid(s, seed)) return out;
    const uint64_t mask = (1ull << s.degree) - 1;
    uint64_t r = seed & mask;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(static_cast<uint8_t>(r & 1u));
        const uint64_t fb = parity64(r & s.taps);
        r = (r >> 1) | (fb << (s.degree - 1));
    }
    return out;
}

// Serial: bit k is XORed with output bit k.
inline Bits whitenBits(const Bits& in, const LfsrSpec& s, uint64_t seed) {
    const Bits seq = lfsrSequence(s, seed, in.size());
    if (seq.size() != in.size()) return in;  // invalid spec: hand the data back unchanged
    Bits out(in);
    for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>((out[i] ^ seq[i]) & 1u);
    return out;
}

// Byte-oriented (the TI chips): each byte is XORed with eight successive output bits, output bit k
// forming bit k of the XOR byte (so the first byte is the register's 8 LSBs).
inline std::vector<uint8_t> whitenBytes(const std::vector<uint8_t>& in, const LfsrSpec& s, uint64_t seed) {
    const Bits seq = lfsrSequence(s, seed, in.size() * 8);
    if (seq.size() != in.size() * 8) return in;
    std::vector<uint8_t> out(in);
    for (size_t i = 0; i < out.size(); ++i)
        for (int k = 0; k < 8; ++k) out[i] = static_cast<uint8_t>(out[i] ^ (seq[i * 8 + static_cast<size_t>(k)] << k));
    return out;
}

struct ScramblerTaps {
    unsigned a = 0, b = 0;  // delays in bits; 1 <= a < b <= 64
};
inline constexpr ScramblerTaps kG3ruh{12, 17};
inline constexpr ScramblerTaps kV27ter{6, 7};     // see the header: source not opened
inline constexpr ScramblerTaps kV29{18, 23};      // see the header: source not opened

inline bool tapsValid(const ScramblerTaps& t) { return t.a >= 1 && t.a < t.b && t.b <= 64; }

// Bits before the start of the stream read as 0 (a descrambler locks after t.b bits whatever
// those were; `scramble` and `descramble` agree when both start from the same zero history).
inline Bits scramble(const Bits& data, const ScramblerTaps& t) {
    Bits out;
    if (!tapsValid(t)) return data;
    out.reserve(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        const unsigned x = (i >= t.a ? out[i - t.a] : 0u) ^ (i >= t.b ? out[i - t.b] : 0u);
        out.push_back(static_cast<uint8_t>((data[i] ^ x) & 1u));
    }
    return out;
}

inline Bits descramble(const Bits& rx, const ScramblerTaps& t) {
    Bits out;
    if (!tapsValid(t)) return rx;
    out.reserve(rx.size());
    for (size_t i = 0; i < rx.size(); ++i) {
        const unsigned x = (i >= t.a ? rx[i - t.a] : 0u) ^ (i >= t.b ? rx[i - t.b] : 0u);
        out.push_back(static_cast<uint8_t>((rx[i] ^ x) & 1u));
    }
    return out;
}

// The user's "whitening" choice for the decoder chain (rawbuf.hpp).
enum class WhitenKind : uint8_t { None, AdditiveBits, AdditiveBytes, SelfSync };
struct WhitenSetting {
    WhitenKind kind = WhitenKind::None;
    LfsrSpec lfsr{};
    uint64_t seed = 0;
    ScramblerTaps taps{};
};

inline Bits applyWhitening(const Bits& in, const WhitenSetting& w) {
    switch (w.kind) {
        case WhitenKind::None: return in;
        case WhitenKind::AdditiveBits: return whitenBits(in, w.lfsr, w.seed);
        case WhitenKind::AdditiveBytes: {
            // whole bytes only; a short tail is passed through. Bits within a byte are MSB first
            // on the wire and the sequence's bit k is bit k (LSB = 0) of the XOR byte.
            const size_t whole = in.size() / 8 * 8;
            const auto bytes = toBytes(slice(in, 0, whole), true);
            auto out = whitenBytes(bytes, w.lfsr, w.seed);
            Bits r = fromBytes(out, true);
            r.insert(r.end(), in.begin() + static_cast<std::ptrdiff_t>(whole), in.end());
            return r;
        }
        case WhitenKind::SelfSync: return descramble(in, w.taps);
    }
    return in;
}

}  // namespace cascade::bits
