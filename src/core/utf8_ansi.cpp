// utf8_ansi.cpp - Windows code page text to UTF-8 (see utf8_text.hpp for why).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/utf8_text.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <vector>

namespace cascade::core {

namespace {

constexpr unsigned kUtf8CodePage = 65001;

// One run of bytes in a code page as UTF-8, or false when the code page refuses it (MB_ERR_INVALID_CHARS): the caller then
// puts U+FFFD for the run. Windows only.
bool convertRun(std::string_view s, unsigned codePage, std::string& out) {
#if defined(_WIN32)
    if (s.empty()) { out.clear(); return true; }
    const int wn = ::MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (wn <= 0) { return false; }
    std::vector<wchar_t> w(static_cast<std::size_t>(wn));
    if (::MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), w.data(), wn) != wn) { return false; }
    const int un = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
    if (un <= 0) { return false; }
    out.assign(static_cast<std::size_t>(un), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, out.data(), un, nullptr, nullptr) != un) { return false; }
    return isValidUtf8(out);
#else
    (void)codePage;
    out.clear();
    for (const char c : s) { utf8Append(out, static_cast<unsigned char>(c)); }   // Latin-1 off Windows
    return true;
#endif
}

bool isLeadByte(unsigned codePage, unsigned char b) {
#if defined(_WIN32)
    return ::IsDBCSLeadByteEx(codePage, b) != FALSE;
#else
    (void)codePage; (void)b;
    return false;
#endif
}

}  // namespace

std::string codePageToUtf8(std::string_view s, unsigned codePage) {
    if (s.empty()) { return std::string(); }
    if (codePage == kUtf8CodePage) { return sanitizeUtf8(s); }
    std::string out;
    if (convertRun(s, codePage, out)) { return out; }
    // The whole text has a byte the code page refuses: take it run by run with the same rule as the repair.
    return repairUtf8WithCodePage(s, codePage);
}

std::string ansiToUtf8(std::string_view s) {
#if defined(_WIN32)
    return codePageToUtf8(s, ::GetACP());
#else
    return codePageToUtf8(s, 0);
#endif
}

// Valid UTF-8 stays exactly as it is; each RUN of bytes that is not valid UTF-8 is read in the code page, as a whole, with
// MB_ERR_INVALID_CHARS. On a double-byte page (932, 936, 949, 950) the second byte of a character is often ASCII, which is
// valid UTF-8 on its own, so a run is extended over every lead byte and the byte after it: a Japanese word in Shift-JIS is
// converted as one piece, not as lead bytes among stray Latin letters. A run the code page refuses becomes U+FFFD per byte.
// With the UTF-8 code page itself (65001) there is nothing to read: U+FFFD straight away.
std::string repairUtf8WithCodePage(std::string_view s, unsigned codePage) {
    const std::size_t ok = utf8ValidUpTo(s);
    if (ok == s.size()) { return std::string(s); }
    if (codePage == kUtf8CodePage) { return sanitizeUtf8(s); }
    std::string out(s.substr(0, ok));
    std::size_t i = ok;
    while (i < s.size()) {
        const std::size_t n = utf8SeqLen(s, i);
        if (n != 0) {
            out.append(s.data() + i, n);
            i += n;
            continue;
        }
        std::size_t j = i;
        while (j < s.size()) {
            if (isLeadByte(codePage, static_cast<unsigned char>(s[j]))) {
                j = (j + 2 <= s.size()) ? j + 2 : s.size();   // the lead byte and its trail byte, whatever the trail looks like
            } else if (utf8SeqLen(s, j) == 0) {
                ++j;
            } else {
                break;
            }
        }
        std::string conv;
        if (convertRun(s.substr(i, j - i), codePage, conv)) {
            out += conv;
        } else {
            for (std::size_t k = i; k < j; ++k) { out += "\xEF\xBF\xBD"; }
        }
        i = j;
    }
    return out;
}

std::string utf8OrAnsiToUtf8(std::string_view s) {
#if defined(_WIN32)
    return repairUtf8WithCodePage(s, ::GetACP());
#else
    return repairUtf8WithCodePage(s, 0);
#endif
}

}  // namespace cascade::core
