// map_compositor.cpp - THE MAP'S PICTURE, painted on the CPU (see map_compositor.hpp for the contract and the reasons).
//
// What is ported, and from where (src/gui/map_view.cpp and gui/track_metrics.hpp are read for the ARITHMETIC only; nothing here
// touches ImGui, GL, the engine or Slint):
//   * the projection: the view is `spanDeg` of longitude across the width, the latitude scale follows from the aspect, Mercator
//     is conformal off the same pixels-per-degree, and a point takes the SHORT way round the antimeridian (mapProjectPoint);
//   * the tile rules: zoom z = round(log2(worldPx / tileSize)) clamped to [minZoom, maxZoom], the visible range from the view's
//     corners, x wraps and y does not, at most 64 tiles per axis, every tile placed from its OWN edges; a READY tile is cached
//     for good, PENDING is never cached (asking is what makes the plugin fetch), MISSING is believed for 600 frames and then
//     asked again (BasemapCache);
//   * the coastline: Natural Earth 1:110m, int16 hundredths of a degree, assembled into closed rings, filled, then outlined;
//   * the graticule, the range rings, the coverage wedges and the trails.
//
// WHERE IT DELIBERATELY DIFFERS FROM THE IMGUI VIEW (each is a decision, not an accident):
//   * THE COAST IS FILLED BY AN EVEN-ODD SCANLINE FILL over every ring at once, so the Caspian (a ring inside Eurasia) is a hole
//     without being classified as water first; ImGui triangulates by ear clipping because it draws triangles on a GPU.
//   * THE RINGS ARE ASSEMBLED THE SAME WAY (14 of the 134 runs are fragments cut at the antimeridian: the Americas in four
//     pieces, Eurasia-and-Africa in three, Fiji and Wrangel Island in two, Antarctica in one that leaves the world at -180 and
//     re-enters at +180 and needs the pole put in) - closing every run with a chord, as the contract's first reading had it,
//     would draw Eurasia as a quadrilateral across the Atlantic.
//   * A TRAIL SEGMENT THAT CROSSES THE ANTIMERIDIAN IS DRAWN THE SHORT WAY, not skipped: each vertex is placed relative to the
//     one before it (lonDelta), and the polyline is offered to the view once per world copy it touches.
//   * THE DASHES ARE 6 PX ON, 4 PX OFF ALONG THE PATH (the contract), where ImGui skips every second segment.
//   * THE RINGS ARE GREAT-CIRCLE POLYLINES PROJECTED, so in equirectangular they are ellipses, wider than tall by 1 / cos(latitude)
//     (a degree of longitude is shorter on the ground than a degree of latitude away from the equator, and the pixels per degree are
//     the same both ways); ImGui draws a circle of the latitude radius. metresPerPixel() is the east-west scale at the centre. Their
//     step is the smallest of 1, 2, 2.5, 5 x 10^n km that leaves fewer than seven in the picture's half diagonal (so three to six show).
//   * THE COVERAGE IS ONE OUTLINE: each bucket's arc, a step along the shared bearing to the next bucket's, the home itself where a bucket
//     is empty (ImGui joins the sectors' middles). Filled apart, 72 wedges leave a hair of sea along every bearing they share.
//   * WITH A TILE SOURCE AND NOT ONE TILE DRAWN (none arrived, none exists) THE COASTLINE IS THE PICTURE, as in ImGui: a map that has not
//     loaded, or whose server cannot be reached, is not a blank. (The contract's either-or is read as: tiles, when there are any.)
//   * AT MOST 64 TILES AN AXIS (ImGui's `> 64` test keeps 65), 256 READY tiles (kMaxTiles; fewer when a tile is big: 96 MiB of RGB).
//   * THE MISSING CLOCK COUNTS compose() CALLS, clean ones too: a host that polls every tick (as it should) gets the retry ImGui gets
//     (600 frames), and a clean view whose clock runs out is recomposed once to ask again.
//   * WHICH BUFFER IS ASKED does not matter: false means "the picture you are showing is still right" and the buffer is not touched, so a
//     host that alternates two buffers keeps working; a change repaints whichever one it is asked into, whole.
//
// Everything is plain loops with bounds checks: a tile buffer shorter than tileSize * tileSize * 3 is MISSING, a coordinate that is not
// a number is skipped, a size past 16384 or 16 Mpixel is refused. The picture is opaque (alpha 255 everywhere).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "map_compositor.hpp"

