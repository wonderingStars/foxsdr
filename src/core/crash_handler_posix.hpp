// crash_handler_posix.hpp - the Linux half of crash_handler.hpp's contract.
//
// Included only by crash_handler.cpp (to forward the public API declared in
// crash_handler.hpp) and by crash_handler_posix.cpp itself. Everything here is
// implementation detail: the public promises - what is covered, what a report
// contains, the allocation-free discipline on the fault path - are all in
// crash_handler.hpp and apply identically on Linux. See crash_handler_posix.cpp
// for how they are kept: sigaction on an alternate stack for SIGSEGV/SIGBUS/
// SIGILL/SIGFPE/SIGTRAP, a SIGABRT net under std::set_terminate exactly as the
// Windows implementation describes (libstdc++'s terminate handler is process-
// wide rather than the MSVC per-thread one, but the net is kept anyway: a
// thread that dies through __cxa_pure_virtual or a direct abort() never goes
// through std::terminate at all), and a libunwind walk of the ucontext the
// kernel hands every signal handler registered with SA_SIGINFO.
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

// The Linux stand-in for TestFaultKind::InvalidParameter. glibc has no CRT
// invalid-parameter fail-fast to exercise, so this is the closest honest
// equivalent: a runtime-detected fatal condition (a direct abort()) reported
// under its own distinct reason rather than folded into the generic SIGABRT
// path, so the test can still tell "this registration fired" from "nothing
// happened, and abort() ran unhandled". See raiseTestFault's kind-3 case.
[[noreturn]] void raiseInvalidParameterTestFault();

}  // namespace cascade::core::posix_detail

#endif  // CASCADE_CORE_CRASH_HANDLER_POSIX_HPP
