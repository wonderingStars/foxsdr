// A tiny ELF file, built in memory, shaped like the split-debug files
// tools/archive-symbols-linux.sh archives - for tests/test_report_reader.cpp.
//
// WHY IT IS BUILT HERE AND NOT COMPILED. The machine this feature was written
// on has no Linux compiler, and no cross compiler, so a genuine `g++ -g` output
// could not be produced for a repository fixture. What the reader test needs
// from a fixture is narrower than that: a file the REAL `addr2line` reads,
// whose answers are known in advance, in two states - with DWARF line tables
// and without. The reader contains no DWARF parser of its own (it drives
// addr2line), so the code under test is its command line, its parse, the
// archive layout it searches and the wording it prints; the DWARF itself is
// only ever interpreted by binutils, and a file binutils reads correctly is an
// honest input to that. Its validity is therefore checked by an independent
// reader rather than assumed: `readelf -S`, `readelf --debug-dump=decodedline`
// and `addr2line` all accept it (see the notes at the top of the test).
//
// What it models, from the real archive (a 0.99.59 `cascade.debug`, read with
// readelf): `.text` is NOBITS, there are no program headers' worth of content,
// and `.symtab`/`.strtab` hold the function table. buildElf(false) is exactly
// that shape - the state of every archived cascade.debug until the Linux build
// started compiling with -g. buildElf(true) adds the three DWARF 4 sections a
// -g build adds (.debug_info, .debug_abbrev, .debug_line).
//
// The layout it encodes, so a test can assert it:
//
//   0x1000  fixtureAlpha  (0x20 bytes)   fixture.cpp:10 at 0x1000, :11 at 0x1008
//   0x1020  fixtureBeta   (0x20 bytes)   fixture.cpp:20 at 0x1020,
//                                        inc/helper.h:7 at 0x1030
//   0x1040  end of the line sequence
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace elffixture {

constexpr std::uint64_t kAlphaAddr = 0x1000;
constexpr std::uint64_t kBetaAddr = 0x1020;
constexpr std::uint64_t kTextSize = 0x40;

using Bytes = std::vector<unsigned char>;

inline void put8(Bytes& b, std::uint64_t v) { b.push_back(static_cast<unsigned char>(v & 0xFF)); }
inline void put16(Bytes& b, std::uint64_t v) {
    for (int i = 0; i < 2; ++i) { put8(b, v >> (8 * i)); }
}
inline void put32(Bytes& b, std::uint64_t v) {
    for (int i = 0; i < 4; ++i) { put8(b, v >> (8 * i)); }
}
inline void put64(Bytes& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) { put8(b, v >> (8 * i)); }
}
inline void putStr(Bytes& b, const std::string& s) {
    for (char c : s) { put8(b, static_cast<unsigned char>(c)); }
    put8(b, 0);
}
inline void putUleb(Bytes& b, std::uint64_t v) {
    do {
        unsigned char byte = static_cast<unsigned char>(v & 0x7F);
        v >>= 7;
        if (v != 0) { byte |= 0x80; }
        b.push_back(byte);
    } while (v != 0);
}
inline void putSleb(Bytes& b, std::int64_t v) {
    bool more = true;
    while (more) {
        unsigned char byte = static_cast<unsigned char>(v & 0x7F);
        v >>= 7;  // arithmetic shift on every compiler this project builds with
        const bool signBit = (byte & 0x40) != 0;
        if ((v == 0 && !signBit) || (v == -1 && signBit)) {
            more = false;
        } else {
            byte |= 0x80;
        }
        b.push_back(byte);
    }
}
inline void patch32(Bytes& b, std::size_t at, std::uint64_t v) {
    for (std::size_t i = 0; i < 4; ++i) {
        b[at + i] = static_cast<unsigned char>((v >> (8 * i)) & 0xFF);
    }
}

// .debug_abbrev: one abbreviation, a childless DW_TAG_compile_unit.
inline Bytes debugAbbrev() {
    Bytes a;
    putUleb(a, 1);     // abbreviation code 1
    putUleb(a, 0x11);  // DW_TAG_compile_unit
    put8(a, 0);        // DW_CHILDREN_no
    const std::uint8_t attrs[][2] = {
        {0x25, 0x08},  // DW_AT_producer  DW_FORM_string
        {0x03, 0x08},  // DW_AT_name      DW_FORM_string
        {0x1B, 0x08},  // DW_AT_comp_dir  DW_FORM_string
        {0x11, 0x01},  // DW_AT_low_pc    DW_FORM_addr
        {0x12, 0x07},  // DW_AT_high_pc   DW_FORM_data8 (a length, since DWARF 4)
        {0x10, 0x17},  // DW_AT_stmt_list DW_FORM_sec_offset
    };
    for (const auto& at : attrs) {
        putUleb(a, at[0]);
        putUleb(a, at[1]);
    }
    put8(a, 0);  // end of this abbreviation's attributes
    put8(a, 0);
    put8(a, 0);  // end of the table
    return a;
}

