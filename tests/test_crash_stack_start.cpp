// Where a crash report's written stack begins - the one thing that made every
// Android crash report land in the SAME group on the dashboard.
//
// WHAT WAS MEASURED (emulator-5556, FoxSDR 0.98.0, against the real site
// binary 2.9.0 over TLS). An Android report's faulting-thread stack came off
// the wire starting like this:
//
//   0  libfoxsdr.so  captureFramesFromContext(ucontext*, unsigned long*, int)
//   1  libfoxsdr.so  writeReport(...)
//   2  libfoxsdr.so  faultSignalHandler(int, siginfo*, void*)
//   3  libc.so       (the sigreturn trampoline)
//   4  libc.so       <- the faulting instruction, which the report's own
//                       `address:` field already names
//   ...              <- and only here the code that actually broke
//
// Those first three frames are the CRASH HANDLER'S OWN, and they are there by
// construction on that platform: the NDK's _Unwind_Backtrace always unwinds
// from where it is called, never from a supplied context, so the walk starts
// inside the handler (crash_handler_posix.cpp's Android branch says so in its
// own comment). The Windows unwinder and desktop Linux's libunwind are both
// given the fault context and therefore start AT the fault.
//
// That difference is not cosmetic, because the dashboard groups a report by
// the nearest frame it can put a name to, walking DOWN from frame 0
// (foxsdrWebsite symbols.go, symbolGroupKey). On Android the nearest nameable
// frame was always captureFramesFromContext - our own code, with a symbol map -
// so every Android fault, whatever broke, got the key
// "crash libc.so @ ...captureFramesFromContext(...)". Measured: that is
// exactly the group the first real Android report produced. Symbolic grouping
// is the feature that stops unrelated faults merging, and on that platform it
// merged all of them. The same three frames also made `fault-thread-own`
// meaningless there: the field is "did any frame of the faulting thread come
// from our module", and the handler is our module, so the answer was yes for
// every Android report regardless of where the fault was.
//
// THE RULE UNDER TEST (core/crash_stack_start.hpp). The report already records
// the faulting instruction separately, read from the fault context, and that
// address appears in the captured walk as the first frame below the trampoline.
// So the written stack starts at the first frame equal to that address - which
// is frame 0 on Windows and desktop Linux (no change at all there) and frame 4
// in the walk above. If the address is not among the first few frames the whole
// stack is written unchanged: losing a stack would be far worse than a
// mis-keyed group, and a stack whose shape this rule does not recognise is one
// nobody has measured.
//
// THIS TEST RUNS EVERYWHERE, and that is the point of the header it includes.
// The rule first lived in crash_handler_posix.cpp, which the root
// CMakeLists.txt removes from CASCADE_CORE under WIN32 - so on the platform
// that ships most of the copies the function could not even be linked, and
// this test failed to build there (LNK2019) while passing on Linux. Reached
// through a header with nothing platform-specific in it, the identical checks
// below run in every configuration; the "desktop shape" case IS the Windows
// shape.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <cstdio>

#include "core/crash_stack_start.hpp"

#include "test_check.hpp"

namespace {

using cascade::core::crashStackStartIndex;
using cascade::core::kCrashStackSearchFrames;

}  // namespace

int main() {
    // --- the measured Android shape: three handler frames, a trampoline, then
    // the faulting instruction. std::uintptr_t, because a real 48-bit address
    // does not fit in MSVC's 32-bit unsigned long ---------------------------
    {
        const std::uintptr_t frames[] = {0x7f0000059865ULL,  // captureFramesFromContext
                                         0x7f00000578EDULL,  // writeReport
                                         0x7f0000058555ULL,  // faultSignalHandler
                                         0x7fabcd05DA10ULL,  // libc trampoline
                                         0x7fabcd061610ULL,  // THE FAULT
                                         0x7fabcd0633E1ULL};
        CHECK(crashStackStartIndex(frames, 6, 0x7fabcd061610ULL) == 4);
    }

    // --- the Windows and desktop-Linux shape: the walk already starts at the
    // fault, so nothing is trimmed -----------------------------------------
    {
        const std::uintptr_t frames[] = {0x4001000ULL, 0x4002000ULL, 0x4003000ULL};
        CHECK(crashStackStartIndex(frames, 3, 0x4001000ULL) == 0);
    }

    // --- the two frame words the handlers actually hold their walks in, so
    // both instantiations are exercised: unsigned long long is
    // crash_handler.cpp's buffer and unsigned long is
    // crash_handler_posix.cpp's (32-bit on MSVC, hence the small addresses) --
    {
        const unsigned long long wide[] = {0x7f0000059865ULL, 0x7fabcd061610ULL};
        CHECK(crashStackStartIndex(wide, 2, 0x7fabcd061610ULL) == 1);
        const unsigned long narrow[] = {0x40001000UL, 0x40002000UL, 0x40003000UL};
        CHECK(crashStackStartIndex(narrow, 3, 0x40002000UL) == 1);
    }

    // --- an address that is not in the walk: keep every frame -------------
    {
        const std::uintptr_t frames[] = {0x4001000ULL, 0x4002000ULL, 0x4003000ULL};
        CHECK(crashStackStartIndex(frames, 3, 0xDEADBEEFULL) == 0);
    }

    // --- no address at all (std::terminate with nothing to report) --------
    {
        const std::uintptr_t frames[] = {0x4001000ULL, 0x4002000ULL};
        CHECK(crashStackStartIndex(frames, 2, 0) == 0);
    }

    // --- nothing to trim -------------------------------------------------
    {
        const std::uintptr_t frames[] = {0x4001000ULL};
        CHECK(crashStackStartIndex(frames, 0, 0x4001000ULL) == 0);
        CHECK(crashStackStartIndex<std::uintptr_t>(nullptr, 4, 0x4001000ULL) == 0);
        CHECK(crashStackStartIndex(frames, -1, 0x4001000ULL) == 0);
    }

    // --- the search window: the last frame inside it is honoured, the first
    // frame outside it is not, and a stack that deep is written whole ------
    {
        std::uintptr_t frames[32];
        for (int i = 0; i < 32; ++i) {
            frames[i] = 0x5000000ULL + static_cast<std::uintptr_t>(i);
        }
        const int window = kCrashStackSearchFrames;
        CHECK(window >= 4);
        CHECK(crashStackStartIndex(frames, 32, frames[window - 1]) == window - 1);
        CHECK(crashStackStartIndex(frames, 32, frames[window]) == 0);
    }

    // --- recursion: the FIRST occurrence wins, so a recursive fault keeps
    // the frames above its own repeat rather than collapsing to the last ---
    {
        const std::uintptr_t frames[] = {0x9001000ULL, 0x9002000ULL, 0x9003000ULL,
                                         0x9002000ULL, 0x9002000ULL};
        CHECK(crashStackStartIndex(frames, 5, 0x9002000ULL) == 1);
    }

    std::printf("test_crash_stack_start: search window %d frames\n", kCrashStackSearchFrames);
    return testSummary("test_crash_stack_start");
}
