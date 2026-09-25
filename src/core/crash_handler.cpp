// See crash_handler.hpp for which four entry points are covered and why, what
// is deliberately NOT covered, and the rules the fault path obeys.
//
// WHAT THE FAULT PATH TOUCHES, in full, so the claim in the header can be
// checked rather than believed:
//
//   NtCreateFile / WriteFile / CloseHandle  kernel calls, no CRT buffering
//   GetLocalTime / GetCurrentProcessId      TEB and shared-page reads
//   RtlCaptureStackBackTrace or
//   RtlLookupFunctionEntry + RtlVirtualUnwind
//   Sleep, only while ANOTHER thread's report is being written
//   memcpy, and integer rendering written by hand
//
// It allocates nothing, opens no CRT stream, takes none of this application's
// locks, and calls no snprintf (which can take a locale lock).
//
// NOT EVEN THE PROCESS HEAP, and that was not true until F204602B5329B268
// (0.99.35). The report used to be opened with CreateFileA, whose DOS-to-NT
// path conversion allocates from the process heap - so a fault on a thread
// whose neighbour held the heap lock stalled the handler (6 of 8 staged runs
// never wrote a report and were killed at the timeout), and a heap that the
// fault itself had trashed left no report in 5 of 8. The report is now
// created with NtCreateFile on an NT path built on the HEALTHY path (see
// prepareNtCrashDir), which is a system call and nothing else. The two large
// buffers it needs - the log ring copy and a CONTEXT to unwind - are STATIC,
// not stack locals, because the fault this most needs to survive is a stack
// overflow, where there is barely a page of stack left.
//
// THE ONE HONEST CAVEAT. Unwinding a stack on x64 means asking ntdll where a
// function's unwind data is (RtlLookupFunctionEntry), and that reads the
// loader's inverted function table under a lock another thread could hold. It
// is not avoidable - there is no unwind without it - so the report is written
// INCREMENTALLY, in decreasing order of value: the fault kind, the faulting
// address as module+offset and the application context reach the disk BEFORE
// the walk is attempted. If the walk ever does deadlock, the file already on
// disk still names the bug.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/crash_handler.hpp"

#include "core/diag_log.hpp"
#include "core/diag_report.hpp"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#include <windows.h>

#include <dbghelp.h>
#include <intrin.h>
#include <io.h>
#include <winternl.h>
#pragma comment(lib, "dbghelp.lib")
#elif defined(__linux__)
// The Linux implementation of every entry point below - see
// crash_handler_posix.cpp for what it covers and why it is a separate file
// rather than a parallel block in this one: it needs its own headers
// (libunwind, <signal.h>, sigaltstack) that would otherwise sit beside
// dbghelp.h's Windows-only equivalents above with nothing in common.
#include "core/crash_handler_posix.hpp"
#endif

