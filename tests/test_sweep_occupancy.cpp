// test_sweep_occupancy.cpp - the occupancy / duty-cycle model
// (core/sweep_occupancy.hpp) proven on GENERATED sweeps whose per-channel duty
// cycle is known exactly: 100 channels of 25 kHz, noise on every point, a
// gain step on every sweep (the noise-floor-relative threshold must follow it),
// a signal 30 dB above the floor on the channels that are "on".
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "core/sweep_occupancy.hpp"
#include "test_check.hpp"

using namespace cascade::core;

namespace {

constexpr std::size_t kCh = 100;
constexpr double kStart = 144.0e6;
constexpr double kStep = 25.0e3;
constexpr std::size_t kPtsPerCh = 4;

ChannelPlan plan100() {
    ChannelPlan p;
    std::string why;
    CHECK(makeChannelPlan(kStart, kStart + kCh * kStep, kStep, p, &why));
    CHECK(p.count == kCh);
    return p;
}

// One sweep: kPtsPerCh points per channel, noise floor -100 dB +- 3 dB (uniform), plus a global
// gain step; channels in `on` get a point at floor+30 dB.
SweepLine makeSweep(const std::vector<bool>& on, double gainDb, double timeS, std::mt19937& rng) {
    std::uniform_real_distribution<double> noise(-3.0, 3.0);
    SweepLine l;
    l.startHz = kStart + kStep / 8.0;
    l.stopHz = kStart + kCh * kStep - kStep / 8.0;
    l.timeS = timeS;
    for (std::size_t c = 0; c < kCh; ++c) {
        for (std::size_t k = 0; k < kPtsPerCh; ++k) {
            double v = -100.0 + noise(rng);
            if (on[c] && k == 1) { v += 30.0; }
            l.db.push_back(static_cast<float>(v + gainDb));
        }
    }
    return l;
}

OccupancyModel make(const ChannelPlan& p, int utcOffset = 0) {
    OccupancyModel m;
    OccupancyConfig c;
    c.plan = p;
    c.utcOffsetMinutes = utcOffset;
    std::string why;
    CHECK(m.configure(c, &why));
    return m;
}

}  // namespace

