// See plugin_module_check.hpp. Pure byte arithmetic over the image's own headers: no loader, no module is mapped.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_module_check.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/plugin_abi.h"

namespace cascade::core {

namespace {

// Little-endian reads that never run past the buffer: false when the bytes are not there.
bool rd16(const std::uint8_t* d, std::size_t n, std::size_t at, std::uint32_t& v) {
    if (at > n || n - at < 2) { return false; }
    v = static_cast<std::uint32_t>(d[at]) | (static_cast<std::uint32_t>(d[at + 1]) << 8);
    return true;
}
bool rd32(const std::uint8_t* d, std::size_t n, std::size_t at, std::uint32_t& v) {
    if (at > n || n - at < 4) { return false; }
    v = static_cast<std::uint32_t>(d[at]) | (static_cast<std::uint32_t>(d[at + 1]) << 8) |
        (static_cast<std::uint32_t>(d[at + 2]) << 16) | (static_cast<std::uint32_t>(d[at + 3]) << 24);
    return true;
}
bool rd64(const std::uint8_t* d, std::size_t n, std::size_t at, std::uint64_t& v) {
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
    if (!rd32(d, n, at, lo) || !rd32(d, n, at + 4, hi)) { return false; }
    v = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
    return true;
}

ModuleCheck verdict(ModuleVerdict v, std::string detail) {
    ModuleCheck c;
    c.verdict = v;
    c.detail = std::move(detail);
    return c;
}

// Whether the NUL-terminated name at `at` is exactly `want`, never reading past the end of the file.
bool nameIs(const std::uint8_t* d, std::size_t n, std::size_t at, const char* want) {
    const std::size_t len = std::strlen(want);
    if (at > n || n - at < len + 1) { return false; }
    return std::memcmp(d + at, want, len) == 0 && d[at + len] == 0;
}

const char* machineWord(ImageFormat f, std::uint32_t m) {
    if (f == ImageFormat::Pe) {
        switch (m) {
            case 0x8664: return "x64";
            case 0xAA64: return "arm64";
            case 0x14C: return "x86";
            case 0x1C4: return "arm";
            default: return "an unknown processor";
        }
    }
    switch (m) {
        case 62: return "x86-64";
        case 183: return "aarch64";
        case 3: return "x86";
        case 40: return "arm";
        default: return "an unknown processor";
    }
}

struct Section {
    std::uint32_t virtualSize = 0;
    std::uint32_t virtualAddress = 0;
    std::uint32_t rawSize = 0;
    std::uint32_t rawPointer = 0;
};

// An RVA to its offset in the file, through the section table (or the headers, below the first section). False when no section holds it
// or the offset is not in the file.
bool rvaToOffset(const std::vector<Section>& sections, std::uint32_t sizeOfHeaders, std::size_t n, std::uint32_t rva, std::size_t& off) {
    if (rva < sizeOfHeaders) {
        off = rva;
        return off < n;
    }
    for (const Section& s : sections) {
        const std::uint32_t span = std::max(s.virtualSize, s.rawSize);
        if (rva >= s.virtualAddress && rva - s.virtualAddress < span) {
            const std::uint64_t o = static_cast<std::uint64_t>(s.rawPointer) + (rva - s.virtualAddress);
            if (o >= n) { return false; }
            off = static_cast<std::size_t>(o);
            return true;
        }
    }
    return false;
}

ModuleCheck checkPe(std::uint32_t wantMachine, const std::uint8_t* d, std::size_t n) {
    std::uint32_t lfanew = 0;
    if (n < 0x40 || !rd32(d, n, 0x3C, lfanew)) { return verdict(ModuleVerdict::Truncated, "the file ends inside its DOS header"); }
    if (lfanew < 0x40 || static_cast<std::uint64_t>(lfanew) + 24 > n) {
        return verdict(ModuleVerdict::Truncated, "the file ends before its PE header");
    }
    if (std::memcmp(d + lfanew, "PE\0\0", 4) != 0) { return verdict(ModuleVerdict::NotAnImage, "it has an MZ header but no PE header"); }
    std::uint32_t machine = 0, nSections = 0, optSize = 0, characteristics = 0;
    rd16(d, n, lfanew + 4, machine);
    rd16(d, n, lfanew + 6, nSections);
    rd16(d, n, lfanew + 20, optSize);
    rd16(d, n, lfanew + 22, characteristics);
    if (machine != wantMachine) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "it was built for %s and this program runs on %s", machineWord(ImageFormat::Pe, machine),
                      machineWord(ImageFormat::Pe, wantMachine));
        return verdict(ModuleVerdict::WrongMachine, buf);
    }
    if ((characteristics & 0x2000u) == 0u) { return verdict(ModuleVerdict::NotLibrary, "it is a program, not a shared library"); }
    const std::size_t opt = static_cast<std::size_t>(lfanew) + 24;
    std::uint32_t magic = 0;
    if (!rd16(d, n, opt, magic) || (magic != 0x20B && magic != 0x10B)) {
        return verdict(ModuleVerdict::Truncated, "its optional header is missing or unreadable");
    }
    const bool plus = magic == 0x20B;
    if (optSize < (plus ? 112u : 96u) || opt + optSize > n) { return verdict(ModuleVerdict::Truncated, "its optional header runs past the end of the file"); }
    std::uint32_t sizeOfHeaders = 0, nDirs = 0;
    rd32(d, n, opt + 60, sizeOfHeaders);
    rd32(d, n, opt + (plus ? 108u : 92u), nDirs);
    if (nSections == 0 || nSections > 96) { return verdict(ModuleVerdict::Truncated, "its section table is empty or absurd"); }
    const std::size_t secAt = opt + optSize;
    if (secAt + static_cast<std::size_t>(nSections) * 40 > n) { return verdict(ModuleVerdict::Truncated, "its section table runs past the end of the file"); }
    std::vector<Section> sections;
    for (std::uint32_t i = 0; i < nSections; ++i) {
        Section s;
        const std::size_t at = secAt + static_cast<std::size_t>(i) * 40;
        rd32(d, n, at + 8, s.virtualSize);
        rd32(d, n, at + 12, s.virtualAddress);
        rd32(d, n, at + 16, s.rawSize);
        rd32(d, n, at + 20, s.rawPointer);
        // THE SIGNATURE OF A TRUNCATED DOWNLOAD: a section whose raw data the file does not hold.
        if (s.rawSize != 0 && static_cast<std::uint64_t>(s.rawPointer) + s.rawSize > n) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "a section ends at byte %llu and the file has %llu",
                          static_cast<unsigned long long>(static_cast<std::uint64_t>(s.rawPointer) + s.rawSize),
                          static_cast<unsigned long long>(n));
            return verdict(ModuleVerdict::Truncated, buf);
        }
        sections.push_back(s);
    }
    std::uint32_t exportRva = 0, exportSize = 0;
    if (nDirs == 0 || !rd32(d, n, opt + (plus ? 112u : 96u), exportRva) || !rd32(d, n, opt + (plus ? 116u : 100u), exportSize) || exportRva == 0 ||
        exportSize == 0) {
        return verdict(ModuleVerdict::NoExports, "it exports nothing");
    }
    std::size_t exportAt = 0;
    if (!rvaToOffset(sections, sizeOfHeaders, n, exportRva, exportAt) || exportAt + 40 > n) {
        return verdict(ModuleVerdict::Truncated, "its export table lies outside the file");
    }
    std::uint32_t nNames = 0, namesRva = 0;
    rd32(d, n, exportAt + 24, nNames);
    rd32(d, n, exportAt + 32, namesRva);
    if (nNames == 0) { return verdict(ModuleVerdict::NoExports, "it exports no named functions"); }
    if (nNames > 200000) { return verdict(ModuleVerdict::Truncated, "its export table claims an absurd number of names"); }
    std::size_t namesAt = 0;
    if (!rvaToOffset(sections, sizeOfHeaders, n, namesRva, namesAt) || namesAt + static_cast<std::size_t>(nNames) * 4 > n) {
        return verdict(ModuleVerdict::Truncated, "its export name table lies outside the file");
    }
    for (std::uint32_t i = 0; i < nNames; ++i) {
        std::uint32_t nameRva = 0;
        rd32(d, n, namesAt + static_cast<std::size_t>(i) * 4, nameRva);
        std::size_t nameAt = 0;
        if (rvaToOffset(sections, sizeOfHeaders, n, nameRva, nameAt) && nameIs(d, n, nameAt, CASCADE_PLUGIN_ENTRY_NAME)) {
            return ModuleCheck{};
        }
    }
    return verdict(ModuleVerdict::NoEntry, std::string("it exports functions but not ") + CASCADE_PLUGIN_ENTRY_NAME);
}

