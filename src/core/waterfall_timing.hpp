// Waterfall speed and time stamps, as stand-alone logic.
//
//  (a) LinePacer: turns the spectra the pipeline produces (any rate, with a time stamp each) into waterfall LINES at a chosen line period,
//      from "every spectrum" down to one line per several minutes (24 hours across a screen: periodForSpan(24 h, rows)). The spectra inside
//      one line period are combined by their MAXIMUM or by their AVERAGE (in the power domain). Lines sit on a grid fixed to the clock,
//      T(k) = t0 + k * period in integer nanoseconds, computed by multiplication and never by "last time + period", so nothing drifts however
//      long it runs (simulated 24 h in the tests); a missing stretch of input becomes gap lines (bounded), and a clock that jumps back
//      restarts the grid instead of stalling.
//  (b) tickRows(): which rows of the waterfall get a time label at a given speed, so that labels fall on ROUND times (100 ms .. 1 day, in UTC or
//      local time), never closer together than a minimum pixel spacing, across midnight and a daylight-saving change.
//
// Time zones: the OS time-zone database is not used here (the wiring supplies offsets); Zone models UTC, a fixed offset, and the European
// and US daylight-saving rules so the DST behaviour is testable. Offsets are assumed to be whole multiples of a minute.
//
// Threads: LinePacer is single-threaded (the caller's); push() allocates nothing after configure(). The tick functions are pure.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace cascade::core::wftime {

constexpr std::int64_t kNsPerSec = 1000000000LL;

// ---- speed ----------------------------------------------------------------------------------------------------------------------------

// The line periods the speed control steps through, fastest first: 0 means "a line for every spectrum".
inline const std::vector<std::int64_t>& speedLadderNs() {
    static const std::vector<std::int64_t> v = {0,
                                                50000000LL, 100000000LL, 200000000LL, 500000000LL,
                                                1 * kNsPerSec, 2 * kNsPerSec, 5 * kNsPerSec, 10 * kNsPerSec, 30 * kNsPerSec,
                                                60 * kNsPerSec, 120 * kNsPerSec, 300 * kNsPerSec, 600 * kNsPerSec};
    return v;
}
// The period that puts `spanSeconds` of history across `rows` rows (24 h across 1000 rows is 86.4 s per line). Clamped to [1 ms, 1 h].
inline std::int64_t periodForSpan(double spanSeconds, int rows) {
    if (!(spanSeconds > 0.0) || rows < 1 || !std::isfinite(spanSeconds)) { return 0; }
    const double ns = spanSeconds * 1e9 / static_cast<double>(rows);
    return static_cast<std::int64_t>(std::llround(std::clamp(ns, 1e6, 3600e9)));
}
// A slider position 0..1 on a logarithmic scale between the fastest and slowest period (both > 0): 0 is the fastest.
inline std::int64_t periodForSlider(double x01, std::int64_t fastestNs, std::int64_t slowestNs) {
    if (fastestNs <= 0 || slowestNs < fastestNs) { return fastestNs > 0 ? fastestNs : 0; }
    const double x = std::isfinite(x01) ? std::clamp(x01, 0.0, 1.0) : 0.0;
    return std::llround(static_cast<double>(fastestNs) * std::pow(static_cast<double>(slowestNs) / static_cast<double>(fastestNs), x));
}

enum class Combine { Max, Average };

// One waterfall line leaves through the emit callback: emit(lineStartNs, db, spectraInLine). spectraInLine == 0 marks a gap line (db is
// the -200 dB floor). The line stands for the period [lineStartNs, lineStartNs + period).
class LinePacer {
public:
    struct Options {
        std::int64_t periodNs = 0;       // 0: every spectrum is its own line
        std::size_t bins = 0;
        Combine combine = Combine::Max;
        bool alignToClock = true;        // grid at multiples of the period on the clock (round times); false: grid starts at the first spectrum
        std::size_t maxGapLines = 64;    // empty lines emitted for a stretch with no input before the grid is simply jumped forward
    };
    struct Stats {
        std::uint64_t spectraIn = 0, linesOut = 0, gapLines = 0, resyncs = 0;
    };
    static constexpr float kFloorDb = -200.0f;

    // Set-up (allocates). False for zero bins or a negative period.
    bool configure(const Options& o) {
        if (o.bins == 0 || o.periodNs < 0) { return false; }
        opt_ = o;
        acc_.assign(o.bins, 0.0f);
        out_.assign(o.bins, kFloorDb);
        reset();
        return true;
    }
    void reset() {
        started_ = false;
        n_ = 0;
        emptyRun_ = 0;
        stats_ = Stats();
    }
    const Stats& stats() const { return stats_; }
    const Options& options() const { return opt_; }

