// track_store.hpp - a store of position tracks (aircraft, ships, balloons, stations) kept for a chosen period and queried "as at
// time T" with interpolation, for timeline replay on the map (row M3 of the gaps survey). PURE LOGIC: no decoder, no clock, no
// screen. The wiring lane maps a decode event or a track point onto add(kind, id, point); the screen lane calls snapshot(T) for
// each frame of a replay and trail() for the tails. Not wired (see the record's WIRING HAND-OFF).
//
// MODEL
//  * A track is identified by (kind, id): id is 1 to 24 characters of letters, digits and "_ . / -" (an ICAO address, an MMSI, a
//    call sign, a balloon serial). A point is (unix seconds, latitude, longitude, altitude in METRES or NaN).
//  * Points of one track are kept sorted by time; an out-of-order point is inserted in place, a point at an existing time replaces it.
//  * RETENTION: points older than (newest time seen anywhere in the store - retentionSec) are dropped, in the track touched by each
//    add and in every track every 1024 adds. Default 6 hours, settable from 60 s to 30 days.
//  * MEMORY IS BOUNDED three ways: maxPointsPerTrack (oldest dropped first), maxTotalPoints and maxTracks (when exceeded the track
//    that has gone longest without an update is evicted whole). Counted in stats().
//  * AS AT T: between two points no more than maxInterpGapSec apart (default 300 s) the position is interpolated on the great circle
//    (so a track across the antimeridian or near a pole is right) and the altitude linearly; across a longer gap, or after the last
//    point, the last known position is HELD for holdSec (default 30 s) and then the track is absent ("gone dark") rather than
//    drawn as a straight line through unknown. Before the first point it is absent.
//  * PERSISTENCE: a text file ("FOXSDR-TRACKS 1"), atomic write, 128 MB read cap, damaged lines skipped and counted, loaded through
//    the same checks as add().
//
// REFUSED INPUT: non-finite time / latitude / longitude, out-of-range coordinates, bad id, a point older than the retention window.
//
// THREADS: one mutex over everything; add() from a decoder thread and queries from the screen thread are safe together.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/geo_math.hpp"
#include "core/safe_file_io.hpp"

namespace cascade::core::tracks {

enum class Kind : char { Aircraft = 'A', Ship = 'S', Balloon = 'B', Station = 'T' };

inline bool kindFromChar(char c, Kind& k) {
    switch (c) {
        case 'A': k = Kind::Aircraft; return true;
        case 'S': k = Kind::Ship; return true;
        case 'B': k = Kind::Balloon; return true;
        case 'T': k = Kind::Station; return true;
    }
    return false;
}

struct Point {
    double t = 0, lat = 0, lon = 0;
    double altM = std::numeric_limits<double>::quiet_NaN();
};

struct Sample {
    double lat = 0, lon = 0, altM = std::numeric_limits<double>::quiet_NaN();
    bool interpolated = false;   // between two points
    bool held = false;           // the last known position, not yet gone dark
    double ageSec = 0;           // T minus the time of the point at or before T
};

struct Config {
    double retentionSec = 6 * 3600.0;
    std::size_t maxPointsPerTrack = 5000;
    std::size_t maxTotalPoints = 500000;
    std::size_t maxTracks = 5000;
    double maxInterpGapSec = 300.0;
    double holdSec = 30.0;
};

enum class AddResult { Added, Replaced, BadInput, BadId, TooOld };
enum class LoadResult { Ok, Missing, TooLarge, BadVersion, Error };

struct Stats {
    std::uint64_t added = 0, replaced = 0, refused = 0, tooOld = 0, tracksEvicted = 0, pointsDropped = 0, loadSkippedLines = 0;
};

struct Entry {
    Kind kind;
    std::string id;
    Sample at;
};

inline bool idOk(const std::string& id) {
    if (id.empty() || id.size() > 24) return false;
    for (unsigned char c : id) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '.' || c == '/' || c == '-';
        if (!ok) return false;
    }
    return true;
}

class TrackStore {
public:
    explicit TrackStore(Config c = Config{}) : cfg_(sanitise(c)) {}

    AddResult add(Kind kind, const std::string& id, const Point& p) {
        std::lock_guard<std::mutex> lk(m_);
        return addLocked(kind, id, p);
    }

