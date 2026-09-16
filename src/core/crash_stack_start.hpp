// crash_stack_start.hpp - where a crash report's written stack begins, and why
// the answer is not always frame zero.
//
// WHAT THIS IS FOR. A crash report carries the faulting instruction twice: once
// as its own `address:` field, read straight from the fault context, and once
// somewhere inside the captured stack walk. On Windows and on desktop Linux
// those two agree at frame 0, because both unwinders are given the fault
// context and start from it. On Android they do not: the NDK's
// _Unwind_Backtrace unwinds from wherever it is CALLED - there is no
// supplied-context form of it - so the walk begins inside the crash handler
// (captureFramesFromContext, writeReport, faultSignalHandler) and the kernel's
// sigreturn trampoline, and only then reaches the fault.
//
// WHAT THAT COST, measured on emulator-5556 against the real crash dashboard
// over TLS. The dashboard groups a report by the nearest frame it can put a
// name to, walking DOWN from frame 0 (foxsdrWebsite symbols.go,
// symbolGroupKey). The nearest nameable frame on Android was therefore always
// captureFramesFromContext - our own code, with a symbol map - so every
// Android fault, whatever had actually broken, arrived under the key
// "crash libc.so @ ...captureFramesFromContext(...)". Symbolic grouping exists
// to stop unrelated faults merging into one signature; on that platform it
// merged all of them. The same frames also made the report's
// `fault-thread-own` field meaningless there: it asks whether any frame of the
// faulting thread came from our own module, and the handler IS our module, so
// the answer was yes for every Android report regardless of where the fault
// was.
//
// THE RULE. Written stacks start at the first frame equal to the faulting
// address the report already names. On Windows and desktop Linux that is frame
// 0 and nothing moves; on Android it is the frame below the trampoline. An
// address that is zero, absent from the walk, or further down than
// kCrashStackSearchFrames leaves the walk untouched - losing a stack would be
// far worse than a mis-keyed group, and a walk whose shape this rule does not
// recognise is one nobody has measured.
//
// WHY IT LIVES IN A HEADER OF ITS OWN, all of two dozen lines. The rule is
// platform-free arithmetic, but the only file that needed it -
// core/crash_handler_posix.cpp - is removed from CASCADE_CORE on Windows, so a
// definition in there is a rule that cannot even be LINKED on the platform
// that ships most of the copies, let alone tested on it. Inline and
// header-only, it compiles into whatever includes it, and
// tests/test_crash_stack_start.cpp runs the identical checks in every
// configuration. Nothing here includes a POSIX header, a Windows header, or
// anything of ImGui's; <cstdint> is the whole dependency.
//
// A TEMPLATE, because the two handlers hold their walks in different words:
// crash_handler.cpp's buffer is unsigned long long and
// crash_handler_posix.cpp's is unsigned long (and on MSVC those are not the
// same width, so a single signature would either truncate real addresses or
// force a cast at the call site). The comparison is done in std::uintptr_t,
// which is the type both of them write into a report.
//
// FAULT-PATH CODE: no allocation, no locks, no globals, no library calls.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_CRASH_STACK_START_HPP
#define CASCADE_CORE_CRASH_STACK_START_HPP

#include <cstdint>

namespace cascade::core {

// How far into a captured walk the faulting instruction is looked for. Small
// on purpose: the frames ahead of it are the handler's own and the kernel's
// trampoline, and there are never many of either.
constexpr int kCrashStackSearchFrames = 8;

// The index the written stack should start at; 0 means "write the whole walk".
// `Word` is the platform handler's frame word (unsigned long long on Windows,
// unsigned long on POSIX); the match is made in std::uintptr_t.
template <typename Word>
inline int crashStackStartIndex(const Word* frames, int frameCount,
                               std::uintptr_t faultAddress) {
    if (frames == nullptr || frameCount <= 0 || faultAddress == 0) { return 0; }
    const int limit =
        (frameCount < kCrashStackSearchFrames) ? frameCount : kCrashStackSearchFrames;
    for (int i = 0; i < limit; ++i) {
        if (static_cast<std::uintptr_t>(frames[i]) == faultAddress) { return i; }
    }
    return 0;
}

}  // namespace cascade::core

#endif  // CASCADE_CORE_CRASH_STACK_START_HPP
