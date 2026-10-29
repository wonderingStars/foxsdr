// test_plugin_validate.cpp - a corrupt or wrong-ABI plugin file is REFUSED WITH A VERDICT, never loaded-and-crashed (plugin containment P1,
// item 5; docs/engine-stage4.md 6.28).
//
//   A  THE FILE AS BYTES: hand-built PE and ELF images, one fault each - truncated at every structure, another processor, an executable,
//      no exports, exports without the plugin's entry point, a name that declares another plugin ABI - and the one good image. No module is
//      mapped for any of them.
//   B  THE HOST, with the real fault probe: a truncated copy, a wrong-ABI name and a gated file are refused WITHOUT their code running (the
//      probe is told to fault in its DllMain, and the process is alive); a probe that faults in DllMain, in the descriptor query, and one
//      whose descriptor points at unreadable memory are refused with a verdict and the process is alive; a descriptor that claims one
//      capability twice is refused by the pure validator (the entry's own tableSize stays advisory, as the ABI's tests have always held).
//   C  A SITE PLUGIN'S FILE THAT IS NOT THE BYTES THAT WERE INSTALLED is refused (the install record's sha256), an official plugin's is not.
//
// The refusal's words are the VERDICT (core/plugin_module_check.hpp): what the Fitted modules window shows against the file.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "core/plugin_fault.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_module_check.hpp"
#include "test_check.hpp"
#include "fault_probe_names.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace fault = cascade::core::fault;
using cascade::core::ImageFormat;
using cascade::core::LoadedPlugin;
using cascade::core::ModuleCheck;
using cascade::core::ModuleVerdict;
using cascade::core::PluginHost;
using cascade::core::PluginRejection;

namespace {

fs::path g_root;
std::string g_probe;

void put16(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    if (b.size() < at + 2) { b.resize(at + 2); }
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    if (b.size() < at + 4) { b.resize(at + 4); }
    for (int i = 0; i < 4; ++i) { b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i)); }
}
void put64(std::vector<std::uint8_t>& b, std::size_t at, std::uint64_t v) {
    put32(b, at, static_cast<std::uint32_t>(v));
    put32(b, at + 4, static_cast<std::uint32_t>(v >> 32));
}

struct PeSpec {
    std::uint32_t machine = 0x8664;
    std::uint32_t characteristics = 0x2022;   // DLL | EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE
    std::vector<std::string> names = {"cascade_plugin_query"};
    bool exportDirectory = true;
    std::uint32_t claimedNames = 0;           // 0: names.size()
    std::uint32_t rawSize = 0x200;            // what the section header says it holds
};

// A minimal PE32+ image: one `.edata` section that holds the export directory, the name pointer table and the names.
std::vector<std::uint8_t> buildPe(const PeSpec& s) {
    std::vector<std::uint8_t> b(0x400, 0);
    b[0] = 'M';
    b[1] = 'Z';
    put32(b, 0x3C, 0x80);
    std::memcpy(&b[0x80], "PE\0\0", 4);
    put16(b, 0x84, s.machine);
    put16(b, 0x86, 1);        // sections
    put16(b, 0x94, 240);      // size of the optional header
    put16(b, 0x96, s.characteristics);
    const std::size_t opt = 0x98;
    put16(b, opt, 0x20B);
    put32(b, opt + 60, 0x200);   // SizeOfHeaders
    put32(b, opt + 108, 16);     // NumberOfRvaAndSizes
    if (s.exportDirectory) {
        put32(b, opt + 112, 0x1000);
        put32(b, opt + 116, 0x100);
    }
    const std::size_t sec = opt + 240;
    std::memcpy(&b[sec], ".edata\0\0", 8);
    put32(b, sec + 8, 0x200);    // VirtualSize
    put32(b, sec + 12, 0x1000);  // VirtualAddress
    put32(b, sec + 16, s.rawSize);
    put32(b, sec + 20, 0x200);   // PointerToRawData
    // the export directory at file 0x200 (RVA 0x1000)
    const std::uint32_t n = s.claimedNames != 0 ? s.claimedNames : static_cast<std::uint32_t>(s.names.size());
    put32(b, 0x200 + 24, n);
    put32(b, 0x200 + 32, 0x1040);   // AddressOfNames
    std::uint32_t nameRva = 0x1080;
    for (std::size_t i = 0; i < s.names.size(); ++i) {
        put32(b, 0x240 + i * 4, nameRva);
        std::memcpy(&b[0x280 + (nameRva - 0x1080)], s.names[i].c_str(), s.names[i].size() + 1);
        nameRva += static_cast<std::uint32_t>(s.names[i].size() + 1);
    }
    return b;
}