#include "gui/coastline_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace foxsdr::slintmap {
namespace {

namespace coast = cascade::gui::coastline;

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kEarthRadiusKm = 6371.0;                 // the sphere gui/track_metrics.hpp measures on
constexpr double kMetresPerDegree = kEarthRadiusKm * 1000.0 * kDegToRad;
constexpr double kMercatorLatLimit = 85.05112878;         // where Web Mercator ends (and where its tiles end)
constexpr double kMaxCentreLat = 89.0;                    // MapView clamps the centre to +/-89

// The palette (the contract; the host hands none): sea, land, coast, graticule, range rings, coverage wedges.
struct Rgb {
    std::uint8_t r, g, b;
};
constexpr Rgb kSea{0x0d, 0x0f, 0x12};
constexpr Rgb kLand{0x1d, 0x20, 0x26};
constexpr Rgb kCoast{0x43, 0x49, 0x55};
constexpr Rgb kGraticule{0x1e, 0x22, 0x29};
constexpr Rgb kRing{0x43, 0x49, 0x55};
constexpr Rgb kCoverage{0x2e, 0x86, 0xc1};
constexpr int kCoverageAlpha = 64;                        // 25 % of 255

constexpr int kMaxDimension = 16384;
constexpr long kMaxPixels = 4096L * 4096L;                // the scratch planes are 5 bytes a pixel
constexpr double kMinLinePx = 60.0;                       // graticule lines are 60..150 px apart
constexpr long kMaxTilesPerAxis = 64;                     // ImGui's cap (it keeps tx0 + 64, 65 tiles; this holds 64): a degenerate view cannot ask for thousands
constexpr std::uint32_t kMaxZoomLevel = 24;               // x and y stay inside the 29 bits of the cache key
constexpr double kTileBudgetBytes = 96.0 * 1024.0 * 1024.0;
constexpr std::size_t kMaxMissing = 8192;                 // MISSING answers remembered (they hold no pixels, only a key and a frame)
constexpr double kDashOn = 6.0;
constexpr double kDashOff = 4.0;

// ------------------------------------------------------------------------------------------------------------------------
// numbers
// ------------------------------------------------------------------------------------------------------------------------

bool finite2(double a, double b) { return std::isfinite(a) && std::isfinite(b); }

// A longitude difference, the SHORT way round: -180..180 (a difference already inside it is returned untouched, so +180 stays
// +180 as mapProjectPoint has it).
double foldDelta(double d) {
    if (d >= -180.0 && d <= 180.0) { return d; }
    double l = std::fmod(d + 180.0, 360.0);
    if (l < 0.0) { l += 360.0; }
    return l - 180.0;
}

// A longitude as a place: -180 <= lon < 180. (One already there comes back as it is: the arithmetic below would move -0.1 by 1e-14.)
double normaliseLon(double lon) {
    if (lon >= -180.0 && lon < 180.0) { return lon; }
    double l = std::fmod(lon + 180.0, 360.0);
    if (l < 0.0) { l += 360.0; }
    return l - 180.0;
}

// Web Mercator, normalised so the whole world is 0..1 in both axes with north at 0 (track_metrics.hpp mapMercatorY).
double mercY(double latDeg) {
    const double lat = std::clamp(latDeg, -kMercatorLatLimit, kMercatorLatLimit);
    const double s = std::sin(lat * kDegToRad);
    return 0.5 - std::log((1.0 + s) / (1.0 - s)) / (4.0 * kPi);
}

double mercLat(double y) {
    const double n = kPi * (1.0 - 2.0 * y);
    return std::atan(std::sinh(n)) / kDegToRad;
}

// Where a great circle from (latDeg) on `bearingDeg` ends after `km`: the latitude, and the longitude OFFSET from the start
// (continuous, -180..180, never wrapped as a place - the only way a connected shape has no seam). destinationPoint's formula.
struct Destination {
    double latDeg;
    double dLonDeg;
};
Destination destination(double latDeg, double bearingDeg, double km) {
    const double p1 = latDeg * kDegToRad;
    const double b = bearingDeg * kDegToRad;
    const double d = km / kEarthRadiusKm;
    double sinP2 = std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(b);
    sinP2 = std::clamp(sinP2, -1.0, 1.0);
    const double p2 = std::asin(sinP2);
    const double y = std::sin(b) * std::sin(d) * std::cos(p1);
    const double x = std::cos(d) - std::sin(p1) * sinP2;
    return {p2 / kDegToRad, std::atan2(y, x) / kDegToRad};
}

double greatCircleKm(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * kDegToRad, p2 = lat2 * kDegToRad;
    const double dp = (lat2 - lat1) * kDegToRad, dl = (lon2 - lon1) * kDegToRad;
    double a = std::sin(dp * 0.5) * std::sin(dp * 0.5) + std::cos(p1) * std::cos(p2) * std::sin(dl * 0.5) * std::sin(dl * 0.5);
    a = std::clamp(a, 0.0, 1.0);
    return 2.0 * kEarthRadiusKm * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

// ------------------------------------------------------------------------------------------------------------------------
// the view: the one projection rule, in pixels
// ------------------------------------------------------------------------------------------------------------------------
struct View {
    double w = 1.0, h = 1.0;
    double cLat = 0.0, cLon = 0.0, span = 360.0;
    bool merc = false;
    double pxPerDeg = 1.0;       // pixels per degree of longitude (and of latitude, equirectangular)
    double pxPerWorld = 360.0;   // pixels across the whole world (Mercator)
    double cMercY = 0.5;         // the centre's normalised Mercator y

    static View make(int width, int height, double cLat, double cLon, double span, bool merc) {
        View v;
        v.w = std::max(1, width);
        v.h = std::max(1, height);
        v.cLat = merc ? std::clamp(cLat, -kMercatorLatLimit, kMercatorLatLimit) : cLat;
        v.cLon = cLon;
        v.span = span;
        v.merc = merc;
        v.pxPerDeg = v.w / span;
        v.pxPerWorld = v.w * 360.0 / span;
        v.cMercY = mercY(v.cLat);
        return v;
    }
    double xOfDelta(double dLon) const { return dLon * pxPerDeg + w * 0.5; }
    double xOfLon(double lonUnwrapped) const { return xOfDelta(lonUnwrapped - cLon); }
    double yOfLat(double lat) const {
        return merc ? (mercY(lat) - cMercY) * pxPerWorld + h * 0.5 : (cLat - lat) * pxPerDeg + h * 0.5;
    }
    double latOfY(double y) const {
        return merc ? mercLat(cMercY + (y - h * 0.5) / pxPerWorld) : cLat - (y - h * 0.5) / pxPerDeg;
    }
    // the poles, or where Mercator stops: no graticule line and no ring runs past them
    double latLimit() const { return merc ? kMercatorLatLimit : 90.0; }
};

// ------------------------------------------------------------------------------------------------------------------------
// the coastline, assembled into rings once
// ------------------------------------------------------------------------------------------------------------------------
struct Ring {
    std::vector<double> lon;     // degrees, CONTINUOUS (may run past +/-180 where the ring crosses the antimeridian)
    std::vector<double> lat;
    bool closed = false;         // a fragment that never found its other end stays an open line
    double minLon = 0, maxLon = 0, minLat = 0, maxLat = 0;
};

int foldHundredths(int v) {    // the two spellings of the antimeridian compare equal
    long r = (static_cast<long>(v) + 18000L) % 36000L;
    if (r < 0) { r += 36000L; }
    return static_cast<int>(r - 18000L);
}

int runLon(std::uint32_t run, std::uint32_t i) {
    return static_cast<int>(coast::kCoords[(static_cast<std::size_t>(coast::kRuns[run].first) + i) * 2u]);
}
int runLat(std::uint32_t run, std::uint32_t i) {
    return static_cast<int>(coast::kCoords[(static_cast<std::size_t>(coast::kRuns[run].first) + i) * 2u + 1u]);
}

// Chains the fragments end to end (treating -180 and +180 as one meridian), depth first with backtracking: a greedy walk takes
// the first matching end and at the Bering Strait that is a two-point stub which swallows the join Eurasia needed.
class Chainer {
public:
    explicit Chainer(const std::vector<std::uint32_t>& fragments) : fragments_(fragments), consumed_(fragments.size(), false) {}

    bool consumed(std::size_t f) const { return consumed_[f]; }
    void consume(const std::vector<std::size_t>& path) { for (std::size_t f : path) { consumed_[f] = true; } }

    // Starts a chain from fragment `f` (whole) and tries to close it; on success `chain` is the closed chain and `path` the
    // fragments it took.
    bool close(std::size_t f, std::vector<std::pair<int, int>>& chain, std::vector<std::size_t>& path) {
        const std::uint32_t run = fragments_[f];
        chain_.clear();
        for (std::uint32_t i = 0; i < coast::kRuns[run].count; ++i) { chain_.emplace_back(runLon(run, i), runLat(run, i)); }
        path_.assign(1, f);
        const bool ok = extend();
        chain = chain_;
        path = path_;
        return ok;
    }

private:
    bool closes() const {
        return chain_.size() >= 4u && foldHundredths(chain_.back().first) == foldHundredths(chain_.front().first) &&
               chain_.back().second == chain_.front().second;
    }
    bool extend() {
        if (closes()) { return true; }
        for (std::size_t f = 0; f < fragments_.size(); ++f) {
            if (consumed_[f]) { continue; }
            if (std::find(path_.begin(), path_.end(), f) != path_.end()) { continue; }
            const std::uint32_t run = fragments_[f];
            const std::uint32_t cnt = coast::kRuns[run].count;
            for (int reversed = 0; reversed < 2; ++reversed) {
                const std::uint32_t head = reversed ? (cnt - 1u) : 0u;
                if (foldHundredths(runLon(run, head)) != foldHundredths(chain_.back().first) || runLat(run, head) != chain_.back().second) {
                    continue;
                }
                const int off = chain_.back().first - runLon(run, head);
                const std::size_t grew = chain_.size();
                for (std::uint32_t k = 1; k < cnt; ++k) {
                    const std::uint32_t i = reversed ? (cnt - 1u - k) : k;
                    chain_.emplace_back(runLon(run, i) + off, runLat(run, i));
                }
                path_.push_back(f);
                if (extend()) { return true; }
                path_.pop_back();
                chain_.resize(grew);
            }
        }
        return false;
    }

    const std::vector<std::uint32_t>& fragments_;
    std::vector<bool> consumed_;
    std::vector<std::pair<int, int>> chain_;
    std::vector<std::size_t> path_;
};

void finishRing(Ring& r) {
    r.minLon = r.maxLon = r.lon.empty() ? 0.0 : r.lon.front();
    r.minLat = r.maxLat = r.lat.empty() ? 0.0 : r.lat.front();
    for (std::size_t i = 0; i < r.lon.size(); ++i) {
        r.minLon = std::min(r.minLon, r.lon[i]);
        r.maxLon = std::max(r.maxLon, r.lon[i]);
        r.minLat = std::min(r.minLat, r.lat[i]);
        r.maxLat = std::max(r.maxLat, r.lat[i]);
    }
}

std::vector<Ring> buildRings() {
    std::vector<Ring> rings;
    std::vector<std::uint32_t> fragments;
    for (std::uint32_t run = 0; run < coast::kRunCount; ++run) {
        const std::uint32_t count = coast::kRuns[run].count;
        if (count < 2u) { continue; }
        const std::uint32_t last = count - 1u;
        if (runLon(run, 0) == runLon(run, last) && runLat(run, 0) == runLat(run, last)) {
            // already a ring: the last point repeats the first, so it is left out
            Ring r;
            for (std::uint32_t i = 0; i < last; ++i) {
                r.lon.push_back(runLon(run, i) / 100.0);
                r.lat.push_back(runLat(run, i) / 100.0);
            }
            r.closed = r.lon.size() >= 3u;
            finishRing(r);
            rings.push_back(std::move(r));
        } else {
            fragments.push_back(run);
        }
    }

    Chainer chainer(fragments);
    std::vector<std::pair<int, int>> chain;
    std::vector<std::size_t> path;
    for (std::size_t f = 0; f < fragments.size(); ++f) {
        if (chainer.consumed(f)) { continue; }
        Ring r;
        if (chainer.close(f, chain, path)) {
            chainer.consume(path);
            r.closed = true;
            const int startLon = chain.front().first;
            const int endLon = chain.back().first;
            const int wrapped = endLon - startLon;
            if (wrapped >= 35900 || wrapped <= -35900) {
                // A ring that went right round the world: Antarctica, whose coast has no southern side in this data. Its ends are
                // the same meridian one world apart, so the run is kept whole and taken to the pole and back.
                double meanLat = 0.0;
                for (const auto& q : chain) { meanLat += q.second; }
                meanLat /= static_cast<double>(chain.size());
                const int pole = (meanLat < 0.0) ? -9000 : 9000;
                chain.emplace_back(endLon, pole);
                chain.emplace_back(startLon, pole);
            } else {
                chain.pop_back();   // the duplicate of the first point
            }
        } else {
            // A stub that joins nothing: an open line, drawn and never filled. (path holds only fragment f.)
            chainer.consume(path);
            r.closed = false;
        }
        for (const auto& q : chain) {
            r.lon.push_back(q.first / 100.0);
            r.lat.push_back(q.second / 100.0);
        }
        finishRing(r);
        rings.push_back(std::move(r));
    }
    return rings;
}

const std::vector<Ring>& coastRings() {
    static const std::vector<Ring> rings = buildRings();
    return rings;
}

// ------------------------------------------------------------------------------------------------------------------------
// an even-odd scanline fill of any number of closed rings at once
// ------------------------------------------------------------------------------------------------------------------------
class ScanFill {
public:
    void begin(int w, int h) {
        w_ = w;
        h_ = h;
        edges_.clear();
    }
    bool empty() const { return edges_.empty(); }

    // A ring of n projected vertices (closed by an edge from the last back to the first). A vertex that is not a number
    // drops the whole ring: a half-projected ring would fill the wrong side.
    void addRing(const double* xs, const double* ys, std::size_t n) {
        if (n < 3u) { return; }
        for (std::size_t i = 0; i < n; ++i) {
            if (!finite2(xs[i], ys[i])) { return; }
        }
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1u == n) ? 0u : i + 1u;
            addEdge(xs[i], ys[i], xs[j], ys[j]);
        }
    }

