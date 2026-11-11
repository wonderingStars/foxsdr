// rf_position_log.hpp - pairs GPS fixes with signal-level readings, grids them for a heat map and exports CSV (row Q5 of the gaps
// survey: RF heat map against GPS position). PURE LOGIC plus the existing NMEA parser: no serial port, no clock, no screen. The
// wiring lane feeds onNmeaLine() with each line the existing GPS reader yields (core/gps_reader.*, core/nmea.*) and the host time
// it was received, and addReading() with each level the engine measures (the S-meter / signalDb value, at the host time of the
// measurement). Not wired (see the record's WIRING HAND-OFF).
//
// REUSE. The sentence parser is the project's own core/nmea.hpp (parseNmeaSentence: GGA, RMC, GLL from any talker, mandatory
// checksum, empty fields = no fix, status A / quality 1-5 only). It lives in cascade_engine, so a test or caller links that.
//
// PAIRING. A fix carries the HOST time it arrived (NMEA has only the time of day). A reading at time t is placed
//  * BETWEEN two fixes no more than maxInterpGapSec apart: linear in latitude and longitude between them (over 10 s that is a
//    straight line to well under a metre of error),
//  * otherwise at the nearest fix within maxFixAgeSec (default 5 s), else it is counted as NoFix and dropped.
// FIX SANITY: fixes must arrive in time order; a fix that implies a speed above maxSpeedMps (default 400 m/s, faster than a
// road or a boat) from the previous accepted fix within 60 s is refused as a jump. The (0, 0) position is refused.
//
// GRID. Square-ish cells of `cellMeters` (default 25 m): the latitude step is cellMeters / 111 194.9 m per degree, the longitude
// step is that divided by cos(latitude of the first fix) so cells stay about square within a city-sized area. Each cell keeps the
// count, the mean level computed in LINEAR POWER (10*log10 of the mean of 10^(dB/10), not a mean of dB values), and the maximum and
// minimum level. Bounded: at most maxSamples raw samples (oldest dropped) and maxCells cells (new cells refused beyond it).
//
// CSV: samplesCsv() "unix_s,lat,lon,level_db,freq_hz" and gridCsv() "lat,lon,count,mean_db,max_db,min_db". Numbers only, so no
// field can need quoting or can carry a formula; both are written atomically by the save functions.
//
// THREADS: one mutex; the fix and reading feeds may come from different threads.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/geo_math.hpp"
#include "core/nmea.hpp"
#include "core/safe_file_io.hpp"

namespace cascade::core::rfpos {

struct Config {
    double cellMeters = 25.0;
    double maxFixAgeSec = 5.0;
    double maxInterpGapSec = 10.0;
    double maxSpeedMps = 400.0;
    std::size_t maxSamples = 200000;
    std::size_t maxCells = 100000;
    double minLevelDb = -250.0;
    double maxLevelDb = 100.0;
};

enum class FixResult { Accepted, NotAFix, Duplicate, OutOfOrder, Jump, BadPosition };
enum class ReadingResult { Stored, NoFix, BadReading, CellLimit };

struct Sample {
    double tSec = 0, lat = 0, lon = 0, levelDb = 0, freqHz = 0;
};

struct Cell {
    double lat = 0, lon = 0;     // centre
    std::uint32_t count = 0;
    double meanDb = 0, maxDb = 0, minDb = 0;
};

struct Stats {
    std::uint64_t lines = 0, fixesAccepted = 0, fixesRefused = 0, readingsStored = 0, readingsNoFix = 0, readingsBad = 0, samplesDropped = 0;
    std::uint64_t checksumFailures = 0, noFixSentences = 0;
};

class RfPositionLog {
public:
    explicit RfPositionLog(Config c = Config{}) : cfg_(sanitise(c)) {}

    // One NMEA line (with or without its line ending) received at host time tSec.
    FixResult onNmeaLine(std::string_view line, double tSec) {
        std::lock_guard<std::mutex> lk(m_);
        ++stats_.lines;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\0')) line.remove_suffix(1);
        NmeaFix fix;
        const NmeaStatus st = parseNmeaSentence(line, fix);
        if (st == NmeaStatus::BadChecksum || st == NmeaStatus::NoChecksum) ++stats_.checksumFailures;
        if (st != NmeaStatus::Ok) return FixResult::NotAFix;
        if (!fix.valid) { ++stats_.noFixSentences; return FixResult::NotAFix; }
        return addFixLocked(tSec, fix.latDeg, fix.lonDeg);
    }