ModuleCheck checkElf(std::uint32_t wantMachine, const std::uint8_t* d, std::size_t n) {
    if (n < 64) { return verdict(ModuleVerdict::Truncated, "the file ends inside its ELF header"); }
    if (d[4] != 2 || d[5] != 1) { return verdict(ModuleVerdict::WrongMachine, "it is not a 64-bit little-endian image"); }
    std::uint32_t type = 0, machine = 0;
    rd16(d, n, 16, type);
    rd16(d, n, 18, machine);
    if (machine != wantMachine) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "it was built for %s and this program runs on %s", machineWord(ImageFormat::Elf, machine),
                      machineWord(ImageFormat::Elf, wantMachine));
        return verdict(ModuleVerdict::WrongMachine, buf);
    }
    if (type != 3) { return verdict(ModuleVerdict::NotLibrary, "it is a program, not a shared object"); }
    std::uint64_t shoff = 0;
    std::uint32_t shentsize = 0, shnum = 0;
    rd64(d, n, 40, shoff);
    rd16(d, n, 58, shentsize);
    rd16(d, n, 60, shnum);
    if (shoff == 0 || shnum == 0) { return ModuleCheck{}; }   // no section headers: the loader's dlsym is the judge of the exports
    if (shentsize < 64 || shoff + static_cast<std::uint64_t>(shnum) * shentsize > n) {
        return verdict(ModuleVerdict::Truncated, "its section headers lie outside the file");
    }
    for (std::uint32_t i = 0; i < shnum; ++i) {
        const std::size_t sh = static_cast<std::size_t>(shoff) + static_cast<std::size_t>(i) * shentsize;
        std::uint32_t shType = 0, link = 0;
        std::uint64_t off = 0, size = 0, entsize = 0;
        rd32(d, n, sh + 4, shType);
        if (shType != 11) { continue; }   // SHT_DYNSYM
        rd64(d, n, sh + 24, off);
        rd64(d, n, sh + 32, size);
        rd32(d, n, sh + 40, link);
        rd64(d, n, sh + 56, entsize);
        if (entsize != 24 || link >= shnum || off + size > n) { return verdict(ModuleVerdict::Truncated, "its dynamic symbol table lies outside the file"); }
        const std::size_t strSh = static_cast<std::size_t>(shoff) + static_cast<std::size_t>(link) * shentsize;
        std::uint64_t strOff = 0, strSize = 0;
        rd64(d, n, strSh + 24, strOff);
        rd64(d, n, strSh + 32, strSize);
        if (strOff + strSize > n) { return verdict(ModuleVerdict::Truncated, "its dynamic string table lies outside the file"); }
        bool anyDefined = false;
        for (std::uint64_t at = off; at + 24 <= off + size; at += 24) {
            std::uint32_t nameIdx = 0, shndx = 0;
            rd32(d, n, static_cast<std::size_t>(at), nameIdx);
            rd16(d, n, static_cast<std::size_t>(at) + 6, shndx);
            if (shndx == 0) { continue; }   // undefined: an import
            anyDefined = true;
            if (nameIdx < strSize && nameIs(d, n, static_cast<std::size_t>(strOff) + nameIdx, CASCADE_PLUGIN_ENTRY_NAME)) { return ModuleCheck{}; }
        }
        return verdict(anyDefined ? ModuleVerdict::NoEntry : ModuleVerdict::NoExports,
                       anyDefined ? std::string("it exports functions but not ") + CASCADE_PLUGIN_ENTRY_NAME : std::string("it exports nothing"));
    }
    return ModuleCheck{};   // no dynamic symbol table among the headers: left to dlsym
}

}  // namespace

