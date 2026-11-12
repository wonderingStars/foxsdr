// adsb_coverage.hpp - ADS-B coverage model: a heat-map grid of positions heard and a 24-hour maximum-range outline by bearing for
// each altitude band (row M2 of the gaps survey). PURE LOGIC: no decoder, no clock, no screen. The wiring lane feeds add() with
// every decoded airborne position (unix seconds, latitude, longitude, altitude in feet or NaN) and the screen lane draws heat()
// and outlinePolygon(). Not wired (see the record's WIRING HAND-OFF).
//
// WHAT IT KEEPS
//  * HEAT: a sparse grid of `cellDeg` degree cells (default 0.05, about 5.5 km). Each cell keeps an AGEING weight: every position adds
//    1 and the weight halves every `halfLifeSec` (default 3 days), so the map shows where traffic is heard NOW and fades with
//    disuse. Cells below weight 0.01 are dropped; at most `maxCells` cells exist (when full, faded cells go first, then the weakest tenth).
//  * RANGE OUTLINE: the receiver's position and, per bearing bin (`binDeg`, default 5 degrees, 72 bins) and per altitude band, the
//    greatest distance heard in the last 24 hours. The 24 hours are kept in 10-minute buckets (145 per bin and band), so the window
//    edge is only exact to 10 minutes (stated; the query window is [now - 24 h - 10 min, now]). Bands (feet): <5000, 5000-9999,
//    10000-19999, 20000-29999, 30000-39999, >=40000, and one for an unknown altitude.
//  * Memory is bounded: cells (maxCells x about 40 bytes), buckets (7 x 72 x 145 x 12 bytes, about 0.9 MB).
//
// REFUSED INPUT (counted, never stored): non-finite or out-of-range values, the exact (0, 0) "null island" position that a failed
// decode produces, a position farther than `maxRangeKm` (default 1000 km: ADS-B ducting records are of the order of 1000 km; beyond
// is a decode error), a position older than 24 hours behind the newest one, or more than 24 hours ahead of it (a bad clock).
//
// PERSISTENCE: a small text file ("FOXSDR-ADSBCOV 1"), written atomically, read with a 64 MB cap, damaged lines skipped and
// counted, a different version refused. The range part of a file is kept only if its receiver position is within 1 km of this
// model's (the bearings mean nothing from another place); the heat part is kept either way.
//
// THREADS: every call takes one mutex; add() from a decoder thread and the readers from the screen thread are safe together.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/geo_math.hpp"
#include "core/safe_file_io.hpp"

namespace cascade::core::adsbcov {

inline constexpr int kBands = 7;            // six altitude bands and "unknown"
inline constexpr int kUnknownBand = 6;
inline constexpr double kBucketSec = 600.0;
inline constexpr int kBucketRing = 145;     // 24 h / 10 min + 1
inline constexpr double kWindowSec = 86400.0;

inline int bandOf(double altFt) {
    if (!std::isfinite(altFt) || altFt < -2000.0 || altFt > 100000.0) return kUnknownBand;
    if (altFt < 5000) return 0;
    if (altFt < 10000) return 1;
    if (altFt < 20000) return 2;
    if (altFt < 30000) return 3;
    if (altFt < 40000) return 4;
    return 5;
}

inline const char* bandLabel(int band) {
    static const char* k[] = {"below 5000 ft", "5000-9999 ft", "10000-19999 ft", "20000-29999 ft",
                              "30000-39999 ft", "40000 ft and above", "altitude unknown"};
    return (band >= 0 && band < kBands) ? k[band] : "";
}

struct Config {
    double rxLat = 0, rxLon = 0;
    double cellDeg = 0.05;
    double halfLifeSec = 3.0 * 86400.0;
    int binDeg = 5;                 // must divide 360
    double maxRangeKm = 1000.0;
    std::size_t maxCells = 200000;
};

struct HeatCell {
    double lat = 0, lon = 0;    // cell centre
    double weight = 0;
};

enum class AddResult { Added, BadInput, TooFar, Stale, Future };

struct Stats {
    std::uint64_t added = 0, badInput = 0, tooFar = 0, stale = 0, future = 0, cellsEvicted = 0;
    std::uint64_t loadSkippedLines = 0;
};

enum class LoadResult { Ok, Missing, TooLarge, BadVersion, Error };

class AdsbCoverage {
public:
    explicit AdsbCoverage(Config c = Config{}) : cfg_(sanitise(c)), bins_(360 / cfg_.binDeg) {
        buckets_.assign(static_cast<std::size_t>(kBands) * static_cast<std::size_t>(bins_) * kBucketRing, Bucket{});
    }

