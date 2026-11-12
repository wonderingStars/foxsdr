// hamops_util.hpp - shared helpers for the hamops lane's tests: civil time, a deterministic generator, a source reader.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace hamops_test {

// days_from_civil (Howard Hinnant, public domain)
inline std::int64_t unixFromCivil(int y, int m, int d, int hh, int mm, int ss) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days = era * 146097 + static_cast<std::int64_t>(doe) - 719468;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

struct Rng {
    std::uint64_t s = 0x9E3779B97F4A7C15ull;
    explicit Rng(std::uint64_t seed = 1) : s(seed * 0x9E3779B97F4A7C15ull + 12345) {}
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    std::uint32_t below(std::uint32_t n) { return n == 0 ? 0 : static_cast<std::uint32_t>(next() % n); }
};

inline std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

#ifndef HAMOPS_SRC_DIR
#define HAMOPS_SRC_DIR "."
#endif
inline std::string srcFile(const std::string& rel) { return readFile(std::string(HAMOPS_SRC_DIR) + "/" + rel); }

// Removes // comments and /* */ comments and string-literal contents, so a scan looks at code only.
inline std::string stripCommentsAndStrings(const std::string& s) {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, 2, "//") == 0) { while (i < s.size() && s[i] != '\n') { ++i; } continue; }
        if (s.compare(i, 2, "/*") == 0) { i += 2; while (i + 1 < s.size() && s.compare(i, 2, "*/") != 0) { ++i; } i += 2; continue; }
        if (s[i] == '"') {
            out.push_back('"');
            ++i;
            while (i < s.size() && s[i] != '"') { if (s[i] == '\\') { ++i; } ++i; }
            out.push_back('"');
            ++i;
            continue;
        }
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

}  // namespace hamops_test