const char* moduleVerdictWord(ModuleVerdict v) {
    switch (v) {
        case ModuleVerdict::Ok: return "ok";
        case ModuleVerdict::Unreadable: return "unreadable";
        case ModuleVerdict::NotAnImage: return "not-a-module";
        case ModuleVerdict::Truncated: return "truncated";
        case ModuleVerdict::WrongMachine: return "wrong-machine";
        case ModuleVerdict::NotLibrary: return "not-a-library";
        case ModuleVerdict::NoExports: return "no-exports";
        case ModuleVerdict::NoEntry: return "no-entry";
        case ModuleVerdict::WrongAbi: return "wrong-abi";
        case ModuleVerdict::Tampered: return "changed-since-install";
        case ModuleVerdict::Corrupt: return "corrupt";
        case ModuleVerdict::Faulted: return "faulted-last-session";
        case ModuleVerdict::SafeMode: return "safe-mode";
        case ModuleVerdict::Quarantined: return "switched-off";
    }
    return "unknown";
}

ImageFormat hostImageFormat() {
#if defined(_WIN32)
    return ImageFormat::Pe;
#elif defined(__APPLE__)
    return ImageFormat::MachO;
#else
    return ImageFormat::Elf;
#endif
}

std::uint32_t hostMachine() {
#if defined(_WIN32)
#if defined(_M_ARM64)
    return 0xAA64;
#elif defined(_M_X64)
    return 0x8664;
#else
    return 0x14C;
#endif
#else
#if defined(__aarch64__)
    return 183;
#elif defined(__x86_64__)
    return 62;
#else
    return 3;
#endif
#endif
}

