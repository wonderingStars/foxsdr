// wire_c_crash_trace.hpp - a test-only crash reporter: on an unhandled exception it prints the code, the faulting address and a symbolised stack of the crashing thread to stdout
// (DbgHelp, the program's own PDB), then lets the process end. Used while a flaky crash of wiring lane C's tests is being found; included by one test.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>

#include <cstdio>

#pragma comment(lib, "dbghelp.lib")

namespace wirec_test {

inline LONG WINAPI crashTraceFilter(EXCEPTION_POINTERS* info) {
    std::printf("\nCRASH: code 0x%08lx at %p (thread %lu)\n", info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
        std::printf("       %s address %p\n", info->ExceptionRecord->ExceptionInformation[0] == 0 ? "reading" : "writing", reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]));
    }
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, nullptr, TRUE);
    CONTEXT ctx = *info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 40; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) { break; }
        if (frame.AddrPC.Offset == 0) { break; }
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        const bool haveSym = SymFromAddr(process, frame.AddrPC.Offset, &disp, sym) != FALSE;
        const bool haveLine = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisp, &line) != FALSE;
        std::printf("  #%d %p %s%s%s:%lu\n", i, reinterpret_cast<void*>(frame.AddrPC.Offset), haveSym ? sym->Name : "?", haveLine ? " " : "", haveLine ? line.FileName : "", haveLine ? line.LineNumber : 0ul);
    }
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

inline void installCrashTrace() { SetUnhandledExceptionFilter(&crashTraceFilter); }

}  // namespace wirec_test
#else
namespace wirec_test {
inline void installCrashTrace() {}
}  // namespace wirec_test
#endif