ModuleCheck pe(const std::vector<std::uint8_t>& b) { return cascade::core::checkModuleBytesFor(ImageFormat::Pe, 0x8664, b.data(), b.size()); }

struct ElfSpec {
    std::uint32_t machine = 62;
    std::uint32_t type = 3;   // ET_DYN
    std::vector<std::string> names = {"cascade_plugin_query"};
    bool sections = true;
};

// A minimal ELF64: a header, a .dynsym and a .dynstr, three section headers (null, .dynsym, .dynstr).
std::vector<std::uint8_t> buildElf(const ElfSpec& s) {
    std::string str(1, '\0');
    std::vector<std::uint32_t> idx;
    for (const std::string& nme : s.names) {
        idx.push_back(static_cast<std::uint32_t>(str.size()));
        str += nme;
        str.push_back('\0');
    }
    const std::size_t symOff = 64;
    const std::size_t symSize = 24 * (1 + s.names.size());
    const std::size_t strOff = symOff + symSize;
    const std::size_t shOff = (strOff + str.size() + 7) & ~std::size_t{7};
    std::vector<std::uint8_t> b(shOff + 3 * 64, 0);
    b[0] = 0x7F; b[1] = 'E'; b[2] = 'L'; b[3] = 'F'; b[4] = 2; b[5] = 1; b[6] = 1;
    put16(b, 16, s.type);
    put16(b, 18, s.machine);
    put64(b, 40, s.sections ? shOff : 0);
    put16(b, 54, 56);
    put16(b, 58, 64);
    put16(b, 60, s.sections ? 3 : 0);
    for (std::size_t i = 0; i < s.names.size(); ++i) {
        const std::size_t at = symOff + 24 * (1 + i);
        put32(b, at, idx[i]);
        put16(b, at + 6, 1);   // defined
    }
    std::memcpy(&b[strOff], str.data(), str.size());
    // section 1: .dynsym
    const std::size_t s1 = shOff + 64;
    put32(b, s1 + 4, 11);
    put64(b, s1 + 24, symOff);
    put64(b, s1 + 32, symSize);
    put32(b, s1 + 40, 2);
    put64(b, s1 + 56, 24);
    // section 2: .dynstr
    const std::size_t s2 = shOff + 128;
    put32(b, s2 + 4, 3);
    put64(b, s2 + 24, strOff);
    put64(b, s2 + 32, str.size());
    return b;
}

ModuleCheck elf(const std::vector<std::uint8_t>& b) { return cascade::core::checkModuleBytesFor(ImageFormat::Elf, 62, b.data(), b.size()); }

