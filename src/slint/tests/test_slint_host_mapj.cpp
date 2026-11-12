// SCREEN LANE J (docs/multi-radio-records/screen-j.md): the map page's added rows, no window. Sections:
//   A. the layers: defaults, the three blocked sources cannot be turned on, the legend and the credit line follow the switches, the path kinds
//   B. the compositor's new layers: cells (heat), the Maidenhead lines, a closed trail's wash, a trail of its own width, and what setTrails compares
//   C. heat, range outline and signal-level cells from the real models (generated aircraft and a generated NMEA track)
//   D. the locator grid's labels and the worked / heard cells
//   E. replay: clock, speeds, scrub, the live edge; the recorder over the real track store
//   F. the aircraft filter
//   G. the unit choice on the map's words, and the scale bar
//   H. the sky table and the polar plot of a pass
//   I. the antenna page's numbers, and the tile cache's words
//   J. the ABI constants copied here, read from plugin_abi.h
// Map data here is generated; no real call sign, position of a person or address.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "mapj_logic.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

using namespace foxsdr::slintmapj;
namespace access = cascade::core::access;
namespace slm = foxsdr::slintmap;

namespace {

std::string srcPath(const char* rel) {
    std::string p = __FILE__;
    for (int i = 0; i < 3; ++i) {
        const std::size_t cut = p.find_last_of("/\\");
        if (cut == std::string::npos) { break; }
        p.resize(cut);
    }
    return p + "/" + rel;
}
std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
std::string idTr(const std::string& s) { return s; }
const std::function<std::string(const std::string&)> kTr = idTr;

struct Pic {
    int w, h;
    std::vector<std::uint8_t> px;
    Pic(int w_, int h_) : w(w_), h(h_), px(static_cast<std::size_t>(w_) * h_ * 4u, 0) {}
    const std::uint8_t* at(int x, int y) const { return &px[(static_cast<std::size_t>(y) * w + x) * 4u]; }
};

// a compositor over an equirectangular, graticule-less, coastline-less view: the sea colour everywhere, so a washed pixel is plain to see
struct Rig {
    slm::TileCompositor c;
    Pic pic{400, 300};
    Rig() {
        c.setSize(400, 300);
        c.setView(10.0, 20.0, 40.0);       // 40 degrees across 400 px: 10 px a degree
        c.setLayers(false, false);
    }
    void compose() { CHECK(c.compose(pic.px.data(), pic.w, pic.h)); }
    bool differsFromSea(int x, int y) const {
        const std::uint8_t* p = pic.at(x, y);
        return !(p[0] == 0x0d && p[1] == 0x0f && p[2] == 0x12);
    }
};

void sectionLayers() {
    LayerSet s = LayerSet::defaults();
    CHECK(s.get(Layer::Airspaces));
    CHECK(s.get(Layer::AprsPaths));
    CHECK(!s.get(Layer::AdsbHeat));
    CHECK(!s.get(Layer::Locators));
    // the three sources that need terms and a key stay off, however many times they are asked
    for (Layer l : {Layer::WeatherRadar, Layer::ReceiverMaps, Layer::Repeaters}) {
        CHECK(layerInfo(l).needsSource);
        CHECK(!s.set(l, true));
        CHECK(!s.get(l));
        CHECK(s.set(l, false));
    }
    CHECK(s.set(Layer::AdsbHeat, true));
    CHECK(s.get(Layer::AdsbHeat));
    CHECK(!s.set(static_cast<Layer>(99), true));
    // only the layers that need no source are open
    int open = 0;
    for (int i = 0; i < kLayerCount; ++i) { if (!layerInfo(static_cast<Layer>(i)).needsSource) { ++open; } }
    CHECK(open == 6);
    CHECK(std::string(blockedNote()).find("terms") != std::string::npos);
    // legend: heat adds two rows, range one, the shading rows only when there is a worked / heard set
    LayerSet none;
    CHECK(layerLegend(none, true).empty());
    LayerSet l2;
    l2.set(Layer::AdsbHeat, true);
    CHECK(layerLegend(l2, false).size() == 2u);
    l2.set(Layer::AdsbRange, true);
    CHECK(layerLegend(l2, false).size() == 3u);
    LayerSet l3;
    l3.set(Layer::Locators, true);
    CHECK(layerLegend(l3, false).empty());
    CHECK(layerLegend(l3, true).size() == 2u);
    // credit: the base first, then each layer that is on and has a credit
    CHECK(creditLine("Base", none, kTr) == "Base");
    CHECK(creditLine("Base", l2, kTr).find("Base | Heat map") == 0u);
    CHECK(creditLine("", l3, kTr) == "Locator grid: Maidenhead");
    CHECK(creditLine("B", l2, [](const std::string& k) { return "<" + k + ">"; }).find("<Heat map: your own receiver's aircraft>") != std::string::npos);
    // the APRS path ids
    CHECK(isAprsPath("W0XX-1~path", 3u));
    CHECK(!isAprsPath("W0XX-1~path", 1u));
    CHECK(!isAprsPath("W0XX-1", 3u));
    CHECK(!isAprsPath("~path", 3u));
    CHECK(isClosedPath(1u) && isClosedPath(3u) && !isClosedPath(2u) && !isClosedPath(0u));
}

void sectionCompositor() {
    {   // a cell washes its box and nothing else
        Rig r;
        slm::Cell cell;
        cell.latMin = 10.0; cell.latMax = 12.0; cell.lonMin = 20.0; cell.lonMax = 22.0; cell.rgba = 0xff0000ffu;   // opaque red
        r.c.setCells({cell});
        r.compose();
        double fx = 0, fy = 0;
        CHECK(r.c.project(11.0, 21.0, fx, fy));
        const int x = static_cast<int>(fx * 400), y = static_cast<int>(fy * 300);
        CHECK(r.pic.at(x, y)[0] == 255 && r.pic.at(x, y)[1] == 0 && r.pic.at(x, y)[2] == 0);
        CHECK(!r.differsFromSea(x + 40, y));
        CHECK(!r.differsFromSea(x, y + 40));
        // the box is 20 px wide and 20 px tall (10 px a degree, 2 degrees)
        CHECK(r.differsFromSea(x + 8, y) && !r.differsFromSea(x + 13, y));
        CHECK(r.differsFromSea(x, y + 8) && !r.differsFromSea(x, y + 13));
        // translucent: alpha 128 lands halfway between the sea and the colour
        Rig q;
        cell.rgba = 0xff000080u;
        q.c.setCells({cell});
        q.compose();
        CHECK(q.pic.at(x, y)[0] > 100 && q.pic.at(x, y)[0] < 140);
        // a box smaller than a pixel is still one pixel
        Rig tiny;
        slm::Cell t2;
        t2.latMin = 11.0; t2.latMax = 11.001; t2.lonMin = 21.0; t2.lonMax = 21.001; t2.rgba = 0xffffffffu;
        tiny.c.setCells({t2});
        tiny.compose();
        CHECK(tiny.differsFromSea(x, y));
        // the world repeats: a cell at 20 + 360 east is drawn where 20 is when the view is wide enough to show both
        Rig wide;
        wide.c.setView(10.0, 20.0, 400.0);
        slm::Cell far;
        far.latMin = 10.0; far.latMax = 12.0; far.lonMin = 380.0; far.lonMax = 382.0; far.rgba = 0xff0000ffu;
        wide.c.setCells({far});
        wide.compose();
        double wx = 0, wy = 0;
        CHECK(wide.c.project(11.0, 21.0, wx, wy));
        CHECK(wide.differsFromSea(static_cast<int>(wx * 400), static_cast<int>(wy * 300)));
    }
    {   // bad cells and the cap
        slm::TileCompositor c;
        c.setSize(64, 64);
        slm::Cell nan;
        nan.latMin = std::nan("");
        c.setCells({nan});
        std::vector<std::uint8_t> buf(64 * 64 * 4);
        CHECK(c.compose(buf.data(), 64, 64));
        slm::Cell good;
        good.latMax = 1.0; good.lonMax = 1.0; good.rgba = 0xffffffffu;
        std::vector<slm::Cell> many(slm::TileCompositor::kMaxCells + 100, good);
        c.setCells(many);                                         // capped, not refused
        CHECK(c.compose(buf.data(), 64, 64));
        c.setCells(many);
        CHECK(!c.compose(buf.data(), 64, 64));                    // the same cells: nothing changed
    }
    {   // the Maidenhead lines: squares are 2 x 1 degrees, 20 x 10 px here; lines at even longitudes and every latitude
        Rig r;
        r.c.setView(10.0, 20.0, 40.0);
        r.c.setGrid(2);
        CHECK(r.c.gridLevel() == 2);
        r.compose();
        double fx = 0, fy = 0;
        CHECK(r.c.project(10.5, 20.0, fx, fy));                  // lon 20 is on an even degree: a vertical line
        CHECK(r.differsFromSea(static_cast<int>(std::floor(fx * 400)), static_cast<int>(fy * 300)));
        CHECK(r.c.project(10.5, 21.0, fx, fy));                  // lon 21 is mid-square: no line through the sea at this height
        CHECK(!r.differsFromSea(static_cast<int>(fx * 400), static_cast<int>(fy * 300)));
        // level 3 at this zoom is closer than kMinGridLinePx: drawn as level 2 (a grid is never a smear)
        Rig r3;
        r3.c.setGrid(3);
        r3.compose();
        Rig r2;
        r2.c.setGrid(2);
        r2.compose();
        CHECK(r3.pic.px == r2.pic.px);
        // off: nothing
        Rig r0;
        r0.compose();
        bool any = false;
        for (int y = 0; y < 300 && !any; ++y) { for (int x = 0; x < 400; ++x) { if (r0.differsFromSea(x, y)) { any = true; break; } } }
        CHECK(!any);
        r0.c.setGrid(9);
        CHECK(r0.c.gridLevel() == 3);
        r0.c.setGrid(-4);
        CHECK(r0.c.gridLevel() == 0);
    }
    {   // a closed trail is washed inside under its stroke; an open one is not
        Rig r;
        slm::Trail sq;
        sq.points = {{8.0, 15.0}, {8.0, 25.0}, {14.0, 25.0}, {14.0, 15.0}};
        sq.closed = true;
        sq.rgba = 0xf0aa3cffu;
        sq.fillAlpha = 90;
        sq.widthPx = 2.f;
        r.c.setTrails({sq});
        r.compose();
        double fx = 0, fy = 0;
        CHECK(r.c.project(11.0, 20.0, fx, fy));
        CHECK(r.differsFromSea(static_cast<int>(fx * 400), static_cast<int>(fy * 300)));          // the inside
        Rig r2;
        sq.fillAlpha = 0;
        r2.c.setTrails({sq});
        r2.compose();
        CHECK(!r2.differsFromSea(static_cast<int>(fx * 400), static_cast<int>(fy * 300)));         // no wash, no inside
        sq.closed = false;
        sq.fillAlpha = 90;
        Rig r3;
        r3.c.setTrails({sq});
        r3.compose();
        CHECK(!r3.differsFromSea(static_cast<int>(fx * 400), static_cast<int>(fy * 300)));         // open: never washed
    }
    {   // a trail with a width of its own is that wide whatever the Display setting says
        Rig thin, wide;
        thin.c.setTrailLook(10, 48, slm::TrailStyle::Line);
        wide.c.setTrailLook(10, 48, slm::TrailStyle::Line);
        slm::Trail a;
        a.points = {{10.0, 10.0}, {10.0, 30.0}};
        a.rgba = 0xffffffffu;
        a.widthPx = 1.f;
        thin.c.setTrails({a});
        thin.compose();
        a.widthPx = 0.f;                                       // the Display width: 10 px
        wide.c.setTrails({a});
        wide.compose();
        double fx = 0, fy = 0;
        CHECK(thin.c.project(10.0, 20.0, fx, fy));
        const int x = static_cast<int>(fx * 400), y = static_cast<int>(fy * 300);
        int thinRows = 0, wideRows = 0;
        for (int dy = -12; dy <= 12; ++dy) {
            if (thin.differsFromSea(x, y + dy)) { ++thinRows; }
            if (wide.differsFromSea(x, y + dy)) { ++wideRows; }
        }
        CHECK(thinRows >= 1 && thinRows <= 3);
        CHECK(wideRows >= 9 && wideRows <= 12);
        // setTrails notices a changed width and a changed fill (the compare covers them)
        slm::Trail b = a;
        b.widthPx = 1.f;
        thin.c.setTrails({a});
        CHECK(thin.c.compose(thin.pic.px.data(), 400, 300));
        thin.c.setTrails({a});
        CHECK(!thin.c.compose(thin.pic.px.data(), 400, 300));
        thin.c.setTrails({b});
        CHECK(thin.c.compose(thin.pic.px.data(), 400, 300));
        b.fillAlpha = 5;
        thin.c.setTrails({b});
        CHECK(thin.c.compose(thin.pic.px.data(), 400, 300));
    }
}

void sectionModels() {
    namespace ac = cascade::core::adsbcov;
    ac::Config cfg;
    cfg.rxLat = 51.0;
    cfg.rxLon = 0.0;
    cfg.cellDeg = 0.1;
    ac::AdsbCoverage cov(cfg);
    // generated aircraft: 40 positions east of the receiver at 20000 ft, one cell hit ten times, noise-free
    const double t0 = 1.7e9;
    for (int i = 0; i < 40; ++i) { CHECK(cov.add(t0 + i, 51.0 + 0.001 * i, 0.5 + 0.0002 * i, 20000.0) == ac::AddResult::Added); }
    for (int i = 0; i < 9; ++i) { cov.add(t0 + 50 + i, 51.5, 1.5, 33000.0); }
    cov.add(t0 + 60, 51.0, 2.5, 33000.0);
    const auto heat = cov.heat(t0 + 100);
    const auto cells = heatCells(heat, cfg.cellDeg);
    CHECK(!cells.empty() && cells.size() == heat.size());
    // the hottest cell (the 40 repeats, strongest first) is the hottest colour; the single hit is colder
    CHECK(cells.front().rgba == heatRgba(1.0));
    CHECK((cells.back().rgba >> 24) != (cells.front().rgba >> 24) || (cells.back().rgba & 0xffu) != (cells.front().rgba & 0xffu));
    CHECK_NEAR(cells.front().latMax - cells.front().latMin, 0.1, 1e-9);
    CHECK(heatCells({}, 0.1).empty());
    CHECK(heatCells(heat, 0.0).empty());
    CHECK(heatCells(heat, std::nan("")).empty());
    // the ramp: cold is blue, hot is red, the alpha grows
    CHECK((heatRgba(0.0) >> 24) < (heatRgba(0.5) >> 24));
    CHECK(((heatRgba(1.0) >> 16) & 0xffu) < ((heatRgba(0.5) >> 16) & 0xffu));
    CHECK((heatRgba(0.0) & 0xffu) < (heatRgba(1.0) & 0xffu));
    CHECK(((heatRgba(0.0) >> 8) & 0xffu) > ((heatRgba(0.0) >> 24) & 0xffu));        // cold is bluer than red
    CHECK(((heatRgba(1.0) >> 24) & 0xffu) > ((heatRgba(1.0) >> 8) & 0xffu));        // hot is redder than blue
    CHECK(heatRgba(std::nan("")) == heatRgba(0.0));
    CHECK(heatRgba(7.0) == heatRgba(1.0));
    // the outline: a polygon through the receiver where nothing was heard, closed, washed, one colour per band
    const auto poly = cov.outlinePolygon(t0 + 100, -1);
    std::vector<std::pair<double, double>> pts;
    for (const auto& p : poly) { pts.emplace_back(p.lat, p.lon); }
    const slm::Trail tr = outlineTrail(pts, 4);
    CHECK(tr.closed && tr.fillAlpha > 0u && tr.widthPx >= 1.f && tr.points.size() == poly.size());
    CHECK(outlineRgba(0) != outlineRgba(1) && outlineRgba(-1) != outlineRgba(0) && outlineRgba(99) == outlineRgba(-1));
    CHECK(cov.maxRangeKm(t0 + 100, 4) > 100.0);                      // the 33000 ft aircraft 2.5 degrees east
    CHECK(cov.maxRangeKm(t0 + 100, 0) == 0.0);
    // APRS path trail: thin and faint
    const slm::Trail ap = aprsPathTrail({{51.0, 0.0}, {51.5, 0.5}});
    CHECK(ap.widthPx == 1.f && (ap.rgba & 0xffu) < 128u && !ap.closed);
    // signal level against position from a generated NMEA track (checksummed GGA sentences)
    cascade::core::rfpos::Config rc;
    rc.cellMeters = 50.0;
    cascade::core::rfpos::RfPositionLog log(rc);
    const auto sentence = [](double latMin, double lonMin) {
        char body[128];
        std::snprintf(body, sizeof body, "GPGGA,120000,51%07.4f,N,000%07.4f,E,1,08,0.9,50.0,M,47.0,M,,", latMin, lonMin);
        unsigned cs = 0;
        for (const char* p = body; *p; ++p) { cs ^= static_cast<unsigned char>(*p); }
        char line[160];
        std::snprintf(line, sizeof line, "$%s*%02X", body, cs);
        return std::string(line);
    };
    int stored = 0;
    for (int i = 0; i < 30; ++i) {
        const double t = 1000.0 + i;
        log.onNmeaLine(sentence(0.0 + 0.01 * i, 0.0 + 0.02 * i), t);
        if (log.addReading(t, -90.0 + i, 100e6) == cascade::core::rfpos::ReadingResult::Stored) { ++stored; }
    }
    CHECK(stored >= 20);
    const auto lc = levelCells(log.cells(), rc.cellMeters);
    CHECK(!lc.empty());
    // weakest is bluest, strongest is reddest
    std::uint32_t minR = 255, maxR = 0;
    for (const auto& c : lc) { minR = std::min<std::uint32_t>(minR, c.rgba >> 24); maxR = std::max<std::uint32_t>(maxR, c.rgba >> 24); }
    CHECK(maxR > minR);
    CHECK(levelCells({}, 25.0).empty());
    CHECK(levelCells(log.cells(), 0.0).empty());
    CHECK(levelRgba(-90.0, -90.0, -60.0) == heatRgba(0.0) - (heatRgba(0.0) & 0xffu) + 140u);
    // a cell is about cellMeters across
    CHECK_NEAR((lc.front().latMax - lc.front().latMin) * 111320.0, 50.0, 1e-6);
}

void sectionLocator() {
    CHECK(gridLevelFor(360.0) == 1 && gridLevelFor(61.0) == 1);
    CHECK(gridLevelFor(60.0) == 2 && gridLevelFor(2.0) == 2);
    CHECK(gridLevelFor(1.5) == 3 && gridLevelFor(0.01) == 3);
    CHECK(gridLevelFor(0.0) == 0 && gridLevelFor(-5.0) == 0 && gridLevelFor(std::nan("")) == 0);
    // squares: the cell centred on 51.5 N 0.5 W is IO91 (a known locator: Greenwich meridian is the IO91/JO01 boundary)
    const auto sq = gridLabels(51.5, -0.5, 4.0, 3.0, 2, 200);
    CHECK(!sq.empty());
    bool found = false;
    for (const auto& g : sq) {
        if (g.text == "IO91") { found = true; CHECK_NEAR(g.lat, 51.5, 1e-9); CHECK_NEAR(g.lon, -1.0, 1e-9); }
        CHECK(g.text.size() == 4u);
    }
    CHECK(found);
    CHECK(sq.front().text == "IO91" || sq.front().text == "JO01" || sq.front().text == "IO81" || sq.front().text == "JO11");   // nearest the centre first
    const auto fields = gridLabels(50.0, 0.0, 120.0, 60.0, 1, 500);
    CHECK(!fields.empty());
    for (const auto& g : fields) { CHECK(g.text.size() == 2u); }
    const auto sub = gridLabels(51.5, -0.5, 1.0, 0.6, 3, 500);
    CHECK(!sub.empty());
    for (const auto& g : sub) { CHECK(g.text.size() == 6u); }
    // the cap, a bad level and a view of nothing
    CHECK(gridLabels(51.5, -0.5, 4.0, 3.0, 2, 3).size() == 3u);
    CHECK(gridLabels(51.5, -0.5, 4.0, 3.0, 0, 3).empty());
    CHECK(gridLabels(51.5, -0.5, 4.0, 3.0, 4, 3).empty());
    CHECK(gridLabels(51.5, -0.5, 4.0, 3.0, 2, 0).empty());
    CHECK(gridLabels(51.5, std::nan(""), 4.0, 3.0, 2, 9).empty());
    CHECK(gridLabels(0.0, 0.0, 360.0, 180.0, 3, 9999).empty());       // 4000 x 2000 subsquares: far too many to name
    // across the antimeridian the labels still name real squares (the position runs past 180)
    const auto am = gridLabels(10.0, 179.0, 6.0, 4.0, 2, 200);
    bool past = false;
    for (const auto& g : am) { if (g.lon > 180.0) { past = true; } CHECK(g.text.size() == 4u); }
    CHECK(past);
    // worked / heard cells
    const auto cells = locatorCells({{"IO91", MarkState::Heard}, {"JO01", MarkState::Worked}, {"IO91", MarkState::Worked}, {"nonsense", MarkState::Worked}, {"IO91wm", MarkState::Heard}});
    CHECK(cells.size() == 3u);                       // IO91 (worked beats heard), JO01, IO91wm; the nonsense one is dropped
    CHECK_NEAR(cells[0].lonMax - cells[0].lonMin, 2.0, 1e-9);
    CHECK_NEAR(cells[0].latMax - cells[0].latMin, 1.0, 1e-9);
    CHECK(cells[0].rgba == cells[1].rgba);           // both worked
    CHECK(cells[2].rgba != cells[0].rgba);           // heard
    CHECK_NEAR(cells[2].lonMax - cells[2].lonMin, 2.0 / 24.0, 1e-9);
    CHECK(locatorCells({}).empty());
}

void sectionReplay() {
    Replay r;
    CHECK(!r.active && r.fraction() == 0.0);
    r.tick(5.0);
    CHECK(r.t == 0.0);                               // not active: nothing moves
    r.open(1000.0, 5000.0);
    CHECK(r.active && !r.playing);
    CHECK_NEAR(r.t, 4400.0, 1e-9);                   // ten minutes behind the newest
    r.tick(5.0);
    CHECK_NEAR(r.t, 4400.0, 1e-9);                   // paused
    r.play();
    r.tick(10.0);
    CHECK_NEAR(r.t, 4410.0, 1e-9);
    r.faster();
    CHECK(r.speedIndex == 1 && speedText(r.speedIndex) == "4x");
    r.tick(10.0);
    CHECK_NEAR(r.t, 4450.0, 1e-9);
    r.setSpeed(3);
    CHECK(speedText(3) == "64x");
    r.faster();
    CHECK(r.speedIndex == 0);                        // wraps to the slowest
    r.setSpeed(99);
    CHECK(r.speedIndex == kReplaySpeeds - 1);
    r.tick(100.0);                                   // 6400 s at 64x: past the newest instant
    CHECK(r.t == 5000.0 && !r.playing);
    r.play();                                        // from the end starts again at the start
    CHECK(r.t == 1000.0 && r.playing);
    r.pause();
    r.scrub(0.5);
    CHECK_NEAR(r.t, 3000.0, 1e-9);
    r.scrub(2.0);
    CHECK(r.t == 5000.0);
    r.scrub(-1.0);
    CHECK(r.t == 1000.0);
    r.scrub(std::nan(""));
    CHECK(r.t == 1000.0);
    r.setRange(2000.0, 4000.0);                      // the store shed its oldest: t stays inside
    CHECK(r.t == 2000.0);
    r.setRange(4000.0, 2000.0);                      // nonsense is ignored
    CHECK(r.t0 == 2000.0 && r.t1 == 4000.0);
    r.tick(-5.0);
    r.tick(std::nan(""));
    CHECK(r.t == 2000.0);
    r.close();
    CHECK(!r.active && !r.playing);
    Replay s;
    s.open(100.0, 200.0);                            // shorter than ten minutes: starts at the start
    CHECK(s.t == 100.0);
    Replay bad;
    bad.open(5.0, 1.0);
    CHECK(!bad.active);
    bad.open(std::nan(""), 1.0);
    CHECK(!bad.active);
    // words
    CHECK(clockText(0.0) == "00:00:00 UTC");
    CHECK(clockText(1700000000.0) == "22:13:20 UTC");
    CHECK(clockText(-1.0) == "--:--:-- UTC" && clockText(std::nan("")) == "--:--:-- UTC");
    CHECK(behindText(100.0, 100.0, kTr) == "live edge");
    CHECK(behindText(100.0, 70.0, kTr) == "30 s behind");
    CHECK(behindText(1000.0, 670.0, kTr) == "5 min 30 s behind");
    CHECK(behindText(100.0, 200.0, kTr) == "live edge");

    // the recorder over the real store: throttled, three kinds, hostile ids counted and skipped
    TrackRecorder rec;
    std::vector<FeedItem> items = {{kKindAircraft, "ABC123", 51.0, 0.0, 9000.0, true}, {kKindVessel, "VES1", 50.5, -1.0, 0.0, false}, {kKindStation, "STN-1", 52.0, 1.0, 0.0, false},
                                   {kKindSatellite, "SAT1", 0.0, 0.0, 0.0, false}, {kKindUnknown, "UNK", 0.0, 0.0, 0.0, false}, {kKindAircraft, "has space", 1.0, 1.0, 0.0, false},
                                   {kKindAircraft, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 1.0, 1.0, 0.0, false}};
    const double base = 1.7e9;
    CHECK(rec.feed(base, items) == 3);
    CHECK(rec.skippedIds() == 2u);
    CHECK(rec.feed(base + 1.0, items) == 0);         // under the two-second gap
    CHECK(rec.feed(base + 2.5, items) == 3);
    double first = 0, last = 0;
    CHECK(rec.store().timeRange(first, last));
    CHECK_NEAR(first, base, 1e-6);
    CHECK_NEAR(last, base + 2.5, 1e-6);
    items[0].lat = 51.1;
    rec.feed(base + 5.0, items);
    // the snapshot at T: interpolated between the two points either side
    const auto snap = rec.store().snapshot(base + 3.75);
    bool sawAircraft = false;
    for (const auto& e : snap) {
        if (e.id == "ABC123") { sawAircraft = true; CHECK(e.at.lat > 51.0 && e.at.lat < 51.1); CHECK_NEAR(e.at.altM, 9000.0, 1e-6); }
    }
    CHECK(sawAircraft);
    CHECK(rec.store().snapshot(base - 1000.0).empty());   // before anything was heard
    CHECK(rec.feed(std::nan(""), items) == 0);
    rec.clear();
    CHECK(rec.skippedIds() == 0u && rec.store().snapshot(base + 3.0).empty());
}

void sectionFilter() {
    FilterSubject a;
    a.id = "ABC123"; a.label = "FOXAIR1"; a.altM = 10668.0; a.hasAlt = true; a.speedMps = 230.0; a.hasSpeed = true;      // 35000 ft
    FilterSubject g;
    g.id = "DEF456"; g.label = "GROUND2"; g.altM = 3.0; g.hasAlt = true; g.speedMps = 4.0; g.hasSpeed = true;
    FilterSubject u;
    u.id = "GHI789"; u.label = "NOALT3";
    AircraftFilter f;
    CHECK(filterPasses(f, a) && filterPasses(f, g) && filterPasses(f, u));       // disabled: everything
    f.enabled = true;
    CHECK(filterPasses(f, a) && filterPasses(f, g) && filterPasses(f, u));       // enabled with no condition: everything
    CHECK(filterConditionCount(f) == 0);
    f.altMinOn = true; f.altMinFt = 20000.0;
    CHECK(filterPasses(f, a) && !filterPasses(f, g) && !filterPasses(f, u));     // an unknown altitude cannot be shown to be above
    f.altMaxOn = true; f.altMaxFt = 30000.0;
    CHECK(!filterPasses(f, a));
    f.altMaxFt = 40000.0;
    CHECK(filterPasses(f, a));
    f.altMinOn = f.altMaxOn = false;
    f.hideGround = true;
    CHECK(isOnGround(g) && !isOnGround(a) && !isOnGround(u));
    CHECK(filterPasses(f, a) && !filterPasses(f, g) && filterPasses(f, u));
    FilterSubject taxiFast = g;
    taxiFast.speedMps = 60.0;                                                   // 3 m up at 60 m/s is a landing roll or a take-off: in the air for this rule
    CHECK(!isOnGround(taxiFast));
    FilterSubject noSpeed = g;
    noSpeed.hasSpeed = false;
    CHECK(isOnGround(noSpeed));
    f.hideGround = false;
    f.callsign = "foxa";
    CHECK(filterPasses(f, a) && !filterPasses(f, g));
    f.callsign = "abc1";
    CHECK(filterPasses(f, a));                                                  // the address matches too
    f.callsign = "";
    // what only the plugin knows: not known = not shown, never guessed
    f.militaryOnly = true;
    CHECK(filterNeedsPluginFields(f) && !filterPasses(f, a));
    a.known = true; a.military = true; a.typeCode = "C130"; a.operatorName = "Test Air Force";
    CHECK(filterPasses(f, a));
    f.militaryOnly = false;
    f.typeText = "c13";
    CHECK(filterPasses(f, a) && !filterPasses(f, g));
    f.typeText = "";
    f.operatorText = "air force";
    CHECK(filterPasses(f, a) && filterNeedsPluginFields(f));
    f.operatorText = "";
    CHECK(!filterNeedsPluginFields(f));
    // the count and its words
    f.altMinOn = true; f.hideGround = true; f.callsign = "x";
    CHECK(filterConditionCount(f) == 3);
    CHECK(filterSummary(f, kTr) == "3 conditions");
    f.altMinOn = false; f.hideGround = false;
    CHECK(filterSummary(f, kTr) == "1 condition");
    f.callsign = "";
    CHECK(filterSummary(f, kTr) == "no conditions");
    f.callsign = "x";
    f.enabled = false;
    CHECK(filterSummary(f, kTr) == "no conditions");
    // numbers typed into the fields
    double v = 0;
    CHECK(parseNumber("12000", 0, 60000, v) && v == 12000.0);
    CHECK(parseNumber(" 12 000 ", 0, 60000, v) && v == 12000.0);
    CHECK(parseNumber("12,5", 0, 60000, v) && v == 12.5);
    CHECK(parseNumber("1,200.5", 0, 60000, v) && v == 1200.5);
    CHECK(!parseNumber("", 0, 60000, v));
    CHECK(!parseNumber("abc", 0, 60000, v));
    CHECK(!parseNumber("12x", 0, 60000, v));
    CHECK(!parseNumber("-1", 0, 60000, v));
    CHECK(!parseNumber("70000", 0, 60000, v));
    CHECK(!parseNumber("nan", 0, 60000, v));
    CHECK(!parseNumber("inf", 0, 60000, v));
}

void sectionUnits() {
    using access::UnitSystem;
    // Automatic is what the map has always printed: an aircraft's altitude in feet, a vessel's speed in knots
    CHECK(altitudeText(UnitSystem::Automatic, kKindAircraft, 10668.0).find("ft") != std::string::npos);
    CHECK(altitudeText(UnitSystem::Metric, kKindAircraft, 10668.0).find(" m") != std::string::npos);
    CHECK(altitudeText(UnitSystem::Imperial, kKindAircraft, 10668.0) == "35000 ft");
    CHECK(speedTextOf(UnitSystem::Imperial, kKindAircraft, 100.0).find("mph") != std::string::npos);
    CHECK(speedTextOf(UnitSystem::Aviation, kKindVessel, 10.0).find("kt") != std::string::npos);
    CHECK(distanceText(UnitSystem::Nautical, kKindVessel, 1852.0) == "1.0 nmi" || distanceText(UnitSystem::Nautical, kKindVessel, 1852.0).find("nmi") != std::string::npos);
    CHECK(altitudeText(UnitSystem::Metric, kKindAircraft, std::nan("")).empty());
    CHECK(objectKindOf(kKindSatellite) == access::ObjectKind::Satellite && objectKindOf(kKindStation) == access::ObjectKind::Other && objectKindOf(99) == access::ObjectKind::Other);
    // the choice cycles through every system once
    UnitSystem s = UnitSystem::Automatic;
    int steps = 0;
    do { s = nextUnitSystem(s); ++steps; } while (s != UnitSystem::Automatic && steps < 20);
    CHECK(steps == access::kUnitSystemCount);
    CHECK(unitChoiceName(UnitSystem::Automatic) == "Automatic" && unitChoiceName(UnitSystem::Nautical) == "Nautical");
    // scale bar: 1000 px wide, 100 m a pixel: 0.3 of the width is 30 km
    ScaleBar b = scaleBar(UnitSystem::Metric, 100.0, 1000.0);
    CHECK(b.text == "20 km");
    CHECK_NEAR(b.frac, 20000.0 / 100.0 / 1000.0, 1e-12);
    b = scaleBar(UnitSystem::Imperial, 100.0, 1000.0);
    CHECK(b.text == "10 mi");                                   // 30 km is 18.6 mi: the largest 1-2-5 below it is 10
    CHECK_NEAR(b.frac, 10.0 * 1609.344 / 100.0 / 1000.0, 1e-12);
    b = scaleBar(UnitSystem::Aviation, 100.0, 1000.0);
    CHECK(b.text == "10 nmi");
    b = scaleBar(UnitSystem::Nautical, 1.0, 1000.0);            // 300 m across: under a nautical mile, so metres
    CHECK(b.text == "200 m");
    b = scaleBar(UnitSystem::Imperial, 0.5, 1000.0);            // 150 m = 492 ft
    CHECK(b.text == "200 ft");
    b = scaleBar(UnitSystem::Metric, 0.001, 1000.0);            // 0.3 m across: no bar rather than a wrong one
    CHECK(b.text.empty() && b.frac == 0.0);
    CHECK(scaleBar(UnitSystem::Metric, 0.0, 1000.0).text.empty());
    CHECK(scaleBar(UnitSystem::Metric, 100.0, -3.0).text.empty());
    CHECK(scaleBar(UnitSystem::Metric, std::nan(""), 1000.0).text.empty());
    // every bar is at most 30 percent of the pane and at least 6 percent (a 1-2-5 step is never under a fifth of the next)
    for (double mpp : {0.3, 1.0, 7.0, 33.0, 250.0, 5000.0}) {
        for (UnitSystem u : {UnitSystem::Metric, UnitSystem::Imperial, UnitSystem::Aviation, UnitSystem::Nautical}) {
            const ScaleBar x = scaleBar(u, mpp, 900.0);
            if (x.text.empty()) { continue; }
            CHECK(x.frac <= 0.3 + 1e-9 && x.frac >= 0.06);
        }
    }
}

void sectionSky() {
    // a generated place and time: the Sun at the equator on the equinox noon at longitude 0 is nearly overhead
    const double equinoxNoon = 1710936000.0;      // 2024-03-20 12:00:00 UTC, within a few minutes of the equinox
    const auto rows = skyRows(0.0, 0.0, equinoxNoon);
    CHECK(rows.size() == 7u);
    CHECK(rows[0].name == "Sun" && rows[1].name == "Moon");
    CHECK(rows[0].elDeg > 80.0 && !rows[0].below);
    CHECK(rows[0].azText.size() >= 3u);
    for (const auto& r : rows) { CHECK(r.below == (r.elDeg < 0.0)); }
    // midnight: the Sun is under the horizon
    CHECK(skyRows(0.0, 0.0, equinoxNoon + 12.0 * 3600.0)[0].below);
    CHECK(skyRows(95.0, 0.0, equinoxNoon).empty());
    CHECK(skyRows(0.0, 0.0, std::nan("")).empty());
    // the polar plot: zenith at the centre, the horizon on the rim, north up, east to the right
    PolarPoint p = polarPoint(0.0, 90.0);
    CHECK_NEAR(p.x, 0.0, 1e-12);
    CHECK_NEAR(p.y, 0.0, 1e-12);
    p = polarPoint(0.0, 0.0);
    CHECK_NEAR(p.x, 0.0, 1e-12);
    CHECK_NEAR(p.y, -1.0, 1e-12);
    p = polarPoint(90.0, 0.0);
    CHECK_NEAR(p.x, 1.0, 1e-12);
    CHECK_NEAR(p.y, 0.0, 1e-9);
    p = polarPoint(180.0, 45.0);
    CHECK_NEAR(p.x, 0.0, 1e-9);
    CHECK_NEAR(p.y, 0.5, 1e-12);
    p = polarPoint(270.0, -10.0);
    CHECK(p.below);
    CHECK_NEAR(std::hypot(p.x, p.y), 1.0, 1e-12);
    CHECK(polarPoint(std::nan(""), 10.0).below);
    // a pass: the made-up one rises at 215 degrees, peaks at 64, sets at 45
    const auto pass = madeUpPass();
    const PassSummary s = summarisePass(pass);
    CHECK(s.valid);
    CHECK_NEAR(s.maxElDeg, 64.0, 0.5);
    CHECK_NEAR(s.aosAzDeg, 215.0, 1e-9);
    CHECK_NEAR(s.losAzDeg, std::fmod(405.0, 360.0), 1e-9);
    CHECK_NEAR(s.durationSec, 600.0, 1e-9);
    const auto trace = passTrace(pass);
    CHECK(trace.size() == pass.size() - 0u || trace.size() >= pass.size() - 2u);
    for (const auto& q : trace) { CHECK(std::hypot(q.x, q.y) <= 1.0 + 1e-9); }
    // samples under the horizon are left out of the summary and the trace
    std::vector<PassSample> low = {{0.0, 10.0, -5.0}, {10.0, 20.0, 5.0}, {20.0, 30.0, 40.0}, {30.0, 40.0, -3.0}};
    const PassSummary ls = summarisePass(low);
    CHECK(ls.valid && ls.aosAzDeg == 20.0 && ls.losAzDeg == 30.0 && ls.maxElDeg == 40.0 && ls.durationSec == 10.0);
    CHECK(passTrace(low).size() == 2u);
    CHECK(!summarisePass({}).valid);
    CHECK(!summarisePass({{0.0, 0.0, -1.0}}).valid);
}

void sectionAntenna() {
    AntennaInput in;
    in.fMHz = 14.2;
    CHECK(antennaError(in).empty());
    const auto rows = antennaRows(in);
    CHECK(!rows.empty());
    const auto find = [&](const std::string& item) -> const AntennaRow* {
        for (const auto& r : rows) { if (r.item == item) { return &r; } }
        return nullptr;
    };
    const AntennaRow* w = find("Wavelength");
    CHECK(w != nullptr && w->metres == "21.11 m");           // 299792458 / 14.2e6 = 21.1121
    const AntennaRow* d = find("Half-wave dipole");
    CHECK(d != nullptr && d->metres == "10.07 m");           // 0.5 * 21.1121 * 0.954
    CHECK(d != nullptr && d->feet == "33.04 ft");
    CHECK(find("Quarter-wave ground plane") != nullptr && find("Quarter-wave ground plane")->metres == "5.04 m");
    CHECK(find("Radial, each") != nullptr && find("Radial, each")->metres == "5.29 m");
    CHECK(find("Yagi reflector") != nullptr && !find("Yagi reflector")->note.empty());
    CHECK(find("Yagi driven element") != nullptr && find("Yagi director 1") != nullptr && find("Yagi director 2") == nullptr);   // 3 elements: one director
    CHECK(find("Yagi boom") != nullptr);
    CHECK(find("Coax length") != nullptr);
    const AntennaRow* c = find("Coax length");
    CHECK(c != nullptr && c->metres == "3.48 m");            // 0.66 * 0.25 * 21.1121
    const AntennaRow* loss = find("Free-space path loss");
    CHECK(loss != nullptr && loss->metres == "95.5 dB" && loss->feet.empty() && loss->note.empty());   // 20 log10(100) + 20 log10(14.2) + 32.44
    // inside ten wavelengths the formula is said not to hold
    AntennaInput near = in;
    near.distanceKm = 0.05;
    const auto nearRows = antennaRows(near);
    bool warned = false;
    for (const auto& r : nearRows) { if (r.item == "Free-space path loss") { warned = !r.note.empty(); } }
    CHECK(warned);
    // more directors
    AntennaInput six = in;
    six.yagiElements = 6;
    int directors = 0;
    for (const auto& r : antennaRows(six)) { if (r.item.rfind("Yagi director", 0) == 0) { ++directors; } }
    CHECK(directors == 4);
    // bad input: each field says what is wrong; a bad frequency gives no rows at all
    AntennaInput bad = in;
    bad.fMHz = 0.0;
    CHECK(!antennaError(bad).empty() && antennaRows(bad).empty());
    bad.fMHz = 300001.0;
    CHECK(antennaRows(bad).empty());
    bad.fMHz = std::nan("");
    CHECK(!antennaError(bad).empty() && antennaRows(bad).empty());
    bad = in; bad.coaxFraction = 0.0;
    CHECK(!antennaError(bad).empty());
    bad = in; bad.vf = 1.5;
    CHECK(!antennaError(bad).empty());
    bad = in; bad.distanceKm = -1.0;
    CHECK(!antennaError(bad).empty());
    bad = in; bad.yagiElements = 1;
    CHECK(!antennaError(bad).empty());
    bool noYagi = true;
    for (const auto& r : antennaRows(bad)) { if (r.item.rfind("Yagi", 0) == 0) { noYagi = false; } }
    CHECK(noYagi);                                            // the other rows still stand
    bad = in; bad.vf = 0.0;
    bool noCoax = true;
    for (const auto& r : antennaRows(bad)) { if (r.item == "Coax length") { noCoax = false; } }
    CHECK(noCoax);
    // the tile cache's words
    cascade::core::tilecache::Stats st;
    st.hits = 3; st.staleHits = 2; st.misses = 9;
    CHECK(tileCacheText(12, 3u << 19, 256ull << 20, st, kTr) == "12 tiles, 1.5 of 256 MB; 5 hits, 9 misses");
}

// What each layer costs a composed frame (1024 x 768, a view of the south of England at zoom 5 degrees): the median of 15 composes with the layer off and on. Printed for the record;
// the only assertion is a generous ceiling (a compose that took a quarter of a second would be a defect on any machine).
void sectionCost() {
    const auto timeIt = [](const std::function<void(slm::TileCompositor&)>& setup) {
        slm::TileCompositor c;
        c.setSize(1024, 768);
        c.setView(51.5, -1.0, 5.0);
        c.setLayers(true, true);
        c.setHome(true, 51.5, -1.0);
        setup(c);
        std::vector<std::uint8_t> buf(1024u * 768u * 4u);
        std::vector<double> ms;
        for (int i = 0; i < 15; ++i) {
            c.setView(51.5 + 0.0001 * i, -1.0, 5.0);                                  // a move: every compose is a full one
            const auto t0 = std::chrono::steady_clock::now();
            c.compose(buf.data(), 1024, 768);
            ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        std::sort(ms.begin(), ms.end());
        return ms[ms.size() / 2];
    };
    const double base = timeIt([](slm::TileCompositor&) {});
    std::vector<slm::Cell> heat;
    for (int i = 0; i < 20000; ++i) {
        slm::Cell c;
        c.latMin = 50.0 + (i % 100) * 0.03; c.latMax = c.latMin + 0.05; c.lonMin = -3.0 + (i / 100) * 0.02; c.lonMax = c.lonMin + 0.05; c.rgba = 0xff8000a0u;
        heat.push_back(c);
    }
    const double heatMs = timeIt([&](slm::TileCompositor& c) { c.setCells(heat); });
    const double gridMs = timeIt([](slm::TileCompositor& c) { c.setGrid(2); });
    slm::Trail poly;
    for (int i = 0; i < 72; ++i) { poly.points.emplace_back(51.5 + 2.0 * std::sin(i * 0.0872665), -1.0 + 3.0 * std::cos(i * 0.0872665)); }
    poly.closed = true; poly.rgba = 0xf0aa3cffu; poly.fillAlpha = 38; poly.widthPx = 2.f;
    const double fillMs = timeIt([&](slm::TileCompositor& c) { c.setTrails({poly}); });
    slm::Trail line;
    line.points = {{50.0, -3.0}, {52.0, 1.0}, {51.0, 2.0}};
    line.rgba = 0xc8c8c85au; line.widthPx = 1.f;
    std::vector<slm::Trail> lines(60, line);
    const double linesMs = timeIt([&](slm::TileCompositor& c) { c.setTrails(lines); });
    std::printf("COST compose 1024x768 median of 15, ms: base %.2f | heat 20000 cells %.2f | grid %.2f | washed outline %.2f | 60 thin lines %.2f", base, heatMs, gridMs, fillMs, linesMs);
    std::puts("");
    CHECK(base < 250.0 && heatMs < 250.0 && gridMs < 250.0 && fillMs < 250.0 && linesMs < 250.0);
}

void sectionAbi() {
    const std::string abi = slurp(srcPath("core/plugin_abi.h"));
    CHECK(!abi.empty());
    CHECK(abi.find("#define CASCADE_TRACK_UNKNOWN 0u") != std::string::npos && kKindUnknown == 0);
    CHECK(abi.find("#define CASCADE_TRACK_AIRCRAFT 1u") != std::string::npos && kKindAircraft == 1);
    CHECK(abi.find("#define CASCADE_TRACK_VESSEL 2u") != std::string::npos && kKindVessel == 2);
    CHECK(abi.find("#define CASCADE_TRACK_STATION 3u") != std::string::npos && kKindStation == 3);
    CHECK(abi.find("#define CASCADE_TRACK_SATELLITE 4u") != std::string::npos && kKindSatellite == 4);
    CHECK(abi.find("#define CASCADE_PATH_FLAG_CLOSED 0x00000001u") != std::string::npos);
}

}  // namespace

int main() {
    sectionLayers();
    sectionCompositor();
    sectionModels();
    sectionLocator();
    sectionReplay();
    sectionFilter();
    sectionUnits();
    sectionSky();
    sectionAntenna();
    sectionCost();
    sectionAbi();
    return testSummary("test_slint_host_mapj");
}
