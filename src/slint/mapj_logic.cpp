// mapj_logic.cpp - see mapj_logic.hpp. SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "mapj_logic.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>

#include "core/antenna_calc.hpp"
#include "core/geo_math.hpp"
#include "core/i18n.hpp"
#include "core/locator.hpp"
#include "core/sky_position.hpp"

namespace foxsdr::slintmapj {
namespace {

namespace access = cascade::core::access;
namespace geo = cascade::core::geo;

constexpr double kPi = 3.14159265358979323846;

std::string fmt(const char* f, double a) {
    char buf[96];
    std::snprintf(buf, sizeof buf, f, a);
    return buf;
}

std::string lowered(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

bool containsNoCase(const std::string& hay, const std::string& needle) {
    if (needle.empty()) { return true; }
    return lowered(hay).find(lowered(needle)) != std::string::npos;
}

std::uint32_t rgba(int r, int g, int b, int a) {
    return (static_cast<std::uint32_t>(r) << 24) | (static_cast<std::uint32_t>(g) << 16) | (static_cast<std::uint32_t>(b) << 8) | static_cast<std::uint32_t>(a);
}

// three-stop ramp: 0 cold, 0.5 warm, 1 hot
std::uint32_t ramp(double t, int alphaLow, int alphaHigh) {
    if (!std::isfinite(t)) { t = 0.0; }
    t = std::clamp(t, 0.0, 1.0);
    const int c0[3] = {30, 70, 210}, c1[3] = {250, 220, 40}, c2[3] = {230, 40, 30};
    int c[3];
    for (int i = 0; i < 3; ++i) {
        c[i] = t < 0.5 ? static_cast<int>(std::lround(c0[i] + (c1[i] - c0[i]) * (t / 0.5))) : static_cast<int>(std::lround(c1[i] + (c2[i] - c1[i]) * ((t - 0.5) / 0.5)));
    }
    return rgba(c[0], c[1], c[2], static_cast<int>(std::lround(alphaLow + (alphaHigh - alphaLow) * t)));
}

constexpr LayerInfo kLayers[kLayerCount] = {
    {FOX_TR_NOOP("Airspaces"), FOX_TR_NOOP("Closed outlines a plugin publishes: airspaces, footprints and boundaries."), "", false},
    {FOX_TR_NOOP("Weather radar"), FOX_TR_NOOP("Rain radar over the map."), "", true},
    {FOX_TR_NOOP("Receiver map"), FOX_TR_NOOP("Other people's receivers on the map."), "", true},
    {FOX_TR_NOOP("Repeaters"), FOX_TR_NOOP("Repeater stations on the map."), "", true},
    {FOX_TR_NOOP("Heat map"), FOX_TR_NOOP("Where aircraft were heard from your receiver position, hottest where most often, fading over days."), FOX_TR_NOOP("Heat map: your own receiver's aircraft"), false},
    {FOX_TR_NOOP("24 h range"), FOX_TR_NOOP("The furthest each bearing has been heard in the last 24 hours, as an outline."), FOX_TR_NOOP("Range outline: your own receiver, last 24 hours"), false},
    {FOX_TR_NOOP("APRS paths"), FOX_TR_NOOP("Thin faint lines for the digipeaters a packet went through. The APRS plugin draws them only while its digipaths setting is on."), "", false},
    {FOX_TR_NOOP("Locator grid"), FOX_TR_NOOP("The Maidenhead grid: fields, squares and subsquares by zoom, with the squares heard and worked shaded."), FOX_TR_NOOP("Locator grid: Maidenhead"), false},
    {FOX_TR_NOOP("Signal level"), FOX_TR_NOOP("The signal level your receiver measured at each place a GPS fix put it, from weak blue to strong red."), FOX_TR_NOOP("Signal level against GPS position: your own log"), false},
};

}  // namespace

// ------------------------------------------------------------------------------------------------------------------------
// layers
// ------------------------------------------------------------------------------------------------------------------------
const LayerInfo& layerInfo(Layer l) {
    const int i = static_cast<int>(l);
    return kLayers[(i >= 0 && i < kLayerCount) ? i : 0];
}

const char* blockedNote() { return FOX_TR_NOOP("Off: this data source needs its terms and a key read first."); }

LayerSet LayerSet::defaults() {
    LayerSet s;
    s.on[static_cast<std::size_t>(Layer::Airspaces)] = true;
    s.on[static_cast<std::size_t>(Layer::AprsPaths)] = true;
    return s;
}

bool LayerSet::set(Layer l, bool value) {
    const int i = static_cast<int>(l);
    if (i < 0 || i >= kLayerCount) { return false; }
    if (value && layerInfo(l).needsSource) { return false; }
    on[static_cast<std::size_t>(i)] = value;
    return true;
}

std::vector<LegendEntry> layerLegend(const LayerSet& set, bool workedHeardShown) {
    std::vector<LegendEntry> out;
    if (set.get(Layer::AdsbHeat)) {
        out.push_back({FOX_TR_NOOP("Heard rarely"), heatRgba(0.1)});
        out.push_back({FOX_TR_NOOP("Heard often"), heatRgba(0.95)});
    }
    if (set.get(Layer::AdsbRange)) { out.push_back({FOX_TR_NOOP("Range, last 24 h"), outlineRgba(-1)}); }
    if (set.get(Layer::Airspaces)) { out.push_back({FOX_TR_NOOP("Airspace or footprint"), rgba(240, 170, 60, 255)}); }
    if (set.get(Layer::AprsPaths)) { out.push_back({FOX_TR_NOOP("APRS path"), rgba(200, 200, 200, 255)}); }
    if (set.get(Layer::Locators) && workedHeardShown) {
        out.push_back({FOX_TR_NOOP("Heard square"), rgba(70, 170, 230, 255)});
        out.push_back({FOX_TR_NOOP("Worked square"), rgba(70, 200, 90, 255)});
    }
    if (set.get(Layer::RfLevel)) {
        out.push_back({FOX_TR_NOOP("Signal weak"), levelRgba(0.0, 0.0, 1.0)});
        out.push_back({FOX_TR_NOOP("Signal strong"), levelRgba(1.0, 0.0, 1.0)});
    }
    return out;
}

std::string creditLine(const std::string& base, const LayerSet& set, const std::function<std::string(const std::string&)>& tr) {
    std::string out = base;
    for (int i = 0; i < kLayerCount; ++i) {
        if (!set.on[static_cast<std::size_t>(i)]) { continue; }
        const char* credit = kLayers[i].credit;
        if (credit == nullptr || credit[0] == '\0') { continue; }
        if (!out.empty()) { out += " | "; }
        out += tr ? tr(credit) : std::string(credit);
    }
    return out;
}

bool isAprsPath(const std::string& id, std::uint32_t kind) {
    static const std::string tail = "~path";
    return kind == static_cast<std::uint32_t>(kKindStation) && id.size() > tail.size() && id.compare(id.size() - tail.size(), tail.size(), tail) == 0;
}

bool isClosedPath(std::uint32_t flags) { return (flags & 0x1u) != 0u; }   // CASCADE_PATH_FLAG_CLOSED (tests hold it to the ABI header)

std::uint32_t heatRgba(double t01) { return ramp(t01, 70, 190); }

std::uint32_t outlineRgba(int band) {
    static const std::uint32_t k[7] = {rgba(255, 140, 40, 255), rgba(255, 200, 60, 255), rgba(200, 230, 70, 255), rgba(90, 220, 140, 255),
                                       rgba(70, 190, 230, 255), rgba(140, 130, 250, 255), rgba(170, 170, 170, 255)};
    return (band >= 0 && band < 7) ? k[band] : rgba(240, 240, 240, 255);
}

std::vector<slintmap::Cell> heatCells(const std::vector<cascade::core::adsbcov::HeatCell>& cells, double cellDeg) {
    std::vector<slintmap::Cell> out;
    if (!(cellDeg > 0.0) || !std::isfinite(cellDeg) || cells.empty()) { return out; }
    double wmax = 0.0;
    for (const auto& c : cells) {
        if (std::isfinite(c.weight)) { wmax = std::max(wmax, c.weight); }
    }
    if (!(wmax > 0.0)) { return out; }
    const double norm = std::log1p(wmax);
    out.reserve(cells.size());
    for (const auto& c : cells) {
        if (!std::isfinite(c.weight) || !(c.weight > 0.0) || !std::isfinite(c.lat) || !std::isfinite(c.lon)) { continue; }
        slintmap::Cell cell;
        cell.latMin = c.lat - cellDeg * 0.5;
        cell.latMax = c.lat + cellDeg * 0.5;
        cell.lonMin = c.lon - cellDeg * 0.5;
        cell.lonMax = c.lon + cellDeg * 0.5;
        cell.rgba = heatRgba(std::log1p(c.weight) / norm);
        out.push_back(cell);
    }
    return out;
}

slintmap::Trail outlineTrail(const std::vector<std::pair<double, double>>& polygon, int band) {
    slintmap::Trail t;
    t.points = polygon;
    t.closed = true;
    t.rgba = outlineRgba(band);
    t.widthPx = 2.f;
    t.fillAlpha = 38;
    return t;
}

slintmap::Trail aprsPathTrail(const std::vector<std::pair<double, double>>& points) {
    slintmap::Trail t;
    t.points = points;
    t.rgba = rgba(200, 200, 200, 90);     // faint
    t.widthPx = 1.f;                      // thin
    return t;
}

std::uint32_t levelRgba(double db, double minDb, double maxDb) {
    double t = (maxDb > minDb) ? (db - minDb) / (maxDb - minDb) : 0.0;
    return ramp(t, 140, 170);
}

std::vector<slintmap::Cell> levelCells(const std::vector<cascade::core::rfpos::Cell>& cells, double cellMeters) {
    std::vector<slintmap::Cell> out;
    if (cells.empty() || !(cellMeters > 0.0) || !std::isfinite(cellMeters)) { return out; }
    double lo = std::numeric_limits<double>::infinity(), hi = -std::numeric_limits<double>::infinity();
    for (const auto& c : cells) {
        if (!std::isfinite(c.meanDb)) { continue; }
        lo = std::min(lo, c.meanDb);
        hi = std::max(hi, c.meanDb);
    }
    if (!(hi >= lo)) { return out; }
    const double halfLat = cellMeters * 0.5 / 111320.0;
    out.reserve(cells.size());
    for (const auto& c : cells) {
        if (!std::isfinite(c.lat) || !std::isfinite(c.lon) || !std::isfinite(c.meanDb)) { continue; }
        const double halfLon = halfLat / std::max(0.01, std::cos(c.lat * kPi / 180.0));
        slintmap::Cell cell;
        cell.latMin = c.lat - halfLat;
        cell.latMax = c.lat + halfLat;
        cell.lonMin = c.lon - halfLon;
        cell.lonMax = c.lon + halfLon;
        cell.rgba = levelRgba(c.meanDb, lo, hi == lo ? lo + 1.0 : hi);
        out.push_back(cell);
    }
    return out;
}

// ------------------------------------------------------------------------------------------------------------------------
// the Maidenhead grid
// ------------------------------------------------------------------------------------------------------------------------
int gridLevelFor(double spanDeg) {
    if (!std::isfinite(spanDeg) || spanDeg <= 0.0) { return 0; }
    if (spanDeg > 60.0) { return 1; }
    if (spanDeg > 1.5) { return 2; }
    return 3;
}

namespace {
void gridStep(int level, double& lonStep, double& latStep) {
    if (level <= 1) { lonStep = 20.0; latStep = 10.0; }
    else if (level == 2) { lonStep = 2.0; latStep = 1.0; }
    else { lonStep = 2.0 / 24.0; latStep = 1.0 / 24.0; }
}
}  // namespace

std::vector<GridLabel> gridLabels(double centreLat, double centreLon, double spanLonDeg, double spanLatDeg, int level, std::size_t cap) {
    std::vector<GridLabel> out;
    if (level < 1 || level > 3 || cap == 0 || !std::isfinite(centreLat) || !std::isfinite(centreLon) || !(spanLonDeg > 0.0) || !(spanLatDeg > 0.0)) { return out; }
    double lonStep = 0.0, latStep = 0.0;
    gridStep(level, lonStep, latStep);
    const double west = centreLon - spanLonDeg * 0.5, east = centreLon + spanLonDeg * 0.5;
    const double south = std::max(-90.0, centreLat - spanLatDeg * 0.5), north = std::min(90.0, centreLat + spanLatDeg * 0.5);
    const double j0 = std::floor((west + 180.0) / lonStep), j1 = std::floor((east + 180.0) / lonStep);
    const double i0 = std::floor((south + 90.0) / latStep), i1 = std::floor((north + 90.0) / latStep);
    if ((j1 - j0 + 1.0) * (i1 - i0 + 1.0) > 20000.0) { return out; }       // far too many cells to name: the grid is not offered
    const int chars = level == 1 ? 2 : level == 2 ? 4 : 6;
    for (double i = i0; i <= i1; i += 1.0) {
        const double lat = (i + 0.5) * latStep - 90.0;
        if (lat < -90.0 || lat > 90.0) { continue; }
        for (double j = j0; j <= j1; j += 1.0) {
            const double lon = (j + 0.5) * lonStep - 180.0;        // continuous: a view across the antimeridian runs past 180
            if (lon < west - lonStep * 0.5 || lon > east + lonStep * 0.5) { continue; }
            std::string text;
            const double wrapped = geo::wrapLon(lon);
            if (!cascade::core::locator::encode(lat, wrapped, chars <= 4 ? 4 : chars, text)) { continue; }
            text = text.substr(0, static_cast<std::size_t>(chars));
            GridLabel g;
            g.lat = lat;
            g.lon = lon;
            g.text = std::move(text);
            out.push_back(std::move(g));
        }
    }
    std::sort(out.begin(), out.end(), [&](const GridLabel& a, const GridLabel& b) {
        const double da = (a.lat - centreLat) * (a.lat - centreLat) + (a.lon - centreLon) * (a.lon - centreLon);
        const double db = (b.lat - centreLat) * (b.lat - centreLat) + (b.lon - centreLon) * (b.lon - centreLon);
        return da < db;
    });
    if (out.size() > cap) { out.resize(cap); }
    return out;
}

std::vector<slintmap::Cell> locatorCells(const std::vector<LocatorMark>& marks) {
    // one cell per distinct locator; worked beats heard
    std::vector<std::pair<std::string, int>> uniq;
    for (const LocatorMark& m : marks) {
        bool found = false;
        for (auto& u : uniq) {
            if (u.first == m.locator) { u.second = std::max(u.second, static_cast<int>(m.state)); found = true; break; }
        }
        if (!found) { uniq.emplace_back(m.locator, static_cast<int>(m.state)); }
    }
    std::vector<slintmap::Cell> out;
    for (const auto& u : uniq) {
        cascade::core::locator::Position p;
        if (!cascade::core::locator::decode(u.first, p)) { continue; }
        double lonW = 2.0, latH = 1.0;
        if (p.chars == 6) { lonW = 2.0 / 24.0; latH = 1.0 / 24.0; }
        else if (p.chars == 8) { lonW = 2.0 / 240.0; latH = 1.0 / 240.0; }
        slintmap::Cell c;
        c.latMin = p.lat - latH * 0.5;
        c.latMax = p.lat + latH * 0.5;
        c.lonMin = p.lon - lonW * 0.5;
        c.lonMax = p.lon + lonW * 0.5;
        c.rgba = u.second >= static_cast<int>(MarkState::Worked) ? rgba(70, 200, 90, 110) : rgba(70, 170, 230, 90);
        out.push_back(c);
    }
    return out;
}

// ------------------------------------------------------------------------------------------------------------------------
// replay
// ------------------------------------------------------------------------------------------------------------------------
void Replay::open(double first, double last) {
    if (!std::isfinite(first) || !std::isfinite(last) || last < first) { return; }
    active = true;
    playing = false;
    t0 = first;
    t1 = last;
    t = std::max(first, last - 600.0);
}

void Replay::close() {
    active = false;
    playing = false;
}

void Replay::setRange(double first, double last) {
    if (!std::isfinite(first) || !std::isfinite(last) || last < first) { return; }
    t0 = first;
    t1 = last;
    t = std::clamp(t, t0, t1);
}

void Replay::play() {
    if (!active) { return; }
    if (t >= t1) { t = t0; }                  // play from the end starts again from the start
    playing = true;
}
void Replay::pause() { playing = false; }
void Replay::toggle() { if (playing) { pause(); } else { play(); } }
void Replay::setSpeed(int index) { speedIndex = std::clamp(index, 0, kReplaySpeeds - 1); }
void Replay::faster() { speedIndex = (speedIndex + 1) % kReplaySpeeds; }

void Replay::scrub(double fraction01) {
    if (!active || !std::isfinite(fraction01)) { return; }
    t = t0 + std::clamp(fraction01, 0.0, 1.0) * (t1 - t0);
}

void Replay::tick(double dtSec) {
    if (!active || !playing || !std::isfinite(dtSec) || dtSec <= 0.0) { return; }
    t += dtSec * kReplaySpeedFactor[static_cast<std::size_t>(std::clamp(speedIndex, 0, kReplaySpeeds - 1))];
    if (t >= t1) {
        t = t1;
        playing = false;                       // the newest instant: nothing further to show
    }
}

double Replay::fraction() const { return t1 > t0 ? std::clamp((t - t0) / (t1 - t0), 0.0, 1.0) : 0.0; }

std::string clockText(double unixSec) {
    if (!std::isfinite(unixSec) || unixSec < 0.0 || unixSec > 253402300799.0) { return "--:--:-- UTC"; }
    const std::time_t tt = static_cast<std::time_t>(unixSec);
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &tt);
#else
    gmtime_r(&tt, &tmv);
#endif
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d UTC", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

std::string speedText(int speedIndex) {
    const int i = std::clamp(speedIndex, 0, kReplaySpeeds - 1);
    return fmt("%.0fx", kReplaySpeedFactor[static_cast<std::size_t>(i)]);
}

std::string behindText(double newestSec, double tSec, const std::function<std::string(const std::string&)>& tr) {
    const auto word = [&](const char* key) { return tr ? tr(key) : std::string(key); };
    const double d = newestSec - tSec;
    if (!std::isfinite(d) || d < 1.0) { return word("live edge"); }
    const long total = static_cast<long>(d);
    char buf[96];
    if (total >= 60) { std::snprintf(buf, sizeof buf, word("%d min %d s behind").c_str(), static_cast<int>(total / 60), static_cast<int>(total % 60)); }
    else { std::snprintf(buf, sizeof buf, word("%d s behind").c_str(), static_cast<int>(total)); }
    return buf;
}

int TrackRecorder::feed(double nowSec, const std::vector<FeedItem>& items, double minGapSec) {
    if (!std::isfinite(nowSec)) { return 0; }
    int added = 0;
    for (const FeedItem& it : items) {
        char kc = 0;
        if (it.kind == kKindAircraft) { kc = 'A'; }
        else if (it.kind == kKindVessel) { kc = 'S'; }
        else if (it.kind == kKindStation) { kc = 'T'; }
        else { continue; }
        if (!cascade::core::tracks::idOk(it.id)) {
            ++skippedIds_;
            continue;
        }
        const std::string key = std::string(1, kc) + it.id;
        auto found = last_.find(key);
        if (found != last_.end() && nowSec - found->second < minGapSec && nowSec >= found->second) { continue; }
        cascade::core::tracks::Point p;
        p.t = nowSec;
        p.lat = it.lat;
        p.lon = it.lon;
        if (it.hasAlt && std::isfinite(it.altM)) { p.altM = it.altM; }
        const auto r = store_->add(static_cast<cascade::core::tracks::Kind>(kc), it.id, p);
        if (r == cascade::core::tracks::AddResult::Added || r == cascade::core::tracks::AddResult::Replaced) {
            last_[key] = nowSec;
            ++added;
        }
    }
    if (last_.size() > 20000u) { last_.clear(); }
    return added;
}

void TrackRecorder::clear() {
    store_ = std::make_unique<cascade::core::tracks::TrackStore>(cfg_);
    last_.clear();
    skippedIds_ = 0;
}

// ------------------------------------------------------------------------------------------------------------------------
// the filter
// ------------------------------------------------------------------------------------------------------------------------
bool isOnGround(const FilterSubject& s) {
    if (!s.hasAlt) { return false; }
    if (s.altM >= 15.24) { return false; }                   // 50 ft
    return !s.hasSpeed || s.speedMps < 25.0;
}

bool filterPasses(const AircraftFilter& f, const FilterSubject& s) {
    if (!f.enabled) { return true; }
    const double ft = s.altM / 0.3048;
    if (f.altMinOn && (!s.hasAlt || ft < f.altMinFt)) { return false; }
    if (f.altMaxOn && (!s.hasAlt || ft > f.altMaxFt)) { return false; }
    if (f.hideGround && isOnGround(s)) { return false; }
    if (!f.callsign.empty() && !containsNoCase(s.label, f.callsign) && !containsNoCase(s.id, f.callsign)) { return false; }
    if (f.militaryOnly && !(s.known && s.military)) { return false; }
    if (!f.typeText.empty() && !(s.known && containsNoCase(s.typeCode, f.typeText))) { return false; }
    if (!f.operatorText.empty() && !(s.known && containsNoCase(s.operatorName, f.operatorText))) { return false; }
    return true;
}

bool filterNeedsPluginFields(const AircraftFilter& f) { return f.militaryOnly || !f.typeText.empty() || !f.operatorText.empty(); }

int filterConditionCount(const AircraftFilter& f) {
    return (f.altMinOn ? 1 : 0) + (f.altMaxOn ? 1 : 0) + (f.hideGround ? 1 : 0) + (f.callsign.empty() ? 0 : 1) + (f.militaryOnly ? 1 : 0) + (f.typeText.empty() ? 0 : 1) +
           (f.operatorText.empty() ? 0 : 1);
}

std::string filterSummary(const AircraftFilter& f, const std::function<std::string(const std::string&)>& tr) {
    const int n = filterConditionCount(f);
    const auto word = [&](const char* key) { return tr ? tr(key) : std::string(key); };
    if (!f.enabled || n == 0) { return word("no conditions"); }
    char buf[96];
    std::snprintf(buf, sizeof buf, word(n == 1 ? "%d condition" : "%d conditions").c_str(), n);
    return buf;
}

bool parseNumber(const std::string& text, double lo, double hi, double& out) {
    std::string t;
    bool dot = false;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\'') { continue; }
        if (c == '.') { dot = true; }
        t += c;
    }
    if (!dot) { for (char& c : t) { if (c == ',') { c = '.'; } } }
    else { t.erase(std::remove(t.begin(), t.end(), ','), t.end()); }
    if (t.empty()) { return false; }
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || *end != '\0' || !std::isfinite(v) || v < lo || v > hi) { return false; }
    out = v;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------
