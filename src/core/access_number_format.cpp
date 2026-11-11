// access_number_format.cpp - the Windows locale reader behind
// access_number_format.hpp. GetLocaleInfoEx (winnls.h) for an explicit locale
// name; LOCALE_NOUSEROVERRIDE only when the caller asks to ignore the user's
// Region settings (tests do; the application does not).
//
// Nothing here shows a window, opens a registry key for writing or touches a
// setting: it only reads locale data.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/access_number_format.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace cascade::core::access {

#ifdef _WIN32
namespace {

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) { return std::string(); }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) { return std::string(); }
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring toWide(const std::string& s) {
    if (s.empty()) { return std::wstring(); }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) { return std::wstring(); }
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

bool info(const wchar_t* name, LCTYPE type, DWORD overrideFlag, std::wstring& out) {
    wchar_t buf[128];
    const int n = GetLocaleInfoEx(name, type | overrideFlag, buf, 128);
    if (n <= 0) { return false; }
    out.assign(buf, static_cast<std::size_t>(n - 1));  // n includes the terminator
    return true;
}

bool infoInt(const wchar_t* name, LCTYPE type, DWORD overrideFlag, int& out) {
    std::wstring s;
    if (!info(name, type, overrideFlag, s) || s.empty()) { return false; }
    out = _wtoi(s.c_str());
    return true;
}

}  // namespace
#endif

bool loadNumberSymbols(const std::string& localeName, bool honourUserOverride, NumberSymbols& out) {
#ifdef _WIN32
    if (localeName.empty() || localeName.size() > 84) { return false; }  // LOCALE_NAME_MAX_LENGTH is 85
    const std::wstring wname = toWide(localeName);
    if (wname.empty() || !IsValidLocaleName(wname.c_str())) { return false; }
    const DWORD ov = honourUserOverride ? 0 : LOCALE_NOUSEROVERRIDE;
    NumberSymbols s;
    s.name = localeName;
    std::wstring w;
    if (!info(wname.c_str(), LOCALE_SDECIMAL, ov, w) || w.empty()) { return false; }
    s.decimal = toUtf8(w);
    if (info(wname.c_str(), LOCALE_STHOUSAND, ov, w)) { s.group = toUtf8(w); } else { s.group.clear(); }
    if (info(wname.c_str(), LOCALE_SGROUPING, ov, w)) { s.grouping = toUtf8(w); }
    if (info(wname.c_str(), LOCALE_SNEGATIVESIGN, ov, w) && !w.empty()) { s.negative = toUtf8(w); }
    int v = 0;
    if (infoInt(wname.c_str(), LOCALE_INEGNUMBER, ov, v)) { s.negativeFormat = v; }
    if (infoInt(wname.c_str(), LOCALE_IDIGITSUBSTITUTION, ov, v)) { s.digitSubstitution = v; }
    if (info(wname.c_str(), LOCALE_SNATIVEDIGITS, ov, w) && w.size() == 10) {
        for (std::size_t i = 0; i < 10; ++i) { s.nativeDigits[i] = toUtf8(std::wstring(1, w[i])); }
    }
    out = s;
    return true;
#else
    (void)localeName; (void)honourUserOverride; (void)out;
    return false;  // no Windows locale API on this platform
#endif
}

}  // namespace cascade::core::access
