// vendor_lib.cpp - see vendor_lib.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/vendor_lib.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cascade::source::vendorlib {

namespace fs = std::filesystem;

namespace {

std::atomic<std::uint64_t> g_silentLoads{0};

std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

fs::path pathFromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string utf8FromPath(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

// The name only, printable ASCII, bounded: it goes into a sentence.
std::string shownName(const std::string& fileName) {
    std::string s;
    for (const unsigned char c : fileName) {
        if (s.size() >= 64) { break; }
        s += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
    }
    return s.empty() ? std::string("the library") : s;
}

std::string bitnessWord(std::uint32_t machine) {
    switch (machine) {
        case kMachineX86: return "32-bit";
        case kMachineX64: return "64-bit";
        case kMachineArm64: return "64-bit ARM";
        default: break;
    }
    return "unknown-processor";
}

Check bad(Verdict v, const std::string& sentence, std::uint32_t machine = 0) {
    Check c;
    c.verdict = v;
    c.sentence = sentence;
    c.machine = machine;
    return c;
}

}  // namespace

const char* verdictWord(Verdict v) {
    switch (v) {
        case Verdict::Ok: return "ok";
        case Verdict::NotFound: return "not-found";
        case Verdict::Unreadable: return "unreadable";
        case Verdict::NotAnImage: return "not-an-image";
        case Verdict::Truncated: return "truncated";
        case Verdict::NotALibrary: return "not-a-library";
        case Verdict::Wrong32Bit: return "wrong-32-bit";
        case Verdict::WrongMachine: return "wrong-machine";
        case Verdict::LoadFailed: return "load-failed";
        case Verdict::SymbolMissing: return "symbol-missing";
        case Verdict::NotThisPlatform: return "not-this-platform";
    }
    return "unknown";
}

std::uint32_t hostMachine() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return kMachineArm64;
#elif defined(_M_X64) || defined(__x86_64__)
    return kMachineX64;
#else
    return kMachineX86;
#endif
}

Check checkBytes(const std::uint8_t* d, std::size_t n, std::uint32_t host, const std::string& fileName) {
    const std::string nm = shownName(fileName);
    if (d == nullptr || n < 0x40 || d[0] != 'M' || d[1] != 'Z') {
        return bad(Verdict::NotAnImage, nm + " is not a Windows library (it does not begin like one).");
    }
    const std::uint32_t pe = le32(d + 0x3C);
    // PE signature + COFF header (20) must fit; the optional header and the section table are checked below.
    if (pe < 0x40 || pe > n || n - pe < 24) { return bad(Verdict::Truncated, nm + " is cut short: its headers run past the end of the file."); }
    if (std::memcmp(d + pe, "PE\0\0", 4) != 0) { return bad(Verdict::NotAnImage, nm + " is not a Windows library (no PE signature)."); }
    const std::uint8_t* coff = d + pe + 4;
    const std::uint32_t machine = le16(coff);
    const std::uint32_t sections = le16(coff + 2);
    const std::uint32_t optSize = le16(coff + 16);
    const std::uint32_t characteristics = le16(coff + 18);
    const std::size_t optAt = static_cast<std::size_t>(pe) + 24;
    if (optSize < 2 || optAt + optSize > n) { return bad(Verdict::Truncated, nm + " is cut short: its optional header runs past the end of the file.", machine); }
    const std::uint32_t magic = le16(d + optAt);
    if (magic != 0x10B && magic != 0x20B) { return bad(Verdict::NotAnImage, nm + " is not a Windows library (unknown image kind).", machine); }
    if (sections > 96) { return bad(Verdict::NotAnImage, nm + " is not a Windows library (an impossible section count).", machine); }
    const std::size_t tableAt = optAt + optSize;
    if (tableAt + static_cast<std::size_t>(sections) * 40u > n) {
        return bad(Verdict::Truncated, nm + " is cut short: its section table runs past the end of the file.", machine);
    }
    for (std::uint32_t i = 0; i < sections; ++i) {
        const std::uint8_t* s = d + tableAt + static_cast<std::size_t>(i) * 40u;
        const std::uint64_t rawSize = le32(s + 16);
        const std::uint64_t rawAt = le32(s + 20);
        if (rawSize != 0 && rawAt + rawSize > n) {
            return bad(Verdict::Truncated, nm + " is cut short: part of it is missing (an incomplete download?).", machine);
        }
    }
    if ((characteristics & 0x2000u) == 0) {
        return bad(Verdict::NotALibrary, nm + " is a program, not a library.", machine);
    }
    if ((machine == kMachineX64 || machine == kMachineArm64) != (magic == 0x20B) && machine != 0) {
        return bad(Verdict::NotAnImage, nm + " is not a Windows library (its headers disagree about the processor).", machine);
    }
    if (machine != host) {
        if (machine == kMachineX86 || host == kMachineX86) {
            return bad(Verdict::Wrong32Bit,
                       nm + " is a " + bitnessWord(machine) + " library and FoxSDR is " + bitnessWord(host) +
                           ", so it cannot be loaded here. FoxSDR does not load it; the maker's 64-bit file is needed.",
                       machine);
        }
        return bad(Verdict::WrongMachine, nm + " was built for a different processor than this one.", machine);
    }
    Check ok;
    ok.machine = machine;
    return ok;
}