    void setRetentionSec(double sec) {
        std::lock_guard<std::mutex> lk(m_);
        if (!std::isfinite(sec)) return;
        cfg_.retentionSec = std::min(30.0 * 86400.0, std::max(60.0, sec));
        trimAllLocked();
    }

    // The position of one track at time T, or nullopt (not seen yet, or gone dark).
    std::optional<Sample> at(Kind kind, const std::string& id, double T) const {
        std::lock_guard<std::mutex> lk(m_);
        auto it = tracks_.find(key(kind, id));
        if (it == tracks_.end()) return std::nullopt;
        return sampleLocked(it->second.pts, T);
    }

    // Every track visible at T (optionally only one kind), ordered by kind then id.
    std::vector<Entry> snapshot(double T, std::optional<Kind> only = std::nullopt) const {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<Entry> out;
        for (const auto& kv : tracks_) {
            const Track& tr = kv.second;
            if (only && tr.kind != *only) continue;
            if (auto s = sampleLocked(tr.pts, T)) out.push_back({tr.kind, tr.id, *s});
        }
        return out;
    }

    // Stored points of a track with t in [t0, t1], thinned evenly to at most maxPoints (first and last kept).
    std::vector<Point> trail(Kind kind, const std::string& id, double t0, double t1, std::size_t maxPoints = 500) const {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<Point> out;
        auto it = tracks_.find(key(kind, id));
        if (it == tracks_.end() || maxPoints < 2) return out;
        std::vector<const Point*> sel;
        for (const Point& p : it->second.pts)
            if (p.t >= t0 && p.t <= t1) sel.push_back(&p);
        if (sel.size() <= maxPoints) {
            for (auto* p : sel) out.push_back(*p);
            return out;
        }
        for (std::size_t i = 0; i < maxPoints; ++i) out.push_back(*sel[i * (sel.size() - 1) / (maxPoints - 1)]);
        return out;
    }

    // Earliest and latest point time over all tracks; false when the store is empty.
    bool timeRange(double& first, double& last) const {
        std::lock_guard<std::mutex> lk(m_);
        bool any = false;
        for (const auto& kv : tracks_) {
            if (kv.second.pts.empty()) continue;
            const double a = kv.second.pts.front().t, b = kv.second.pts.back().t;
            if (!any) { first = a; last = b; any = true; }
            else { first = std::min(first, a); last = std::max(last, b); }
        }
        return any;
    }

    std::size_t trackCount() const { std::lock_guard<std::mutex> lk(m_); return tracks_.size(); }
    std::size_t pointCount() const { std::lock_guard<std::mutex> lk(m_); return total_; }
    Stats stats() const { std::lock_guard<std::mutex> lk(m_); return stats_; }
    Config config() const { std::lock_guard<std::mutex> lk(m_); return cfg_; }

    // ---- persistence ------------------------------------------------------------------------------------------------------------
    bool save(const std::filesystem::path& path) const {
        std::string s = "FOXSDR-TRACKS 1\n";
        {
            std::lock_guard<std::mutex> lk(m_);
            char buf[200];
            for (const auto& kv : tracks_) {
                const Track& tr = kv.second;
                for (const Point& p : tr.pts) {
                    if (std::isfinite(p.altM))
                        std::snprintf(buf, sizeof buf, "T %c %s %.3f %.7f %.7f %.1f\n", static_cast<char>(tr.kind), tr.id.c_str(), p.t, p.lat, p.lon, p.altM);
                    else
                        std::snprintf(buf, sizeof buf, "T %c %s %.3f %.7f %.7f -\n", static_cast<char>(tr.kind), tr.id.c_str(), p.t, p.lat, p.lon);
                    s += buf;
                }
            }
        }
        return safeio::writeAtomic(path, s);
    }

