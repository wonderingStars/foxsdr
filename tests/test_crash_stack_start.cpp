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
// construction on this platform: _Unwind_Backtrace always unwinds from where
// it is called, never from a supplied context, so the walk starts inside the
// handler (crash_handler_posix.cpp's Android branch says so in its comment).
// The desktop libunwind branch walks the ucontext itself and therefore starts
// AT the fault.
//
// That difference is not cosmetic, because the dashboard groups a report by
// the nearest frame it can put a name to, walking DOWN from frame 0
// (foxsdrWebsite symbols.go, symbolGroupKey). On Android the nearest nameable
// frame is always captureFramesFromContext - our own code, with a symbol map -
// so every Android fault, whatever broke, gets the key
// "crash libc.so @ ...captureFramesFromContext(...)". Measured: that is
// exactly the group the first real Android report produced. Symbolic grouping
// is the feature that stops unrelated faults merging, and on this platform it
// merged ALL of them.
//
// The same three frames also made `fault-thread-own` meaningless here: the
// field is "did any frame of the faulting thread come from our module", and
// the handler is our module, so the answer was yes for every Android report
// regardless of where the fault was.
//
// THE RULE UNDER TEST. The report already records the faulting instruction
// separately, read from the signal's ucontext (`address:`), and that address
// appears in the captured walk as the first frame BELOW the trampoline. So the
// written stack starts at the first frame equal to that address - which is
// frame 0 on desktop Linux (no change at all there) and frame 4 in the walk
// above. If the address is not among the first few frames the whole stack is
// written unchanged: losing a stack would be far worse than a mis-keyed group,
// and a stack whose shape this rule does not recognise is one nobody has
// measured.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <cstdio>

#include "core/crash_handler_posix.hpp"

#include "test_check.hpp"

namespace {

using cascade::core::posix_detail::stackStartIndex;

}  // namespace

int main() {
    // --- the measured Android shape: three handler frames, a trampoline, then
    // the faulting instruction -------------------------------------------
    {
        const unsigned long frames[] = {0x7f0000059865UL,  // captureFramesFromContext
                                        0x7f00000578EDUL,  // writeReport
                                        0x7f0000058555UL,  // faultSignalHandler
                                        0x7fabcd05DA10UL,  // libc trampoline
                                        0x7fabcd061610UL,  // THE FAULT
                                        0x7fabcd0633E1UL};
        CHECK(stackStartIndex(frames, 6, 0x7fabcd061610UL) == 4);
    }

    // --- the desktop shape: the walk already starts at the fault ----------
    {
        const unsigned long frames[] = {0x4001000UL, 0x4002000UL, 0x4003000UL};
        CHECK(stackStartIndex(frames, 3, 0x4001000UL) == 0);
    }

    // --- an address that is not in the walk: keep every frame -------------
    {
        const unsigned long frames[] = {0x4001000UL, 0x4002000UL, 0x4003000UL};
        CHECK(stackStartIndex(frames, 3, 0xDEADBEEFUL) == 0);
    }

    // --- no address at all (std::terminate with nothing to report) --------
    {
        const unsigned long frames[] = {0x4001000UL, 0x4002000UL};
        CHECK(stackStartIndex(frames, 2, 0) == 0);
    }

    // --- nothing to trim -------------------------------------------------
    {
        const unsigned long frames[] = {0x4001000UL};
        CHECK(stackStartIndex(frames, 0, 0x4001000UL) == 0);
        CHECK(stackStartIndex(nullptr, 4, 0x4001000UL) == 0);
        CHECK(stackStartIndex(frames, -1, 0x4001000UL) == 0);
    }

    // --- the search window: the last frame inside it is honoured, the first
    // frame outside it is not, and a stack that deep is written whole ------
    {
        unsigned long frames[32];
        for (int i = 0; i < 32; ++i) { frames[i] = 0x5000000UL + static_cast<unsigned long>(i); }
        const int window = cascade::core::posix_detail::kStackStartSearchFrames;
        CHECK(window >= 4);
        CHECK(stackStartIndex(frames, 32, frames[window - 1]) == window - 1);
        CHECK(stackStartIndex(frames, 32, frames[window]) == 0);
    }

    // --- recursion: the FIRST occurrence wins, so a recursive fault keeps
    // the frames above its own repeat rather than collapsing to the last ---
    {
        const unsigned long frames[] = {0x9001000UL, 0x9002000UL, 0x9003000UL,
                                        0x9002000UL, 0x9002000UL};
        CHECK(stackStartIndex(frames, 5, 0x9002000UL) == 1);
    }

    std::printf("test_crash_stack_start: search window %d frames\n",
                cascade::core::posix_detail::kStackStartSearchFrames);
    return testSummary("test_crash_stack_start");
}