Check checkFile(const std::string& utf8Path) {
    const fs::path p = pathFromUtf8(utf8Path);
    const std::string nm = utf8FromPath(p.filename());
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) { return bad(Verdict::NotFound, shownName(nm) + " was not found."); }
    const std::uintmax_t size = fs::file_size(p, ec);
    if (ec || size > kMaxLibraryBytes) { return bad(Verdict::Unreadable, shownName(nm) + " cannot be read, or is far too large to be a library."); }
    std::ifstream in(p, std::ios::binary);
    if (!in) { return bad(Verdict::Unreadable, shownName(nm) + " cannot be read."); }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size != 0) { in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)); }
    if (!in && size != 0) { return bad(Verdict::Unreadable, shownName(nm) + " cannot be read."); }
    return checkBytes(bytes.data(), bytes.size(), hostMachine(), nm);
}

std::string findLibrary(const std::string& where, const std::string& fileName, std::string& why) {
    why.clear();
    std::error_code ec;
    const std::string nm = shownName(fileName);
    // The name must be a bare file name: a separator in it would let a setting walk out of the directory it names.
    if (fileName.empty() || fileName.find_first_of("/\\:") != std::string::npos) {
        why = "the library name is not a plain file name.";
        return {};
    }
    if (!where.empty()) {
        const fs::path w = pathFromUtf8(where);
        if (fs::is_directory(w, ec) && !ec) {
            const fs::path f = w / pathFromUtf8(fileName);
            if (fs::is_regular_file(f, ec) && !ec) { return utf8FromPath(fs::absolute(f, ec)); }
            why = nm + " is not in the folder you gave.";
            return {};
        }
        ec.clear();
        if (fs::is_regular_file(w, ec) && !ec) { return utf8FromPath(fs::absolute(w, ec)); }
        why = "the library path you gave does not exist.";
        return {};
    }
#if defined(_WIN32)
    const DWORD need = ::GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::wstring path(need, L'\0');
    if (need != 0) { path.resize(::GetEnvironmentVariableW(L"PATH", path.data(), need)); }
    std::size_t pos = 0;
    while (pos <= path.size()) {
        std::size_t end = path.find(L';', pos);
        if (end == std::wstring::npos) { end = path.size(); }
        std::wstring dir = path.substr(pos, end - pos);
        pos = end + 1;
        if (dir.size() >= 2 && dir.front() == L'"' && dir.back() == L'"') { dir = dir.substr(1, dir.size() - 2); }
        if (dir.empty()) { continue; }
        const fs::path d(dir);
        // Only absolute entries: an empty or relative PATH entry means "the current directory", which is never searched.
        if (!d.is_absolute()) { continue; }
        const fs::path f = d / pathFromUtf8(fileName);
        if (fs::is_regular_file(f, ec) && !ec) { return utf8FromPath(f); }
        ec.clear();
    }
#endif
    why = nm + " was not found. Install the maker's software, or give the folder it is in.";
    return {};
}

std::uint32_t lastOsError() {
#if defined(_WIN32)
    return ::GetLastError();
#else
    return 0;
#endif
}

Library::~Library() { close(); }

std::uint64_t Library::silentLoadCount() { return g_silentLoads.load(); }

Check Library::open(const std::string& utf8Path) {
    close();
    Check c = checkFile(utf8Path);
    if (!c.ok()) { return c; }
    const std::string nm = shownName(utf8FromPath(pathFromUtf8(utf8Path).filename()));
#if defined(_WIN32)
    const std::wstring wide = pathFromUtf8(utf8Path).wstring();
    // Thread-scoped error mode (SetErrorMode would change it for every thread of the application): a missing dependency is an error code,
    // never a box on the visible desktop. Restored on every path out.
    DWORD previous = 0;
    const BOOL haveOld = ::SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous);
    g_silentLoads.fetch_add(1);
    ::SetLastError(0);
    HMODULE m = ::LoadLibraryExW(wide.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    const DWORD err = ::GetLastError();
    if (haveOld) { ::SetThreadErrorMode(previous, nullptr); }
    if (m == nullptr) {
        std::string why = nm + " could not be loaded";
        if (err == ERROR_MOD_NOT_FOUND) {
            why += ": it needs another library that is not installed beside it (Windows error 126).";
        } else if (err == ERROR_BAD_EXE_FORMAT) {
            why += ": it is not a library this program can use (Windows error 193).";
        } else {
            why += " (Windows error " + std::to_string(static_cast<unsigned long>(err)) + ").";
        }
        Check r = bad(Verdict::LoadFailed, why, c.machine);
        return r;
    }
    module_ = m;
    return c;
#else
    (void)nm;
    return bad(Verdict::NotThisPlatform, "this library is for Windows only.");
#endif
}

void* Library::symbol(const char* name) const {
#if defined(_WIN32)
    if (module_ == nullptr || name == nullptr) { return nullptr; }
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(module_), name));
#else
    (void)name;
    return nullptr;
#endif
}

void Library::close() {
#if defined(_WIN32)
    if (module_ != nullptr) { ::FreeLibrary(static_cast<HMODULE>(module_)); }
#endif
    module_ = nullptr;
}

std::string resolveAll(const Library& lib, const std::vector<const char*>& names, std::vector<void*>& out) {
    out.assign(names.size(), nullptr);
    std::string missing;
    for (std::size_t i = 0; i < names.size(); ++i) {
        out[i] = lib.symbol(names[i]);
        if (out[i] == nullptr) {
            if (!missing.empty()) { missing += ", "; }
            missing += names[i];
        }
    }
    return missing;
}

}  // namespace cascade::source::vendorlib