// ---- A ----------------------------------------------------------------------------------------------------------------------------
void testBytes() {
    // the good image
    CHECK(pe(buildPe(PeSpec{})).ok());
    CHECK(pe(buildPe(PeSpec{})).detail.empty());
    {   // a plugin among other exports
        PeSpec sp;
        sp.names = {"DllRegisterServer", "cascade_plugin_query", "zzz"};
        CHECK(pe(buildPe(sp)).ok());
    }
    // another processor
    {
        PeSpec sp;
        sp.machine = 0x14C;
        const ModuleCheck c = pe(buildPe(sp));
        CHECK(c.verdict == ModuleVerdict::WrongMachine);
        CHECK(c.detail.find("x86") != std::string::npos && c.detail.find("x64") != std::string::npos);
    }
    {
        PeSpec sp;
        sp.machine = 0xAA64;
        CHECK(pe(buildPe(sp)).verdict == ModuleVerdict::WrongMachine);
    }
    // an executable renamed .dll
    {
        PeSpec sp;
        sp.characteristics = 0x0022;
        CHECK(pe(buildPe(sp)).verdict == ModuleVerdict::NotLibrary);
    }
    // no export directory at all, and one with no named exports
    {
        PeSpec sp;
        sp.exportDirectory = false;
        CHECK(pe(buildPe(sp)).verdict == ModuleVerdict::NoExports);
        PeSpec none;
        none.names = {};
        CHECK(pe(buildPe(none)).verdict == ModuleVerdict::NoExports);
    }
    // exports that are not the plugin's
    {
        PeSpec sp;
        sp.names = {"DllMain", "cascade_plugin_quer", "cascade_plugin_query2", "xcascade_plugin_query"};
        const ModuleCheck c = pe(buildPe(sp));
        CHECK(c.verdict == ModuleVerdict::NoEntry);
        CHECK(c.detail.find("cascade_plugin_query") != std::string::npos);
    }
    // an export table that claims more names than the file holds
    {
        PeSpec sp;
        sp.claimedNames = 5000000;
        CHECK(pe(buildPe(sp)).verdict == ModuleVerdict::Truncated);
        PeSpec few;
        few.claimedNames = 100000;   // plausible count, table runs off the end
        CHECK(pe(buildPe(few)).verdict == ModuleVerdict::Truncated);
    }
    // a section whose raw data is not in the file: a truncated download
    {
        PeSpec sp;
        sp.rawSize = 0x4000;
        const ModuleCheck c = pe(buildPe(sp));
        CHECK(c.verdict == ModuleVerdict::Truncated);
        CHECK(c.detail.find("section ends at byte") != std::string::npos);
    }
    // cut at every interesting length: nothing is ever read past the end, and none of them is accepted
    {
        const std::vector<std::uint8_t> good = buildPe(PeSpec{});
        for (const std::size_t cut : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{0x3B}, std::size_t{0x40}, std::size_t{0x7F},
                                      std::size_t{0x84}, std::size_t{0x97}, std::size_t{0x98}, std::size_t{0x120}, std::size_t{0x187},
                                      std::size_t{0x1B0}, std::size_t{0x1FF}, std::size_t{0x200}, std::size_t{0x23F}, std::size_t{0x2FF}, std::size_t{0x3FF}}) {
            std::vector<std::uint8_t> t(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
            const ModuleCheck c = pe(t);
            if (c.ok()) { std::printf("FAIL a PE cut at %zu was accepted\n", cut); }
            CHECK(!c.ok());
        }
    }
    // not an image
    {
        const std::vector<std::uint8_t> text = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'};
        CHECK(pe(text).verdict == ModuleVerdict::NotAnImage);
        CHECK(cascade::core::checkModuleBytesFor(ImageFormat::Pe, 0x8664, nullptr, 0).verdict == ModuleVerdict::NotAnImage);
        std::vector<std::uint8_t> mzOnly = {'M', 'Z', 0, 0};
        CHECK(!pe(mzOnly).ok());
        std::vector<std::uint8_t> badSig = buildPe(PeSpec{});
        badSig[0x80] = 'X';
        CHECK(pe(badSig).verdict == ModuleVerdict::NotAnImage);
        // a Windows image offered to a Linux loader and the other way round
        const std::vector<std::uint8_t> winImage = buildPe(PeSpec{});
        CHECK(cascade::core::checkModuleBytesFor(ImageFormat::Elf, 62, winImage.data(), winImage.size()).verdict == ModuleVerdict::NotAnImage);
        const std::vector<std::uint8_t> elfImage = buildElf(ElfSpec{});
        CHECK(pe(elfImage).verdict == ModuleVerdict::NotAnImage);
    }
    // ELF
    CHECK(elf(buildElf(ElfSpec{})).ok());
    {
        ElfSpec sp;
        sp.machine = 183;
        CHECK(elf(buildElf(sp)).verdict == ModuleVerdict::WrongMachine);
        ElfSpec exe;
        exe.type = 2;
        CHECK(elf(buildElf(exe)).verdict == ModuleVerdict::NotLibrary);
        ElfSpec other;
        other.names = {"foo", "bar"};
        CHECK(elf(buildElf(other)).verdict == ModuleVerdict::NoEntry);
        ElfSpec none;
        none.names = {};
        CHECK(elf(buildElf(none)).verdict == ModuleVerdict::NoExports);
        const std::vector<std::uint8_t> good = buildElf(ElfSpec{});
        std::vector<std::uint8_t> cut(good.begin(), good.begin() + 100);
        CHECK(elf(cut).verdict == ModuleVerdict::Truncated);
        std::vector<std::uint8_t> noSections(good.begin(), good.begin() + 63);
        CHECK(!elf(noSections).ok());
    }
    // a Mach-O is accepted on its magic (UNVERIFIED on a Mac) and nothing else
    {
        const std::vector<std::uint8_t> macho = {0xCF, 0xFA, 0xED, 0xFE, 0, 0, 0, 0};
        CHECK(cascade::core::checkModuleBytesFor(ImageFormat::MachO, 0, macho.data(), macho.size()).ok());
        CHECK(!cascade::core::checkModuleBytesFor(ImageFormat::MachO, 0, macho.data(), 2).ok());
    }
    // the words are closed and distinct
    {
        std::vector<std::string> words;
        for (int v = 0; v <= static_cast<int>(ModuleVerdict::Quarantined); ++v) {
            words.push_back(cascade::core::moduleVerdictWord(static_cast<ModuleVerdict>(v)));
        }
        for (std::size_t i = 0; i < words.size(); ++i) {
            CHECK(!words[i].empty() && words[i] != "unknown");
            for (std::size_t j = i + 1; j < words.size(); ++j) { CHECK(words[i] != words[j]); }
        }
    }
    // the ABI a file name declares
    CHECK(cascade::core::abiDeclaredByFileName("pocsag-decoder-1.0.2-abi3-win-x64.dll") == 3);
    CHECK(cascade::core::abiDeclaredByFileName("old-0.1.0-abi2-win-x64.dll") == 2);
    CHECK(cascade::core::abiDeclaredByFileName("plain-name.dll") == 0);
    CHECK(cascade::core::abiDeclaredByFileName("cabin-1.0.dll") == 0);               // "-abi" must start a token
    CHECK(cascade::core::abiDeclaredByFileName("x-abiX-win.dll") == 0);
    CHECK(cascade::core::abiDeclaredByFileName("x-abi3x.dll") == 0);                 // digits then a letter: not the token
    // checkModuleFile refuses a name that declares another ABI without reading a byte (the file does not exist)
    {
        const ModuleCheck c = cascade::core::checkModuleFile((g_root / "nope-1.0.0-abi2-win-x64.dll").string());
        CHECK(c.verdict == ModuleVerdict::WrongAbi);
        CHECK(c.detail.find("ABI 2") != std::string::npos && c.detail.find("ABI 3") != std::string::npos);
        CHECK(cascade::core::checkModuleFile((g_root / "nope-1.0.0-abi3-win-x64.dll").string()).verdict == ModuleVerdict::Unreadable);
    }
}

