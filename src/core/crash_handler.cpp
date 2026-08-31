// See crash_handler.hpp for which four entry points are covered and why, what
// is deliberately NOT covered, and the rules the fault path obeys.
//
// WHAT THE FAULT PATH TOUCHES, in full, so the claim in the header can be
// checked rather than believed:
//
//   CreateFileA / WriteFile / CloseHandle   kernel calls, no CRT buffering
//   GetLocalTime / GetCurrentProcessId      TEB and shared-page reads
//   RtlCaptureStackBackTrace or
//   RtlLookupFunctionEntry + RtlVirtualUnwind
//   memcpy, and integer rendering written by hand
//
// It allocates nothing, opens no CRT stream, takes none of this application's
// locks, and calls no snprintf (which can take a locale lock). The two large
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
#pragma comment(lib, "dbghelp.lib")
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
long g_inHandler = 0;   // InterlockedCompareExchange guard against re-entry
long g_reportSeq = 0;

// The ring copy. 48 KiB in BSS rather than on a stack that may have just
// overflowed. Safe to be static because g_inHandler admits exactly one
// handler at a time.
char g_ringBuf[DiagLog::kRingLines * DiagLog::kLineBytes + 1] = {};

// Everything else the fault path needs is static for the same reason: a stack
// overflow leaves roughly one page of stack, and 62 frames plus two paths plus
// a module record is more than that page can safely hold. Static is safe here
// only because g_inHandler admits exactly one handler at a time.
char g_reportPath[kPathBytes] = {};
char g_dumpPath[kPathBytes] = {};

#if defined(_WIN32)
unsigned long long g_frames[kMaxFrames] = {};
CONTEXT g_walkContext;  // ditto: 1.2 KiB, and unwinding mutates it
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