ModuleCheck checkModuleBytesFor(ImageFormat format, std::uint32_t machine, const std::uint8_t* d, std::size_t n) {
    if (d == nullptr || n < 4) { return verdict(ModuleVerdict::NotAnImage, "it is too small to be a library"); }
    switch (format) {
        case ImageFormat::Pe:
            if (d[0] != 'M' || d[1] != 'Z') { return verdict(ModuleVerdict::NotAnImage, "it is not a Windows library (no MZ header)"); }
            return checkPe(machine, d, n);
        case ImageFormat::Elf:
            if (d[0] != 0x7F || d[1] != 'E' || d[2] != 'L' || d[3] != 'F') {
                return verdict(ModuleVerdict::NotAnImage, "it is not a shared object (no ELF header)");
            }
            return checkElf(machine, d, n);
        case ImageFormat::MachO: {
            // UNVERIFIED on a Mac: accepted on the magic alone (thin 64-bit either order, or a universal binary); the loader judges the rest.
            std::uint32_t magic = 0;
            rd32(d, n, 0, magic);
            if (magic == 0xFEEDFACFu || magic == 0xCFFAEDFEu || magic == 0xCAFEBABEu || magic == 0xBEBAFECAu) { return ModuleCheck{}; }
            return verdict(ModuleVerdict::NotAnImage, "it is not a dynamic library (no Mach-O header)");
        }
    }
    return ModuleCheck{};
}

ModuleCheck checkModuleBytes(const std::uint8_t* data, std::size_t n) {
    return checkModuleBytesFor(hostImageFormat(), hostMachine(), data, n);
}

bool siteFileTampered(const std::vector<InstalledPlugin>& records, const std::string& fileName, ModuleCheck& out) {
    auto lower = [](std::string v) {
        for (char& c : v) {
            if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
        }
        return v;
    };
    const std::string want = lower(fileName);
    for (const InstalledPlugin& r : records) {
        if (r.source.empty() || !r.digestMismatch || lower(r.file) != want) { continue; }
        out.verdict = ModuleVerdict::Tampered;
        out.detail =
            "this file is not the one that was installed from the plugin site (its sha256 no longer matches the record made at install), "
            "so it was not loaded. Remove it and install it again";
        return true;
    }
    return false;
}

unsigned abiDeclaredByFileName(const std::string& fileName) {
    // "...-abi3-win-x64.dll": the token is "-abi" followed by digits and then '-' or '.'.
    for (std::size_t at = fileName.find("-abi"); at != std::string::npos; at = fileName.find("-abi", at + 1)) {
        std::size_t i = at + 4;
        unsigned v = 0;
        std::size_t digits = 0;
        while (i < fileName.size() && fileName[i] >= '0' && fileName[i] <= '9' && digits < 4) {
            v = v * 10u + static_cast<unsigned>(fileName[i] - '0');
            ++i;
            ++digits;
        }
        if (digits > 0 && (i == fileName.size() || fileName[i] == '-' || fileName[i] == '.')) { return v; }
    }
    return 0;
}

ModuleCheck checkModuleFile(const std::string& path) {
    // The ABI the name declares first: it needs no byte of the file, and a plugin built for another ABI is refused without reading it.
    const std::size_t cut = path.find_last_of("/\\");
    const std::string leaf = cut == std::string::npos ? path : path.substr(cut + 1);
    const unsigned declared = abiDeclaredByFileName(leaf);
    if (declared != 0 && declared != static_cast<unsigned>(CASCADE_PLUGIN_ABI_VERSION)) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "its file name says it was built for plugin ABI %u and this program takes ABI %d", declared,
                      static_cast<int>(CASCADE_PLUGIN_ABI_VERSION));
        return verdict(ModuleVerdict::WrongAbi, buf);
    }
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) { return verdict(ModuleVerdict::Unreadable, "the file could not be opened"); }
    const std::streamoff size = in.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > kMaxModuleBytes) { return verdict(ModuleVerdict::Unreadable, "the file is larger than any plugin"); }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.seekg(0);
    if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), size)) { return verdict(ModuleVerdict::Unreadable, "the file could not be read"); }
    return checkModuleBytes(bytes.data(), bytes.size());
}

}  // namespace cascade::core