namespace cascade::core {

namespace {

constexpr std::size_t kPathBytes = 512;
constexpr int kMaxFrames = 62;

// ALL FIXED STORAGE, prepared while the process is still healthy.
char g_crashDir[kPathBytes] = {};
char g_lastPath[kPathBytes] = {};
bool g_enabled = false;
bool g_minidump = false;
bool g_exitAfterReport = false;
long g_reportSeq = 0;

// The ring copy. 48 KiB in BSS rather than on a stack that may have just
// overflowed. Safe to be static because the fault path (acquireFaultPath)
// admits exactly one writer at a time.
char g_ringBuf[DiagLog::kRingLines * DiagLog::kLineBytes + 1] = {};

// Everything else the fault path needs is static for the same reason: a stack
// overflow leaves roughly one page of stack, and 62 frames plus two paths plus
// a module record is more than that page can safely hold. Static is safe here
// only because acquireFaultPath admits exactly one writer at a time.
char g_reportPath[kPathBytes] = {};
char g_dumpPath[kPathBytes] = {};

#if defined(_WIN32)
// ---------------------------------------------------------------------------
// ONE WRITER AT A TIME, and a second thread WAITS for it rather than killing it
// ---------------------------------------------------------------------------
//
// Field report F204602B5329B268 (0.99.35): the enumeration child probing
// driver=uhd died with 0xE0000002, this file's "entered twice" code. Until
// then every entry point shared one flag and treated ANY second entry as a
// fault inside the handler: TerminateProcess, at once. But UHD's discovery
// runs every device family's find function on a thread of its own, and the
// memory the known libusb fault corrupts is shared by all of them - so two
// threads dying together is the ordinary shape of that fault, not an
// exotic one. The second thread's TerminateProcess killed the FIRST thread
// in the middle of its report: measured, 8 of 8 staged runs left a truncated
// report and the exit code 0xE0000002, which names nothing
// (tests/test_crash_second_fault.cpp).
//
// So the entry records WHICH thread holds the fault path:
//
//   - the SAME thread entering again is a fault inside the handler. That is
//     still fatal - retrying would recurse - but it now says so on the way
//     out (handlerCannotRun) instead of vanishing.
//   - ANOTHER thread waits, up to kOtherThreadWaitMs, for the first report to
//     finish. With exitAfterReport the first handler then terminates the
//     process with the FIRST fault's code and a complete report; otherwise it
//     releases the path and this thread writes its own report after it.
//
// The absorbed-fault entry points take the same path, which they did not
// before: they write into the same static buffers, and nothing stopped an
// absorbed report and a fatal one from building their paths in the same
// g_reportPath at once.
volatile LONG g_faultPathThread = 0;  // the thread id holding it; 0 = free
volatile LONG g_cannotRunEntries = 0;

// Long enough for any report that is going to finish (they take
// milliseconds), and well inside the 20 s the enumeration parent waits for a
// child - so a handler that really is stuck still leaves the parent a line
// saying so instead of a timeout.
constexpr DWORD kOtherThreadWaitMs = 10000;

enum class FaultPathEntry { Acquired, SameThread, TimedOut };

FaultPathEntry acquireFaultPath(DWORD waitMs) {
    const LONG me = static_cast<LONG>(::GetCurrentThreadId());
    const ULONGLONG start = ::GetTickCount64();
    for (;;) {
        const LONG held = ::InterlockedCompareExchange(&g_faultPathThread, me, 0);
        if (held == 0) { return FaultPathEntry::Acquired; }
        if (held == me) { return FaultPathEntry::SameThread; }
        if (::GetTickCount64() - start >= waitMs) { return FaultPathEntry::TimedOut; }
        ::Sleep(5);
    }
}

void releaseFaultPath() { ::InterlockedExchange(&g_faultPathThread, 0); }

// The report file currently open, so a fault INSIDE the handler can still say
// so at the end of it. Written only by the thread holding the fault path.
volatile HANDLE g_openReport = INVALID_HANDLE_VALUE;

// Where faultLineToStdout writes; captured on the healthy path.
HANDLE g_faultLine = INVALID_HANDLE_VALUE;

// ---------------------------------------------------------------------------
// Opening the report WITHOUT the process heap
// ---------------------------------------------------------------------------
using NtCreateFileFn = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                                    PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
NtCreateFileFn g_ntCreateFile = nullptr;
constexpr std::size_t kNtPathChars = 1024;
// "\??\C:\Users\...\crashes\" - the crash directory as an NT path, built on
// the healthy path; the fault path only appends a file name to it.
wchar_t g_ntDir[kNtPathChars] = {};
std::size_t g_ntDirLen = 0;
wchar_t g_ntFile[kNtPathChars] = {};

// From ntifs.h, which a user-mode build does not include.
constexpr ULONG kFileOverwriteIf = 0x00000005;
constexpr ULONG kFileNonDirectoryFile = 0x00000040;
constexpr ULONG kFileSynchronousIoNonalert = 0x00000020;
constexpr ULONG kObjCaseInsensitive = 0x00000040;

// HEALTHY PATH ONLY. g_crashDir is interpreted exactly as CreateFileA would
// interpret it (the ANSI code page, which is UTF-8 when the manifest says so),
// made absolute, and given the NT prefix for its kind: a drive path, a UNC
// share, or one that already carries the \\?\ prefix. Anything this cannot
// render leaves g_ntDirLen 0, and the fault path falls back to CreateFileA -
// which is what every build before this one did.
void prepareNtCrashDir() {
    g_ntDirLen = 0;
    g_ntDir[0] = L'\0';
    if (g_ntCreateFile == nullptr) {
        HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll != nullptr) {
            g_ntCreateFile = reinterpret_cast<NtCreateFileFn>(
                reinterpret_cast<void*>(::GetProcAddress(ntdll, "NtCreateFile")));
        }
    }
    if (g_ntCreateFile == nullptr || g_crashDir[0] == '\0') { return; }
    wchar_t wide[kNtPathChars] = {};
    if (::MultiByteToWideChar(CP_ACP, 0, g_crashDir, -1, wide, static_cast<int>(kNtPathChars)) ==
        0) {
        return;
    }
    wchar_t full[kNtPathChars] = {};
    const DWORD n = ::GetFullPathNameW(wide, static_cast<DWORD>(kNtPathChars), full, nullptr);
    if (n == 0 || n >= kNtPathChars) { return; }
    const wchar_t* rest = full;
    const wchar_t* prefix = L"\\??\\";
    if (std::wcsncmp(full, L"\\\\?\\", 4) == 0 || std::wcsncmp(full, L"\\\\.\\", 4) == 0) {
        rest = full + 4;  // already a device path: "\\?\X:\..." or "\\?\UNC\..."
    } else if (std::wcsncmp(full, L"\\\\", 2) == 0) {
        prefix = L"\\??\\UNC\\";
        rest = full + 2;
    }
    std::size_t at = 0;
    for (std::size_t i = 0; prefix[i] != L'\0' && at + 1 < kNtPathChars; ++i) {
        g_ntDir[at++] = prefix[i];
    }
    for (std::size_t i = 0; rest[i] != L'\0' && at + 1 < kNtPathChars; ++i) {
        g_ntDir[at++] = rest[i];
    }
    if (at == 0 || at + 2 >= kNtPathChars) { return; }
    if (g_ntDir[at - 1] != L'\\') { g_ntDir[at++] = L'\\'; }
    g_ntDir[at] = L'\0';
    g_ntDirLen = at;
}

// Opens `win32Path` (built by buildReportPath, so its last component is plain
// ASCII) for writing. NtCreateFile on the prepared NT directory when there is
// one; CreateFileA otherwise.
HANDLE openForFaultPath(const char* win32Path) {
    if (g_ntCreateFile != nullptr && g_ntDirLen > 0) {
        const char* name = win32Path;
        for (const char* p = win32Path; *p != '\0'; ++p) {
            if (*p == '\\' || *p == '/') { name = p + 1; }
        }
        std::size_t at = 0;
        for (; at < g_ntDirLen; ++at) { g_ntFile[at] = g_ntDir[at]; }
        for (std::size_t i = 0; name[i] != '\0' && at + 1 < kNtPathChars; ++i) {
            g_ntFile[at++] = static_cast<wchar_t>(static_cast<unsigned char>(name[i]));
        }
        g_ntFile[at] = L'\0';
        UNICODE_STRING us;
        us.Buffer = g_ntFile;
        us.Length = static_cast<USHORT>(at * sizeof(wchar_t));
        us.MaximumLength = static_cast<USHORT>((at + 1) * sizeof(wchar_t));
        OBJECT_ATTRIBUTES oa;
        std::memset(&oa, 0, sizeof(oa));
        oa.Length = sizeof(oa);
        oa.ObjectName = &us;
        oa.Attributes = kObjCaseInsensitive;
        IO_STATUS_BLOCK iosb;
        std::memset(&iosb, 0, sizeof(iosb));
        HANDLE h = nullptr;
        const LONG status = g_ntCreateFile(&h, FILE_GENERIC_WRITE, &oa, &iosb, nullptr,
                                           FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                                           kFileOverwriteIf,
                                           kFileNonDirectoryFile | kFileSynchronousIoNonalert,
                                           nullptr, 0);
        if (status >= 0 && h != nullptr) { return h; }
    }
    return ::CreateFileA(win32Path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
}

unsigned long long g_frames[kMaxFrames] = {};
CONTEXT g_walkContext;  // ditto: 1.2 KiB, and unwinding mutates it
// The base of the main executable image, read at install time, so the fault
// path can ask "does any frame of the faulting stack lie in our own code"
// with a table walk and no loader call.
std::uintptr_t g_selfBase = 0;
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);
// Resolved at INSTALL time, never from the fault path: LoadLibrary takes the
// loader lock, which is the one lock a crash handler must never wait on.
MiniDumpWriteDumpFn g_miniDumpWriteDump = nullptr;

LPTOP_LEVEL_EXCEPTION_FILTER g_prevSehFilter = nullptr;
std::terminate_handler g_prevTerminate = nullptr;

// ---------------------------------------------------------------------------
// The allocation-free writer
// ---------------------------------------------------------------------------
struct Emit {
    HANDLE h = INVALID_HANDLE_VALUE;