    LoadResult load(const std::filesystem::path& path) {
        std::string text;
        switch (safeio::readCapped(path, 128ull << 20, text)) {
            case safeio::ReadResult::Missing: return LoadResult::Missing;
            case safeio::ReadResult::TooLarge: return LoadResult::TooLarge;
            case safeio::ReadResult::Error: return LoadResult::Error;
            case safeio::ReadResult::Ok: break;
        }
        std::istringstream in(text);
        std::string line;
        if (!std::getline(in, line) || line != "FOXSDR-TRACKS 1") return LoadResult::BadVersion;
        std::uint64_t skipped = 0;
        std::vector<std::pair<std::pair<Kind, std::string>, Point>> rows;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            char kc = 0;
            char id[64] = {0};
            char alt[40] = {0};
            double t, la, lo;
            Kind k;
            if (std::sscanf(line.c_str(), "T %c %63s %lf %lf %lf %39s", &kc, id, &t, &la, &lo, alt) == 6 && kindFromChar(kc, k)) {
                Point p;
                p.t = t; p.lat = la; p.lon = lo;
                if (alt[0] != '-' || alt[1] != 0) {
                    char* end = nullptr;
                    const double a = std::strtod(alt, &end);
                    if (end == alt || *end != 0) { ++skipped; continue; }
                    p.altM = a;
                }
                rows.push_back({{k, id}, p});
            } else {
                ++skipped;
            }
        }
        std::lock_guard<std::mutex> lk(m_);
        // oldest first, so retention is judged against the newest time of the file
        std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.t < b.second.t; });
        for (auto& r : rows) {
            const AddResult res = addLocked(r.first.first, r.first.second, r.second);
            if (res == AddResult::BadInput || res == AddResult::BadId) ++skipped;
        }
        stats_.loadSkippedLines += skipped;
        return LoadResult::Ok;
    }

