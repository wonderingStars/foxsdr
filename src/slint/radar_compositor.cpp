// radar_compositor.cpp - the radar tube's ground (see radar_compositor.hpp). A port of the CPU-expressible part of ScopeView::draw (gui/scope_view.cpp 2456-2750 and 3075-3152): the coastline
// through scopeRelative / scopeProject, the wash, the crosshair, the trails, the scan lines, the rings and the ticks. Colours are the classic's own (scope_view.cpp 79-87, 165-173).
//
// DEPARTURES, each deliberate:
//   * PHOSPHOR off paints the plain dark ground, no coastline (the classic only changes how a map TILE is tinted, and the Slint painter has no tiles yet).
//   * With PHOSPHOR on the green wash goes UNDER the coastline (the classic washes tiles; a wash over a one-pixel coast would swallow it).
//   * The rings, ticks and crosshair stay on the middle of the TUBE when the view is panned, as in the classic (scope_view.cpp 2443-2449, 3083-3104); what moves with the view is the ground,
//     the trails and the targets.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "radar_compositor.hpp"

#include <algorithm>
#include <cmath>

#include "gui/coastline_data.hpp"

namespace foxsdr::slintscope {

namespace gui = cascade::gui;

namespace {

constexpr Rgba kGround{10, 28, 13, 255};         // kGround #0a1c0d
constexpr Rgba kWash{24, 74, 26, 120};           // kPhosphorWash
constexpr Rgba kCoast{90, 160, 70, 210};         // kCoast
constexpr Rgba kCrossHair{134, 214, 74, 41};     // kCrossHair
constexpr Rgba kRing{134, 214, 74, 36};          // kRing
constexpr Rgba kRim{134, 214, 74, 77};           // kRim (the outer ring)
constexpr Rgba kTick{134, 214, 74, 90};          // kTick
constexpr Rgba kScan{0, 0, 0, 30};               // the lead's scan line: alpha 30 every 3 px
constexpr double kPi = 3.14159265358979323846;

Rgba fromPacked(std::uint32_t v) {   // 0xRRGGBBAA
    return Rgba{static_cast<std::uint8_t>(v >> 24), static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v)};
}

// A ring stroked per row over only the pixels near it (Canvas::ring walks the whole bounding square).
void strokeRing(Canvas& cv, float cx, float cy, float r, float wd, Rgba c) {
    const float hw = wd * 0.5f;
    const float outer = r + hw + 1.0f, inner = std::max(0.0f, r - hw - 1.0f);
    const int ya = std::max(0, static_cast<int>(std::floor(cy - outer))), yb = std::min(cv.height() - 1, static_cast<int>(std::ceil(cy + outer)));
    for (int y = ya; y <= yb; ++y) {
        const float dy = static_cast<float>(y) + 0.5f - cy;
        if (std::abs(dy) > outer) { continue; }
        const float xo = std::sqrt(outer * outer - dy * dy);
        const float xi = std::abs(dy) < inner ? std::sqrt(inner * inner - dy * dy) : 0.0f;
        const int xa = std::max(0, static_cast<int>(std::floor(cx - xo))), xb = std::min(cv.width() - 1, static_cast<int>(std::ceil(cx + xo)));
        const auto px = [&](int x) {
            const float dx = static_cast<float>(x) + 0.5f - cx;
            const float d = std::abs(std::hypot(dx, dy) - r);
            const float cover = std::clamp(hw + 0.5f - d, 0.0f, 1.0f);
            if (cover > 0.0f) { cv.blend(x, y, c, cover); }
        };
        if (xi > 1.0f) {   // the hollow between the two arcs of the ring on this row has no coverage
            const int a = static_cast<int>(std::ceil(cx - xi + 1.0f)), b = static_cast<int>(std::floor(cx + xi - 1.0f));
            for (int x = xa; x <= xb && x < a; ++x) { px(x); }
            for (int x = std::max(xa, b); x <= xb; ++x) { px(x); }
        } else {
            for (int x = xa; x <= xb; ++x) { px(x); }
        }
    }
}

// The corners go transparent and the rim is anti-aliased: the tube is a disc.
void maskDisc(Rgba* px, int w, int h, float cx, float cy, float r) {
    const float ro = r + 0.5f, ri = r - 0.5f;
    for (int y = 0; y < h; ++y) {
        Rgba* row = px + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        const float dy = static_cast<float>(y) + 0.5f - cy;
        if (std::abs(dy) >= ro) { std::fill(row, row + w, Rgba{0, 0, 0, 0}); continue; }
        const float xo = std::sqrt(ro * ro - dy * dy);
        const float xi = (ri > std::abs(dy)) ? std::sqrt(ri * ri - dy * dy) : 0.0f;
        const int x0 = std::clamp(static_cast<int>(std::floor(cx - xo - 1.0f)), 0, w), x1 = std::clamp(static_cast<int>(std::ceil(cx + xo + 1.0f)), 0, w);
        std::fill(row, row + x0, Rgba{0, 0, 0, 0});
        std::fill(row + x1, row + w, Rgba{0, 0, 0, 0});
        const auto edge = [&](int x) {
            const float dx = static_cast<float>(x) + 0.5f - cx;
            const float cover = std::clamp(ro - std::hypot(dx, dy), 0.0f, 1.0f);
            if (cover <= 0.0f) { row[x] = Rgba{0, 0, 0, 0}; }
            else if (cover < 1.0f) { row[x].a = static_cast<std::uint8_t>(static_cast<float>(row[x].a) * cover + 0.5f); }
        };
        if (xi > 1.0f) {   // the pixels well inside the disc keep what was painted
            const int a = static_cast<int>(std::ceil(cx - xi + 1.0f)), b = static_cast<int>(std::floor(cx + xi - 1.0f));
            for (int x = x0; x < x1 && x < a; ++x) { edge(x); }
            for (int x = std::max(x0, b); x < x1; ++x) { edge(x); }
        } else {
            for (int x = x0; x < x1; ++x) { edge(x); }
        }
    }
}

}  // namespace