    // Calls span(y, xBegin, xEnd) for every filled run of every row, [xBegin, xEnd) clipped to the picture: a pixel is filled when
    // its CENTRE lies inside, so two rings sharing an edge never both claim a pixel.
    template <class Span>
    void run(Span&& span) {
        std::sort(edges_.begin(), edges_.end(), [](const Edge& a, const Edge& b) { return a.first < b.first; });
        active_.clear();
        std::size_t next = 0;
        int y = edges_.empty() ? 0 : edges_.front().first;
        while (next < edges_.size() || !active_.empty()) {
            if (active_.empty() && next < edges_.size() && edges_[next].first > y) { y = edges_[next].first; }
            while (next < edges_.size() && edges_[next].first <= y) {
                active_.push_back({edges_[next].x, edges_[next].slope, edges_[next].last});
                ++next;
            }
            // drop what ended above this row
            std::size_t keep = 0;
            for (std::size_t i = 0; i < active_.size(); ++i) {
                if (active_[i].last >= y) { active_[keep++] = active_[i]; }
            }
            active_.resize(keep);
            if (y >= h_) { break; }
            xs_.clear();
            for (const Active& a : active_) { xs_.push_back(a.x); }
            std::sort(xs_.begin(), xs_.end());
            for (std::size_t i = 0; i + 1u < xs_.size(); i += 2u) {
                const double xa = std::ceil(xs_[i] - 0.5);
                const double xb = std::ceil(xs_[i + 1u] - 0.5);
                const int x0 = static_cast<int>(std::clamp(xa, 0.0, static_cast<double>(w_)));
                const int x1 = static_cast<int>(std::clamp(xb, 0.0, static_cast<double>(w_)));
                if (x1 > x0) { span(y, x0, x1); }
            }
            for (Active& a : active_) { a.x += a.slope; }
            ++y;
        }
    }

private:
    struct Edge {
        int first, last;      // the rows whose centre lies in [ya, yb), clipped to the picture
        double x, slope;      // x at row `first`'s centre, and dx per row
    };
    struct Active {
        double x, slope;
        int last;
    };

    void addEdge(double xa, double ya, double xb, double yb) {
        if (ya == yb) { return; }                       // horizontal: crosses no scanline
        if (ya > yb) {
            std::swap(xa, xb);
            std::swap(ya, yb);
        }
        const double r0 = std::ceil(ya - 0.5);
        const double r1 = std::ceil(yb - 0.5) - 1.0;
        if (r1 < r0 || r1 < 0.0 || r0 > static_cast<double>(h_ - 1)) { return; }
        const double slope = (xb - xa) / (yb - ya);
        const double first = std::max(r0, 0.0);
        const double last = std::min(r1, static_cast<double>(h_ - 1));
        const double x = xa + (first + 0.5 - ya) * slope;
        if (!std::isfinite(slope) || !std::isfinite(x)) { return; }     // (a sort of NaN is not a sort)
        edges_.push_back({static_cast<int>(first), static_cast<int>(last), x, slope});
    }

    int w_ = 0, h_ = 0;
    std::vector<Edge> edges_;
    std::vector<Active> active_;
    std::vector<double> xs_;
};

// ------------------------------------------------------------------------------------------------------------------------
// the frame, and the anti-aliased strokes
// ------------------------------------------------------------------------------------------------------------------------
struct Frame {
    std::uint8_t* px = nullptr;
    int w = 0, h = 0;
    std::uint8_t* at(int x, int y) const { return px + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4u; }
};

void blendPixel(std::uint8_t* p, Rgb c, int a) {   // a: 0..255
    if (a <= 0) { return; }
    if (a >= 255) {
        p[0] = c.r;
        p[1] = c.g;
        p[2] = c.b;
        return;
    }
    p[0] = static_cast<std::uint8_t>((p[0] * (255 - a) + c.r * a + 127) / 255);
    p[1] = static_cast<std::uint8_t>((p[1] * (255 - a) + c.g * a + 127) / 255);
    p[2] = static_cast<std::uint8_t>((p[2] * (255 - a) + c.b * a + 127) / 255);
}

// The per-pixel planes a stroke set is accumulated in: the generation that last touched a pixel, and its best coverage. A pixel
// two segments of ONE polyline both cover is composited once (a translucent trail would otherwise be darker at every joint).
struct Scratch {
    std::vector<std::uint32_t> stamp;
    std::vector<std::uint8_t> cov;
    std::vector<std::uint32_t> touched;
    std::vector<std::uint8_t> mask;
    std::uint32_t generation = 0;
    void size(std::size_t pixels) {
        if (stamp.size() != pixels) {
            stamp.assign(pixels, 0u);
            cov.assign(pixels, 0u);
            generation = 0;
        }
    }
};

class Raster {
public:
    Raster(Frame f, Scratch& s) : f_(f), s_(s) {}

    void begin() {
        ++s_.generation;
        s_.touched.clear();
    }
    // Composites what was accumulated since begin() in `c` at `alpha` (0..255) times each pixel's coverage.
    void end(Rgb c, int alpha) {
        for (std::uint32_t idx : s_.touched) {
            const int a = (static_cast<int>(s_.cov[idx]) * alpha + 127) / 255;
            blendPixel(f_.px + static_cast<std::size_t>(idx) * 4u, c, a);
        }
        s_.touched.clear();
    }

    // The portion of a segment worth rasterising: the parameter range [t0, t1] inside the picture grown by `margin`. A coordinate
    // that is not a number, or a segment wholly outside, answers false.
    bool visibleRange(double x0, double y0, double x1, double y1, double margin, double& t0, double& t1) const {
        if (!finite2(x0, y0) || !finite2(x1, y1)) { return false; }
        t0 = 0.0;
        t1 = 1.0;
        const double dx = x1 - x0, dy = y1 - y0;
        const auto test = [&](double p, double q) {
            if (p == 0.0) { return q >= 0.0; }
            const double r = q / p;
            if (p < 0.0) {
                if (r > t1) { return false; }
                t0 = std::max(t0, r);
            } else {
                if (r < t0) { return false; }
                t1 = std::min(t1, r);
            }
            return true;
        };
        return test(-dx, x0 + margin) && test(dx, f_.w + margin - x0) && test(-dy, y0 + margin) && test(dy, f_.h + margin - y0);
    }

    // A segment of half-width `hw` (a 1 px line is 0.5): the coverage of a pixel is how far its centre is inside the capsule.
    void segment(double x0, double y0, double x1, double y1, double hw) {
        double t0 = 0.0, t1 = 1.0;
        if (!visibleRange(x0, y0, x1, y1, hw + 1.0, t0, t1)) { return; }
        const double ax = x0 + (x1 - x0) * t0, ay = y0 + (y1 - y0) * t0;
        const double bx = x0 + (x1 - x0) * t1, by = y0 + (y1 - y0) * t1;
        const double dx = bx - ax, dy = by - ay;
        const double len2 = dx * dx + dy * dy;
        if (len2 < 1e-12) {
            dot(ax, ay, hw);
            return;
        }
        const double reach = hw + 0.5;
        const auto cover = [&](int px, int py) {
            const double cx = px + 0.5, cy = py + 0.5;
            double t = ((cx - ax) * dx + (cy - ay) * dy) / len2;
            t = std::clamp(t, 0.0, 1.0);
            const double ex = ax + t * dx - cx, ey = ay + t * dy - cy;
            const double c = reach - std::sqrt(ex * ex + ey * ey);
            if (c > 0.0) { plot(px, py, std::min(c, 1.0)); }
        };
        if (std::fabs(dx) >= std::fabs(dy)) {
            const double slope = dy / dx;
            const double band = reach * std::sqrt(1.0 + slope * slope) + 0.5;
            const double lo = std::min(ax, bx), hi = std::max(ax, bx);
            const int p0 = static_cast<int>(std::max(0.0, std::floor(lo - reach)));
            const int p1 = static_cast<int>(std::min(static_cast<double>(f_.w - 1), std::floor(hi + reach)));
            for (int px = p0; px <= p1; ++px) {
                const double cx = std::clamp(px + 0.5, lo, hi);
                const double yl = ay + (cx - ax) * slope;
                const int q0 = static_cast<int>(std::max(0.0, std::floor(yl - band)));
                const int q1 = static_cast<int>(std::min(static_cast<double>(f_.h - 1), std::floor(yl + band)));
                for (int py = q0; py <= q1; ++py) { cover(px, py); }
            }
        } else {
            const double slope = dx / dy;
            const double band = reach * std::sqrt(1.0 + slope * slope) + 0.5;
            const double lo = std::min(ay, by), hi = std::max(ay, by);
            const int q0 = static_cast<int>(std::max(0.0, std::floor(lo - reach)));
            const int q1 = static_cast<int>(std::min(static_cast<double>(f_.h - 1), std::floor(hi + reach)));
            for (int py = q0; py <= q1; ++py) {
                const double cy = std::clamp(py + 0.5, lo, hi);
                const double xl = ax + (cy - ay) * slope;
                const int p0 = static_cast<int>(std::max(0.0, std::floor(xl - band)));
                const int p1 = static_cast<int>(std::min(static_cast<double>(f_.w - 1), std::floor(xl + band)));
                for (int px = p0; px <= p1; ++px) { cover(px, py); }
            }
        }
    }

