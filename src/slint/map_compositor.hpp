// map_compositor.hpp - THE MAP'S PICTURE, painted on the host's side (the plugin kit, docs/engine-stage4.md 6.37).
//
// The ImGui map draws tiles as GL textures, the coastline as earcut triangles and every overlay per frame. The Slint
// window has no GL at its disposal on the UI thread and draws an `image`: this class composes the whole base picture -
// the basemap plugin's tiles (Web Mercator) or the compiled-in Natural Earth coastline (equirectangular), the graticule,
// the range rings about the receiver, the coverage wedges and the trails - into a caller-owned RGBA buffer, and only
// when something changed (a view move, a tile that arrived, a layer switched). The targets and their labels are NOT
// here: they are Slint items the host places from project() so they hit-test and carry tooltips.
//
// No Slint, no engine and no ImGui type appears here: only the C++ standard library and the public-domain coastline data
// (gui/coastline_data.hpp), so tests/test_slint_host_map.cpp composes without a window (the map family agent writes
// map_compositor.cpp and that test; see COMPONENTS.md, "map.slint + map_compositor").
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace foxsdr::slintmap {

// A tile the compositor asked the plugin for: the ImGui window's BasemapCache::rawTile contract (CASCADE_TILE_*):
// 1 READY with `rgb` filled (tileSize * tileSize * 3 bytes, no row padding), 0 PENDING (ask again next compose: asking
// is what makes the plugin fetch it), -1 MISSING (believed for kMissingRetryComposes composes, then asked again).
using TileSource = std::function<int(std::uint32_t z, std::uint32_t x, std::uint32_t y, std::vector<std::uint8_t>& rgb)>;

// THE DISPLAY SETTINGS THE MAP APPLIES (SYSTEM > Display: "Aircraft icons", "Trail width" and the trail-style list; the interface half's
// aircraftIconPx, mapTrailWidthPx and mapTrailStyle, core/config_halves.hpp). The bounds are the classic window's (gui/aircraft_icons.hpp
// clampAircraftIconPx, aircraftWingspanPx, clampTrailWidthPx - that header includes imgui.h, so the numbers are copied here and
// tests/test_slint_host_map.cpp reads the source to hold them equal): an icon is 16..96 px across (48 by default); a trail is 1 px up to the
// WINGSPAN of the icon in force (114/128 of it, rounded: 86 px at 96), so a hand-edited config cannot draw a 1000 px aeroplane or a trail that
// is broader than the aircraft drawing it.
inline constexpr int kAircraftIconMinPx = 16;
inline constexpr int kAircraftIconMaxPx = 96;
inline constexpr int kAircraftIconDefaultPx = 48;
inline constexpr int kTrailWidthMinPx = 1;
inline constexpr int kTrailWidthDefaultPx = 4;
inline constexpr float kAircraftSpanFraction = 114.0f / 128.0f;
inline constexpr int kRibbonExtraPx = 3;                  // a ribbon is the line's width + 3 px (gui/scope_view.cpp: "the ribbon a little wider than the line")

int clampAircraftIconPx(int px);
int aircraftWingspanPx(int iconPx);
int clampTrailWidthPx(int widthPx, int iconPx);

enum class TrailStyle : int { Line = 0, Ribbon = 1 };     // AppConfig::mapTrailStyle (anything but 1 is a line, as map_view.cpp has it)

// What the three settings come to once bounded: what the map is handed each publish.
struct MapLook {
    int iconPx = kAircraftIconDefaultPx;
    int trailWidthPx = kTrailWidthDefaultPx;
    TrailStyle trailStyle = TrailStyle::Line;
    bool operator==(const MapLook& o) const { return iconPx == o.iconPx && trailWidthPx == o.trailWidthPx && trailStyle == o.trailStyle; }
    bool operator!=(const MapLook& o) const { return !(*this == o); }
};
// The saved values (any ints) as the bounded look: icon 16..96, trail 1..wingspan, style 1 = ribbon and anything else a line.
MapLook mapLookOf(int savedIconPx, int savedTrailWidthPx, int savedTrailStyle);
// The pixels across a target of CASCADE_TRACK_* `kind` is drawn at: the look's icon size for an aircraft (kind 1), 0 (the pane's own 18 px
// glyph) for every other kind - the settings are the AIRCRAFT icons'.
int targetIconPx(int kind, const MapLook& look);

struct Trail {
    std::vector<std::pair<double, double>> points;   // lat, lon degrees
    std::uint32_t rgba = 0xffffffffu;                // 0xRRGGBBAA
    bool dashed = false;
    bool closed = false;
    // LANE J (docs/multi-radio-records/screen-j.md): a line of its own width, and a washed inside. widthPx 0 = the Display setting's width and
    // style, as before; above 0 = that many pixels across (a thin faint APRS path, 1 px), the style ignored. fillAlpha is for a CLOSED trail
    // only: the inside is washed in the trail's colour at that alpha (0..255) under the stroke (an airspace, the 24-hour range outline).
    float widthPx = 0.f;
    std::uint8_t fillAlpha = 0;
};

// A box of the map in degrees, washed in one colour (0xRRGGBBAA): a heat-map cell, a worked or heard grid square, a level-against-position cell.
struct Cell {
    double latMin = 0.0, latMax = 0.0, lonMin = 0.0, lonMax = 0.0;
    std::uint32_t rgba = 0u;
    bool operator==(const Cell& o) const {
        return latMin == o.latMin && latMax == o.latMax && lonMin == o.lonMin && lonMax == o.lonMax && rgba == o.rgba;
    }
};

class TileCompositor {
public:
    TileCompositor();
    ~TileCompositor();
    TileCompositor(const TileCompositor&) = delete;
    TileCompositor& operator=(const TileCompositor&) = delete;

    // The picture's size in pixels (the pane's size): a change recomposes everything.
    void setSize(int width, int height);
    // The view: the centre and the longitude span across the width; the latitude span follows the aspect ratio
    // (equirectangular) or the Mercator scale (Web Mercator).
    void setView(double centreLatDeg, double centreLonDeg, double spanDeg);
    // Web Mercator while a basemap is attached (tiles are Mercator by construction), else equirectangular.
    void setMercator(bool mercator);
    // The basemap plugin's figures and the fetch. tileSize 64..1024; an empty source detaches (clearTiles() too).
    void setTileSource(TileSource source, std::uint32_t tileSize, std::uint32_t minZoom, std::uint32_t maxZoom);
    void clearTiles();
    void setLayers(bool coastline, bool graticule);
    void setHome(bool has, double latDeg, double lonDeg);
    // 72 buckets of 5 degrees of bearing, km each (0 = nothing in that wedge); empty = the overlay is off.
    void setCoverage(const std::vector<double>& kmPerBucket);
    void setTrails(const std::vector<Trail>& trails);
    // LANE J: boxes washed under the graticule, rings and trails, most of kMaxCells of them (the rest are not drawn); a cell whose edges are not numbers is dropped.
    void setCells(const std::vector<Cell>& cells);
    // LANE J: the Maidenhead grid lines: 0 off, 1 fields (20 x 10 degrees), 2 squares (2 x 1), 3 subsquares (5 x 2.5 minutes). A level whose lines would be closer than
    // kMinGridLinePx apart is drawn as the level above it (a grid is never a smear).
    void setGrid(int level);
    int gridLevel() const;
    // The width and the style every trail is stroked with (a trail of 2 px, as a line, until this is called). The width is bounded as the
    // classic window bounds it for `iconPx` (clampTrailWidthPx): 1 .. the wingspan of the icon; a ribbon is kRibbonExtraPx wider than the line.
    void setTrailLook(int widthPx, int iconPx, TrailStyle style);
    int trailWidthPx() const;            // the bounded width (a ribbon's extra not counted)
    TrailStyle trailStyle() const;
    double trailStrokePx() const;        // the width the trails are stroked at: trailWidthPx(), + kRibbonExtraPx for a ribbon

    // Paints into `rgba` (width * height * 4, tightly packed) and returns true; returns false and touches nothing when
    // nothing changed since the last compose into a buffer of this size. A buffer of another size than setSize's is
    // refused (false).
    bool compose(std::uint8_t* rgba, int width, int height);

    // Where a position lands, as fractions of the picture (0..1); false when it is off the picture (fx, fy are still
    // filled, so a caller may draw a trail's segment that leaves the edge).
    bool project(double latDeg, double lonDeg, double& fx, double& fy) const;
    void unproject(double fx, double fy, double& latDeg, double& lonDeg) const;
    // The ground distance one pixel covers at the centre.
    double metresPerPixel() const;
    // The tile zoom the current view composes at (-1 without a tile source).
    int tileZoom() const;
    std::size_t residentTiles() const;
    double centreLatDeg() const;
    double centreLonDeg() const;
    double spanDeg() const;
    bool mercator() const;

    static constexpr std::size_t kMaxTiles = 256;              // the CPU cache's cap
    static constexpr std::size_t kMaxCells = 40000;
    static constexpr double kMinGridLinePx = 6.0;
    static constexpr std::uint64_t kMissingRetryComposes = 600;
    static constexpr double kMinSpanDeg = 0.02;
    static constexpr double kMaxSpanDeg = 360.0;

private:
    struct Impl;
    Impl* impl_;
};

// THE COASTLINE'S RINGS (docs 6.65): the flat map's own rings - the closed runs of the Natural Earth data, the fragments chained end to end, Antarctica taken to the
// pole - handed out for the globe views' land mask. Degrees; longitude CONTINUOUS (a ring that crosses the antimeridian runs past +/-180). Built once.
struct CoastRing {
    std::vector<double> lon, lat;
    bool closed = false;              // an open line (a stub that joined nothing) is never filled
};
const std::vector<CoastRing>& coastlineRings();

}  // namespace foxsdr::slintmap