std::size_t paintRadarGround(Rgba* px, int w, int h, const RadarScope& scope, RadarPaintScratch& scratch) {
    if (px == nullptr || w <= 0 || h <= 0) { return 0; }
    Canvas cv(px, w, h);
    const int side = std::min(w, h);
    if (side < kRadarMinTubePx) { cv.fill(Rgba{0, 0, 0, 0}); return 0; }
    const float cx = static_cast<float>(w) * 0.5f, cy = static_cast<float>(h) * 0.5f;
    const float radius = static_cast<float>(side) * 0.5f - 2.0f;   // two pixels of margin so the outer ring's stroke is not shaved
    const RadarFrame& fr = scope.frame();
    std::size_t strokes = 0;

    cv.fill(kGround);
    if (!fr.hasRx) {   // the empty state: the view shows the position offers over a dark tube
        maskDisc(px, w, h, cx, cy, radius + 1.0f);
        return 0;
    }

    // ---- the ground: the wash, then the coastline from the middle of the view ----
    if (scope.optPhosphor()) {
        cv.rect(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), kWash);
        const double full = static_cast<double>(fr.rangeNm);
        const double coverNm = full * 1.45;   // the circle through the square's corners, and a little over
        const double viewLat = scope.viewLatDeg(), viewLon = scope.viewLonDeg();
        for (std::uint32_t r = 0; r < gui::coastline::kRunCount; ++r) {
            const gui::coastline::Run& run = gui::coastline::kRuns[r];
            bool havePrev = false, prevInside = false;
            float prevX = 0.0f, prevY = 0.0f;
            double prevLon = 0.0;
            for (std::uint32_t i = 0; i < run.count; ++i) {
                const std::size_t k = (static_cast<std::size_t>(run.first) + i) * 2u;
                const double lon = static_cast<double>(gui::coastline::kCoords[k]) / 100.0;
                const double lat = static_cast<double>(gui::coastline::kCoords[k + 1]) / 100.0;
                // placed by the pair that places a target, and culled by range from the middle of the glass
                const gui::ScopePolar pol = gui::scopeRelative(viewLat, viewLon, lat, lon);
                const bool inside = pol.rangeNm <= coverNm;
                const gui::ScopePoint q = gui::scopeProject(cx, cy, radius, pol.rangeNm, pol.bearingDeg, full);
                const float x = static_cast<float>(q.x), y = static_cast<float>(q.y);
                if (havePrev) {
                    // a segment that jumps more than half the world is a wrap, not a coast
                    const bool wrap = std::fabs(lon - prevLon) > 180.0;
                    if (!wrap && (inside || prevInside)) {
                        cv.line(prevX, prevY, x, y, 1.0f, kCoast);
                        ++strokes;
                    }
                }
                prevX = x;
                prevY = y;
                prevInside = inside;
                prevLon = lon;
                havePrev = true;
            }
        }
    }
    (void)scratch;   // (the coast is stroked point by point; the scratch is kept for the day it is batched)

    // ---- the crosshair, through the middle of the view ----
    cv.rect(std::floor(cx), cy - radius, std::floor(cx) + 1.0f, cy + radius, kCrossHair);
    cv.rect(cx - radius, std::floor(cy), cx + radius, std::floor(cy) + 1.0f, kCrossHair);
    strokes += 2;

    // ---- the trails: LINE one width, RIBBON three pixels wider, the selected aircraft's a little wider again (scope_view.cpp 2804-2812) ----
    if (scope.optTrail() != 0) {
        const float ox = fr.radiusPx > 0.0f ? cx - fr.centreX : 0.0f, oy = fr.radiusPx > 0.0f ? cy - fr.centreY : 0.0f;
        const float base = static_cast<float>(fr.trailWidthPx);
        for (const RadarTrail& t : scope.trails()) {
            const float wd = (scope.optTrail() == 2 ? base + 3.0f : base) * (t.selected ? 1.4f : 1.0f);
            for (std::size_t j = 1; j < t.points.size(); ++j) {
                const RadarTrail::Point& a = t.points[j - 1];
                const RadarTrail::Point& b = t.points[j];
                cv.line(a.x + ox, a.y + oy, b.x + ox, b.y + oy, wd, fromPacked(b.rgba));
                ++strokes;
            }
        }
    }

    // ---- the glass: one dark row every three pixels ----
    for (float y = 1.5f; y < static_cast<float>(h); y += 3.0f) { cv.rect(0.0f, std::floor(y), static_cast<float>(w), std::floor(y) + 1.0f, kScan); }

    // ---- the four rings (the outer one brighter and 1.8 px) and the twelve bearing ticks (the cardinals longer and thicker), above the glass ----
    for (int ring = 1; ring <= gui::kScopeRingCount; ++ring) {
        const float r = radius * static_cast<float>(ring) / static_cast<float>(gui::kScopeRingCount);
        const bool outermost = ring == gui::kScopeRingCount;
        strokeRing(cv, cx, cy, r, outermost ? 1.8f : 1.0f, outermost ? kRim : kRing);
        ++strokes;
    }
    for (int b = 0; b < 360; b += 30) {
        const double a = static_cast<double>(b) * kPi / 180.0;
        const float sx = static_cast<float>(std::sin(a)), sy = static_cast<float>(-std::cos(a));
        const bool cardinal = (b % 90) == 0;
        const float inner = radius * (cardinal ? 0.955f : 0.975f);
        cv.line(cx + sx * inner, cy + sy * inner, cx + sx * radius, cy + sy * radius, cardinal ? 1.8f : 1.0f, kTick);
        ++strokes;
    }

    maskDisc(px, w, h, cx, cy, radius + 1.0f);
    return strokes;
}

