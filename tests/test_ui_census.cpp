/*
 * THE INTERFACE CENSUS COSTS NOTHING WHEN IT IS OFF.
 *
 * gui/ui_census.hpp lets every key, rail section, switch row, status card and
 * deck part note itself, every frame, so the theme census can prove no theme
 * hides a function. It is on only for a test run (FOXSDR_UI_CENSUS). The
 * first cut built each name - "key:" + the key's id - as a std::string at the
 * call, BEFORE asking whether anybody was listening: an allocation per part
 * per frame in every user's application, for a list nobody was keeping
 * (repair round, 2026-09-25). Names are now handed over in pieces and put
 * together only when the census is on. This counts every heap allocation the
 * calls make with it off, and requires none.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>

#include "gui/ui_census.hpp"
#include "test_check.hpp"

namespace {
std::atomic<long> gAllocs{0};
std::atomic<bool> gCounting{false};
}  // namespace

// Every allocation in this process goes through these while the test counts.
void* operator new(std::size_t n) {
    if (gCounting.load()) { ++gAllocs; }
    if (void* p = std::malloc(n != 0 ? n : 1)) { return p; }
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (gCounting.load()) { ++gAllocs; }
    if (void* p = std::malloc(n != 0 ? n : 1)) { return p; }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main() {
    namespace census = cascade::gui::census;
    // OFF: the variable is not set when the census first asks.
#if defined(_WIN32)
    _putenv_s("FOXSDR_UI_CENSUS", "");
#else
    unsetenv("FOXSDR_UI_CENSUS");
#endif
    CHECK(!census::enabled());

    // A key id well past any small-string buffer, as the rail's real ids are
    // ("section:Source###source" is 23 bytes; MSVC's buffer holds 15).
    const std::string longId = "a-rather-long-key-identifier-well-past-any-small-string-buffer";
    const char* caption = "MESSAGE RATE";
    std::printf("  with the census off, noting 60000 parts allocates nothing\n");
    gAllocs = 0;
    gCounting = true;
    for (int i = 0; i < 10000; ++i) {
        census::note("key:", longId);
        census::note("section:", "Source###source");
        census::note("status:", caption);
        census::note("bank:", i % 5);
        census::note("deck:stop");
        census::rect("deck:lamp", i % 4, 0.0f, 0.0f, 1.0f, 1.0f);
        census::rect("deck:stop", 0.0f, 0.0f, 1.0f, 1.0f);
    }
    gCounting = false;
    const long allocs = gAllocs.load();
    if (allocs != 0) { std::printf("  %ld allocations with the census off\n", allocs); }
    CHECK(allocs == 0);

    // The counter itself counts: a std::string that outgrows its buffer is one
    // allocation, or this test proves nothing.
    gAllocs = 0;
    gCounting = true;
    {
        const std::string s = std::string("key:") + longId;
        CHECK(!s.empty());
    }
    gCounting = false;
    CHECK(gAllocs.load() >= 1);

    return testSummary("test_ui_census");
}
