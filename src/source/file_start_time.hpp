// file_start_time.hpp - WHEN A RECORDING REALLY STARTED (engine serial lane A, row P15).
//
// A recording's own clock is not in the WAV; two places know it. FoxSDR names a take with the date and time it began ({datetime} of the naming template: YYYYMMDD_HHMMSS in the
// computer's local time), and a file's last-write time is when its recording ended, so the start is that minus the length. The name is the better witness (a copied file keeps its
// name and loses its write time), so it goes first.
//
// startFromName reads the first date and time in a base name: YYYYMMDD then one of "_", "-", " ", "T" then HHMMSS, or YYYY-MM-DD then one of those then HH-MM-SS with "-", "." or ":" between
// the parts. Years 1990 to 2100, a real month, day (checked against the month), hour, minute and second (60 is a leap second and is read as 59). The time is read as LOCAL time.
//
// Pure functions: nothing here opens a file but recordingStart's last-write query, which answers 0 (unknown) rather than throwing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace cascade::source {

namespace start_time_detail {

inline bool digits(const std::string& s, std::size_t at, std::size_t n, int& out) {
    if (at + n > s.size()) { return false; }
    int v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const char c = s[at + i];
        if (c < '0' || c > '9') { return false; }
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

inline bool isSep(char c) { return c == '_' || c == '-' || c == ' ' || c == 'T'; }
inline bool isTimeSep(char c) { return c == '-' || c == '.' || c == ':'; }

inline int daysIn(int year, int month) {
    static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return d[month - 1] + ((month == 2 && leap) ? 1 : 0);
}

inline std::optional<std::int64_t> toMs(int y, int mo, int d, int h, int mi, int sec) {
    if (y < 1990 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > daysIn(y, mo) || h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 60) { return std::nullopt; }
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = sec > 59 ? 59 : sec;
    t.tm_isdst = -1;
    const std::time_t tt = std::mktime(&t);
    if (tt == static_cast<std::time_t>(-1)) { return std::nullopt; }
    return static_cast<std::int64_t>(tt) * 1000;
}

}  // namespace start_time_detail

inline std::optional<std::int64_t> startFromName(const std::string& base) {
    using namespace start_time_detail;
    for (std::size_t i = 0; i < base.size(); ++i) {
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
        // YYYYMMDD<sep>HHMMSS
        if (digits(base, i, 8, d) && i + 15 <= base.size() && isSep(base[i + 8]) && digits(base, i + 9, 6, s)) {
            digits(base, i, 4, y);
            digits(base, i + 4, 2, mo);
            digits(base, i + 6, 2, d);
            digits(base, i + 9, 2, h);
            digits(base, i + 11, 2, mi);
            digits(base, i + 13, 2, s);
            if (const auto ms = toMs(y, mo, d, h, mi, s)) { return ms; }
        }
        // YYYY-MM-DD<sep>HH-MM-SS
        if (i + 19 <= base.size() && digits(base, i, 4, y) && base[i + 4] == '-' && digits(base, i + 5, 2, mo) && base[i + 7] == '-' && digits(base, i + 8, 2, d) && isSep(base[i + 10]) &&
            digits(base, i + 11, 2, h) && isTimeSep(base[i + 13]) && digits(base, i + 14, 2, mi) && isTimeSep(base[i + 16]) && digits(base, i + 17, 2, s)) {
            if (const auto ms = toMs(y, mo, d, h, mi, s)) { return ms; }
        }
    }
    return std::nullopt;
}

// (ms since 1970 UTC, where it came from: 1 the file name, 2 the last-write time minus the length, 0 unknown with 0 ms).
inline std::pair<std::int64_t, int> recordingStart(const std::string& path, double durationSec) {
    std::error_code ec;
    const std::filesystem::path p(path);
    if (const auto ms = startFromName(p.filename().string())) { return {*ms, 1}; }
    const auto ft = std::filesystem::last_write_time(p, ec);
    if (ec) { return {0, 0}; }
    const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
    const std::int64_t endMs = std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count();
    const std::int64_t startMs = endMs - static_cast<std::int64_t>(durationSec * 1000.0);
    return startMs > 0 ? std::pair<std::int64_t, int>{startMs, 2} : std::pair<std::int64_t, int>{0, 0};
}

}  // namespace cascade::source
