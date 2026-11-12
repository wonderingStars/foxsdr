// Tests for core/access_number_format.{hpp,cpp}: locale-aware number output and
// input. Part 1 is the pure formatter/parser against hand-written symbol sets.
// Part 2 reads the REAL Windows locale data for EXPLICIT locale names with
// LOCALE_NOUSEROVERRIDE, so the result does not depend on this PC's regional
// settings, and cross-checks the formatter against the OS's own
// GetNumberFormatEx.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/access_number_format.hpp"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "test_check.hpp"

namespace {

using namespace cascade::core::access;

NumberSymbols en() { return NumberSymbols{}; }
NumberSymbols de() { NumberSymbols s; s.name = "de-DE"; s.decimal = ","; s.group = "."; return s; }
NumberSymbols fr() {
    NumberSymbols s; s.name = "fr-FR"; s.decimal = ","; s.group = "\xE2\x80\xAF"; return s;  // U+202F
}
NumberSymbols hi() { NumberSymbols s; s.name = "hi-IN"; s.grouping = "3;2;0"; return s; }
NumberSymbols ar() {
    NumberSymbols s; s.name = "ar-xx";
    s.decimal = "\xD9\xAB"; s.group = "\xD9\xAC";  // U+066B, U+066C
    s.digitSubstitution = 2;
    s.nativeDigits = {"\xD9\xA0", "\xD9\xA1", "\xD9\xA2", "\xD9\xA3", "\xD9\xA4",
                      "\xD9\xA5", "\xD9\xA6", "\xD9\xA7", "\xD9\xA8", "\xD9\xA9"};
    return s;
}

void formatBasics() {
    CHECK(formatNumber(7074.0, 3, en()) == "7074.000");
    CHECK(formatNumber(7074.0, 3, de()) == "7074,000");
    CHECK(formatNumber(7.074, 3, fr()) == "7,074");
    CHECK(formatNumber(0.0, 0, en()).empty() == false);
    CHECK(formatNumber(0.0, 0, en()) == "0");
    CHECK(formatNumber(-12.5, 1, en()) == "-12.5");
    CHECK(formatNumber(-12.5, 1, de()) == "-12,5");
    CHECK(formatNumber(-0.04, 1, en()) == "0.0");        // rounds to zero: no sign
    CHECK(formatNumber(-0.0, 2, en()) == "0.00");
    CHECK(formatNumber(2.5, 0, en()) == "2");              // printf's round-half-even on the binary value
    CHECK(formatNumber(1.005, 2, en()) == "1.00");         // 1.005 is 1.00499999999999989...
    // decimals clamp
    CHECK(formatNumber(1.5, -3, en()) == "2");
    CHECK(formatNumber(1.0, 40, en()) == "1.000000000000");
    // not finite
    CHECK(formatNumber(std::nan(""), 2, en()).empty());
    CHECK(formatNumber(INFINITY, 2, en()).empty());
    CHECK(formatNumber(-INFINITY, 2, en()).empty());
}

void formatGrouping() {
    CHECK(formatNumber(1234567.891, 2, en(), DigitPolicy::Latin, true) == "1,234,567.89");
    CHECK(formatNumber(1234567.891, 2, de(), DigitPolicy::Latin, true) == "1.234.567,89");
    CHECK(formatNumber(1234567.891, 2, fr(), DigitPolicy::Latin, true) == "1\xE2\x80\xAF" "234\xE2\x80\xAF" "567,89");
    CHECK(formatNumber(1234567.891, 2, hi(), DigitPolicy::Latin, true) == "12,34,567.89");
    CHECK(formatNumber(300000000000.0, 0, hi(), DigitPolicy::Latin, true) == "3,00,00,00,00,000");
    CHECK(formatNumber(999.0, 0, en(), DigitPolicy::Latin, true) == "999");
    CHECK(formatNumber(1000.0, 0, en(), DigitPolicy::Latin, true) == "1,000");
    CHECK(formatNumber(0.5, 1, en(), DigitPolicy::Latin, true) == "0.5");
    // The four patterns from Microsoft's LOCALE_SGROUPING page.
    NumberSymbols s = en();
    s.grouping = "3;0"; CHECK(formatNumber(3e12, 0, s, DigitPolicy::Latin, true) == "3,000,000,000,000");
    s.grouping = "3;2;0"; CHECK(formatNumber(3e12, 0, s, DigitPolicy::Latin, true) == "30,00,00,00,00,000");
    s.grouping = "3"; CHECK(formatNumber(3e9, 0, s, DigitPolicy::Latin, true) == "3000000,000");
    s.grouping = "3;2"; CHECK(formatNumber(3e9, 0, s, DigitPolicy::Latin, true) == "30000,00,000");
    s.grouping = ""; CHECK(formatNumber(3e9, 0, s, DigitPolicy::Latin, true) == "3000000000");
    s.grouping = "x;y"; CHECK(formatNumber(3e9, 0, s, DigitPolicy::Latin, true) == "3000000000");
    s.grouping = "0"; CHECK(formatNumber(3e9, 0, s, DigitPolicy::Latin, true) == "3000000000");
    // Grouping is off unless asked.
    CHECK(formatNumber(1234567.0, 0, en()) == "1234567");
}

void formatDigitPolicy() {
    const NumberSymbols a = ar();
    CHECK(formatNumber(1234.5, 1, a, DigitPolicy::Latin) == "1234\xD9\xAB" "5");
    // Native: digits mapped, the decimal symbol left alone.
    CHECK(formatNumber(1234.5, 1, a, DigitPolicy::Native) ==
          "\xD9\xA1\xD9\xA2\xD9\xA3\xD9\xA4\xD9\xAB\xD9\xA5");
    CHECK(formatNumber(1234.5, 1, a, DigitPolicy::FromLocale) == formatNumber(1234.5, 1, a, DigitPolicy::Native));
    NumberSymbols none = a; none.digitSubstitution = 1;
    CHECK(formatNumber(1234.5, 1, none, DigitPolicy::FromLocale) == formatNumber(1234.5, 1, none, DigitPolicy::Latin));
    NumberSymbols ctx = a; ctx.digitSubstitution = 0;   // context-based: Latin (no context)
    CHECK(formatNumber(1.0, 0, ctx, DigitPolicy::FromLocale) == "1");
    // grouped native digits keep the group symbol
    CHECK(formatNumber(1234567.0, 0, a, DigitPolicy::Native, true) ==
          "\xD9\xA1\xD9\xAC\xD9\xA2\xD9\xA3\xD9\xA4\xD9\xAC\xD9\xA5\xD9\xA6\xD9\xA7");
}

void formatNegativePatterns() {
    NumberSymbols s = en();
    s.negativeFormat = 0; CHECK(formatNumber(-5.5, 1, s) == "(5.5)");
    s.negativeFormat = 1; CHECK(formatNumber(-5.5, 1, s) == "-5.5");
    s.negativeFormat = 2; CHECK(formatNumber(-5.5, 1, s) == "- 5.5");
    s.negativeFormat = 3; CHECK(formatNumber(-5.5, 1, s) == "5.5-");
    s.negativeFormat = 4; CHECK(formatNumber(-5.5, 1, s) == "5.5 -");
    s.negativeFormat = 99; CHECK(formatNumber(-5.5, 1, s) == "-5.5");   // unknown: the plain pattern
    s.negativeFormat = 3; CHECK(formatNumber(5.5, 1, s) == "5.5");
}

void parseTable() {
    struct C { const char* text; NumberSymbols sym; bool lenient; bool ok; double v; };
    const C cases[] = {
        {"7074", en(), true, true, 7074.0},
        {"7074.5", en(), true, true, 7074.5},
        {"7,074.5", en(), true, true, 7074.5},
        {"7.074,5", en(), true, true, 7074.5},          // lenient: the last separator is the decimal
        {"1,234", en(), true, true, 1234.0},            // locale group followed by three digits
        {"1.234", de(), true, true, 1234.0},            // same rule in German
        {"1,5", en(), true, true, 1.5},                 // not the group (one digit follows): decimal
        {"7,074", fr(), true, true, 7.074},
        {"7,074", de(), true, true, 7.074},
        {"1.234,56", de(), true, true, 1234.56},
        {"1 234,56", fr(), true, true, 1234.56},
        {"1\xE2\x80\xAF" "234,56", fr(), true, true, 1234.56},
        {"1\xC2\xA0" "234,56", fr(), true, true, 1234.56},
        {"12,34,567.89", hi(), true, true, 1234567.89},
        {"12,34,567.89", hi(), false, true, 1234567.89},
        {"1,234,567", en(), true, true, 1234567.0},
        {"-12.5", en(), true, true, -12.5},
        {"\xE2\x88\x92" "12.5", en(), true, true, -12.5},    // U+2212 minus
        {"+12.5", en(), true, true, 12.5},
        {"(12.5)", en(), true, true, -12.5},
        {".5", en(), true, true, 0.5},
        {",5", en(), true, true, 0.5},                  // lenient: one digit after, so not a group
        {"12.", en(), true, true, 12.0},
        {"  42  ", en(), true, true, 42.0},
        // Native digits in, whatever the locale
        {"\xD9\xA1\xD9\xA2\xD9\xA3\xD9\xA4\xD9\xAB\xD9\xA5", ar(), true, true, 1234.5},
        {"\xDB\xB1\xDB\xB2", en(), true, true, 12.0},                       // Extended Arabic-Indic
        {"\xE0\xA5\xA7\xE0\xA5\xA8", hi(), true, true, 12.0},               // Devanagari
        {"\xEF\xBC\x91\xEF\xBC\x92", en(), true, true, 12.0},               // fullwidth
        {"1234", en(), false, true, 1234.0},
        // strict refuses what lenient forgives
        {"7.074,5", en(), false, false, 0.0},
        {"1,23,456", en(), false, false, 0.0},                              // wrong group sizes for 3;0
        {"1,5", en(), false, false, 0.0},                                    // comma is the group in en, one digit follows
        {"12,34,567.89", en(), false, false, 0.0},
        {"1.5", de(), false, false, 0.0},                                    // '.' is German's group, one digit follows
        // refused outright
        {"", en(), true, false, 0.0},
        {"   ", en(), true, false, 0.0},
        {"abc", en(), true, false, 0.0},
        {"12abc", en(), true, false, 0.0},
        {"1e5", en(), true, false, 0.0},
        {"0x10", en(), true, false, 0.0},
        {"-", en(), true, false, 0.0},
        {".", en(), true, false, 0.0},
        {",", en(), true, false, 0.0},
        {"1..2", en(), true, false, 0.0},
        {",5", en(), false, false, 0.0},   // strict: a leading group separator
        {"1,2,3.4", en(), true, false, 0.0},
        {"1.2.3,4,5", en(), true, false, 0.0},
        {"(12.5", en(), true, false, 0.0},
        {"--1", en(), true, false, 0.0},
        {"1-2", en(), true, false, 0.0},
        {"1.5.", en(), true, false, 0.0},
        {"nan", en(), true, false, 0.0},
        {"inf", en(), true, false, 0.0},
    };
    for (const C& c : cases) {
        const std::optional<double> r = parseNumber(c.text, c.sym, c.lenient);
        if (c.ok) {
            CHECK(r.has_value());
            if (r.has_value()) { CHECK_NEAR(*r, c.v, 1e-9); }
            else { std::printf("  (case '%s' in %s)\n", c.text, c.sym.name.c_str()); }
        } else {
            CHECK(!r.has_value());
            if (r.has_value()) { std::printf("  (case '%s' in %s parsed as %g)\n", c.text, c.sym.name.c_str(), *r); }
        }
    }
}

void roundTripAcrossLocales() {
    std::mt19937_64 rng(12345);
    std::uniform_real_distribution<double> mant(-1.0, 1.0);
    std::uniform_int_distribution<int> expo(-3, 12), dec(0, 6);
    const NumberSymbols locs[] = {en(), de(), fr(), hi(), ar()};
    const DigitPolicy pols[] = {DigitPolicy::Latin, DigitPolicy::Native, DigitPolicy::FromLocale};
    for (const NumberSymbols& s : locs) {
        for (DigitPolicy p : pols) {
            for (int i = 0; i < 400; ++i) {
                const double v = mant(rng) * std::pow(10.0, expo(rng));
                const int d = dec(rng);
                const bool grouped = (i % 2) == 0;
                const std::string t = formatNumber(v, d, s, p, grouped);
                const std::optional<double> r = parseNumber(t, s, true);
                CHECK(r.has_value());
                if (!r.has_value()) { std::printf("  (round trip lost '%s' in %s)\n", t.c_str(), s.name.c_str()); continue; }
                const double scale = std::pow(10.0, d);
                CHECK(std::fabs(*r - v) <= 0.5 / scale + 1e-9 * std::fabs(v));
                // strict mode reads what the formatter wrote for the same locale
                const std::optional<double> st = parseNumber(t, s, false);
                CHECK(st.has_value());
            }
        }
    }
}

void fuzzNeverCrashes() {
    std::mt19937_64 rng(777);
    const NumberSymbols locs[] = {en(), de(), fr(), hi(), ar()};
    std::uniform_int_distribution<int> len(0, 24), byte(0, 255);
    const char alphabet[] = "0123456789.,- +()\xC2\xA0\xE2\x80\xAF\xD9\xAB\xD9\xAC\xE0\xA5\xA7";
    for (int i = 0; i < 20000; ++i) {
        std::string s;
        const int n = len(rng);
        for (int k = 0; k < n; ++k) {
            s += (i % 3 == 0) ? static_cast<char>(byte(rng))
                              : alphabet[static_cast<std::size_t>(byte(rng)) % (sizeof alphabet - 1)];
        }
        for (const NumberSymbols& sym : locs) {
            const auto a = parseNumber(s, sym, true);
            const auto b = parseNumber(s, sym, false);
            if (a.has_value()) { CHECK(std::isfinite(*a)); }
            if (b.has_value()) { CHECK(std::isfinite(*b)); }
        }
    }
    CHECK(true);  // reaching here is the assertion: no crash, no hang
}

#ifdef _WIN32
std::string hexOf(const std::string& s) {
    std::string out;
    char b[8];
    for (unsigned char c : s) { std::snprintf(b, sizeof b, "%02X", c); out += b; }
    return out;
}

void windowsLocales() {
    const char* names[] = {"en-GB", "de-DE", "fr-FR", "ar-SA", "hi-IN"};
    NumberSymbols got[5];
    for (int i = 0; i < 5; ++i) {
        const bool ok = loadNumberSymbols(names[i], false, got[i]);
        CHECK(ok);
        if (!ok) { std::printf("  (locale %s not available)\n", names[i]); continue; }
        CHECK(got[i].name == names[i]);
        CHECK(!got[i].decimal.empty());
        std::printf("  %s: decimal=%s group=%s grouping=%s neg=%s negfmt=%d digitsub=%d digits0=%s\n",
                    names[i], hexOf(got[i].decimal).c_str(), hexOf(got[i].group).c_str(),
                    got[i].grouping.c_str(), hexOf(got[i].negative).c_str(), got[i].negativeFormat,
                    got[i].digitSubstitution, hexOf(got[i].nativeDigits[0]).c_str());
    }
    // Facts that hold for these locales on every Windows 10/11 locale database.
    CHECK(got[0].decimal == "." && got[0].group == "," && got[0].grouping == "3;0");   // en-GB
    CHECK(got[1].decimal == "," && got[1].group == "." && got[1].grouping == "3;0");   // de-DE
    CHECK(got[2].decimal == ",");                                                      // fr-FR
    // fr-FR's group is a space of some kind (NBSP U+00A0 or narrow NBSP U+202F by OS version).
    CHECK(got[2].group == "\xC2\xA0" || got[2].group == "\xE2\x80\xAF");
    CHECK(got[4].grouping == "3;2;0");                                                 // hi-IN: lakh grouping
    CHECK(got[4].decimal == ".");
    // ar-SA: the symbols exist and ten native digits were read; their identity is
    // reported above, not asserted (differs across OS versions).
    for (const std::string& d : got[3].nativeDigits) { CHECK(!d.empty()); }

    // Format through the real data and compare with GetNumberFormatEx, the OS's
    // own formatter, for the same locale with no user override: 1234567.891 at
    // the locale's default digits (Latin policy, grouped).
    for (int i = 0; i < 5; ++i) {
        if (i == 3) { continue; }  // ar-SA: GetNumberFormatEx may apply digit substitution; reported only
        wchar_t wname[32];
        for (int k = 0; k < 32; ++k) { wname[k] = static_cast<wchar_t>(names[i][k]); if (!names[i][k]) { break; } }
        wchar_t out[128];
        const int n = GetNumberFormatEx(wname, LOCALE_NOUSEROVERRIDE, L"1234567.891", nullptr, out, 128);
        CHECK(n > 0);
        if (n <= 0) { continue; }
        // digits after the decimal: the locale's IDIGITS (default 2)
        wchar_t dig[16];
        GetLocaleInfoEx(wname, LOCALE_IDIGITS | LOCALE_NOUSEROVERRIDE, dig, 16);
        const int decimals = _wtoi(dig);
        const std::string mine = formatNumber(1234567.891, decimals, got[i], DigitPolicy::Latin, true);
        int bytes = WideCharToMultiByte(CP_UTF8, 0, out, -1, nullptr, 0, nullptr, nullptr);
        std::string os(static_cast<std::size_t>(bytes > 0 ? bytes - 1 : 0), '\0');
        WideCharToMultiByte(CP_UTF8, 0, out, -1, os.data(), bytes, nullptr, nullptr);
        CHECK(mine == os);
        if (mine != os) { std::printf("  (%s: mine=%s os=%s)\n", names[i], hexOf(mine).c_str(), hexOf(os).c_str()); }
        // and the parser reads the OS's own output back
        const auto back = parseNumber(os, got[i], false);
        CHECK(back.has_value());
        if (back.has_value()) { CHECK_NEAR(*back, 1234567.891, 0.5 / std::pow(10.0, decimals) + 1e-9); }
    }

    // Unknown and malformed names are refused, never defaulted.
    NumberSymbols junk = en();
    CHECK(!loadNumberSymbols("", false, junk));
    CHECK(!loadNumberSymbols("xx-notalocale-ZZ", false, junk));
    CHECK(!loadNumberSymbols(std::string(200, 'a'), false, junk));
    CHECK(junk.name == "en-US");   // untouched on failure
}
#endif

}  // namespace

int main() {
    formatBasics();
    formatGrouping();
    formatDigitPolicy();
    formatNegativePatterns();
    parseTable();
    roundTripAcrossLocales();
    fuzzNeverCrashes();
#ifdef _WIN32
    windowsLocales();
#else
    SKIP_LINUX("Windows locale API (GetLocaleInfoEx) is not present");
#endif
    return testSummary("test_access_number_format");
}
