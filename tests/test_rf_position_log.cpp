// Tests for core/rf_position_log.hpp: GPS fixes from NMEA text (through the project's own parser) paired with level readings,
// gridded and exported. The proof is a GENERATED NMEA track: a drive past a transmitter whose level follows free-space loss.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/rf_position_log.hpp"

#include <atomic>
#include <cctype>
#include <limits>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

#include "test_check.hpp"

namespace rp = cascade::core::rfpos;
namespace geo = cascade::core::geo;
namespace fs = std::filesystem;
using cascade::core::nmeaFrame;

static std::string fmtCoord(double v, bool lat) {
    const double a = std::fabs(v);
    const int deg = static_cast<int>(a);
    const double min = (a - deg) * 60.0;
    char buf[32];
    if (lat) std::snprintf(buf, sizeof buf, "%02d%07.4f,%c", deg, min, v >= 0 ? 'N' : 'S');
    else std::snprintf(buf, sizeof buf, "%03d%07.4f,%c", deg, min, v >= 0 ? 'E' : 'W');
    return buf;
}
static std::string gga(double sod, double lat, double lon, int quality = 1) {
    const int h = static_cast<int>(sod / 3600), m = static_cast<int>(sod / 60) % 60, s = static_cast<int>(sod) % 60;
    char buf[200];
    std::snprintf(buf, sizeof buf, "GPGGA,%02d%02d%02d.00,%s,%s,%d,08,0.9,10.0,M,47.0,M,,", h, m, s, fmtCoord(lat, true).c_str(), fmtCoord(lon, false).c_str(), quality);
    return nmeaFrame(buf);
}
static std::string rmc(double sod, double lat, double lon, char status = 'A') {
    const int h = static_cast<int>(sod / 3600), m = static_cast<int>(sod / 60) % 60, s = static_cast<int>(sod) % 60;
    char buf[200];
    std::snprintf(buf, sizeof buf, "GPRMC,%02d%02d%02d.00,%c,%s,%s,19.4,90.0,111126,,,A", h, m, s, status, fmtCoord(lat, true).c_str(), fmtCoord(lon, false).c_str());
    return nmeaFrame(buf);
}

