// clipboard_files.cpp - see clipboard_files.hpp. (Moved here from gui/paste_files.cpp, docs 6.63; the bytes of the parse are unchanged.)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/clipboard_files.hpp"

#include <cstdint>
#include <cstring>

#include "core/utf8_text.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cascade::core {

namespace {

// UTF-16 code units (little-endian, as Windows stores them) to UTF-8, up to the
// first NUL or `units`. A lone surrogate becomes U+FFFD rather than a byte
// sequence no reader accepts.
std::string utf16ToUtf8(const unsigned char* p, std::size_t units) {
    std::string out;
    for (std::size_t i = 0; i < units; ++i) {
        unsigned int u = static_cast<unsigned int>(p[2 * i]) | (static_cast<unsigned int>(p[2 * i + 1]) << 8);
        if (u == 0) { break; }
        if (u >= 0xD800 && u <= 0xDBFF) {
            unsigned int lo = 0;
            if (i + 1 < units) {
                lo = static_cast<unsigned int>(p[2 * i + 2]) | (static_cast<unsigned int>(p[2 * i + 3]) << 8);
            }
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            } else {
                u = 0xFFFD;
            }
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            u = 0xFFFD;
        }
        utf8Append(out, u);
    }
    return out;
}

// The narrow branch: the system code page on Windows, the bytes as they are
// elsewhere (where no such block is ever made).
std::string narrowToUtf8(const unsigned char* p, std::size_t bytes) {
    std::size_t n = 0;
    while (n < bytes && p[n] != 0) { ++n; }
    if (n == 0) { return std::string(); }
#if defined(_WIN32)
    const int wn = ::MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(p), static_cast<int>(n),
                                         nullptr, 0);
    if (wn <= 0) { return std::string(); }
    std::wstring w(static_cast<std::size_t>(wn), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(p), static_cast<int>(n), w.data(), wn);
    const int un = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
    if (un <= 0) { return std::string(); }
    std::string out(static_cast<std::size_t>(un), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, out.data(), un, nullptr, nullptr);
    return out;
#else
    return std::string(reinterpret_cast<const char*>(p), n);
#endif
}

}  // namespace

std::string firstDroppedFile(const unsigned char* block, std::size_t bytes) {
    // DROPFILES: DWORD pFiles; POINT pt (two LONGs); BOOL fNC; BOOL fWide.
    constexpr std::size_t kHeader = 20;
    if (block == nullptr || bytes < kHeader) { return std::string(); }
    std::uint32_t offset = 0;
    std::uint32_t wide = 0;
    std::memcpy(&offset, block, 4);
    std::memcpy(&wide, block + 16, 4);
    if (offset >= bytes) { return std::string(); }
    const unsigned char* list = block + offset;
    const std::size_t avail = bytes - offset;
    if (wide != 0) {
        // A path is text up to a NUL unit; a list that runs off the end is not one.
        const std::size_t units = avail / 2;
        std::size_t n = 0;
        while (n < units && (list[2 * n] != 0 || list[2 * n + 1] != 0)) { ++n; }
        if (n == units) { return std::string(); }
        return utf16ToUtf8(list, n);
    }
    std::size_t n = 0;
    while (n < avail && list[n] != 0) { ++n; }
    if (n == avail) { return std::string(); }
    return narrowToUtf8(list, n);
}

std::string firstFileOnClipboard() {
#if defined(_WIN32)
    if (!::IsClipboardFormatAvailable(CF_HDROP)) { return std::string(); }
    if (!::OpenClipboard(nullptr)) { return std::string(); }
    std::string path;
    if (HANDLE h = ::GetClipboardData(CF_HDROP); h != nullptr) {
        if (const void* p = ::GlobalLock(h); p != nullptr) {
            path = firstDroppedFile(static_cast<const unsigned char*>(p), static_cast<std::size_t>(::GlobalSize(h)));
            ::GlobalUnlock(h);
        }
    }
    ::CloseClipboard();
    return path;
#else
    return std::string();
#endif
}

}  // namespace cascade::core
