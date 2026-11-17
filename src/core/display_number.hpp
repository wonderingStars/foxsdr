// display_number.hpp - THE NUMBER FORMAT THE WINDOW SHOWS, readable from every unit that writes a readout (must-fix lane A, owner decision 73).
//
// What is DISPLAYED follows the person's Windows regional format (core/access_number_format.hpp; the Accessibility setting "Number format": follow Windows, the default, or always a dot); what a machine
// reads keeps the dot. host_logic.cpp (setNumberLocale) owns the choice and writes it here; a unit that formats a readout without linking host_logic (signal_logic.cpp) asks this header, so the SIGNAL page's
// readouts follow the same sign as the counter, the level and the markers do. With nothing set (a test, a bounded run) every function returns the plain dotted text it was given.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdio>
#include <cstring>
#include <string>

#include "core/access_number_format.hpp"

namespace cascade::core {

struct DisplayNumberFormat {
    bool local = false;                                   // true once a locale has been set
    access::NumberSymbols symbols;
    access::DigitPolicy policy = access::DigitPolicy::Latin;
};

inline DisplayNumberFormat& displayNumberFormat() {
    static DisplayNumberFormat f;
    return f;
}

inline void setDisplayNumberFormat(const access::NumberSymbols* symbols, access::DigitPolicy policy) {
    DisplayNumberFormat& f = displayNumberFormat();
    f.local = symbols != nullptr;
    if (symbols != nullptr) { f.symbols = *symbols; }
    f.policy = policy;
}

// `value` with `decimals` places in the shown format; the dotted "%.<decimals>f" when no format is set.
inline std::string displayNumber(double value, int decimals) {
    const DisplayNumberFormat& f = displayNumberFormat();
    if (f.local) { return access::formatNumber(value, decimals, f.symbols, f.policy); }
    char b[64];
    std::snprintf(b, sizeof(b), "%.*f", decimals < 0 ? 0 : decimals > 12 ? 12 : decimals, value);
    return b;
}

// `text` was built with a dotted "%.<decimals>f" of `value`: the first such figure is replaced by the shown format's. Nothing set, or the figure not found: the text is returned unchanged.
inline std::string localizeDisplayNumber(const std::string& text, double value, int decimals) {
    if (!displayNumberFormat().local) { return text; }
    char b[64];
    std::snprintf(b, sizeof(b), "%.*f", decimals < 0 ? 0 : decimals > 12 ? 12 : decimals, value);
    const std::size_t at = text.find(b);
    if (at == std::string::npos) { return text; }
    return text.substr(0, at) + displayNumber(value, decimals) + text.substr(at + std::strlen(b));
}

}  // namespace cascade::core