// units
// ------------------------------------------------------------------------------------------------------------------------
access::ObjectKind objectKindOf(int trackKind) {
    if (trackKind == kKindAircraft) { return access::ObjectKind::Aircraft; }
    if (trackKind == kKindVessel) { return access::ObjectKind::Vessel; }
    if (trackKind == kKindSatellite) { return access::ObjectKind::Satellite; }
    return access::ObjectKind::Other;
}

std::string altitudeText(access::UnitSystem sys, int trackKind, double altM) {
    return access::text(access::quantify(sys, objectKindOf(trackKind), access::Quantity::Altitude, altM));
}
std::string speedTextOf(access::UnitSystem sys, int trackKind, double speedMps) {
    return access::text(access::quantify(sys, objectKindOf(trackKind), access::Quantity::Speed, speedMps));
}
std::string distanceText(access::UnitSystem sys, int trackKind, double metres) {
    return access::text(access::quantify(sys, objectKindOf(trackKind), access::Quantity::Distance, metres));
}

std::string unitChoiceName(access::UnitSystem sys) {
    switch (sys) {
        case access::UnitSystem::Metric: return FOX_TR_NOOP("Metric");
        case access::UnitSystem::Imperial: return FOX_TR_NOOP("Imperial");
        case access::UnitSystem::Aviation: return FOX_TR_NOOP("Aviation");
        case access::UnitSystem::Nautical: return FOX_TR_NOOP("Nautical");
        default: return FOX_TR_NOOP("Automatic");
    }
}

