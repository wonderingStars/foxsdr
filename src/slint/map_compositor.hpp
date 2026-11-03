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

struct Trail {
    std::vector<std::pair<double, double>> points;   // lat, lon degrees
    std::uint32_t rgba = 0xffffffffu;                // 0xRRGGBBAA
    bool dashed = false;
    bool closed = false;
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
    static constexpr std::uint64_t kMissingRetryComposes = 600;
    static constexpr double kMinSpanDeg = 0.02;
    static constexpr double kMaxSpanDeg = 360.0;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace foxsdr::slintmap
