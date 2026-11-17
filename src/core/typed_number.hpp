// typed_number.hpp - A NUMBER A PERSON TYPES MAY USE EITHER DECIMAL SIGN (engine lane E1, owner decision 73, 2026-11-16).
//
// THE RULE. What is DISPLAYED follows the person's Windows regional format (core/access_number_format.hpp, the Accessibility setting "Number format": "Follow the Windows regional
// settings", the default, or "Always use a dot"). What is TYPED is accepted in BOTH forms: "1,5" and "1.5" are the same number. What a machine reads - files, logs, exports, the
// settings file, a report, the engine's commands - keeps the dot always.
//
// typedNumberText() is the one step the typed side needs: a text with exactly one comma and no dot is read with the comma as the decimal sign. A text with both ("1,234.5", "1.234,5")
// is left alone (the caller's own parse decides: grouping is not guessed here), and so is a text with two commas ("1,234,567"): they are never read as a decimal sign.
//
// ONE CASE IS REFUSED (must-fix lane A, owner decision 73 kept): on a computer whose Windows decimal sign is a dot, a text with ONE comma followed by exactly three digits ("14,200") is ambiguous and is refused
// with a plain sentence (typedCommaAmbiguous); on a computer whose decimal sign is a comma the comma stays the decimal sign and "14,200" is 14.2.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <string>

namespace cascade::core {

// THE PERSON'S DECIMAL SIGN, for the one case the two forms cannot be told apart (must-fix lane A, owner decision 73): "14,200" is fourteen point two on a computer whose Windows decimal sign is
// a comma and a thousands mark on one whose decimal sign is a dot. The window sets this once (host_logic setNumberFormat, from the same symbols that format what it shows); the default is the dot.
inline std::atomic<bool>& typedDecimalCommaFlag() {
    static std::atomic<bool> flag{false};
    return flag;
}
inline void setTypedDecimalComma(bool decimalIsComma) { typedDecimalCommaFlag().store(decimalIsComma); }
inline bool typedDecimalComma() { return typedDecimalCommaFlag().load(); }

// AMBIGUOUS: no dot, exactly ONE comma, at least one digit before it and exactly three digits right after it ("14,200", "-3,000 kHz"; not "14,2", "14,2000", ",200", "1,234,567", "1,234.5").
// On a dot locale such a text is refused with a plain sentence (it could be 14.2 or 14200); on a comma locale the comma is the decimal sign and nothing is ambiguous.
inline bool typedCommaAmbiguous(const std::string& text, bool decimalIsComma) {
    if (decimalIsComma) { return false; }
    std::size_t commas = 0;
    std::size_t at = std::string::npos;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '.') { return false; }
        if (text[i] == ',') { ++commas; at = i; }
    }
    if (commas != 1 || at == 0) { return false; }
    bool digitBefore = false;
    for (std::size_t i = 0; i < at; ++i) { if (text[i] >= '0' && text[i] <= '9') { digitBefore = true; } }
    if (!digitBefore) { return false; }
    std::size_t after = 0;
    for (std::size_t i = at + 1; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) { ++after; }
    return after == 3;
}
inline bool typedCommaAmbiguous(const std::string& text) { return typedCommaAmbiguous(text, typedDecimalComma()); }

inline std::string typedNumberText(const std::string& text) {
    std::size_t commas = 0;
    std::size_t at = std::string::npos;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '.') { return text; }
        if (text[i] == ',') { ++commas; at = i; }
    }
    if (commas != 1) { return text; }
    std::string out = text;
    out[at] = '.';
    return out;
}

// The same, for a window whose number format is known: an ambiguous text is returned unchanged (the caller refuses it with typedCommaAmbiguous first).
inline std::string typedNumberText(const std::string& text, bool decimalIsComma) {
    return typedCommaAmbiguous(text, decimalIsComma) ? text : typedNumberText(text);
}

}  // namespace cascade::core
