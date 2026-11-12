// access_number_format.hpp - locale-aware number output and input.
//
// Until now only INPUT accepted a decimal comma (host_logic.cpp parseFrequencyHz,
// freq_import.cpp); every readout printed "%.1f" with a dot, so a German or
// French user saw 7.074 where their own convention is 7,074. This file is the
// formatter/parser pair, split in two so that the arithmetic can be tested
// without the operating system:
//
//   1. NumberSymbols - the six facts a locale contributes: decimal separator,
//      group separator, grouping pattern, negative sign and pattern, and the
//      ten native digits with the digit-substitution preference.
//   2. formatNumber / parseNumber - pure functions of (value, symbols). No
//      Windows header is included here.
//
// loadNumberSymbols() (access_number_format.cpp) fills the symbols from the
// Windows locale API (GetLocaleInfoEx) for a locale NAME given explicitly, so a
// test names "de-DE" and gets German whatever this PC's regional settings are.
// `honourUserOverride` is false in every test (LOCALE_NOUSEROVERRIDE: Microsoft
// Learn, locale-nouseroverride, opened 2026-11-11) and true in the application,
// where the user's own Region settings are exactly what they asked for.
//
// GROUPING follows LOCALE_SGROUPING (Microsoft Learn, locale-sgrouping, opened
// 2026-11-11): sizes of each group of digits left of the decimal, separated by
// semicolons, an explicit size for each group, and "if the last value is 0, the
// preceding value is repeated": "3;0" is 3,000,000; "3;2;0" is the Indic
// 30,00,00,000; "3" is 3000000,000 (one group only); "3;2" is 30000000,00,000.
//
// DIGIT POLICY (the hi-IN / ar-SA question). Three policies, default Latin:
//   Latin      always ASCII 0-9. The default, for three reasons: a radio
//              frequency is read off an instrument face and the whole band
//              plan, bookmark and plugin vocabulary is ASCII digits; a typed
//              frequency must survive a round trip through a text box on a
//              keyboard that has no native digits; and the bundled UI fonts are
//              not shown to carry Arabic-Indic or Devanagari digit glyphs
//              (UNVERIFIED - screen lane K must check before offering Native).
//   Native     the locale's own ten digits whatever it prefers.
//   FromLocale native only where the locale's LOCALE_IDIGITSUBSTITUTION is 2
//              ("native digit substitution", Microsoft Learn,
//              locale-idigitsubstitution). Value 0 (context-based) is treated
//              as Latin: there is no preceding text to take a context from.
// The PARSER accepts Latin, Arabic-Indic (U+0660-0669), Extended Arabic-Indic
// (U+06F0-06F9), Devanagari (U+0966-096F) and fullwidth (U+FF10-FF19) digits
// whatever the policy, so a number typed or pasted in any of them is read.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace cascade::core::access {

enum class DigitPolicy : std::uint8_t { Latin = 0, Native, FromLocale };

struct NumberSymbols {
    std::string name = "en-US";   // the locale these came from (invariant-like default)
    std::string decimal = ".";    // UTF-8
    std::string group = ",";      // UTF-8; may be empty (no grouping)
    std::string grouping = "3;0"; // LOCALE_SGROUPING form
    std::string negative = "-";   // UTF-8
    int negativeFormat = 1;       // LOCALE_INEGNUMBER: 0 (1.1) 1 -1.1 2 - 1.1 3 1.1- 4 1.1 -
    int digitSubstitution = 1;    // LOCALE_IDIGITSUBSTITUTION: 0 context, 1 none, 2 native
    std::array<std::string, 10> nativeDigits = {"0", "1", "2", "3", "4",
                                                "5", "6", "7", "8", "9"};  // UTF-8
};

// Load from the OS for an explicit locale name; false when the name is not a
// valid locale or the platform has no locale API (non-Windows build).
bool loadNumberSymbols(const std::string& localeName, bool honourUserOverride, NumberSymbols& out);