    void raw(const char* s, std::size_t n) const {
        if (h == INVALID_HANDLE_VALUE || n == 0) { return; }
        DWORD written = 0;
        ::WriteFile(h, s, static_cast<DWORD>(n), &written, nullptr);
    }
    void str(const char* s) const {
        if (s == nullptr) { return; }
        std::size_t n = 0;
        while (s[n] != '\0') { ++n; }
        raw(s, n);
    }
    void hex(unsigned long long v, int digits) const {
        char buf[24];
        static const char kHex[] = "0123456789ABCDEF";
        if (digits <= 0) {
            digits = 1;
            for (unsigned long long t = v >> 4; t != 0; t >>= 4) { ++digits; }
        }
        if (digits > 16) { digits = 16; }
        for (int i = 0; i < digits; ++i) {
            buf[i] = kHex[(v >> ((digits - 1 - i) * 4)) & 0xFull];
        }
        raw(buf, static_cast<std::size_t>(digits));
    }
    void dec(unsigned long long v) const {
        char buf[24];
        int n = 0;
        if (v == 0) {
            buf[n++] = '0';
        } else {
            char tmp[24];
            int t = 0;
            while (v != 0 && t < 24) {
                tmp[t++] = static_cast<char>('0' + (v % 10));
                v /= 10;
            }
            while (t > 0) { buf[n++] = tmp[--t]; }
        }
        raw(buf, static_cast<std::size_t>(n));
    }
    // "<module>+0x<offset>", or the bare address when the address belongs to
    // no module the snapshot knows about. The raw address is preserved rather
    // than dropped - a known limit, not a hang. See diag_report.hpp.
    void addr(std::uintptr_t a) const {
        DiagModule m;
        std::uintptr_t off = 0;
        if (resolveAddress(a, m, off)) {
            str(m.name);
            str("+0x");
            hex(static_cast<unsigned long long>(off), 0);
        } else {
            str("0x");
            hex(static_cast<unsigned long long>(a), 16);
        }
    }
};

// Two-digit unpadded append, used only for the file name.
void appendPad2(char* dst, std::size_t& at, std::size_t cap, unsigned v) {
    if (at + 2 >= cap) { return; }
    dst[at++] = static_cast<char>('0' + ((v / 10) % 10));
    dst[at++] = static_cast<char>('0' + (v % 10));
}

// Every decimal digit of v, used only for the file name.
void appendDecimal(char* dst, std::size_t& at, std::size_t cap, unsigned long v) {
    char digits[16];
    int n = 0;
    if (v == 0) {
        digits[n++] = '0';
    } else {
        while (v != 0 && n < 16) {
            digits[n++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        }
    }
    while (n > 0 && at + 1 < cap) { dst[at++] = digits[--n]; }
}

// "<dir>\crash-YYYYMMDD-HHMMSS-<pid>-<seq>.txt", built by hand. Unique per
// process AND per report, so a second fault cannot silently overwrite the
// first one's evidence - which is why <seq> is written in FULL. The name only
// resolves to the second and the file is opened CREATE_ALWAYS, so the sequence
// number is all that separates two reports written in the same second; this
// used to print only its last digit, and the 11th report of a second then
// truncated the 1st (tests/test_crash_absorbed_child.cpp, the burst case).
void buildReportPath(char* out, std::size_t cap, const char* prefix, long seq) {
    std::size_t at = 0;
    for (std::size_t i = 0; g_crashDir[i] != '\0' && at + 1 < cap; ++i) { out[at++] = g_crashDir[i]; }
    if (at + 1 < cap) { out[at++] = '\\'; }
    for (std::size_t i = 0; prefix[i] != '\0' && at + 1 < cap; ++i) { out[at++] = prefix[i]; }

    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    if (at + 4 < cap) {
        appendPad2(out, at, cap, st.wYear / 100u);
        appendPad2(out, at, cap, st.wYear % 100u);
    }
    appendPad2(out, at, cap, st.wMonth);
    appendPad2(out, at, cap, st.wDay);
    if (at + 1 < cap) { out[at++] = '-'; }
    appendPad2(out, at, cap, st.wHour);
    appendPad2(out, at, cap, st.wMinute);
    appendPad2(out, at, cap, st.wSecond);
    if (at + 1 < cap) { out[at++] = '-'; }

    appendDecimal(out, at, cap, ::GetCurrentProcessId());
    if (at + 1 < cap) { out[at++] = '-'; }
    appendDecimal(out, at, cap, static_cast<unsigned long>(seq));
    const char* ext = ".txt";
    for (int i = 0; ext[i] != '\0' && at + 1 < cap; ++i) { out[at++] = ext[i]; }
    out[at] = '\0';
}

// x64 unwind. POD-only and wrapped in __except so a bad frame pointer ends the
// walk instead of the process - MSVC forbids SEH in a function holding objects
// that need unwinding, which is why this is its own function.
#if defined(_M_X64)
int walkFromContext(CONTEXT* ctx, ULONG64* frames, int maxFrames) {
    int n = 0;
    __try {
        while (n < maxFrames && ctx->Rip != 0) {
            frames[n++] = ctx->Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION rf = ::RtlLookupFunctionEntry(ctx->Rip, &imageBase, nullptr);
            if (rf == nullptr) {
                // A leaf function has no unwind data: its return address is at
                // the top of the stack. One hop only - guessing further would
                // manufacture frames.
                if (ctx->Rsp == 0) { break; }
                ctx->Rip = *reinterpret_cast<DWORD64*>(ctx->Rsp);
                ctx->Rsp += 8;
            } else {
                PVOID handlerData = nullptr;
                DWORD64 establisher = 0;
                ::RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx->Rip, rf, ctx,
                                   &handlerData, &establisher, nullptr);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Whatever was collected before the bad frame is still worth having.
    }
    return n;
}
#endif

void writeContextBlock(const Emit& e) {
    // A pointer into storage rendered on the healthy path. Nothing is
    // formatted here and nothing is allocated - diagContextBlock() would have
    // meant a std::string, and a std::string means the heap.
    e.str("--- context ---\n");
    int len = 0;
    const char* block = diagContextRaw(len);
    e.raw(block, static_cast<std::size_t>(len));
}

void writeModules(const Emit& e) {
    e.str("--- modules ---\n");
    const int n = moduleCount();
    for (int i = 0; i < n; ++i) {
        DiagModule m;
        if (!moduleAt(i, m)) { continue; }
        e.str("  ");
        e.str(m.name);
        e.str(" base=0x");
        e.hex(static_cast<unsigned long long>(m.base), 16);
        e.str(" size=0x");
        e.hex(static_cast<unsigned long long>(m.size), 0);
        e.str(" pdb=");
        e.str(m.pdb[0] != '\0' ? m.pdb : "(none)");
        e.str(" build=");
        e.str(m.buildId[0] != '\0' ? m.buildId : "(none)");
        e.str("\n");
    }
}

void writeRing(const Emit& e) {
    const std::uint64_t total = DiagLog::instance().linesWritten();
    const std::uint64_t kept =
        (total < static_cast<std::uint64_t>(DiagLog::kRingLines))
            ? total
            : static_cast<std::uint64_t>(DiagLog::kRingLines);
    e.str("--- log (last ");
    e.dec(kept);
    e.str(" of ");
    e.dec(total);
    e.str(" lines) ---\n");
    const std::size_t used = DiagLog::instance().copyRingRaw(g_ringBuf, sizeof(g_ringBuf));
    e.raw(g_ringBuf, used);
}

// The one thing the process still does with a CRT stream, and only because it
// predates this file: a single line on stderr naming the fault. Kept because a
// terminal user seeing "cascade: fatal exception ... in SoapyUHD.dll" has
// already been the whole diagnosis once (2026-08-15, libusb inside SoapyUHD).
void stderrAttribution(unsigned long code, std::uintptr_t addr) {
    char buf[256];
    std::size_t at = 0;
    const char* head = "cascade: fatal exception 0x";
    for (std::size_t i = 0; head[i] != '\0'; ++i) { buf[at++] = head[i]; }
    static const char kHex[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; --i) { buf[at++] = kHex[(code >> (i * 4)) & 0xFul]; }
    buf[at++] = ' ';
    buf[at++] = 'a';
    buf[at++] = 't';
    buf[at++] = ' ';
    DiagModule m;
    std::uintptr_t off = 0;
    if (resolveAddress(addr, m, off)) {
        for (std::size_t i = 0; m.name[i] != '\0' && at < 200; ++i) { buf[at++] = m.name[i]; }
        buf[at++] = '+';
        buf[at++] = '0';
        buf[at++] = 'x';
        for (int i = 15; i >= 0; --i) {
            buf[at++] = kHex[(static_cast<unsigned long long>(off) >> (i * 4)) & 0xFull];
        }
    } else {
        buf[at++] = '0';
        buf[at++] = 'x';
        for (int i = 15; i >= 0; --i) {
            buf[at++] = kHex[(static_cast<unsigned long long>(addr) >> (i * 4)) & 0xFull];
        }
    }
    buf[at++] = '\n';
    DWORD written = 0;
    // While the stderr capture is on, STD_ERROR_HANDLE is the pipe that feeds
    // the log - and this line, written there, would be read back and logged
    // as "vendor: cascade: fatal exception" by a thread racing the report's
    // own ring copy. It goes to the stderr the process had before the capture
    // instead, which is where a terminal user would have seen it anyway.
    HANDLE h = static_cast<HANDLE>(originalStderrHandle());
    if (h == nullptr) { h = ::GetStdHandle(STD_ERROR_HANDLE); }
    ::WriteFile(h, buf, static_cast<DWORD>(at), &written, nullptr);
}

// HOW MUCH STACK MUST BE LEFT BEFORE A WALK IS ATTEMPTED.
//
// RtlCaptureStackBackTrace itself needs very little, but the report that
// motivated this measured ZERO: the faulting instruction was the CALL, which
// faults while pushing its return address. 64 KiB is the size of the default
// stack reserve's first few commits and is far more than any of the work below
// this point needs, so a thread with less than this left is one that has
// already been driven to the edge by whatever ran before the fault - exactly
// the PPL worker in the two reports. Cheap to be generous: the cost of
// refusing is one stack section, and the cost of being wrong is the process.
constexpr std::uintptr_t kStackWalkMarginBytes = 64u * 1024u;

using GetThreadStackLimitsFn = void(WINAPI*)(PULONG_PTR, PULONG_PTR);
GetThreadStackLimitsFn g_getThreadStackLimits = nullptr;

// Resolved on the HEALTHY path (installCrashHandlers). GetProcAddress from a
// fault handler would be one more call into the loader than the handler's own
// contract allows.
void resolveStackLimitsApi() {
    if (g_getThreadStackLimits != nullptr) { return; }
    HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
    if (k32 == nullptr) { return; }
    g_getThreadStackLimits = reinterpret_cast<GetThreadStackLimitsFn>(
        reinterpret_cast<void*>(::GetProcAddress(k32, "GetCurrentThreadStackLimits")));
}

// True when this thread has room to spare. UNRESOLVABLE ANSWERS ARE "YES", not
// "no": the API is present on every version this product supports, and a build
// or platform where it is not must not lose the stack of every terminate,
// purecall and abort report - those paths have always walked here and have
// never been the ones that ran out.
bool stackHeadroomIsSafe() {
    if (g_getThreadStackLimits == nullptr) { return true; }
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    g_getThreadStackLimits(&low, &high);
    if (low == 0 || high <= low) { return true; }
    // _AddressOfReturnAddress is one slot above the current frame's return
    // address: near enough to the stack pointer for a 64 KiB margin, and it
    // needs no inline assembly or intrinsic that differs per architecture.
    const std::uintptr_t here = reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
    if (here <= static_cast<std::uintptr_t>(low)) { return false; }
    return (here - static_cast<std::uintptr_t>(low)) >= kStackWalkMarginBytes;
}

void writeMinidump(EXCEPTION_POINTERS* ep, const char* txtPath) {
    if (!g_minidump || g_miniDumpWriteDump == nullptr) { return; }
    // <report>.dmp beside the text report. LOCAL ONLY and never uploaded: a
    // minidump is process memory, which on this application can include file
    // paths and captured IQ. See PRIVACY.md.
    std::size_t at = 0;
    while (txtPath[at] != '\0' && at + 8 < kPathBytes) {
        g_dumpPath[at] = txtPath[at];
        ++at;
    }
    if (at >= 4) { at -= 4; }  // drop ".txt"
    const char* ext = ".dmp";
    for (int i = 0; i < 4; ++i) { g_dumpPath[at++] = ext[i]; }
    g_dumpPath[at] = '\0';

    HANDLE h = openForFaultPath(g_dumpPath);
    if (h == INVALID_HANDLE_VALUE) { return; }
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = ::GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    g_miniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), h,
                        MiniDumpWithDataSegs, (ep != nullptr) ? &mei : nullptr, nullptr,
                        nullptr);
    ::CloseHandle(h);
}

// THE STACK WALK IS BEST EFFORT, AND MUST NEVER BE ABLE TO KILL THE PROCESS
// IT IS DOCUMENTING. That is not a general principle applied for neatness; it
// is a fix for a crash observed in the field (B9D41A8D, 0.64.0).
//
// The faulting instruction was the CALL to RtlCaptureStackBackTrace itself,
// at crash_handler.cpp:402 - the branch reached only when there is no
// exception context, which is exactly how reportAbsorbedFault arrives when
// soapy_enum_proc reports an enumeration child dying. A call instruction
// faults while PUSHING ITS RETURN ADDRESS, so the stack it was about to walk
// had no room left: this path runs on a PPL worker thread owned by the
// std::async device scan, not on the main thread or a thread this code
// created, and it is the one caller that reaches here with a nearly spent
// stack.
//
// The report is deliberately written INCREMENTALLY, most valuable first - the
// header, the signature and the application context are already on disk by
// the time this runs. So the correct answer to a fault here is a stack
// section that says it could not be taken, not a dead process and no report
// at all. The irony of the crash reporter being the thing that crashed is
// worth one guard.
// AND THE GUARD WAS NOT ENOUGH, which is field report "crash cascade.exe @
// captureFramesGuarded" (0.96.3, Windows 10.0.22631).
//
// Same shape as B9D41A8D above and the same caller - an enumeration child died,
// reportAbsorbedFault ran on the std::async worker thread of the scanSoapy
// lambda with no exception context of its own, and the fallback below walked
// that worker's own nearly-spent stack. The __try did not save it, and could
// not: a STACK OVERFLOW leaves no room for the exception dispatcher either, so
// the second-chance fault kills the process at the same instruction the first
// one happened on. A guard that needs stack cannot guard a stack overflow.
//
// So the guard is now structural rather than only __try:
//
//   1. `mayWalkCurrentThread` false means the caller has said its own stack is
//      not the answer, and the current thread is never touched. An absorbed
//      CHILD-PROCESS fault is exactly that case: the child died, this process
//      did not, and the frames of whichever worker noticed describe the
//      noticing and not the fault. See reportAbsorbedChildFault.
//   2. THE HEADROOM IS MEASURED BEFORE THE CALL. GetCurrentThreadStackLimits
//      gives the committed low bound of this thread's stack; the distance from
//      it to the current frame is how much is left. Below kStackWalkMarginBytes
//      the walk is refused rather than attempted, because attempting it is what
//      crashed. A refusal costs a stack section that says it could not be
//      taken; attempting it costs the report AND the process.
//   3. A SUPPLIED CONTEXT IS THE ONLY STACK WALKED. A null or zeroed
//      ContextRecord used to fall through to "walk whatever thread is running
//      this handler", silently substituting one thread's story for another's -
//      which is how this fault reached the unguarded call in the first place.
//      A caller that supplies EXCEPTION_POINTERS is asking for THAT stack, and
//      gets no frames rather than somebody else's.
int captureFramesGuarded(EXCEPTION_POINTERS* ep, bool mayWalkCurrentThread) {
    __try {
        int n = 0;
        if (ep != nullptr) {
#if defined(_M_X64)
            // The FAULTING stack, not the handler's: unwound from the context
            // Windows captured at the moment of the fault. A context with no
            // Rip has no stack to unwind and says so with zero frames.
            if (ep->ContextRecord != nullptr && ep->ContextRecord->Rip != 0) {
                std::memcpy(&g_walkContext, ep->ContextRecord, sizeof(CONTEXT));
                n = walkFromContext(&g_walkContext, g_frames, kMaxFrames);
            }
#endif
            return n;
        }
        if (!mayWalkCurrentThread) { return 0; }
        // No exception context (terminate, purecall, invalid parameter, abort):
        // the handler runs on the offending thread, so its own stack IS the
        // answer - provided there is enough of it left to ask the question.
        if (!stackHeadroomIsSafe()) { return 0; }
        n = static_cast<int>(
            ::RtlCaptureStackBackTrace(0, static_cast<ULONG>(kMaxFrames),
                                       reinterpret_cast<PVOID*>(g_frames), nullptr));
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Still here, and still worth having: it catches the bad frame pointer
        // and the unreadable context, which are recoverable. It is the stack
        // overflow it cannot catch, and the headroom check above is what stands
        // in for it.
        return 0;
    }
}

// WHAT A CHILD PROCESS'S ABSORBED FAULT ADDS TO A REPORT, and what it takes
// away. Present (non-null) only on the reportAbsorbedChildFault path: the fault
// happened in ANOTHER PROCESS, so this one has no frames to offer and no
// minidump worth writing, and the two facts that do identify it - what the
// child died of and which attempt it was - go in the process block.
struct ChildFault {
    unsigned long exitCode = 0;
    int attempt = 0;
    // Hashed into the signature in place of the (absent) faulting module, so
    // one child death does not group with every other of its exit code. Null
    // keeps the old "?". See reportAbsorbedChildFault.
    const char* signatureTag = nullptr;
};

// The whole report, written incrementally so a deadlock in the unwinder still
// leaves the identifying half on disk. See the file header.
void writeReport(const char* reason, unsigned long code, std::uintptr_t faultAddr,
                 EXCEPTION_POINTERS* ep, const ChildFault* child = nullptr) {
    if (!g_enabled || g_crashDir[0] == '\0') { return; }

    const long seq = ::InterlockedIncrement(&g_reportSeq);
    buildReportPath(g_reportPath, kPathBytes, "crash-", seq);

    Emit e;
    e.h = openForFaultPath(g_reportPath);
    if (e.h == INVALID_HANDLE_VALUE) { return; }
    g_openReport = e.h;

    DiagModule fm;
    std::uintptr_t foff = 0;
    const bool resolved = resolveAddress(faultAddr, fm, foff);
    char sig[17];
    const char* sigModule = resolved ? fm.name : "?";
    if (child != nullptr && child->signatureTag != nullptr && child->signatureTag[0] != '\0') {
        sigModule = child->signatureTag;
    }
    crashSignatureRaw(code, sigModule, foff, sig);

    // THE HEADER. Every "name: value" line below is inventoried in
    // crashReportFieldNames() and documented in PRIVACY.md, and
    // tests/test_crash_capture.cpp compares the two as SETS, both ways,
    // against a report from a real fault. A line added here without being
    // added there fails that test rather than shipping undocumented.
    e.str("kind: crash\n");
    e.str("reason: ");
    e.str(reason);
    e.str("\ncode: 0x");
    e.hex(code, 8);
    e.str("\naddress: ");
    e.addr(faultAddr);
    e.str("\nsignature: ");
    e.str(sig);
    e.str("\nthread: ");
    e.dec(::GetCurrentThreadId());
    e.str("\n");
    writeContextBlock(e);

    e.str("--- stack (thread ");
    e.dec(::GetCurrentThreadId());
    e.str(") ---\n");
    // A CHILD-PROCESS FAULT NEVER WALKS. The frames of whichever of this
    // process's threads noticed the death describe the noticing, not the fault
    // - and walking them is what crashed the parent in the 0.96.3 report. The
    // reason string, the child's exit code and the attempt number are the whole
    // of what this process knows, and they are worth more than a stack of the
    // observer.
    const int nFrames = (child != nullptr) ? 0 : captureFramesGuarded(ep, true);
    if (nFrames == 0) {
        // SAID, not left as an empty section. A stack section with no frames
        // and no explanation reads as "this fault had no stack", which is
        // never true; it means the walk could not be taken, and a reader needs
        // to know which of the two they are looking at.
        if (child != nullptr) {
            e.str("  (no frames: the fault was in a child process, so this "
                  "process has no stack to show)\n");
        } else {
            e.str("  (no frames: the stack could not be walked)\n");
        }
    }
    for (int i = 0; i < nFrames; ++i) {
        e.str("  ");
        e.addr(static_cast<std::uintptr_t>(g_frames[i]));
        e.str("\n");
    }

    // THE PROCESS BLOCK: two facts a reader asked for and the report could
    // not answer. "uptime-sec" is how long the session had run - a fault 45 s
    // after a rate change and one three hours in are different bugs at the
    // same address. "fault-thread-own" is whether the faulting thread was one
    // of ours: any frame of the walked stack inside the main executable
    // means our code is somewhere beneath the fault; none means a thread a
    // vendor driver created and ran entirely in its own code, which is where
    // the 0.88.0 RTL-SDR report could not be placed. Written AFTER the stack
    // because it is derived from it, and the header must reach disk before
    // the walk (see the file header); it is parsed by crash_upload.cpp and
    // sent as `uptimeSec` and `faultThreadOwn`.
    e.str("--- process ---\nuptime-sec: ");
    e.dec(processUptimeSec());
    e.str("\nfault-thread-own: ");
    if (nFrames == 0) {
        // A walk that could not be taken answers neither yes nor no.
        e.str("unknown");
    } else {
        bool own = false;
        for (int i = 0; i < nFrames && !own; ++i) {
            DiagModule m;
            std::uintptr_t off = 0;
            if (resolveAddress(static_cast<std::uintptr_t>(g_frames[i]), m, off) &&
                m.base == g_selfBase) {
                own = true;
            }
        }
        e.str(own ? "yes" : "no");
    }
    e.str("\n");
    // THE TWO FACTS A CHILD DEATH ACTUALLY CARRIES. In the process block rather
    // than the header because the header's field set is inventoried in
    // crashReportFieldNames() and compared BOTH WAYS against a report from a
    // real fault (tests/test_crash_capture.cpp) - a line that appears on one
    // path and not another would fail that comparison for every report that is
    // not a child death.
    if (child != nullptr) {
        e.str("child-exit-code: 0x");
        e.hex(child->exitCode, 8);
        e.str("\nchild-attempt: ");
        e.dec(static_cast<unsigned long>(child->attempt));
        e.str("\n");
    }

    writeModules(e);
    writeRing(e);
    g_openReport = INVALID_HANDLE_VALUE;
    ::CloseHandle(e.h);

    std::memcpy(g_lastPath, g_reportPath, kPathBytes);
    // NO MINIDUMP FOR A CHILD FAULT: a dump of this process's memory describes
    // the process that SURVIVED, and dumps are the most revealing artefact this
    // product can write (PRIVACY.md). One that cannot document the fault is not
    // worth the pages it would put on the user's disk.
    if (child == nullptr) { writeMinidump(ep, g_reportPath); }
}

void finish(unsigned long exitCode) {
    if (!g_exitAfterReport) { return; }
    // TerminateProcess, not exit(): a CRT exit from a broken process runs
    // atexit handlers and static destructors over state that has already
    // failed once.
    ::TerminateProcess(::GetCurrentProcess(), exitCode);
}

// THE LINE FOR THE PARENT (CrashHandlerConfig::faultLineToStdout), written
// FIRST, before the report: whatever becomes of the report, the process that
// reads this one's stdout learns what it died of.
void faultLine(const char* what, unsigned long code, std::uintptr_t addr) {
    if (g_faultLine == INVALID_HANDLE_VALUE) { return; }
    Emit e;
    e.h = g_faultLine;
    e.str(kFaultLinePrefix);
    e.str(what);
    e.str(" 0x");
    e.hex(code, 8);
    e.str(" at ");
    e.addr(addr);
    e.str("\n");
}

// THE HANDLER CANNOT RUN, and says so on the way out rather than vanishing.
// `sameThread` is a fault INSIDE the handler (this thread already holds the
// fault path); otherwise another thread's report did not finish within
// kOtherThreadWaitMs. Either way the process dies with 0xE0000002, as it
// always has - but the parent's line and the end of the half-written report
// now carry what this thread was about to report.
//
// Entered at most once: anything here that faults again comes straight back
// in, and the second arrival just terminates.
[[noreturn]] void handlerCannotRun(bool sameThread, const char* what, unsigned long code,
                                   std::uintptr_t addr) {
    if (::InterlockedIncrement(&g_cannotRunEntries) == 1) {
        const char* why = sameThread ? "second fault inside the crash handler"
                                     : "crash handler did not finish on another thread";
        if (g_faultLine != INVALID_HANDLE_VALUE) {
            Emit e;
            e.h = g_faultLine;
            e.str(kFaultLinePrefix);
            e.str(why);
            e.str(": ");
            e.str(what);
            e.str(" 0x");
            e.hex(code, 8);
            e.str("\n");
        }
        const HANDLE open = g_openReport;
        if (open != INVALID_HANDLE_VALUE) {
            Emit e;
            e.h = open;
            e.str("\n--- ");
            e.str(why);
            e.str(" ---\n");
            e.str(what);
            e.str(" 0x");
            e.hex(code, 8);
            e.str(" at ");
            e.addr(addr);
            e.str("\n");
        }
    }
    ::TerminateProcess(::GetCurrentProcess(), 0xE0000002ul);
    for (;;) { ::Sleep(INFINITE); }
}

// Every fatal entry point starts here: take the fault path, or report why it
// cannot be taken. Returns only when this thread holds it.
void enterFatal(const char* what, unsigned long code, std::uintptr_t addr) {
    const FaultPathEntry entry = acquireFaultPath(kOtherThreadWaitMs);
    if (entry == FaultPathEntry::Acquired) { return; }
    handlerCannotRun(entry == FaultPathEntry::SameThread, what, code, addr);
}

LONG WINAPI sehFilter(EXCEPTION_POINTERS* ep) {
    const unsigned long code =
        (ep != nullptr && ep->ExceptionRecord != nullptr)
            ? static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode)
            : 0ul;
    const auto addr =
        (ep != nullptr && ep->ExceptionRecord != nullptr)
            ? reinterpret_cast<std::uintptr_t>(ep->ExceptionRecord->ExceptionAddress)
            : 0u;
    const char* reason = "structured exception";
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: reason = "access violation"; break;
        case EXCEPTION_STACK_OVERFLOW: reason = "stack overflow"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO: reason = "integer divide by zero"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION: reason = "illegal instruction"; break;
        case 0xE06D7363ul: reason = "unhandled c++ exception"; break;
        default: break;
    }
    enterFatal(reason, code, addr);
    faultLine(reason, code, addr);
    stderrAttribution(code, addr);
    writeReport(reason, code, addr, ep);
    finish(code != 0 ? code : 0xE0000001ul);
    // Production default: let the original exception continue to Windows, so
    // WER still behaves exactly as it always has on a user's machine. Any
    // filter that was already installed still gets its turn.
    releaseFaultPath();
    if (g_prevSehFilter != nullptr) { return g_prevSehFilter(ep); }
    return EXCEPTION_CONTINUE_SEARCH;
}

