// aircraft_out.cpp - see aircraft_out.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/aircraft_out.hpp"

#include <cmath>

namespace cascade::net {
namespace {

constexpr std::uint8_t kEsc = 0x1A;

// ---- text writer with no allocation and no locale ----------------------------------------------------------------------

struct Writer {
    char* p;
    std::size_t cap;
    std::size_t n = 0;
    bool ok = true;
    void ch(char c) {
        if (n < cap) { p[n++] = c; } else { ok = false; }
    }
    void str(const char* s) {
        while (*s) { ch(*s++); }
    }
    void uint(std::uint64_t v, int minDigits = 1) {
        char t[24];
        int k = 0;
        do {
            t[k++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v != 0);
        while (k < minDigits) { t[k++] = '0'; }
        while (k > 0) { ch(t[--k]); }
    }
    void sint(std::int64_t v) {
        if (v < 0) {
            ch('-');
            uint(static_cast<std::uint64_t>(-(v + 1)) + 1);
        } else {
            uint(static_cast<std::uint64_t>(v));
        }
    }
    // Five decimals, rounded half away from zero; "-0.00000" is written as "0.00000".
    void fixed5(double v) {
        const long long scaled = std::llround(std::fabs(v) * 1e5);
        if (v < 0 && scaled != 0) { ch('-'); }
        uint(static_cast<std::uint64_t>(scaled / 100000));
        ch('.');
        uint(static_cast<std::uint64_t>(scaled % 100000), 5);
    }
};

enum Carries : unsigned {
    kCall = 1u << 0,
    kAlt = 1u << 1,
    kGs = 1u << 2,
    kTrk = 1u << 3,
    kPos = 1u << 4,
    kVr = 1u << 5,
    kSqk = 1u << 6,
    kAlert = 1u << 7,
    kEmerg = 1u << 8,
    kSpi = 1u << 9,
    kGnd = 1u << 10,
};

// Which fields each transmission type fills (index 1..8).
constexpr unsigned kCarries[9] = {
    0,
    kCall,                                           // 1 ES identification and category
    kAlt | kGs | kTrk | kPos | kGnd,                 // 2 ES surface position
    kAlt | kPos | kAlert | kEmerg | kSpi | kGnd,     // 3 ES airborne position
    kGs | kTrk | kVr,                                // 4 ES airborne velocity
    kAlt | kAlert | kSpi | kGnd,                     // 5 surveillance altitude
    kAlt | kSqk | kAlert | kEmerg | kSpi | kGnd,     // 6 surveillance identity
    kAlt | kGnd,                                     // 7 air-to-air
    kGnd,                                            // 8 all-call reply
};

bool upperHex6(const char* h) {
    for (int i = 0; i < 6; ++i) {
        const char c = h[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) { return false; }
    }
    return h[6] == '\0';
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void writeDate(Writer& w, const SbsTime& t) {
    w.uint(static_cast<std::uint64_t>(clampi(t.year, 0, 9999)), 4);
    w.ch('/');
    w.uint(static_cast<std::uint64_t>(clampi(t.month, 1, 12)), 2);
    w.ch('/');
    w.uint(static_cast<std::uint64_t>(clampi(t.day, 1, 31)), 2);
}
void writeTime(Writer& w, const SbsTime& t) {
    w.uint(static_cast<std::uint64_t>(clampi(t.hour, 0, 23)), 2);
    w.ch(':');
    w.uint(static_cast<std::uint64_t>(clampi(t.minute, 0, 59)), 2);
    w.ch(':');
    w.uint(static_cast<std::uint64_t>(clampi(t.second, 0, 60)), 2);
    w.ch('.');
    w.uint(static_cast<std::uint64_t>(clampi(t.millisecond, 0, 999)), 3);
}

void writeFlag(Writer& w, const std::optional<bool>& v) { w.str((v && *v) ? "-1" : "0"); }

}  // namespace

std::size_t beastEncode(const AircraftFrame& f, std::uint8_t* out, std::size_t cap) noexcept {
    char type = 0;
    switch (f.rawLen) {
        case 2: type = '1'; break;
        case 7: type = '2'; break;
        case 14: type = '3'; break;
        default: return 0;
    }
    if (cap < kBeastMaxFrameBytes) { return 0; }
    std::size_t n = 0;
    out[n++] = kEsc;  // the marker: the one 0x1A that is not doubled
    out[n++] = static_cast<std::uint8_t>(type);
    auto put = [&](std::uint8_t b) {
        out[n++] = b;
        if (b == kEsc) { out[n++] = kEsc; }
    };
    for (int shift = 40; shift >= 0; shift -= 8) { put(static_cast<std::uint8_t>(f.timestamp12MHz >> shift)); }
    put(f.level);
    for (std::size_t i = 0; i < f.rawLen; ++i) { put(f.raw[i]); }
    return n;
}

std::uint64_t beastTimestampFromSeconds(double seconds) noexcept {
    if (!std::isfinite(seconds) || seconds < 0) { return 0; }
    const double ticks = std::fmod(seconds, 281474976710656.0 / 12.0e6) * 12.0e6;  // wrap at 2^48 ticks
    return static_cast<std::uint64_t>(ticks) & 0xFFFFFFFFFFFFull;
}

std::uint8_t beastSignalFromAmplitude(double a) noexcept {
    if (!std::isfinite(a) || a <= 0) { return 0; }
    if (a >= 1.0) { return 255; }
    return static_cast<std::uint8_t>(std::lround(a * 255.0));
}

SbsTime sbsTimeFromUnixMs(std::int64_t ms) noexcept {
    // Floor division so a time before 1970 is still a valid calendar time; the date part is Hinnant's civil-from-days.
    std::int64_t secs = ms / 1000;
    std::int64_t rem = ms % 1000;
    if (rem < 0) {
        rem += 1000;
        --secs;
    }
    std::int64_t days = secs / 86400;
    std::int64_t sod = secs % 86400;
    if (sod < 0) {
        sod += 86400;
        --days;
    }
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t doe = days - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
    SbsTime t;
    t.year = static_cast<int>(y + (m <= 2 ? 1 : 0));
    t.month = static_cast<int>(m);
    t.day = static_cast<int>(d);
    t.hour = static_cast<int>(sod / 3600);
    t.minute = static_cast<int>((sod % 3600) / 60);
    t.second = static_cast<int>(sod % 60);
    t.millisecond = static_cast<int>(rem);
    return t;
}

std::size_t sbsEncode(const SbsMessage& m, char* out, std::size_t cap) noexcept {
    if (m.transmissionType < 1 || m.transmissionType > 8) { return 0; }
    if (!upperHex6(m.hexIdent)) { return 0; }
    const unsigned has = kCarries[m.transmissionType];
    Writer w{out, cap};
    w.str("MSG,");
    w.uint(static_cast<std::uint64_t>(m.transmissionType));
    w.ch(',');
    w.sint(m.sessionId);
    w.ch(',');
    w.sint(m.aircraftId);
    w.ch(',');
    w.str(m.hexIdent);
    w.ch(',');
    w.sint(m.flightId);
    w.ch(',');
    writeDate(w, m.generated);
    w.ch(',');
    writeTime(w, m.generated);
    w.ch(',');
    writeDate(w, m.generated);
    w.ch(',');
    writeTime(w, m.generated);
    w.ch(',');
    // 11 callsign
    if (has & kCall) {
        for (int i = 0; i < 8 && m.callsign[i] != '\0'; ++i) {
            const char c = m.callsign[i];
            const bool good = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
            w.ch(good ? c : '?');
        }
    }
    w.ch(',');
    // 12 altitude
    if ((has & kAlt) && m.altitudeFt) { w.sint(*m.altitudeFt); }
    w.ch(',');
    // 13 ground speed
    if ((has & kGs) && m.groundSpeedKt && *m.groundSpeedKt >= 0) { w.sint(*m.groundSpeedKt); }
    w.ch(',');
    // 14 track
    if ((has & kTrk) && m.trackDeg && std::isfinite(*m.trackDeg)) {
        long long t = std::llround(*m.trackDeg) % 360;
        if (t < 0) { t += 360; }
        w.sint(t);
    }
    w.ch(',');
    // 15, 16 latitude, longitude (both or neither)
    const bool posOk = (has & kPos) && m.latitude && m.longitude && std::isfinite(*m.latitude) && std::isfinite(*m.longitude) &&
                       std::fabs(*m.latitude) <= 90.0 && std::fabs(*m.longitude) <= 180.0;
    if (posOk) { w.fixed5(*m.latitude); }
    w.ch(',');
    if (posOk) { w.fixed5(*m.longitude); }
    w.ch(',');
    // 17 vertical rate
    if ((has & kVr) && m.verticalRateFpm) { w.sint(*m.verticalRateFpm); }
    w.ch(',');
    // 18 squawk
    if ((has & kSqk) && m.squawk) {
        const int s = *m.squawk;
        const bool octal = s >= 0 && s <= 7777 && (s % 10) <= 7 && ((s / 10) % 10) <= 7 && ((s / 100) % 10) <= 7;
        if (octal) { w.uint(static_cast<std::uint64_t>(s), 4); }
    }
    w.ch(',');
    // 19 alert, 20 emergency, 21 SPI, 22 on ground
    if (has & kAlert) { writeFlag(w, m.alert); }
    w.ch(',');
    if (has & kEmerg) { writeFlag(w, m.emergency); }
    w.ch(',');
    if (has & kSpi) { writeFlag(w, m.spi); }
    w.ch(',');
    if (has & kGnd) { writeFlag(w, m.onGround); }
    w.str("\r\n");
    return w.ok ? w.n : 0;
}

bool BeastOut::submit(const AircraftFrame& f) noexcept {
    std::uint8_t buf[kBeastMaxFrameBytes];
    const std::size_t n = beastEncode(f, buf, sizeof(buf));
    if (n == 0) { return false; }
    return fan_.publish(buf, n);
}

bool SbsOut::submit(const SbsMessage& m) noexcept {
    char buf[kSbsMaxLineBytes];
    const std::size_t n = sbsEncode(m, buf, sizeof(buf));
    if (n == 0) { return false; }
    return fan_.publish(buf, n);
}

}  // namespace cascade::net