int main() {
    const fs::path tmp = fs::temp_directory_path() / "netmap-rfpos-test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    const double T0 = 1.7e9;

    // --- NMEA lines through the existing parser --------------------------------------------------------------------------------
    {
        rp::RfPositionLog l;
        CHECK(l.onNmeaLine(gga(36000, 51.5, -0.1), T0) == rp::FixResult::Accepted);
        CHECK(l.onNmeaLine(rmc(36000, 51.5, -0.1) + "\r\n", T0) == rp::FixResult::Duplicate);   // same second, same place
        CHECK(l.onNmeaLine(gga(36001, 51.5, -0.1, 0), T0 + 1) == rp::FixResult::NotAFix);          // quality 0: no fix
        CHECK(l.onNmeaLine(rmc(36001, 51.5, -0.1, 'V'), T0 + 1) == rp::FixResult::NotAFix);        // status void
        std::string bad = gga(36002, 51.5, -0.1);
        bad[20] = bad[20] == '1' ? '2' : '1';                                                         // corrupt a digit
        CHECK(l.onNmeaLine(bad, T0 + 2) == rp::FixResult::NotAFix);
        CHECK(l.onNmeaLine("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,", T0 + 2) == rp::FixResult::NotAFix);   // no checksum
        CHECK(l.onNmeaLine(nmeaFrame("GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00"), T0 + 2) == rp::FixResult::NotAFix);
        CHECK(l.onNmeaLine("", T0 + 2) == rp::FixResult::NotAFix);
        CHECK(l.onNmeaLine("garbage \x01\xff", T0 + 2) == rp::FixResult::NotAFix);
        CHECK(l.onNmeaLine(nmeaFrame("GNGGA,000000.00,,,,,0,00,99.99,,,,,,"), T0 + 2) == rp::FixResult::NotAFix);                   // empty fields
        CHECK(l.onNmeaLine(nmeaFrame("GNRMC,010203.00,A,5130.0000,N,00006.0000,W,0.0,0.0,111126,,,A"), T0 + 3) == rp::FixResult::Accepted);   // GN talker
        const auto st = l.stats();
        CHECK(st.fixesAccepted == 2 && st.lines == 11);
        CHECK(st.checksumFailures >= 2 && st.noFixSentences >= 3);
    }

    // --- a generated drive past a transmitter ------------------------------------------------------------------------------------
    {
        const double txLat = 51.5000, txLon = -0.1000;
        const double pathLat = 51.5027;                         // about 300 m north of the transmitter
        const double lon0 = -0.1200, lon1 = -0.0800;            // 2.8 km at 10 m/s = 280 s
        const double secs = 280.0;
        auto lonAt = [&](double t) { return lon0 + (lon1 - lon0) * t / secs; };
        auto levelAt = [&](double lat, double lon) {
            const double d = std::max(10.0, geo::distanceKm(txLat, txLon, lat, lon) * 1000.0);
            return -30.0 - 20.0 * std::log10(d);
        };
        rp::RfPositionLog l;
        std::mt19937 rng(77);
        std::normal_distribution<double> noise(0.0, 0.5);
        std::vector<double> truthLevel;
        int stored = 0;
        for (int s = 0; s <= 280; ++s) {
            const double t = T0 + s;
            CHECK(l.onNmeaLine(gga(36000.0 + s, pathLat, lonAt(s)), t) == rp::FixResult::Accepted);
            CHECK(l.onNmeaLine(rmc(36000.0 + s, pathLat, lonAt(s)), t) == rp::FixResult::Duplicate);
            if (s == 0) continue;
            // the second just ended is bracketed by this fix and the previous one: feed its ten readings now, as a live run would
            for (int k = 0; k < 10; ++k) {
                const double rt = (s - 1) + k / 10.0;
                const double lvl = levelAt(pathLat, lonAt(rt)) + noise(rng);
                if (l.addReading(T0 + rt, lvl, 145500000.0) == rp::ReadingResult::Stored) ++stored;
            }
        }
        CHECK(stored == 2800);
        CHECK(l.sampleCount() == 2800);
        // every stored position is within 2 m of the true place at that time (NMEA text is quantised to 0.0001 minute, about 0.2 m)
        const auto samples = l.samples();
        double worst = 0;
        for (const auto& x : samples) {
            const double rt = x.tSec - T0;
            worst = std::max(worst, geo::distanceKm(pathLat, lonAt(rt), x.lat, x.lon) * 1000.0);
        }
        std::printf("INFO generated drive: %zu samples, worst pairing error %.3f m, %zu cells\n", samples.size(), worst, l.cellCount());
        CHECK(worst < 2.0);
        // the strongest cell sits at the closest approach
        const auto cells = l.cells();
        CHECK(!cells.empty());
        const rp::Cell* best = &cells[0];
        for (const auto& c : cells)
            if (c.meanDb > best->meanDb) best = &c;
        const double offM = geo::distanceKm(pathLat, txLon, best->lat, best->lon) * 1000.0;
        std::printf("INFO strongest cell mean %.1f dB at %.0f m from the closest-approach point (cell %.0f m)\n", best->meanDb, offM, l.cellCount() ? 25.0 : 0.0);
        CHECK(offM < 60.0);
        // the mean level falls with distance from the transmitter: rings of 500 m
        std::vector<double> sum(8, 0.0);
        std::vector<int> cnt(8, 0);
        for (const auto& c : cells) {
            const double d = geo::distanceKm(txLat, txLon, c.lat, c.lon) * 1000.0;
            const int ring = std::min(7, static_cast<int>(d / 500.0));
            sum[static_cast<std::size_t>(ring)] += c.meanDb;
            ++cnt[static_cast<std::size_t>(ring)];
        }
        double prev = 1e9;
        int rings = 0;
        for (std::size_t r = 0; r < 8; ++r) {
            if (cnt[r] == 0) continue;
            const double m = sum[r] / cnt[r];
            std::printf("INFO ring %zu (%zu-%zu m): mean of cell means %.1f dB over %d cells\n", r, r * 500, r * 500 + 500, m, cnt[r]);
            CHECK(m < prev);
            prev = m;
            ++rings;
        }
        CHECK(rings >= 3);
        // cell counts add up to the samples
        std::uint64_t total = 0;
        for (const auto& c : cells) total += c.count;
        CHECK(total == 2800);
        // the cell mean of one cell agrees with an independent calculation from the raw samples
        const double cdLat = 25.0 / 111194.9, cdLon = cdLat / std::cos(geo::degToRad(samples[0].lat));
        const auto key = [&](double la, double lo) { return std::make_pair(static_cast<long long>(std::floor(la / cdLat)), static_cast<long long>(std::floor(lo / cdLon))); };
        const auto k0 = key(best->lat, best->lon);
        double lin = 0, mx = -1e9, mn = 1e9;
        int n = 0;
        for (const auto& x : samples)
            if (key(x.lat, x.lon) == k0) { lin += std::pow(10.0, x.levelDb / 10.0); mx = std::max(mx, x.levelDb); mn = std::min(mn, x.levelDb); ++n; }
        CHECK(n == static_cast<int>(best->count));
        CHECK_NEAR(best->meanDb, 10.0 * std::log10(lin / n), 1e-9);
        CHECK_NEAR(best->maxDb, mx, 1e-12);
        CHECK_NEAR(best->minDb, mn, 1e-12);
        // CSV: exports parse back to the same numbers
        const std::string csv = l.samplesCsv();
        std::istringstream in(csv);
        std::string line;
        std::getline(in, line);
        CHECK(line == "unix_s,lat,lon,level_db,freq_hz");
        int rows = 0;
        bool rowsOk = true;
        while (std::getline(in, line)) {
            double t, la, lo, lv, fr;
            if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf", &t, &la, &lo, &lv, &fr) != 5 || std::fabs(la - samples[static_cast<std::size_t>(rows)].lat) > 1e-6 ||
                std::fabs(lv - samples[static_cast<std::size_t>(rows)].levelDb) > 0.006 || fr != 145500000.0)
                rowsOk = false;
            ++rows;
        }
        CHECK(rows == 2800 && rowsOk);
        std::istringstream g(l.gridCsv());
        std::getline(g, line);
        CHECK(line == "lat,lon,count,mean_db,max_db,min_db");
        std::uint64_t gc = 0;
        int gr = 0;
        while (std::getline(g, line)) {
            double la, lo, mean, mxx, mnn;
            unsigned c;
            CHECK(std::sscanf(line.c_str(), "%lf,%lf,%u,%lf,%lf,%lf", &la, &lo, &c, &mean, &mxx, &mnn) == 6);
            gc += c;
            ++gr;
        }
        CHECK(gc == 2800 && gr == static_cast<int>(l.cellCount()));
        // only digits, signs, points, commas and newlines after the header (nothing a spreadsheet could read as a formula)
        bool plain = true;
        for (std::size_t i = csv.find('\n') + 1; i < csv.size(); ++i)
            if (!(std::isdigit(static_cast<unsigned char>(csv[i])) || csv[i] == '.' || csv[i] == ',' || csv[i] == '-' || csv[i] == '\n')) plain = false;
        CHECK(plain);
        CHECK(l.saveSamplesCsv(tmp / "samples.csv") && l.saveGridCsv(tmp / "grid.csv"));
        CHECK(fs::file_size(tmp / "samples.csv") == csv.size());
        CHECK(!fs::exists(tmp / "samples.csv.tmp"));
        CHECK(!l.saveGridCsv(tmp / "samples.csv" / "x.csv"));
    }

    // --- cell statistics in linear power ---------------------------------------------------------------------------------------------
    {
        rp::RfPositionLog l;
        l.addFix(T0, 51.5, -0.1);
        l.addFix(T0 + 10, 51.5, -0.1);
        CHECK(l.addReading(T0 + 1, -10.0) == rp::ReadingResult::Stored);
        CHECK(l.addReading(T0 + 2, -20.0) == rp::ReadingResult::Stored);
        const auto c = l.cells();
        CHECK(c.size() == 1 && c[0].count == 2);
        CHECK_NEAR(c[0].meanDb, 10.0 * std::log10((0.1 + 0.01) / 2.0), 1e-9);   // -12.596 dB, not the -15 dB mean of the dB values
        CHECK(c[0].maxDb == -10.0 && c[0].minDb == -20.0);
    }

    // --- pairing rules -----------------------------------------------------------------------------------------------------------------
    {
        rp::RfPositionLog l;
        CHECK(l.addReading(T0, -50.0) == rp::ReadingResult::NoFix);                  // no fix yet
        l.addFix(T0, 10.0, 20.0);
        l.addFix(T0 + 10, 10.0, 20.02);
        CHECK(l.addReading(T0 + 5, -50.0) == rp::ReadingResult::Stored);              // between: the midpoint
        CHECK_NEAR(l.samples().back().lon, 20.01, 1e-9);
        CHECK(l.addReading(T0 + 2.5, -50.0) == rp::ReadingResult::Stored);
        CHECK_NEAR(l.samples().back().lon, 20.005, 1e-9);
        CHECK(l.addReading(T0 + 14, -50.0) == rp::ReadingResult::Stored);             // 4 s after the last fix: held at that fix
        CHECK(l.samples().back().lon == 20.02);
        CHECK(l.addReading(T0 + 16, -50.0) == rp::ReadingResult::NoFix);              // 6 s after: too old
        CHECK(l.addReading(T0 - 4, -50.0) == rp::ReadingResult::Stored);             // 4 s before the first fix: nearest
        CHECK(l.samples().back().lon == 20.0);
        CHECK(l.addReading(T0 - 6, -50.0) == rp::ReadingResult::NoFix);
        // a gap of more than 10 s between fixes: no interpolation, nearest within 5 s only
        l.addFix(T0 + 100, 10.0, 22.0);
        CHECK(l.addReading(T0 + 50, -50.0) == rp::ReadingResult::NoFix);
        CHECK(l.addReading(T0 + 97, -50.0) == rp::ReadingResult::Stored);
        CHECK(l.samples().back().lon == 22.0);
        // across the antimeridian the midpoint goes the short way
        rp::RfPositionLog w;
        w.addFix(T0, 5.0, 179.99);
        w.addFix(T0 + 10, 5.0, -179.99);
        w.addReading(T0 + 5, -40.0);
        CHECK_NEAR(std::fabs(w.samples().back().lon), 180.0, 1e-9);
    }

    // --- refusals ------------------------------------------------------------------------------------------------------------------------
    {
        rp::RfPositionLog l;
        CHECK(l.addFix(T0, 51.5, -0.1) == rp::FixResult::Accepted);
        CHECK(l.addFix(T0 + 1, 52.5, -0.1) == rp::FixResult::Jump);                   // 111 km in a second
        CHECK(l.addFix(T0 - 5, 51.5, -0.1) == rp::FixResult::OutOfOrder);
        CHECK(l.addFix(T0 + 0.0004, 51.5, -0.1) == rp::FixResult::Duplicate);
        CHECK(l.addFix(T0 + 1, 0.0, 0.0) == rp::FixResult::BadPosition);
        CHECK(l.addFix(T0 + 1, 91, 0) == rp::FixResult::BadPosition);
        CHECK(l.addFix(T0 + 1, std::nan(""), 0) == rp::FixResult::BadPosition);
        CHECK(l.addFix(std::nan(""), 51.5, 0) == rp::FixResult::BadPosition);
        CHECK(l.addFix(T0 + 2, 51.5001, -0.1) == rp::FixResult::Accepted);              // 11 m in 2 s: fine
        CHECK(l.addFix(T0 + 3600, 55.0, 5.0) == rp::FixResult::Accepted);               // after a long gap a move is believable
        CHECK(l.addReading(std::nan(""), -50) == rp::ReadingResult::BadReading);
        CHECK(l.addReading(T0, std::nan("")) == rp::ReadingResult::BadReading);
        CHECK(l.addReading(T0, -300.0) == rp::ReadingResult::BadReading);
        CHECK(l.addReading(T0, 150.0) == rp::ReadingResult::BadReading);
        CHECK(l.addReading(T0, -50.0, -1.0) == rp::ReadingResult::BadReading);
        CHECK(l.addReading(T0, -50.0, std::numeric_limits<double>::infinity()) == rp::ReadingResult::BadReading);
        CHECK(l.stats().readingsBad == 6 && l.stats().fixesRefused == 6);
        l.clear();
        CHECK(l.cellCount() == 0 && l.sampleCount() == 0 && l.addReading(T0, -50) == rp::ReadingResult::NoFix);
        // nonsense configuration is repaired
        rp::Config c;
        c.cellMeters = -1;
        c.maxSamples = 0;
        c.maxCells = 0;
        c.maxSpeedMps = std::nan("");
        rp::RfPositionLog r(c);
        r.addFix(T0, 10, 10);
        CHECK(r.addReading(T0, -50) == rp::ReadingResult::Stored);
    }

    // --- bounds ---------------------------------------------------------------------------------------------------------------------------
    {
        rp::Config c;
        c.maxSamples = 100;
        c.maxCells = 50;
        c.cellMeters = 10;
        rp::RfPositionLog l(c);
        l.addFix(T0, 51.5, -0.1);
        l.addFix(T0 + 5, 51.5, -0.1);
        int limited = 0;
        for (int i = 0; i < 1000; ++i) {
            // spread over many cells by moving the fix chain: add a fix every second moving east at 10 m/s
            l.addFix(T0 + 5 + i, 51.5, -0.1 + i * 0.00014);
            if (l.addReading(T0 + 5 + i, -60.0) == rp::ReadingResult::CellLimit) ++limited;
        }
        CHECK(l.sampleCount() <= 100);
        CHECK(l.cellCount() == 50);
        CHECK(limited > 0);
        rp::Config c2;
        c2.maxSamples = 100;
        rp::RfPositionLog m(c2);
        m.addFix(T0, 51.5, -0.1);
        for (int i = 0; i < 1000; ++i) { m.addFix(T0 + i, 51.5, -0.1); m.addReading(T0 + i, -60.0 - (i % 5)); }
        CHECK(m.sampleCount() == 100 && m.stats().samplesDropped == 900);
        CHECK(m.cells()[0].count == 1000);   // the grid keeps its totals; only the raw sample list is bounded
    }

    // --- concurrency ----------------------------------------------------------------------------------------------------------------------
    {
        rp::RfPositionLog l;
        std::atomic<bool> stop{false};
        std::thread reader([&] {
            while (!stop) { (void)l.cells(); (void)l.samplesCsv(); }
        });
        std::thread fixes([&] {
            for (int s = 0; s < 3000; ++s) l.onNmeaLine(gga(36000.0 + s % 80000, 51.5 + s * 1e-6, -0.1), T0 + s);
        });
        std::thread readings([&] {
            for (int s = 0; s < 30000; ++s) l.addReading(T0 + s / 10.0, -60.0 - (s % 7));
        });
        fixes.join();
        readings.join();
        stop = true;
        reader.join();
        const auto st = l.stats();
        CHECK(st.readingsStored + st.readingsNoFix + st.readingsBad == 30000);
        CHECK(st.fixesAccepted > 2000);
    }

    // --- hostile NMEA text ------------------------------------------------------------------------------------------------------------------
    {
        rp::RfPositionLog l;
        std::mt19937 rng(5);
        const std::string alphabet = "$GPGAMRC,*0123456789ABCDEFNSEW.-\r\n\x01\xff";
        int accepted = 0;
        for (int i = 0; i < 50000; ++i) {
            std::string t;
            const int n = static_cast<int>(rng() % 90);
            for (int k = 0; k < n; ++k) t += alphabet[rng() % alphabet.size()];
            if (l.onNmeaLine(t, T0 + i) == rp::FixResult::Accepted) ++accepted;
        }
        CHECK(accepted == 0);   // random text with a valid checksum AND a valid position essentially never happens
        std::string big(100000, 'A');
        CHECK(l.onNmeaLine(big, T0) == rp::FixResult::NotAFix);
        CHECK(l.onNmeaLine(std::string("$GPGGA\0,x", 9), T0) == rp::FixResult::NotAFix);
    }

    fs::remove_all(tmp);
    return testSummary("test_rf_position_log");
}
