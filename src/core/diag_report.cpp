// See diag_report.hpp for why the module table is snapshotted on the healthy
// path, why the build id and not the version is the durable key, and what a
// report has to carry for an engineer who was not there.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/diag_report.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <psapi.h>
#pragma comment(lib, "psapi.lib")
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

const char* leafName(const char* path) {
    if (path == nullptr) { return ""; }
    const char* leaf = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '\\' || *p == '/') { leaf = p + 1; }
    }
    return leaf;
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

}  // namespace

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
    return 0;
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
    if (ctx.plugins.empty()) {
        block += "plugin: (none)\n";
    } else {
        for (const std::string& s : ctx.plugins) { block += "plugin: " + s + "\n"; }
    }

    std::size_t n = block.size();
    if (n > kContextBytes - 1) { n = kContextBytes - 1; }
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

const std::vector<std::string>& bundleFieldNames() {
    // THE INVENTORY. PRIVACY.md documents these field by field, and
    // tests/test_diagnostics.cpp compares this list with what the bundle
    // actually emits IN BOTH DIRECTIONS: a field added to the bundle without
    // being added here fails, and so does a field documented here that the
    // bundle stopped emitting.
    static const std::vector<std::string> names = {
        "generated", "version",   "commit",     "os",
        "arch",      "mode",      "source",     "sample-rate",
        "device-open", "sdr-model", "plugin",   "log-path",
        "crash-dir", "last-run-unclean", "launches", "crashes",
        "log-lines-total"};
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
    static const std::vector<std::string> names = {"kind", "stalled-ms", "threshold-ms",
                                                   "signature", "threads"};
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
    kv("log-path", in.logPath);
    kv("crash-dir", in.crashDir);
    out += in.lastRunUnclean ? "last-run-unclean: yes\n" : "last-run-unclean: no\n";
    out += "launches: " + std::to_string(in.launches) + "\n";
    out += "crashes: " + std::to_string(in.crashes) + "\n";
    out += "log-lines-total: " + std::to_string(in.logLinesTotal) + "\n";

    out += "\n--- log ---\n";
    for (const std::string& line : in.logLines) {
        out += line;
        out += "\n";
    }
    return out;
}

}  // namespace cascade::core