    const Config& config() const { return cfg_; }
    int binCount() const { return bins_; }

    AddResult add(double tSec, double lat, double lon, double altFt) {
        std::lock_guard<std::mutex> lk(m_);
        if (!std::isfinite(tSec) || !geo::validLatLon(lat, lon) || (lat == 0.0 && lon == 0.0)) {
            ++stats_.badInput;
            return AddResult::BadInput;
        }
        if (haveNewest_) {
            if (tSec < newest_ - kWindowSec) { ++stats_.stale; return AddResult::Stale; }
            if (tSec > newest_ + kWindowSec) { ++stats_.future; return AddResult::Future; }
        }
        const double rng = geo::distanceKm(cfg_.rxLat, cfg_.rxLon, lat, lon);
        if (rng > cfg_.maxRangeKm) { ++stats_.tooFar; return AddResult::TooFar; }
        if (!haveNewest_ || tSec > newest_) { newest_ = tSec; haveNewest_ = true; }
        ++stats_.added;

        // heat
        const std::int64_t li = static_cast<std::int64_t>(std::floor((lat + 90.0) / cfg_.cellDeg));
        const std::int64_t lj = static_cast<std::int64_t>(std::floor((lon + 180.0) / cfg_.cellDeg));
        const std::uint64_t key = (static_cast<std::uint64_t>(li) << 32) ^ static_cast<std::uint64_t>(lj & 0xffffffff);
        auto it = cells_.find(key);
        if (it == cells_.end()) {
            if (cells_.size() >= cfg_.maxCells) makeRoom(tSec);
            it = cells_.emplace(key, Cell{li, lj, 0.0, tSec}).first;
        }
        Cell& c = it->second;
        if (tSec > c.last) {
            c.weight *= decay(tSec - c.last);
            c.last = tSec;
        }
        c.weight += 1.0;

        // range outline
        const double brg = geo::bearingDeg(cfg_.rxLat, cfg_.rxLon, lat, lon);
        int bin = static_cast<int>(brg / cfg_.binDeg);
        if (bin >= bins_) bin = bins_ - 1;
        const std::int64_t bid = static_cast<std::int64_t>(std::floor(tSec / kBucketSec));
        Bucket& b = bucket(bandOf(altFt), bin, bid);
        if (b.id != bid) {
            b.id = bid;
            b.rangeKm = static_cast<float>(rng);
        } else if (rng > b.rangeKm) {
            b.rangeKm = static_cast<float>(rng);
        }
        return AddResult::Added;
    }

    // Heat cells with weight >= minWeight at time `now`, strongest first, at most maxCells of them.
    std::vector<HeatCell> heat(double now, double minWeight = 0.05, std::size_t maxCells = 20000) const {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<HeatCell> out;
        for (const auto& kv : cells_) {
            const Cell& c = kv.second;
            const double w = c.weight * decay(std::max(0.0, now - c.last));
            if (w < minWeight) continue;
            out.push_back({-90.0 + (static_cast<double>(c.li) + 0.5) * cfg_.cellDeg,
                           -180.0 + (static_cast<double>(c.lj) + 0.5) * cfg_.cellDeg, w});
        }
        std::sort(out.begin(), out.end(), [](const HeatCell& a, const HeatCell& b) { return a.weight > b.weight; });
        if (out.size() > maxCells) out.resize(maxCells);
        return out;
    }

    // Greatest distance (km) heard in each bearing bin over the 24 hours to `now`; band -1 = every band, else 0..6. 0 = nothing.
    std::vector<double> outlineKm(double now, int band = -1) const {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<double> out(static_cast<std::size_t>(bins_), 0.0);
        const std::int64_t nowId = static_cast<std::int64_t>(std::floor(now / kBucketSec));
        const std::int64_t oldest = nowId - (kBucketRing - 1);
        for (int bd = 0; bd < kBands; ++bd) {
            if (band >= 0 && bd != band) continue;
            for (int bin = 0; bin < bins_; ++bin) {
                double best = out[static_cast<std::size_t>(bin)];
                for (int r = 0; r < kBucketRing; ++r) {
                    const Bucket& b = buckets_[index(bd, bin, r)];
                    if (b.id >= oldest && b.id <= nowId && b.rangeKm > best) best = b.rangeKm;
                }
                out[static_cast<std::size_t>(bin)] = best;
            }
        }
        return out;
    }

