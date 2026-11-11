// sys_portable.cpp - see sys_portable.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/sys_portable.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace cascade::core::sys {
namespace {

std::string upper(std::string s) {
    for (auto& c : s) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
    return s;
}

bool reservedDeviceName(const std::string& name) {
    // The device names Windows reserves (with or without an extension): CON PRN AUX NUL COM1-9 LPT1-9.
    std::string stem = upper(name.substr(0, name.find('.')));
    while (!stem.empty() && stem.back() == ' ') { stem.pop_back(); }
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") { return true; }
    if (stem.size() == 4 && (stem.rfind("COM", 0) == 0 || stem.rfind("LPT", 0) == 0) && stem[3] >= '1' && stem[3] <= '9') { return true; }
    return false;
}

// Is `inner` (already made absolute) lexically inside `outer`?
bool lexicallyInside(const fs::path& outer, const fs::path& inner) {
    const fs::path o = outer.lexically_normal();
    const fs::path i = inner.lexically_normal();
    auto oi = o.begin();
    auto ii = i.begin();
    for (; oi != o.end(); ++oi, ++ii) {
        if (oi->empty()) { continue; }   // the trailing separator
        if (ii == i.end() || *oi != *ii) { return false; }
    }
    return true;
}

// Same, after resolving every link on the way.
bool physicallyInside(const fs::path& outer, const fs::path& inner) {
    std::error_code ec1, ec2;
    const fs::path o = fs::weakly_canonical(outer, ec1);
    const fs::path i = fs::weakly_canonical(inner, ec2);
    if (ec1 || ec2) { return false; }
    return lexicallyInside(o, i);
}

}  // namespace

bool validPortableFolderName(const std::string& name) {
    if (name.empty() || name.size() > 64) { return false; }
    if (name == "." || name == "..") { return false; }
    if (name.front() == ' ' || name.back() == ' ' || name.back() == '.') { return false; }
    for (unsigned char c : name) {
        const bool ok = std::isalnum(c) || c == '.' || c == '_' || c == '-' || c == ' ';
        if (!ok) { return false; }   // no separators, colons, wildcards, control or non-ASCII characters
    }
    if (name.find("..") != std::string::npos) { return false; }
    if (reservedDeviceName(name)) { return false; }
    return true;
}

PortablePaths resolvePortable(const fs::path& exeDir) {
    PortablePaths p;
    std::error_code ec;
    const fs::path marker = exeDir / kPortableMarkerName;
    const fs::file_status st = fs::symlink_status(marker, ec);
    if (ec || !fs::exists(st)) { return p; }   // no marker (or unreadable folder): not portable, no error
    p.markerFound = true;
    if (!fs::is_regular_file(st)) { p.error = "the portable marker is not a plain file"; return p; }
    const auto size = fs::file_size(marker, ec);
    if (ec || size > 256) { p.error = "the portable marker is unreadable or larger than 256 bytes"; return p; }
    std::ifstream in(marker, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // First line, trimmed of whitespace and a UTF-8 byte-order mark; empty = the default folder.
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) { text.erase(0, 3); }
    const auto nl = text.find_first_of("\r\n");
    if (nl != std::string::npos) { text.erase(nl); }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) { text.pop_back(); }
    std::size_t lead = 0;
    while (lead < text.size() && (text[lead] == ' ' || text[lead] == '\t')) { ++lead; }
    text.erase(0, lead);
    const std::string name = text.empty() ? std::string(kPortableDefaultFolder) : text;
    if (!validPortableFolderName(name)) {
        p.error = "the portable marker names a folder that is not a plain folder name";
        return p;
    }
    p.root = exeDir / name;
    p.settingsRoot = p.root / "settings";
    p.dataRoot = p.root / "data";
    p.logRoot = p.root / "logs";
    if (!lexicallyInside(exeDir, p.root)) { p.error = "the portable folder is not inside the program's folder"; p.root.clear(); p.settingsRoot.clear(); p.dataRoot.clear(); p.logRoot.clear(); return p; }
    p.portable = true;
    return p;
}

std::vector<std::pair<std::string, std::string>> portableEnvOverrides(const PortablePaths& p) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!p.portable) { return out; }
    out.emplace_back("APPDATA", p.settingsRoot.string());
    out.emplace_back("LOCALAPPDATA", p.dataRoot.string());
    return out;
}

bool ensurePortableRoots(const fs::path& exeDir, PortablePaths& p, std::string& error) {
    error.clear();
    if (!p.portable) { error = "not portable"; return false; }
    // The program's folder must itself exist and be a folder.
    std::error_code ec;
    if (!fs::is_directory(exeDir, ec)) { error = "the program's folder is not a folder"; return false; }
    for (const fs::path& dir : {p.root, p.settingsRoot, p.dataRoot, p.logRoot}) {
        // Before creating: where would it land, links resolved? (A pre-made link at the data folder's name that leads elsewhere is refused.)
        if (!lexicallyInside(exeDir, dir) || !physicallyInside(exeDir, dir)) { error = "a portable folder would leave the program's folder"; return false; }
        fs::create_directory(dir, ec);   // single level: the parent was made by the previous iteration
        if (ec) { error = "cannot create " + dir.filename().string() + ": " + ec.message(); return false; }
        if (!fs::is_directory(dir, ec) || ec) { error = dir.filename().string() + " is not a folder"; return false; }
        if (!physicallyInside(exeDir, dir)) { error = "a portable folder resolves outside the program's folder"; return false; }
    }
    return true;
}

fs::path currentExeDir() {
#ifdef _WIN32
    std::wstring buf(1024, L'\0');
    for (int tries = 0; tries < 6; ++tries) {
        const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) { return {}; }
        if (n < buf.size()) {
            buf.resize(n);
            return fs::path(buf).parent_path();
        }
        buf.resize(buf.size() * 2);
    }
#endif
    return {};
}

}  // namespace cascade::core::sys
