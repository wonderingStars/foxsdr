// Tests for waterfall speed and time stamps (core/waterfall_timing.hpp): the line pacer (combining by maximum or average, a grid fixed to the
// clock with no drift over a simulated 24 hours, gaps, a clock that jumps back, no allocation) and the tick placement (round times, no overlap,
// UTC and local, across midnight and daylight-saving changes in both directions).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/waterfall_timing.hpp"

#include "alloc_probe.hpp"
#include "test_check.hpp"

#include <cstdio>
#include <cstring>
#include <random>
#include <string>

using namespace cascade::core::wftime;

namespace {

constexpr std::int64_t kS = 1000000000LL;
// 2026-11-11 00:00:00 UTC in nanoseconds
constexpr std::int64_t kEpoch = 1794355200LL * kS;

std::int64_t utcMs(int y, int mo, int d, int h = 0, int mi = 0, int s = 0) {
    return (detail::daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400LL + h * 3600LL + mi * 60LL + s) * 1000LL;
}

struct Line {
    std::int64_t t;
    std::uint32_t n;
    std::vector<float> db;
};

void testSpeedHelpers() {
    const auto& L = speedLadderNs();
    CHECK(L.front() == 0);
    for (std::size_t i = 2; i < L.size(); ++i) { CHECK(L[i] > L[i - 1]); }
    CHECK(L.back() >= 600 * kS);
    CHECK(periodForSpan(86400.0, 1000) == 86400000000LL);   // 86.4 s per line
    CHECK(periodForSpan(86400.0, 1080) == 80000000000LL);
    CHECK(periodForSpan(0.0, 1000) == 0 && periodForSpan(-5.0, 1000) == 0 && periodForSpan(60.0, 0) == 0 && periodForSpan(std::nan(""), 100) == 0);
    CHECK(periodForSpan(1e-9, 1000) == 1000000LL);          // clamped to 1 ms
    CHECK(periodForSpan(1e12, 10) == 3600 * kS);            // clamped to 1 h
    // slider: logarithmic, monotone, ends exact
    CHECK(periodForSlider(0.0, 50000000LL, 600 * kS) == 50000000LL);
    CHECK(periodForSlider(1.0, 50000000LL, 600 * kS) == 600 * kS);
    CHECK(periodForSlider(7.0, 50000000LL, 600 * kS) == 600 * kS && periodForSlider(-1.0, 50000000LL, 600 * kS) == 50000000LL);
    CHECK(periodForSlider(std::nan(""), 50000000LL, 600 * kS) == 50000000LL);
    std::int64_t prev = 0;
    for (int i = 0; i <= 100; ++i) {
        const std::int64_t v = periodForSlider(i / 100.0, 50000000LL, 600 * kS);
        CHECK(v >= prev);
        prev = v;
    }
    CHECK(periodForSlider(0.5, 1 * kS, 100 * kS) == 10 * kS);
    CHECK(periodForSlider(0.5, 0, 100 * kS) == 0);
}

void testPassThroughAndCombine() {
    LinePacer p;
    std::vector<Line> got;
    auto emit = [&](std::int64_t t, const float* db, std::uint32_t n) { got.push_back({t, n, std::vector<float>(db, db + 3)}); };
    CHECK(!p.configure({0, 0}));
    CHECK(!p.configure({-5, 3}));
    LinePacer::Options o;
    o.bins = 3;
    CHECK(p.configure(o));  // period 0
    const float a[3] = {-50, -60, std::nanf("")};
    CHECK(p.push(123, a, emit) == 1);
    CHECK(got.size() == 1 && got[0].t == 123 && got[0].n == 1 && got[0].db[0] == -50 && got[0].db[2] == LinePacer::kFloorDb);
    CHECK(p.push(124, nullptr, emit) == 0);
    // maximum over a one-second line: spectra every 100 ms
    got.clear();
    o.periodNs = kS;
    o.combine = Combine::Max;
    CHECK(p.configure(o));
    for (int i = 0; i < 25; ++i) {
        const float s[3] = {-100.0f + static_cast<float>(i), -20.0f - static_cast<float>(i), -70.0f};
        p.push(kEpoch + i * (kS / 10), s, emit);
    }
    CHECK(got.size() == 2);  // two complete lines (the third is still open)
    CHECK(got[0].t == kEpoch && got[0].n == 10 && got[1].t == kEpoch + kS && got[1].n == 10);
    CHECK(got[0].db[0] == -91.0f && got[0].db[1] == -20.0f && got[0].db[2] == -70.0f);  // max over i = 0..9
    CHECK(got[1].db[0] == -81.0f && got[1].db[1] == -30.0f);
    // average in the power domain: -10 dB and -20 dB average to 10 log10((0.1 + 0.01) / 2) = -12.596 dB
    got.clear();
    o.combine = Combine::Average;
    CHECK(p.configure(o));
    const float x[3] = {-10, -10, -10}, y[3] = {-20, -20, -20};
    p.push(kEpoch + 1, x, emit);
    p.push(kEpoch + 2, y, emit);
    p.push(kEpoch + kS, x, emit);
    CHECK(got.size() == 1 && got[0].n == 2);
    CHECK_NEAR(got[0].db[0], 10.0 * std::log10(0.055), 1e-3);
    // the flush emits the partial line once
    CHECK(p.flush(emit) == 1 && got.size() == 2 && got[1].n == 1);
    CHECK(p.flush(emit) == 0);
    CHECK_NEAR(got[1].db[0], -10.0, 1e-3);
    // grid alignment on the clock: lines start at multiples of the period
    got.clear();
    o.combine = Combine::Max;
    o.periodNs = 10 * kS;
    CHECK(p.configure(o));
    p.push(kEpoch + 23 * kS + 5, x, emit);
    p.push(kEpoch + 31 * kS, x, emit);
    CHECK(got.size() == 1 && got[0].t == kEpoch + 20 * kS);
    // unaligned: the grid starts at the first spectrum
    got.clear();
    o.alignToClock = false;
    CHECK(p.configure(o));
    p.push(kEpoch + 23 * kS + 5, x, emit);
    p.push(kEpoch + 33 * kS + 5, x, emit);
    CHECK(got.size() == 1 && got[0].t == kEpoch + 23 * kS + 5);
    // a late spectrum (older than the open line's start but within a period) joins the open line
    got.clear();
    o.alignToClock = true;
    CHECK(p.configure(o));
    p.push(kEpoch + 15 * kS, x, emit);
    p.push(kEpoch + 8 * kS, y, emit);  // belongs to the previous period: counted into the open line, not a new line, not a resync
    p.push(kEpoch + 21 * kS, x, emit);
    CHECK(got.size() == 1 && got[0].n == 2 && p.stats().resyncs == 0);
}

// The grid must not drift: simulated clock, spectra at `fps` with jitter, for 24 hours.
void drift24h(std::int64_t periodNs, double fps, const char* name) {
    LinePacer p;
    LinePacer::Options o;
    o.bins = 2;
    o.periodNs = periodNs;
    o.combine = Combine::Max;
    CHECK(p.configure(o));
    std::mt19937 g(77);
    std::uniform_real_distribution<double> jit(-0.004, 0.004);  // +-4 ms of arrival jitter
    const std::int64_t t0 = kEpoch + 1234567;                    // not on any grid
    const std::int64_t span = 86400LL * kS;
    std::uint64_t lines = 0, bad = 0, spectra = 0;
    std::int64_t lastT = 0, expectFirst = -1;  // set from the first spectrum actually pushed
    std::int64_t maxDev = 0;
    std::uint64_t minN = ~0ull, maxN = 0;
    auto emit = [&](std::int64_t t, const float*, std::uint32_t n) {
        // line k must start exactly at first + k * period
        const std::int64_t want = expectFirst + static_cast<std::int64_t>(lines) * periodNs;
        maxDev = std::max(maxDev, std::llabs(t - want));
        if (t != want) { ++bad; }
        ++lines;
        lastT = t;
        if (lines > 2) { minN = std::min<std::uint64_t>(minN, n); maxN = std::max<std::uint64_t>(maxN, n); }
    };
    const float s[2] = {-40, -50};
    const double step = 1e9 / fps;
    for (std::uint64_t i = 0; static_cast<double>(i) * step < static_cast<double>(span); ++i) {
        const std::int64_t t = t0 + static_cast<std::int64_t>(static_cast<double>(i) * step + jit(g) * 1e9);
        if (expectFirst < 0) { expectFirst = t - (t % periodNs); }
        p.push(t, s, emit);
        ++spectra;
    }
    const std::int64_t expectLines = span / periodNs;
    std::printf("24 h, %s per line, %.0f spectra/s: %llu spectra, %llu lines (ideal %lld), line-time error %lld ns, last line %lld ns on the grid\n", name, fps,
                static_cast<unsigned long long>(spectra), static_cast<unsigned long long>(lines), static_cast<long long>(expectLines),
                static_cast<long long>(maxDev), static_cast<long long>((lastT - expectFirst) % periodNs));
    CHECK(bad == 0 && maxDev == 0);
    CHECK(static_cast<std::int64_t>(lines) >= expectLines - 1 && static_cast<std::int64_t>(lines) <= expectLines + 1);
    CHECK(p.stats().gapLines == 0 && p.stats().resyncs == 0);
    (void)minN;
    (void)maxN;
}

void testDrift() {
    drift24h(86400000000LL, 30.0, "86.4 s");     // 24 h across 1000 rows
    drift24h(333333333LL, 100.0, "1/3 s");       // a period that is not a whole number of milliseconds
    drift24h(60 * kS, 30.0, "1 min");
}

void testGapsAndJumps() {
    LinePacer p;
    LinePacer::Options o;
    o.bins = 2;
    o.periodNs = kS;
    o.maxGapLines = 8;
    CHECK(p.configure(o));
    std::vector<Line> got;
    auto emit = [&](std::int64_t t, const float* db, std::uint32_t n) { got.push_back({t, n, std::vector<float>(db, db + 2)}); };
    const float s[2] = {-30, -40};
    p.push(kEpoch + 100, s, emit);
    p.push(kEpoch + kS + 100, s, emit);  // line 0 complete
    CHECK(got.size() == 1);
    // silence of 5 periods then data: gap lines for the 5 periods, each flagged and at the floor
    got.clear();
    p.push(kEpoch + 7 * kS + 100, s, emit);
    CHECK(got.size() == 6);  // the open line of period 1 (one spectrum) and five empty ones for periods 2..6
    CHECK(got[0].n == 1 && got[0].t == kEpoch + kS);
    for (std::size_t i = 1; i < got.size(); ++i) { CHECK(got[i].n == 0 && got[i].db[0] == LinePacer::kFloorDb && got[i].t == kEpoch + static_cast<std::int64_t>(i + 1) * kS); }
    CHECK(p.stats().gapLines == 5);
    // a very long silence: gap lines are capped and the grid is jumped, staying on the clock grid
    got.clear();
    p.push(kEpoch + 3600 * kS + 500, s, emit);
    std::size_t gaps = 0;
    for (const Line& l : got) { if (l.n == 0) { ++gaps; } }
    std::printf("one hour of silence at 1 s per line: %zu gap lines emitted (cap 8), resyncs %llu\n", gaps, static_cast<unsigned long long>(p.stats().resyncs));
    CHECK(gaps == 8);
    CHECK(p.stats().resyncs >= 1);
    got.clear();
    p.push(kEpoch + 3601 * kS + 500, s, emit);
    CHECK(got.size() == 1 && got[0].t == kEpoch + 3600 * kS && got[0].t % kS == 0);  // back on the grid
    // the clock jumps back by an hour: restart on the new time, no flood, no crash, a resync counted
    got.clear();
    const auto before = p.stats().resyncs;
    p.push(kEpoch + 100 * kS, s, emit);
    p.push(kEpoch + 101 * kS + 1, s, emit);
    CHECK(p.stats().resyncs == before + 1);
    CHECK(got.size() == 1 && got[0].t == kEpoch + 100 * kS && got[0].n == 1);
    // negative times (before 1970) floor correctly
    LinePacer q;
    o.maxGapLines = 64;
    CHECK(q.configure(o));
    got.clear();
    q.push(-2500000000LL, s, emit);
    q.push(-1000000000LL, s, emit);
    CHECK(got.size() >= 1 && got[0].t == -3 * kS);
}

void testNoAllocation() {
    LinePacer p;
    LinePacer::Options o;
    o.bins = 1024;
    o.periodNs = kS / 2;
    o.combine = Combine::Average;
    CHECK(p.configure(o));
    std::vector<float> s(1024, -60.0f);
    long lines = 0;
    auto emit = [&](std::int64_t, const float*, std::uint32_t) { ++lines; };
    p.push(kEpoch, s.data(), emit);
    AllocProbe probe;
    for (int i = 1; i < 2000; ++i) { p.push(kEpoch + i * (kS / 40), s.data(), emit); }
    p.flush(emit);
    const long news = probe.news();
    CHECK(news == 0);
    CHECK(lines > 90);
}

// --------------------------------------------------------------------------------------------------------------------------------------

std::vector<Tick> ticksOf(const TickParams& p, const Zone& z, std::int64_t* interval = nullptr) {
    std::vector<Tick> v(4096);
    const std::size_t n = tickRows(p, z, v.data(), v.size(), interval);
    v.resize(n);
    return v;
}

void testTickBasics() {
    TickParams p;
    p.newestUtcMs = utcMs(2026, 11, 11, 12, 34, 56) + 789;
    p.periodSec = 1.0;
    p.rows = 600;
    p.rowPx = 1.0;
    p.minLabelPx = 50.0;
    std::int64_t I = 0;
    std::vector<Tick> t = ticksOf(p, Zone::utc(), &I);
    CHECK(I == 60000);  // 30 s would be 30 px: too close; 1 min is 60 px
    CHECK(t.size() >= 9 && t.size() <= 10);
    for (std::size_t i = 0; i < t.size(); ++i) {
        CHECK(t[i].utcMs % 60000 == 0);
        CHECK(std::strlen(t[i].label) == 5 && t[i].label[2] == ':');
        if (i > 0) { CHECK(t[i].row > t[i - 1].row && (t[i].row - t[i - 1].row) * p.rowPx >= p.minLabelPx); }
    }
    CHECK(std::string(t[0].label) == "12:34");
    CHECK_NEAR(t[0].row, 56.789, 1e-9);  // 56.789 s of rows between the newest line and 12:34:00
    // seconds-level interval formatting and a sub-second one
    p.periodSec = 0.1;
    p.rows = 600;
    p.rowPx = 4.0;
    p.minLabelPx = 80.0;  // 0.2 s per tick would be 8 px; need 80 px -> 2 s
    t = ticksOf(p, Zone::utc(), &I);
    CHECK(I == 2000 && t[0].label[8] == 0 && t[0].label[2] == ':' && t[0].label[5] == ':');
    p.periodSec = 0.01;
    p.rowPx = 10.0;
    p.minLabelPx = 300.0;  // 100 ms per pixel-ms: 300 ms needed -> 500 ms
    t = ticksOf(p, Zone::utc(), &I);
    CHECK(I == 500 && std::strlen(t[0].label) == 10 && t[0].label[8] == '.');
    // degenerate input: no ticks, no crash
    TickParams bad = p;
    for (int k = 0; k < 6; ++k) {
        bad = p;
        if (k == 0) { bad.rows = 0; }
        if (k == 1) { bad.periodSec = 0; }
        if (k == 2) { bad.minLabelPx = -1; }
        if (k == 3) { bad.rowPx = std::nan(""); }
        if (k == 4) { bad.periodSec = INFINITY; }
        if (k == 5) { bad.rows = -4; }
        CHECK(ticksOf(bad, Zone::utc()).empty());
    }
    Tick one[1];
    CHECK(tickRows(p, Zone::utc(), one, 0) == 0 && tickRows(p, Zone::utc(), nullptr, 5) == 0);
    CHECK(tickRows(p, Zone::utc(), one, 1) == 1);  // a short buffer is filled, not overrun
}

// For every speed of the ladder and several row heights: ticks on round times, never overlapping, monotone, within the screen.
void testTickSweep() {
    long checked = 0;
    for (std::int64_t periodNs : speedLadderNs()) {
        for (double rowPx : {1.0, 2.0, 3.0}) {
            for (double minPx : {30.0, 60.0, 120.0}) {
                for (bool local : {false, true}) {
                    TickParams p;
                    p.newestUtcMs = utcMs(2026, 11, 11, 3, 17, 29) + 411;
                    p.periodSec = periodNs == 0 ? 0.033 : static_cast<double>(periodNs) * 1e-9;
                    p.rows = 900;
                    p.rowPx = rowPx;
                    p.minLabelPx = minPx;
                    p.local = local;
                    const Zone z = Zone::fixed(5 * 3600 + 45 * 60);  // Nepal: +5:45, the awkward one
                    std::int64_t I = 0;
                    const std::vector<Tick> t = ticksOf(p, z, &I);
                    CHECK(I > 0);
                    for (std::size_t i = 0; i < t.size(); ++i) {
                        const std::int64_t localMs = t[i].utcMs + (local ? (5 * 3600 + 45 * 60) * 1000LL : 0);
                        CHECK(localMs % I == 0);
                        CHECK(t[i].row >= -1e-9 && t[i].row <= (p.rows - 1) + 1e-9);
                        if (i > 0) { CHECK((t[i].row - t[i - 1].row) * rowPx >= minPx - 1e-6); }
                        ++checked;
                    }
                }
            }
        }
    }
    std::printf("tick sweep: %ld ticks checked over 14 speeds x 3 row heights x 3 spacings x UTC/local\n", checked);
    CHECK(checked > 1000);
}

void testMidnightAndZones() {
    // across midnight UTC, 1 minute ticks: 23:55 .. 00:05
    TickParams p;
    p.newestUtcMs = utcMs(2026, 11, 12, 0, 5, 0);
    p.periodSec = 1.0;
    p.rows = 601;
    p.rowPx = 1.0;
    p.minLabelPx = 50.0;
    std::vector<Tick> t = ticksOf(p, Zone::utc());
    int mid = 0;
    bool order = true;
    for (const Tick& k : t) {
        if (k.dateChange) {
            ++mid;
            CHECK(std::string(k.label) == "00:00" && std::string(k.date) == "2026-11-12");
            CHECK_NEAR(k.row, 300.0, 1e-9);
        } else {
            CHECK(k.date[0] == 0);
        }
    }
    CHECK(mid == 1);
    CHECK(order);
    // hours: a day line shows the date, other ticks the hour
    p.newestUtcMs = utcMs(2026, 11, 12, 6, 0, 0);
    p.periodSec = 86.4;
    p.rows = 1000;
    p.rowPx = 1.0;
    p.minLabelPx = 40.0;
    std::int64_t I = 0;
    t = ticksOf(p, Zone::utc(), &I);
    CHECK(I == 3600000 * 1 || I == 7200000);  // 86.4 s * 40 px = 57.6 min -> 1 h
    CHECK(I == 3600000);
    bool sawDay = false;
    for (const Tick& k : t) {
        if (k.dateChange) {
            sawDay = true;
            CHECK(std::string(k.label) == "11-12");
        } else {
            CHECK(std::strlen(k.label) == 5 && k.label[2] == ':' && k.label[3] == '0' && k.label[4] == '0');
        }
    }
    CHECK(sawDay);
    // local midnight, fixed zone +5:45: the day line is where LOCAL time passes 00:00
    p.local = true;
    p.newestUtcMs = utcMs(2026, 11, 12, 0, 0, 0);
    t = ticksOf(p, Zone::fixed(5 * 3600 + 45 * 60));
    for (const Tick& k : t) {
        const Civil c = civilOf(k.utcMs, Zone::fixed(5 * 3600 + 45 * 60));
        CHECK(c.minute == 0 && c.second == 0);
        if (k.dateChange) { CHECK(c.hour == 0); }
    }
    CHECK(!t.empty() && t[0].utcMs % 3600000 == 15 * 60000);  // 45 minutes past... +5:45 means local :00 is UTC :15
}

void testDaylightSaving() {
    // zone rule sanity: London 2026-03-29 01:00 UTC forward, 2026-10-25 01:00 UTC back; New York 2026-03-08 and 2026-11-01
    const Zone uk = Zone::eu(0), ny = Zone::us(-5 * 3600);
    CHECK(uk.offsetAt(utcMs(2026, 3, 29, 0, 59, 59) / 1000) == 0 && uk.offsetAt(utcMs(2026, 3, 29, 1, 0, 0) / 1000) == 3600);
    CHECK(uk.offsetAt(utcMs(2026, 10, 25, 0, 59, 59) / 1000) == 3600 && uk.offsetAt(utcMs(2026, 10, 25, 1, 0, 0) / 1000) == 0);
    CHECK(ny.offsetAt(utcMs(2026, 3, 8, 6, 59, 59) / 1000) == -5 * 3600 && ny.offsetAt(utcMs(2026, 3, 8, 7, 0, 0) / 1000) == -4 * 3600);
    CHECK(ny.offsetAt(utcMs(2026, 11, 1, 5, 59, 59) / 1000) == -4 * 3600 && ny.offsetAt(utcMs(2026, 11, 1, 6, 0, 0) / 1000) == -5 * 3600);
    CHECK(Zone::eu(3600).offsetAt(utcMs(2026, 7, 1) / 1000) == 7200);  // a continental zone in summer
    // spring forward in London, hourly ticks over 00:00 UTC .. 05:00 UTC on 2026-03-29: local 00:00, 02:00 (01:00 does not exist), 03:00, ...
    TickParams p;
    p.local = true;
    p.newestUtcMs = utcMs(2026, 3, 29, 5, 0, 0);
    p.periodSec = 36.0;  // 1 h = 100 rows
    p.rows = 501;
    p.rowPx = 1.0;
    p.minLabelPx = 60.0;
    std::int64_t I = 0;
    std::vector<Tick> t = ticksOf(p, uk, &I);
    CHECK(I == 3600000);
    std::vector<std::string> labels;
    for (auto it = t.rbegin(); it != t.rend(); ++it) { labels.push_back(it->dateChange ? std::string("D") : it->label); }
    std::string joined;
    for (const auto& l : labels) { joined += l + ","; }
    std::printf("London spring forward, hourly local ticks from 00:00 UTC to 05:00 UTC: %s\n", joined.c_str());
    CHECK(joined == "D,02:00,03:00,04:00,05:00,06:00,");  // local 00:00 (midnight), no 01:00, then 02:00 (= 01:00 UTC) .. 06:00 (= 05:00 UTC)
    // fall back: 2026-10-25, UTC 22:00 .. 03:00; local 01:00 happens twice (00:00 UTC BST and 01:00 UTC GMT)
    p.newestUtcMs = utcMs(2026, 10, 25, 3, 0, 0);
    t = ticksOf(p, uk, &I);
    labels.clear();
    for (auto it = t.rbegin(); it != t.rend(); ++it) { labels.push_back(it->dateChange ? std::string("D") : it->label); }
    joined.clear();
    for (const auto& l : labels) { joined += l + ","; }
    std::printf("London fall back, hourly local ticks: %s\n", joined.c_str());
    int ones = 0;
    for (const auto& l : labels) { if (l == "01:00") { ++ones; } }
    CHECK(ones == 2);  // the repeated hour is labelled twice, at two different rows
    for (std::size_t i = 1; i < t.size(); ++i) { CHECK((t[i].row - t[i - 1].row) * p.rowPx >= p.minLabelPx - 1e-9 && t[i].row > t[i - 1].row); }
    // six-hour ticks across the spring change are 5 hours apart there: the interval choice keeps the margin and nothing overlaps
    p.local = true;
    p.periodSec = 216.0;  // 6 h = 100 rows
    p.rows = 500;
    p.minLabelPx = 90.0;
    p.newestUtcMs = utcMs(2026, 3, 29, 20, 0, 0);
    t = ticksOf(p, uk, &I);
    CHECK(I == 43200000);  // 6 h would be 5 h apart across the change (too close at 90 px): the one-hour margin moves the choice to 12 h
    for (std::size_t i = 1; i < t.size(); ++i) { CHECK((t[i].row - t[i - 1].row) * p.rowPx >= p.minLabelPx - 1e-9); }
    for (const Tick& k : t) { CHECK(detail::floorMod(k.utcMs + uk.offsetAt(k.utcMs / 1000) * 1000LL, I) == 0); }
    // the same hour-boundary behaviour in New York (US rule) across 2026-11-01
    p.periodSec = 36.0;
    p.rows = 501;
    p.minLabelPx = 60.0;
    p.newestUtcMs = utcMs(2026, 11, 1, 9, 0, 0);
    t = ticksOf(p, ny, &I);
    int twos = 0;
    for (const Tick& k : t) { if (!k.dateChange && std::string(k.label) == "01:00") { ++twos; } }
    CHECK(twos == 2);
}

void testCalendar() {
    CHECK(detail::daysFromCivil(1970, 1, 1) == 0 && detail::daysFromCivil(2000, 3, 1) == 11017);
    for (std::int64_t d = -800000; d < 900000; d += 977) {
        std::int64_t y;
        unsigned m, dd;
        detail::civilFromDays(d, &y, &m, &dd);
        CHECK(detail::daysFromCivil(y, m, dd) == d);
    }
    const Civil c = civilOf(utcMs(2026, 11, 11, 13, 14, 15) + 678, Zone::utc());
    CHECK(c.year == 2026 && c.month == 11 && c.day == 11 && c.hour == 13 && c.minute == 14 && c.second == 15 && c.millis == 678);
    const Civil n = civilOf(-1, Zone::utc());  // 1969-12-31 23:59:59.999
    CHECK(n.year == 1969 && n.month == 12 && n.day == 31 && n.hour == 23 && n.millis == 999);
    CHECK(detail::nthSunday(2026, 3, 0) == detail::daysFromCivil(2026, 3, 29));
    CHECK(detail::nthSunday(2026, 3, 2) == detail::daysFromCivil(2026, 3, 8));
    CHECK(detail::nthSunday(2026, 11, 1) == detail::daysFromCivil(2026, 11, 1));
}

}  // namespace

int main() {
    testSpeedHelpers();
    testPassThroughAndCombine();
    testDrift();
    testGapsAndJumps();
    testNoAllocation();
    testCalendar();
    testTickBasics();
    testTickSweep();
    testMidnightAndZones();
    testDaylightSaving();
    return testSummary("test_waterfall_timing");
}