    // The outline as points around the receiver at each bin's centre bearing; bins with nothing are returned at distance 0 (the
    // receiver itself) so the polygon closes through the receiver rather than inventing range.
    std::vector<geo::LatLon> outlinePolygon(double now, int band = -1) const {
        const std::vector<double> km = outlineKm(now, band);
        std::vector<geo::LatLon> pts;
        pts.reserve(km.size());
        for (std::size_t i = 0; i < km.size(); ++i) {
            const double brg = (static_cast<double>(i) + 0.5) * cfg_.binDeg;
            pts.push_back(km[i] > 0 ? geo::destination(cfg_.rxLat, cfg_.rxLon, brg, km[i]) : geo::LatLon{cfg_.rxLat, cfg_.rxLon});
        }
        return pts;
    }

    double maxRangeKm(double now, int band = -1) const {
        double m = 0;
        for (double v : outlineKm(now, band)) m = std::max(m, v);
        return m;
    }

    std::size_t cellCount() const {
        std::lock_guard<std::mutex> lk(m_);
        return cells_.size();
    }
    Stats stats() const {
        std::lock_guard<std::mutex> lk(m_);
        return stats_;
    }

    // ---- persistence ------------------------------------------------------------------------------------------------------------
    bool save(const std::filesystem::path& path) const {
        std::string s;
        {
            std::lock_guard<std::mutex> lk(m_);
            char buf[160];
            s += "FOXSDR-ADSBCOV 1\n";
            std::snprintf(buf, sizeof buf, "C %.7f %.7f %.6f %d %.0f\n", cfg_.rxLat, cfg_.rxLon, cfg_.cellDeg, cfg_.binDeg, newest_);
            s += buf;
            for (const auto& kv : cells_) {
                const Cell& c = kv.second;
                std::snprintf(buf, sizeof buf, "H %lld %lld %.6f %.1f\n", static_cast<long long>(c.li), static_cast<long long>(c.lj),
                              c.weight, c.last);
                s += buf;
            }
            for (int bd = 0; bd < kBands; ++bd)
                for (int bin = 0; bin < bins_; ++bin)
                    for (int r = 0; r < kBucketRing; ++r) {
                        const Bucket& b = buckets_[index(bd, bin, r)];
                        if (b.id < 0) continue;
                        std::snprintf(buf, sizeof buf, "R %d %d %lld %.3f\n", bd, bin, static_cast<long long>(b.id), b.rangeKm);
                        s += buf;
                    }
        }
        return safeio::writeAtomic(path, s);
    }