void paintSweepFan(Rgba* px, int size) {
    if (px == nullptr || size <= 0) { return; }
    std::fill(px, px + static_cast<std::size_t>(size) * static_cast<std::size_t>(size), Rgba{0, 0, 0, 0});
    constexpr int kSteps = 44;
    constexpr double kSpanDeg = 55.0;
    const float c = static_cast<float>(size) * 0.5f, R = c;
    // the wedge lies up and to the left of the centre (bearing -55..0), so only that quadrant is visited
    const int half = static_cast<int>(std::ceil(c));
    for (int y = 0; y < half; ++y) {
        for (int x = 0; x < half; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - c, dy = static_cast<float>(y) + 0.5f - c;
            if (dx > 0.0f || dy >= 0.0f) { continue; }
            const float d = std::hypot(dx, dy);
            const float cover = std::clamp(R + 0.5f - d, 0.0f, 1.0f);
            if (cover <= 0.0f) { continue; }
            const double bearing = std::atan2(static_cast<double>(dx), static_cast<double>(-dy)) * 180.0 / kPi;   // clockwise from straight up; negative here
            const double t = -bearing / kSpanDeg;
            if (t < 0.0 || t > 1.0) { continue; }
            const int step = std::min(kSteps - 1, static_cast<int>(t * kSteps));
            const double alpha = 77.0 * (1.0 - (static_cast<double>(step) + 0.5) / kSteps);   // the step's mean of its two edges (the classic's per-vertex ramp)
            const int a = static_cast<int>(alpha * static_cast<double>(cover));
            if (a <= 0) { continue; }
            px[static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)] = Rgba{134, 214, 74, static_cast<std::uint8_t>(a)};
        }
    }
}

}  // namespace foxsdr::slintscope
