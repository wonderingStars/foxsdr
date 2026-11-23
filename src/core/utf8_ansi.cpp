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

unsigned systemAnsiCodePage() {
#if defined(_WIN32)
    return ::GetACP();
#else
    return 0;
#endif
}

bool isDoubleByteCodePage(unsigned codePage) {
#if defined(_WIN32)
    if (codePage == kUtf8CodePage) { return false; }
    CPINFO info{};
    return ::GetCPInfo(codePage, &info) != FALSE && info.MaxCharSize == 2 && info.LeadByte[0] != 0;
#else
    (void)codePage;
    return false;
#endif
}

bool isDbcsLeadByte(unsigned codePage, unsigned char b) { return isLeadByte(codePage, b); }

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
//
// DOUBLE-BYTE PAGES, WHICH READING IS RIGHT. Text that is not valid UTF-8 is either machine text in the code page (a Chinese
// Windows sentence) or real UTF-8 with a damaged byte. Many two-byte letters of a double-byte page are, by accident, a valid
// two-byte UTF-8 sequence (CF B5 CD B3, the Chinese word 系统, reads as two Greek-looking letters), so keeping every valid
// UTF-8 sequence would leave those letters wrong. The two are told apart by counting: when the bytes that are part of valid
// multi-byte UTF-8 sequences are at least three times the stray non-ASCII bytes, the text is UTF-8 with damage and every valid
// sequence is kept (a lead byte then does NOT take a following byte that starts a valid UTF-8 letter); otherwise the whole text
// is read in the code page, and only when the code page refuses a byte somewhere is it read run by run. Single-byte pages
// (1252, 1251) always take the run-by-run reading: there a valid UTF-8 letter between code-page letters is far more likely
// UTF-8 than a coincidence. Never invalid output, whatever the input.
std::string repairUtf8WithCodePage(std::string_view s, unsigned codePage) {
    const std::size_t ok = utf8ValidUpTo(s);
    if (ok == s.size()) { return std::string(s); }
    if (codePage == kUtf8CodePage) { return sanitizeUtf8(s); }
    bool keepValidSequences = true;   // false: do not let a lead byte swallow a byte that starts a valid UTF-8 letter
    if (isDoubleByteCodePage(codePage)) {
        std::size_t validBytes = 0;
        std::size_t strayBytes = 0;
        for (std::size_t i = 0; i < s.size();) {
            const std::size_t n = utf8SeqLen(s, i);
            if (n == 0) {
                ++strayBytes;
                ++i;
            } else {
                if (n > 1) { validBytes += n; }
                i += n;
            }
        }
        if (validBytes < 3 * strayBytes) {
            std::string whole;
            if (convertRun(s, codePage, whole)) { return whole; }   // every byte is a letter of the code page
            keepValidSequences = false;                              // the page refuses one somewhere: the run-by-run reading below, pairs first
        }
    }
    // A byte that starts a run to be read in the code page: one that is not valid UTF-8, and, when the text is read as the code page's
    // (keepValidSequences false), the valid multi-byte UTF-8 sequences too (they are letters of the page that look like UTF-8).
    const auto startsRun = [&](std::size_t k) {
        const std::size_t n = utf8SeqLen(s, k);
        return n == 0 || (!keepValidSequences && n > 1);
    };
    std::string out;
    out.reserve(s.size() + 8);
    std::size_t i = 0;
    while (i < s.size()) {
        if (!startsRun(i)) {
            const std::size_t n = utf8SeqLen(s, i);
            out.append(s.data() + i, n);
            i += n;
            continue;
        }
        std::size_t j = i;
        while (j < s.size()) {
            const std::size_t n = utf8SeqLen(s, j);
            if (keepValidSequences && n > 1) { break; }       // damaged UTF-8: a valid letter ends the run, whatever the page says of its first byte
            if (isLeadByte(codePage, static_cast<unsigned char>(s[j]))) {
                if (keepValidSequences && j + 1 < s.size() && utf8SeqLen(s, j + 1) > 1) {
                    ++j;                                      // the next byte starts a valid letter: it is not this lead byte's trail
                    break;
                }
                j = (j + 2 <= s.size()) ? j + 2 : s.size();   // the lead byte and its trail byte, whatever the trail looks like
            } else if (startsRun(j)) {
                j += (n > 1) ? n : 1;
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