private:
    struct Track {
        Kind kind = Kind::Aircraft;
        std::string id;
        std::deque<Point> pts;
        std::uint64_t updateSeq = 0;
    };

    static Config sanitise(Config c) {
        if (!std::isfinite(c.retentionSec) || c.retentionSec < 60.0) c.retentionSec = 6 * 3600.0;
        if (c.retentionSec > 30.0 * 86400.0) c.retentionSec = 30.0 * 86400.0;
        if (c.maxPointsPerTrack < 2) c.maxPointsPerTrack = 2;
        if (c.maxTotalPoints < 2) c.maxTotalPoints = 2;
        if (c.maxTracks < 1) c.maxTracks = 1;
        if (!std::isfinite(c.maxInterpGapSec) || c.maxInterpGapSec < 0) c.maxInterpGapSec = 300.0;
        if (!std::isfinite(c.holdSec) || c.holdSec < 0) c.holdSec = 30.0;
        return c;
    }

    static std::string key(Kind k, const std::string& id) { return std::string(1, static_cast<char>(k)) + id; }

    AddResult addLocked(Kind kind, const std::string& id, const Point& p) {
        if (!idOk(id)) { ++stats_.refused; return AddResult::BadId; }
        if (!std::isfinite(p.t) || !geo::validLatLon(p.lat, p.lon) || (std::isfinite(p.altM) == false && !std::isnan(p.altM))) {
            ++stats_.refused;
            return AddResult::BadInput;
        }
        if (haveNewest_ && p.t < newest_ - cfg_.retentionSec) { ++stats_.tooOld; return AddResult::TooOld; }
        if (!haveNewest_ || p.t > newest_) { newest_ = p.t; haveNewest_ = true; }
        const std::string k = key(kind, id);
        auto it = tracks_.find(k);
        if (it == tracks_.end()) {
            if (tracks_.size() >= cfg_.maxTracks) evictStalest();
            it = tracks_.emplace(k, Track{}).first;
            it->second.kind = kind;
            it->second.id = id;
        }
        Track& tr = it->second;
        tr.updateSeq = ++seq_;
        AddResult res = AddResult::Added;
        if (tr.pts.empty() || p.t > tr.pts.back().t) {
            tr.pts.push_back(p);
            ++total_;
        } else {
            auto pos = std::lower_bound(tr.pts.begin(), tr.pts.end(), p.t, [](const Point& a, double t) { return a.t < t; });
            if (pos != tr.pts.end() && pos->t == p.t) {
                *pos = p;
                res = AddResult::Replaced;
            } else {
                tr.pts.insert(pos, p);
                ++total_;
            }
        }
        if (res == AddResult::Added) ++stats_.added; else ++stats_.replaced;
        trimTrack(tr);
        while (tr.pts.size() > cfg_.maxPointsPerTrack) { tr.pts.pop_front(); --total_; ++stats_.pointsDropped; }
        while (total_ > cfg_.maxTotalPoints && tracks_.size() > 1) evictStalest(&it->second);
        if (total_ > cfg_.maxTotalPoints) {   // one giant track: thin it by dropping the oldest
            while (total_ > cfg_.maxTotalPoints && tr.pts.size() > 1) { tr.pts.pop_front(); --total_; ++stats_.pointsDropped; }
        }
        if (++adds_ % 1024 == 0) trimAllLocked();
        return res;
    }

    void trimTrack(Track& tr) {
        const double cut = newest_ - cfg_.retentionSec;
        while (!tr.pts.empty() && tr.pts.front().t < cut) { tr.pts.pop_front(); --total_; ++stats_.pointsDropped; }
    }

    void trimAllLocked() {
        for (auto it = tracks_.begin(); it != tracks_.end();) {
            trimTrack(it->second);
            if (it->second.pts.empty()) it = tracks_.erase(it); else ++it;
        }
    }

    void evictStalest(const Track* keep = nullptr) {
        auto victim = tracks_.end();
        for (auto it = tracks_.begin(); it != tracks_.end(); ++it) {
            if (&it->second == keep) continue;
            if (victim == tracks_.end() || it->second.updateSeq < victim->second.updateSeq) victim = it;
        }
        if (victim == tracks_.end()) return;
        total_ -= victim->second.pts.size();
        stats_.pointsDropped += victim->second.pts.size();
        ++stats_.tracksEvicted;
        tracks_.erase(victim);
    }

    std::optional<Sample> sampleLocked(const std::deque<Point>& pts, double T) const {
        if (pts.empty() || !std::isfinite(T) || T < pts.front().t) return std::nullopt;
        auto hi = std::upper_bound(pts.begin(), pts.end(), T, [](double t, const Point& p) { return t < p.t; });
        const Point& a = *(hi - 1);   // last point at or before T
        Sample s;
        s.ageSec = T - a.t;
        if (a.t == T) {
            s.lat = a.lat; s.lon = a.lon; s.altM = a.altM;
            return s;
        }
        if (hi != pts.end() && hi->t - a.t <= cfg_.maxInterpGapSec) {
            const Point& b = *hi;
            const double f = (T - a.t) / (b.t - a.t);
            interpolate(a, b, f, s);
            s.interpolated = true;
            return s;
        }
        if (s.ageSec <= cfg_.holdSec) {
            s.lat = a.lat; s.lon = a.lon; s.altM = a.altM;
            s.held = true;
            return s;
        }
        return std::nullopt;
    }

    static void interpolate(const Point& a, const Point& b, double f, Sample& out) {
        auto vec = [](double lat, double lon, double v[3]) {
            const double p = geo::degToRad(lat), l = geo::degToRad(lon);
            v[0] = std::cos(p) * std::cos(l); v[1] = std::cos(p) * std::sin(l); v[2] = std::sin(p);
        };
        double va[3], vb[3];
        vec(a.lat, a.lon, va);
        vec(b.lat, b.lon, vb);
        double dot = va[0] * vb[0] + va[1] * vb[1] + va[2] * vb[2];
        dot = std::max(-1.0, std::min(1.0, dot));
        const double ang = std::acos(dot);
        double r[3];
        if (ang < 1e-9) {
            for (int i = 0; i < 3; ++i) r[i] = va[i];
        } else if (ang > geo::kPi - 1e-6) {   // antipodal: the great circle is not unique; take the straight blend in latitude/longitude
            out.lat = a.lat + (b.lat - a.lat) * f;
            out.lon = geo::wrapLon(a.lon + (b.lon - a.lon) * f);
            out.altM = (std::isfinite(a.altM) && std::isfinite(b.altM)) ? a.altM + (b.altM - a.altM) * f : std::numeric_limits<double>::quiet_NaN();
            return;
        } else {
            const double sa = std::sin((1 - f) * ang) / std::sin(ang), sb = std::sin(f * ang) / std::sin(ang);
            for (int i = 0; i < 3; ++i) r[i] = sa * va[i] + sb * vb[i];
        }
        out.lat = geo::radToDeg(std::atan2(r[2], std::sqrt(r[0] * r[0] + r[1] * r[1])));
        out.lon = geo::radToDeg(std::atan2(r[1], r[0]));
        out.altM = (std::isfinite(a.altM) && std::isfinite(b.altM)) ? a.altM + (b.altM - a.altM) * f : std::numeric_limits<double>::quiet_NaN();
    }

    Config cfg_;
    mutable std::mutex m_;
    std::map<std::string, Track> tracks_;
    std::size_t total_ = 0;
    double newest_ = 0;
    bool haveNewest_ = false;
    std::uint64_t seq_ = 0, adds_ = 0;
    Stats stats_;
};

}  // namespace cascade::core::tracks