// .debug_info: one DWARF 4 compile unit covering the whole of .text.
inline Bytes debugInfo() {
    Bytes i;
    put32(i, 0);  // unit_length, patched below
    put16(i, 4);  // version
    put32(i, 0);  // debug_abbrev_offset
    put8(i, 8);   // address_size
    putUleb(i, 1);
    putStr(i, "foxsdr elf line fixture");
    putStr(i, "fixture.cpp");
    putStr(i, "/fixture/src");
    put64(i, kAlphaAddr);
    put64(i, kTextSize);
    put32(i, 0);  // DW_AT_stmt_list: offset 0 in .debug_line
    patch32(i, 0, i.size() - 4);
    return i;
}

// .debug_line: a DWARF 4 line program with the rows described at the top.
inline Bytes debugLine() {
    Bytes l;
    put32(l, 0);  // unit_length, patched below
    put16(l, 4);  // version
    put32(l, 0);  // header_length, patched below
    const std::size_t afterHeaderLength = l.size();
    put8(l, 1);                     // minimum_instruction_length
    put8(l, 1);                     // maximum_operations_per_instruction
    put8(l, 1);                     // default_is_stmt
    put8(l, 0xFB);                  // line_base = -5
    put8(l, 14);                    // line_range
    put8(l, 13);                    // opcode_base
    const std::uint8_t stdLengths[12] = {0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1};
    for (std::uint8_t n : stdLengths) { put8(l, n); }
    putStr(l, "inc");               // include_directories[1]
    put8(l, 0);                     // end of include_directories
    putStr(l, "fixture.cpp");       // file 1, in the compilation directory
    putUleb(l, 0); putUleb(l, 0); putUleb(l, 0);
    putStr(l, "helper.h");          // file 2, in directory 1 ("inc")
    putUleb(l, 1); putUleb(l, 0); putUleb(l, 0);
    put8(l, 0);                     // end of file_names
    patch32(l, 6, l.size() - afterHeaderLength);

    // DW_LNE_set_address
    put8(l, 0); putUleb(l, 9); put8(l, 2); put64(l, kAlphaAddr);
    // line 1 -> 10 at 0x1000
    put8(l, 3); putSleb(l, 9);
    put8(l, 1);  // DW_LNS_copy
    // 0x1008, line 11
    put8(l, 2); putUleb(l, 8);
    put8(l, 3); putSleb(l, 1);
    put8(l, 1);
    // 0x1020, line 20
    put8(l, 2); putUleb(l, 0x18);
    put8(l, 3); putSleb(l, 9);
    put8(l, 1);
    // 0x1030, file 2, line 7
    put8(l, 2); putUleb(l, 0x10);
    put8(l, 4); putUleb(l, 2);  // DW_LNS_set_file
    put8(l, 3); putSleb(l, -13);
    put8(l, 1);
    // 0x1040, end of sequence
    put8(l, 2); putUleb(l, 0x10);
    put8(l, 0); putUleb(l, 1); put8(l, 1);  // DW_LNE_end_sequence
    patch32(l, 0, l.size() - 4);
    return l;
}

// The machine the file claims to be for. The host's own, so that the native
// binutils on the machine running the test (this is built on an x86-64 runner
// and on an Arm64 one) is reading a file of its own architecture: a distribution
// binutils is not guaranteed to open a foreign-architecture ELF at all.
inline std::uint16_t hostMachine() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return 183;  // EM_AARCH64
#else
    return 62;  // EM_X86_64
#endif
}