access::UnitSystem nextUnitSystem(access::UnitSystem sys) {
    const int n = (static_cast<int>(sys) + 1) % access::kUnitSystemCount;
    return static_cast<access::UnitSystem>(n);
}

ScaleBar scaleBar(access::UnitSystem sys, double metresPerPixel, double paneWidthPx) {
    ScaleBar out;
    if (!(metresPerPixel > 0.0) || !(paneWidthPx > 0.0) || !std::isfinite(metresPerPixel) || !std::isfinite(paneWidthPx)) { return out; }
    struct U { const char* sym; double m; };
    U major{"km", 1000.0}, minor{"m", 1.0};
    switch (sys) {
        case access::UnitSystem::Imperial: major = {"mi", 1609.344}; minor = {"ft", 0.3048}; break;
        case access::UnitSystem::Aviation: major = {"nmi", 1852.0}; minor = {"ft", 0.3048}; break;
        case access::UnitSystem::Nautical: major = {"nmi", 1852.0}; minor = {"m", 1.0}; break;
        default: break;
    }
    const double maxM = 0.3 * paneWidthPx * metresPerPixel;
    for (const U& u : {major, minor}) {
        const double x = maxM / u.m;
        if (!(x >= 1.0)) { continue; }
        const double p = std::pow(10.0, std::floor(std::log10(x)));
        const double m = x / p;
        const double nice = (m >= 5.0 ? 5.0 : m >= 2.0 ? 2.0 : 1.0) * p;
        out.frac = nice * u.m / (metresPerPixel * paneWidthPx);
        out.text = fmt("%.0f ", nice) + u.sym;
        return out;
    }
    return out;
}