    FixResult addFix(double tSec, double lat, double lon) {
        std::lock_guard<std::mutex> lk(m_);
        return addFixLocked(tSec, lat, lon);
    }

    ReadingResult addReading(double tSec, double levelDb, double freqHz = 0.0) {
        std::lock_guard<std::mutex> lk(m_);
        if (!std::isfinite(tSec) || !std::isfinite(levelDb) || levelDb < cfg_.minLevelDb || levelDb > cfg_.maxLevelDb ||
            !std::isfinite(freqHz) || freqHz < 0) {
            ++stats_.readingsBad;
            return ReadingResult::BadReading;
        }
        double lat, lon;
        if (!positionAtLocked(tSec, lat, lon)) { ++stats_.readingsNoFix; return ReadingResult::NoFix; }
        if (!haveGrid_) setupGridLocked(lat, lon);
        const std::int64_t i = static_cast<std::int64_t>(std::floor(lat / dLat_));
        const std::int64_t j = static_cast<std::int64_t>(std::floor(lon / dLon_));
        const std::pair<std::int64_t, std::int64_t> key{i, j};
        auto it = cells_.find(key);
        if (it == cells_.end()) {
            if (cells_.size() >= cfg_.maxCells) return ReadingResult::CellLimit;
            it = cells_.emplace(key, Acc{}).first;
            it->second.maxDb = -1e9;
            it->second.minDb = 1e9;
        }
        Acc& a = it->second;
        ++a.count;
        a.sumLinear += std::pow(10.0, levelDb / 10.0);
        a.maxDb = std::max(a.maxDb, levelDb);
        a.minDb = std::min(a.minDb, levelDb);
        samples_.push_back({tSec, lat, lon, levelDb, freqHz});
        while (samples_.size() > cfg_.maxSamples) { samples_.pop_front(); ++stats_.samplesDropped; }
        ++stats_.readingsStored;
        return ReadingResult::Stored;
    }

    std::vector<Cell> cells() const {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<Cell> out;
        out.reserve(cells_.size());
        for (const auto& kv : cells_) {
            const Acc& a = kv.second;
            Cell c;
            c.lat = (static_cast<double>(kv.first.first) + 0.5) * dLat_;
            c.lon = (static_cast<double>(kv.first.second) + 0.5) * dLon_;
            c.count = a.count;
            c.meanDb = 10.0 * std::log10(a.sumLinear / a.count);
            c.maxDb = a.maxDb;
            c.minDb = a.minDb;
            out.push_back(c);
        }
        return out;
    }

    std::vector<Sample> samples() const {
        std::lock_guard<std::mutex> lk(m_);
        return std::vector<Sample>(samples_.begin(), samples_.end());
    }

    std::string samplesCsv() const {
        std::lock_guard<std::mutex> lk(m_);
        std::string s = "unix_s,lat,lon,level_db,freq_hz\n";
        char buf[160];
        for (const Sample& x : samples_) {
            std::snprintf(buf, sizeof buf, "%.3f,%.7f,%.7f,%.2f,%.0f\n", x.tSec, x.lat, x.lon, x.levelDb, x.freqHz);
            s += buf;
        }
        return s;
    }

    std::string gridCsv() const {
        std::string s = "lat,lon,count,mean_db,max_db,min_db\n";
        char buf[160];
        for (const Cell& c : cells()) {
            std::snprintf(buf, sizeof buf, "%.7f,%.7f,%u,%.2f,%.2f,%.2f\n", c.lat, c.lon, c.count, c.meanDb, c.maxDb, c.minDb);
            s += buf;
        }
        return s;
    }

    bool saveSamplesCsv(const std::filesystem::path& p) const { return safeio::writeAtomic(p, samplesCsv()); }
    bool saveGridCsv(const std::filesystem::path& p) const { return safeio::writeAtomic(p, gridCsv()); }

