// See diag_report.hpp for why the module table is snapshotted on the healthy
// path, why the build id and not the version is the durable key, and what a
// report has to carry for an engineer who was not there.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/diag_report.hpp"

#include "core/diag_log.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <psapi.h>
#include <winternl.h>  // UNICODE_STRING, for the section-name query below
#pragma comment(lib, "psapi.lib")
#else
#include <elf.h>
#include <link.h>
#include <unistd.h>
#endif

namespace cascade::core {

namespace {

// 256 modules is roughly four times what a fully loaded session carries (the
// application, the CRT, the GL driver, SoapySDR and its vendor modules, and
// every decoder plugin). Fixed so the table is a plain array the fault path
// can search with no allocation.
constexpr int kMaxDiagModules = 256;

DiagModule g_modules[kMaxDiagModules];
std::atomic<int> g_moduleCount{0};

// The rendered context block. Fixed storage, written once on the healthy
// path, memcpy'd out by the fault path.
constexpr std::size_t kContextBytes = 4096;
char g_context[kContextBytes] = {};
std::atomic<int> g_contextLen{0};

void copyField(char* dst, std::size_t cap, const char* src) {
    if (cap == 0) { return; }
    std::size_t i = 0;
    while (src != nullptr && src[i] != '\0' && i + 1 < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

// FNV-1a, 64-bit. Chosen because it is four lines, has no tables and no
// allocation, so the same function can run on the fault path and in a test.
std::uint64_t fnv1a(const void* data, std::size_t n, std::uint64_t h) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<std::uint64_t>(p[i]);
        h *= 1099511628211ull;
    }
    return h;
}

// Shared by the PE reader below and the ELF reader in the platform block
// further down: both need "the file name with any directory stripped", and
// both kinds of path can appear with either separator (a PDB path recorded by
// a cross-built linker, a report read back on the other OS from the one that
// wrote it).
const char* leafName(const char* path) {
    if (path == nullptr) { return ""; }
    const char* leaf = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '\\' || *p == '/') { leaf = p + 1; }
    }
    return leaf;
}

#if defined(_WIN32)
// The CodeView RSDS record the linker stamps into every PE it produces. The
// layout is fixed and public; it is spelled out here rather than pulled from a
// header so the byte offsets are reviewable against the same offsets in
// tools/archive-symbols.ps1, which has to produce an identical key.
#pragma pack(push, 1)
struct CvInfoPdb70 {
    DWORD signature;  // 'SDSR'
    GUID guid;
    DWORD age;
    char pdbFileName[1];
};
#pragma pack(pop)

// The symbol-server key: the GUID in its canonical big-endian rendering with
// the hyphens removed, then the age in uppercase hex. Composed field by field
// so the byte order is explicit.
void formatBuildId(const GUID& g, DWORD age, char* out, std::size_t cap) {
    std::snprintf(out, cap, "%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%lX",
                  static_cast<unsigned long>(g.Data1), static_cast<unsigned>(g.Data2),
                  static_cast<unsigned>(g.Data3), static_cast<unsigned>(g.Data4[0]),
                  static_cast<unsigned>(g.Data4[1]), static_cast<unsigned>(g.Data4[2]),
                  static_cast<unsigned>(g.Data4[3]), static_cast<unsigned>(g.Data4[4]),
                  static_cast<unsigned>(g.Data4[5]), static_cast<unsigned>(g.Data4[6]),
                  static_cast<unsigned>(g.Data4[7]), static_cast<unsigned long>(age));
}

// Reads the CodeView record out of a MAPPED image (module base). In a mapped
// image the debug directory's AddressOfRawData is an RVA from the base, so no
// section walk is needed - that is only required for a file on disk.
bool codeViewFromImage(const unsigned char* base, char* buildId, std::size_t buildIdCap,
                       char* pdbName, std::size_t pdbCap) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { return false; }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { return false; }
    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (dir.VirtualAddress == 0 || dir.Size < sizeof(IMAGE_DEBUG_DIRECTORY)) { return false; }
    const auto* dbg = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + dir.VirtualAddress);
    const int entries = static_cast<int>(dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY));
    for (int i = 0; i < entries; ++i) {
        if (dbg[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW) { continue; }
        if (dbg[i].AddressOfRawData == 0 || dbg[i].SizeOfData < sizeof(CvInfoPdb70)) { continue; }
        const auto* cv =
            reinterpret_cast<const CvInfoPdb70*>(base + dbg[i].AddressOfRawData);
        if (cv->signature != 0x53445352u) { continue; }  // 'RSDS'
        formatBuildId(cv->guid, cv->age, buildId, buildIdCap);
        copyField(pdbName, pdbCap, leafName(cv->pdbFileName));
        return true;
    }
    return false;
}
#endif  // _WIN32

