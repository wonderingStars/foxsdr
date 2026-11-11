// The trace's added modes of screen lane B (src/slint/host_logic.cpp paintTrace, docs/multi-radio-records/screen-b.md): MIN HOLD (S5) and the SHAPES, filled and bars (S6). Pixel tests on made-up spectra;
// the plain modes' pictures must not move (the persistence, peak hold and average tests stay as they are).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "host_logic.hpp"
#include "test_check.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace foxsdr::slinthost;

namespace {

constexpr std::uint32_t W = 200, H = 100;
constexpr double kMin = -100, kMax = 0;
const Rgba kLive{0x7a, 0xd7, 0xff, 255}, kHeld{0xff, 0x9f, 0x43, 255}, kAvg{0xf1, 0xea, 0xd6, 255};

int yOf(float db) { return static_cast<int>((1.0 - (db - kMin) / (kMax - kMin)) * (H - 1)); }
Rgba at(const std::vector<Rgba>& px, int x, int y) { return px[static_cast<std::size_t>(y) * W + static_cast<std::size_t>(x)]; }
void frame(std::vector<Rgba>& px, const std::vector<float>& bins, TraceState& st, TraceMode mode) {
    paintTrace(px.data(), W, H, bins.data(), bins.size(), kMin, kMax, st, mode, kLive, kHeld, kAvg);
}
bool isAvg(Rgba c) { return c.r == kAvg.r && c.g == kAvg.g && c.b == kAvg.b && c.a == 255; }
bool isLive(Rgba c) { return c.r == kLive.r && c.g == kLive.g && c.b == kLive.b && c.a == 255; }

void minHold() {
    std::vector<Rgba> px(W * H);
    TraceState st;
    // a noise floor at -60 with one frame where it dips to -80 and a burst to -20: min hold keeps the dip, never the burst
    frame(px, std::vector<float>(800, -60.0f), st, TraceMode::MinHold);
    frame(px, std::vector<float>(800, -80.0f), st, TraceMode::MinHold);
    frame(px, std::vector<float>(800, -20.0f), st, TraceMode::MinHold);
    frame(px, std::vector<float>(800, -60.0f), st, TraceMode::MinHold);
    CHECK(st.minHold.size() == W);
    for (const float v : st.minHold) { CHECK(v == -80.0f); }
    bool heldDrawn = false, liveDrawn = false, burstDrawn = false;
    for (int x = 0; x < static_cast<int>(W); ++x) {
        for (int y = 0; y < static_cast<int>(H); ++y) {
            if (isAvg(at(px, x, y)) && std::abs(y - yOf(-80.0f)) <= 1) { heldDrawn = true; }
            if (isLive(at(px, x, y)) && y == yOf(-60.0f)) { liveDrawn = true; }
            if (std::abs(y - yOf(-20.0f)) <= 1 && at(px, x, y).a != 0) { burstDrawn = true; }
        }
    }
    CHECK(heldDrawn && liveDrawn && !burstDrawn);
    // a lower level lowers it, a higher one does not raise it
    frame(px, std::vector<float>(800, -95.0f), st, TraceMode::MinHold);
    for (const float v : st.minHold) { CHECK(v == -95.0f); }
    frame(px, std::vector<float>(800, -10.0f), st, TraceMode::MinHold);
    for (const float v : st.minHold) { CHECK(v == -95.0f); }
    // leaving the mode forgets the lines, and the other modes' state is not touched by it
    frame(px, std::vector<float>(800, -60.0f), st, TraceMode::Normal);
    CHECK(st.minHold.empty() && st.peak.empty());
    frame(px, std::vector<float>(800, -60.0f), st, TraceMode::PeakHold);
    CHECK(st.minHold.empty() && st.peak.size() == W);
    frame(px, std::vector<float>(800, -60.0f), st, TraceMode::MinHold);
    CHECK(st.minHold.size() == W && st.peak.empty());
    // a reset
    resetTrace(st);
    CHECK(st.minHold.empty());
    // the width changes (a refit): the held line starts again
    TraceState wide;
    std::vector<Rgba> big(400 * H);
    paintTrace(px.data(), W, H, std::vector<float>(800, -50.0f).data(), 800, kMin, kMax, wide, TraceMode::MinHold, kLive, kHeld, kAvg);
    paintTrace(big.data(), 400, H, std::vector<float>(800, -70.0f).data(), 800, kMin, kMax, wide, TraceMode::MinHold, kLive, kHeld, kAvg);
    CHECK(wide.minHold.size() == 400 && wide.minHold[0] == -70.0f);
}

void shapes() {
    std::vector<Rgba> px(W * H);
    TraceState st;
    std::vector<float> bins(800);
    for (std::size_t i = 0; i < bins.size(); ++i) { bins[i] = -70.0f + 40.0f * std::sin(static_cast<float>(i) * 0.02f); }   // a slow wave from -110 to -30
    // LINE: only the line is drawn, nothing under it
    st.shape = 0;
    frame(px, bins, st, TraceMode::Normal);
    const int x = 100;
    float col = -300.0f;
    for (std::size_t i = 4 * 100; i < 4 * 101; ++i) { col = std::max(col, bins[i]); }
    const int y = yOf(col);
    CHECK(at(px, x, y).a == 255 && at(px, x, y + 10).a == 0);
    // FILL: the area under the trace in the trace's colour at a low alpha, the line on top
    st.shape = 1;
    frame(px, bins, st, TraceMode::Normal);
    CHECK(isLive(at(px, x, y)));
    CHECK(at(px, x, y + 10).a == 72 && at(px, x, y + 10).r == kLive.r);
    CHECK(at(px, x, H - 1).a == 72);
    CHECK(at(px, x, y - 3).a == 0);   // above the trace: clear
    std::size_t filled = 0;
    for (const Rgba& p : px) { filled += p.a != 0 ? 1 : 0; }
    CHECK(filled > W * H / 4);
    // BARS: groups of 4 columns, 3 drawn and 1 gap, each as high as its strongest column
    st.shape = 2;
    frame(px, bins, st, TraceMode::Normal);
    for (int g = 5; g < 40; ++g) {
        const int x0 = g * 4;
        float best = -300.0f;
        for (int k = 0; k < 4; ++k) { for (std::size_t i = static_cast<std::size_t>((x0 + k) * 4); i < static_cast<std::size_t>((x0 + k + 1) * 4); ++i) { best = std::max(best, bins[i]); } }
        if (best < -95.0f || best > -5.0f) { continue; }   // (a bar at the picture's edge has no room for the body to be read)
        const int yb = yOf(best);
        CHECK(at(px, x0, yb).a == 255 && at(px, x0 + 2, yb).a == 255);    // the top of the bar
        CHECK(at(px, x0 + 1, std::min<int>(H - 1, yb + 8)).a == 150);     // its body
        CHECK(at(px, x0 + 3, std::min<int>(H - 1, yb + 8)).a == 0);       // the gap
    }
    // the held lines work with every shape (peak hold over a filled trace)
    st.shape = 1;
    resetTrace(st);
    frame(px, std::vector<float>(800, -40.0f), st, TraceMode::PeakHold);
    frame(px, std::vector<float>(800, -80.0f), st, TraceMode::PeakHold);
    CHECK(at(px, 50, yOf(-40.0f)).r == kHeld.r && at(px, 50, yOf(-40.0f)).g == kHeld.g);
    CHECK(at(px, 50, yOf(-80.0f) + 5).a == 72);
    // the persistence trace ignores the shape (its own picture)
    st.shape = 2;
    TraceState p;
    p.shape = 2;
    p.decay = 0.9f;
    for (int i = 0; i < 100; ++i) { frame(px, std::vector<float>(800, -40.0f), p, TraceMode::Persistence); }
    CHECK(at(px, 50, yOf(-40.0f)).a == 255 && at(px, 50, yOf(-40.0f) + 10).a == 0);
}

void cost() {
    std::vector<Rgba> px(1024 * 340);
    TraceState line, fill, bars;
    fill.shape = 1;
    bars.shape = 2;
    std::vector<float> bins(4096);
    for (std::size_t i = 0; i < bins.size(); ++i) { bins[i] = -80.0f + 50.0f * std::sin(static_cast<float>(i) * 0.013f); }
    const auto run = [&](TraceState& s) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 200; ++i) { paintTrace(px.data(), 1024, 340, bins.data(), bins.size(), kMin, kMax, s, TraceMode::MinHold, kLive, kHeld, kAvg); }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 200.0;
    };
    const double l = run(line), f = run(fill), b = run(bars);
    std::printf("trace (1024 x 340, min hold): line %.3f ms, filled %.3f ms, bars %.3f ms a frame\n", l, f, b);
    CHECK(f < 4.0 && b < 4.0 && l < 4.0);
}

}  // namespace

int main() {
    minHold();
    shapes();
    cost();
    return testSummary("test_slint_host_trace_modes");
}