    Stats stats() const { std::lock_guard<std::mutex> lk(m_); return stats_; }
    std::size_t cellCount() const { std::lock_guard<std::mutex> lk(m_); return cells_.size(); }
    std::size_t sampleCount() const { std::lock_guard<std::mutex> lk(m_); return samples_.size(); }

    void clear() {
        std::lock_guard<std::mutex> lk(m_);
        cells_.clear();
        samples_.clear();
        fixes_.clear();
        haveGrid_ = false;
    }

private:
    struct Fix { double t, lat, lon; };
    struct Acc {
        std::uint32_t count = 0;
        double sumLinear = 0, maxDb = -1e9, minDb = 1e9;
    };

    static Config sanitise(Config c) {
        if (!std::isfinite(c.cellMeters) || c.cellMeters < 1.0) c.cellMeters = 25.0;
        if (!std::isfinite(c.maxFixAgeSec) || c.maxFixAgeSec < 0) c.maxFixAgeSec = 5.0;
        if (!std::isfinite(c.maxInterpGapSec) || c.maxInterpGapSec < 0) c.maxInterpGapSec = 10.0;
        if (!std::isfinite(c.maxSpeedMps) || c.maxSpeedMps <= 0) c.maxSpeedMps = 400.0;
        if (c.maxSamples < 1) c.maxSamples = 1;
        if (c.maxCells < 1) c.maxCells = 1;
        return c;
    }

    FixResult addFixLocked(double t, double lat, double lon) {
        if (!std::isfinite(t) || !geo::validLatLon(lat, lon) || (lat == 0.0 && lon == 0.0)) { ++stats_.fixesRefused; return FixResult::BadPosition; }
        if (!fixes_.empty()) {
            const Fix& last = fixes_.back();
            if (std::fabs(t - last.t) < 0.001) return FixResult::Duplicate;     // GGA and RMC of the same second
            if (t < last.t) { ++stats_.fixesRefused; return FixResult::OutOfOrder; }
            const double dt = t - last.t;
            if (dt <= 60.0 && geo::distanceKm(last.lat, last.lon, lat, lon) * 1000.0 / dt > cfg_.maxSpeedMps) {
                ++stats_.fixesRefused;
                return FixResult::Jump;
            }
        }
        fixes_.push_back({t, lat, lon});
        while (fixes_.size() > 64) fixes_.pop_front();
        ++stats_.fixesAccepted;
        return FixResult::Accepted;
    }

    bool positionAtLocked(double t, double& lat, double& lon) const {
        if (fixes_.empty()) return false;
        auto hi = std::upper_bound(fixes_.begin(), fixes_.end(), t, [](double v, const Fix& f) { return v < f.t; });
        if (hi != fixes_.begin() && hi != fixes_.end()) {
            const Fix& a = *(hi - 1);
            const Fix& b = *hi;
            if (b.t - a.t <= cfg_.maxInterpGapSec) {
                const double f = (t - a.t) / (b.t - a.t);
                lat = a.lat + (b.lat - a.lat) * f;
                double dl = b.lon - a.lon;
                if (dl > 180) dl -= 360;
                if (dl < -180) dl += 360;
                lon = geo::wrapLon(a.lon + dl * f);
                return true;
            }
        }
        // nearest fix within the age limit
        const Fix* best = nullptr;
        double bestAge = 1e300;
        if (hi != fixes_.begin()) { const Fix& a = *(hi - 1); bestAge = t - a.t; best = &a; }
        if (hi != fixes_.end()) {
            const double age = hi->t - t;
            if (age < bestAge) { bestAge = age; best = &*hi; }
        }
        if (!best || bestAge > cfg_.maxFixAgeSec) return false;
        lat = best->lat;
        lon = best->lon;
        return true;
    }

    void setupGridLocked(double lat, double) {
        dLat_ = cfg_.cellMeters / 111194.9;
        const double c = std::max(0.05, std::cos(geo::degToRad(lat)));
        dLon_ = dLat_ / c;
        haveGrid_ = true;
    }

    Config cfg_;
    mutable std::mutex m_;
    std::deque<Fix> fixes_;
    std::map<std::pair<std::int64_t, std::int64_t>, Acc> cells_;
    std::deque<Sample> samples_;
    double dLat_ = 0, dLon_ = 0;
    bool haveGrid_ = false;
    Stats stats_;
};

}  // namespace cascade::core::rfpos