// ------------------------------------------------------------------------------------------------------------------------
// the sky
// ------------------------------------------------------------------------------------------------------------------------
std::vector<SkyRow> skyRows(double latDeg, double lonDeg, double unixSec) {
    std::vector<SkyRow> out;
    for (const auto& b : cascade::core::sky::skyPositions(latDeg, lonDeg, unixSec)) {
        SkyRow r;
        r.name = b.name;
        r.azDeg = b.pos.azDeg;
        r.elDeg = b.pos.elDeg;
        r.below = b.pos.elDeg < 0.0;
        r.azText = fmt("%.1f", b.pos.azDeg);
        r.elText = fmt("%.1f", b.pos.elDeg);
        out.push_back(std::move(r));
    }
    return out;
}

PolarPoint polarPoint(double azDeg, double elDeg) {
    PolarPoint p;
    if (!std::isfinite(azDeg) || !std::isfinite(elDeg)) { p.below = true; return p; }
    p.below = elDeg < 0.0;
    const double r = p.below ? 1.0 : (90.0 - std::min(elDeg, 90.0)) / 90.0;
    const double az = azDeg * kPi / 180.0;
    p.x = r * std::sin(az);
    p.y = -r * std::cos(az);
    return p;
}

PassSummary summarisePass(const std::vector<PassSample>& pass) {
    PassSummary s;
    const PassSample* first = nullptr;
    const PassSample* last = nullptr;
    for (const PassSample& x : pass) {
        if (!std::isfinite(x.tSec) || !std::isfinite(x.azDeg) || !std::isfinite(x.elDeg) || x.elDeg < 0.0) { continue; }
        if (first == nullptr) { first = &x; }
        last = &x;
        if (!s.valid || x.elDeg > s.maxElDeg) {
            s.maxElDeg = x.elDeg;
            s.maxElAzDeg = x.azDeg;
        }
        s.valid = true;
    }
    if (!s.valid) { return s; }
    s.aosAzDeg = first->azDeg;
    s.losAzDeg = last->azDeg;
    s.durationSec = last->tSec - first->tSec;
    return s;
}

