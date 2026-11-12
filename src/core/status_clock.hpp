// A UTC clock and a time-zone formatter for the status bar. Pure formatting of a Unix time in milliseconds (the caller reads the clock), a fixed or
// system UTC offset, and the delay to the next second so a timer can tick on the second without drifting.
//
// STANDALONE. Nothing here is wired (docs/feature-records/recording-models.md, Part 9). Calendar arithmetic is core/civil_time.hpp (checked against
// std::chrono for 1900..2200). No locale is used: digits are always ASCII and the layout is the same everywhere (the status bar's own translation
// handles words; this produces numbers and "UTC" / "UTC+01:00").
//
// THE SYSTEM OFFSET (systemUtcOffsetMinutes) asks the C library for the local time at that instant and subtracts: it follows daylight saving as the
// operating system's rules define it for that instant. That call is the only platform-dependent part; the rest is deterministic and tested with fixed
// offsets.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <cstdint>
#include <ctime>
#include <string>

#include "core/civil_time.hpp"

namespace cascade::core::statusclock {

struct ClockStyle {
    bool showSeconds = true;
    bool showDate = false;          // "2026-11-11 " before the time
    bool hour12 = false;            // 12-hour with "AM" / "PM" (24-hour is the radio default)
    bool showZone = true;           // " UTC" / " UTC+01:00" after the time
};

// Zone text: "UTC" for 0 when `utc`, else "UTC+HH:MM" / "UTC-HH:MM" (the offset in minutes, to the minute: India is +05:30, Nepal +05:45).
inline std::string zoneName(int offsetMinutes, bool utc) {
    if (utc || offsetMinutes == 0) { return "UTC"; }
    const int a = offsetMinutes < 0 ? -offsetMinutes : offsetMinutes;
    return std::string("UTC") + (offsetMinutes < 0 ? "-" : "+") + civil::pad(a / 60, 2) + ":" + civil::pad(a % 60, 2);
}

// Valid offsets are -14:00 .. +14:00 in whole minutes (the extremes in use are UTC-12 and UTC+14). Anything else reads as 0.
inline int sanitiseOffset(int offsetMinutes) { return (offsetMinutes < -14 * 60 || offsetMinutes > 14 * 60) ? 0 : offsetMinutes; }

// The time text for `unixMs` at `offsetMinutes` (0 for UTC).
inline std::string format(std::int64_t unixMs, int offsetMinutes, const ClockStyle& style = {}, bool utc = false) {
    const int off = sanitiseOffset(offsetMinutes);
    const civil::DateTime t = civil::fromUnixMs(unixMs, off);
    std::string s;
    if (style.showDate) { s += civil::pad(t.year, 4) + "-" + civil::pad(t.month, 2) + "-" + civil::pad(t.day, 2) + " "; }
    int h = t.hour;
    const char* suffix = "";
    if (style.hour12) { suffix = h >= 12 ? " PM" : " AM"; h = h % 12; if (h == 0) { h = 12; } }
    s += civil::pad(h, 2) + ":" + civil::pad(t.minute, 2);
    if (style.showSeconds) { s += ":" + civil::pad(t.second, 2); }
    s += suffix;
    if (style.showZone) { s += " " + zoneName(off, utc); }
    return s;
}

// Both clocks side by side for the status bar: "14:32:05 UTC" and "15:32:05 UTC+01:00".
struct DualClock { std::string utc, local; };
inline DualClock dual(std::int64_t unixMs, int localOffsetMinutes, const ClockStyle& style = {}) {
    DualClock d;
    d.utc = format(unixMs, 0, style, true);
    d.local = format(unixMs, localOffsetMinutes, style, false);
    return d;
}

// Milliseconds until the next whole second of the clock (1..1000): a timer that sleeps this long and re-reads the clock stays on the second.
inline int msToNextSecond(std::int64_t unixMs) { return 1000 - static_cast<int>(civil::floorMod(unixMs, 1000)); }

// The offset of the machine's local time zone from UTC in minutes at `unixMs` (daylight saving included), or 0 when the C library cannot say.
inline int systemUtcOffsetMinutes(std::int64_t unixMs) {
    const std::time_t secs = static_cast<std::time_t>(civil::floorDiv(unixMs, 1000));
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &secs) != 0) { return 0; }
#else
    if (localtime_r(&secs, &local) == nullptr) { return 0; }
#endif
    const std::int64_t asUtc = civil::toUnixMs(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
    const std::int64_t diffMs = asUtc - civil::floorDiv(unixMs, 1000) * 1000;
    return sanitiseOffset(static_cast<int>(diffMs / 60000));
}

}  // namespace cascade::core::statusclock