// The whole file. withDwarf=false is the symbol-table-only shape. `machine`
// is the e_machine value; the default is the host's.
inline Bytes buildElf(bool withDwarf, std::uint16_t machine = hostMachine()) {
    struct Section {
        std::string name;
        std::uint32_t type;
        std::uint64_t flags;
        std::uint64_t addr;
        Bytes data;           // empty for NOBITS
        std::uint64_t size;   // used for NOBITS only
        std::uint32_t link;
        std::uint32_t info;
        std::uint64_t align;
        std::uint64_t entsize;
        std::uint64_t offset = 0;
    };
    constexpr std::uint32_t kProgbits = 1, kSymtab = 2, kStrtab = 3, kNobits = 8;

    std::vector<Section> s;
    s.push_back({"", 0, 0, 0, {}, 0, 0, 0, 0, 0});
    s.push_back({".text", kNobits, 0x6 /*AX*/, kAlphaAddr, {}, kTextSize, 0, 0, 16, 0});
    if (withDwarf) {
        s.push_back({".debug_info", kProgbits, 0, 0, debugInfo(), 0, 0, 0, 1, 0});
        s.push_back({".debug_abbrev", kProgbits, 0, 0, debugAbbrev(), 0, 0, 0, 1, 0});
        s.push_back({".debug_line", kProgbits, 0, 0, debugLine(), 0, 0, 0, 1, 0});
    }
    const std::uint32_t symtabIndex = static_cast<std::uint32_t>(s.size());
    const std::uint32_t strtabIndex = symtabIndex + 1;

    // .strtab and the symbols that name into it.
    Bytes strtab;
    put8(strtab, 0);
    const auto addName = [&strtab](const std::string& n) {
        const std::size_t at = strtab.size();
        putStr(strtab, n);
        return static_cast<std::uint32_t>(at);
    };
    const std::uint32_t fileName = addName("fixture.cpp");
    const std::uint32_t alphaName = addName("fixtureAlpha");
    const std::uint32_t betaName = addName("fixtureBeta");

    Bytes symtab;
    for (int i = 0; i < 24; ++i) { put8(symtab, 0); }  // the null symbol
    const auto addSym = [&symtab](std::uint32_t name, std::uint8_t info, std::uint16_t shndx,
                                  std::uint64_t value, std::uint64_t size) {
        put32(symtab, name);
        put8(symtab, info);
        put8(symtab, 0);
        put16(symtab, shndx);
        put64(symtab, value);
        put64(symtab, size);
    };
    addSym(fileName, 0x04 /*LOCAL FILE*/, 0xFFF1 /*SHN_ABS*/, 0, 0);
    addSym(alphaName, 0x12 /*GLOBAL FUNC*/, 1, kAlphaAddr, 0x20);
    addSym(betaName, 0x12, 1, kBetaAddr, 0x20);

    s.push_back({".symtab", kSymtab, 0, 0, symtab, 0, strtabIndex, 2, 8, 24});
    s.push_back({".strtab", kStrtab, 0, 0, strtab, 0, 0, 0, 1, 0});

    // .shstrtab names every section including itself.
    const std::uint32_t shstrtabIndex = static_cast<std::uint32_t>(s.size());
    Bytes shstrtab;
    put8(shstrtab, 0);
    std::vector<std::uint32_t> nameOffsets;
    for (const Section& sec : s) {
        if (sec.name.empty()) {
            nameOffsets.push_back(0);
        } else {
            nameOffsets.push_back(static_cast<std::uint32_t>(shstrtab.size()));
            putStr(shstrtab, sec.name);
        }
    }
    nameOffsets.push_back(static_cast<std::uint32_t>(shstrtab.size()));
    putStr(shstrtab, ".shstrtab");
    s.push_back({".shstrtab", kStrtab, 0, 0, shstrtab, 0, 0, 0, 1, 0});

    // Lay the sections out after the 64-byte header.
    Bytes out(64, 0);
    for (std::size_t i = 1; i < s.size(); ++i) {
        const std::uint64_t a = s[i].align == 0 ? 1 : s[i].align;
        while (out.size() % a != 0) { out.push_back(0); }
        s[i].offset = out.size();
        out.insert(out.end(), s[i].data.begin(), s[i].data.end());
    }
    while (out.size() % 8 != 0) { out.push_back(0); }
    const std::uint64_t shoff = out.size();
    for (std::size_t i = 0; i < s.size(); ++i) {
        const Section& sec = s[i];
        put32(out, nameOffsets[i]);
        put32(out, sec.type);
        put64(out, sec.flags);
        put64(out, sec.addr);
        put64(out, sec.offset);
        put64(out, sec.type == kNobits ? sec.size : sec.data.size());
        put32(out, sec.link);
        put32(out, sec.info);
        put64(out, sec.align);
        put64(out, sec.entsize);
    }

    // The ELF header, written over the zeroed 64 bytes.
    Bytes h;
    const unsigned char ident[16] = {0x7F, 'E', 'L', 'F', 2 /*64-bit*/, 1 /*LE*/, 1 /*version*/,
                                     0,    0,   0,   0,   0,           0,         0,           0, 0};
    h.insert(h.end(), ident, ident + 16);
    put16(h, 3);        // ET_DYN, like a PIE executable or a shared object
    put16(h, machine);
    put32(h, 1);        // EV_CURRENT
    put64(h, 0);        // entry
    put64(h, 0);        // phoff
    put64(h, shoff);    // shoff
    put32(h, 0);        // flags
    put16(h, 64);       // ehsize
    put16(h, 0);        // phentsize
    put16(h, 0);        // phnum
    put16(h, 64);       // shentsize
    put16(h, static_cast<std::uint64_t>(s.size()));
    put16(h, shstrtabIndex);
    for (std::size_t i = 0; i < h.size(); ++i) { out[i] = h[i]; }
    return out;
}

}  // namespace elffixture