#if !defined(_WIN32)
// ---------------------------------------------------------------------------
// LINUX: the GNU build id, read the same way readelf/eu-unstrip do
// ---------------------------------------------------------------------------
//
// There is no PDB and no CodeView record here - the durable key an ELF module
// carries is the NT_GNU_BUILD_ID note the linker writes by default (GNU ld and
// lld both emit one unless explicitly disabled), a SHA-1 of the link's actual
// bytes. tools/elf_symmap.py reads the identical note out of the file on disk
// so the running image's key and the archived one are the same value by
// construction, exactly as peBuildId() and codeViewFromImage() agree above for
// a PE. Rendered as 40 lowercase hex digits, which is the spelling
// report_reader.hpp's isElfBuildId() and elfSymbolPath() already expect (that
// reader and its addr2line-based resolver predate this change and needed no
// edits at all).
//
// Read from the MAPPED image via the program headers dl_iterate_phdr hands
// out - a PT_NOTE segment's file content is also its memory content once
// loaded, so no file I/O is needed on this, the healthy, path.
bool gnuBuildIdFromNote(const ElfW(Phdr) * phdrs, int phnum, ElfW(Addr) base, char* out,
                        std::size_t cap) {
    for (int i = 0; i < phnum; ++i) {
        if (phdrs[i].p_type != PT_NOTE) { continue; }
        const auto* p = reinterpret_cast<const unsigned char*>(base + phdrs[i].p_vaddr);
        std::size_t remaining = static_cast<std::size_t>(phdrs[i].p_memsz);
        std::size_t off = 0;
        while (off + sizeof(ElfW(Nhdr)) <= remaining) {
            const auto* nh = reinterpret_cast<const ElfW(Nhdr)*>(p + off);
            const std::size_t hdrsz = sizeof(ElfW(Nhdr));
            const std::size_t namesz = (static_cast<std::size_t>(nh->n_namesz) + 3u) & ~3u;
            const std::size_t descsz = (static_cast<std::size_t>(nh->n_descsz) + 3u) & ~3u;
            if (hdrsz + namesz + descsz > remaining - off) { break; }  // a torn note; stop
            if (nh->n_type == NT_GNU_BUILD_ID && nh->n_descsz > 0) {
                const unsigned char* desc = p + off + hdrsz + namesz;
                static const char kHex[] = "0123456789abcdef";
                std::size_t at = 0;
                for (unsigned k = 0; k < nh->n_descsz && at + 2 < cap; ++k) {
                    out[at++] = kHex[(desc[k] >> 4) & 0xF];
                    out[at++] = kHex[desc[k] & 0xF];
                }
                out[at] = '\0';
                return at > 0;
            }
            off += hdrsz + namesz + descsz;
        }
    }
    return false;
}

// The end of the highest PT_LOAD segment, relative to the load bias - the same
// "does this address belong to this module" extent resolveAddress() already
// tests every frame against for a PE. A module with no PT_LOAD (there is
// always at least one for anything actually mapped) resolves to size 0, which
// resolveAddress() correctly treats as "nothing here".
std::size_t elfLoadSpan(const ElfW(Phdr) * phdrs, int phnum) {
    ElfW(Addr) end = 0;
    for (int i = 0; i < phnum; ++i) {
        if (phdrs[i].p_type != PT_LOAD) { continue; }
        const ElfW(Addr) segEnd = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        if (segEnd > end) { end = segEnd; }
    }
    return static_cast<std::size_t>(end);
}

struct ElfModuleWalk {
    int count = 0;
};

int elfModuleCallback(struct dl_phdr_info* info, std::size_t /*size*/, void* data) {
    auto* walk = static_cast<ElfModuleWalk*>(data);
    if (walk->count >= kMaxDiagModules) { return 0; }  // keep walking; nothing more to store

    DiagModule m;
    m.base = static_cast<std::uintptr_t>(info->dlpi_addr);
    m.size = elfLoadSpan(info->dlpi_phdr, info->dlpi_phnum);

    // dl_iterate_phdr reports the MAIN executable with an empty dlpi_name (and,
    // on some libcs, the vdso the same way) - it is the one module that was
    // never dlopen()'d, so the loader never recorded a path for it. The real
    // path is what this process was actually exec'd from.
    if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0') {
        copyField(m.name, sizeof(m.name), leafName(info->dlpi_name));
    } else {
        char exe[4096] = {};
        const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            exe[n] = '\0';
            copyField(m.name, sizeof(m.name), leafName(exe));
        } else {
            copyField(m.name, sizeof(m.name), "(main)");
        }
    }

    gnuBuildIdFromNote(info->dlpi_phdr, info->dlpi_phnum, info->dlpi_addr, m.buildId,
                       sizeof(m.buildId));
    // No PDB on this platform; the report writer prints "(none)" for an empty
    // field exactly as it already does for a PE module with no CodeView
    // record, so nothing downstream needs to know which reason applies.

    g_modules[walk->count] = m;
    ++walk->count;
    return 0;
}
#endif  // !_WIN32

#if defined(_WIN32)
// ---------------------------------------------------------------------------
// WINDOWS: naming a module the snapshot has never heard of, FROM THE FAULT PATH
// ---------------------------------------------------------------------------
//
// Everything below runs inside the crash handler, so it obeys the handler's
// rules (docs/DIAGNOSTICS.md, "What a fault handler is allowed to do"): no
// allocation, no lock, no CRT formatting. What it uses is VirtualQuery and
// ntdll's NtQueryVirtualMemory - system calls that read the address space's
// own bookkeeping and take neither the loader lock nor the heap lock - and
// plain reads of the module's own headers, behind a __try. The scratch it
// needs is STATIC, never on the stack the fault may have just exhausted; that
// is safe because the fault path admits exactly one writer at a time.

