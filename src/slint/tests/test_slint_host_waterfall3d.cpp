// The 3D WATERFALL SURFACE (src/slint/waterfall3d_logic.hpp and waterfall3d_compositor.hpp, docs 6.64) against synthetic frames: the history's downsampling, windowing, gaps and
// resampling to the grid, the level of detail, the number formats, and the picture and the labels painted for a known scene - the ridge is where the carrier is, the VFO line is amber and
// only when it is asked for and inside the window, the markers' lines are dashed, an empty history draws the box and no surface, the camera moves the picture. No Slint, no engine, no window.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "test_check.hpp"
#include "waterfall3d_compositor.hpp"
#include "waterfall3d_logic.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <cstring>
#include <string>
#include <vector>

using namespace foxsdr::slintview3d;
namespace s3 = foxsdr::scene3d;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

std::vector<float> bins(std::size_t n, float floorDb, std::size_t spike, float spikeDb) {
    std::vector<float> v(n, floorDb);
    if (spike < n) { v[spike] = spikeDb; }
    return v;
}

// pixels near a colour
std::size_t countNear(const std::vector<s3::Rgba>& px, s3::Rgba c, int tol) {
    std::size_t n = 0;
    for (const s3::Rgba& p : px) {
        if (p.a > 200 && std::abs(int(p.r) - c.r) <= tol && std::abs(int(p.g) - c.g) <= tol && std::abs(int(p.b) - c.b) <= tol) { ++n; }
    }
    return n;
}
// warm pixels: red high and blue low - the heat ramp's amber and the VFO's amber; the one-hue blue shading has none
std::size_t countWarm(const std::vector<s3::Rgba>& px) {
    std::size_t n = 0;
    for (const s3::Rgba& p : px) { n += (p.a > 200 && p.r > 170 && p.b < 110 && p.r > p.b + 80) ? 1 : 0; }
    return n;
}
std::size_t opaque(const std::vector<s3::Rgba>& px) { std::size_t n = 0; for (const auto& p : px) { n += p.a > 0 ? 1 : 0; } return n; }

// a grid with a ridge: column `ridge` is loud in every row, the rest at the floor
std::vector<float> ridgeGrid(int rows, int cols, int ridge, float loud, float quiet, int holesFrom = -1) {
    std::vector<float> g(static_cast<std::size_t>(rows * cols), quiet);
    for (int r = 0; r < rows; ++r) {
        g[static_cast<std::size_t>(r * cols + ridge)] = loud;
        if (ridge > 0) { g[static_cast<std::size_t>(r * cols + ridge - 1)] = (loud + quiet) / 2.0f; }
        if (holesFrom >= 0 && r >= holesFrom) { for (int c = 0; c < cols; ++c) { g[static_cast<std::size_t>(r * cols + c)] = kNaN; } }
    }
    return g;
}

Wf3dInput inputFor(const std::vector<float>& g, int rows, int cols) {
    Wf3dInput in;
    in.grid = g.data();
    in.rows = rows;
    in.cols = cols;
    in.centreHz = 145.5e6;
    in.spanHz = 2.4e6;
    in.tunedHz = 145.5e6;
    in.dbMin = -110.0;
    in.dbMax = 0.0;
    in.depthS = 60.0;
    in.camera = waterfall3dDefaultCamera();
    return in;
}

