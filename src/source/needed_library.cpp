// needed_library.cpp - see needed_library.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/needed_library.hpp"

#include <filesystem>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#pragma comment(lib, "version.lib")
#endif

namespace cascade::source::needed {

const char* stateWord(State s) {
    switch (s) {
        case State::Working: return "working";
        case State::NotFound: return "not-found";
        case State::TooOld: return "too-old";
        case State::Wrong32Bit: return "wrong-32-bit";
        case State::WouldNotLoad: return "would-not-load";
        case State::NotALibrary: return "not-a-library";
    }
    return "unknown";
}

std::string fileVersion(const std::string& utf8Path) {
#ifdef _WIN32
    const std::filesystem::path p{std::u8string(utf8Path.begin(), utf8Path.end())};
    const std::wstring w = p.wstring();
    DWORD handle = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(w.c_str(), &handle);
    if (size == 0 || size > 1024 * 1024) { return {}; }
    std::vector<char> buf(size);
    if (!::GetFileVersionInfoW(w.c_str(), 0, size, buf.data())) { return {}; }
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!::VerQueryValueW(buf.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &len) || ffi == nullptr || len < sizeof(VS_FIXEDFILEINFO)) { return {}; }
    return std::to_string(HIWORD(ffi->dwFileVersionMS)) + "." + std::to_string(LOWORD(ffi->dwFileVersionMS)) + "." +
           std::to_string(HIWORD(ffi->dwFileVersionLS)) + "." + std::to_string(LOWORD(ffi->dwFileVersionLS));
#else
    (void)utf8Path;
    return {};
#endif
}

}  // namespace cascade::source::needed