    void dot(double x, double y, double hw) {
        if (!finite2(x, y)) { return; }
        const double reach = hw + 0.5;
        const int p0 = static_cast<int>(std::max(0.0, std::floor(x - reach)));
        const int p1 = static_cast<int>(std::min(static_cast<double>(f_.w - 1), std::floor(x + reach)));
        const int q0 = static_cast<int>(std::max(0.0, std::floor(y - reach)));
        const int q1 = static_cast<int>(std::min(static_cast<double>(f_.h - 1), std::floor(y + reach)));
        for (int py = q0; py <= q1; ++py) {
            for (int px = p0; px <= p1; ++px) {
                const double ex = px + 0.5 - x, ey = py + 0.5 - y;
                const double c = reach - std::sqrt(ex * ex + ey * ey);
                if (c > 0.0) { plot(px, py, std::min(c, 1.0)); }
            }
        }
    }

private:
    void plot(int x, int y, double c) {
        if (x < 0 || y < 0 || x >= f_.w || y >= f_.h) { return; }
        const std::uint32_t idx = static_cast<std::uint32_t>(y) * static_cast<std::uint32_t>(f_.w) + static_cast<std::uint32_t>(x);
        const std::uint8_t c8 = static_cast<std::uint8_t>(std::lround(c * 255.0));
        if (c8 == 0u) { return; }
        if (s_.stamp[idx] != s_.generation) {
            s_.stamp[idx] = s_.generation;
            s_.cov[idx] = c8;
            s_.touched.push_back(idx);
        } else if (c8 > s_.cov[idx]) {
            s_.cov[idx] = c8;
        }
    }

    Frame f_;
    Scratch& s_;
};

struct Dash {
    double on = 0.0, off = 0.0;     // on == 0: solid
};

// One segment of a dashed path. `phase` is the distance into the pattern where the segment starts; it comes back where the segment
// ends. A dash is drawn as a capsule shortened by the cap radius at its TRUE ends only, so the dash is `on` long, caps included,
// and a dash that turns a corner keeps its joint.
void dashSegment(Raster& r, double ax, double ay, double bx, double by, double hw, const Dash& dash, double& phase) {
    const double dx = bx - ax, dy = by - ay;
    const double len = std::sqrt(dx * dx + dy * dy);
    const double period = dash.on + dash.off;
    if (!(len > 0.0) || !(period > 0.0)) { return; }
    const double ux = dx / len, uy = dy / len;
    double t0 = 0.0, t1 = 1.0;
    if (!r.visibleRange(ax, ay, bx, by, hw + 1.0, t0, t1)) {
        phase = std::fmod(phase + len, period);
        return;
    }
    const double d1 = t1 * len;
    double d = t0 * len;
    double p = std::fmod(phase + d, period);
    while (d < d1 - 1e-9) {
        if (p < dash.on) {
            const double take = std::min(dash.on - p, d1 - d);
            const bool starts = (p == 0.0);
            const bool ends = (p + take >= dash.on - 1e-9) && (d + take <= len + 1e-9);
            double sa = d + (starts ? hw : 0.0);
            double sb = d + take - (ends ? hw : 0.0);
            if (sb > sa) {
                r.segment(ax + ux * sa, ay + uy * sa, ax + ux * sb, ay + uy * sb, hw);
            } else if (starts && ends) {
                r.dot(ax + ux * (d + take * 0.5), ay + uy * (d + take * 0.5), hw);   // a dash shorter than its own caps
            }
            d += take;
            p += take;
            if (p >= dash.on - 1e-9) { p = dash.on; }
        } else {
            const double take = std::min(period - p, d1 - d);
            d += take;
            p += take;
            if (p >= period - 1e-9) { p = 0.0; }
        }
    }
    phase = std::fmod(phase + len, period);
}

// A polyline of projected vertices, solid or dashed along its length (the pattern starts at the first vertex).
void strokePath(Raster& r, const double* xs, const double* ys, std::size_t n, bool closed, double hw, const Dash& dash) {
    if (n < 2u) { return; }
    const std::size_t segs = closed ? n : n - 1u;
    double phase = 0.0;
    for (std::size_t i = 0; i < segs; ++i) {
        const std::size_t j = (i + 1u == n) ? 0u : i + 1u;
        if (dash.on > 0.0) {
            dashSegment(r, xs[i], ys[i], xs[j], ys[j], hw, dash, phase);
        } else {
            r.segment(xs[i], ys[i], xs[j], ys[j], hw);
        }
    }
}

bool boxTouchesPicture(const double* xs, const double* ys, std::size_t n, int w, int h, double margin) {
    double x0 = std::numeric_limits<double>::infinity(), x1 = -x0, y0 = x0, y1 = -x0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!finite2(xs[i], ys[i])) { continue; }
        x0 = std::min(x0, xs[i]);
        x1 = std::max(x1, xs[i]);
        y0 = std::min(y0, ys[i]);
        y1 = std::max(y1, ys[i]);
    }
    return x1 >= -margin && x0 <= w + margin && y1 >= -margin && y0 <= h + margin;
}

// The 1-2-2.5-5 ladder, the smallest step above `minStep`.
double niceKmStep(double minStep) {
    static const double mantissa[] = {1.0, 2.0, 2.5, 5.0};
    for (double decade = 0.001; decade < 1e7; decade *= 10.0) {
        for (double m : mantissa) {
            if (m * decade > minStep) { return m * decade; }
        }
    }
    return 5e6;
}

std::uint64_t tileKey(std::uint32_t z, std::uint64_t x, std::uint64_t y) {
    return (static_cast<std::uint64_t>(z) << 58) | (y << 29) | x;
}

}  // namespace

// ========================================================================================================================
// the compositor
// ========================================================================================================================
struct TileCompositor::Impl {
    // ---- what the host set ----
    int width = 0, height = 0;
    double centreLat = 0.0, centreLon = 0.0, span = 360.0;
    bool mercator = false;
    TileSource source;
    std::uint32_t tileSize = 256, minZoom = 0, maxZoom = 19;
    bool layerCoast = true, layerGraticule = true;
    bool hasHome = false;
    double homeLat = 0.0, homeLon = 0.0;
    std::vector<double> coverage;
    std::vector<Trail> trails;
    int trailPx = 2;                                   // the bounded width (a trail of 2 px, a line, until setTrailLook)
    TrailStyle trailStyle = TrailStyle::Line;

    // ---- the state ----
    bool dirty = true;
    std::uint64_t frame = 0;                       // one per compose() call that reached the picture's size check
    std::uint64_t nextRetry = std::numeric_limits<std::uint64_t>::max();
    struct Tile {
        std::vector<std::uint8_t> rgb;
        std::uint64_t lastUsed = 0;
    };
    std::unordered_map<std::uint64_t, Tile> tiles;                  // READY, RGB24 tileSize * tileSize
    std::unordered_map<std::uint64_t, std::uint64_t> missing;       // key -> the frame the MISSING was recorded on
    std::unordered_set<std::uint64_t> asked;                        // keys asked in the compose in progress
    std::vector<std::uint8_t> fetched;                              // the plugin writes into this

    Scratch scratch;
    ScanFill fill;
    std::vector<double> px, py;                                     // projected vertices, reused

    View view() const { return View::make(width, height, centreLat, centreLon, span, mercator); }
    bool sizeOk() const { return width > 0 && height > 0 && static_cast<long>(width) * height <= kMaxPixels; }

