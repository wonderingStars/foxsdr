// Counts the calling thread's global operator new while armed: the repo's pattern for
// "this call allocates nothing" (the same counter as tests/test_engine_publish_alloc.cpp,
// kept here so the signal-block tests share one copy).
//
// INCLUDE IN EXACTLY ONE TRANSLATION UNIT PER TEST EXECUTABLE: it defines the global
// operator new / delete. Usage:
//
//     AllocProbe probe;          // arms the counter for this thread
//     block.process(...);
//     CHECK(probe.news() == 0);  // counted so far; the destructor disarms
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>

namespace allocprobe {
inline thread_local bool t_counting = false;
inline thread_local long t_news = 0;
}  // namespace allocprobe

void* operator new(std::size_t n) {
    if (allocprobe::t_counting) { ++allocprobe::t_news; }
    void* p = std::malloc(n != 0 ? n : 1);
    if (p == nullptr) { throw std::bad_alloc(); }
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

class AllocProbe {
public:
    AllocProbe() {
        allocprobe::t_news = 0;
        allocprobe::t_counting = true;
    }
    ~AllocProbe() { allocprobe::t_counting = false; }
    AllocProbe(const AllocProbe&) = delete;
    AllocProbe& operator=(const AllocProbe&) = delete;
    long news() const { return allocprobe::t_news; }
};
