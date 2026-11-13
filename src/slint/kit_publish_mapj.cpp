// kit_publish_mapj.cpp - SCREEN LANE J (docs/multi-radio-records/screen-j.md): the map page's layers, replay, filter and units, the Sky page (kind 12) and the
// Antenna page (kind 13), as members of KitPublisher. The rules are mapj_logic.*; this file turns them into the window's models and answers the window's one
// callback (MapJEvents.act).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "kit_publish.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "core/geo_math.hpp"
#include "core/i18n.hpp"
#include "core/locator.hpp"
#include "core/plugin_ui_data.hpp"
#include "core/utf8_text.hpp"

namespace foxsdr::slintkit {

namespace mj = foxsdr::slintmapj;
namespace access = cascade::core::access;

namespace {

slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }
std::string fmt(const char* f, double v) { std::string s; cascade::core::formatUtf8(s, f, v); return s; }
template <class T> std::shared_ptr<slint::Model<T>> model(std::vector<T> v) { return std::make_shared<slint::VectorModel<T>>(std::move(v)); }
std::string tr(const std::string& s) { return std::string(cascade::i18n::tr(s.c_str())); }
const std::function<std::string(const std::string&)>& trFn() {
    static const std::function<std::string(const std::string&)> f = [](const std::string& s) { return std::string(cascade::i18n::tr(s.c_str())); };
    return f;
}
double nowUnix() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
slint::Color colourOf(std::uint32_t rgba) {
    return slint::Color::from_argb_uint8(255, static_cast<std::uint8_t>(rgba >> 24), static_cast<std::uint8_t>(rgba >> 16), static_cast<std::uint8_t>(rgba >> 8));
}
// the colour the page's own paths have always had (kit_publish.cpp kindColour) at alpha 0xb0
std::uint32_t pathRgba(std::uint32_t kind) {
    switch (kind) {
        case CASCADE_TRACK_AIRCRAFT: return 0xf5a524b0u;
        case CASCADE_TRACK_VESSEL: return 0x7ad7ffb0u;
        case CASCADE_TRACK_STATION: return 0x3ddc84b0u;
        case CASCADE_TRACK_SATELLITE: return 0xe6e8ecb0u;
        default: return 0xc9ced6b0u;
    }
}
bool envOn(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}
std::string joinNames(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) { out += out.empty() ? s : ", " + s; }
    return out;
}

// the body names: keys of the string tables (the sky model's own names are English)
[[maybe_unused]] const char* const kBodyKeys[] = {FOX_TR_NOOP("Sun"), FOX_TR_NOOP("Moon"), FOX_TR_NOOP("Cassiopeia A"), FOX_TR_NOOP("Cygnus A"), FOX_TR_NOOP("Taurus A (Crab)"),
                                                  FOX_TR_NOOP("Virgo A (M87)"), FOX_TR_NOOP("Sagittarius A*")};

mj::FilterSubject subjectOf(const CascadeTrack& t, const mj::MapJShared& shared) {
    mj::FilterSubject f;
    f.id = t.id;
    f.label = t.label[0] != '\0' ? std::string(t.label) : std::string(t.id);
    f.hasAlt = std::isfinite(t.altM);
    f.altM = f.hasAlt ? t.altM : 0.0;
    f.hasSpeed = std::isfinite(t.speedMps);
    f.speedMps = f.hasSpeed ? t.speedMps : 0.0;
    f.category = static_cast<int>(CASCADE_TRACK_CATEGORY(t.flags));
    if (shared.filterDetail) { f.known = shared.filterDetail(f.id, f); }
    return f;
}