void onTerminate() {
    // The faulting address is this handler's own return site, which resolves
    // to the module whose exception escaped - which is the useful half.
    void* here = _ReturnAddress();
    enterFatal("std::terminate", 0xE0000003ul, reinterpret_cast<std::uintptr_t>(here));
    faultLine("std::terminate", 0xE0000003ul, reinterpret_cast<std::uintptr_t>(here));
    writeReport("std::terminate", 0xE0000003ul, reinterpret_cast<std::uintptr_t>(here), nullptr);
    finish(0xE0000003ul);
    // The report is already written, so the SIGABRT net must not write a
    // second one for the same fault: stand it down and let the process die
    // exactly the way it always did.
    std::signal(SIGABRT, SIG_DFL);
    releaseFaultPath();
    if (g_prevTerminate != nullptr && g_prevTerminate != &onTerminate) { g_prevTerminate(); }
    ::abort();
}

// THE NET UNDER std::set_terminate, and it is not decoration.
//
// MSVC's set_terminate installs a PER-THREAD handler ("each thread is in
// charge of its own termination handling"), so the one installed on the main
// thread does not apply to a thread this application never created - which is
// exactly the case the terminate registration exists for: a vendor SDR driver
// throwing out of its own stream-read thread. Measured, not assumed: with only
// set_terminate installed, an exception escaping a std::thread killed the
// process with 0xC0000409 (__fastfail from the default terminate's abort) and
// left no report at all.
//
// abort() is where every one of those paths converges, on whatever thread they
// happen on, and a SIGABRT handler IS process-wide - the UCRT's abort() raises
// SIGABRT before it fast-fails. So this catches the default terminate on any
// thread, a failed assert, and a direct abort().
void __cdecl onAbortSignal(int) {
    void* here = _ReturnAddress();
    const char* reason = "abort (std::terminate on a thread with no handler, or a direct abort)";
    enterFatal("abort", 0xE0000006ul, reinterpret_cast<std::uintptr_t>(here));
    faultLine("abort", 0xE0000006ul, reinterpret_cast<std::uintptr_t>(here));
    writeReport(reason, 0xE0000006ul, reinterpret_cast<std::uintptr_t>(here), nullptr);
    finish(0xE0000006ul);
    ::_exit(3);
}