namespace numdetail {

// Decode the UTF-8 sequence at s[i]; returns code point, advances i. Invalid
// bytes decode as U+FFFD one byte at a time.
inline std::uint32_t nextCp(const std::string& s, std::size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    auto cont = [&](std::size_t k) -> int {
        return (i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80)
                   ? (static_cast<unsigned char>(s[i + k]) & 0x3F) : -1;
    };
    if (c < 0x80) { ++i; return c; }
    if ((c & 0xE0) == 0xC0 && cont(1) >= 0) {
        const std::uint32_t cp = ((c & 0x1Fu) << 6) | static_cast<std::uint32_t>(cont(1));
        i += 2; return cp;
    }
    if ((c & 0xF0) == 0xE0 && cont(1) >= 0 && cont(2) >= 0) {
        const std::uint32_t cp = ((c & 0x0Fu) << 12) | (static_cast<std::uint32_t>(cont(1)) << 6) |
                                 static_cast<std::uint32_t>(cont(2));
        i += 3; return cp;
    }
    if ((c & 0xF8) == 0xF0 && cont(1) >= 0 && cont(2) >= 0 && cont(3) >= 0) {
        const std::uint32_t cp = ((c & 0x07u) << 18) | (static_cast<std::uint32_t>(cont(1)) << 12) |
                                 (static_cast<std::uint32_t>(cont(2)) << 6) |
                                 static_cast<std::uint32_t>(cont(3));
        i += 4; return cp;
    }
    ++i;
    return 0xFFFD;
}

inline std::vector<std::uint32_t> toCodePoints(const std::string& s) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < s.size();) { out.push_back(nextCp(s, i)); }
    return out;
}

// 0-9 for any supported digit script, -1 otherwise.
inline int digitValue(std::uint32_t cp) {
    if (cp >= '0' && cp <= '9') { return static_cast<int>(cp - '0'); }
    if (cp >= 0x0660 && cp <= 0x0669) { return static_cast<int>(cp - 0x0660); }
    if (cp >= 0x06F0 && cp <= 0x06F9) { return static_cast<int>(cp - 0x06F0); }
    if (cp >= 0x0966 && cp <= 0x096F) { return static_cast<int>(cp - 0x0966); }
    if (cp >= 0xFF10 && cp <= 0xFF19) { return static_cast<int>(cp - 0xFF10); }
    return -1;
}

inline bool isSpaceLike(std::uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == 0x00A0 || cp == 0x2009 || cp == 0x202F ||
           cp == 0x2007 || cp == 0x200E || cp == 0x200F || cp == 0x061C;  // incl. direction marks
}

// "3;2;0" -> {3,2,0}. A malformed or empty pattern is "no grouping" ({}).
inline std::vector<int> parseGrouping(const std::string& g) {
    std::vector<int> out;
    int cur = -1;
    for (char c : g) {
        if (c >= '0' && c <= '9') { cur = (cur < 0 ? 0 : cur * 10) + (c - '0'); if (cur > 99) { return {}; } }
        else if (c == ';') { if (cur < 0) { return {}; } out.push_back(cur); cur = -1; }
        else { return {}; }
    }
    if (cur >= 0) { out.push_back(cur); }
    return out;
}

// Insert `sep` into a string of ASCII digits per the pattern.
inline std::string groupDigits(const std::string& digits, const std::string& sep,
                               const std::vector<int>& pattern) {
    if (sep.empty() || pattern.empty() || pattern[0] <= 0) { return digits; }
    std::vector<std::string> parts;  // right to left
    std::size_t end = digits.size();
    std::size_t idx = 0;
    while (end > 0) {
        int size;
        if (idx < pattern.size()) {
            size = pattern[idx];
            if (size == 0) {  // trailing 0: repeat the preceding value
                size = pattern[idx - 1];  // idx>=1 here: pattern[0] > 0 checked above
            } else { ++idx; }
        } else {
            size = 0;  // pattern without a trailing 0: the rest is one group
        }
        if (size <= 0 || static_cast<std::size_t>(size) >= end) { parts.push_back(digits.substr(0, end)); break; }
        parts.push_back(digits.substr(end - static_cast<std::size_t>(size), static_cast<std::size_t>(size)));
        end -= static_cast<std::size_t>(size);
    }
    std::string out;
    for (std::size_t i = parts.size(); i-- > 0;) {
        out += parts[i];
        if (i > 0) { out += sep; }
    }
    return out;
}

