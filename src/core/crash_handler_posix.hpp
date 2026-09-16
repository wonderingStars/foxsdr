// crash_handler_posix.hpp - the Linux half of crash_handler.hpp's contract.
//
// Included only by crash_handler.cpp (to forward the public API declared in
// crash_handler.hpp) and by crash_handler_posix.cpp itself. Everything here is
// implementation detail: the public promises - what is covered, what a report
// contains, the allocation-free discipline on the fault path - are all in
// crash_handler.hpp and apply identically on Linux AND Android (the NDK build
// takes this same file - see CMakeLists.txt's CASCADE_ANDROID guard around
// crash_handler_posix.cpp, which used to exclude it entirely before this was
// written). See crash_handler_posix.cpp for how the contract is kept:
// sigaction on an alternate stack for SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGTRAP, a
// SIGABRT net under std::set_terminate exactly as the Windows implementation
// describes (libstdc++'s terminate handler is process-wide rather than the
// MSVC per-thread one, but the net is kept anyway: a thread that dies through
// __cxa_pure_virtual or a direct abort() never goes through std::terminate at
// all), and a LOCAL unwind of the ucontext the kernel hands every signal
// handler registered with SA_SIGINFO - libunwind's unw_init_local/unw_step on
// desktop Linux, or clang's compiler-runtime _Unwind_Backtrace (<unwind.h>,
// no separate library) on Android, where the NDK ships no libunwind
// local-unwind package. See that file's header for why the two are equivalent
// in what they capture despite the very different API.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_CRASH_HANDLER_POSIX_HPP
#define CASCADE_CORE_CRASH_HANDLER_POSIX_HPP

#include "core/crash_handler.hpp"

#include <cstdint>
#include <string>

namespace cascade::core::posix_detail {

void install(const CrashHandlerConfig& cfg);
void setEnabled(bool enabled, bool minidump);
std::string lastReportPath();
std::string activeDir();

// `faultAddress` is a plain code address here, never an EXCEPTION_POINTERS*:
// POSIX has no structured-exception equivalent to hand one back through, so a
// caller that wants a stack for an absorbed fault gets the CALLING thread's
// own, exactly as reportAbsorbedFault(..., exceptionPointers=nullptr) does on
// Windows.
void reportAbsorbed(const char* reason, unsigned long code, const void* faultAddress);
void reportAbsorbedChild(const char* reason, unsigned long childExitCode, int attempt);

// TEST HOOK mirror of captureFramesForTest. There is no POSIX analogue of "a
// supplied-but-empty EXCEPTION_POINTERS", so this takes only the
// mayWalkCurrentThread half of the Windows hook's contract.
int captureFramesForTest(bool mayWalkCurrentThread);

// How far into a captured walk the faulting instruction is looked for. Small
// on purpose: the frames ahead of it are this handler's own and the kernel's
// trampoline, and a walk whose shape this rule does not recognise inside the
// first few frames is one nobody has measured - it is written whole instead.
constexpr int kStackStartSearchFrames = 8;

// WHERE A WRITTEN STACK BEGINS, and why the answer is not always frame zero.
//
// The Android branch's _Unwind_Backtrace unwinds from where it is CALLED -
// there is no supplied-context form of it - so a captured walk starts inside
// this handler and only reaches the fault three or four frames down; the
// desktop libunwind branch walks the signal's own ucontext and starts AT the
// fault. Written out raw, an Android report therefore names
// captureFramesFromContext as its first frame, which is what the crash
// dashboard groups a report by (foxsdrWebsite symbols.go walks DOWN to the
// first frame it can put a name to) - so every Android fault, whatever broke,
// shared one group. Measured on emulator-5556 against the real site; see
// tests/test_crash_stack_start.cpp for the frames that came off the wire.
//
// The faulting instruction is already known independently: it is read from the
// ucontext and written as the report's `address:` field. This returns the index
// of the first frame equal to that address, or 0 - meaning "write everything" -
// when the address is zero, absent, or further down than
// kStackStartSearchFrames. On desktop that is frame 0, and nothing changes.
//
// Called from the fault path, so it allocates nothing, locks nothing, and
// touches no global state.
int stackStartIndex(const unsigned long* frames, int frameCount, std::uintptr_t faultAddress);

// The Linux stand-in for TestFaultKind::InvalidParameter. glibc has no CRT
// invalid-parameter fail-fast to exercise, so this is the closest honest
// equivalent: a runtime-detected fatal condition (a direct abort()) reported
// under its own distinct reason rather than folded into the generic SIGABRT
// path, so the test can still tell "this registration fired" from "nothing
// happened, and abort() ran unhandled". See raiseTestFault's kind-3 case.
[[noreturn]] void raiseInvalidParameterTestFault();

}  // namespace cascade::core::posix_detail

#endif  // CASCADE_CORE_CRASH_HANDLER_POSIX_HPP
