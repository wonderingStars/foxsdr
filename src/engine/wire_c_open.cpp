// wire_c_open.cpp - see wire_c_open.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/wire_c_open.hpp"

#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace cascade::engine::wirec {

std::filesystem::path libraryFolderFor(const std::filesystem::path& base, const std::string& pluginName) {
    std::string safe;
    for (const unsigned char ch : pluginName) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
        safe += ok ? static_cast<char>(ch) : '_';
    }
    bool onlyDots = true;
    for (const char ch : safe) { onlyDots = onlyDots && ch == '.'; }
    if (safe.empty() || onlyDots) { safe = "plugin"; }
    if (safe.size() > 64) { safe.resize(64); }
    return base / "plugin-libs" / safe;
}

bool openWithSystem(const std::string& what, bool isFolder, std::string& why) {
#if defined(_WIN32)
    std::wstring wide;
    wide.reserve(what.size());
    for (const unsigned char ch : what) { wide += static_cast<wchar_t>(ch); }   // the address is printable ASCII; a folder path is converted below
    if (isFolder) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, what.c_str(), -1, nullptr, 0);
        if (n <= 0) {
            why = "the folder name could not be converted";
            return false;
        }
        wide.assign(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, what.c_str(), -1, wide.data(), n);
        wide.resize(static_cast<std::size_t>(n) - 1u);
    }
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        why = isFolder ? "the folder could not be opened" : "the page could not be opened";
        return false;
    }
    return true;
#else
    (void)what;
    (void)isFolder;
    why = "opening is not available on this system";
    return false;
#endif
}

}  // namespace cascade::engine::wirec