// The size of the k-th group counting from the right (k = 0 is the group next to
// the decimal point), per the pattern; 0 means "the rest of the digits" (a
// pattern without a trailing 0 has no further groups).
inline int expectedGroupSize(const std::vector<int>& pat, std::size_t k) {
    if (pat.empty() || pat[0] <= 0) { return 0; }
    if (k < pat.size()) {
        if (pat[k] != 0) { return pat[k]; }
        return pat[k - 1];  // trailing 0: repeat; k >= 1 because pat[0] > 0
    }
    return pat.back() == 0 ? pat[pat.size() - 2] : 0;
}

inline std::string encodeDigits(const std::string& ascii, const NumberSymbols& sym, bool native) {
    if (!native) { return ascii; }
    std::string out;
    for (char c : ascii) {
        if (c >= '0' && c <= '9') { out += sym.nativeDigits[static_cast<std::size_t>(c - '0')]; }
        else { out += c; }
    }
    return out;
}

}  // namespace numdetail

// FORMAT. NaN and infinity give an empty string (an unknown value is the
// caller's to word). `decimals` is clamped to 0..12. A value that rounds to zero
// prints without a sign. Grouping is OFF unless asked: a frequency readout
// "7074.000" is read digit by digit and a group separator in it would be
// spoken as a pause - callers ask for grouping on big counts only.
inline std::string formatNumber(double value, int decimals, const NumberSymbols& sym,
                                DigitPolicy policy = DigitPolicy::Latin, bool grouped = false) {
    if (!std::isfinite(value)) { return std::string(); }
    if (decimals < 0) { decimals = 0; }
    if (decimals > 12) { decimals = 12; }
    char buf[400];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, std::fabs(value));
    const std::string ascii = buf;
    const std::size_t dot = ascii.find('.');
    std::string whole = ascii.substr(0, dot);
    const std::string frac = (dot == std::string::npos) ? std::string() : ascii.substr(dot + 1);
    bool zero = true;
    for (char c : ascii) { if (c >= '1' && c <= '9') { zero = false; break; } }
    const bool negative = std::signbit(value) && !zero;

    if (grouped) { whole = numdetail::groupDigits(whole, sym.group, numdetail::parseGrouping(sym.grouping)); }
    const bool native = (policy == DigitPolicy::Native) ||
                        (policy == DigitPolicy::FromLocale && sym.digitSubstitution == 2);
    // Only the ASCII digits are mapped; the separators (which may themselves be
    // non-ASCII) are left alone.
    std::string out = numdetail::encodeDigits(whole, sym, native);
    if (!frac.empty()) { out += sym.decimal + numdetail::encodeDigits(frac, sym, native); }
    if (!negative) { return out; }
    switch (sym.negativeFormat) {
        case 0: return "(" + out + ")";
        case 2: return sym.negative + " " + out;
        case 3: return out + sym.negative;
        case 4: return out + " " + sym.negative;
        default: return sym.negative + out;
    }
}