// NtQueryVirtualMemory(process, address, MemorySectionName = 2, buffer,
// length, returned): for an address inside a mapped image it fills the buffer
// with the NT path of the file the image was mapped from. Resolved on the
// healthy path (prepareModuleAdoption) - GetProcAddress in a handler is a call
// into the loader.
using NtQueryVirtualMemoryFn = LONG(NTAPI*)(HANDLE, PVOID, int, PVOID, SIZE_T, PSIZE_T);
std::atomic<NtQueryVirtualMemoryFn> g_ntQueryVirtualMemory{nullptr};
constexpr int kMemorySectionName = 2;

// 1024 UTF-16 units: far longer than any path the loader maps from. A longer
// one makes the query fail and the module is named "unknown-image", which
// still gives its base, its size and the offset - nothing is invented.
constexpr std::size_t kSectionNameChars = 1024;
struct SectionNameBuffer {
    UNICODE_STRING name;
    WCHAR chars[kSectionNameChars];
};
SectionNameBuffer g_sectionName;
MEMORY_BASIC_INFORMATION g_adoptQuery;
DiagModule g_adoptModule;

// The image's own idea of its size, read from its headers. POD only and behind
// __try: a header page that cannot be read ends the lookup instead of the
// process.
bool imageSizeFromHeaders(std::uintptr_t base, std::size_t& size) {
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) { return false; }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) { return false; }
        size = static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage);
        return size != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The file name (no directory) out of the NT path the query left in
// g_sectionName, narrowed by hand: printable ASCII kept, anything else '?'.
// The pointer the kernel wrote is checked to lie INSIDE the buffer before it is
// followed. Returns false when there is no name to give.
bool leafNameFromSectionName(char* dst, std::size_t cap) {
    const UNICODE_STRING& u = g_sectionName.name;
    const WCHAR* first = g_sectionName.chars;
    const WCHAR* last = g_sectionName.chars + kSectionNameChars;
    const std::size_t n = u.Length / sizeof(WCHAR);
    if (u.Buffer == nullptr || n == 0 || u.Buffer < first || u.Buffer + n > last) { return false; }
    std::size_t start = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (u.Buffer[i] == L'\\' || u.Buffer[i] == L'/') { start = i + 1; }
    }
    std::size_t at = 0;
    for (std::size_t i = start; i < n && at + 1 < cap; ++i) {
        const WCHAR c = u.Buffer[i];
        dst[at++] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
    }
    dst[at] = '\0';
    return at > 0;
}
#endif  // _WIN32

}  // namespace

void prepareModuleAdoption() {
#if defined(_WIN32)
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) { return; }
    g_ntQueryVirtualMemory.store(
        reinterpret_cast<NtQueryVirtualMemoryFn>(
            reinterpret_cast<void*>(::GetProcAddress(ntdll, "NtQueryVirtualMemory"))),
        std::memory_order_release);
#endif
}

bool adoptModuleContaining(std::uintptr_t addr) {
#if defined(_WIN32)
    if (addr == 0) { return false; }
    {
        DiagModule known;
        std::uintptr_t off = 0;
        if (resolveAddress(addr, known, off)) { return true; }
    }
    const int n = g_moduleCount.load(std::memory_order_acquire);
    if (n >= kMaxDiagModules) { return false; }

    // IMAGE memory only: code in a heap, a JIT page or a stack has no module
    // to name, and inventing one would put a lie in the report.
    if (::VirtualQuery(reinterpret_cast<LPCVOID>(addr), &g_adoptQuery, sizeof(g_adoptQuery)) !=
        sizeof(g_adoptQuery)) {
        return false;
    }
    if (g_adoptQuery.Type != MEM_IMAGE || g_adoptQuery.State != MEM_COMMIT) { return false; }
    const auto base = reinterpret_cast<std::uintptr_t>(g_adoptQuery.AllocationBase);
    if (base == 0 || base > addr) { return false; }

    g_adoptModule = DiagModule{};
    g_adoptModule.base = base;
    std::size_t size = 0;
    if (!imageSizeFromHeaders(base, size) || addr - base >= size) {
        // Headers unreadable (or not describing this address): the part of the
        // allocation this region proves is mapped, up to and past `addr`.
        size = reinterpret_cast<std::uintptr_t>(g_adoptQuery.BaseAddress) +
               g_adoptQuery.RegionSize - base;
    }
    g_adoptModule.size = size;

    bool named = false;
    if (const NtQueryVirtualMemoryFn query = g_ntQueryVirtualMemory.load(std::memory_order_acquire)) {
        g_sectionName.name = UNICODE_STRING{};
        SIZE_T returned = 0;
        const LONG status = query(::GetCurrentProcess(), reinterpret_cast<PVOID>(addr),
                                  kMemorySectionName, &g_sectionName, sizeof(g_sectionName),
                                  &returned);
        named = status >= 0 &&
                leafNameFromSectionName(g_adoptModule.name, sizeof(g_adoptModule.name));
    }
    if (!named) { copyField(g_adoptModule.name, sizeof(g_adoptModule.name), "unknown-image"); }
    // No pdb and no build id: reading the CodeView record means formatting it,
    // and formatting is a CRT call. A module nobody archived symbols for - the
    // vendor's, which is what arrives late - needs its name, base and size.

    g_modules[n] = g_adoptModule;
    g_moduleCount.store(n + 1, std::memory_order_release);
    return true;
#else
    (void)addr;
    return false;
#endif
}