void __cdecl onPureCall() {
    void* here = _ReturnAddress();
    enterFatal("purecall", 0xE0000004ul, reinterpret_cast<std::uintptr_t>(here));
    faultLine("purecall", 0xE0000004ul, reinterpret_cast<std::uintptr_t>(here));
    writeReport("purecall", 0xE0000004ul, reinterpret_cast<std::uintptr_t>(here), nullptr);
    finish(0xE0000004ul);
    std::signal(SIGABRT, SIG_DFL);  // see onTerminate: one fault, one report
    releaseFaultPath();
    ::abort();
}

void __cdecl onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int,
                                uintptr_t) {
    // The wide-character arguments are DELIBERATELY dropped: they are only
    // populated in a debug CRT, and formatting them would mean a CRT call on
    // the fault path for a string that is empty in every shipped build.
    void* here = _ReturnAddress();
    enterFatal("invalid parameter", 0xE0000005ul, reinterpret_cast<std::uintptr_t>(here));
    faultLine("invalid parameter", 0xE0000005ul, reinterpret_cast<std::uintptr_t>(here));
    writeReport("invalid parameter", 0xE0000005ul, reinterpret_cast<std::uintptr_t>(here),
                nullptr);
    finish(0xE0000005ul);
    std::signal(SIGABRT, SIG_DFL);  // see onTerminate: one fault, one report
    releaseFaultPath();
    ::abort();
}
#endif  // _WIN32

}  // namespace

