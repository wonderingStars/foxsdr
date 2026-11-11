// qso_log.hpp - the contact record the contest and logging models share (hamops lane), and the small pure helpers around it:
// UTC calendar split, callsign character check, the Cabrillo mode code and the ADIF band for a frequency.
//
// A Qso is the user's OWN log data (their callsign, the callsign worked, the exchange). It lives in memory in the caller; nothing here writes to
// stderr/stdout, to a setting or to a report. Writers that produce a file take a path the user chose.
//
// SOURCES (opened 2026-11-11, quoted in docs/feature-records/hamops.md):
//   * Cabrillo QSO data, https://wwrof.org/cabrillo/cabrillo-qso-data/ : call = "Only A-Z, 0-9, and /"; mode codes CW, PH, FM, RY, DG;
//     date yyyy-mm-dd UTC, time nnnn UTC (0000-2359); frequency in kHz.
//   * ADIF 3.1.4, https://adif.org/314/ADIF_314.htm : the Band enumeration (band, lower and upper MHz) used by adifBand().
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cascade::core::ham {

struct Qso {
    std::int64_t unixTime = 0;        // UTC seconds since 1970-01-01
    double freqKHz = 0.0;             // kilohertz, as Cabrillo wants it
    std::string mode;                 // FoxSDR mode name: CW, USB, LSB, AM, FM, NFM, WFM, RTTY, FT8 ...
    std::string myCall;               // the logging station's call
    std::string call;                 // the station worked
    std::vector<std::string> sent;    // exchange tokens sent, in the contest's field order (see cabrillo.hpp)
    std::vector<std::string> rcvd;    // exchange tokens received, same order
    int transmitter = -1;             // Cabrillo "t" column: 0 or 1, -1 = none
};

struct UtcParts {
    int year = 1970, month = 1, day = 1, hour = 0, minute = 0, second = 0;
};

// Howard Hinnant's civil_from_days (public domain). Valid for any int64 the calendar can hold; callers bound the input.
inline UtcParts utcFromUnix(std::int64_t t) {
    std::int64_t days = t / 86400;
    std::int64_t rem = t % 86400;
    if (rem < 0) { rem += 86400; --days; }
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    std::int64_t y = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) { ++y; }
    UtcParts p;
    p.year = static_cast<int>(y);
    p.month = static_cast<int>(m);
    p.day = static_cast<int>(d);
    p.hour = static_cast<int>(rem / 3600);
    p.minute = static_cast<int>((rem % 3600) / 60);
    p.second = static_cast<int>(rem % 60);
    return p;
}

inline std::string upperAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 'a' + 'A'); }
    }
    return s;
}

// Cabrillo: "call: Only A-Z, 0-9, and /". Upper-cases first; empty or any other character is refused.
inline bool normaliseCall(const std::string& in, std::string& out) {
    out = upperAscii(in);
    if (out.empty() || out.size() > 32) { return false; }
    for (char c : out) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/';
        if (!ok) { return false; }
    }
    return true;
}

// FoxSDR mode name -> Cabrillo mode code (CW, PH, FM, RY, DG). Empty when the mode has no Cabrillo code (a broadcast mode such as WFM).
inline std::string cabrilloMode(const std::string& foxMode) {
    const std::string m = upperAscii(foxMode);
    if (m == "CW" || m == "CWR") { return "CW"; }
    if (m == "USB" || m == "LSB" || m == "SSB" || m == "AM" || m == "PH") { return "PH"; }
    if (m == "FM" || m == "NFM") { return "FM"; }
    if (m == "RTTY" || m == "RY") { return "RY"; }
    if (m == "FT8" || m == "FT4" || m == "PSK31" || m == "PSK" || m == "JT65" || m == "DIGI" || m == "DG" || m == "MFSK" || m == "OLIVIA") { return "DG"; }
    return std::string();
}

// ADIF 3.1.4 Band enumeration (opened table, MHz lower..upper). Empty when the frequency is in no listed band.
inline std::string adifBand(double mhz) {
    struct B { const char* name; double lo, hi; };
    static const B kBands[] = {
        {"160m", 1.8, 2.0},       {"80m", 3.5, 4.0},        {"60m", 5.06, 5.45},     {"40m", 7.0, 7.3},
        {"30m", 10.1, 10.15},     {"20m", 14.0, 14.35},     {"17m", 18.068, 18.168}, {"15m", 21.0, 21.45},
        {"12m", 24.89, 24.99},    {"10m", 28.0, 29.7},      {"6m", 50.0, 54.0},      {"4m", 70.0, 71.0},
        {"2m", 144.0, 148.0},     {"1.25m", 222.0, 225.0}};
    for (const B& b : kBands) {
        if (mhz >= b.lo && mhz <= b.hi) { return b.name; }
    }
    return std::string();
}

}  // namespace cascade::core::ham
