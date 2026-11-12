// Tests for core/track_store.hpp: tracks kept for a period, queried as at time T with great-circle interpolation, bounded memory,
// save and load, concurrency, and a replay of a simulated hour of traffic.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/track_store.hpp"

#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

#include "test_check.hpp"

namespace tr = cascade::core::tracks;
namespace fs = std::filesystem;

static const double kNaN = std::numeric_limits<double>::quiet_NaN();
static tr::Point P(double t, double lat, double lon, double alt = kNaN) {
    tr::Point p;
    p.t = t; p.lat = lat; p.lon = lon; p.altM = alt;
    return p;
}

int main() {
    const fs::path tmp = fs::temp_directory_path() / "netmap-tracks-test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    const double T0 = 1.7e9;

    // --- exact, interpolated, held, gone dark -----------------------------------------------------------------------------------------
    {
        tr::TrackStore s;
        CHECK(s.add(tr::Kind::Aircraft, "4CA7B2", P(T0, 0, 0, 1000)) == tr::AddResult::Added);
        CHECK(s.add(tr::Kind::Aircraft, "4CA7B2", P(T0 + 10, 0, 10, 2000)) == tr::AddResult::Added);
        auto a = s.at(tr::Kind::Aircraft, "4CA7B2", T0);
        CHECK(a && !a->interpolated && !a->held && a->lon == 0 && a->altM == 1000);
        auto mid = s.at(tr::Kind::Aircraft, "4CA7B2", T0 + 5);
        CHECK(mid && mid->interpolated);
        CHECK_NEAR(mid->lat, 0, 1e-9);
        CHECK_NEAR(mid->lon, 5, 1e-9);
        CHECK_NEAR(mid->altM, 1500, 1e-9);
        CHECK_NEAR(mid->ageSec, 5, 1e-9);
        CHECK_NEAR(s.at(tr::Kind::Aircraft, "4CA7B2", T0 + 2.5)->lon, 2.5, 1e-9);
        CHECK(!s.at(tr::Kind::Aircraft, "4CA7B2", T0 - 0.001));           // not seen yet
        auto held = s.at(tr::Kind::Aircraft, "4CA7B2", T0 + 25);          // 15 s after the last point
        CHECK(held && held->held && !held->interpolated && held->lon == 10);
        CHECK(!s.at(tr::Kind::Aircraft, "4CA7B2", T0 + 41));              // 31 s after: gone dark
        CHECK(!s.at(tr::Kind::Aircraft, "NOPE", T0));
        CHECK(!s.at(tr::Kind::Ship, "4CA7B2", T0));                       // the kind is part of the identity
        CHECK(!s.at(tr::Kind::Aircraft, "4CA7B2", kNaN));
        // a long gap: held for 30 s after the earlier point, then absent, then exact at the next point
        s.add(tr::Kind::Aircraft, "GAP", P(T0, 10, 10));
        s.add(tr::Kind::Aircraft, "GAP", P(T0 + 1000, 10, 20));
        CHECK(s.at(tr::Kind::Aircraft, "GAP", T0 + 10)->held);
        CHECK(!s.at(tr::Kind::Aircraft, "GAP", T0 + 100));
        CHECK(!s.at(tr::Kind::Aircraft, "GAP", T0 + 999));
        CHECK(s.at(tr::Kind::Aircraft, "GAP", T0 + 1000)->lon == 20);
        // altitude unknown on one end: unknown between
        s.add(tr::Kind::Aircraft, "NOALT", P(T0, 1, 1, 100));
        s.add(tr::Kind::Aircraft, "NOALT", P(T0 + 10, 1, 2));
        CHECK(std::isnan(s.at(tr::Kind::Aircraft, "NOALT", T0 + 5)->altM));
    }

    // --- the great circle, not the straight line --------------------------------------------------------------------------------------
    {
        tr::TrackStore s;
        s.add(tr::Kind::Ship, "GC", P(T0, 60, 0));
        s.add(tr::Kind::Ship, "GC", P(T0 + 100, 60, 90));
        // maximum 300 s gap allowed; this one is 100 s. The midpoint of a 90-degree longitude difference at 60 N:
        // tan(lat_m) = tan(60) / cos(45)
        const double expect = std::atan(std::tan(60.0 * cascade::core::geo::kPi / 180) / std::cos(45.0 * cascade::core::geo::kPi / 180)) * 180 / cascade::core::geo::kPi;
        auto m = s.at(tr::Kind::Ship, "GC", T0 + 50);
        CHECK_NEAR(m->lat, expect, 1e-6);
        CHECK_NEAR(m->lon, 45.0, 1e-6);
        CHECK(m->lat > 66.0);   // a straight lat/lon blend would give exactly 60
        // across the antimeridian
        s.add(tr::Kind::Ship, "AM", P(T0, 0, 179));
        s.add(tr::Kind::Ship, "AM", P(T0 + 10, 0, -179));
        auto a = s.at(tr::Kind::Ship, "AM", T0 + 5);
        CHECK_NEAR(std::fabs(a->lon), 180.0, 1e-6);
        CHECK_NEAR(a->lat, 0, 1e-9);
        auto a2 = s.at(tr::Kind::Ship, "AM", T0 + 7.5);
        CHECK_NEAR(a2->lon, -179.5, 1e-6);
        // over the pole
        s.add(tr::Kind::Balloon, "POLE", P(T0, 89, 0));
        s.add(tr::Kind::Balloon, "POLE", P(T0 + 10, 89, 180));
        auto p = s.at(tr::Kind::Balloon, "POLE", T0 + 5);
        CHECK_NEAR(p->lat, 90.0, 1e-6);
        // identical points and an antipodal pair do not produce NaN
        s.add(tr::Kind::Station, "SAME", P(T0, 10, 10));
        s.add(tr::Kind::Station, "SAME", P(T0 + 10, 10, 10));
        CHECK_NEAR(s.at(tr::Kind::Station, "SAME", T0 + 5)->lat, 10, 1e-9);
        s.add(tr::Kind::Station, "ANTI", P(T0, 0, 0));
        s.add(tr::Kind::Station, "ANTI", P(T0 + 10, 0, 180));
        CHECK(std::isfinite(s.at(tr::Kind::Station, "ANTI", T0 + 5)->lon));
    }

    // --- ordering, replacement, retention ---------------------------------------------------------------------------------------------
    {
        tr::TrackStore s;
        s.add(tr::Kind::Aircraft, "A", P(T0 + 20, 0, 2));
        s.add(tr::Kind::Aircraft, "A", P(T0, 0, 0));
        s.add(tr::Kind::Aircraft, "A", P(T0 + 10, 0, 1));
        CHECK(s.add(tr::Kind::Aircraft, "A", P(T0 + 10, 0, 1.5)) == tr::AddResult::Replaced);
        CHECK(s.pointCount() == 3);
        CHECK_NEAR(s.at(tr::Kind::Aircraft, "A", T0 + 10)->lon, 1.5, 1e-12);
        CHECK_NEAR(s.at(tr::Kind::Aircraft, "A", T0 + 5)->lon, 0.75, 1e-9);
        double a = 0, b = 0;
        CHECK(s.timeRange(a, b) && a == T0 && b == T0 + 20);
        tr::TrackStore empty;
        CHECK(!empty.timeRange(a, b));

        tr::Config c;
        c.retentionSec = 600;
        tr::TrackStore r(c);
        r.add(tr::Kind::Aircraft, "R", P(T0, 0, 0));
        r.add(tr::Kind::Aircraft, "R", P(T0 + 300, 0, 1));
        CHECK(r.pointCount() == 2);
        r.add(tr::Kind::Aircraft, "R", P(T0 + 1000, 0, 2));                    // newest 1000: points before 400 go
        CHECK(r.pointCount() == 1);
        CHECK(r.add(tr::Kind::Aircraft, "R", P(T0 + 100, 0, 3)) == tr::AddResult::TooOld);
        CHECK(r.stats().tooOld == 1);
        // another track's newer point ages this one out too, after the periodic sweep (1024 adds)
        r.add(tr::Kind::Ship, "S", P(T0 + 2000, 5, 5));
        for (int i = 0; i < 1100; ++i) r.add(tr::Kind::Ship, "S", P(T0 + 2000 + i * 0.01, 5, 5));
        CHECK(r.trackCount() == 1);                                            // "R" aged out and was swept
        // lowering the retention trims at once; absurd values are clamped
        tr::TrackStore q;
        for (int i = 0; i < 100; ++i) q.add(tr::Kind::Aircraft, "Q", P(T0 + i * 60, 0, i * 0.01));
        q.setRetentionSec(1200);
        CHECK(q.pointCount() == 21);
        q.setRetentionSec(5);
        CHECK(q.config().retentionSec == 60);
        q.setRetentionSec(1e12);
        CHECK(q.config().retentionSec == 30 * 86400.0);
        q.setRetentionSec(kNaN);
        CHECK(q.config().retentionSec == 30 * 86400.0);
    }

    // --- refusals ----------------------------------------------------------------------------------------------------------------------
    {
        tr::TrackStore s;
        CHECK(s.add(tr::Kind::Aircraft, "", P(T0, 0, 0)) == tr::AddResult::BadId);
        CHECK(s.add(tr::Kind::Aircraft, std::string(25, 'A'), P(T0, 0, 0)) == tr::AddResult::BadId);
        CHECK(s.add(tr::Kind::Aircraft, "A B", P(T0, 0, 0)) == tr::AddResult::BadId);
        CHECK(s.add(tr::Kind::Aircraft, "A\nB", P(T0, 0, 0)) == tr::AddResult::BadId);
        CHECK(s.add(tr::Kind::Aircraft, "A\\B", P(T0, 0, 0)) == tr::AddResult::BadId);
        CHECK(s.add(tr::Kind::Aircraft, "\xC3\xA9", P(T0, 0, 0)) == tr::AddResult::BadId);
        CHECK(s.add(tr::Kind::Aircraft, std::string("A\0B", 3), P(T0, 0, 0)) == tr::AddResult::BadId);
        CHECK(s.add(tr::Kind::Aircraft, "W1AW/P", P(T0, 0, 0)) == tr::AddResult::Added);
        CHECK(s.add(tr::Kind::Aircraft, "X", P(kNaN, 0, 0)) == tr::AddResult::BadInput);
        CHECK(s.add(tr::Kind::Aircraft, "X", P(T0, 91, 0)) == tr::AddResult::BadInput);
        CHECK(s.add(tr::Kind::Aircraft, "X", P(T0, 0, -181)) == tr::AddResult::BadInput);
        CHECK(s.add(tr::Kind::Aircraft, "X", P(T0, kNaN, 0)) == tr::AddResult::BadInput);
        CHECK(s.add(tr::Kind::Aircraft, "X", P(T0, 0, 0, std::numeric_limits<double>::infinity())) == tr::AddResult::BadInput);
        CHECK(s.trackCount() == 1);
        CHECK(s.stats().refused == 12);   // seven bad ids and five bad inputs
        // the same id of two kinds are two tracks
        s.add(tr::Kind::Ship, "W1AW/P", P(T0, 1, 1));
        CHECK(s.trackCount() == 2);
    }

    // --- bounded memory ----------------------------------------------------------------------------------------------------------------
    {
        tr::Config c;
        c.maxPointsPerTrack = 10;
        c.maxTracks = 5;
        c.maxTotalPoints = 30;
        c.retentionSec = 86400;
        tr::TrackStore s(c);
        for (int i = 0; i < 100; ++i) s.add(tr::Kind::Aircraft, "ONE", P(T0 + i, 0, i * 0.001));
        CHECK(s.pointCount() == 10);
        CHECK(s.at(tr::Kind::Aircraft, "ONE", T0 + 99)->lon > 0.09);     // the newest survive
        CHECK(!s.at(tr::Kind::Aircraft, "ONE", T0 + 50));                // the oldest were dropped
        for (int k = 0; k < 20; ++k) s.add(tr::Kind::Ship, "T" + std::to_string(k), P(T0 + 200 + k, 1, 1));
        CHECK(s.trackCount() <= 5);
        CHECK(s.stats().tracksEvicted >= 15);
        CHECK(s.at(tr::Kind::Ship, "T19", T0 + 219).has_value());        // the latest is there
        CHECK(!s.at(tr::Kind::Ship, "T0", T0 + 200));                    // the stalest went first
        for (int k = 0; k < 5; ++k)
            for (int i = 0; i < 10; ++i) s.add(tr::Kind::Station, "X" + std::to_string(k), P(T0 + 300 + k * 10 + i, 2, 2));
        CHECK(s.pointCount() <= 30);
        // many tracks, many points: the totals hold
        tr::Config big;
        big.maxTotalPoints = 5000;
        big.maxTracks = 400;
        tr::TrackStore b(big);
        std::mt19937 rng(3);
        for (int i = 0; i < 100000; ++i) b.add(tr::Kind::Aircraft, "N" + std::to_string(rng() % 1000), P(T0 + i * 0.01, 10, 10));
        CHECK(b.pointCount() <= 5000 && b.trackCount() <= 400);
    }

    // --- snapshot and trail ------------------------------------------------------------------------------------------------------------
    {
        tr::TrackStore s;
        s.add(tr::Kind::Aircraft, "B", P(T0, 0, 0));
        s.add(tr::Kind::Aircraft, "A", P(T0, 1, 1));
        s.add(tr::Kind::Ship, "A", P(T0, 2, 2));
        s.add(tr::Kind::Balloon, "Z", P(T0 + 500, 3, 3));
        auto all = s.snapshot(T0 + 10);
        CHECK(all.size() == 3);                               // the balloon is not seen yet; the others are held (10 s)
        CHECK(all[0].kind == tr::Kind::Aircraft && all[0].id == "A");
        CHECK(all[1].id == "B" && all[2].kind == tr::Kind::Ship);
        CHECK(s.snapshot(T0 + 10, tr::Kind::Ship).size() == 1);
        CHECK(s.snapshot(T0 + 10, tr::Kind::Station).empty());
        CHECK(s.snapshot(T0 + 100).empty());                  // all gone dark
        CHECK(s.snapshot(T0 + 500).size() == 1);

        tr::TrackStore t;
        for (int i = 0; i < 1000; ++i) t.add(tr::Kind::Aircraft, "L", P(T0 + i, 0, i * 0.001));
        auto tail = t.trail(tr::Kind::Aircraft, "L", T0, T0 + 999, 50);
        CHECK(tail.size() == 50 && tail.front().t == T0 && tail.back().t == T0 + 999);
        bool mono = true;
        for (std::size_t i = 1; i < tail.size(); ++i) mono = mono && tail[i].t > tail[i - 1].t;
        CHECK(mono);
        CHECK(t.trail(tr::Kind::Aircraft, "L", T0 + 10, T0 + 19, 500).size() == 10);
        CHECK(t.trail(tr::Kind::Aircraft, "L", T0 + 2000, T0 + 3000, 500).empty());
        CHECK(t.trail(tr::Kind::Aircraft, "none", T0, T0 + 10, 500).empty());
        CHECK(t.trail(tr::Kind::Aircraft, "L", T0, T0 + 10, 1).empty());
    }

    // --- persistence -------------------------------------------------------------------------------------------------------------------
    {
        tr::TrackStore s;
        std::mt19937 rng(21);
        std::uniform_real_distribution<double> la(-80, 80), lo(-179, 179);
        for (int k = 0; k < 30; ++k)
            for (int i = 0; i < 50; ++i)
                s.add(static_cast<tr::Kind>("ASBT"[k % 4]), "ID" + std::to_string(k), P(T0 + i * 10, la(rng), lo(rng), (i % 7 == 0) ? kNaN : i * 100.0));
        const fs::path f = tmp / "tracks.txt";
        CHECK(s.save(f));
        tr::TrackStore r;
        CHECK(r.load(f) == tr::LoadResult::Ok);
        CHECK(r.trackCount() == s.trackCount() && r.pointCount() == s.pointCount());
        bool same = true;
        for (const auto& e : s.snapshot(T0 + 250)) {
            auto x = r.at(e.kind, e.id, T0 + 250);
            same = same && x && std::fabs(x->lat - e.at.lat) < 1e-6 && std::fabs(x->lon - e.at.lon) < 1e-6 &&
                   (std::isnan(x->altM) == std::isnan(e.at.altM));
        }
        CHECK(same);
        std::string text;
        { std::ifstream in(f, std::ios::binary); text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()); }
        { std::ofstream o(tmp / "t.txt", std::ios::binary); o << text.substr(0, text.size() / 3 + 5); }
        tr::TrackStore t1;
        CHECK(t1.load(tmp / "t.txt") == tr::LoadResult::Ok && t1.pointCount() > 0 && t1.pointCount() < s.pointCount());
        { std::ofstream o(tmp / "j.txt", std::ios::binary); o << "FOXSDR-TRACKS 1\nT Q BAD 1 2 3 -\nT A ok x y z -\nT A ok 1 2 3 4abc\nT A ok 1 99 3 -\nT A GOOD 1700000000 5 6 -\nrubbish\n"; }
        tr::TrackStore t2;
        CHECK(t2.load(tmp / "j.txt") == tr::LoadResult::Ok);
        CHECK(t2.pointCount() == 1 && t2.stats().loadSkippedLines == 5);
        { std::ofstream o(tmp / "v.txt", std::ios::binary); o << "FOXSDR-TRACKS 9\n"; }
        CHECK(t2.load(tmp / "v.txt") == tr::LoadResult::BadVersion);
        CHECK(t2.load(tmp / "absent.txt") == tr::LoadResult::Missing);
        { std::ofstream o(tmp / "big.txt", std::ios::binary); o << "FOXSDR-TRACKS 1\n"; o.seekp((128ll << 20) + 10); o.put('\n'); }
        CHECK(t2.load(tmp / "big.txt") == tr::LoadResult::TooLarge);
        // retention applies on load: a store with a one-hour window loads only the last hour of a longer file
        tr::TrackStore longer;
        for (int i = 0; i < 100; ++i) longer.add(tr::Kind::Aircraft, "LONG", P(T0 + i * 600, 0, i * 0.01));
        CHECK(longer.save(tmp / "long.txt"));
        tr::Config c1;
        c1.retentionSec = 3600;
        tr::TrackStore shortWin(c1);
        CHECK(shortWin.load(tmp / "long.txt") == tr::LoadResult::Ok);
        CHECK(shortWin.pointCount() == 7);
    }

    // --- concurrency --------------------------------------------------------------------------------------------------------------------
    {
        tr::TrackStore s;
        std::atomic<bool> stop{false};
        std::atomic<long> reads{0};
        std::thread reader([&] {
            while (!stop) {
                (void)s.snapshot(T0 + 500);
                (void)s.at(tr::Kind::Aircraft, "C0", T0 + 100);
                (void)s.trail(tr::Kind::Aircraft, "C1", T0, T0 + 1000, 20);
                ++reads;
            }
        });
        std::vector<std::thread> w;
        for (int k = 0; k < 4; ++k)
            w.emplace_back([&, k] {
                for (int i = 0; i < 3000; ++i) s.add(tr::Kind::Aircraft, "C" + std::to_string(k), P(T0 + i * 0.5, 10, 10 + i * 0.001));
            });
        for (auto& t : w) t.join();
        stop = true;
        reader.join();
        CHECK(s.trackCount() == 4);
        CHECK(s.stats().added == 12000);
        CHECK(reads > 0);
    }

    // --- replay of a simulated hour: 200 aircraft flying along the equator at constant speed, one point every 5 s ----------------------
    {
        tr::TrackStore s;
        const int n = 200;
        auto lonAt = [](int k, double t) { return -90.0 + k * 0.4 + 0.002 * t; };   // degrees; 0.002 deg/s
        for (int k = 0; k < n; ++k)
            for (int i = 0; i <= 720; ++i) s.add(tr::Kind::Aircraft, "F" + std::to_string(k), P(T0 + i * 5.0, 0, lonAt(k, i * 5.0), 10000));
        std::mt19937 rng(8);
        std::uniform_real_distribution<double> tt(0, 3600);
        double worst = 0;
        int interp = 0;
        for (int q = 0; q < 100; ++q) {
            const double t = tt(rng);
            const auto snap = s.snapshot(T0 + t, tr::Kind::Aircraft);
            CHECK(static_cast<int>(snap.size()) == n);
            for (const auto& e : snap) {
                const int k = std::stoi(e.id.substr(1));
                worst = std::max(worst, std::fabs(e.at.lon - lonAt(k, t)));
                worst = std::max(worst, std::fabs(e.at.lat));
                if (e.at.interpolated) ++interp;
            }
        }
        std::printf("INFO replay: 200 tracks x 721 points, 100 random times, worst position error %.3g deg, %d interpolated samples\n", worst, interp);
        CHECK(worst < 1e-9);
        CHECK(interp > 15000);
        CHECK(s.pointCount() == static_cast<std::size_t>(n * 721));
    }

    fs::remove_all(tmp);
    return testSummary("test_track_store");
}