std::vector<PolarPoint> passTrace(const std::vector<PassSample>& pass) {
    std::vector<PolarPoint> out;
    for (const PassSample& x : pass) {
        if (!std::isfinite(x.azDeg) || !std::isfinite(x.elDeg) || x.elDeg < 0.0) { continue; }
        out.push_back(polarPoint(x.azDeg, x.elDeg));
    }
    return out;
}

std::vector<PassSample> madeUpPass() {
    std::vector<PassSample> out;
    for (int i = 0; i <= 60; ++i) {
        const double f = i / 60.0;
        PassSample s;
        s.tSec = 600.0 * f;
        s.azDeg = std::fmod(215.0 + 190.0 * f, 360.0);
        s.elDeg = 64.0 * std::sin(kPi * f);
        out.push_back(s);
    }
    return out;
}

// ------------------------------------------------------------------------------------------------------------------------
// antenna
// ------------------------------------------------------------------------------------------------------------------------
namespace {
std::string lengthM(double m) { return fmt(m >= 100.0 ? "%.1f m" : "%.2f m", m); }
std::string lengthFt(double m) {
    const double ft = cascade::core::antenna::metresToFeet(m);
    return fmt(ft >= 100.0 ? "%.1f ft" : "%.2f ft", ft);
}
AntennaRow lengthRow(const char* item, double m, const char* note = "") { return {item, lengthM(m), lengthFt(m), note}; }
}  // namespace