void installCrashHandlers(const CrashHandlerConfig& cfg) {
#if defined(_WIN32)
    g_enabled = cfg.enabled;
    g_minidump = cfg.minidump;
    g_exitAfterReport = cfg.exitAfterReport;

    // REMEMBERED EVEN WHEN DISABLED, and NOT created: a session that starts
    // with diagnostics switched off must leave no directory behind, but the
    // Settings toggle has to have somewhere to put a report if the user turns
    // it on mid-session. The path is a string; the directory is what "off
    // means off" is about.
    g_crashDir[0] = '\0';
    if (!cfg.crashDir.empty()) {
        std::size_t n = cfg.crashDir.size();
        if (n > kPathBytes - 1) { n = kPathBytes - 1; }
        std::memcpy(g_crashDir, cfg.crashDir.data(), n);
        g_crashDir[n] = '\0';
    }
    if (cfg.enabled && g_crashDir[0] != '\0') {
        // CREATED HERE, on the healthy path, so no fault path ever has to.
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(cfg.crashDir), ec);
        if (ec && !std::filesystem::is_directory(std::filesystem::path(cfg.crashDir))) {
            // Nowhere to write means capture is OFF, not attempted-and-failed
            // from inside a broken process.
            g_enabled = false;
            g_crashDir[0] = '\0';
        }
    } else {
        g_enabled = false;
    }
    // The NT path the fault path opens the report on, without the heap - see
    // prepareNtCrashDir. Built whether or not capture is on, because the
    // Settings toggle can turn it on mid-session (setCrashCaptureEnabled
    // builds it again then).
    prepareNtCrashDir();
    // The parent's line (faultLineToStdout). The handle as it is NOW: the
    // enumeration child's stdout is the pipe its parent reads.
    g_faultLine = INVALID_HANDLE_VALUE;
    if (cfg.faultLineToStdout) {
        const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (out != nullptr) { g_faultLine = out; }
    }

    // The stack-headroom API the frame capture consults, resolved HERE because
    // GetProcAddress from a fault handler is a call into the loader the
    // handler's own contract forbids. See stackHeadroomIsSafe().
    resolveStackLimitsApi();

    // The module snapshot the fault path searches. Refreshed again by the
    // application after anything that loads code; this is just the floor.
    if (moduleCount() == 0) { refreshModuleTable(); }
    // Our own image, for the process block's fault-thread-own line. A module
    // handle IS its base address; read here on the healthy path.
    g_selfBase = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));

    if (g_minidump && g_miniDumpWriteDump == nullptr) {
        // Resolved NOW. LoadLibrary from a fault handler takes the loader lock.
        HMODULE dbghelp = ::LoadLibraryA("dbghelp.dll");
        if (dbghelp != nullptr) {
            g_miniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFn>(
                reinterpret_cast<void*>(::GetProcAddress(dbghelp, "MiniDumpWriteDump")));
        }
    }

    // Four separate registrations, because they are four separate failure
    // paths and any one of them can be missing without the others noticing.
    const LPTOP_LEVEL_EXCEPTION_FILTER prev = ::SetUnhandledExceptionFilter(&sehFilter);
    if (prev != &sehFilter) { g_prevSehFilter = prev; }
    const std::terminate_handler prevT = std::set_terminate(&onTerminate);
    if (prevT != &onTerminate) { g_prevTerminate = prevT; }
    ::_set_invalid_parameter_handler(&onInvalidParameter);
    ::_set_purecall_handler(&onPureCall);
    // The net under the per-thread terminate handler. See onAbortSignal.
    std::signal(SIGABRT, &onAbortSignal);