// "<dir>\crash-YYYYMMDD-HHMMSS-<pid>-<seq>.txt", built by hand. Unique per
// process AND per report, so a second fault cannot silently overwrite the
// first one's evidence.
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

    unsigned long pid = ::GetCurrentProcessId();
    char digits[16];
    int n = 0;
    if (pid == 0) {
        digits[n++] = '0';
    } else {
        while (pid != 0 && n < 16) {
            digits[n++] = static_cast<char>('0' + (pid % 10));
            pid /= 10;
        }
    }
    while (n > 0 && at + 1 < cap) { out[at++] = digits[--n]; }
    if (at + 1 < cap) { out[at++] = '-'; }
    if (at + 1 < cap) { out[at++] = static_cast<char>('0' + (seq % 10)); }
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
    ::WriteFile(::GetStdHandle(STD_ERROR_HANDLE), buf, static_cast<DWORD>(at), &written, nullptr);
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

    HANDLE h = ::CreateFileA(g_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
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
int captureFramesGuarded(EXCEPTION_POINTERS* ep) {
    __try {
        int n = 0;
#if defined(_M_X64)
        if (ep != nullptr && ep->ContextRecord != nullptr) {
            // The FAULTING stack, not the handler's: unwound from the context
            // Windows captured at the moment of the fault.
            std::memcpy(&g_walkContext, ep->ContextRecord, sizeof(CONTEXT));
            n = walkFromContext(&g_walkContext, g_frames, kMaxFrames);
        }
#else
        (void)ep;
#endif
        if (n == 0) {
            // No exception context (terminate, purecall, invalid parameter):
            // the handler runs on the offending thread, so its own stack IS
            // the answer.
            n = static_cast<int>(
                ::RtlCaptureStackBackTrace(0, static_cast<ULONG>(kMaxFrames),
                                           reinterpret_cast<PVOID*>(g_frames), nullptr));
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Includes EXCEPTION_STACK_OVERFLOW, which is why this is __except and
        // not catch(...): a stack overflow is a structured exception and no
        // C++ handler would ever see it.
        return 0;
    }
}

// The whole report, written incrementally so a deadlock in the unwinder still
// leaves the identifying half on disk. See the file header.
void writeReport(const char* reason, unsigned long code, std::uintptr_t faultAddr,
                 EXCEPTION_POINTERS* ep) {
    if (!g_enabled || g_crashDir[0] == '\0') { return; }

    const long seq = ::InterlockedIncrement(&g_reportSeq);
    buildReportPath(g_reportPath, kPathBytes, "crash-", seq);

    Emit e;
    e.h = ::CreateFileA(g_reportPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (e.h == INVALID_HANDLE_VALUE) { return; }

    DiagModule fm;
    std::uintptr_t foff = 0;
    const bool resolved = resolveAddress(faultAddr, fm, foff);
    char sig[17];
    crashSignatureRaw(code, resolved ? fm.name : "?", foff, sig);

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
    const int nFrames = captureFramesGuarded(ep);
    if (nFrames == 0) {
        // SAID, not left as an empty section. A stack section with no frames
        // and no explanation reads as "this fault had no stack", which is
        // never true; it means the walk could not be taken, and a reader needs
        // to know which of the two they are looking at.
        e.str("  (no frames: the stack could not be walked)\n");
    }
    for (int i = 0; i < nFrames; ++i) {
        e.str("  ");
        e.addr(static_cast<std::uintptr_t>(g_frames[i]));
        e.str("\n");
    }

    writeModules(e);
    writeRing(e);
    ::CloseHandle(e.h);

    std::memcpy(g_lastPath, g_reportPath, kPathBytes);
    writeMinidump(ep, g_reportPath);
}

void finish(unsigned long exitCode) {
    if (!g_exitAfterReport) { return; }
    // TerminateProcess, not exit(): a CRT exit from a broken process runs
    // atexit handlers and static destructors over state that has already
    // failed once.
    ::TerminateProcess(::GetCurrentProcess(), exitCode);
}

LONG WINAPI sehFilter(EXCEPTION_POINTERS* ep) {
    if (::InterlockedCompareExchange(&g_inHandler, 1, 0) != 0) {
        // A fault INSIDE the handler. Do not try again; die quietly rather
        // than recursing until the stack is gone.
        ::TerminateProcess(::GetCurrentProcess(), 0xE0000002ul);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const unsigned long code =
        (ep != nullptr && ep->ExceptionRecord != nullptr)
            ? static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode)
            : 0ul;
    const auto addr =
        (ep != nullptr && ep->ExceptionRecord != nullptr)
            ? reinterpret_cast<std::uintptr_t>(ep->ExceptionRecord->ExceptionAddress)
            : 0u;
    stderrAttribution(code, addr);

    const char* reason = "structured exception";
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: reason = "access violation"; break;
        case EXCEPTION_STACK_OVERFLOW: reason = "stack overflow"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO: reason = "integer divide by zero"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION: reason = "illegal instruction"; break;
        case 0xE06D7363ul: reason = "unhandled c++ exception"; break;
        default: break;
    }
    writeReport(reason, code, addr, ep);
    finish(code != 0 ? code : 0xE0000001ul);
    // Production default: let the original exception continue to Windows, so
    // WER still behaves exactly as it always has on a user's machine. Any
    // filter that was already installed still gets its turn.
    ::InterlockedExchange(&g_inHandler, 0);
    if (g_prevSehFilter != nullptr) { return g_prevSehFilter(ep); }
    return EXCEPTION_CONTINUE_SEARCH;
}

void onTerminate() {
    if (::InterlockedCompareExchange(&g_inHandler, 1, 0) != 0) {
        ::TerminateProcess(::GetCurrentProcess(), 0xE0000002ul);
        return;
    }
    // The faulting address is this handler's own return site, which resolves
    // to the module whose exception escaped - which is the useful half.
    void* here = _ReturnAddress();
    writeReport("std::terminate", 0xE0000003ul, reinterpret_cast<std::uintptr_t>(here), nullptr);
    finish(0xE0000003ul);
    // The report is already written, so the SIGABRT net must not write a
    // second one for the same fault: stand it down and let the process die
    // exactly the way it always did.
    std::signal(SIGABRT, SIG_DFL);
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
    if (::InterlockedCompareExchange(&g_inHandler, 1, 0) != 0) {
        ::TerminateProcess(::GetCurrentProcess(), 0xE0000002ul);
        return;
    }
    void* here = _ReturnAddress();
    writeReport("abort (std::terminate on a thread with no handler, or a direct abort)",
                0xE0000006ul, reinterpret_cast<std::uintptr_t>(here), nullptr);
    finish(0xE0000006ul);
    ::_exit(3);
}

void __cdecl onPureCall() {
    if (::InterlockedCompareExchange(&g_inHandler, 1, 0) != 0) {
        ::TerminateProcess(::GetCurrentProcess(), 0xE0000002ul);
        return;
    }
    void* here = _ReturnAddress();
    writeReport("purecall", 0xE0000004ul, reinterpret_cast<std::uintptr_t>(here), nullptr);
    finish(0xE0000004ul);
    std::signal(SIGABRT, SIG_DFL);  // see onTerminate: one fault, one report
    ::abort();
}

void __cdecl onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int,
                                uintptr_t) {
    if (::InterlockedCompareExchange(&g_inHandler, 1, 0) != 0) {
        ::TerminateProcess(::GetCurrentProcess(), 0xE0000002ul);
        return;
    }
    // The wide-character arguments are DELIBERATELY dropped: they are only
    // populated in a debug CRT, and formatting them would mean a CRT call on
    // the fault path for a string that is empty in every shipped build.
    void* here = _ReturnAddress();
    writeReport("invalid parameter", 0xE0000005ul, reinterpret_cast<std::uintptr_t>(here),
                nullptr);
    finish(0xE0000005ul);
    std::signal(SIGABRT, SIG_DFL);  // see onTerminate: one fault, one report
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

    // The module snapshot the fault path searches. Refreshed again by the
    // application after anything that loads code; this is just the floor.
    if (moduleCount() == 0) { refreshModuleTable(); }

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
    g_minidump = minidump;
    if (g_minidump && g_miniDumpWriteDump == nullptr) {
        HMODULE dbghelp = ::LoadLibraryA("dbghelp.dll");
        if (dbghelp != nullptr) {
            g_miniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFn>(
                reinterpret_cast<void*>(::GetProcAddress(dbghelp, "MiniDumpWriteDump")));
        }
    }
#else
    (void)enabled;
    (void)minidump;
#endif
}

std::string lastCrashReportPath() {
#if defined(_WIN32)
    return std::string(g_lastPath);
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
#else
    return std::string();
#endif
}

void reportAbsorbedFault(const char* reason, unsigned long code, const void* faultAddress,
                         void* exceptionPointers) {
#if defined(_WIN32)
    // A SEPARATE latch from g_inHandler, and that is not tidiness. g_inHandler
    // means "a FATAL handler is running"; sehFilter reads a set g_inHandler as
    // proof it has faulted inside itself and answers by killing the process.
    // Borrowing it here would turn a real crash that happened to land while
    // this absorbed report was being written into a silent TerminateProcess
    // with no report at all.
    static long inAbsorbed = 0;
    if (::InterlockedCompareExchange(&inAbsorbed, 1, 0) != 0) { return; }
    writeReport(reason != nullptr ? reason : "absorbed fault", code,
                reinterpret_cast<std::uintptr_t>(faultAddress),
                static_cast<EXCEPTION_POINTERS*>(exceptionPointers));
    ::InterlockedExchange(&inAbsorbed, 0);
#else
    (void)reason;
    (void)code;
    (void)faultAddress;
    (void)exceptionPointers;
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
#if defined(_MSC_VER)
#pragma optimize("", off)
#endif
void pokePureVirtual(PureBase* b) { b->nowhere(); }
#if defined(_MSC_VER)
#pragma optimize("", on)
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
#endif
            break;
        }
    }
    std::abort();  // reached only if the fault above failed to happen
}

}  // namespace cascade::core
