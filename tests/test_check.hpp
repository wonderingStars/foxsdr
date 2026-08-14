// Minimal dependency-free test harness: one executable per test file.
//
//   int main() {
//       CHECK(cond);
//       CHECK_NEAR(a, b, tol);
//       return testSummary("test_name");
//   }
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cmath>
#include <cstdio>

inline int g_checksFailed = 0;
inline int g_checksRun = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checksRun;                                                  \
        if (!(cond)) {                                                  \
            ++g_checksFailed;                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
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
    std::printf("%s: %d checks, %d failed\n", name, g_checksRun, g_checksFailed);
    return g_checksFailed ? 1 : 0;
}