#elif defined(__linux__)
    posix_detail::install(cfg);
#else
    (void)cfg;
#endif
}

void setCrashCaptureEnabled(bool enabled, bool minidump) {
#if defined(_WIN32)
    g_enabled = enabled && g_crashDir[0] != '\0';
    // Switched ON mid-session: the directory was deliberately not created at
    // install time, so create it now, on this healthy path, rather than
    // discovering from inside a broken process that there is nowhere to write.
    if (g_enabled) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(g_crashDir), ec);
        if (ec && !std::filesystem::is_directory(std::filesystem::path(g_crashDir))) {
            g_enabled = false;
        }
    }
    prepareNtCrashDir();
    g_minidump = minidump;
    if (g_minidump && g_miniDumpWriteDump == nullptr) {
        HMODULE dbghelp = ::LoadLibraryA("dbghelp.dll");
        if (dbghelp != nullptr) {
            g_miniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFn>(
                reinterpret_cast<void*>(::GetProcAddress(dbghelp, "MiniDumpWriteDump")));
        }
    }
#elif defined(__linux__)
    posix_detail::setEnabled(enabled, minidump);
#else
    (void)enabled;
    (void)minidump;
#endif
}

std::string lastCrashReportPath() {
#if defined(_WIN32)
    return std::string(g_lastPath);
#elif defined(__linux__)
    return posix_detail::lastReportPath();
#else
    return std::string();
#endif
}