    // Offer one spectrum (opt.bins dB values) with its time in nanoseconds. Returns how many lines were emitted. Non-finite bins count as the floor.
    template <class Emit>
    int push(std::int64_t tNs, const float* db, Emit&& emit) {
        if (db == nullptr || opt_.bins == 0) { return 0; }
        ++stats_.spectraIn;
        if (opt_.periodNs == 0) {
            for (std::size_t i = 0; i < opt_.bins; ++i) { out_[i] = std::isfinite(db[i]) ? db[i] : kFloorDb; }
            emit(tNs, static_cast<const float*>(out_.data()), 1u);
            ++stats_.linesOut;
            return 1;
        }
        const std::int64_t P = opt_.periodNs;
        int emitted = 0;
        if (!started_) {
            started_ = true;
            curStart_ = opt_.alignToClock ? floorTo(tNs, P) : tNs;
            n_ = 0;
        } else if (tNs < curStart_ - P) {  // the clock jumped back by more than a period: restart the grid here, drop the partial line
            curStart_ = opt_.alignToClock ? floorTo(tNs, P) : tNs;
            n_ = 0;
            emptyRun_ = 0;
            ++stats_.resyncs;
        }
        while (tNs >= curStart_ + P) {
            if (n_ > 0) {
                finishLine(emit);
                ++emitted;
                emptyRun_ = 0;
            } else if (emptyRun_ < opt_.maxGapLines) {
                for (std::size_t i = 0; i < opt_.bins; ++i) { out_[i] = kFloorDb; }
                emit(curStart_, static_cast<const float*>(out_.data()), 0u);
                ++stats_.linesOut;
                ++stats_.gapLines;
                ++emitted;
                ++emptyRun_;
            } else {  // a long silence: skip whole periods in one step, keeping the grid
                const std::int64_t skip = (tNs - curStart_) / P;
                curStart_ += skip * P;
                ++stats_.resyncs;
                emptyRun_ = 0;
                break;
            }
            if (n_ > 0) { n_ = 0; }
            curStart_ += P;
        }
        accumulate(db);
        return emitted;
    }

    // Emit the line in progress, if it holds any spectrum (end of a recording, a speed change).
    template <class Emit>
    int flush(Emit&& emit) {
        if (!started_ || n_ == 0) { return 0; }
        finishLine(emit);
        curStart_ += opt_.periodNs;
        n_ = 0;
        return 1;
    }

private:
    static std::int64_t floorTo(std::int64_t t, std::int64_t p) {
        std::int64_t r = t % p;
        if (r < 0) { r += p; }
        return t - r;
    }
    void accumulate(const float* db) {
        if (opt_.combine == Combine::Max) {
            for (std::size_t i = 0; i < opt_.bins; ++i) {
                const float v = std::isfinite(db[i]) ? db[i] : kFloorDb;
                acc_[i] = (n_ == 0) ? v : std::max(acc_[i], v);
            }
        } else {
            for (std::size_t i = 0; i < opt_.bins; ++i) {
                const float v = std::isfinite(db[i]) ? db[i] : kFloorDb;
                const float p = std::exp2(v * 0.33219280948873623f);  // 10^(v/10)
                acc_[i] = (n_ == 0) ? p : acc_[i] + p;
            }
        }
        ++n_;
    }
    template <class Emit>
    void finishLine(Emit&& emit) {
        if (opt_.combine == Combine::Max) {
            for (std::size_t i = 0; i < opt_.bins; ++i) { out_[i] = acc_[i]; }
        } else {
            const float inv = 1.0f / static_cast<float>(n_);
            for (std::size_t i = 0; i < opt_.bins; ++i) { out_[i] = 3.0102999566398120f * std::log2(std::max(acc_[i] * inv, 1e-20f)); }
        }
        emit(curStart_, static_cast<const float*>(out_.data()), static_cast<std::uint32_t>(n_));
        ++stats_.linesOut;
    }

    Options opt_;
    std::vector<float> acc_, out_;
    bool started_ = false;
    std::int64_t curStart_ = 0;
    std::uint32_t n_ = 0;
    std::size_t emptyRun_ = 0;
    Stats stats_;
};

// ---- time zones and calendar -------------------------------------------------------------------------------------------------------------

namespace detail {
inline std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}
inline void civilFromDays(std::int64_t z, std::int64_t* y, unsigned* m, unsigned* d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t yy = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yy + (*m <= 2);
}
inline std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) { --q; }
    return q;
}
inline std::int64_t floorMod(std::int64_t a, std::int64_t b) { return a - floorDiv(a, b) * b; }
// Day number (days since 1970-01-01) of the n-th (1-based; 0 = last) Sunday of a month.
inline std::int64_t nthSunday(std::int64_t year, unsigned month, int n) {
    auto weekday = [](std::int64_t days) { return static_cast<int>(floorMod(days + 4, 7)); };  // 0 = Sunday (1970-01-01 was a Thursday)
    if (n > 0) {
        const std::int64_t first = daysFromCivil(year, month, 1);
        return first + floorMod(7 - weekday(first), 7) + 7 * (n - 1);
    }
    const std::int64_t nextFirst = month == 12 ? daysFromCivil(year + 1, 1, 1) : daysFromCivil(year, month + 1, 1);
    const std::int64_t last = nextFirst - 1;
    return last - weekday(last);
}
}  // namespace detail