int refreshModuleTable() {
#if defined(_WIN32)
    // HEALTHY PATH ONLY. EnumProcessModules reads loader data under the loader
    // lock; doing this from a fault handler is the one way to turn a crash
    // into a deadlock inside the reporter. See the header.
    HMODULE mods[kMaxDiagModules];
    DWORD needed = 0;
    if (::EnumProcessModules(::GetCurrentProcess(), mods, sizeof(mods), &needed) == 0) {
        return g_moduleCount.load(std::memory_order_relaxed);
    }
    int n = static_cast<int>(needed / sizeof(HMODULE));
    if (n > kMaxDiagModules) { n = kMaxDiagModules; }

    int kept = 0;
    for (int i = 0; i < n; ++i) {
        MODULEINFO mi{};
        if (::GetModuleInformation(::GetCurrentProcess(), mods[i], &mi, sizeof(mi)) == 0) {
            continue;
        }
        DiagModule m;
        m.base = reinterpret_cast<std::uintptr_t>(mi.lpBaseOfDll);
        m.size = static_cast<std::size_t>(mi.SizeOfImage);
        char nameBuf[MAX_PATH] = {};
        ::GetModuleBaseNameA(::GetCurrentProcess(), mods[i], nameBuf, sizeof(nameBuf));
        copyField(m.name, sizeof(m.name), nameBuf);
        codeViewFromImage(reinterpret_cast<const unsigned char*>(mi.lpBaseOfDll), m.buildId,
                          sizeof(m.buildId), m.pdb, sizeof(m.pdb));
        g_modules[kept] = m;
        ++kept;
    }
    g_moduleCount.store(kept, std::memory_order_release);
    return kept;
#else
    // HEALTHY PATH ONLY, same as the Windows half above: dl_iterate_phdr walks
    // the dynamic linker's own link map, which glibc guards with a lock
    // (dl_load_lock/dl_load_write_lock) that dlopen/dlclose can hold - reading
    // it from a signal handler mid-fault is the same class of risk
    // EnumProcessModules is under the loader lock, so this is called only at
    // start-up and again after anything that can load code (a plugin dlopen).
    ElfModuleWalk walk;
    ::dl_iterate_phdr(elfModuleCallback, &walk);
    g_moduleCount.store(walk.count, std::memory_order_release);
    return walk.count;
#endif
}

int moduleCount() { return g_moduleCount.load(std::memory_order_acquire); }

bool moduleAt(int index, DiagModule& out) {
    const int n = g_moduleCount.load(std::memory_order_acquire);
    if (index < 0 || index >= n) { return false; }
    out = g_modules[index];
    return true;
}

bool resolveAddress(std::uintptr_t addr, DiagModule& out, std::uintptr_t& offset) {
    const int n = g_moduleCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        const DiagModule& m = g_modules[i];
        if (m.base != 0 && addr >= m.base && addr < m.base + m.size) {
            out = m;
            offset = addr - m.base;
            return true;
        }
    }
    return false;
}