    // ---- tiles ----
    std::size_t tileCap() const {
        const double bytes = static_cast<double>(tileSize) * tileSize * 3.0;
        return static_cast<std::size_t>(std::clamp(kTileBudgetBytes / bytes, 4.0, static_cast<double>(TileCompositor::kMaxTiles)));
    }
    int zoomFor(const View& v) const {
        const double want = std::log2(v.pxPerWorld / static_cast<double>(tileSize));
        const double z = std::floor(want + 0.5);
        return static_cast<int>(std::clamp(z, static_cast<double>(minZoom), static_cast<double>(maxZoom)));
    }
    void updateRetry() {
        nextRetry = std::numeric_limits<std::uint64_t>::max();
        for (const auto& kv : missing) { nextRetry = std::min(nextRetry, kv.second + TileCompositor::kMissingRetryComposes); }
    }
    const std::uint8_t* fetch(std::uint32_t z, std::uint64_t tx, std::uint64_t ty, bool& pending) {
        const std::uint64_t key = tileKey(z, tx, ty);
        const auto it = tiles.find(key);
        if (it != tiles.end()) {
            it->second.lastUsed = frame;
            return it->second.rgb.data();
        }
        if (missing.count(key) != 0u) { return nullptr; }              // believed
        if (!asked.insert(key).second) { return nullptr; }             // asked already in this compose (a PENDING answer)
        const std::size_t need = static_cast<std::size_t>(tileSize) * tileSize * 3u;
        fetched.clear();
        int got = -1;
        try {
            got = source(z, static_cast<std::uint32_t>(tx), static_cast<std::uint32_t>(ty), fetched);
        } catch (...) {
            got = -1;
        }
        if (got == 0) {
            pending = true;                                            // never cached: the next compose asks again
            return nullptr;
        }
        if (got != 1 || fetched.size() < need) {
            if (missing.size() >= kMaxMissing) { missing.clear(); }    // (a view panned over thousands of absent tiles: forget, ask again)
            missing[key] = frame;                                      // a short buffer is a tile that will not come
            return nullptr;
        }
        Tile t;
        t.rgb.assign(fetched.begin(), fetched.begin() + static_cast<std::ptrdiff_t>(need));
        t.lastUsed = frame;
        const auto placed = tiles.emplace(key, std::move(t));
        return placed.first->second.rgb.data();
    }
    void evict() {
        const std::size_t cap = tileCap();
        if (tiles.size() <= cap) { return; }
        std::vector<std::pair<std::uint64_t, std::uint64_t>> byAge;
        byAge.reserve(tiles.size());
        for (const auto& kv : tiles) { byAge.emplace_back(kv.second.lastUsed, kv.first); }
        std::sort(byAge.begin(), byAge.end());
        const std::size_t drop = tiles.size() - cap;
        for (std::size_t i = 0; i < drop && i < byAge.size(); ++i) { tiles.erase(byAge[i].second); }
    }

    // A tile's pixels at its place: the edges are where the tile's own edges project, so neighbours abut without a gap. At scale
    // 1 the tile is copied from a whole-pixel origin; otherwise each pixel is bilinear from its own tile (clamped at the tile's
    // edge, as GL_CLAMP_TO_EDGE does).
    void blit(const Frame& f, const std::uint8_t* rgb, double xW, double xE, double yN, double yS, long originX, long originY) {
        const long ts = static_cast<long>(tileSize);
        const double sx = (xE - xW) / ts, sy = (yS - yN) / ts;
        if (std::fabs(sx - 1.0) < 1e-6 && std::fabs(sy - 1.0) < 1e-6) {
            const long x0 = std::max(0L, originX), x1 = std::min(static_cast<long>(f.w), originX + ts);
            const long y0 = std::max(0L, originY), y1 = std::min(static_cast<long>(f.h), originY + ts);
            if (x1 <= x0 || y1 <= y0) { return; }
            for (long y = y0; y < y1; ++y) {
                const std::uint8_t* src = rgb + (static_cast<std::size_t>(y - originY) * static_cast<std::size_t>(ts) + static_cast<std::size_t>(x0 - originX)) * 3u;
                std::uint8_t* dst = f.at(static_cast<int>(x0), static_cast<int>(y));
                for (long x = x0; x < x1; ++x, src += 3, dst += 4) {
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = 255;
                }
            }
            return;
        }
        const long x0 = static_cast<long>(std::max(0.0, std::ceil(xW - 0.5))), x1 = static_cast<long>(std::min(static_cast<double>(f.w), std::ceil(xE - 0.5)));
        const long y0 = static_cast<long>(std::max(0.0, std::ceil(yN - 0.5))), y1 = static_cast<long>(std::min(static_cast<double>(f.h), std::ceil(yS - 0.5)));
        if (x1 <= x0 || y1 <= y0) { return; }
        // the columns' source texels and weights, once per tile
        struct Tap {
            int i0, i1, w;     // w: 0..256 weight of i1
        };
        std::vector<Tap> cols(static_cast<std::size_t>(x1 - x0));
        for (long x = x0; x < x1; ++x) {
            const double u = std::clamp((x + 0.5 - xW) / sx - 0.5, 0.0, static_cast<double>(ts - 1));
            const int i0 = static_cast<int>(u);
            cols[static_cast<std::size_t>(x - x0)] = {i0, static_cast<int>(std::min<long>(i0 + 1, ts - 1)), static_cast<int>((u - i0) * 256.0)};
        }
        for (long y = y0; y < y1; ++y) {
            const double v = std::clamp((y + 0.5 - yN) / sy - 0.5, 0.0, static_cast<double>(ts - 1));
            const long j0 = static_cast<long>(v);
            const long j1 = std::min(j0 + 1, ts - 1);
            const int wy = static_cast<int>((v - j0) * 256.0);
            const std::uint8_t* r0 = rgb + static_cast<std::size_t>(j0) * static_cast<std::size_t>(ts) * 3u;
            const std::uint8_t* r1 = rgb + static_cast<std::size_t>(j1) * static_cast<std::size_t>(ts) * 3u;
            std::uint8_t* dst = f.at(static_cast<int>(x0), static_cast<int>(y));
            for (long x = x0; x < x1; ++x, dst += 4) {
                const Tap& t = cols[static_cast<std::size_t>(x - x0)];
                const std::uint8_t* a = r0 + static_cast<std::size_t>(t.i0) * 3u;
                const std::uint8_t* b = r0 + static_cast<std::size_t>(t.i1) * 3u;
                const std::uint8_t* c = r1 + static_cast<std::size_t>(t.i0) * 3u;
                const std::uint8_t* d = r1 + static_cast<std::size_t>(t.i1) * 3u;
                for (int k = 0; k < 3; ++k) {
                    const int top = a[k] * (256 - t.w) + b[k] * t.w;
                    const int bot = c[k] * (256 - t.w) + d[k] * t.w;
                    dst[k] = static_cast<std::uint8_t>((top * (256 - wy) + bot * wy) >> 16);
                }
                dst[3] = 255;
            }
        }
    }

    // Every visible tile at the zoom, asked of the source unless cached or believed missing. Returns whether any tile was drawn.
    bool drawTiles(const Frame& f, const View& v, bool& pending) {
        if (!source) { return false; }
        const int z = zoomFor(v);
        const double n = std::ldexp(1.0, z);
        const long nl = static_cast<long>(n);
        const double x0n = (v.cLon - v.span * 0.5 + 180.0) / 360.0;
        const double x1n = (v.cLon + v.span * 0.5 + 180.0) / 360.0;
        const double halfH = (v.h * 0.5) / v.pxPerWorld;
        const double y0n = v.cMercY - halfH, y1n = v.cMercY + halfH;
        long tx0 = static_cast<long>(std::floor(x0n * n)), tx1 = static_cast<long>(std::floor(x1n * n));
        long ty0 = static_cast<long>(std::floor(y0n * n)), ty1 = static_cast<long>(std::floor(y1n * n));
        if (tx1 < tx0) { std::swap(tx0, tx1); }
        ty0 = std::max(ty0, 0L);
        ty1 = std::min(ty1, nl - 1);
        if (tx1 - tx0 + 1 > kMaxTilesPerAxis) { tx1 = tx0 + kMaxTilesPerAxis - 1; }
        if (ty1 - ty0 + 1 > kMaxTilesPerAxis) { ty1 = ty0 + kMaxTilesPerAxis - 1; }
        // the whole-pixel origin of the first tile, for the scale-1 case: its neighbours are a tile size apart exactly
        const double xFirst = v.xOfLon(static_cast<double>(tx0) / n * 360.0 - 180.0);
        const double yFirst = (static_cast<double>(ty0) / n - v.cMercY) * v.pxPerWorld + v.h * 0.5;
        const long baseX = std::lround(xFirst), baseY = std::lround(yFirst);
        bool any = false;
        for (long ty = ty0; ty <= ty1; ++ty) {
            for (long txRaw = tx0; txRaw <= tx1; ++txRaw) {
                const double xW = v.xOfLon(static_cast<double>(txRaw) / n * 360.0 - 180.0);
                const double xE = v.xOfLon(static_cast<double>(txRaw + 1) / n * 360.0 - 180.0);
                const double yN = (static_cast<double>(ty) / n - v.cMercY) * v.pxPerWorld + v.h * 0.5;
                const double yS = (static_cast<double>(ty + 1) / n - v.cMercY) * v.pxPerWorld + v.h * 0.5;
                if (xE <= 0.0 || xW >= v.w || yS <= 0.0 || yN >= v.h) { continue; }   // not on the picture: not asked for
                long tx = txRaw % nl;
                if (tx < 0) { tx += nl; }
                const std::uint8_t* rgb = fetch(static_cast<std::uint32_t>(z), static_cast<std::uint64_t>(tx), static_cast<std::uint64_t>(ty), pending);
                if (rgb == nullptr) { continue; }
                blit(f, rgb, xW, xE, yN, yS, baseX + (txRaw - tx0) * static_cast<long>(tileSize), baseY + (ty - ty0) * static_cast<long>(tileSize));
                any = true;
            }
        }
        return any;
    }

