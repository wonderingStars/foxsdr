// typed_number.hpp - A NUMBER A PERSON TYPES MAY USE EITHER DECIMAL SIGN (engine lane E1, owner decision 73, 2026-11-16).
//
// THE RULE. What is DISPLAYED follows the person's Windows regional format (core/access_number_format.hpp, the Accessibility setting "Number format": "Follow the Windows regional
// settings", the default, or "Always use a dot"). What is TYPED is accepted in BOTH forms: "1,5" and "1.5" are the same number. What a machine reads - files, logs, exports, the
// settings file, a report, the engine's commands - keeps the dot always.
//
// typedNumberText() is the one step the typed side needs: a text with exactly one comma and no dot is read with the comma as the decimal sign. A text with both ("1,234.5", "1.234,5")
// is left alone (the caller's own parse decides: grouping is not guessed here), and so is a text with two commas ("1,234,567"): they are never read as a decimal sign.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>

namespace cascade::core {

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

}  // namespace cascade::core
