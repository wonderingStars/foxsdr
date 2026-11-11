// Counts the calling thread's global operator new while armed (the repo's pattern for "this call allocates nothing"). Include in EXACTLY ONE
// translation unit per test executable: it defines the global operator new / delete.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>

namespace dspm_alloc {
inline thread_local bool t_counting = false;
inline thread_local long t_news = 0;
}  // namespace dspm_alloc

void* operator new(std::size_t n) {
    if (dspm_alloc::t_counting) { ++dspm_alloc::t_news; }
    void* p = std::malloc(n != 0 ? n : 1);
    if (p == nullptr) { throw std::bad_alloc(); }
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

class DspmAllocProbe {
public:
    DspmAllocProbe() {
        dspm_alloc::t_news = 0;
        dspm_alloc::t_counting = true;
    }
    ~DspmAllocProbe() { dspm_alloc::t_counting = false; }
    DspmAllocProbe(const DspmAllocProbe&) = delete;
    DspmAllocProbe& operator=(const DspmAllocProbe&) = delete;
    long news() const { return dspm_alloc::t_news; }
};