struct Zone {
    enum class Kind { Utc, Fixed, Eu, Us };
    Kind kind = Kind::Utc;
    int stdOffsetSec = 0;  // standard-time offset from UTC (east positive)

    static Zone utc() { return Zone{}; }
    static Zone fixed(int offsetSec) { return Zone{Kind::Fixed, offsetSec}; }
    // European Union rule: +1 h from the last Sunday of March 01:00 UTC to the last Sunday of October 01:00 UTC.
    static Zone eu(int stdOffsetSec) { return Zone{Kind::Eu, stdOffsetSec}; }
    // US rule: +1 h from the second Sunday of March 02:00 local standard time to the first Sunday of November 02:00 local daylight time.
    static Zone us(int stdOffsetSec) { return Zone{Kind::Us, stdOffsetSec}; }

    // Offset from UTC in force at a UTC instant.
    int offsetAt(std::int64_t utcSec) const {
        if (kind == Kind::Utc) { return 0; }
        if (kind == Kind::Fixed) { return stdOffsetSec; }
        std::int64_t y;
        unsigned m, d;
        detail::civilFromDays(detail::floorDiv(utcSec, 86400), &y, &m, &d);
        if (kind == Kind::Eu) {
            const std::int64_t start = detail::nthSunday(y, 3, 0) * 86400 + 3600;
            const std::int64_t end = detail::nthSunday(y, 10, 0) * 86400 + 3600;
            return (utcSec >= start && utcSec < end) ? stdOffsetSec + 3600 : stdOffsetSec;
        }
        const std::int64_t start = detail::nthSunday(y, 3, 2) * 86400 + 2 * 3600 - stdOffsetSec;
        const std::int64_t end = detail::nthSunday(y, 11, 1) * 86400 + 2 * 3600 - (stdOffsetSec + 3600);
        return (utcSec >= start && utcSec < end) ? stdOffsetSec + 3600 : stdOffsetSec;
    }
};

struct Civil {
    std::int64_t year = 1970;
    unsigned month = 1, day = 1;
    int hour = 0, minute = 0, second = 0, millis = 0;
};
inline Civil civilOf(std::int64_t utcMs, const Zone& z) {
    const std::int64_t localMs = utcMs + static_cast<std::int64_t>(z.offsetAt(detail::floorDiv(utcMs, 1000))) * 1000;
    const std::int64_t days = detail::floorDiv(localMs, 86400000LL);
    const std::int64_t msOfDay = detail::floorMod(localMs, 86400000LL);
    Civil c;
    detail::civilFromDays(days, &c.year, &c.month, &c.day);
    c.hour = static_cast<int>(msOfDay / 3600000);
    c.minute = static_cast<int>((msOfDay / 60000) % 60);
    c.second = static_cast<int>((msOfDay / 1000) % 60);
    c.millis = static_cast<int>(msOfDay % 1000);
    return c;
}

// ---- time-stamp ticks ----------------------------------------------------------------------------------------------------------------------

// Round intervals, in milliseconds.
inline const std::vector<std::int64_t>& tickLadderMs() {
    static const std::vector<std::int64_t> v = {100, 200, 500, 1000, 2000, 5000, 10000, 15000, 30000, 60000, 120000, 300000, 600000, 900000,
                                                1800000, 3600000, 7200000, 10800000, 21600000, 43200000, 86400000, 172800000};
    return v;
}

struct TickParams {
    std::int64_t newestUtcMs = 0;  // time of row 0 (the newest line, drawn at the top)
    double periodSec = 1.0;        // seconds between neighbouring rows
    int rows = 0;                  // rows on screen (row 0 .. rows-1)
    double rowPx = 1.0;            // pixels per row
    double minLabelPx = 40.0;      // labels never closer than this
    bool local = false;            // false: UTC
};

struct Tick {
    double row = 0.0;              // row position (0 = newest); fractional when the round time falls between rows
    std::int64_t utcMs = 0;
    char label[24] = {0};          // "HH:MM:SS.d", "HH:MM:SS", "HH:MM" or, at a day boundary and for day intervals, the date
    char date[16] = {0};           // "YYYY-MM-DD" of a tick at local midnight, else empty
    bool dateChange = false;       // the tick is at 00:00 (of the zone in use): draw it as a day line
};