// ---- the history ----
void testHistory() {
    // 25 frames a second for 10 s, each with a spike at bin 1000 of 4096
    WaterfallHistory h;
    const std::vector<float> b = bins(4096, -100.0f, 1000, -20.0f);
    std::int64_t t = 0;
    for (int i = 0; i < 250; ++i) { h.push(t, b.data(), b.size(), 7); t += 40; }
    CHECK(h.size() == 250u);
    const std::int64_t now = t;   // 10 000 ms
    CHECK_NEAR(h.spanSeconds(now), 10.0, 0.05);
    std::vector<float> g;
    const std::size_t have = h.grid(now, 30.0, 31, 192, g);
    CHECK(g.size() == 31u * 192u);
    CHECK(have >= 10 && have <= 12);   // ages 0..10 s have data: the rest of the 30 s depth is a hole
    CHECK(std::isfinite(g[0]));
    CHECK(std::isnan(g[static_cast<std::size_t>(20 * 192 + 5)]));
    CHECK(std::isnan(g[static_cast<std::size_t>(30 * 192)]));
    // the spike is in stored column 46 (1000 * 192 / 4096) and survives the downsampling as the column's maximum; the quiet columns stay at the floor
    CHECK_NEAR(g[46], -20.0, 1e-4);
    CHECK_NEAR(g[10], -100.0, 1e-4);
    // a coarser grid keeps it: 96 columns take the maximum of two stored columns each
    std::vector<float> g2;
    h.grid(now, 30.0, 31, 96, g2);
    CHECK_NEAR(g2[23], -20.0, 1e-4);
    CHECK_NEAR(g2[24], -100.0, 1e-4);
    // a wider depth fills a thinner share of the rows
    std::vector<float> g3;
    const std::size_t have120 = h.grid(now, 120.0, 61, 96, g3);
    CHECK(have120 >= 5 && have120 <= 7);   // 10 s of 120 s over 61 rows: rows of age 0, 2, ... 10
    // row 0 is the newest even when the grid is asked for at a later time (a stalled receiver): the newest row is then stale and the grid empty
    std::vector<float> g4;
    CHECK(h.grid(now + 60000, 30.0, 31, 96, g4) == 0u);
    CHECK(std::isnan(g4[0]));
    // a gap in the reception is a hole, not the old picture stretched over it
    WaterfallHistory gap;
    for (int i = 0; i < 25; ++i) { gap.push(i * 40, b.data(), b.size(), 1); }        // 0..960 ms
    for (int i = 0; i < 25; ++i) { gap.push(20000 + i * 40, b.data(), b.size(), 1); }  // 20 000..20 960 ms
    std::vector<float> g5;
    gap.grid(21000, 30.0, 31, 96, g5);
    CHECK(std::isfinite(g5[0]));                                           // age 0 (21 000 ms): the newest row
    CHECK(std::isnan(g5[static_cast<std::size_t>(10 * 96)]));              // age 10 s (11 000 ms): inside the gap
    CHECK(std::isfinite(g5[static_cast<std::size_t>(20 * 96)]));           // age 20 s (1 000 ms): the first burst
    // a new window starts the history again
    h.push(now + 40, b.data(), b.size(), 8);
    CHECK(h.size() == 1u);
    h.clear();
    CHECK(h.size() == 0u);
    CHECK(h.spanSeconds(now) == 0.0);
    std::vector<float> g6;
    CHECK(h.grid(now, 30.0, 31, 96, g6) == 0u);
    // old rows are trimmed
    WaterfallHistory long_;
    for (int i = 0; i < 400; ++i) { long_.push(i * 1000, b.data(), 64, 1); }   // 400 s at 1 Hz
    CHECK(long_.size() <= 127u);
    CHECK(long_.size() >= 120u);
    // the frame rate cap holds
    WaterfallHistory fast;
    for (int i = 0; i < 6000; ++i) { fast.push(i, b.data(), 64, 1); }          // 1 kHz for 6 s
    CHECK(fast.size() <= kWf3dMaxRows);
    // empty and null input is ignored
    WaterfallHistory e;
    e.push(0, nullptr, 10, 1);
    e.push(0, b.data(), 0, 1);
    CHECK(e.size() == 0u);
    // a clock that goes backwards starts again
    WaterfallHistory back;
    back.push(5000, b.data(), 64, 1);
    back.push(1000, b.data(), 64, 1);
    CHECK(back.size() == 1u);
    // non-finite levels become the floor, not a NaN hole
    std::vector<float> weird(256, kNaN);
    WaterfallHistory w;
    w.push(0, weird.data(), weird.size(), 1);
    std::vector<float> gw;
    w.grid(0, 30.0, 4, 8, gw);
    CHECK(std::isfinite(gw[0]) && gw[0] < -250.0f);
}

void testGridSizeAndFormats() {
    int rows = 0, cols = 0;
    gridSizeFor(1100, 520, rows, cols);
    CHECK(cols == 137 && rows == 86);
    gridSizeFor(10, 10, rows, cols);
    CHECK(cols == 48 && rows == 24);
    gridSizeFor(5000, 5000, rows, cols);
    CHECK(cols == kWf3dStoreCols && rows == 96);
    CHECK(formatMHz(145.5e6, 600000.0) == "145.50");
    CHECK(formatMHz(145.5e6, 25000.0) == "145.500");
    CHECK(formatMHz(145.5e6, 500.0) == "145.5000");
    CHECK(formatMHz(145.5e6, 5.0) == "145.500000");
    CHECK(formatMHz(145.5e6, 5.0e6) == "145.5");
    CHECK(formatMHz(145.2123e6, 1000.0) == "145.212");
    CHECK(formatAge(15.0, 15.0) == "15");
    CHECK(formatAge(7.5, 7.5 / 3.0) == "7.5");
    CHECK(formatDb(-110.0) == "-110");
    CHECK(formatDb(-82.5) == "-82" || formatDb(-82.5) == "-83");
}

