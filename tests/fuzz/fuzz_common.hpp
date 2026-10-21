// Shared helpers for the fuzz targets in this directory.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define FUZZ_GETPID _getpid
#else
#include <unistd.h>
#define FUZZ_GETPID getpid
#endif

namespace fuzz {

// Reads a fuzz input as a stream of typed values, the way libFuzzer's own
// FuzzedDataProvider does (that header ships with Clang only, and these targets
// must also build under MSVC and be replayable on any platform). Every read
// past the end returns zero/empty, so a short input is still a valid input.
class Bytes {
public:
    Bytes(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    std::size_t remaining() const { return size_ - pos_; }

    std::uint8_t u8() { return pos_ < size_ ? data_[pos_++] : 0; }

    std::uint32_t u32() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) { v |= static_cast<std::uint32_t>(u8()) << (8 * i); }
        return v;
    }

    std::uint64_t u64() {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) { v |= static_cast<std::uint64_t>(u8()) << (8 * i); }
        return v;
    }

    // A value in [lo, hi], inclusive. hi <= lo returns lo.
    std::uint32_t inRange(std::uint32_t lo, std::uint32_t hi) {
        if (hi <= lo) { return lo; }
        const std::uint64_t span = static_cast<std::uint64_t>(hi) - lo + 1;
        return lo + static_cast<std::uint32_t>(u32() % span);
    }

    // The raw IEEE-754 pattern of a double - NaN, infinity, denormals and
    // negative zero all reachable, which is the point.
    double rawDouble() {
        const std::uint64_t bits = u64();
        double d;
        std::memcpy(&d, &bits, sizeof(d));
        return d;
    }

    float rawFloat() {
        const std::uint32_t bits = u32();
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }

    // Up to n bytes as a string (fewer if the input is shorter).
    std::string str(std::size_t n) {
        const std::size_t take = std::min(n, remaining());
        std::string s(reinterpret_cast<const char*>(data_ + pos_), take);
        pos_ += take;
        return s;
    }

    std::string rest() { return str(remaining()); }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

inline std::string asString(const std::uint8_t* data, std::size_t size) {
    return std::string(reinterpret_cast<const char*>(data), size);
}

// A scratch directory for targets whose entry point reads a FILE (config.json,
// bookmark lists, band plans, I/Q files). Named with the process id so two
// fuzzers - or a fuzzer and a replay - never share one, created beside the
// working directory like the test suite's own scratch directories, and removed
// when the process ends normally. A crash leaves it behind, which is what
// anyone chasing the crash wants.
inline const std::filesystem::path& scratchDir() {
    static const std::filesystem::path dir = [] {
        std::filesystem::path d =
            std::filesystem::path("fuzz_scratch_" + std::to_string(FUZZ_GETPID()));
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d, ec);
        std::atexit([] {
            std::error_code ec2;
            std::filesystem::remove_all("fuzz_scratch_" + std::to_string(FUZZ_GETPID()), ec2);
        });
        return d;
    }();
    return dir;
}

// Writes `data` to <scratch>/<name> (replacing it) and returns the path.
inline std::string writeScratch(const std::string& name, const std::uint8_t* data,
                                std::size_t size) {
    const std::filesystem::path p = scratchDir() / name;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (size != 0) {
        f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    f.close();
    return p.string();
}

// Prints and aborts: a property of the code under test that did not hold. Used
// instead of assert() so it fires in a Release build, where these run.
[[noreturn]] inline void fail(const char* what, const char* file, int line) {
    std::fprintf(stderr, "FUZZ PROPERTY VIOLATED: %s (%s:%d)\n", what, file, line);
    std::fflush(stderr);
    std::abort();
}

}  // namespace fuzz

#define FUZZ_REQUIRE(cond)                                        \
    do {                                                          \
        if (!(cond)) { ::fuzz::fail(#cond, __FILE__, __LINE__); } \
    } while (0)