bool peBuildId(const std::string& path, std::string& buildId, std::string& pdbName) {
    buildId.clear();
    pdbName.clear();
#if defined(_WIN32)
    std::ifstream in(path, std::ios::binary);
    if (!in) { return false; }
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
    if (b.size() < 0x40 || b[0] != 'M' || b[1] != 'Z') { return false; }

    auto u16 = [&b](std::size_t o) -> unsigned {
        return static_cast<unsigned>(b[o]) | (static_cast<unsigned>(b[o + 1]) << 8);
    };
    auto u32 = [&b](std::size_t o) -> std::uint32_t {
        return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8) |
               (static_cast<std::uint32_t>(b[o + 2]) << 16) |
               (static_cast<std::uint32_t>(b[o + 3]) << 24);
    };

    const std::size_t peOff = u32(0x3C);
    if (peOff == 0 || peOff + 24 >= b.size() || u32(peOff) != 0x00004550u) { return false; }
    const std::size_t coff = peOff + 4;
    const unsigned numSections = u16(coff + 2);
    const unsigned sizeOfOptional = u16(coff + 16);
    const std::size_t optOff = coff + 20;
    if (optOff + sizeOfOptional > b.size()) { return false; }
    const unsigned magic = u16(optOff);
    std::size_t dirOff = 0;
    if (magic == 0x20b) {
        dirOff = optOff + 112;  // PE32+
    } else if (magic == 0x10b) {
        dirOff = optOff + 96;  // PE32
    } else {
        return false;
    }
    // Data directory 6 is IMAGE_DIRECTORY_ENTRY_DEBUG.
    const std::size_t dbgDirEntry = dirOff + 6 * 8;
    if (dbgDirEntry + 8 > b.size()) { return false; }
    const std::uint32_t debugRva = u32(dbgDirEntry);
    const std::uint32_t debugSize = u32(dbgDirEntry + 4);
    if (debugRva == 0 || debugSize == 0) { return false; }

    // RVA -> file offset through the section table. Unlike a mapped image, a
    // file on disk has its sections at their raw offsets.
    const std::size_t secOff = optOff + sizeOfOptional;
    std::size_t fileOff = 0;
    for (unsigned i = 0; i < numSections; ++i) {
        const std::size_t s = secOff + i * 40u;
        if (s + 40 > b.size()) { break; }
        const std::uint32_t vsize = u32(s + 8);
        const std::uint32_t va = u32(s + 12);
        const std::uint32_t rawSize = u32(s + 16);
        const std::uint32_t raw = u32(s + 20);
        const std::uint32_t span = (vsize > rawSize) ? vsize : rawSize;
        if (debugRva >= va && debugRva < va + span) {
            fileOff = raw + (debugRva - va);
            break;
        }
    }
    if (fileOff == 0) { return false; }

    const int entries = static_cast<int>(debugSize / 28u);
    for (int i = 0; i < entries; ++i) {
        const std::size_t e = fileOff + static_cast<std::size_t>(i) * 28u;
        if (e + 28 > b.size()) { break; }
        if (u32(e + 12) != 2u) { continue; }  // IMAGE_DEBUG_TYPE_CODEVIEW
        const std::uint32_t cvSize = u32(e + 16);
        const std::uint32_t cvOff = u32(e + 24);
        if (cvOff == 0 || cvSize < 25 || cvOff + cvSize > b.size()) { continue; }
        if (std::memcmp(&b[cvOff], "RSDS", 4) != 0) { continue; }

        GUID g{};
        g.Data1 = u32(cvOff + 4);
        g.Data2 = static_cast<unsigned short>(u16(cvOff + 8));
        g.Data3 = static_cast<unsigned short>(u16(cvOff + 10));
        for (int k = 0; k < 8; ++k) { g.Data4[k] = b[cvOff + 12 + static_cast<std::size_t>(k)]; }
        const std::uint32_t age = u32(cvOff + 20);
        char id[64] = {};
        formatBuildId(g, age, id, sizeof(id));
        buildId = id;

        std::string recorded;
        for (std::size_t p = cvOff + 24; p < cvOff + cvSize && p < b.size() && b[p] != 0; ++p) {
            recorded.push_back(static_cast<char>(b[p]));
        }
        pdbName = leafName(recorded.c_str());
        return !buildId.empty();
    }
    return false;
#else
    (void)path;
    return false;
#endif
}

namespace {

// "-50 dB": a level or a threshold, whole decibels. Never a frequency.
std::string wholeDb(double db) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f dB", db);
    return buf;
}

// THE FOUR SOUND-PATH LINES (see DiagAudio for why they exist and what they
// must never carry). Each says "(unknown)" when nothing filled the struct in,
// so an unfilled context cannot read as a healthy one.
std::string audioOutputText(const DiagAudio& a) {
    if (!a.known) { return "(unknown)"; }
    // An open in flight is a state of the driver, not of the sink - nothing
    // else is true of the stream until it finishes (the Sinks panel makes the
    // same call).
    if (a.opening) { return "opening"; }
    if (!a.everOpened) { return "none - no output device has opened"; }
    if (!a.alive) { return "stopped - the stream died and is being reopened"; }
    std::string s = "open, ";
    s += a.hostApi.empty() ? std::string("unknown host API") : a.hostApi;
    s += a.channels == 2 ? ", 2 channels" : ", 1 channel";
    if (a.restarts > 0) {
        s += ", restarted " + std::to_string(a.restarts) + (a.restarts == 1 ? " time" : " times");
    }
    return s;
}

std::string volumeText(const DiagAudio& a) {
    if (!a.known) { return "(unknown)"; }
    int v = a.volumePercent;
    if (v < 0) { v = 0; }
    if (v > 100) { v = 100; }
    return std::to_string(v) + "%";
}

// WHO muted it, because "muted" alone sends the reporter to the wrong control.
// A plugin is described, never named: the Sinks panel names it, but a bundle
// that said "ADS-B" would say which band the receiver was parked on, and what
// somebody listens to is the most sensitive thing this application knows.
std::string mutedText(const DiagAudio& a) {
    if (!a.known) { return "(unknown)"; }
    std::string s;
    auto add = [&s](const char* who) {
        if (!s.empty()) { s += " + "; }
        s += who;
    };
    if (a.mutedByUser) { add("you"); }
    if (a.mutedByPlugin) { add("a decoder plugin"); }
    if (a.mutedByTransmit) { add("transmit key"); }
    return s.empty() ? std::string("no") : s;
}