std::string activeCrashDir() {
#if defined(_WIN32)
    // BOTH questions in one answer, on purpose - see the header. An armed
    // directory is a directory AND the switch being on; either half alone is
    // not consent to write there.
    if (!g_enabled) { return std::string(); }
    return std::string(g_crashDir);
#elif defined(__linux__)
    return posix_detail::activeDir();
#else
    return std::string();
#endif
}

void reportAbsorbedFault(const char* reason, unsigned long code, const void* faultAddress,
                         void* exceptionPointers) {
#if defined(_WIN32)
    // THE SAME FAULT PATH AS THE FATAL HANDLERS (F204602B5329B268): it writes
    // into the same static buffers, so it must hold the same writer. A fatal
    // fault on ANOTHER thread while this report is written waits for it and
    // then reports its own; one on THIS thread is a fault inside the report
    // writer and is named as that. Before, this path had a latch of its own,
    // so an absorbed report and a fatal one could build their paths in the
    // same g_reportPath at once. A report already being written on this
    // thread, or one that never finished on another, means this one is not
    // written - the old latch's answer, kept.
    if (acquireFaultPath(kOtherThreadWaitMs) != FaultPathEntry::Acquired) { return; }
    writeReport(reason != nullptr ? reason : "absorbed fault", code,
                reinterpret_cast<std::uintptr_t>(faultAddress),
                static_cast<EXCEPTION_POINTERS*>(exceptionPointers));
    releaseFaultPath();
#elif defined(__linux__)
    // No POSIX equivalent of EXCEPTION_POINTERS: every absorbed-fault caller
    // on this platform reports the calling thread's own stack, exactly as
    // reportAbsorbedFault(..., exceptionPointers=nullptr) already does on
    // Windows.
    (void)exceptionPointers;
    posix_detail::reportAbsorbed(reason, code, faultAddress);
#else
    (void)reason;
    (void)code;
    (void)faultAddress;
    (void)exceptionPointers;
#endif
}

void reportAbsorbedChildFault(const char* reason, unsigned long childExitCode, int attempt,
                              const char* signatureTag) {
#if defined(_WIN32)
    // The same writer as every other report - see reportAbsorbedFault.
    if (acquireFaultPath(kOtherThreadWaitMs) != FaultPathEntry::Acquired) { return; }
    ChildFault child;
    child.exitCode = childExitCode;
    child.attempt = attempt;
    child.signatureTag = signatureTag;
    // faultAddr 0 and ep nullptr, deliberately: the address that faulted is in
    // another process's address space and means nothing in this one. The
    // signature therefore groups on the reason and the code, which is what
    // distinguishes one child death from another.
    writeReport(reason != nullptr ? reason : "child process fault (contained)", childExitCode,
                0u, nullptr, &child);
    releaseFaultPath();
#elif defined(__linux__)
    posix_detail::reportAbsorbedChild(reason, childExitCode, attempt, signatureTag);
#else
    (void)reason;
    (void)childExitCode;
    (void)attempt;
    (void)signatureTag;
#endif
}

int captureFramesForTest(void* exceptionPointers, bool mayWalkCurrentThread) {
#if defined(_WIN32)
    return captureFramesGuarded(static_cast<EXCEPTION_POINTERS*>(exceptionPointers),
                                mayWalkCurrentThread);
#elif defined(__linux__)
    // exceptionPointers has no POSIX meaning (see reportAbsorbedFault above);
    // the only property this hook can exercise here is the
    // mayWalkCurrentThread half of the Windows contract.
    (void)exceptionPointers;
    return posix_detail::captureFramesForTest(mayWalkCurrentThread);
#else
    (void)exceptionPointers;
    (void)mayWalkCurrentThread;
    return 0;
#endif
}

void holdFaultPathForTest() {
#if defined(_WIN32)
    (void)acquireFaultPath(0);
#elif defined(__linux__)
    posix_detail::holdFaultPathForTest();
#endif
}

// ---------------------------------------------------------------------------
// The test hook. Four real faults, one per registration.
// ---------------------------------------------------------------------------
namespace {

struct PureBase;
void pokePureVirtual(PureBase* b);

struct PureBase {
    PureBase() { pokePureVirtual(this); }
    virtual ~PureBase() = default;
    virtual void nowhere() = 0;
};
struct PureDerived : PureBase {
    void nowhere() override {}
};

// Out of line and un-optimised so the compiler cannot devirtualise the call
// away: during PureBase's constructor the dynamic type IS PureBase, and
// PureBase::nowhere has no body, so this reaches _purecall.
//
// GCC NEEDS THIS TOO, not just MSVC - found the hard way porting this test to
// Linux. At -O3, GCC proves that calling a pure virtual during PureBase's own
// construction is guaranteed undefined behaviour and replaces the ENTIRE
// virtual dispatch with a direct call to abort() in a ".cold" clone -
// skipping the vtable indirection and __cxa_pure_virtual entirely. A debugger
// backtrace at the resulting abort() showed `raiseTestFault(...) [clone
// .cold]` calling `abort()` directly, with no frame for pokePureVirtual, the
// vtable, or __cxa_pure_virtual anywhere in it - proof the call itself was
// deleted, not merely inlined. That devirtualisation is exactly what this
// out-of-line function exists to prevent, and only the MSVC half of the
// guard was ever applied.
#if defined(_MSC_VER)
#pragma optimize("", off)
#elif defined(__clang__)
#pragma clang optimize off
#elif defined(__GNUC__)
__attribute__((optimize("O0")))
#endif
void pokePureVirtual(PureBase* b) { b->nowhere(); }
#if defined(_MSC_VER)
#pragma optimize("", on)
#elif defined(__clang__)
#pragma clang optimize on
#endif

}  // namespace

void raiseTestFault(TestFaultKind kind) {
    switch (kind) {
        case TestFaultKind::AccessViolation: {
            // volatile so the store is really emitted rather than folded into
            // a compile-time trap the handler would never see.
            volatile int* p = reinterpret_cast<volatile int*>(0);
            *p = 1;
            break;
        }
        case TestFaultKind::Terminate: {
            // An exception ESCAPING A THREAD - the path a vendor SDR driver
            // takes when it throws out of a stream read.
            std::thread t([] { throw std::runtime_error("deliberate test fault"); });
            t.join();
            break;
        }
        case TestFaultKind::PureCall: {
            PureDerived d;
            (void)d;
            break;
        }
        case TestFaultKind::InvalidParameter: {
#if defined(_WIN32)
            // _get_osfhandle with an out-of-range descriptor invokes the CRT's
            // invalid-parameter handler, which by default kills the process
            // without ever raising an exception the SEH filter could see.
            volatile int fd = 12345;
            (void)::_get_osfhandle(fd);
#elif defined(__linux__)
            // glibc has no CRT invalid-parameter fail-fast to invoke; see
            // crash_handler_posix.hpp for the closest honest equivalent this
            // exercises instead. [[noreturn]], so nothing here falls through.
            posix_detail::raiseInvalidParameterTestFault();
#endif
            break;
        }
    }
    std::abort();  // reached only if the fault above failed to happen
}

}  // namespace cascade::core
