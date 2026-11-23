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

std::string codePageToUtf8(std::string_view s, unsigned codePage) {
    if (s.empty()) { return std::string(); }
#if defined(_WIN32)
    // No MB_ERR_INVALID_CHARS: a byte the code page has no letter for becomes a
    // replacement character in the wide text, which is the right outcome.
    const int wn = ::MultiByteToWideChar(codePage, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (wn > 0) {
        std::vector<wchar_t> w(static_cast<std::size_t>(wn));
        ::MultiByteToWideChar(codePage, 0, s.data(), static_cast<int>(s.size()), w.data(), wn);
        const int un = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
        if (un > 0) {
            std::string out(static_cast<std::size_t>(un), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, out.data(), un, nullptr, nullptr);
            return sanitizeUtf8(out);
        }
    }
    return sanitizeUtf8(s);
#else
    (void)codePage;
    // No code pages off Windows: read each byte as Latin-1.
    std::string out;
    out.reserve(s.size());
    for (const char c : s) { utf8Append(out, static_cast<unsigned char>(c)); }
    return out;
#endif
}

std::string ansiToUtf8(std::string_view s) {
#if defined(_WIN32)
    return codePageToUtf8(s, CP_ACP);
#else
    return codePageToUtf8(s, 0);
#endif
}

// Valid UTF-8 stays exactly as it is; each run of bytes that is not valid UTF-8 is read in the code page. A line that is
// mostly UTF-8 with one stray byte keeps its accents, and a line that is wholly ANSI ("El sistema no puede encontrar el
// archivo" with a 0xF3 in it) comes out as the right letters.
std::string repairUtf8WithCodePage(std::string_view s, unsigned codePage) {
    const std::size_t ok = utf8ValidUpTo(s);
    if (ok == s.size()) { return std::string(s); }
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
        while (j < s.size() && utf8SeqLen(s, j) == 0) { ++j; }
        out += codePageToUtf8(s.substr(i, j - i), codePage);
        i = j;
    }
    return out;
}

std::string utf8OrAnsiToUtf8(std::string_view s) {
#if defined(_WIN32)
    return repairUtf8WithCodePage(s, CP_ACP);
#else
    return repairUtf8WithCodePage(s, 0);
#endif
}

}  // namespace cascade::core