int main() {
    const ChannelPlan plan = plan100();

    // ---- plan refusals and shape ----
    {
        ChannelPlan p; std::string why;
        CHECK(!makeChannelPlan(10, 10, 1, p, &why) && why == "stop must be above start");
        CHECK(!makeChannelPlan(20, 10, 1, p, &why));
        CHECK(!makeChannelPlan(0, 100, 0.5, p, &why) && why == "step below 1 Hz");
        CHECK(!makeChannelPlan(0, 1e9, 1, p, &why) && why == "too many channels");
        CHECK(!makeChannelPlan(std::nan(""), 100, 1, p, &why));
        CHECK(!makeChannelPlan(-1, 100, 1, p, &why));
        CHECK(makeChannelPlan(0, 100, 30, p, &why) && p.count == 4);
        CHECK(p.channelStopHz(3) == 100.0 && p.channelStartHz(3) == 90.0);   // the last is clipped to stop
        CHECK(makeChannelPlan(0, 90, 30, p, &why) && p.count == 3);          // exact multiple: no empty channel
        CHECK(makeChannelPlan(0, static_cast<double>(kOccMaxChannels), 1, p, &why) && p.count == kOccMaxChannels);
    }

    // ---- config refusals ----
    {
        OccupancyModel m; OccupancyConfig c; std::string why;
        c.plan = plan;
        c.marginDb = -1; CHECK(!m.configure(c, &why)); c.marginDb = 10;
        c.floorPercentile = 0.9; CHECK(!m.configure(c, &why)); c.floorPercentile = 0.2;
        c.utcOffsetMinutes = 900; CHECK(!m.configure(c, &why)); c.utcOffsetMinutes = 0;
        c.maxGapS = 0; CHECK(!m.configure(c, &why)); c.maxGapS = 60;
        c.fixedFloor = true; c.fixedFloorDb = std::nan(""); CHECK(!m.configure(c, &why)); c.fixedFloor = false;
        c.minChannelsForPercentile = 0; CHECK(!m.configure(c, &why)); c.minChannelsForPercentile = 8;
        OccupancyConfig empty; CHECK(!m.configure(empty, &why));
        CHECK(m.configure(c, &why));
        std::mt19937 rng(1);
        OccupancyModel unconfigured;
        CHECK(!unconfigured.addSweep(makeSweep(std::vector<bool>(kCh, false), 0, 0, rng), &why) && why == "not configured");
    }

    // ---- proof 1: exact duty cycles, deterministic on/off, gain steps on every sweep ----
    {
        std::mt19937 rng(11);
        OccupancyModel m = make(plan);
        const struct { std::size_t ch; int dutyPct; } duty[] = {{3, 10}, {7, 50}, {12, 90}, {20, 25}, {55, 100}, {80, 1}};
        std::uniform_real_distribution<double> gain(-8.0, 8.0);
        const std::size_t sweeps = 2000;
        for (std::size_t s = 0; s < sweeps; ++s) {
            std::vector<bool> on(kCh, false);
            for (const auto& d : duty) {
                // channel is on for the first dutyPct sweeps of every hundred; offset per channel so they do not align
                on[d.ch] = ((s + d.ch * 13) % 100) < static_cast<std::size_t>(d.dutyPct);
            }
            std::string why;
            CHECK(m.addSweep(makeSweep(on, gain(rng), 1000.0 + static_cast<double>(s), rng), &why));
        }
        m.finish();
        m.finish();   // idempotent
        CHECK(m.sweepsAccepted() == sweeps);
        for (const auto& d : duty) { CHECK_NEAR(m.percent(d.ch), static_cast<double>(d.dutyPct), 1e-9); }
        for (std::size_t c = 0; c < kCh; ++c) {
            bool listed = false;
            for (const auto& d : duty) { listed = listed || d.ch == c; }
            if (!listed) { CHECK(m.percent(c) == 0.0); }   // noise alone never trips the 10 dB margin
            CHECK_NEAR(m.observedS(c), 2000.0, 1e-9);
        }
        CHECK(std::isnan(m.percent(kCh)));   // out of range
    }

    // ---- proof 2: a random (Markov) on/off signal: the model equals the generator's own count ----
    {
        std::mt19937 rng(23);
        OccupancyModel m = make(plan);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        std::vector<char> state(kCh, 0);
        std::vector<std::size_t> onCount(kCh, 0);
        const std::size_t sweeps = 5000;
        for (std::size_t s = 0; s < sweeps; ++s) {
            std::vector<bool> on(kCh, false);
            for (std::size_t c = 0; c < 20; ++c) {   // channels 0..19 are active, with different stickiness
                const double pOn = 0.02 + 0.04 * static_cast<double>(c % 5);
                const double pOff = 0.10;
                if (state[c] == 0 && u(rng) < pOn) { state[c] = 1; } else if (state[c] == 1 && u(rng) < pOff) { state[c] = 0; }
                on[c] = state[c] != 0;
                if (on[c]) { ++onCount[c]; }
            }
            CHECK(m.addSweep(makeSweep(on, 0.0, static_cast<double>(s), rng)));
        }
        m.finish();
        for (std::size_t c = 0; c < 20; ++c) {
            CHECK_NEAR(m.percent(c), 100.0 * static_cast<double>(onCount[c]) / sweeps, 1e-9);
        }
        // the generator did produce a spread of duty cycles, not a degenerate one
        CHECK(m.percent(4) > 20.0 && m.percent(0) < 40.0);
    }

    // ---- proof 3: time-of-day histogram, UTC and with a local offset, across midnight ----
    {
        std::mt19937 rng(31);
        for (int off : {0, 60, -300, 330}) {
            OccupancyModel m = make(plan, off);
            const double t0 = 1.7e9 - std::fmod(1.7e9, 86400.0);   // a UTC midnight
            for (int s = 0; s < 2880; ++s) {                        // 24 h at 30 s
                const double t = t0 + 30.0 * s;
                const int utcHour = s / 120;
                std::vector<bool> on(kCh, false);
                on[5] = utcHour >= 8 && utcHour <= 11;              // channel 5 is on 08:00-11:59 UTC
                CHECK(m.addSweep(makeSweep(on, 0.0, t, rng), nullptr));
            }
            m.finish();
            for (int h = 0; h < 24; ++h) {
                const int utc = ((h - off / 60) % 24 + 24) % 24;   // local hour h holds UTC hour utc (whole-hour offsets)
                if (off % 60 == 0) {
                    CHECK_NEAR(m.hourPercent(5, h), (utc >= 8 && utc <= 11) ? 100.0 : 0.0, 1e-9);
                    CHECK_NEAR(m.hourPercent(h), (utc >= 8 && utc <= 11) ? 1.0 : 0.0, 1e-9);   // 1 of 100 channels
                }
                CHECK_NEAR(m.observedS(5), 2880.0 * 30.0, 1e-6);
            }
            if (off == 330) {   // +5:30: UTC 08:00-11:59 is local 13:30-17:29 -> hours 13 (half) 14..16 full, 17 (half)
                CHECK_NEAR(m.hourPercent(5, 13), 100.0 * 0.5 * 120.0 / 120.0 * 1.0, 1e-9);   // half the hour is on, the rest off => 50 %
                CHECK_NEAR(m.hourPercent(5, 14), 100.0, 1e-9);
                CHECK_NEAR(m.hourPercent(5, 17), 50.0, 1e-9);
                CHECK_NEAR(m.hourPercent(5, 12), 0.0, 1e-9);
            }
        }
        // negative time and out-of-range hours
        OccupancyModel m = make(plan);
        CHECK(std::isnan(m.hourPercent(24)) && std::isnan(m.hourPercent(-1)) && std::isnan(m.hourPercent(0)));
        CHECK(m.addSweep(makeSweep(std::vector<bool>(kCh, false), 0.0, -5.0, rng)));
        CHECK(m.addSweep(makeSweep(std::vector<bool>(kCh, false), 0.0, -4.0, rng)));
        m.finish();
        CHECK(m.observedS(0) == 2.0);
        CHECK_NEAR(m.hourPercent(0, 23), 0.0, 1e-9);   // 23:59:55 UTC the day before
    }

    // ---- time weighting, the gap cap, the last sweep ----
    {
        std::mt19937 rng(41);
        OccupancyModel m = make(plan);
        std::vector<bool> on(kCh, false), off(kCh, false);
        on[2] = true;
        CHECK(m.addSweep(makeSweep(on, 0, 0.0, rng)));      // on for 30 s (until the next sweep)
        CHECK(m.addSweep(makeSweep(off, 0, 30.0, rng)));    // off for 10 s
        CHECK(m.addSweep(makeSweep(off, 0, 40.0, rng)));    // off for 3600 s of silence -> capped to 60 s
        CHECK(m.addSweep(makeSweep(on, 0, 3640.0, rng)));   // on, the last: holds for the previous interval (60 s)
        m.finish();
        CHECK_NEAR(m.observedS(2), 30.0 + 10.0 + 60.0 + 60.0, 1e-9);
        CHECK_NEAR(m.occupiedS(2), 30.0 + 60.0, 1e-9);
        CHECK_NEAR(m.percent(2), 100.0 * 90.0 / 160.0, 1e-9);
        // a single sweep holds for the nominal dwell
        OccupancyModel one = make(plan);
        CHECK(one.addSweep(makeSweep(on, 0, 5.0, rng)));
        one.finish();
        CHECK(one.observedS(2) == 1.0 && one.percent(2) == 100.0);
        // nothing at all
        OccupancyModel none = make(plan);
        none.finish();
        CHECK(std::isnan(none.percent(0)) && none.observedS(0) == 0.0);
    }

    // ---- refusals: time, lines, too few channels for a floor; fixed floor; partial coverage ----
    {
        std::mt19937 rng(51);
        OccupancyModel m = make(plan);
        std::string why;
        std::vector<bool> none(kCh, false);
        CHECK(m.addSweep(makeSweep(none, 0, 10.0, rng)));
        CHECK(!m.addSweep(makeSweep(none, 0, 10.0, rng), &why) && why == "time does not increase");
        CHECK(!m.addSweep(makeSweep(none, 0, 9.0, rng), &why));
        CHECK(m.sweepsAccepted() == 1);
        SweepLine bad = makeSweep(none, 0, 11.0, rng);
        bad.db[3] = std::numeric_limits<float>::infinity();
        CHECK(!m.addSweep(bad, &why) && why == "line refused");
        // a line covering 5 channels is too few for a percentile floor
        SweepLine small{kStart, kStart + 5 * kStep - 1.0, std::vector<float>(5, -100.f), 12.0};
        CHECK(!m.addSweep(small, &why) && why == "too few observed channels for a floor estimate");
        // ... but fine with a fixed floor, and then only the channels it covers are observed
        OccupancyModel f;
        OccupancyConfig c; c.plan = plan; c.fixedFloor = true; c.fixedFloorDb = -100.0; c.marginDb = 10.0;
        CHECK(f.configure(c));
        SweepLine s2{kStart + kStep / 2, kStart + 4.5 * kStep, {-100.f, -85.f, -105.f, -50.f, -100.f}, 0.0};
        CHECK(f.addSweep(s2));
        CHECK(f.lastThresholdDb() == -90.0);
        f.finish();
        CHECK(f.percent(0) == 0.0 && f.percent(1) == 100.0 && f.percent(3) == 100.0 && f.percent(2) == 0.0);
        CHECK(std::isnan(f.percent(7)));   // never observed
        // a sweep outside the plan observes nothing and is accepted
        SweepLine outside{300e6, 301e6, {-50.f, -50.f}, 1.0};
        OccupancyModel f2; CHECK(f2.configure(c)); CHECK(f2.addSweep(outside)); f2.finish(); CHECK(std::isnan(f2.percent(0)));
    }

    // ---- the CSV exports, literally ----
    {
        ChannelPlan p; CHECK(makeChannelPlan(1000, 1300, 100, p));
        OccupancyModel m; OccupancyConfig c; c.plan = p; c.fixedFloor = true; c.fixedFloorDb = -100; c.marginDb = 10;
        CHECK(m.configure(c));
        // 3 channels; sweeps at t = 3600*5 (hour 5): ch0 on, ch1 off, ch2 not seen; then t+10: ch0 off, ch1 on
        CHECK(m.addSweep(SweepLine{1000, 1199, {-50.f, -50.f, -100.f, -100.f}, 18000.0}));
        CHECK(m.addSweep(SweepLine{1000, 1199, {-100.f, -100.f, -50.f, -50.f}, 18010.0}));
        m.finish();
        const std::string ch = m.channelsCsv();
        CHECK(ch == "#foxsdr-occupancy,channels,1\nchannel,start_hz,stop_hz,observed_s,occupied_s,percent\n"
                    "0,1000,1100,20,10,50\n1,1100,1200,20,10,50\n2,1200,1300,0,0,\n");
        const std::string hrs = m.hoursCsv();
        CHECK(hrs.rfind("#foxsdr-occupancy,hours,1\nhour,observed_s,occupied_s,percent\n0,0,0,\n", 0) == 0);
        CHECK(hrs.find("\n5,40,20,50\n") != std::string::npos);
        const std::string chh = m.channelHoursCsv();
        CHECK(chh == "#foxsdr-occupancy,channel-hours,1\nchannel,hour,observed_s,occupied_s,percent\n0,5,20,10,50\n1,5,20,10,50\n");
    }

    // ---- fuzz: random lines never fault; percentages stay within 0..100 ----
    {
        std::mt19937 rng(77);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        OccupancyModel m = make(plan);
        double t = 0;
        for (int it = 0; it < 4000; ++it) {
            SweepLine l;
            l.startHz = kStart + (u(rng) - 0.3) * 3e6;
            l.stopHz = l.startHz + (u(rng) - 0.1) * 4e6;
            l.db.resize(rng() % 400);
            for (auto& x : l.db) { x = rng() % 6 == 0 ? std::nanf("") : static_cast<float>(-130.0 + 90.0 * u(rng)); }
            t += (rng() % 5 == 0) ? -1.0 : u(rng) * 200.0;
            l.timeS = t;
            m.addSweep(l);
        }
        m.finish();
        for (std::size_t c = 0; c < kCh; ++c) {
            const double p = m.percent(c);
            CHECK(std::isnan(p) || (p >= 0.0 && p <= 100.0));
            CHECK(m.occupiedS(c) <= m.observedS(c) + 1e-9);
        }
        for (int h = 0; h < 24; ++h) {
            const double p = m.hourPercent(h);
            CHECK(std::isnan(p) || (p >= 0.0 && p <= 100.0));
        }
    }

    return testSummary("test_sweep_occupancy");
}
