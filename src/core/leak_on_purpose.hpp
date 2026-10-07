// leak_on_purpose.hpp - tells LeakSanitizer that a leak is the design (0.99.69).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// WHY THIS EXISTS. Several places in the application give up on a thread that
// will not come back (a vendor driver stuck in a read, a USB transfer the
// operating system never completes) and then LEAK the object that thread is
// inside, because freeing it under a running thread is a use-after-free and a
// hang on the window's thread is worse than a few hundred bytes. That is a
// decision, written beside each of those sites. In a shipped build nothing
// looks at it: the stranded thread keeps the object reachable for as long as
// the process lives.
//
// A TEST CHANGES THAT. It lets the stranded thread go (so no test leaves a
// thread asleep in a fake) and the object is then unreachable, which is a leak
// by LeakSanitizer's definition and failed eleven tests of the first Linux
// sanitizer run (run 37355910809). The answer is not a suppression for the
// whole class - a leak of the same object on an ordinary path must still be
// found - but a mark on exactly the object that is abandoned on purpose, made
// where the decision is made.
//
// WHAT IT DOES. With AddressSanitizer on Linux (LeakSanitizer is part of it
// there), the object, and everything reachable only through it, is excluded
// from the leak report. Everywhere else it is the identity function: MSVC's
// AddressSanitizer has no leak checker, and a build with no sanitizer must be
// exactly the build it was. CASCADE_SANITIZE_ADDRESS is defined by
// cmake/sanitize.cmake for the instrumented targets only.
#ifndef CASCADE_CORE_LEAK_ON_PURPOSE_HPP
#define CASCADE_CORE_LEAK_ON_PURPOSE_HPP

#if defined(CASCADE_SANITIZE_ADDRESS) && defined(__linux__)
// Declared here rather than by <sanitizer/lsan_interface.h>, which not every
// toolchain package ships; the runtime that carries it is libasan/libclang_rt.
extern "C" void __lsan_ignore_object(const void* p);
#endif

namespace cascade::core {

// Marks the heap object `p` points into as deliberately never freed; returns
// `p` so a release() can be wrapped in place: leakOnPurpose(dev_.release()).
template <class T>
inline T* leakOnPurpose(T* p) noexcept {
#if defined(CASCADE_SANITIZE_ADDRESS) && defined(__linux__)
    if (p != nullptr) { __lsan_ignore_object(p); }
#endif
    return p;
}

}  // namespace cascade::core

#endif  // CASCADE_CORE_LEAK_ON_PURPOSE_HPP