// PARSE. strict = only this locale's symbols (group separators must sit where
// the grouping pattern puts them). lenient additionally accepts the other of
// '.' / ',' as the decimal when that is unambiguous - see the rules below.
//
// Lenient rules, in order:
//   * digits of any supported script are read; spaces and direction marks are
//     dropped; '-', U+2212 and a leading '+' are signs; "(...)" is negative.
//   * both '.' and ',' present: the LAST separator is the decimal point, the
//     other kind must be a group separator (repeated or single); anything else
//     fails.
//   * one kind of separator, more than once: all are group separators; the
//     groups after the first must each be exactly 3 digits (2 for the lakh
//     pattern's inner groups is accepted when the locale grouping says so).
//   * one separator once: if it is the locale's decimal symbol it is the
//     decimal. If it is the locale's group symbol and exactly three digits
//     follow, it is a group separator (the locale wins: "1,234" in en-GB is
//     1234). Otherwise it is the decimal ("7,074" in a locale whose decimal
//     is '.' and whose group is not ',' reads as 7.074).
// Returns nullopt on empty input, no digits, or any character that is none of
// the above (so "12abc", "1e5" and "0x10" are refused, never half-read).
inline std::optional<double> parseNumber(const std::string& text, const NumberSymbols& sym,
                                         bool lenient = true) {
    const std::vector<std::uint32_t> cps = numdetail::toCodePoints(text);
    const std::vector<std::uint32_t> dec = numdetail::toCodePoints(sym.decimal);
    const std::vector<std::uint32_t> grp = numdetail::toCodePoints(sym.group);

    // Tokens: digit characters (as '0'-'9'), 'D' decimal-ish, 'G' group-ish
    // marker with the original symbol class.
    enum class Sep : std::uint8_t { Dot, Comma, LocaleDec, LocaleGrp };
    struct Tok { bool digit; char ch; Sep sep; };
    std::vector<Tok> toks;
    bool negative = false, sawSign = false, paren = false;
    std::size_t i = 0;
    auto matches = [&](const std::vector<std::uint32_t>& sym2) {
        if (sym2.empty() || i + sym2.size() > cps.size()) { return false; }
        for (std::size_t k = 0; k < sym2.size(); ++k) { if (cps[i + k] != sym2[k]) { return false; } }
        return true;
    };
    while (i < cps.size()) {
        const std::uint32_t cp = cps[i];
        const int dv = numdetail::digitValue(cp);
        if (dv >= 0) { toks.push_back({true, static_cast<char>('0' + dv), Sep::Dot}); ++i; continue; }
        if (numdetail::isSpaceLike(cp)) {
            // a space-like character that IS the group symbol still counts as a group
            if (matches(grp) && !(cp == ' ' && grp.size() == 1 && grp[0] != ' ')) {
                toks.push_back({false, 'G', Sep::LocaleGrp}); i += grp.size();
            } else { ++i; }
            continue;
        }
        if (cp == '(' && toks.empty() && !paren && !sawSign) { paren = true; negative = true; ++i; continue; }
        if (cp == ')' && paren && i + 1 == cps.size()) { ++i; paren = false; sawSign = true; continue; }
        if ((cp == '-' || cp == 0x2212) && toks.empty() && !sawSign) { negative = true; sawSign = true; ++i; continue; }
        if (cp == '-' && !toks.empty() && !sawSign && sym.negativeFormat >= 3 && i + 1 == cps.size()) {
            negative = true; sawSign = true; ++i; continue;  // trailing minus, as in some Arabic locales
        }
        if (cp == '+' && toks.empty() && !sawSign) { sawSign = true; ++i; continue; }
        if (matches(dec)) { toks.push_back({false, 'D', Sep::LocaleDec}); i += dec.size(); continue; }
        if (matches(grp)) { toks.push_back({false, 'G', Sep::LocaleGrp}); i += grp.size(); continue; }
        if (cp == '.') { toks.push_back({false, 'D', Sep::Dot}); ++i; continue; }
        if (cp == ',') { toks.push_back({false, 'D', Sep::Comma}); ++i; continue; }
        if (cp == 0x066B) { toks.push_back({false, 'D', Sep::LocaleDec}); ++i; continue; }   // Arabic decimal separator
        if (cp == 0x066C) { toks.push_back({false, 'G', Sep::LocaleGrp}); ++i; continue; }   // Arabic thousands separator
        return std::nullopt;
    }
    if (paren) { return std::nullopt; }

    // Classify each separator: decimal or group, then rebuild "iii.fff".
    std::vector<std::size_t> sepAt;
    for (std::size_t t = 0; t < toks.size(); ++t) { if (!toks[t].digit) { sepAt.push_back(t); } }
    bool anyDigit = false;
    for (const Tok& t : toks) { if (t.digit) { anyDigit = true; break; } }
    if (!anyDigit) { return std::nullopt; }

    auto isDecSym = [&](const Tok& t) {  // does this separator spell the locale's decimal?
        if (t.sep == Sep::LocaleDec) { return true; }
        if (t.sep == Sep::LocaleGrp) { return false; }
        // '.' or ',' typed: it spells the locale decimal when the locale decimal is that character
        if (t.sep == Sep::Dot) { return sym.decimal == "."; }
        return sym.decimal == ",";
    };
    auto isGrpSym = [&](const Tok& t) {
        if (t.sep == Sep::LocaleGrp) { return true; }
        if (t.sep == Sep::LocaleDec) { return false; }
        if (t.sep == Sep::Dot) { return sym.group == "."; }
        return sym.group == ",";
    };

    std::vector<bool> isDecimal(toks.size(), false);
    int decimalIndex = -1;
    if (!lenient) {
        for (std::size_t s : sepAt) {
            if (isDecSym(toks[s])) {
                if (decimalIndex >= 0) { return std::nullopt; }
                decimalIndex = static_cast<int>(s);
            } else if (!isGrpSym(toks[s])) { return std::nullopt; }
        }
    } else if (sepAt.size() == 1) {
        const Tok& t = toks[sepAt[0]];
        std::size_t after = 0;
        for (std::size_t k = sepAt[0] + 1; k < toks.size(); ++k) { if (toks[k].digit) { ++after; } }
        if (isDecSym(t)) { decimalIndex = static_cast<int>(sepAt[0]); }
        else if (isGrpSym(t) && after == 3) { decimalIndex = -1; }
        else { decimalIndex = static_cast<int>(sepAt[0]); }
    } else if (sepAt.size() > 1) {
        // The kinds present, by what they spell.
        auto kindOf = [](const Tok& t) { return t.sep == Sep::Dot ? 0 : (t.sep == Sep::Comma ? 1 : 2 + (t.sep == Sep::LocaleGrp ? 1 : 0)); };
        const int lastKind = kindOf(toks[sepAt.back()]);
        bool allSame = true;
        for (std::size_t s : sepAt) { if (kindOf(toks[s]) != lastKind) { allSame = false; } }
        if (allSame) {
            decimalIndex = -1;  // repeated: groups only
        } else {
            decimalIndex = static_cast<int>(sepAt.back());
            for (std::size_t s : sepAt) {
                if (static_cast<int>(s) != decimalIndex && kindOf(toks[s]) == kindOf(toks[sepAt.back()])) { return std::nullopt; }
            }
        }
    }
    if (decimalIndex >= 0) { isDecimal[static_cast<std::size_t>(decimalIndex)] = true; }

    // Rebuild "iii.fff", checking the integer part's groups as we go.
    std::string ascii;
    std::vector<std::size_t> runs;  // digit counts of the integer part, left to right
    std::size_t run = 0;
    bool inFraction = false;
    for (std::size_t t = 0; t < toks.size(); ++t) {
        if (toks[t].digit) {
            ascii += toks[t].ch;
            if (inFraction) { continue; }
            ++run;
            continue;
        }
        if (static_cast<int>(t) == decimalIndex) {
            runs.push_back(run); run = 0; inFraction = true; ascii += '.';
            continue;
        }
        if (inFraction) { return std::nullopt; }  // a separator after the decimal point
        if (run == 0) { return std::nullopt; }    // leading or doubled group separator
        runs.push_back(run); run = 0;
    }
    if (!inFraction) { runs.push_back(run); }
    if (runs.size() > 1) {
        const std::vector<int> pat = numdetail::parseGrouping(sym.grouping);
        // runs[0..n-1] are the integer groups left to right; with a decimal the
        // vector's last entry is the integer run just before it, so the same.
        const std::size_t n = runs.size();
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t fromRight = n - 1 - k;
            const std::size_t have = runs[k];
            if (have == 0) { return std::nullopt; }
            const int want = numdetail::expectedGroupSize(pat, fromRight);  // 0 = "the rest"
            if (k == 0) {
                // The head group may be shorter than a full group, never longer
                // (strict); lenient accepts a long head ("1234,567").
                if (!lenient && want > 0 && static_cast<int>(have) > want) { return std::nullopt; }
            } else if (want == 0) {
                if (!lenient) { return std::nullopt; }
            } else if (static_cast<int>(have) != want) {
                if (!(lenient && have == 3)) { return std::nullopt; }
            }
        }
    }
    if (ascii.empty() || ascii == ".") { return std::nullopt; }
    if (ascii.front() == '.') { ascii.insert(ascii.begin(), '0'); }
    if (ascii.back() == '.') { ascii.pop_back(); }
    char* endp = nullptr;
    const double v = std::strtod(ascii.c_str(), &endp);
    if (endp == nullptr || *endp != '\0' || !std::isfinite(v)) { return std::nullopt; }
    return negative ? -v : v;
}

}  // namespace cascade::core::access
