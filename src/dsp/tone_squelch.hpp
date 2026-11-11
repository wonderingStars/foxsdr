// tone_squelch.hpp - the rules about a tone squelch's SETTINGS (engine wiring lane 1, group B): what a CTCSS tone or a DCS code may be, and how a DCS code is
// written. Pure and header-only, so the API's range check, the engine, the settings loader and the tests all ask one place.
//
// A CTCSS tone is one of the 50 of dsp/ctcss.hpp, or 0 for "any tone" (the gate follows whatever tone is locked). A DCS code is written the way people write it
// ("023", "754"): three octal digits read as an ordinary number (23, 754) - the form every control and file in the application uses - or 0 for "any code". The
// detector wants the OCTAL VALUE (023 as 023, which is 19), so digitsToOctal converts, refusing a digit past 7 or a number past 777. A code that is not one of the
// 83 standard ones is refused. A polarity is 0 any, 1 normal, 2 inverted (dsp::DcsDecoder::Polarity).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include "dsp/ctcss.hpp"
#include "dsp/dcs.hpp"

namespace cascade::dsp::tone {

inline constexpr int kOff = 0;
inline constexpr int kCtcss = 1;
inline constexpr int kDcs = 2;

// 0 (any tone) or one of the 50.
inline bool validCtcssHz(double hz) { return hz == 0.0 || ctcssIndexOfHz(hz) >= 0; }

// "023" as 23 -> octal 023 (19). False for a digit past 7, a negative number or one past 777.
inline bool digitsToOctal(int digits, unsigned& octal) {
    if (digits < 0 || digits > 777) { return false; }
    const int a = digits / 100, b = (digits / 10) % 10, c = digits % 10;
    if (a > 7 || b > 7 || c > 7) { return false; }
    octal = static_cast<unsigned>(a * 64 + b * 8 + c);
    return true;
}

// The octal value back to the digits as written (019 octal 023 -> 23).
inline int octalToDigits(unsigned octal) { return static_cast<int>(((octal >> 6) & 7u) * 100u + ((octal >> 3) & 7u) * 10u + (octal & 7u)); }

// 0 (any code) or one of the 83, written as digits.
inline bool validDcsDigits(int digits) {
    if (digits == 0) { return true; }
    unsigned octal = 0;
    return digitsToOctal(digits, octal) && dcsIsStandardCode(octal);
}

inline bool validPolarity(int p) { return p >= 0 && p <= 2; }

}  // namespace cascade::dsp::tone