std::string antennaError(const AntennaInput& in) {
    namespace ant = cascade::core::antenna;
    if (!ant::freqOk(in.fMHz)) { return FOX_TR_NOOP("The frequency must be between 0.01 and 300000 MHz."); }
    if (!std::isfinite(in.coaxFraction) || in.coaxFraction <= 0.0 || in.coaxFraction > 100.0) { return FOX_TR_NOOP("The coax length must be above 0 and at most 100 wavelengths."); }
    if (!std::isfinite(in.vf) || in.vf <= 0.0 || in.vf > 1.0) { return FOX_TR_NOOP("The velocity factor must be above 0 and at most 1."); }
    if (!std::isfinite(in.distanceKm) || in.distanceKm <= 0.0 || in.distanceKm > 1e9) { return FOX_TR_NOOP("The distance must be above 0 km."); }
    if (in.yagiElements < 2 || in.yagiElements > 20) { return FOX_TR_NOOP("A Yagi has 2 to 20 elements."); }
    return std::string();
}

std::vector<AntennaRow> antennaRows(const AntennaInput& in) {
    namespace ant = cascade::core::antenna;
    std::vector<AntennaRow> rows;
    if (!ant::freqOk(in.fMHz)) { return rows; }
    if (const auto w = ant::wavelengthM(in.fMHz)) { rows.push_back(lengthRow(FOX_TR_NOOP("Wavelength"), *w)); }
    if (const auto d = ant::dipoleLengthM(in.fMHz)) { rows.push_back(lengthRow(FOX_TR_NOOP("Half-wave dipole"), *d, FOX_TR_NOOP("Overall wire length; trim on an analyser."))); }
    if (const auto g = ant::groundPlaneM(in.fMHz)) {
        rows.push_back(lengthRow(FOX_TR_NOOP("Quarter-wave ground plane"), g->radiatorM));
        rows.push_back(lengthRow(FOX_TR_NOOP("Radial, each"), g->radialM));
    }
    if (in.yagiElements >= 2 && in.yagiElements <= 20) {
        if (const auto y = ant::yagiStartingLengths(in.fMHz, in.yagiElements)) {
            int directors = 0;
            double boom = 0.0;
            for (std::size_t i = 0; i < y->size(); ++i) {
                const auto& e = (*y)[i];
                boom = std::max(boom, e.positionM);
                const std::string role = e.role;
                const char* note = i == 0 ? FOX_TR_NOOP("Starting lengths only: tune with an antenna analyser.") : "";
                if (role == "reflector") { rows.push_back(lengthRow(FOX_TR_NOOP("Yagi reflector"), e.lengthM, note)); }
                else if (role == "driven") { rows.push_back(lengthRow(FOX_TR_NOOP("Yagi driven element"), e.lengthM, note)); }
                else {
                    ++directors;
                    AntennaRow r = lengthRow(FOX_TR_NOOP("Yagi director"), e.lengthM);
                    r.item = std::string(FOX_TR_NOOP("Yagi director")) + " " + std::to_string(directors);
                    rows.push_back(std::move(r));
                }
            }
            rows.push_back(lengthRow(FOX_TR_NOOP("Yagi boom"), boom));
        }
    }
    if (const auto c = ant::coaxLengthM(in.fMHz, in.coaxFraction, in.vf)) { rows.push_back(lengthRow(FOX_TR_NOOP("Coax length"), *c)); }
    if (const auto loss = ant::fsplDb(in.distanceKm, in.fMHz)) {
        AntennaRow r;
        r.item = FOX_TR_NOOP("Free-space path loss");
        r.metres = fmt("%.1f dB", *loss);
        r.note = ant::fsplFarField(in.distanceKm, in.fMHz) ? "" : FOX_TR_NOOP("Closer than ten wavelengths: the formula does not hold.");
        rows.push_back(std::move(r));
    }
    return rows;
}

// ------------------------------------------------------------------------------------------------------------------------
// the tile cache
// ------------------------------------------------------------------------------------------------------------------------
std::string tileCacheText(std::size_t tiles, std::uint64_t bytes, std::uint64_t maxBytes, const cascade::core::tilecache::Stats& stats,
                          const std::function<std::string(const std::string&)>& tr) {
    const std::string f = tr ? tr("%zu tiles, %.1f of %.0f MB; %llu hits, %llu misses") : std::string("%zu tiles, %.1f of %.0f MB; %llu hits, %llu misses");
    char buf[192];
    std::snprintf(buf, sizeof buf, f.c_str(), tiles, static_cast<double>(bytes) / 1048576.0, static_cast<double>(maxBytes) / 1048576.0,
                  static_cast<unsigned long long>(stats.hits + stats.staleHits), static_cast<unsigned long long>(stats.misses));
    return buf;
}

}  // namespace foxsdr::slintmapj