// THE PATCH PAGE'S RADIOS, as a count and the driver kinds: "none", "1 (rtlsdr)",
// "2 (rtlsdr, soapy)". A kind is only ever the LEADING run of lower-case letters
// and digits of what it is handed, at most sixteen of them, and anything that
// leaves nothing reads "other". The caller hands over the part of a device key
// before its first '|', and this is the second lock on the same door: a whole key
// ("rtlsdr|serial=0000000A") stops at the '|', and an argument string that starts
// with a capital or a symbol leaves nothing, so neither can carry a serial into a
// report. At most eight kinds are listed; the count always says how many there are.
std::string patchRadiosText(const std::vector<std::string>& kinds) {
    if (kinds.empty()) { return "none"; }
    constexpr std::size_t kListed = 8;
    std::string s = std::to_string(kinds.size()) + " (";
    for (std::size_t i = 0; i < kinds.size() && i < kListed; ++i) {
        std::string k;
        for (const char c : kinds[i]) {
            const bool lower = c >= 'a' && c <= 'z';
            const bool digit = c >= '0' && c <= '9';
            if (!lower && !digit) { break; }
            k += c;
            if (k.size() >= 16) { break; }
        }
        if (i > 0) { s += ", "; }
        s += k.empty() ? std::string("other") : k;
    }
    if (kinds.size() > kListed) { s += ", ..."; }
    return s + ")";
}

// The threshold, the gate's own state, and - when the receiver has measured one
// - the channel power the gate is judging, so "the squelch is above the signal"
// reads straight off one line.
std::string squelchText(const DiagAudio& a) {
    if (!a.known) { return "(unknown)"; }
    std::string s = wholeDb(a.squelchDb);
    s += a.squelchOpen ? ", open" : ", closed";
    if (a.signalDb > -199.0) { s += " (signal " + wholeDb(a.signalDb) + ")"; }
    return s;
}

}  // namespace

void setDiagContext(const DiagContext& ctx) {
    // Rendered ONCE, here, on the healthy path. The fault path writes these
    // bytes out and formats nothing.
    //
    // Every line is "<name>: <value>", which is also the shape the bundle's
    // field inventory is parsed from - so a field added here without being
    // added to bundleFieldNames() and to PRIVACY.md fails
    // tests/test_diagnostics.cpp rather than shipping undocumented.
    // Assembled as a std::string here (healthy path, allocation is fine) and
    // then copied ONCE into the fixed buffer the fault path reads.
    char rate[32] = {};
    std::snprintf(rate, sizeof(rate), "%.0f", ctx.sampleRateHz);

    std::string block;
    block.reserve(1024);
    block += "version: " + ctx.version + "\n";
    block += "commit: " + ctx.commit + "\n";
    block += "os: " + ctx.os + "\n";
    block += "arch: " + ctx.arch + "\n";
    block += "mode: " + ctx.mode + "\n";
    block += "source: " + ctx.sourceKind + "\n";
    // Deliberately NOT "... Hz", and deliberately no tuned frequency anywhere:
    // what somebody listens to is the most sensitive thing this application
    // knows. tests/test_diagnostics.cpp asserts its absence.
    block += std::string("sample-rate: ") + rate + "\n";
    block += std::string("device-open: ") + (ctx.deviceOpen ? "yes" : "no") + "\n";
    block += "sdr-model: " + (ctx.sdrModel.empty() ? std::string("(none)") : ctx.sdrModel) + "\n";
    // How the radio's crystal is being corrected (0.99.56): support's first
    // question when a signal is "not where the band plan says".
    block += "ppm: " + (ctx.ppm.empty() ? std::string("off") : ctx.ppm) + "\n";
    // THE SOUND PATH (0.99.61), BEFORE the plugin list: that list is the one
    // part of this block that grows without bound (up to 32 lines), and a
    // block that overruns its fixed buffer is cut at the END.
    block += "audio-output: " + audioOutputText(ctx.audio) + "\n";
    block += "volume: " + volumeText(ctx.audio) + "\n";
    block += "audio-muted: " + mutedText(ctx.audio) + "\n";
    block += "squelch: " + squelchText(ctx.audio) + "\n";
    // THE PATCH PAGE'S RADIOS (0.99.62), also before the plugin list. `source`
    // above is the receiver's radio; these are the OTHER signal paths.
    block += "patch-radios: " + patchRadiosText(ctx.patchRadioKinds) + "\n";
    if (ctx.plugins.empty()) {
        block += "plugin: (none)\n";
    } else {
        for (const std::string& s : ctx.plugins) { block += "plugin: " + s + "\n"; }
    }

    std::size_t n = block.size();
    if (n > kContextBytes - 1) { n = kContextBytes - 1; }
    // UNCHANGED MEANS UNTOUCHED. The application renders this every frame
    // (AppWindow::refreshDiagContext), so the common call has nothing new to
    // say, and rewriting the buffer 60 times a second would give a fault
    // handler on another thread 60 chances a second to read it half-written.
    // Only the writer ever changes it, so the comparison needs no lock.
    if (static_cast<int>(n) == g_contextLen.load(std::memory_order_acquire) &&
        std::memcmp(g_context, block.data(), n) == 0) {
        return;
    }
    std::memcpy(g_context, block.data(), n);
    g_context[n] = '\0';
    g_contextLen.store(static_cast<int>(n), std::memory_order_release);
}