// The interval chosen for these parameters (ms), 0 if the parameters make no sense.
inline std::int64_t tickIntervalMs(const TickParams& p) {
    if (!(p.periodSec > 0.0) || !(p.rowPx > 0.0) || !(p.minLabelPx > 0.0) || !std::isfinite(p.periodSec) || !std::isfinite(p.rowPx) ||
        !std::isfinite(p.minLabelPx) || p.rows < 1) {
        return 0;
    }
    const double msPerPx = p.periodSec * 1000.0 / p.rowPx;
    for (std::int64_t I : tickLadderMs()) {
        // Local multi-hour ticks can sit one hour closer across a daylight-saving change: keep that margin.
        const double worst = static_cast<double>((p.local && I >= 7200000) ? I - 3600000 : I);
        if (worst / msPerPx >= p.minLabelPx) { return I; }
    }
    return tickLadderMs().back();
}

inline void formatLabel(const Civil& c, std::int64_t intervalMs, bool midnight, char* label, std::size_t cap, char* date, std::size_t dcap) {
    std::snprintf(date, dcap, "%04lld-%02u-%02u", static_cast<long long>(c.year), c.month, c.day);
    if (intervalMs >= 86400000LL) {
        std::snprintf(label, cap, "%02u-%02u", c.month, c.day);
    } else if (midnight && intervalMs >= 3600000LL) {
        std::snprintf(label, cap, "%02u-%02u", c.month, c.day);
    } else if (intervalMs < 1000) {
        std::snprintf(label, cap, "%02d:%02d:%02d.%d", c.hour, c.minute, c.second, c.millis / 100);
    } else if (intervalMs < 60000) {
        std::snprintf(label, cap, "%02d:%02d:%02d", c.hour, c.minute, c.second);
    } else {
        std::snprintf(label, cap, "%02d:%02d", c.hour, c.minute);
    }
}

// Fills `out` (capacity cap) with the ticks visible in rows 0 .. rows-1, ordered from the newest (row 0) to the oldest; returns how many. Every
// tick is at a round local (or UTC) time of the interval chosen, ticks are never closer than minLabelPx (a later one that would be is
// dropped), and a repeated hour at a daylight-saving fall-back produces its ticks twice, each at its own row.
inline std::size_t tickRows(const TickParams& p, const Zone& zone, Tick* out, std::size_t cap, std::int64_t* intervalOut = nullptr) {
    const std::int64_t I = tickIntervalMs(p);
    if (intervalOut != nullptr) { *intervalOut = I; }
    if (I == 0 || out == nullptr || cap == 0) { return 0; }
    const std::int64_t spanMs = static_cast<std::int64_t>(std::llround(p.periodSec * 1000.0 * static_cast<double>(p.rows - 1)));
    const std::int64_t oldest = p.newestUtcMs - spanMs;
    // candidate step: the finest UTC grid on which every round local time lies (offsets are whole minutes; real zones are quarter hours)
    std::int64_t g;
    if (!p.local) { g = I; }
    else if (I < 60000) { g = I; }
    else if (I < 1800000) { g = 60000; }
    else { g = 900000; }
    std::int64_t t = detail::floorDiv(oldest, g) * g;
    if (t < oldest) { t += g; }
    std::vector<Tick> found;  // small: at most a few hundred ticks fit a screen
    for (; t <= p.newestUtcMs; t += g) {
        const int off = p.local ? zone.offsetAt(detail::floorDiv(t, 1000)) : 0;
        const std::int64_t localMs = t + static_cast<std::int64_t>(off) * 1000;
        if (detail::floorMod(localMs, I) != 0) { continue; }
        Tick k;
        k.utcMs = t;
        k.row = static_cast<double>(p.newestUtcMs - t) / (p.periodSec * 1000.0);
        const Civil c = civilOf(t, p.local ? zone : Zone::utc());
        const bool midnight = c.hour == 0 && c.minute == 0 && c.second == 0 && c.millis == 0;
        k.dateChange = midnight;
        formatLabel(c, I, midnight, k.label, sizeof k.label, k.date, sizeof k.date);
        if (!midnight) { k.date[0] = 0; }
        found.push_back(k);
        if (found.size() > 4000) { break; }
    }
    // newest first; drop any tick closer than minLabelPx to the one kept before it
    std::reverse(found.begin(), found.end());
    std::size_t n = 0;
    double lastRow = -1e18;
    for (const Tick& k : found) {
        if (n > 0 && (k.row - lastRow) * p.rowPx < p.minLabelPx - 1e-9) { continue; }
        if (n >= cap) { break; }
        out[n++] = k;
        lastRow = k.row;
    }
    return n;
}

}  // namespace cascade::core::wftime
