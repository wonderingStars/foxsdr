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

std::string utf8OrAnsiToUtf8(std::string_view s) {
    if (isValidUtf8(s)) { return std::string(s); }
    return ansiToUtf8(s);
}

}  // namespace cascade::core