std::string diagContextBlock() {
    const int n = g_contextLen.load(std::memory_order_acquire);
    if (n <= 0) { return std::string(); }
    return std::string(g_context, g_context + n);
}

const char* diagContextRaw(int& lenOut) {
    lenOut = g_contextLen.load(std::memory_order_acquire);
    if (lenOut < 0) { lenOut = 0; }
    return g_context;
}

void crashSignatureRaw(unsigned long code, const char* moduleName, std::uintptr_t offset,
                       char out[17]) {
    std::uint64_t h = 1469598103934665603ull;
    const std::uint32_t c = static_cast<std::uint32_t>(code);
    h = fnv1a(&c, sizeof(c), h);
    if (moduleName != nullptr) { h = fnv1a(moduleName, std::strlen(moduleName), h); }
    const std::uint64_t off = static_cast<std::uint64_t>(offset);
    h = fnv1a(&off, sizeof(off), h);
    // Hand-rendered: this runs on the fault path, where snprintf could take a
    // locale lock and a locale lock is a lock.
    static const char kHex[] = "0123456789ABCDEF";
    for (int i = 0; i < 16; ++i) { out[i] = kHex[(h >> ((15 - i) * 4)) & 0xFull]; }
    out[16] = '\0';
}

std::string crashSignature(unsigned long code, const char* moduleName, std::uintptr_t offset) {
    // Built from the fault kind, the faulting MODULE and the offset WITHIN it -
    // never the absolute address (ASLR moves it every run) and never the time
    // (unique by construction). Two runs of the same fault group together.
    char buf[17];
    crashSignatureRaw(code, moduleName, offset, buf);
    return std::string(buf);
}

std::uintptr_t mainImageBase() {
#if defined(_WIN32)
    // The process block's own image base: no loader lock, no allocation.
    return reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
#else
    // dl_iterate_phdr always visits the main program first (see
    // elfModuleCallback), so module 0 of the snapshot is it. A non-PIE
    // executable reports a load bias of 0, which reads as "unknown".
    DiagModule m;
    return moduleAt(0, m) ? m.base : static_cast<std::uintptr_t>(0);
#endif
}

bool inMainImage(std::uintptr_t addr) {
    const std::uintptr_t self = mainImageBase();
    if (self == 0) { return false; }
    DiagModule m;
    std::uintptr_t off = 0;
    return resolveAddress(addr, m, off) && m.base == self;
}

std::string freezeSignature(unsigned long kindTag, const std::uintptr_t* frames, int count) {
    // No frames at all keys exactly as the old code did for a thread with none:
    // an unnamed module at offset 0.
    if (frames == nullptr || count <= 0) { return crashSignature(kindTag, "?", 0); }

    DiagModule m;
    std::uintptr_t off = 0;
    // THE FIRST FRAME OF OURS, nearest the top: the code of this program that
    // was waiting, which is what tells one freeze from another.
    const std::uintptr_t self = mainImageBase();
    if (self != 0) {
        for (int i = 0; i < count; ++i) {
            if (resolveAddress(frames[i], m, off) && m.base == self) {
                return crashSignature(kindTag, m.name, off);
            }
        }
    }
    // Nothing of ours on this stack: the old key, frame 0.
    if (resolveAddress(frames[0], m, off)) { return crashSignature(kindTag, m.name, off); }
    return crashSignature(kindTag, "?", 0);
}

const std::vector<std::string>& bundleFieldNames() {
    // THE INVENTORY. PRIVACY.md documents these field by field, and
    // tests/test_diagnostics.cpp compares this list with what the bundle
    // actually emits IN BOTH DIRECTIONS: a field added to the bundle without
    // being added here fails, and so does a field documented here that the
    // bundle stopped emitting.
    static const std::vector<std::string> names = {
        "generated", "version",   "commit",     "os",
        "arch",      "mode",      "source",     "sample-rate",
        "device-open", "sdr-model", "ppm",      "plugin",   "log-path",
        "crash-dir", "last-run-unclean", "launches", "crashes",
        "log-lines-total", "sdrplay-service",
        // The sound path (0.99.61): see DiagAudio.
        "audio-output", "volume", "audio-muted", "squelch",
        // The patch page's radios, by driver kind (0.99.62): see
        // DiagContext::patchRadioKinds.
        "patch-radios"};
    return names;
}

const std::vector<std::string>& crashReportFieldNames() {
    // The header crash_handler.cpp writeReport() emits, in order, before
    // "--- context ---". tests/test_crash_capture.cpp parses a report from a
    // REAL fault in a REAL child process and compares the set both ways, so a
    // seventh line cannot be added here without the test failing and
    // PRIVACY.md being updated with it.
    static const std::vector<std::string> names = {"kind",      "reason",    "code",
                                                   "address",   "signature", "thread"};
    return names;
}

