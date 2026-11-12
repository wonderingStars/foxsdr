// BIT-VIEW LIBRARY, part 1: the bit buffer type and its helpers.
//
// The whole library (src/core/bits/*.hpp) is host-side, header-only, plugin
// independent and pure: every function is a function of its arguments, holds no
// global state, touches no file or network and prints nothing. A bit is one byte
// holding 0 or 1 (readable, easy to slice; frames are short).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cascade::bits {

using Bits = std::vector<uint8_t>;  // every element is 0 or 1

// "1011 0001" -> bits. Spaces, '_' and '.' are ignored; any other character makes ok false
// and returns what was read before it.
inline Bits fromString(std::string_view s, bool* ok = nullptr) {
    Bits b;
    if (ok) *ok = true;
    for (char c : s) {
        if (c == '0' || c == '1') b.push_back(static_cast<uint8_t>(c - '0'));
        else if (c == ' ' || c == '_' || c == '.') continue;
        else {
            if (ok) *ok = false;
            break;
        }
    }
    return b;
}

inline std::string toString(const Bits& b, size_t group = 0) {
    std::string s;
    s.reserve(b.size() + (group ? b.size() / group : 0));
    for (size_t i = 0; i < b.size(); ++i) {
        if (group && i && i % group == 0) s.push_back(' ');
        s.push_back(b[i] ? '1' : '0');
    }
    return s;
}

inline Bits fromBytes(const std::vector<uint8_t>& bytes, bool msbFirst = true) {
    Bits b;
    b.reserve(bytes.size() * 8);
    for (uint8_t v : bytes)
        for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(msbFirst ? (v >> (7 - i)) & 1 : (v >> i) & 1));
    return b;
}

// Pack bits into bytes; a short tail is padded with zeros (the last byte's low bits for MSB first).
inline std::vector<uint8_t> toBytes(const Bits& b, bool msbFirst = true) {
    std::vector<uint8_t> out((b.size() + 7) / 8, 0);
    for (size_t i = 0; i < b.size(); ++i)
        if (b[i]) out[i / 8] = static_cast<uint8_t>(out[i / 8] | (msbFirst ? (0x80u >> (i % 8)) : (1u << (i % 8))));
    return out;
}

// n <= 64 bits starting at pos, as an unsigned number. Bits past the end read as 0.
inline uint64_t getField(const Bits& b, size_t pos, size_t n, bool msbFirst = true) {
    if (n > 64) n = 64;
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint64_t bit = (pos + i < b.size()) ? (b[pos + i] & 1u) : 0u;
        if (msbFirst) v = (v << 1) | bit;
        else v |= bit << i;
    }
    return v;
}

inline Bits slice(const Bits& b, size_t pos, size_t n) {
    if (pos >= b.size()) return {};
    if (n > b.size() - pos) n = b.size() - pos;
    return Bits(b.begin() + static_cast<std::ptrdiff_t>(pos), b.begin() + static_cast<std::ptrdiff_t>(pos + n));
}

inline Bits inverted(const Bits& b) {
    Bits o(b);
    for (auto& x : o) x = static_cast<uint8_t>(x ^ 1u);
    return o;
}

inline size_t hamming(const Bits& a, const Bits& b) {
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    size_t d = (a.size() > b.size() ? a.size() : b.size()) - n;
    for (size_t i = 0; i < n; ++i) d += (a[i] ^ b[i]) & 1u;
    return d;
}

// Reverse the bit order inside each whole 8-bit group (a short tail is left alone).
inline Bits reverseBitsInBytes(const Bits& b) {
    Bits o(b);
    for (size_t i = 0; i + 8 <= o.size(); i += 8)
        for (size_t k = 0; k < 4; ++k) std::swap(o[i + k], o[i + 7 - k]);
    return o;
}

inline unsigned parity64(uint64_t v) {
    v ^= v >> 32; v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return static_cast<unsigned>(v & 1u);
}

}  // namespace cascade::bits