// ---- the picture ----
void testPaint() {
    const int rows = 40, cols = 60;
    const std::vector<float> g = ridgeGrid(rows, cols, 30, -20.0f, -100.0f);
    s3::Scene scene;
    std::vector<s3::Rgba> px(420 * 300);

    // the surface is drawn, with its words
    Wf3dInput in = inputFor(g, rows, cols);
    in.shading = 1;
    Wf3dResult r = paintWaterfall3d(scene, px.data(), 420, 300, in);
    CHECK(r.hasData);
    const std::size_t allTris = static_cast<std::size_t>((rows - 1) * (cols - 1) * 2);
    CHECK(r.triangles >= allTris * 95 / 100 && r.triangles <= allTris);   // (an edge-on sliver may be dropped)
    CHECK(opaque(px) > 8000);
    // 5 + 5 + 5 ticks, 3 captions, the VFO's frequency
    int ticks = 0, captions = 0, notes = 0;
    for (const s3::Label3D& l : r.labels) { if (l.kind == 0) { ++ticks; } else if (l.kind == 1) { ++captions; } else { ++notes; } }
    CHECK(ticks == 15);
    CHECK(captions == 3);
    CHECK(notes == 1);
    for (const s3::Label3D& l : r.labels) { CHECK(l.x >= -0.02f && l.x <= 1.02f && l.y >= -0.02f && l.y <= 1.02f); }
    // the tick words: the window's edges and middle in MHz (step 0.6 MHz: two decimals)
    bool low = false, high = false, mid = false, minDb = false, maxDb = false, age0 = false, age60 = false;
    for (const s3::Label3D& l : r.labels) {
        low = low || l.text == "144.30";
        high = high || l.text == "146.70";
        mid = mid || l.text == "145.50";
        minDb = minDb || l.text == "-110";
        maxDb = maxDb || l.text == "0";
        age0 = age0 || l.text == "0";
        age60 = age60 || l.text == "60";
    }
    CHECK(low && high && mid && minDb && maxDb && age0 && age60);
    bool vfoLabel = false;
    for (const s3::Label3D& l : r.labels) { vfoLabel = vfoLabel || (l.kind == 2 && l.text == "145.500"); }
    CHECK(vfoLabel);

    // the colour ramp has the strength key's amber and white at the ridge's top; the one-hue shading does not
    std::vector<s3::Rgba> px1(420 * 300);
    Wf3dInput ramp = in;
    ramp.showVfo = false;
    paintWaterfall3d(scene, px1.data(), 420, 300, ramp);
    CHECK(countWarm(px1) > 20);
    std::vector<s3::Rgba> px2(420 * 300);
    Wf3dInput hs = in;
    hs.shading = 0;
    hs.showVfo = false;
    Wf3dResult r2 = paintWaterfall3d(scene, px2.data(), 420, 300, hs);
    CHECK(r2.hasData);
    CHECK(countWarm(px2) == 0u);

    // the VFO line is amber: with the ramp off the surface adds none, the line does; off, or outside the window, it goes
    Wf3dInput vfoOn = hs;
    vfoOn.showVfo = true;
    std::vector<s3::Rgba> px3(420 * 300);
    paintWaterfall3d(scene, px3.data(), 420, 300, vfoOn);
    CHECK(countWarm(px3) > 40);
    Wf3dInput vfoOut = vfoOn;
    vfoOut.tunedHz = 150.0e6;
    std::vector<s3::Rgba> px4(420 * 300);
    Wf3dResult ro = paintWaterfall3d(scene, px4.data(), 420, 300, vfoOut);
    CHECK(countWarm(px4) == 0u);
    int notesOut = 0;
    for (const s3::Label3D& l : ro.labels) { notesOut += l.kind == 2 ? 1 : 0; }
    CHECK(notesOut == 0);

    // a user marker: a dashed ivory line and its tab at the far end
    Wf3dInput mk = hs;
    mk.showVfo = false;
    mk.markers.push_back({145.212e6, "M1 145.212 MHz"});
    std::vector<s3::Rgba> px5(420 * 300);
    Wf3dResult rm = paintWaterfall3d(scene, px5.data(), 420, 300, mk);
    CHECK(countNear(px5, s3::Rgba{241, 234, 214, 255}, 20) > 20);
    bool tab = false;
    for (const s3::Label3D& l : rm.labels) { tab = tab || (l.kind == 2 && l.text == "M1 145.212 MHz"); }
    CHECK(tab);
    mk.markers[0].hz = 160.0e6;   // outside the window: no line and no tab
    std::vector<s3::Rgba> px6(420 * 300);
    Wf3dResult rn = paintWaterfall3d(scene, px6.data(), 420, 300, mk);
    CHECK(countNear(px6, s3::Rgba{241, 234, 214, 255}, 20) == 0u);
    for (const s3::Label3D& l : rn.labels) { CHECK(l.text != "M1 145.212 MHz"); }

    // determinism and the camera
    std::vector<s3::Rgba> a(420 * 300), b(420 * 300), c(420 * 300);
    paintWaterfall3d(scene, a.data(), 420, 300, in);
    paintWaterfall3d(scene, b.data(), 420, 300, in);
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(s3::Rgba)) == 0);
    Wf3dInput turned = in;
    turned.camera.azimuthDeg = 30.0f;
    turned.camera.elevationDeg = 60.0f;
    Wf3dResult rt = paintWaterfall3d(scene, c.data(), 420, 300, turned);
    CHECK(std::memcmp(a.data(), c.data(), a.size() * sizeof(s3::Rgba)) != 0);
    CHECK(rt.hasData);
    // the same data, a bigger zoom: more of the picture is covered
    Wf3dInput close = in;
    close.camera.zoom = 1.6f;
    std::vector<s3::Rgba> d(420 * 300);
    paintWaterfall3d(scene, d.data(), 420, 300, close);
    CHECK(opaque(d) != opaque(a));

    // an empty history: the box and its words, no surface, no VFO tab
    const std::vector<float> none(static_cast<std::size_t>(rows * cols), kNaN);
    Wf3dInput empty = inputFor(none, rows, cols);
    std::vector<s3::Rgba> e(420 * 300);
    Wf3dResult re = paintWaterfall3d(scene, e.data(), 420, 300, empty);
    CHECK(!re.hasData);
    CHECK(re.triangles == 0u);
    CHECK(opaque(e) > 500);
    int notesE = 0, ticksE = 0;
    for (const s3::Label3D& l : re.labels) { notesE += l.kind == 2 ? 1 : 0; ticksE += l.kind == 0 ? 1 : 0; }
    CHECK(notesE == 0 && ticksE == 15);
    // a grid with only its newest rows real: the holes are not meshed
    const std::vector<float> partial = ridgeGrid(rows, cols, 30, -20.0f, -100.0f, 10);
    Wf3dInput part = inputFor(partial, rows, cols);
    std::vector<s3::Rgba> pp(420 * 300);
    Wf3dResult rp = paintWaterfall3d(scene, pp.data(), 420, 300, part);
    CHECK(rp.hasData);
    CHECK(rp.triangles >= static_cast<std::size_t>(9 * (cols - 1) * 2) * 95 / 100 && rp.triangles <= static_cast<std::size_t>(9 * (cols - 1) * 2));
    // no data pointer, no window: survived
    Wf3dInput nul = in;
    nul.grid = nullptr;
    std::vector<s3::Rgba> nn(420 * 300);
    CHECK(!paintWaterfall3d(scene, nn.data(), 420, 300, nul).hasData);
    Wf3dInput nospan = in;
    nospan.spanHz = 0.0;
    const Wf3dResult rs = paintWaterfall3d(scene, nn.data(), 420, 300, nospan);
    CHECK(rs.labels.empty());
    // the ramps the key shows
    CHECK(&waterfall3dRamp(1) != &waterfall3dRamp(0));
    CHECK(waterfall3dRamp(1).at(1.0f).r == 255);
    CHECK(waterfall3dRamp(0).at(1.0f).b > 200);
}

}  // namespace

int main() {
    testHistory();
    testGridSizeAndFormats();
    testPaint();
    return testSummary("test_slint_host_waterfall3d");
}