    // ---- the land ----
    // The latitudes the picture shows, for culling rings that are wholly above or below it.
    void latRange(const View& v, double& south, double& north) const {
        north = v.latOfY(0.0);
        south = v.latOfY(v.h);
        if (south > north) { std::swap(south, north); }
    }
    // The world copies (whole turns of longitude) of a ring that can touch the picture.
    static void copyRange(const View& v, const Ring& r, int& lo, int& hi) {
        const double west = v.cLon - v.span * 0.5, east = v.cLon + v.span * 0.5;
        lo = static_cast<int>(std::clamp(std::floor((west - r.maxLon) / 360.0), -16.0, 16.0));
        hi = static_cast<int>(std::clamp(std::ceil((east - r.minLon) / 360.0), -16.0, 16.0));
    }
    void projectRing(const View& v, const Ring& r, double offsetDeg) {
        const std::size_t n = r.lon.size();
        px.resize(n);
        py.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            px[i] = v.xOfLon(r.lon[i] + offsetDeg);
            py[i] = v.yOfLat(r.lat[i]);
        }
    }

    void drawLand(const Frame& f, const View& v) {
        double south = 0.0, north = 0.0;
        latRange(v, south, north);
        const auto& rings = coastRings();
        fill.begin(f.w, f.h);
        for (const Ring& r : rings) {
            if (!r.closed || r.maxLat < south || r.minLat > north) { continue; }
            int lo = 0, hi = 0;
            copyRange(v, r, lo, hi);
            for (int k = lo; k <= hi; ++k) {
                const double off = 360.0 * k;
                if (r.maxLon + off < v.cLon - v.span * 0.5 || r.minLon + off > v.cLon + v.span * 0.5) { continue; }
                projectRing(v, r, off);
                // A ring wholly to one side of the picture toggles the parity twice before any pixel: it is not needed.
                if (!boxTouchesPicture(px.data(), py.data(), px.size(), f.w, f.h, 1.0)) { continue; }
                fill.addRing(px.data(), py.data(), px.size());
            }
        }
        fill.run([&](int y, int x0, int x1) {
            std::uint8_t* p = f.at(x0, y);
            for (int x = x0; x < x1; ++x, p += 4) {
                p[0] = kLand.r;
                p[1] = kLand.g;
                p[2] = kLand.b;
            }
        });
        // the coast over the fill, every ring (and every open stub) once per world copy
        Raster raster(f, scratch);
        const Dash solid;
        for (const Ring& r : rings) {
            if (r.lon.size() < 2u || r.maxLat < south - 1.0 || r.minLat > north + 1.0) { continue; }
            int lo = 0, hi = 0;
            copyRange(v, r, lo, hi);
            for (int k = lo; k <= hi; ++k) {
                const double off = 360.0 * k;
                if (r.maxLon + off < v.cLon - v.span * 0.5 - 1.0 || r.minLon + off > v.cLon + v.span * 0.5 + 1.0) { continue; }
                projectRing(v, r, off);
                if (!boxTouchesPicture(px.data(), py.data(), px.size(), f.w, f.h, 2.0)) { continue; }
                raster.begin();
                strokePath(raster, px.data(), py.data(), px.size(), r.closed, 0.5, solid);
                raster.end(kCoast, 255);
            }
        }
    }

    // ---- the graticule ----
    static double graticuleStep(double pxPerDeg) {
        static const double steps[] = {0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 45.0};
        for (double s : steps) {
            if (s * pxPerDeg >= kMinLinePx) { return s; }
        }
        return 45.0;
    }
    void drawGraticule(const Frame& f, const View& v) {
        const double step = graticuleStep(v.pxPerDeg);
        const double limit = v.latLimit();
        const double west = v.cLon - v.span * 0.5, east = v.cLon + v.span * 0.5;
        // the rows the meridians run over: from the north limit to the south limit, clipped to the picture
        const double yNorth = v.yOfLat(limit), ySouth = v.yOfLat(-limit);
        const int r0 = static_cast<int>(std::clamp(std::ceil(yNorth - 0.5), 0.0, static_cast<double>(f.h)));
        const int r1 = static_cast<int>(std::clamp(std::ceil(ySouth - 0.5), 0.0, static_cast<double>(f.h)));
        const double k0 = std::ceil(west / step), k1 = std::floor(east / step);
        for (double k = k0; k <= k1 && k - k0 < 4096.0; k += 1.0) {
            const double x = std::floor(v.xOfLon(k * step));
            if (x < 0.0 || x >= f.w) { continue; }
            for (int y = r0; y < r1; ++y) {
                std::uint8_t* p = f.at(static_cast<int>(x), y);
                p[0] = kGraticule.r;
                p[1] = kGraticule.g;
                p[2] = kGraticule.b;
            }
        }
        double south = 0.0, north = 0.0;
        latRange(v, south, north);
        south = std::max(south, -limit);
        north = std::min(north, limit);
        const double j0 = std::ceil(south / step), j1 = std::floor(north / step);
        for (double j = j0; j <= j1 && j - j0 < 4096.0; j += 1.0) {
            const double y = std::floor(v.yOfLat(j * step));
            if (y < 0.0 || y >= f.h) { continue; }
            std::uint8_t* p = f.at(0, static_cast<int>(y));
            for (int x = 0; x < f.w; ++x, p += 4) {
                p[0] = kGraticule.r;
                p[1] = kGraticule.g;
                p[2] = kGraticule.b;
            }
        }
    }

    // ---- the home: range rings and coverage ----
    double metresPerPixel(const View& v) const {
        return kMetresPerDegree * std::cos(v.cLat * kDegToRad) / v.pxPerDeg;
    }
    // Where the home is, as an offset in degrees of longitude from the view centre (the short way) and a pixel row.
    void homeAt(const View& v, double& xPx, double& yPx) const {
        xPx = v.xOfDelta(foldDelta(homeLon - v.cLon));
        yPx = v.yOfLat(homeLat);
    }

    void drawRings(const Frame& f, const View& v) {
        const double mpp = metresPerPixel(v);
        const double reachKm = 0.5 * std::hypot(v.w, v.h) * mpp / 1000.0;
        if (!(reachKm > 0.0) || !std::isfinite(reachKm)) { return; }
        const double step = niceKmStep(reachKm / 7.0);
        // the band of distances the picture covers: sampled round its edge, and 0 when the home is on it
        double hx = 0.0, hy = 0.0;
        homeAt(v, hx, hy);
        double dMin = std::numeric_limits<double>::infinity(), dMax = 0.0;
        for (int i = 0; i <= 16; ++i) {
            const double t = i / 16.0;
            const double edge[4][2] = {{t * v.w, 0.0}, {t * v.w, v.h}, {0.0, t * v.h}, {v.w, t * v.h}};
            for (const auto& e : edge) {
                const double lat = v.latOfY(e[1]);
                const double lon = v.cLon + (e[0] - v.w * 0.5) / v.pxPerDeg;
                const double km = greatCircleKm(homeLat, homeLon, std::clamp(lat, -90.0, 90.0), lon);
                dMin = std::min(dMin, km);
                dMax = std::max(dMax, km);
            }
        }
        const bool inside = hx >= 0.0 && hx <= v.w && hy >= 0.0 && hy <= v.h;
        if (inside) { dMin = 0.0; } else { dMin *= 0.9; }
        const long first = std::max(1L, static_cast<long>(std::ceil(dMin / step)));
        const long last = std::min(static_cast<long>(std::floor(dMax / step)), first + 40L);
        Raster raster(f, scratch);
        const Dash solid;
        for (long k = first; k <= last; ++k) {
            const double km = static_cast<double>(k) * step;
            if (km >= kEarthRadiusKm * kPi) { break; }
            const double rPx = km * 1000.0 / mpp;
            const int count = static_cast<int>(std::clamp(std::ceil(std::sqrt(49.3 * rPx)), 24.0, 720.0));
            px.resize(static_cast<std::size_t>(count));
            py.resize(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) {
                const Destination d = destination(homeLat, 360.0 * i / count, km);
                px[static_cast<std::size_t>(i)] = hx + d.dLonDeg * v.pxPerDeg;
                py[static_cast<std::size_t>(i)] = v.yOfLat(d.latDeg);
            }
            for (int copy = -1; copy <= 1; ++copy) {
                const double shift = copy * 360.0 * v.pxPerDeg;
                std::vector<double> sx(px);
                for (double& x : sx) { x += shift; }
                if (!boxTouchesPicture(sx.data(), py.data(), sx.size(), f.w, f.h, 2.0)) { continue; }
                raster.begin();
                strokePath(raster, sx.data(), py.data(), sx.size(), true, 0.5, solid);
                raster.end(kRing, 255);
            }
        }
    }

    // The coverage wedges: one filled wedge per five-degree bucket from the home out to its km. They are filled as ONE outline - each
    // bucket's arc, then a step along the shared bearing to the next bucket's arc, and the home itself wherever a bucket is empty -
    // because two wedges filled apart leave a hair of sea along every shared bearing (the straight edges to two different radii
    // are not the same line once projected) and double the colour wherever they overlap.
    void drawCoverage(const Frame& f, const View& v) {
        double hx = 0.0, hy = 0.0;
        homeAt(v, hx, hy);
        const std::size_t buckets = std::min<std::size_t>(coverage.size(), 72u);
        constexpr int kArc = 5;                       // an arc is drawn in steps of a degree: a great circle bends when projected
        std::vector<double> ox, oy;
        bool filled = false, lastWasHome = false;
        for (std::size_t b = 0; b < 72u; ++b) {
            const double km = b < buckets ? coverage[b] : 0.0;
            if (!(km > 0.0)) {
                if (!lastWasHome) {
                    ox.push_back(hx);
                    oy.push_back(hy);
                    lastWasHome = true;
                }
                continue;
            }
            filled = true;
            lastWasHome = false;
            for (int j = 0; j <= kArc; ++j) {
                const Destination d = destination(homeLat, static_cast<double>(b) * 5.0 + 5.0 * j / kArc, km);
                ox.push_back(hx + d.dLonDeg * v.pxPerDeg);
                oy.push_back(v.yOfLat(std::clamp(d.latDeg, -89.9, 89.9)));
            }
        }
        if (!filled) { return; }
        scratch.mask.assign(static_cast<std::size_t>(f.w) * static_cast<std::size_t>(f.h), 0u);
        bool any = false;
        std::vector<double> sx(ox.size());
        for (int copy = -1; copy <= 1; ++copy) {
            const double shift = copy * 360.0 * v.pxPerDeg;
            for (std::size_t i = 0; i < ox.size(); ++i) { sx[i] = ox[i] + shift; }
            if (!boxTouchesPicture(sx.data(), oy.data(), sx.size(), f.w, f.h, 1.0)) { continue; }
            fill.begin(f.w, f.h);
            fill.addRing(sx.data(), oy.data(), sx.size());
            fill.run([&](int y, int x0, int x1) {
                std::uint8_t* m = scratch.mask.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(f.w);
                std::fill(m + x0, m + x1, static_cast<std::uint8_t>(1));
                any = true;
            });
        }
        if (!any) { return; }
        // composited once, through a mask, so a pixel two copies of the outline both cover is not doubled either
        for (int y = 0; y < f.h; ++y) {
            const std::uint8_t* m = scratch.mask.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(f.w);
            std::uint8_t* p = f.at(0, y);
            for (int x = 0; x < f.w; ++x, p += 4) {
                if (m[x] != 0u) { blendPixel(p, kCoverage, kCoverageAlpha); }
            }
        }
    }

    // ---- the trails ----
    void drawTrails(const Frame& f, const View& v) {
        const double trailStrokePx = static_cast<double>(trailPx) + (trailStyle == TrailStyle::Ribbon ? kRibbonExtraPx : 0);
        Raster raster(f, scratch);
        std::vector<double> ux, uy, sx;
        for (const Trail& t : trails) {
            const int alpha = static_cast<int>(t.rgba & 0xffu);
            if (alpha == 0 || t.points.size() < 2u) { continue; }
            const Rgb colour{static_cast<std::uint8_t>(t.rgba >> 24), static_cast<std::uint8_t>(t.rgba >> 16), static_cast<std::uint8_t>(t.rgba >> 8)};
            // Each vertex is placed relative to the one before it, the SHORT way round, so a track that crosses the antimeridian
            // is one line; the whole is then offered once per turn of longitude it can touch.
            ux.clear();
            uy.clear();
            double prevLon = 0.0, u = 0.0;
            bool first = true;
            for (const auto& p : t.points) {
                if (!finite2(p.first, p.second)) { continue; }
                if (first) {
                    u = foldDelta(p.second - v.cLon);
                    first = false;
                } else {
                    u += foldDelta(p.second - prevLon);
                }
                prevLon = p.second;
                ux.push_back(u);
                uy.push_back(v.yOfLat(p.first));
            }
            if (ux.size() < 2u) { continue; }
            double uMin = ux.front(), uMax = ux.front();
            for (double q : ux) {
                uMin = std::min(uMin, q);
                uMax = std::max(uMax, q);
            }
            const double halfSpan = v.span * 0.5;
            const int kLo = static_cast<int>(std::clamp(std::ceil((-halfSpan - uMax) / 360.0), -4.0, 4.0));
            const int kHi = static_cast<int>(std::clamp(std::floor((halfSpan - uMin) / 360.0), -4.0, 4.0));
            // The width is the Display setting's (setTrailLook). A dash is a capsule, its caps as wide as the line is, so the pattern grows with the
            // half-width past 1 px (a 6 px dash with 43 px caps would be a solid bar): 6 on, 4 off at the 2 px default, 60 on, 40 off at 20 px.
            const double hw = trailStrokePx * 0.5;
            const double dashScale = std::max(1.0, hw);
            const Dash dash = t.dashed ? Dash{kDashOn * dashScale, kDashOff * dashScale} : Dash{};
            for (int k = kLo; k <= kHi; ++k) {
                sx.resize(ux.size());
                for (std::size_t i = 0; i < ux.size(); ++i) { sx[i] = v.xOfDelta(ux[i] + 360.0 * k); }
                if (!boxTouchesPicture(sx.data(), uy.data(), sx.size(), f.w, f.h, 3.0)) { continue; }
                raster.begin();
                strokePath(raster, sx.data(), uy.data(), sx.size(), t.closed, hw, dash);
                raster.end(colour, alpha);
            }
        }
    }
};

