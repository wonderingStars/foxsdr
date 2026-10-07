// Minimal dependency-free test harness: one executable per test file.
//
//   int main() {
//       CHECK(cond);
//       CHECK_NEAR(a, b, tol);
//       return testSummary("test_name");
//   }
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <cmath>
#include <cstdio>

// ATOMIC, because a CHECK is called from worker threads as well as from main()
// (a fake driver's callback, a launcher the code under test runs on its own
// thread), and two plain `++g_checksRun` on two threads are a data race. Found by
// ThreadSanitizer in the first Linux run (37355910809, test_sdrplay_service); the
// same global is written by every test that checks from a thread. They count and
// compare like the ints they replace; printing one needs .load().
inline std::atomic<int> g_checksFailed{0};
inline std::atomic<int> g_checksRun{0};
inline std::atomic<int> g_checksSkipped{0};

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checksRun;                                                  \
        if (!(cond)) {                                                  \
            ++g_checksFailed;                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                \
    } while (0)

// A HONEST PLATFORM SKIP, never a silent pass.
//
// Some behaviour (today: real Windows crash/hang capture) genuinely does not
// exist yet on a platform, so asserting it there would either fail forever
// against a documented no-op or - worse - be quietly dropped from an #ifdef
// with nothing to say it was ever meant to run. SKIP_LINUX prints why and
// counts itself in the summary line, so "0 failed" and "nothing was checked"
// can never be confused. It never counts as a failure: a skip is a stated
// fact about this platform's current capability, not a defect in the test.
#define SKIP_LINUX(reason)                                    \
    do {                                                      \
        ++g_checksSkipped;                                    \
        std::printf("SKIP (linux): %s\n", reason);            \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                       \
    do {                                                                            \
        ++g_checksRun;                                                              \
        const double check_a = static_cast<double>(a);                              \
        const double check_b = static_cast<double>(b);                              \
        if (!(std::fabs(check_a - check_b) <= (tol))) {                             \
            ++g_checksFailed;                                                       \
            std::printf("FAIL %s:%d  %s=%.9g vs %s=%.9g (tol %.3g)\n", __FILE__,    \
                        __LINE__, #a, check_a, #b, check_b,                         \
                        static_cast<double>(tol));                                  \
        }                                                                           \
    } while (0)

inline int testSummary(const char* name) {
    // Skipped is reported only when nonzero, so the ~120 tests that never
    // call SKIP_LINUX keep the exact summary line they have always printed.
    if (g_checksSkipped > 0) {
        std::printf("%s: %d checks, %d failed, %d skipped\n", name, g_checksRun.load(),
                    g_checksFailed.load(), g_checksSkipped.load());
    } else {
        std::printf("%s: %d checks, %d failed\n", name, g_checksRun.load(), g_checksFailed.load());
    }
    return g_checksFailed.load() != 0 ? 1 : 0;
}
