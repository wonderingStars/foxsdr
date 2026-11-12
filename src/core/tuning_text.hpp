// Small text rules shared by the tuning lane's models: what a user-typed name, note, tag or mode may contain. Pure, header-only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cctype>
#include <cstddef>
#include <string>

namespace cascade::core::tuning {

inline bool validModeName(const std::string& m) {
    if (m.empty() || m.size() > 8) { return false; }
    for (const unsigned char c : m) {
        if (!(std::isalnum(c) || c == '+' || c == '-')) { return false; }
    }
    return true;
}

// Printable UTF-8 text without control characters, at most maxBytes. Used for labels and names typed by the user.
inline bool validUserText(const std::string& s, std::size_t maxBytes, bool allowNewline = false) {
    if (s.size() > maxBytes) { return false; }
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if (c < 0x80) {
            if (c < 0x20 && !(allowNewline && (c == '\n' || c == '\t'))) { return false; }
            if (c == 0x7f) { return false; }
        } else if (c >= 0xC2 && c <= 0xDF) { len = 2; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; }
        else { return false; }
        if (i + len > s.size()) { return false; }
        for (std::size_t k = 1; k < len; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) { return false; }
        }
        if (len == 3) {  // overlong and surrogates
            const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            if (c == 0xE0 && c1 < 0xA0) { return false; }
            if (c == 0xED && c1 >= 0xA0) { return false; }
        }
        if (len == 4) {
            const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            if (c == 0xF0 && c1 < 0x90) { return false; }
            if (c == 0xF4 && c1 >= 0x90) { return false; }
        }
        i += len;
    }
    return true;
}

}  // namespace cascade::core::tuning
