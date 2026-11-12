// eol_fixture.hpp - where the stand-in vendor libraries of the osslibs lane are (see tests/CMakeLists.txt, "lane osslibs"), and a scratch folder
// with the process id in its name.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace eol {

namespace fs = std::filesystem;

inline fs::path exeDir() {
    wchar_t b[MAX_PATH * 2];
    const DWORD n = ::GetModuleFileNameW(nullptr, b, static_cast<DWORD>(sizeof b / sizeof b[0]));
    return fs::path(std::wstring(b, n)).parent_path();
}
// tests/<config>/test_x.exe -> tests/fixture_osslibs/<config>
inline fs::path fixtureDir() {
    const fs::path d = exeDir();
    return d.parent_path() / "fixture_osslibs" / d.filename();
}
inline fs::path depDir() { return fixtureDir().parent_path() / "dep" / fixtureDir().filename(); }
inline std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
inline fs::path scratch(const std::string& tag) {
    fs::path p = fs::temp_directory_path() / ("eol-" + tag + "-" + std::to_string(::GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}
inline void writeFile(const fs::path& p, const std::vector<std::uint8_t>& b) {
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}
inline std::vector<std::uint8_t> readFile(const fs::path& p) {
    std::ifstream i(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
}

}  // namespace eol
