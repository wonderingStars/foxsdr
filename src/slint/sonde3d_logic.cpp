// sonde3d_logic.cpp - see sonde3d_logic.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "sonde3d_logic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace foxsdr::slintview3d {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kKmPerDeg = 111.195;
}  // namespace

bool SondeHistory::add(const std::string& id, double tS, double lat, double lon, double altM) {
    if (id.empty() || !std::isfinite(lat) || !std::isfinite(lon) || std::fabs(lat) > 90.0 || std::fabs(lon) > 180.0) { return false; }
    auto it = tracks_.find(id);
    if (it == tracks_.end()) {
        if (tracks_.size() >= kSondeMaxTracks) {   // make room: the balloon heard longest ago goes
            auto oldest = tracks_.begin();
            for (auto k = tracks_.begin(); k != tracks_.end(); ++k) { if (k->second.back().tS < oldest->second.back().tS) { oldest = k; } }
            tracks_.erase(oldest);
        }
        it = tracks_.emplace(id, std::vector<SondeSample>()).first;
    }
    std::vector<SondeSample>& v = it->second;
    if (!v.empty()) {
        const SondeSample& last = v.back();
        if (tS < last.tS) { return false; }
        double e = 0.0, n = 0.0;
        localKm(last.lat, last.lon, lat, lon, e, n);
        const double metres = std::hypot(e, n) * 1000.0;
        const bool altMoved = std::isfinite(altM) != std::isfinite(last.altM) || (std::isfinite(altM) && std::fabs(altM - last.altM) >= 3.0);
        if (metres < 3.0 && !altMoved && tS - last.tS < 10.0) { return false; }
    }
    v.push_back({tS, lat, lon, altM});
    if (v.size() > kSondeMaxSamples) { v.erase(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() - kSondeMaxSamples)); }
    return true;
}

const std::vector<SondeSample>* SondeHistory::of(const std::string& id) const {
    const auto it = tracks_.find(id);
    return it == tracks_.end() ? nullptr : &it->second;
}

void SondeHistory::prune(const std::vector<std::string>& live, double nowS, double maxAgeS) {
    for (auto it = tracks_.begin(); it != tracks_.end();) {
        const bool isLive = std::find(live.begin(), live.end(), it->first) != live.end();
        if (!isLive && (it->second.empty() || nowS - it->second.back().tS > maxAgeS)) { it = tracks_.erase(it); }
        else { ++it; }
    }
}

SondeDerived deriveSonde(const std::vector<SondeSample>& s) {
    SondeDerived d;
    d.currentAltM = kNaN;
    d.climbMs = kNaN;
    if (s.empty()) { return d; }
    d.hasTrack = true;
    d.points = s.size();
    d.currentAltM = s.back().altM;
    double maxAlt = -1e18;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (std::isfinite(s[i].altM) && s[i].altM > maxAlt) { maxAlt = s[i].altM; d.maxIndex = i; }
    }
    d.maxAltM = maxAlt > -1e17 ? maxAlt : kNaN;
    // burst: since the maximum the altitude has fallen by at least 300 m or 2 % of it, and the newest sample is below that line
    if (std::isfinite(d.maxAltM) && std::isfinite(d.currentAltM) && d.maxIndex + 1 < s.size()) {
        const double drop = std::max(300.0, 0.02 * d.maxAltM);
        d.burst = d.currentAltM <= d.maxAltM - drop;
    }
    // climb: against the newest sample at least 8 s old, within the last 90 s
    const SondeSample& now = s.back();
    if (std::isfinite(now.altM)) {
        for (std::size_t i = s.size(); i-- > 0;) {
            const double dt = now.tS - s[i].tS;
            if (dt > 90.0) { break; }
            if (dt >= 8.0 && std::isfinite(s[i].altM)) { d.climbMs = (now.altM - s[i].altM) / dt; d.haveClimb = true; break; }
        }
    }
    return d;
}

void localKm(double lat0, double lon0, double lat, double lon, double& eastKm, double& northKm) {
    const double mean = 0.5 * (lat0 + lat) * 3.14159265358979323846 / 180.0;
    double dlon = lon - lon0;
    if (dlon > 180.0) { dlon -= 360.0; }
    if (dlon < -180.0) { dlon += 360.0; }
    eastKm = dlon * kKmPerDeg * std::cos(mean);
    northKm = (lat - lat0) * kKmPerDeg;
}

double niceCeilKm(double v) {
    if (!(v > 0.0) || !std::isfinite(v)) { return 5.0; }
    for (const double a : {5.0, 10.0, 20.0, 25.0, 50.0, 100.0, 200.0, 250.0, 500.0}) { if (a >= v) { return a; } }
    return std::ceil(v / 100.0) * 100.0;
}

std::string formatKm(double km) {
    char b[24];
    if (std::fabs(km - std::round(km)) < 0.05) { std::snprintf(b, sizeof(b), "%.0f", km); }
    else { std::snprintf(b, sizeof(b), "%.1f", km); }
    std::string s = b;
    if (s == "-0") { s = "0"; }
    return s;
}

double ladderFraction(double altM) {
    if (!std::isfinite(altM)) { return -1.0; }
    return std::clamp(altM / 1000.0 / kSondeLadderTopKm, 0.0, 1.0);
}

std::string climbText(double ms, bool have) {
    if (!have || !std::isfinite(ms)) { return std::string(); }
    char b[32];
    std::snprintf(b, sizeof(b), "%+.1f m/s", ms);
    return b;
}

}  // namespace foxsdr::slintview3d
