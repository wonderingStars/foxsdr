// check.hpp - the smallest test harness that does the job: every check is
// recorded and the run continues, and the process exits non-zero if any
// failed. No framework dependency, so the tests build wherever the code does.
//
// Checks compare whole values; nothing here indexes a container on the
// strength of an earlier check having passed (a failed size check followed by
// v[0] is an out-of-bounds read in exactly the run that has something to
// report).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <type_traits>

namespace check {

inline int& failures() {
    static int n = 0;
    return n;
}
inline int& passes() {
    static int n = 0;
    return n;
}

inline void record(bool ok, const char* file, int line, const std::string& what) {
    if (ok) {
        ++passes();
    } else {
        ++failures();
        std::printf("FAIL %s:%d: %s\n", file, line, what.c_str());
        std::fflush(stdout);
    }
}

template <class T>
std::string str(const T& v) {
    if constexpr (std::is_same_v<T, bool>) {
        return v ? "true" : "false";
    } else if constexpr (std::is_enum_v<T>) {
        return std::to_string(static_cast<long long>(v));
    } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
        return std::to_string(static_cast<long long>(v));
    } else if constexpr (std::is_integral_v<T>) {
        return std::to_string(static_cast<unsigned long long>(v));
    } else if constexpr (std::is_floating_point_v<T>) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.9g", static_cast<double>(v));
        return b;
    } else if constexpr (std::is_convertible_v<const T&, std::string>) {
        return "\"" + std::string(v) + "\"";
    } else {
        return "?";
    }
}

// Polls `cond` until it holds or `timeoutMs` passes. For behaviour that lands
// asynchronously (the engine applies commands on its own thread).
inline bool waitFor(const std::function<bool()>& cond, int timeoutMs) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return cond();
}

inline int finish(const char* name) {
    std::printf("%s: %d checks passed, %d failed\n", name, passes(), failures());
    std::fflush(stdout);
    return failures() == 0 ? 0 : 1;
}

}  // namespace check

#define CHECK(cond) ::check::record(static_cast<bool>(cond), __FILE__, __LINE__, #cond)
#define CHECK_EQ(a, b)                                                                        \
    do {                                                                                      \
        const auto& va_ = (a);                                                                \
        const auto& vb_ = (b);                                                                \
        const bool ok_ = (va_ == vb_);                                                        \
        ::check::record(ok_, __FILE__, __LINE__,                                              \
                        std::string(#a " == " #b "  (") + ::check::str(va_) + " vs " +        \
                            ::check::str(vb_) + ")");                                         \
    } while (0)