// ========================================================================================================================
// the public face
// ========================================================================================================================
TileCompositor::TileCompositor() : impl_(new Impl) {}
TileCompositor::~TileCompositor() { delete impl_; }

void TileCompositor::setSize(int width, int height) {
    width = std::clamp(width, 0, kMaxDimension);
    height = std::clamp(height, 0, kMaxDimension);
    if (width == impl_->width && height == impl_->height) { return; }
    impl_->width = width;
    impl_->height = height;
    impl_->dirty = true;
}

void TileCompositor::setView(double centreLatDeg, double centreLonDeg, double spanDeg) {
    // A place that is not a place is not gone to: a NaN centre is a NaN in every projected coordinate.
    if (!std::isfinite(centreLatDeg) || !std::isfinite(centreLonDeg) || !std::isfinite(spanDeg)) { return; }
    const double lat = std::clamp(centreLatDeg, -kMaxCentreLat, kMaxCentreLat);
    const double lon = normaliseLon(centreLonDeg);
    const double span = std::clamp(spanDeg, kMinSpanDeg, kMaxSpanDeg);
    if (lat == impl_->centreLat && lon == impl_->centreLon && span == impl_->span) { return; }
    impl_->centreLat = lat;
    impl_->centreLon = lon;
    impl_->span = span;
    impl_->dirty = true;
}

void TileCompositor::setMercator(bool mercator) {
    if (mercator == impl_->mercator) { return; }
    impl_->mercator = mercator;
    impl_->dirty = true;
}

void TileCompositor::setTileSource(TileSource source, std::uint32_t tileSize, std::uint32_t minZoom, std::uint32_t maxZoom) {
    // A new source is a new set of tiles; whatever was cached belongs to the old one.
    impl_->tiles.clear();
    impl_->missing.clear();
    impl_->nextRetry = std::numeric_limits<std::uint64_t>::max();
    impl_->dirty = true;
    if (!source) {
        impl_->source = nullptr;
        return;
    }
    impl_->source = std::move(source);
    impl_->tileSize = std::clamp<std::uint32_t>(tileSize, 64u, 1024u);
    impl_->minZoom = std::min(minZoom, kMaxZoomLevel);
    impl_->maxZoom = std::clamp(maxZoom, impl_->minZoom, kMaxZoomLevel);
}

void TileCompositor::clearTiles() {
    impl_->tiles.clear();
    impl_->missing.clear();
    impl_->nextRetry = std::numeric_limits<std::uint64_t>::max();
    impl_->dirty = true;
}

void TileCompositor::setLayers(bool coastline, bool graticule) {
    if (coastline == impl_->layerCoast && graticule == impl_->layerGraticule) { return; }
    impl_->layerCoast = coastline;
    impl_->layerGraticule = graticule;
    impl_->dirty = true;
}