// the NMEA GGA sentence of a position (a generated track for a bounded run)
std::string gga(double lat, double lon, double tSec) {
    const double al = std::fabs(lat), ao = std::fabs(lon);
    const int ld = static_cast<int>(al), od = static_cast<int>(ao);
    char body[160];
    std::snprintf(body, sizeof body, "GPGGA,%02d%02d%02d,%02d%07.4f,%c,%03d%07.4f,%c,1,08,0.9,50.0,M,47.0,M,,", static_cast<int>(tSec / 3600) % 24, static_cast<int>(tSec / 60) % 60, static_cast<int>(tSec) % 60,
                  ld, (al - ld) * 60.0, lat >= 0 ? 'N' : 'S', od, (ao - od) * 60.0, lon >= 0 ? 'E' : 'W');
    unsigned cs = 0;
    for (const char* p = body; *p != '\0'; ++p) { cs ^= static_cast<unsigned char>(*p); }
    char line[200];
    std::snprintf(line, sizeof line, "$%s*%02X", body, cs);
    return line;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// the pages
// ---------------------------------------------------------------------------------------------------------------------------------
void KitPublisher::openSkyPage() {
    openPage("sky", 12, "", "", cascade::i18n::tr("SKY"));
    showPage("sky");
}

void KitPublisher::openAntennaPage() {
    openPage("antenna", 13, "", "", cascade::i18n::tr("ANTENNA"));
    showPage("antenna");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// once a tick, for every open map page: the replay's clock, the recorder (the page's tracks, one point a track every two seconds), the coverage model
// ---------------------------------------------------------------------------------------------------------------------------------
void KitPublisher::feedMapSurfaces(const FoxReceiverState& state) {
    const double now = nowUnix();
    const bool home = (state.flags & FOXAPI_RX_POSITION_SET) != 0u;
    if (home && !mj_.testFeedsDone) {
        mj_.testFeedsDone = true;      // the bounded run's made-up feeds (never set in a release): the data a wiring lane will supply
        const double lat = state.rxLatDeg, lon = state.rxLonDeg;
        if (envOn("FOXSDR_SLINT_TEST_LOCATORS")) {
            std::string here, north, east;
            if (cascade::core::locator::encode(lat, lon, 4, here) && cascade::core::locator::encode(std::min(89.0, lat + 1.0), lon, 4, north) && cascade::core::locator::encode(lat, std::min(179.0, lon + 2.0), 4, east)) {
                mj_.locatorMarks = {{here, mj::MarkState::Heard}, {north, mj::MarkState::Worked}, {east, mj::MarkState::Heard}};
            }
        }
        if (envOn("FOXSDR_SLINT_TEST_RFTRACK")) {
            for (int i = 0; i < 80; ++i) {
                const double t = 1000.0 + i;
                mj_.rfLog.onNmeaLine(gga(lat + 0.0005 * (i % 40), lon + 0.0007 * (i / 2), t), t);
                mj_.rfLog.addReading(t, -95.0 + 0.5 * i, 100e6);
            }
        }
        if (envOn("FOXSDR_SLINT_TEST_PASS")) { mj_.passProvider = [] { return mj::madeUpPass(); }; }
        if (envOn("FOXSDR_SLINT_TEST_FILTER")) {
            mj_.filterDetail = [](const std::string& id, mj::FilterSubject& f) {
                const std::size_t h = std::hash<std::string>{}(id);
                f.military = (h % 2u) == 0u;
                f.typeCode = f.military ? "C130" : "B738";
                f.operatorName = f.military ? "Test Air Force" : "Test Airways";
                return true;
            };
        }
    }
    for (const Page& p : pages_) {
        if (p.kind != 5) { continue; }
        MapSurface& s = surfaceFor(p.plugin);
        mj::MapJSurface& j = s.j;
        double dt = j.lastTickSec < 0.0 ? 0.0 : now - j.lastTickSec;
        j.lastTickSec = now;
        dt = std::clamp(dt, 0.0, 1.0);
        if (j.replay.active) {
            double first = 0.0, last = 0.0;
            if (j.recorder.store().timeRange(first, last)) { j.replay.setRange(first, last); }
            j.replay.tick(dt);
        }
        if (j.lastFeedSec >= 0.0 && now - j.lastFeedSec < 1.0) { continue; }
        j.lastFeedSec = now;
        std::vector<mj::FeedItem> items;
        for (const cascade::core::HostTrack& trk : kit_.tracks()) {
            if (trk.plugin != p.plugin || !cascade::core::trackHasPosition(trk.t) || !cascade::core::trackPresentation(trk.t).visible) { continue; }
            mj::FeedItem it;
            it.kind = static_cast<int>(trk.t.kind);
            it.id = trk.t.id;
            it.lat = trk.t.latDeg;
            it.lon = trk.t.lonDeg;
            it.hasAlt = std::isfinite(trk.t.altM);
            it.altM = it.hasAlt ? trk.t.altM : 0.0;
            items.push_back(std::move(it));
        }
        j.recorder.feed(now, items);
        if (home) {
            if (!j.coverage || std::fabs(j.covLat - state.rxLatDeg) > 0.001 || std::fabs(j.covLon - state.rxLonDeg) > 0.001) {
                cascade::core::adsbcov::Config c;
                c.rxLat = state.rxLatDeg;
                c.rxLon = state.rxLonDeg;
                j.coverage = std::make_unique<cascade::core::adsbcov::AdsbCoverage>(c);
                j.covLat = state.rxLatDeg;
                j.covLon = state.rxLonDeg;
            }
            for (const mj::FeedItem& it : items) {
                if (it.kind != mj::kKindAircraft) { continue; }
                j.coverage->add(now, it.lat, it.lon, it.hasAlt ? it.altM / 0.3048 : std::nan(""));
            }
        } else { j.coverage.reset(); }
    }
}

// The tracks of a replay at instant T, as the page's own kind of track (so the list, the targets and the staleness rule need no second path).
std::vector<cascade::core::HostTrack> KitPublisher::replayTracks(MapSurface& s, const std::string& plugin) const {
    std::vector<cascade::core::HostTrack> out;
    for (const auto& e : s.j.recorder.store().snapshot(s.j.replay.t)) {
        cascade::core::HostTrack h;
        h.plugin = plugin;
        std::snprintf(h.t.id, sizeof h.t.id, "%s", e.id.c_str());
        std::snprintf(h.t.label, sizeof h.t.label, "%s", e.id.c_str());
        h.t.latDeg = e.at.lat;
        h.t.lonDeg = e.at.lon;
        h.t.altM = e.at.altM;
        h.t.courseDeg = std::nan("");
        h.t.speedMps = std::nan("");
        h.t.ageMs = static_cast<std::uint64_t>(std::max(0.0, e.at.ageSec) * 1000.0);
        h.t.kind = e.kind == cascade::core::tracks::Kind::Aircraft ? CASCADE_TRACK_AIRCRAFT : e.kind == cascade::core::tracks::Kind::Ship ? CASCADE_TRACK_VESSEL : CASCADE_TRACK_STATION;
        h.t.flags = 0u;
        out.push_back(std::move(h));
    }
    return out;
}

// The aircraft filter over the page's tracks: the kept ones, the count hidden.
void KitPublisher::applyFilter(MapSurface& s, std::vector<const cascade::core::HostTrack*>& mine) const {
    s.j.hiddenByFilter = 0;
    if (!s.j.filter.enabled || mj::filterConditionCount(s.j.filter) == 0) { return; }
    std::vector<const cascade::core::HostTrack*> kept;
    for (const auto* t : mine) {
        if (t->t.kind == CASCADE_TRACK_AIRCRAFT && !mj::filterPasses(s.j.filter, subjectOf(t->t, mj_))) {
            ++s.j.hiddenByFilter;
            continue;
        }
        kept.push_back(t);
    }
    mine.swap(kept);
}

// The layers' drawing: the closed outlines (airspaces), the APRS path lines, the heat, the range outline, the signal-level cells, the Maidenhead grid and its captions.
// `paths` are the page's plugin paths; trails is what setTrails gets (lines behind, then the rest). Returns the words for the status line.
std::string KitPublisher::drawMapLayers(MapSurface& s, MapPage* pg, bool trailsOn, const std::string& plugin, std::vector<slintmap::Trail>& trails, std::vector<::GridLabel>& labels, std::string& sig) {
    mj::MapJSurface& j = s.j;
    const double now = nowUnix();
    std::vector<slintmap::Trail> lines, closed, open;
    for (const cascade::core::HostPath& path : kit_.paths()) {
        if (path.plugin != plugin) { continue; }
        std::vector<std::pair<double, double>> pts;
        for (const CascadePathPoint& pt : path.points) { pts.emplace_back(pt.latDeg, pt.lonDeg); }
        if (mj::isAprsPath(path.id, path.kind)) {
            if (j.layers.get(mj::Layer::AprsPaths)) { lines.push_back(mj::aprsPathTrail(pts)); }
            continue;
        }
        slintmap::Trail trl;
        trl.points = std::move(pts);
        const bool isClosed = mj::isClosedPath(path.flags);
        trl.closed = isClosed;
        trl.dashed = (path.flags & CASCADE_PATH_FLAG_DASHED) != 0u;
        trl.rgba = pathRgba(path.kind);
        if (isClosed) {
            if (!j.layers.get(mj::Layer::Airspaces)) { continue; }
            trl.fillAlpha = 28;
            closed.push_back(std::move(trl));
        } else {
            if (!trailsOn) { continue; }
            open.push_back(std::move(trl));
        }
    }
    std::vector<slintmap::Cell> cells;
    std::string status;
    const auto note = [&](const std::string& t) { status += status.empty() ? t : "  |  " + t; };
    if (j.layers.get(mj::Layer::AdsbHeat) && j.coverage) {
        const auto heat = j.coverage->heat(now);
        cells = mj::heatCells(heat, j.coverage->config().cellDeg);
        note(cascade::core::formatText(tr("%zu heat cells").c_str(), cells.size()));
    }
    std::vector<slintmap::Trail> outline;
    if (j.layers.get(mj::Layer::AdsbRange) && j.coverage) {
        std::vector<std::pair<double, double>> pts;
        for (const auto& p : j.coverage->outlinePolygon(now, -1)) { pts.emplace_back(p.lat, p.lon); }
        const double km = j.coverage->maxRangeKm(now, -1);
        if (km > 0.0) {
            outline.push_back(mj::outlineTrail(pts, -1));
            note(cascade::core::formatText(tr("range %s").c_str(), mj::distanceText(j.units, mj::kKindAircraft, km * 1000.0).c_str()));
        }
    }
    if (j.layers.get(mj::Layer::RfLevel)) {
        const auto lc = mj::levelCells(mj_.rfLog.cells(), cascade::core::rfpos::Config{}.cellMeters);
        cells.insert(cells.end(), lc.begin(), lc.end());
        note(lc.empty() ? tr("No GPS fix") : cascade::core::formatText(tr("%zu level cells").c_str(), lc.size()));
    }
    int grid = 0;
    labels.clear();
    if (j.layers.get(mj::Layer::Locators) && pg != nullptr) {
        grid = mj::gridLevelFor(pg->spanDeg);
        const auto shaded = mj::locatorCells(mj_.locatorMarks);
        cells.insert(cells.end(), shaded.begin(), shaded.end());
        double lat0 = 0, lon0 = 0, lat1 = 0, lon1 = 0;
        s.comp.unproject(0.5, 0.0, lat0, lon0);
        s.comp.unproject(0.5, 1.0, lat1, lon1);
        const double spanLat = std::fabs(lat0 - lat1);
        for (const auto& g : mj::gridLabels(s.comp.centreLatDeg(), s.comp.centreLonDeg(), pg->spanDeg, spanLat, grid, 120)) {
            double fx = 0, fy = 0;
            if (!s.comp.project(g.lat, g.lon, fx, fy)) { continue; }
            ::GridLabel gl{};
            gl.x = static_cast<float>(fx);
            gl.y = static_cast<float>(fy);
            gl.text = ss(g.text);
            labels.push_back(std::move(gl));
        }
    }
    s.comp.setGrid(grid);
    s.comp.setCells(cells);
    // order, back to front: the APRS lines, the washed outlines, the range outline, the plugin's open trails
    trails.clear();
    for (auto& t : lines) { trails.push_back(std::move(t)); }
    for (auto& t : closed) { trails.push_back(std::move(t)); }
    for (auto& t : outline) { trails.push_back(std::move(t)); }
    for (auto& t : open) { trails.push_back(std::move(t)); }
    j.cellCount = static_cast<int>(cells.size());
    j.gridLevel = grid;
    j.labelCount = static_cast<int>(labels.size());
    j.lineCount = static_cast<int>(lines.size());
    sig += "J" + std::to_string(j.cellCount) + "." + std::to_string(grid) + "." + std::to_string(lines.size()) + "." + std::to_string(closed.size()) + std::to_string(outline.size());
    return status;
}

// The map page's model beyond the compositor: the switches, the legend rows the layers add, the credit line, the replay bar, the filter bar, the unit key.
void KitPublisher::fillMapJModel(MapSurface& s, MapPage* pg, const std::string& status, std::vector<::GridLabel>& labels, const std::string& baseCredit, ::MapModel& m, std::string& sig) {
    mj::MapJSurface& j = s.j;
    const double now = nowUnix();
    ::MapJModel jm{};
    jm.layers_open = j.layersOpen;
    std::vector<::LayerRow> rows;
    std::vector<std::string> blocked;
    for (int i = 0; i < mj::kLayerCount; ++i) {
        const mj::Layer l = static_cast<mj::Layer>(i);
        const mj::LayerInfo& info = mj::layerInfo(l);
        ::LayerRow r{};
        r.label = ss(tr(info.label));
        r.on = j.layers.get(l);
        r.enabled = !info.needsSource;
        r.tip = ss(info.needsSource ? tr(info.tip) + " " + tr(mj::blockedNote()) : tr(info.tip));
        if (info.needsSource) { blocked.push_back(tr(info.label)); }
        sig += std::string(r.on ? "1" : "0");
        rows.push_back(std::move(r));
    }
    jm.layers = model(std::move(rows));
    jm.blocked_line = ss(blocked.empty() ? std::string() : tr(mj::blockedNote()) + " " + joinNames(blocked) + ".");
    jm.units = ss(tr("Units") + ": " + tr(mj::unitChoiceName(j.units)));
    // the tile cache's line, read every five seconds and never made if there is no folder
    if (now - mj_.cacheReadSec > 5.0) {
        mj_.cacheReadSec = now;
        mj_.cacheHas = false;
        mj_.cacheLine = tr("No map tiles are kept on disk.");
        std::error_code ec;
        if (!mj_.tileCacheRoot.empty() && std::filesystem::exists(mj_.tileCacheRoot, ec)) {
            cascade::core::tilecache::TileCache cache(mj_.tileCacheRoot);
            mj_.cacheHas = cache.tileCount() > 0;
            mj_.cacheLine = mj::tileCacheText(cache.tileCount(), cache.totalBytes(), cascade::core::tilecache::Config{}.maxBytes, cache.stats(), trFn());
        }
    }
    jm.cache_text = ss(mj_.cacheLine);
    jm.cache_has = mj_.cacheHas;
    jm.status = ss(status);
    // replay
    ::ReplayModel rm{};
    rm.shown = j.replayBar;
    double first = 0.0, last = 0.0;
    const bool have = j.recorder.store().timeRange(first, last);
    rm.empty = !have;
    rm.active = j.replay.active;
    rm.playing = j.replay.playing;
    rm.pos = static_cast<float>(j.replay.active ? j.replay.fraction() : 1.0);
    rm.clock = ss(mj::clockText(j.replay.active ? j.replay.t : now));
    rm.behind = ss(!have ? tr("nothing recorded yet") : mj::behindText(j.replay.active ? j.replay.t1 : last, j.replay.active ? j.replay.t : last, trFn()));
    rm.speed = ss(mj::speedText(j.replay.speedIndex));
    jm.replay = rm;
    sig += "R" + std::to_string(j.replayBar) + std::to_string(rm.active) + std::to_string(rm.playing) + fmt("%.3f", rm.pos) + std::string(rm.clock) + std::string(rm.behind) + std::string(rm.speed) + std::to_string(rm.empty);
    // filter
    ::FilterModel fm{};
    fm.shown = j.filterBar;
    fm.enabled = j.filter.enabled;
    fm.hide_ground = j.filter.hideGround;
    fm.military = j.filter.militaryOnly;
    fm.callsign = ss(j.filter.callsign);
    fm.alt_min = ss(j.altMinText);
    fm.alt_max = ss(j.altMaxText);
    fm.type_text = ss(j.filter.typeText);
    fm.operator_text = ss(j.filter.operatorText);
    fm.plugin_fields = static_cast<bool>(mj_.filterDetail);
    fm.summary = ss(mj::filterSummary(j.filter, trFn()) + (j.hiddenByFilter > 0 ? "  |  " + cascade::core::formatText(tr("%d hidden").c_str(), j.hiddenByFilter) : std::string()));
    fm.note = ss(mj_.filterDetail ? std::string() : tr("Type, operator and military need the ADS-B plugin to say what each aircraft is: those three do not filter yet."));
    fm.alt_min_error = ss(j.altMinError);
    fm.alt_max_error = ss(j.altMaxError);
    jm.filter = fm;
    sig += "F" + std::to_string(j.filterBar) + std::to_string(j.filter.enabled) + std::to_string(j.filter.hideGround) + std::to_string(j.filter.militaryOnly) + j.filter.callsign + "|" + j.altMinText + "|" + j.altMaxText +
           j.filter.typeText + j.filter.operatorText + std::to_string(j.hiddenByFilter) + j.altMinError + j.altMaxError;
    jm.grid_labels = model(std::move(labels));
    sig += "G" + std::to_string(j.labelCount) + std::string(jm.units) + std::string(jm.status) + mj_.cacheLine + std::to_string(j.layersOpen);
    // the legend rows the layers add, after the altitude rows
    std::vector<::LegendRow> legend;
    if (m.legend) { for (std::size_t i = 0; i < m.legend->row_count(); ++i) { legend.push_back(*m.legend->row_data(i)); } }
    for (const mj::LegendEntry& e : mj::layerLegend(j.layers, !mj_.locatorMarks.empty())) { legend.push_back({ss(tr(e.label)), colourOf(e.rgba)}); }
    m.legend = model(std::move(legend));
    m.attribution = ss(mj::creditLine(std::string(m.attribution), j.layers, trFn()));
    m.j = jm;
    (void)pg;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the window's one callback
// ---------------------------------------------------------------------------------------------------------------------------------
void KitPublisher::mapJAct(int pageIndex, const std::string& op, const std::string& arg) {
    // the Antenna page's fields (page kind 13): the field's text, kept as typed
    if (op.rfind("ant-", 0) == 0) {
        struct Spec { const char* op; std::size_t slot; double lo, hi; };
        static const Spec specs[] = {{"ant-freq", 0, 0.01, 300000.0}, {"ant-distance", 1, 1e-9, 1e9}, {"ant-coax", 2, 1e-9, 100.0}, {"ant-vf", 3, 1e-9, 1.0}, {"ant-yagi", 4, 2.0, 20.0}};
        for (const Spec& sp : specs) {
            if (op != sp.op) { continue; }
            mj_.antennaText[sp.slot] = arg;
            double v = 0.0;
            if (!mj::parseNumber(arg, sp.lo, sp.hi, v)) { mj_.antennaError = FOX_TR_NOOP("That is not a number this field accepts."); return; }
            mj_.antennaError.clear();
            if (sp.slot == 0) { mj_.antenna.fMHz = v; }
            else if (sp.slot == 1) { mj_.antenna.distanceKm = v; }
            else if (sp.slot == 2) { mj_.antenna.coaxFraction = v; }
            else if (sp.slot == 3) { mj_.antenna.vf = v; }
            else { mj_.antenna.yagiElements = static_cast<int>(std::lround(v)); }
            return;
        }
        return;
    }
    if (pageIndex < 0 || pageIndex >= static_cast<int>(pages_.size()) || pages_[static_cast<std::size_t>(pageIndex)].kind != 5) { return; }
    MapSurface& s = surfaceFor(pages_[static_cast<std::size_t>(pageIndex)].plugin);
    mj::MapJSurface& j = s.j;
    const bool on = arg == "1";
    if (op == "layers") { j.layersOpen = !j.layersOpen; }
    else if (op == "layer") {
        const std::size_t colon = arg.find(':');
        if (colon == std::string::npos) { return; }
        const int idx = std::atoi(arg.substr(0, colon).c_str());
        if (idx < 0 || idx >= mj::kLayerCount) { return; }
        j.layers.set(static_cast<mj::Layer>(idx), arg.substr(colon + 1) == "1");
    }
    else if (op == "units") { j.units = mj::nextUnitSystem(j.units); }
    else if (op == "cache-clear") {
        std::error_code ec;
        if (!mj_.tileCacheRoot.empty() && std::filesystem::exists(mj_.tileCacheRoot, ec)) {
            cascade::core::tilecache::TileCache cache(mj_.tileCacheRoot);
            cache.clear();
        }
        mj_.cacheReadSec = -100.0;
    }
    else if (op == "replay-bar") {
        j.replayBar = !j.replayBar;
        if (!j.replayBar) { j.replay.close(); }
    }
    else if (op == "replay-toggle" || op == "replay-scrub" || op == "replay-speed" || op == "replay-now") {
        double first = 0.0, last = 0.0;
        const bool have = j.recorder.store().timeRange(first, last);
        if (op == "replay-now") { j.replay.close(); return; }
        if (op == "replay-speed") { j.replay.faster(); return; }
        if (!have) { return; }
        if (!j.replay.active) { j.replay.open(first, last); }
        else { j.replay.setRange(first, last); }
        if (op == "replay-toggle") { j.replay.toggle(); }
        else { j.replay.scrub(std::atof(arg.c_str())); }
    }
    else if (op == "filter-bar") { j.filterBar = !j.filterBar; }
    else if (op == "filter-on") { j.filter.enabled = on; }
    else if (op == "filter-ground") { j.filter.hideGround = on; if (on) { j.filter.enabled = true; } }
    else if (op == "filter-military") { j.filter.militaryOnly = on; if (on) { j.filter.enabled = true; } }
    else if (op == "filter-callsign") { j.filter.callsign = arg; if (!arg.empty()) { j.filter.enabled = true; } }
    else if (op == "filter-type") { j.filter.typeText = arg; if (!arg.empty()) { j.filter.enabled = true; } }
    else if (op == "filter-operator") { j.filter.operatorText = arg; if (!arg.empty()) { j.filter.enabled = true; } }
    else if (op == "filter-altmin" || op == "filter-altmax") {
        const bool isMin = op == "filter-altmin";
        std::string& text = isMin ? j.altMinText : j.altMaxText;
        std::string& err = isMin ? j.altMinError : j.altMaxError;
        text = arg;
        err.clear();
        bool trimmedEmpty = true;
        for (char c : arg) { if (c != ' ') { trimmedEmpty = false; } }
        if (trimmedEmpty) { (isMin ? j.filter.altMinOn : j.filter.altMaxOn) = false; return; }
        double v = 0.0;
        if (!mj::parseNumber(arg, -2000.0, 100000.0, v)) { err = tr("Not a number between -2000 and 100000."); (isMin ? j.filter.altMinOn : j.filter.altMaxOn) = false; return; }
        (isMin ? j.filter.altMinOn : j.filter.altMaxOn) = true;
        (isMin ? j.filter.altMinFt : j.filter.altMaxFt) = v;
        j.filter.enabled = true;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the Sky page (kind 12)
// ---------------------------------------------------------------------------------------------------------------------------------
void KitPublisher::fillSky(PageTab& t, const FoxReceiverState& state, std::string& sig) {
    ::SkyModel m{};
    const bool home = (state.flags & FOXAPI_RX_POSITION_SET) != 0u;
    const double now = nowUnix();
    m.position_text = ss(home ? "RX " + fmt("%.4f", state.rxLatDeg) + " " + fmt("%.4f", state.rxLonDeg) + "  |  " + mj::clockText(now) : tr("No receiver position: set it on a map page (Set RX here)."));
    m.note = ss(tr("Elevation is geometric: no refraction. A body under the horizon is greyed."));
    m.stellarium_note = ss(tr("Stellarium link: not built (it needs Stellarium installed)."));
    m.columns = model(std::vector<slint::SharedString>{ss(tr("Body")), ss(tr("Azimuth")), ss(tr("Elevation")), ss(tr("Note"))});
    m.widths = model(std::vector<float>{150.f, 80.f, 80.f, 0.f});
    std::vector<TableRow> rows;
    std::vector<::PolarDot> dots;
    const auto ink = [](const std::string& name) -> std::uint32_t {
        return name == "Sun" ? 0xf5a524ffu : name == "Moon" ? 0xe6e8ecffu : 0x7ad7ffffu;
    };
    if (home) {
        for (const mj::SkyRow& r : mj::skyRows(state.rxLatDeg, state.rxLonDeg, now)) {
            TableRow rw{};
            rw.cells = model(std::vector<slint::SharedString>{ss(tr(r.name)), ss(r.azText), ss(r.elText), ss(r.below ? tr("below the horizon") : std::string())});
            rw.kind = RowKind::Cells;
            rw.tone = r.below ? RowTone::Muted : RowTone::Normal;
            rw.band = (rows.size() % 2u) == 1u;
            rw.pressable = false;
            sig += r.name + r.azText + r.elText + ";";
            if (r.name == "Sun") { mj_.sunEl = r.elText; }
            rows.push_back(std::move(rw));
            const mj::PolarPoint p = mj::polarPoint(r.azDeg, r.elDeg);
            ::PolarDot d{};
            d.x = static_cast<float>((p.x + 1.0) / 2.0);
            d.y = static_cast<float>((p.y + 1.0) / 2.0);
            d.label = ss(tr(r.name));
            d.ink = colourOf(ink(r.name));
            d.below = p.below;
            dots.push_back(std::move(d));
        }
    }
    mj_.skyRows = static_cast<int>(rows.size());
    m.rows = model(std::move(rows));
    // the pass (the satellites plugin's az/el, hand-off): the provider's samples as a path in a 0..200 box
    std::string commands;
    m.pass_text = ss(tr("No pass to show: the satellites plugin does not hand its azimuth and elevation to the window yet."));
    if (mj_.passProvider) {
        const std::vector<mj::PassSample> pass = mj_.passProvider();
        const mj::PassSummary ps = mj::summarisePass(pass);
        if (ps.valid) {
            for (const mj::PolarPoint& p : mj::passTrace(pass)) { commands += (commands.empty() ? "M " : " L ") + fmt("%.1f", (p.x + 1.0) * 100.0) + " " + fmt("%.1f", (p.y + 1.0) * 100.0); }
            m.pass_text = ss(cascade::core::formatText(tr("Pass: rises at %03.0f, peaks at %.0f degrees (azimuth %03.0f), sets at %03.0f, %.0f min").c_str(), ps.aosAzDeg, ps.maxElDeg, ps.maxElAzDeg, ps.losAzDeg, ps.durationSec / 60.0));
            const mj::PolarPoint a = mj::polarPoint(ps.aosAzDeg, 0.0);
            ::PolarDot d{};
            d.x = static_cast<float>((a.x + 1.0) / 2.0);
            d.y = static_cast<float>((a.y + 1.0) / 2.0);
            d.label = ss(tr("rise"));
            d.ink = colourOf(0x3ddc84ffu);
            dots.push_back(std::move(d));
        }
    }
    m.pass_commands = ss(commands);
    m.pass_ink = colourOf(0x3ddc84ffu);
    m.dots = model(std::move(dots));
    sig += std::string(m.position_text) + commands + std::string(m.pass_text);
    t.sky = m;
    t.chip = ss(home ? "" : tr("NO POSITION"));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the Antenna page (kind 13)
// ---------------------------------------------------------------------------------------------------------------------------------
void KitPublisher::fillAntenna(PageTab& t, std::string& sig) {
    ::AntennaModel m{};
    m.freq = ss(mj_.antennaText[0]);
    m.distance = ss(mj_.antennaText[1]);
    m.coax_fraction = ss(mj_.antennaText[2]);
    m.vf = ss(mj_.antennaText[3]);
    m.yagi = ss(mj_.antennaText[4]);
    const std::string bad = mj::antennaError(mj_.antenna);
    m.error = ss(!mj_.antennaError.empty() ? tr(mj_.antennaError) : bad.empty() ? std::string() : tr(bad));
    m.columns = model(std::vector<slint::SharedString>{ss(tr("Item")), ss(tr("Metres")), ss(tr("Feet")), ss(tr("Note"))});
    m.widths = model(std::vector<float>{220.f, 110.f, 110.f, 0.f});
    std::vector<TableRow> rows;
    for (const mj::AntennaRow& r : mj::antennaRows(mj_.antenna)) {
        std::string item = tr(r.item);
        if (r.item.rfind("Yagi director ", 0) == 0) { item = tr("Yagi director") + r.item.substr(std::string("Yagi director").size()); }
        TableRow rw{};
        rw.cells = model(std::vector<slint::SharedString>{ss(item), ss(r.metres), ss(r.feet), ss(r.note.empty() ? std::string() : tr(r.note))});
        rw.kind = RowKind::Cells;
        rw.tone = RowTone::Normal;
        rw.band = (rows.size() % 2u) == 1u;
        rw.pressable = false;
        sig += item + r.metres + r.feet + r.note + ";";
        rows.push_back(std::move(rw));
    }
    mj_.antennaRows = static_cast<int>(rows.size());
    m.rows = model(std::move(rows));
    m.note = ss(tr("Lengths for a wire in free space. Starting values for a Yagi: tune with an antenna analyser."));
    sig += std::string(m.error) + std::string(m.freq) + std::string(m.distance) + std::string(m.coax_fraction) + std::string(m.vf) + std::string(m.yagi);
    t.antenna = m;
}

// ---------------------------------------------------------------------------------------------------------------------------------
std::string KitPublisher::mapJState(const std::string& what) const {
    const MapSurface* first = surfaces_.empty() ? nullptr : surfaces_.begin()->second.get();
    if (what == "antennaRows") { return std::to_string(mj_.antennaRows); }
    if (what == "antennaError") { return mj_.antennaError; }
    if (what == "dipole") { for (const mj::AntennaRow& r : mj::antennaRows(mj_.antenna)) { if (r.item == "Half-wave dipole") { return r.metres; } } return std::string(); }
    if (what == "loss") { for (const mj::AntennaRow& r : mj::antennaRows(mj_.antenna)) { if (r.item == "Free-space path loss") { return r.metres; } } return std::string(); }
    if (what == "sunEl") { return mj_.sunEl; }
    if (what == "passMax") { if (!mj_.passProvider) { return "-1"; } const mj::PassSummary ps = mj::summarisePass(mj_.passProvider()); return ps.valid ? fmt("%.0f", ps.maxElDeg) : std::string("-1"); }
    if (what == "skyRows") { return std::to_string(mj_.skyRows); }
    if (first == nullptr) { return std::string(); }
    const mj::MapJSurface& j = first->j;
    if (what == "layers") { std::string s; for (int i = 0; i < mj::kLayerCount; ++i) { s += j.layers.get(static_cast<mj::Layer>(i)) ? '1' : '0'; } return s; }
    if (what == "cells") { return std::to_string(j.cellCount); }
    if (what == "grid") { return std::to_string(j.gridLevel); }
    if (what == "labels") { return std::to_string(j.labelCount); }
    if (what == "lines") { return std::to_string(j.lineCount); }
    if (what == "hidden") { return std::to_string(j.hiddenByFilter); }
    if (what == "units") { return mj::unitChoiceName(j.units); }
    if (what == "replay") { return j.replay.active ? (j.replay.playing ? "playing" : "paused") : "live"; }
    if (what == "replaySpeed") { return mj::speedText(j.replay.speedIndex); }
    if (what == "replayPos") { return fmt("%.3f", j.replay.fraction()); }
    if (what == "recorded") {
        double a = 0, b = 0;
        return j.recorder.store().timeRange(a, b) ? fmt("%.0f", b - a) : std::string("-1");
    }
    if (what == "filter") { return j.filter.enabled ? "on" : "off"; }
    if (what == "conditions") { return std::to_string(mj::filterConditionCount(j.filter)); }
    return std::string();
}

}  // namespace foxsdr::slintkit