const std::vector<std::string>& hangReportFieldNames() {
    // The header hang_watchdog.cpp captureAllThreads() emits before
    // "--- context ---". tests/test_diag_hang.cpp compares the set both ways
    // against a report written by a real stall.
    // `note` is one sentence saying what KIND of stall this was, and it is
    // always written - for a "hang" as well as a "stall" - precisely so this
    // set does not depend on which. A field that appears on one path and not
    // another fails the both-ways comparison for every report of the other
    // kind, which is how a report with nothing wrong with it would start
    // failing the suite.
    static const std::vector<std::string> names = {"kind",      "note",     "stalled-ms",
                                                   "threshold-ms", "signature", "threads"};
    return names;
}

std::string buildDiagnosticsBundle(const DiagBundleInput& in) {
    std::string out;
    out.reserve(8192);
    // The title carries no "name: value" pair on purpose: the inventory parser
    // in the test treats every such line in the header as a declared field.
    out += "FoxSDR diagnostics bundle\n";

    char stamp[32] = {};
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d", tmv.tm_year + 1900,
                  tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    out += "generated: ";
    out += stamp;
    out += "\n";

    // The context block is REUSED, not re-derived: it is the same text a crash
    // report carries, so a support bundle and a crash report describe the
    // application in identical words, and there is one place to keep in step
    // with PRIVACY.md rather than two.
    setDiagContext(in.context);
    out += diagContextBlock();

    auto kv = [&out](const char* name, const std::string& value) {
        out += name;
        out += ": ";
        out += value.empty() ? std::string("(none)") : value;
        out += "\n";
    };
    // Paths under the user's profile: never with the account name in them
    // (core::scrubUploadPath). The bundle is made to be pasted into a public
    // bug report, and one was, with "C:\Users\<their name>\..." in it.
    kv("log-path", scrubUploadPath(in.logPath));
    kv("crash-dir", scrubUploadPath(in.crashDir));
    out += in.lastRunUnclean ? "last-run-unclean: yes\n" : "last-run-unclean: no\n";
    out += "launches: " + std::to_string(in.launches) + "\n";
    out += "crashes: " + std::to_string(in.crashes) + "\n";
    out += "log-lines-total: " + std::to_string(in.logLinesTotal) + "\n";
    // A service's name and state, nothing about the user (see PRIVACY.md).
    kv("sdrplay-service", in.sdrPlayService);

    out += "\n--- log ---\n";
    // Scrubbed exactly as an uploaded report's log is (core::scrubUploadLog):
    // a bundle is made to be pasted into an email or a public issue.
    // The plugin inventory the header above has just printed is passed in,
    // so a plugin whose NAME says "MHz" keeps its name and version in the
    // log too (0.99.44, GitHub issue 5: "plugin: loaded # MHz Beacons #").
    for (const std::string& line : scrubUploadLog(in.logLines, in.context.plugins)) {
        out += line;
        out += "\n";
    }

    // THE SESSIONS BEFORE THIS ONE, after the current log and each under its own
    // heading (0.99.62). Both are written ONLY when the caller asked for them
    // (history.included): with diagnostics off nothing is read and nothing is
    // added, not even a heading. Every line of either goes through the same scrub
    // as the log above - the previous session's log is a log, and the report
    // lines are built from fields a file supplied.
    if (in.history.included) {
        const DiagHistory& h = in.history;

        out += "\n";
        out += kPreviousSessionHeading;
        out += "\n";
        if (h.pending) {
            out += "(still being read - copy the diagnostics again in a moment)\n";
        } else if (!h.previous.found) {
            out += "none - " +
                   (h.previous.reason.empty() ? std::string("there is no earlier session to show")
                                              : h.previous.reason) +
                   "\n";
        } else {
            std::string head = "session: " + (h.previous.build.empty()
                                                  ? std::string("(build not recorded)")
                                                  : h.previous.build);
            head += ", " + std::to_string(h.previous.sessionLines) + " lines in the log files, ";
            head += h.previous.lines.size() < h.previous.sessionLines
                        ? "the last " + std::to_string(h.previous.lines.size()) + " follow"
                        : std::string("all of them follow");
            out += scrubUploadLine(head);
            out += "\n";
            for (const std::string& line : scrubUploadLog(h.previous.lines, in.context.plugins)) {
                out += line;
                out += "\n";
            }
        }

        out += "\n";
        out += kReportsHeading;
        out += "\n";
        if (h.pending) {
            out += "(still being read - copy the diagnostics again in a moment)\n";
        } else if (!h.reports.readable) {
            out += "none - the reports folder could not be read\n";
        } else if (h.reports.newest.empty()) {
            out += "none - the reports folder holds no crash or freeze report\n";
        } else {
            out += "newest " + std::to_string(h.reports.newest.size()) + " of " +
                   std::to_string(h.reports.total) + (h.reports.totalCapped ? "+" : "") +
                   " - when it was written, what it was, and what became of sending it\n";
            for (const ReportSummary& r : h.reports.newest) {
                out += scrubUploadLine(reportSummaryLine(r));
                out += "\n";
            }
        }
    }
    return out;
}

}  // namespace cascade::core