void TileCompositor::setHome(bool has, double latDeg, double lonDeg) {
    if (has && !finite2(latDeg, lonDeg)) { has = false; }       // a home that is not a place is no home
    if (!has) {
        latDeg = 0.0;
        lonDeg = 0.0;
    }
    if (has == impl_->hasHome && latDeg == impl_->homeLat && lonDeg == impl_->homeLon) { return; }
    impl_->hasHome = has;
    impl_->homeLat = std::clamp(latDeg, -90.0, 90.0);
    impl_->homeLon = lonDeg;
    impl_->dirty = true;
}

void TileCompositor::setCoverage(const std::vector<double>& kmPerBucket) {
    std::vector<double> clean(kmPerBucket.begin(), kmPerBucket.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(kmPerBucket.size(), 72u)));
    bool any = false;
    for (double& km : clean) {
        if (!std::isfinite(km) || km < 0.0) { km = 0.0; }           // nothing heard that way
        if (km > 0.0) { any = true; }
    }
    if (!any) { clean.clear(); }                                    // nothing in any wedge is the overlay off
    if (clean == impl_->coverage) { return; }
    impl_->coverage = std::move(clean);
    impl_->dirty = true;
}

void TileCompositor::setTrails(const std::vector<Trail>& trails) {
    std::vector<Trail> clean;
    clean.reserve(trails.size());
    for (const Trail& t : trails) {
        Trail c;
        c.rgba = t.rgba;
        c.dashed = t.dashed;
        c.closed = t.closed;
        c.points.reserve(t.points.size());
        for (const auto& p : t.points) {
            if (finite2(p.first, p.second)) { c.points.push_back(p); }   // a vertex that is not a place is not drawn
        }
        clean.push_back(std::move(c));
    }
    const auto same = [](const Trail& a, const Trail& b) {
        return a.rgba == b.rgba && a.dashed == b.dashed && a.closed == b.closed && a.points == b.points;
    };
    if (clean.size() == impl_->trails.size() && std::equal(clean.begin(), clean.end(), impl_->trails.begin(), same)) { return; }
    impl_->trails = std::move(clean);
    impl_->dirty = true;
}

bool TileCompositor::compose(std::uint8_t* rgba, int width, int height) {
    Impl& m = *impl_;
    if (rgba == nullptr || width != m.width || height != m.height || !m.sizeOk()) { return false; }
    ++m.frame;
    // (Which buffer it is does not matter: a host that alternates two, as the spectrum's does, asks the other one and is told false, and
    // goes on showing the picture it has. A change repaints whichever buffer it is asked into, whole.)
    // A MISSING that has been believed long enough is asked again, even when nothing else moved.
    const bool tilePath = m.source && m.mercator;
    if (!m.dirty && m.frame >= m.nextRetry) { m.dirty = true; }
    if (!m.dirty) { return false; }

    const Frame f{rgba, width, height};
    const View v = m.view();
    m.scratch.size(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));

    // the sea
    {
        std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 4u);
        for (int x = 0; x < width; ++x) {
            row[static_cast<std::size_t>(x) * 4u + 0u] = kSea.r;
            row[static_cast<std::size_t>(x) * 4u + 1u] = kSea.g;
            row[static_cast<std::size_t>(x) * 4u + 2u] = kSea.b;
            row[static_cast<std::size_t>(x) * 4u + 3u] = 255;
        }
        for (int y = 0; y < height; ++y) { std::memcpy(f.at(0, y), row.data(), row.size()); }
    }

    // the tiles (Mercator with a source), else the coastline; with a source and not one tile drawn the coastline is the picture,
    // as in the ImGui view (a map with no tiles yet, or none reachable, is not a blank)
    bool pending = false;
    bool tiled = false;
    // (the retry clock belongs to the tile path: a view that no longer asks for tiles stops it, or every clean call after it would compose)
    if (!tilePath) { m.nextRetry = std::numeric_limits<std::uint64_t>::max(); }
    if (tilePath) {
        m.asked.clear();
        for (auto it = m.missing.begin(); it != m.missing.end();) {
            if (m.frame - it->second >= TileCompositor::kMissingRetryComposes) { it = m.missing.erase(it); } else { ++it; }
        }
        tiled = m.drawTiles(f, v, pending);
        m.evict();
        m.updateRetry();
    }
    if (m.layerCoast && !tiled) { m.drawLand(f, v); }
    if (m.layerGraticule) { m.drawGraticule(f, v); }
    if (m.hasHome) {
        m.drawRings(f, v);
        if (!m.coverage.empty()) { m.drawCoverage(f, v); }
    }
    if (!m.trails.empty()) { m.drawTrails(f, v); }

    m.dirty = pending;                      // a PENDING tile is asked again by the next compose
    return true;
}

bool TileCompositor::project(double latDeg, double lonDeg, double& fx, double& fy) const {
    const Impl& m = *impl_;
    if (!finite2(latDeg, lonDeg)) {
        fx = fy = std::numeric_limits<double>::quiet_NaN();
        return false;
    }
    const double aspect = (m.width > 0 && m.height > 0) ? static_cast<double>(m.width) / m.height : 1.0;
    fx = 0.5 + foldDelta(lonDeg - m.centreLon) / m.span;
    if (m.mercator) {
        const double cy = mercY(std::clamp(m.centreLat, -kMercatorLatLimit, kMercatorLatLimit));
        fy = 0.5 + (mercY(latDeg) - cy) * 360.0 * aspect / m.span;
    } else {
        fy = 0.5 + (m.centreLat - latDeg) * aspect / m.span;
    }
    return fx >= 0.0 && fx <= 1.0 && fy >= 0.0 && fy <= 1.0;
}

void TileCompositor::unproject(double fx, double fy, double& latDeg, double& lonDeg) const {
    const Impl& m = *impl_;
    const double aspect = (m.width > 0 && m.height > 0) ? static_cast<double>(m.width) / m.height : 1.0;
    lonDeg = normaliseLon(m.centreLon + (fx - 0.5) * m.span);
    if (m.mercator) {
        const double cy = mercY(std::clamp(m.centreLat, -kMercatorLatLimit, kMercatorLatLimit));
        latDeg = mercLat(cy + (fy - 0.5) * m.span / (360.0 * aspect));
    } else {
        latDeg = m.centreLat - (fy - 0.5) * m.span / aspect;
    }
}

double TileCompositor::metresPerPixel() const {
    const Impl& m = *impl_;
    const View v = m.view();
    return m.metresPerPixel(v);
}

int TileCompositor::tileZoom() const {
    const Impl& m = *impl_;
    if (!m.source || !m.mercator || m.width <= 0 || m.height <= 0) { return -1; }
    return m.zoomFor(m.view());
}

std::size_t TileCompositor::residentTiles() const { return impl_->tiles.size(); }
double TileCompositor::centreLatDeg() const { return impl_->centreLat; }
double TileCompositor::centreLonDeg() const { return impl_->centreLon; }
double TileCompositor::spanDeg() const { return impl_->span; }
bool TileCompositor::mercator() const { return impl_->mercator; }

// ------------------------------------------------------------------------------------------------------------------------
// the display settings the map applies
// ------------------------------------------------------------------------------------------------------------------------
int clampAircraftIconPx(int px) {
    if (px < kAircraftIconMinPx) { return kAircraftIconMinPx; }
    if (px > kAircraftIconMaxPx) { return kAircraftIconMaxPx; }
    return px;
}

int aircraftWingspanPx(int iconPx) {
    const int span = static_cast<int>(static_cast<float>(clampAircraftIconPx(iconPx)) * kAircraftSpanFraction + 0.5f);
    return span < kTrailWidthMinPx ? kTrailWidthMinPx : span;
}

int clampTrailWidthPx(int widthPx, int iconPx) {
    if (widthPx < kTrailWidthMinPx) { return kTrailWidthMinPx; }
    const int span = aircraftWingspanPx(iconPx);
    return widthPx > span ? span : widthPx;
}

MapLook mapLookOf(int savedIconPx, int savedTrailWidthPx, int savedTrailStyle) {
    MapLook look;
    look.iconPx = clampAircraftIconPx(savedIconPx);
    look.trailWidthPx = clampTrailWidthPx(savedTrailWidthPx, look.iconPx);
    look.trailStyle = savedTrailStyle == 1 ? TrailStyle::Ribbon : TrailStyle::Line;
    return look;
}

int targetIconPx(int kind, const MapLook& look) { return kind == 1 ? look.iconPx : 0; }

void TileCompositor::setTrailLook(int widthPx, int iconPx, TrailStyle style) {
    const int bounded = clampTrailWidthPx(widthPx, iconPx);
    if (style != TrailStyle::Ribbon) { style = TrailStyle::Line; }
    if (bounded == impl_->trailPx && style == impl_->trailStyle) { return; }
    impl_->trailPx = bounded;
    impl_->trailStyle = style;
    impl_->dirty = true;
}
int TileCompositor::trailWidthPx() const { return impl_->trailPx; }
TrailStyle TileCompositor::trailStyle() const { return impl_->trailStyle; }
double TileCompositor::trailStrokePx() const { return static_cast<double>(impl_->trailPx) + (impl_->trailStyle == TrailStyle::Ribbon ? kRibbonExtraPx : 0); }

}  // namespace foxsdr::slintmap
