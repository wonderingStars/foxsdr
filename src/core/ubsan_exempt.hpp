// ubsan_exempt.hpp - for the few functions whose job is to commit undefined behaviour (0.99.69).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// WHY THIS EXISTS. The crash handler is tested by faulting a real child process
// on purpose: a store through a null pointer is the access violation, and the
// handler under test must be the one that sees it. UndefinedBehaviorSanitizer
// sees it first. Its null check reports the store and, under halt_on_error,
// ends the child with exit status 1 BEFORE the fault is raised, so the handler
// never runs and the test reads that as "the handler wrote nothing" (the first
// Linux sanitizer run, 37355910809: test_crash_capture and test_soapy_enum_proc
// failed this way, 24 checks and the whole file).
//
// Turning UBSan off for the sanitizer job would be the wrong answer, and so would
// halt_on_error=0 for those tests: both give up the check on every other line. The
// exemption is on the one function whose purpose the check contradicts, and
// nothing else. Elsewhere, and in every build without UBSan, it expands to
// nothing.
#ifndef CASCADE_CORE_UBSAN_EXEMPT_HPP
#define CASCADE_CORE_UBSAN_EXEMPT_HPP

#if defined(__clang__) || defined(__GNUC__)
#define CASCADE_UBSAN_EXEMPT __attribute__((no_sanitize("undefined")))
#else
#define CASCADE_UBSAN_EXEMPT
#endif

#endif  // CASCADE_CORE_UBSAN_EXEMPT_HPP