    // Replaces nothing on failure. On Ok merges the file into the (normally empty) model.
    LoadResult load(const std::filesystem::path& path) {
        std::string text;
        switch (safeio::readCapped(path, 64ull << 20, text)) {
            case safeio::ReadResult::Missing: return LoadResult::Missing;
            case safeio::ReadResult::TooLarge: return LoadResult::TooLarge;
            case safeio::ReadResult::Error: return LoadResult::Error;
            case safeio::ReadResult::Ok: break;
        }
        std::istringstream in(text);
        std::string line;
        if (!std::getline(in, line) || line != "FOXSDR-ADSBCOV 1") return LoadResult::BadVersion;
        std::lock_guard<std::mutex> lk(m_);
        bool keepRange = false, haveC = false;
        std::uint64_t skipped = 0;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (line[0] == 'C') {
                double rl, ro, cd, nw;
                int bd;
                if (std::sscanf(line.c_str(), "C %lf %lf %lf %d %lf", &rl, &ro, &cd, &bd, &nw) == 5 && geo::validLatLon(rl, ro)) {
                    haveC = true;
                    keepRange = geo::distanceKm(rl, ro, cfg_.rxLat, cfg_.rxLon) <= 1.0 && bd == cfg_.binDeg;
                    fileCellDegOk_ = std::fabs(cd - cfg_.cellDeg) < 1e-9;
                    if (std::isfinite(nw) && (!haveNewest_ || nw > newest_)) { newest_ = nw; haveNewest_ = true; }
                } else {
                    ++skipped;
                }
            } else if (line[0] == 'H') {
                long long li, lj;
                double w, last;
                if (haveC && fileCellDegOk_ && std::sscanf(line.c_str(), "H %lld %lld %lf %lf", &li, &lj, &w, &last) == 4 &&
                    std::isfinite(w) && w > 0 && w < 1e12 && std::isfinite(last) && cells_.size() < cfg_.maxCells) {
                    const std::uint64_t key = (static_cast<std::uint64_t>(li) << 32) ^ static_cast<std::uint64_t>(lj & 0xffffffff);
                    cells_[key] = Cell{li, lj, w, last};
                } else {
                    ++skipped;
                }
            } else if (line[0] == 'R') {
                int bd, bin;
                long long id;
                float km;
                if (haveC && keepRange && std::sscanf(line.c_str(), "R %d %d %lld %f", &bd, &bin, &id, &km) == 4 && bd >= 0 &&
                    bd < kBands && bin >= 0 && bin < bins_ && id >= 0 && std::isfinite(km) && km > 0 &&
                    km <= static_cast<float>(cfg_.maxRangeKm)) {
                    Bucket& b = bucket(bd, bin, id);
                    if (b.id != id || km > b.rangeKm) { b.id = id; b.rangeKm = km; }
                } else if (!(haveC && !keepRange)) {
                    ++skipped;
                }
            } else {
                ++skipped;
            }
        }
        stats_.loadSkippedLines += skipped;
        return LoadResult::Ok;
    }

private:
    struct Cell {
        std::int64_t li = 0, lj = 0;
        double weight = 0;
        double last = 0;
    };
    struct Bucket {
        std::int64_t id = -1;
        float rangeKm = 0;
    };

    static Config sanitise(Config c) {
        if (!std::isfinite(c.cellDeg) || c.cellDeg < 0.001 || c.cellDeg > 5.0) c.cellDeg = 0.05;
        if (!std::isfinite(c.halfLifeSec) || c.halfLifeSec < 60.0) c.halfLifeSec = 3.0 * 86400.0;
        if (c.binDeg < 1 || c.binDeg > 180 || 360 % c.binDeg != 0) c.binDeg = 5;
        if (!std::isfinite(c.maxRangeKm) || c.maxRangeKm < 1.0) c.maxRangeKm = 1000.0;
        if (c.maxCells < 16) c.maxCells = 16;
        if (!geo::validLatLon(c.rxLat, c.rxLon)) { c.rxLat = 0; c.rxLon = 0; }
        return c;
    }

    double decay(double dt) const { return std::exp2(-dt / cfg_.halfLifeSec); }

    std::size_t index(int band, int bin, int slot) const {
        return (static_cast<std::size_t>(band) * static_cast<std::size_t>(bins_) + static_cast<std::size_t>(bin)) * kBucketRing +
               static_cast<std::size_t>(slot);
    }
    Bucket& bucket(int band, int bin, std::int64_t id) { return buckets_[index(band, bin, static_cast<int>(id % kBucketRing))]; }

    void makeRoom(double now) {
        for (auto it = cells_.begin(); it != cells_.end();) {
            if (it->second.weight * decay(std::max(0.0, now - it->second.last)) < 0.01) {
                it = cells_.erase(it);
                ++stats_.cellsEvicted;
            } else {
                ++it;
            }
        }
        if (cells_.size() < cfg_.maxCells) return;
        std::vector<std::pair<double, std::uint64_t>> w;
        w.reserve(cells_.size());
        for (const auto& kv : cells_) w.push_back({kv.second.weight * decay(std::max(0.0, now - kv.second.last)), kv.first});
        const std::size_t drop = std::max<std::size_t>(1, cells_.size() / 10);
        std::nth_element(w.begin(), w.begin() + static_cast<std::ptrdiff_t>(drop), w.end());
        for (std::size_t i = 0; i < drop; ++i) {
            cells_.erase(w[i].second);
            ++stats_.cellsEvicted;
        }
    }

    Config cfg_;
    int bins_;
    mutable std::mutex m_;
    std::unordered_map<std::uint64_t, Cell> cells_;
    std::vector<Bucket> buckets_;
    double newest_ = 0;
    bool haveNewest_ = false;
    bool fileCellDegOk_ = false;
    Stats stats_;
};

}  // namespace cascade::core::adsbcov