// ---- B ----------------------------------------------------------------------------------------------------------------------------
void setEnvVar(const char* name, const char* value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value);
#else
    if (value == nullptr) { ::unsetenv(name); } else { ::setenv(name, value, 1); }
#endif
}

std::vector<std::uint8_t> readBytes(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

const LoadedPlugin* byLeaf(const PluginHost& host, const std::string& leaf) {
    for (const LoadedPlugin& p : host.plugins()) {
        if (fs::path(p.path).filename().string() == leaf) { return &p; }
    }
    return nullptr;
}

void testRealFiles() {
    // the real probe is a plugin; a copy of it cut short is not
    CHECK(cascade::core::checkModuleFile(g_probe).ok());
    const std::vector<std::uint8_t> whole = readBytes(g_probe);
    CHECK(whole.size() > 4000);
#if defined(_WIN32)
    for (const double frac : {0.2, 0.5, 0.9, 0.999}) {
        const std::vector<std::uint8_t> t(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(static_cast<double>(whole.size()) * frac));
        const ModuleCheck c = cascade::core::checkModuleBytes(t.data(), t.size());
        if (c.ok()) { std::printf("FAIL the probe cut at %.3f of its length was accepted\n", frac); }
        CHECK(!c.ok());
    }
#endif
}

void testHost() {
    const fs::path dir = g_root / "plugins";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    auto copy = [&](const std::string& leaf) { fs::copy_file(g_probe, dir / leaf, fs::copy_options::overwrite_existing, ec); };
    auto truncated = [&](const std::string& leaf, double frac) {
        const std::vector<std::uint8_t> w = readBytes(g_probe);
        std::ofstream out(dir / leaf, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(w.data()), static_cast<std::streamsize>(static_cast<double>(w.size()) * frac));
    };
    copy("val-good" CASCADE_PROBE_SUFFIX);
    truncated("val-cut" CASCADE_PROBE_SUFFIX, 0.5);
    copy("val-oldabi" CASCADE_PROBE_SUFFIX_ABI2);
    copy("val-gated" CASCADE_PROBE_SUFFIX);
    copy("val-dllmain" CASCADE_PROBE_SUFFIX);
    copy("val-query" CASCADE_PROBE_SUFFIX);
    copy("val-wildname" CASCADE_PROBE_SUFFIX);
    copy("val-wildtables" CASCADE_PROBE_SUFFIX);
    copy("val-entrysize" CASCADE_PROBE_SUFFIX);
    copy("val-entrydup" CASCADE_PROBE_SUFFIX);

    // the file-level and gate refusals first, with the probe told to fault in DllMain: if any of those files' code ran, the process would
    // be dead (the faults there are absorbed on Windows, so the proof is the verdict AND that no module was mapped)
    PluginHost host;
    host.setLoadGate([](const std::string& leaf, ModuleVerdict& v, std::string& sentence) {
        if (leaf == "val-gated" CASCADE_PROBE_SUFFIX) {
            v = ModuleVerdict::Faulted;
            sentence = "Not loaded: a test gate said so";
            return true;
        }
        return false;
    });
    setEnvVar("FAULT_PROBE", "dllmain-av");
    // (only the gated, cut and old-ABI files are in play for this scan: move the faulting ones aside)
    const fs::path aside = g_root / "aside";
    fs::create_directories(aside);
    for (const char* leaf : {"val-good" CASCADE_PROBE_SUFFIX, "val-dllmain" CASCADE_PROBE_SUFFIX, "val-query" CASCADE_PROBE_SUFFIX,
                             "val-wildname" CASCADE_PROBE_SUFFIX, "val-wildtables" CASCADE_PROBE_SUFFIX,
                             "val-entrysize" CASCADE_PROBE_SUFFIX, "val-entrydup" CASCADE_PROBE_SUFFIX}) {
        fs::rename(dir / leaf, aside / leaf, ec);
    }
    host.scan(dir.string());
    setEnvVar("FAULT_PROBE", nullptr);
    CHECK(host.loadedCount() == 0);
    {
        const LoadedPlugin* cut = byLeaf(host, "val-cut" CASCADE_PROBE_SUFFIX);
        CHECK(cut != nullptr && !cut->loaded && cut->nativeHandle == nullptr);
        if (cut != nullptr) {
            CHECK(cut->verdict == "truncated");
            CHECK(cut->error.rfind("Not loaded: ", 0) == 0);
        }
        const LoadedPlugin* old = byLeaf(host, "val-oldabi" CASCADE_PROBE_SUFFIX_ABI2);
        CHECK(old != nullptr && !old->loaded && old->nativeHandle == nullptr);
        if (old != nullptr) {
            CHECK(old->verdict == "wrong-abi");
            CHECK(old->error.find("ABI 2") != std::string::npos);
        }
        const LoadedPlugin* gated = byLeaf(host, "val-gated" CASCADE_PROBE_SUFFIX);
        CHECK(gated != nullptr && !gated->loaded && gated->nativeHandle == nullptr);
        if (gated != nullptr) {
            CHECK(gated->verdict == "faulted-last-session");
            CHECK(gated->error == "Not loaded: a test gate said so");
        }
    }
    host.unloadAll();

    // the corrupt modules: each faults in its own way, with real code running, and each is refused with a verdict and the process is alive
    for (const char* leaf : {"val-good" CASCADE_PROBE_SUFFIX, "val-dllmain" CASCADE_PROBE_SUFFIX, "val-query" CASCADE_PROBE_SUFFIX,
                             "val-wildname" CASCADE_PROBE_SUFFIX, "val-wildtables" CASCADE_PROBE_SUFFIX,
                             "val-entrysize" CASCADE_PROBE_SUFFIX, "val-entrydup" CASCADE_PROBE_SUFFIX}) {
        fs::rename(aside / leaf, dir / leaf, ec);
    }
    fs::remove(dir / "val-cut" CASCADE_PROBE_SUFFIX, ec);
    fs::remove(dir / "val-oldabi" CASCADE_PROBE_SUFFIX, ec);
    fs::remove(dir / "val-gated" CASCADE_PROBE_SUFFIX, ec);
    fs::remove(dir / "val-oldabi" CASCADE_PROBE_SUFFIX_ABI2, ec);
    // The faults are chosen by the environment AT THE TIME THE MODULE IS MAPPED AND QUERIED, one module at a time: each case scans a folder
    // of its own holding one copy.
    struct Case {
        const char* leaf;
        const char* probe;
        const char* verdict;         // "" for a refusal with no verdict word (the loader's or the query's own)
        bool absorbedOnWindows;      // the case needs structured exceptions
    };
    const Case cases[] = {
        // (the Windows loader itself contains a fault in DllMain: LoadLibrary answers "initialization routine failed" and the host reports
        // the loader's words - this case proves the process survives it and that nothing of the module is left mapped)
        {"val-dllmain" CASCADE_PROBE_SUFFIX, "dllmain-av", "", true},
        {"val-query" CASCADE_PROBE_SUFFIX, "query-av", "", true},
        {"val-wildname" CASCADE_PROBE_SUFFIX, "desc-wild-name", "corrupt", true},
        {"val-wildtables" CASCADE_PROBE_SUFFIX, "desc-wild-tables", "corrupt", true},
        {"val-entrydup" CASCADE_PROBE_SUFFIX, "entry-dup", "", false},
    };
    for (const Case& c : cases) {
#if !defined(_WIN32)
        if (c.absorbedOnWindows) {
            SKIP_LINUX("a plugin's fault cannot be absorbed off Windows");
            continue;
        }
#endif
        const fs::path caseDir = g_root / (std::string("case-") + c.probe);
        fs::remove_all(caseDir, ec);
        fs::create_directories(caseDir);
        fs::copy_file(dir / c.leaf, caseDir / c.leaf, fs::copy_options::overwrite_existing, ec);
        PluginHost h;
        setEnvVar("FAULT_PROBE", c.probe);
        h.scan(caseDir.string());
        setEnvVar("FAULT_PROBE", nullptr);
        const LoadedPlugin* p = byLeaf(h, c.leaf);
        CHECK(p != nullptr);                                  // THE PROCESS IS ALIVE: this line ran
        if (p == nullptr) { continue; }
        if (p->loaded) { std::printf("FAIL %s was loaded\n", c.probe); }
        CHECK(!p->loaded);
        CHECK(p->nativeHandle == nullptr);
        CHECK(!p->error.empty());
        if (p->verdict != c.verdict) { std::printf("FAIL %s: verdict '%s', error: %s\n", c.probe, p->verdict.c_str(), p->error.c_str()); }
        CHECK(p->verdict == c.verdict);
        if (std::string(c.probe) == "dllmain-av") { CHECK(p->error.find("initialization routine failed") != std::string::npos); }
        if (std::string(c.probe) == "query-av") { CHECK(p->error.find("structured exception") != std::string::npos); }
        if (std::string(c.probe) == "entry-dup") { CHECK(p->error.find("two tables for one capability") != std::string::npos); }
        h.unloadAll();
    }

    // THE ENTRY'S OWN tableSize IS ADVISORY (it always was: tests/test_plugin_api.cpp "tableSize is advisory"; the table's structSize is the size the host
    // trusts). A module whose entry says a size that is not this host's is therefore NOT refused by the containment's descriptor checks - they refuse only
    // what is ambiguous (two tables for one capability) or unreadable. Held with a real module so that a later tightening is a decision, not an accident.
    {
        const fs::path caseDir = g_root / "case-entry-size";
        fs::remove_all(caseDir, ec);
        fs::create_directories(caseDir);
        fs::copy_file(dir / "val-entrysize" CASCADE_PROBE_SUFFIX, caseDir / "val-entrysize" CASCADE_PROBE_SUFFIX, fs::copy_options::overwrite_existing, ec);
        PluginHost h;
        setEnvVar("FAULT_PROBE", "entry-size");
        h.scan(caseDir.string());
        setEnvVar("FAULT_PROBE", nullptr);
        const LoadedPlugin* p = byLeaf(h, "val-entrysize" CASCADE_PROBE_SUFFIX);
        CHECK(p != nullptr);
        if (p != nullptr) {
            if (!p->loaded) { std::printf("FAIL the advisory-size module was refused: %s\n", p->error.c_str()); }
            CHECK(p->loaded);
            CHECK(p->verdict.empty());
        }
        h.unloadAll();
    }

    // a plugin that faulted in DllMain or in its query is switched off for the session: the next scan refuses it WITHOUT mapping it again
    {
        const fs::path caseDir = g_root / "case-again";
        fs::remove_all(caseDir, ec);
        fs::create_directories(caseDir);
        fs::copy_file(dir / "val-query" CASCADE_PROBE_SUFFIX, caseDir / "val-query" CASCADE_PROBE_SUFFIX, fs::copy_options::overwrite_existing, ec);
        PluginHost h;
#if defined(_WIN32)
        CHECK(fault::isQuarantinedKey("val-query" CASCADE_PROBE_SUFFIX));
        setEnvVar("FAULT_PROBE", "query-av");   // were the module mapped and queried again, it would fault again
        h.scan(caseDir.string());
        setEnvVar("FAULT_PROBE", nullptr);
        const LoadedPlugin* p = byLeaf(h, "val-query" CASCADE_PROBE_SUFFIX);
        CHECK(p != nullptr && !p->loaded);
        if (p != nullptr) {
            CHECK(p->verdict == "switched-off");
            CHECK(p->error.find("RE-ENABLE") != std::string::npos);
        }
        // and a re-enable makes it a candidate again
        CHECK(fault::release(fault::slotFor("val-query" CASCADE_PROBE_SUFFIX)));
        h.scan(caseDir.string());
        p = byLeaf(h, "val-query" CASCADE_PROBE_SUFFIX);
        CHECK(p != nullptr && p->loaded);
#else
        h.scan(caseDir.string());
#endif
        h.unloadAll();
    }

    // THE LOADER IS NOT LEFT LOCKED by an absorbed fault in a module's start-up code: another thread can still map a library
#if defined(_WIN32)
    {
        std::atomic<bool> done{false};
        std::thread t([&] {
            HMODULE m = ::LoadLibraryA("dnsapi.dll");
            if (m != nullptr) { ::FreeLibrary(m); }
            done = true;
        });
        for (int i = 0; i < 100 && !done.load(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        CHECK(done.load());
        if (done.load()) { t.join(); } else { t.detach(); }
    }
#endif
}

// ---- the pure validator -----------------------------------------------------------------------------------------------------------
std::uint32_t g_dummyDecoderCalls = 0;
void* decCreate(uint32_t) { return &g_dummyDecoderCalls; }
void decProcess(void*, const float*, size_t) {}
int32_t decPoll(void*, char*, size_t) { return 0; }
void decDestroy(void*) {}
const CascadeDecoderApi kDecoder = {static_cast<uint32_t>(sizeof(CascadeDecoderApi)), 0u, &decCreate, &decProcess, &decPoll, &decDestroy};

CascadePluginDesc descriptor(const CascadeCapabilityEntry* entries, uint32_t count) {
    CascadePluginDesc d{};
    d.structSize = static_cast<uint32_t>(sizeof(CascadePluginDesc));
    d.abiVersion = CASCADE_PLUGIN_ABI_VERSION;
    d.name = "Pure";
    d.version = "1.0.0";
    d.author = "t";
    d.licence = "MIT";
    d.capabilities = CASCADE_CAP_DECODER;
    d.capabilityCount = count;
    d.capabilityTables = entries;
    return d;
}

void testValidator() {
    {   // the good one
        const CascadeCapabilityEntry e[] = {{CASCADE_CAP_DECODER, static_cast<uint32_t>(sizeof(CascadeDecoderApi)), &kDecoder}};
        const CascadePluginDesc d = descriptor(e, 1);
        CHECK(cascade::core::validatePluginDesc(&d) == PluginRejection::None);
    }
    {   // the entry's own table size is ADVISORY (the ABI header says the host compares it; this host has always trusted the table's own structSize, which
        // is checked below - tests/test_plugin_api.cpp "tableSize is advisory" holds the same): a size that is not this host's, or zero, is not a refusal
        const CascadeCapabilityEntry e[] = {{CASCADE_CAP_DECODER, static_cast<uint32_t>(sizeof(CascadeDecoderApi)) + 8u, &kDecoder}};
        const CascadePluginDesc d = descriptor(e, 1);
        CHECK(cascade::core::validatePluginDesc(&d) == PluginRejection::None);
        const CascadeCapabilityEntry z[] = {{CASCADE_CAP_DECODER, 0u, &kDecoder}};
        const CascadePluginDesc dz = descriptor(z, 1);
        CHECK(cascade::core::validatePluginDesc(&dz) == PluginRejection::None);
    }
    {   // two tables for one declared capability
        const CascadeCapabilityEntry e[] = {{CASCADE_CAP_DECODER, static_cast<uint32_t>(sizeof(CascadeDecoderApi)), &kDecoder},
                                            {CASCADE_CAP_DECODER, static_cast<uint32_t>(sizeof(CascadeDecoderApi)), &kDecoder}};
        const CascadePluginDesc d = descriptor(e, 2);
        CHECK(cascade::core::validatePluginDesc(&d) == PluginRejection::DuplicateCapabilityTable);
    }
    {   // an entry that names no capability is skipped, as the ABI says: the plugin is still usable
        const CascadeCapabilityEntry e[] = {{0u, 0u, nullptr}, {CASCADE_CAP_DECODER, static_cast<uint32_t>(sizeof(CascadeDecoderApi)), &kDecoder}};
        const CascadePluginDesc d = descriptor(e, 2);
        CHECK(cascade::core::validatePluginDesc(&d) == PluginRejection::None);
    }
    {   // a capability the host does not know, alongside a good one, is ignored; its (wrong) size is not this host's business
        const CascadeCapabilityEntry e[] = {{CASCADE_CAP_DECODER, static_cast<uint32_t>(sizeof(CascadeDecoderApi)), &kDecoder},
                                            {0x80000000u, 7u, &kDecoder}};
        CascadePluginDesc d = descriptor(e, 2);
        d.capabilities |= 0x80000000u;
        CHECK(cascade::core::validatePluginDesc(&d) == PluginRejection::None);
    }
    CHECK(std::string(cascade::core::pluginRejectionMessage(PluginRejection::DuplicateCapabilityTable)) != "unknown rejection");
}

// ---- C ----------------------------------------------------------------------------------------------------------------------------
void testSiteTamper() {
    using cascade::core::InstalledPlugin;
    std::vector<InstalledPlugin> records(3);
    records[0].id = "a";
    records[0].source = "https://site.example/plugins";
    records[0].file = "Site-Plug-1.0.0-abi3-win-x64.dll";
    records[0].digestMismatch = true;
    records[1].id = "b";
    records[1].source = "https://site.example/plugins";
    records[1].file = "site-ok-1.0.0-abi3-win-x64.dll";
    records[1].digestMismatch = false;
    records[2].id = "c";
    records[2].source = "";   // the official catalogue
    records[2].file = "official-1.0.0-abi3-win-x64.dll";
    records[2].digestMismatch = true;
    ModuleCheck c;
    CHECK(cascade::core::siteFileTampered(records, "site-plug-1.0.0-abi3-win-x64.dll", c));   // case-insensitive, like NTFS
    CHECK(c.verdict == ModuleVerdict::Tampered);
    CHECK(c.detail.find("sha256") != std::string::npos);
    ModuleCheck none;
    CHECK(!cascade::core::siteFileTampered(records, "site-ok-1.0.0-abi3-win-x64.dll", none));      // agrees with its record
    CHECK(!cascade::core::siteFileTampered(records, "official-1.0.0-abi3-win-x64.dll", none));     // official: a note, not a refusal
    CHECK(!cascade::core::siteFileTampered(records, "unrecorded.dll", none));                      // no record: unmanaged, left alone
    CHECK(none.verdict == ModuleVerdict::Ok);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: test_plugin_validate <fault probe module>\n");
        return 2;
    }
    g_probe = argv[1];
    g_root = fs::temp_directory_path() / ("foxsdr-test-plugin-validate-" + std::to_string(static_cast<unsigned long long>(
#if defined(_WIN32)
                                                                                            ::GetCurrentProcessId()
#else
                                                                                            1
#endif
                                                                                                )));
    std::error_code ec;
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root);

    testBytes();
    testRealFiles();
    testValidator();
    testSiteTamper();
    testHost();

    fs::remove_all(g_root, ec);
    return testSummary("test_plugin_validate");
}
