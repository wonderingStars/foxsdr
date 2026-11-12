// Calendar arithmetic with no time zone database and no C library calendar: days <-> year/month/day (proleptic Gregorian) and a fixed
// UTC offset applied by the caller. Pure functions, thread-safe, locale-independent. Used by the recording file names and the status-bar clock.
//
// The day-count algorithms are the well-known civil-from-days / days-from-civil pair (Howard Hinnant, "chrono-Compatible Low-Level Date
// Algorithms", from memory: UNVERIFIED as a citation, verified here by the test against std::chrono's calendar for every day of 1900..2200).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace cascade::core::civil {

struct DateTime {
    int year = 1970, month = 1, day = 1;
    int hour = 0, minute = 0, second = 0, millis = 0;
    int weekday = 4;   // 0 = Sunday .. 6 = Saturday (1970-01-01 was a Thursday)
    int dayOfYear = 1; // 1..366
};

inline std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) { --q; }
    return q;
}
inline std::int64_t floorMod(std::int64_t a, std::int64_t b) { return a - floorDiv(a, b) * b; }

inline std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = floorDiv(y, 400);
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

inline void civilFromDays(std::int64_t z, int* y, int* m, int* d) {
    z += 719468;
    const std::int64_t era = floorDiv(z, 146097);
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t yy = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    *m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    *y = static_cast<int>(yy + (*m <= 2));
}

inline bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

// Unix milliseconds (UTC) plus a fixed offset in minutes -> broken-down time.
inline DateTime fromUnixMs(std::int64_t unixMs, int offsetMinutes = 0) {
    const std::int64_t ms = unixMs + static_cast<std::int64_t>(offsetMinutes) * 60000;
    const std::int64_t days = floorDiv(ms, 86400000);
    const std::int64_t rem = floorMod(ms, 86400000);
    DateTime t;
    civilFromDays(days, &t.year, &t.month, &t.day);
    t.hour = static_cast<int>(rem / 3600000);
    t.minute = static_cast<int>((rem / 60000) % 60);
    t.second = static_cast<int>((rem / 1000) % 60);
    t.millis = static_cast<int>(rem % 1000);
    t.weekday = static_cast<int>(floorMod(days + 4, 7));
    t.dayOfYear = static_cast<int>(days - daysFromCivil(t.year, 1, 1)) + 1;
    return t;
}

// Broken-down UTC -> Unix milliseconds. No range checks beyond what the arithmetic does (month 1..12 is the caller's business).
inline std::int64_t toUnixMs(int y, int mo, int d, int h, int mi, int s, int ms = 0) {
    return daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400000 + static_cast<std::int64_t>(h) * 3600000 +
           static_cast<std::int64_t>(mi) * 60000 + static_cast<std::int64_t>(s) * 1000 + ms;
}

inline std::string pad(int v, int width) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%0*d", width, v);
    return buf;
}

// "+HHMM" / "-HHMM", or "Z" for zero when `zForZero`.
inline std::string offsetText(int offsetMinutes, bool zForZero) {
    if (offsetMinutes == 0 && zForZero) { return "Z"; }
    const int a = offsetMinutes < 0 ? -offsetMinutes : offsetMinutes;
    return std::string(offsetMinutes < 0 ? "-" : "+") + pad(a / 60, 2) + pad(a % 60, 2);
}

}  // namespace cascade::core::civil
