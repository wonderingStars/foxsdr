// Tests for core/adsb_coverage.hpp: heat grid with ageing, 24-hour range outline by bearing and altitude band, persistence,
// bounded memory, hostile input, concurrency. A synthetic day of traffic obeying the radio-horizon law proves the outline; a run over
// real decoded positions is attempted and SKIPS when the file is absent (see the record: no decoded positions exist on this machine).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/adsb_coverage.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <thread>

#include "test_check.hpp"

namespace cov = cascade::core::adsbcov;
namespace geo = cascade::core::geo;
namespace fs = std::filesystem;

static constexpr double kT0 = 1.7e9;
static const double kNaN = std::numeric_limits<double>::quiet_NaN();

static cov::Config cfgAt(double lat, double lon) {
    cov::Config c;
    c.rxLat = lat;
    c.rxLon = lon;
    return c;
}

int main() {
    const fs::path tmp = fs::temp_directory_path() / "netmap-adsbcov-test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    // --- altitude bands --------------------------------------------------------------------------------------------------------
    CHECK(cov::bandOf(0) == 0 && cov::bandOf(4999.9) == 0 && cov::bandOf(5000) == 1 && cov::bandOf(9999) == 1);
    CHECK(cov::bandOf(10000) == 2 && cov::bandOf(20000) == 3 && cov::bandOf(30000) == 4 && cov::bandOf(39999) == 4);
    CHECK(cov::bandOf(40000) == 5 && cov::bandOf(99999) == 5);
    CHECK(cov::bandOf(-500) == 0);                       // a little below sea level is still the lowest band
    CHECK(cov::bandOf(kNaN) == cov::kUnknownBand && cov::bandOf(-5000) == cov::kUnknownBand && cov::bandOf(1e6) == cov::kUnknownBand);

    // --- heat: counting, ageing, location --------------------------------------------------------------------------------------
    {
        cov::Config hc = cfgAt(51.5, -0.1);
        hc.halfLifeSec = 6 * 3600.0;   // short, so the ageing test stays inside the 24 h clock-sanity window
        cov::AdsbCoverage m(hc);
        for (int i = 0; i < 10; ++i) CHECK(m.add(kT0 + i, 52.012, 0.512, 30000) == cov::AddResult::Added);
        auto h = m.heat(kT0 + 10);
        CHECK(h.size() == 1);
        CHECK_NEAR(h[0].weight, 10.0, 0.01);
        CHECK_NEAR(h[0].lat, std::floor((52.012 + 90) / 0.05) * 0.05 - 90 + 0.025, 1e-9);   // the cell centre
        CHECK_NEAR(h[0].lon, std::floor((0.512 + 180) / 0.05) * 0.05 - 180 + 0.025, 1e-9);
        const double hl = m.config().halfLifeSec;
        CHECK_NEAR(m.heat(kT0 + 9 + hl)[0].weight, 5.0, 0.01);                  // one half-life: half
        CHECK_NEAR(m.heat(kT0 + 9 + 2 * hl)[0].weight, 2.5, 0.01);
        CHECK(m.heat(kT0 + 9 + 12 * hl).empty());                               // twelve half-lives: below the 0.05 floor
        // a new position after a half-life: old weight halves, then +1
        m.add(kT0 + 9 + hl, 52.012, 0.512, 30000);
        CHECK_NEAR(m.heat(kT0 + 9 + hl)[0].weight, 6.0, 0.01);
        // two cells, strongest first
        m.add(kT0 + 9 + hl, 53.0, 1.0, 1000);
        h = m.heat(kT0 + 9 + hl);
        CHECK(h.size() == 2 && h[0].weight >= h[1].weight);
        CHECK(m.heat(kT0 + 9 + hl, 0.05, 1).size() == 1);
        CHECK(m.heat(kT0 + 9 + hl, 100.0).empty());
    }

    // --- outline: bearing bins, bands, window -----------------------------------------------------------------------------------
    {
        cov::AdsbCoverage m(cfgAt(51.5, -0.1));
        auto put = [&](double t, double brg, double km, double alt) {
            const auto p = geo::destination(51.5, -0.1, brg, km);
            return m.add(t, p.lat, p.lon, alt);
        };
        CHECK(put(kT0, 91, 100, 35000) == cov::AddResult::Added);
        CHECK(put(kT0 + 1, 92, 250, 35000) == cov::AddResult::Added);     // same 90..95 bin, farther
        CHECK(put(kT0 + 2, 93, 80, 2000) == cov::AddResult::Added);       // same bin, low band, nearer
        CHECK(put(kT0 + 3, 271, 400, 41000) == cov::AddResult::Added);
        CHECK(put(kT0 + 4, 0.5, 60, kNaN) == cov::AddResult::Added);      // unknown altitude
        const auto all = m.outlineKm(kT0 + 10);
        CHECK(static_cast<int>(all.size()) == 72);
        CHECK_NEAR(all[18], 250.0, 0.1);                                  // bin 90..95
        CHECK_NEAR(all[54], 400.0, 0.1);                                  // bin 270..275
        CHECK_NEAR(all[0], 60.0, 0.1);
        CHECK(all[30] == 0.0);                                            // nothing there
        CHECK_NEAR(m.outlineKm(kT0 + 10, 4)[18], 250.0, 0.1);             // band 30000-39999
        CHECK_NEAR(m.outlineKm(kT0 + 10, 0)[18], 80.0, 0.1);              // band below 5000
        CHECK(m.outlineKm(kT0 + 10, 2)[18] == 0.0);
        CHECK_NEAR(m.outlineKm(kT0 + 10, cov::kUnknownBand)[0], 60.0, 0.1);
        CHECK_NEAR(m.maxRangeKm(kT0 + 10), 400.0, 0.1);
        CHECK_NEAR(m.maxRangeKm(kT0 + 10, 5), 400.0, 0.1);
        // the 24 hour window: kept at 23 h 50 min, gone at 25 h
        CHECK_NEAR(m.outlineKm(kT0 + 23.8 * 3600)[18], 250.0, 0.1);
        CHECK(m.outlineKm(kT0 + 25 * 3600)[18] == 0.0);
        CHECK(m.maxRangeKm(kT0 + 25 * 3600) == 0.0);
        // a later, shorter sighting does not lower the maximum; a later farther one raises it
        put(kT0 + 100, 92, 120, 35000);
        CHECK_NEAR(m.outlineKm(kT0 + 200)[18], 250.0, 0.1);
        put(kT0 + 101, 92, 300, 35000);
        CHECK_NEAR(m.outlineKm(kT0 + 200)[18], 300.0, 0.1);
        // when the old maximum ages out the newer, smaller one is what remains
        CHECK_NEAR(m.outlineKm(kT0 + 101 + 23.8 * 3600)[18], 300.0, 0.1);
        CHECK(m.outlineKm(kT0 + 101 + 25 * 3600)[18] == 0.0);
        // polygon: points sit at the stated range along the bin-centre bearing; empty bins collapse onto the receiver
        const auto poly = m.outlinePolygon(kT0 + 200);
        CHECK(poly.size() == 72);
        CHECK_NEAR(geo::distanceKm(51.5, -0.1, poly[18].lat, poly[18].lon), 300.0, 0.2);
        CHECK_NEAR(geo::bearingDeg(51.5, -0.1, poly[18].lat, poly[18].lon), 92.5, 0.2);
        CHECK(poly[30].lat == 51.5 && poly[30].lon == -0.1);
    }

    // --- refusals ----------------------------------------------------------------------------------------------------------------
    {
        cov::AdsbCoverage m(cfgAt(51.5, -0.1));
        CHECK(m.add(kNaN, 52, 0, 1000) == cov::AddResult::BadInput);
        CHECK(m.add(kT0, kNaN, 0, 1000) == cov::AddResult::BadInput);
        CHECK(m.add(kT0, 91, 0, 1000) == cov::AddResult::BadInput);
        CHECK(m.add(kT0, 52, 181, 1000) == cov::AddResult::BadInput);
        CHECK(m.add(kT0, 0.0, 0.0, 1000) == cov::AddResult::BadInput);        // null island
        CHECK(m.add(kT0, 52, std::numeric_limits<double>::infinity(), 1000) == cov::AddResult::BadInput);
        CHECK(m.add(kT0, 40.0, -3.7, 1000) == cov::AddResult::TooFar);        // Madrid, 1260 km from London
        CHECK(m.add(kT0, 52, 0, 1000) == cov::AddResult::Added);
        CHECK(m.add(kT0 - 2 * 86400.0, 52, 0, 1000) == cov::AddResult::Stale);
        CHECK(m.add(kT0 + 2 * 86400.0, 52, 0, 1000) == cov::AddResult::Future);
        const auto st = m.stats();
        CHECK(st.badInput == 6 && st.tooFar == 1 && st.added == 1 && st.stale == 1 && st.future == 1);
        CHECK(m.cellCount() == 1);
        // out-of-order within the day is accepted
        CHECK(m.add(kT0 - 3600, 52.5, 0.5, 1000) == cov::AddResult::Added);
        // bad configuration falls back to defaults rather than crashing
        cov::Config bad;
        bad.cellDeg = kNaN;
        bad.binDeg = 7;
        bad.halfLifeSec = -5;
        bad.maxRangeKm = 0;
        bad.maxCells = 0;
        bad.rxLat = 95;
        cov::AdsbCoverage b(bad);
        CHECK(b.binCount() == 72 && b.config().cellDeg == 0.05 && b.config().maxCells >= 16);
        CHECK(b.add(kT0, 0.1, 0.1, 100) == cov::AddResult::Added);
    }

    // --- bounded memory ----------------------------------------------------------------------------------------------------------
    {
        cov::Config c = cfgAt(51.5, -0.1);
        c.maxCells = 100;
        c.maxRangeKm = 1000;
        cov::AdsbCoverage m(c);
        std::mt19937 rng(5);
        std::uniform_real_distribution<double> d(-4.0, 4.0);
        for (int i = 0; i < 20000; ++i) m.add(kT0 + i, 51.5 + d(rng), -0.1 + d(rng), 20000);
        CHECK(m.cellCount() <= 100);
        CHECK(m.stats().cellsEvicted > 0);
        CHECK(!m.heat(kT0 + 20000).empty());
        // the most recent position's cell is present
        m.add(kT0 + 20001, 51.5, -0.1, 20000);
        bool found = false;
        for (const auto& h : m.heat(kT0 + 20001, 0.0))
            if (std::fabs(h.lat - 51.5) < 0.05 && std::fabs(h.lon + 0.1) < 0.05) found = true;
        CHECK(found);
    }

    // --- persistence -------------------------------------------------------------------------------------------------------------
    {
        cov::AdsbCoverage m(cfgAt(51.5, -0.1));
        std::mt19937 rng(11);
        std::uniform_real_distribution<double> brg(0, 360), km(5, 400);
        for (int i = 0; i < 3000; ++i) {
            const auto p = geo::destination(51.5, -0.1, brg(rng), km(rng));
            m.add(kT0 + i, p.lat, p.lon, 5000.0 * (i % 8));
        }
        const fs::path f = tmp / "cov.txt";
        CHECK(m.save(f));
        CHECK(!fs::exists(tmp / "cov.txt.tmp"));
        cov::AdsbCoverage r(cfgAt(51.5, -0.1));
        CHECK(r.load(f) == cov::LoadResult::Ok);
        const double now = kT0 + 4000;
        const auto a = m.outlineKm(now), b = r.outlineKm(now);
        bool same = a.size() == b.size();
        for (std::size_t i = 0; same && i < a.size(); ++i) same = std::fabs(a[i] - b[i]) < 0.01;
        CHECK(same);
        for (int band = 0; band < cov::kBands; ++band) {
            const auto x = m.outlineKm(now, band), y = r.outlineKm(now, band);
            bool eq = true;
            for (std::size_t i = 0; i < x.size(); ++i) eq = eq && std::fabs(x[i] - y[i]) < 0.01;
            CHECK(eq);
        }
        const auto ha = m.heat(now, 0.0, 100000), hb = r.heat(now, 0.0, 100000);
        CHECK(ha.size() == hb.size() && ha.size() > 100);
        double sa = 0, sb = 0;
        for (const auto& c : ha) sa += c.weight;
        for (const auto& c : hb) sb += c.weight;
        CHECK_NEAR(sa, sb, 0.01 * sa);
        // a file from a different receiver position keeps the heat but not the bearings
        cov::AdsbCoverage moved(cfgAt(48.85, 2.35));
        CHECK(moved.load(f) == cov::LoadResult::Ok);
        CHECK(moved.maxRangeKm(now) == 0.0);
        CHECK(moved.cellCount() == r.cellCount());
        // damaged files: truncation mid-line, garbage lines, wrong header, empty, missing, huge
        std::string text;
        { std::ifstream in(f, std::ios::binary); text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()); }
        { std::ofstream o(tmp / "trunc.txt", std::ios::binary); o << text.substr(0, text.size() / 2 + 3); }
        cov::AdsbCoverage t1(cfgAt(51.5, -0.1));
        CHECK(t1.load(tmp / "trunc.txt") == cov::LoadResult::Ok);
        CHECK(t1.cellCount() > 0 && t1.cellCount() < r.cellCount());
        { std::ofstream o(tmp / "junk.txt", std::ios::binary); o << "FOXSDR-ADSBCOV 1\nC 51.5 -0.1 0.05 5 1700000000\nH x y z\nR 99 99 1 5\nZZZ\nH 1 2 -5 3\nR 1 1 1 nan\n" << std::string("\0\xff\xfe", 3) << "\n"; }
        cov::AdsbCoverage t2(cfgAt(51.5, -0.1));
        CHECK(t2.load(tmp / "junk.txt") == cov::LoadResult::Ok);
        CHECK(t2.cellCount() == 0 && t2.stats().loadSkippedLines >= 5);
        { std::ofstream o(tmp / "ver.txt", std::ios::binary); o << "FOXSDR-ADSBCOV 2\n"; }
        cov::AdsbCoverage t3(cfgAt(51.5, -0.1));
        CHECK(t3.load(tmp / "ver.txt") == cov::LoadResult::BadVersion);
        { std::ofstream o(tmp / "empty.txt", std::ios::binary); }
        CHECK(t3.load(tmp / "empty.txt") == cov::LoadResult::BadVersion);
        CHECK(t3.load(tmp / "nothere.txt") == cov::LoadResult::Missing);
        { std::ofstream o(tmp / "huge.txt", std::ios::binary); o << "FOXSDR-ADSBCOV 1\n"; o.seekp((64ll << 20) + 10); o.put('\n'); }
        CHECK(t3.load(tmp / "huge.txt") == cov::LoadResult::TooLarge);
        CHECK(t3.cellCount() == 0);
        // an unwritable target reports false
        CHECK(!m.save(tmp / "cov.txt" / "inside.txt"));
    }

    // --- a synthetic day obeying the radio horizon -----------------------------------------------------------------------------------
    {
        const double rxLat = 51.5, rxLon = -0.1, hRx = 10.0;
        cov::AdsbCoverage m(cfgAt(rxLat, rxLon));
        std::mt19937 rng(2024);
        std::uniform_real_distribution<double> brg(0, 360), alt(0, 45000), dist(0, 700), tt(0, 86000);
        auto horizonKm = [&](double altFt) { return 4.12 * (std::sqrt(hRx) + std::sqrt(altFt * 0.3048)); };
        int accepted = 0;
        for (int i = 0; i < 300000; ++i) {
            const double a = alt(rng), d = dist(rng);
            if (d > horizonKm(a)) continue;                       // below the radio horizon: never heard
            const auto p = geo::destination(rxLat, rxLon, brg(rng), d);
            if (m.add(kT0 + tt(rng), p.lat, p.lon, a) == cov::AddResult::Added) ++accepted;
        }
        CHECK(accepted > 50000);
        const double now = kT0 + 86400;
        const double upper[] = {5000, 10000, 20000, 30000, 40000, 45000};
        double prevMax = 0;
        for (int band = 0; band < 6; ++band) {
            const auto o = m.outlineKm(now, band);
            double mx = 0;
            int covered = 0;
            for (double v : o) {
                mx = std::max(mx, v);
                if (v > 0) ++covered;
                CHECK(v <= horizonKm(upper[band]) + 0.5);          // never beyond the horizon of the band's highest altitude
            }
            std::printf("INFO synthetic day band %d: max range %.1f km (horizon of band top %.1f km), %d of 72 bins\n", band, mx,
                        horizonKm(upper[band]), covered);
            CHECK(covered >= 70);
            CHECK(mx > 0.85 * horizonKm(upper[band]) - 40.0);      // and close to it, so the outline is filled in
            CHECK(mx >= prevMax - 1.0);                            // higher bands reach farther
            prevMax = mx;
        }
    }

    // --- concurrency -------------------------------------------------------------------------------------------------------------
    {
        cov::AdsbCoverage m(cfgAt(51.5, -0.1));
        std::atomic<bool> stop{false};
        std::atomic<int> readerRuns{0};
        std::thread reader([&] {
            while (!stop) {
                (void)m.heat(kT0 + 1000);
                (void)m.outlineKm(kT0 + 1000, -1);
                (void)m.outlinePolygon(kT0 + 1000, 3);
                ++readerRuns;
            }
        });
        std::vector<std::thread> w;
        std::atomic<int> okAdds{0};
        for (int k = 0; k < 4; ++k)
            w.emplace_back([&, k] {
                std::mt19937 rng(100 + k);
                std::uniform_real_distribution<double> d(-2.0, 2.0);
                for (int i = 0; i < 5000; ++i)
                    if (m.add(kT0 + i * 0.1, 51.5 + d(rng), -0.1 + d(rng), 30000) == cov::AddResult::Added) ++okAdds;
            });
        for (auto& t : w) t.join();
        stop = true;
        reader.join();
        CHECK(okAdds == 20000);
        CHECK(m.stats().added == 20000);
        CHECK(readerRuns > 0);
    }

    // --- real decoded positions, if the local file exists ---------------------------------------------------------------------------
    {
        const fs::path real = "D:/foxsdr-samples/adsb-decoder/decoded-positions.csv";   // unix_s,lat,lon,alt_ft (not present on this machine)
        if (!fs::exists(real)) {
            std::printf("SKIP real ADS-B positions: %s is absent (the MANIFEST lists I/Q recordings only; no decoder is in this tree)\n",
                        real.string().c_str());
        } else {
            std::ifstream in(real);
            std::string line;
            cov::AdsbCoverage m(cfgAt(51.0, 0.0));   // the sample page's stated receiver position (lat 51, lon 0)
            int n = 0;
            while (std::getline(in, line)) {
                double t, la, lo, al;
                if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf", &t, &la, &lo, &al) == 4 && m.add(t, la, lo, al) == cov::AddResult::Added) ++n;
            }
            std::printf("INFO real positions added: %d\n", n);
            CHECK(n > 0);
        }
    }

    fs::remove_all(tmp);
    return testSummary("test_adsb_coverage");
}
