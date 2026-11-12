// The persistence (phosphor) trace (src/slint/host_logic.cpp paintTrace with TraceMode::Persistence, lane S1 item 3): a density of the last seconds of the trace, brighter where the trace spent more of
// them, that decays. Pixel tests on made-up spectra (the way the 3D and pane measurements make theirs), the decay arithmetic, that the plain modes' pictures are not touched, the cost of one frame.
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

std::vector<float> flat(float db, std::size_t n = 800) { return std::vector<float>(n, db); }

int yOf(float db) { return static_cast<int>((1.0 - (db - kMin) / (kMax - kMin)) * (H - 1)); }

void frame(std::vector<Rgba>& px, const std::vector<float>& bins, TraceState& st, TraceMode mode = TraceMode::Persistence) {
    paintTrace(px.data(), W, H, bins.data(), bins.size(), kMin, kMax, st, mode, kLive, kHeld, kAvg);
}

std::uint8_t alphaAt(const std::vector<Rgba>& px, int x, int y) { return px[static_cast<std::size_t>(y) * W + static_cast<std::size_t>(x)].a; }

void steady() {
    std::vector<Rgba> px(W * H);
    TraceState st;
    st.decay = 0.9f;
    const auto bins = flat(-40.0f);
    for (int i = 0; i < 150; ++i) { frame(px, bins, st); }
    const int y = yOf(-40.0f);
    CHECK(alphaAt(px, 100, y) == 255);                 // a cell the trace visits on every frame settles fully lit
    CHECK(alphaAt(px, 100, y - 10) == 0 && alphaAt(px, 100, y + 10) == 0);   // and the rest of the column is clear
    CHECK(alphaAt(px, 0, y) == 255 && alphaAt(px, W - 1, y) == 255);
    // a lit cell is the strength ramp's colour, not the plain trace's
    const Rgba c = px[static_cast<std::size_t>(y) * W + 100];
    CHECK(!(c.r == kLive.r && c.g == kLive.g && c.b == kLive.b));
}

void decay() {
    std::vector<Rgba> px(W * H);
    TraceState st;
    st.decay = 0.8f;
    for (int i = 0; i < 80; ++i) { frame(px, flat(-30.0f), st); }
    const int oldY = yOf(-30.0f);
    CHECK(alphaAt(px, 50, oldY) == 255);
    // the trace moves: the old line fades frame by frame, the new one comes up
    std::uint8_t prev = 255;
    bool monotone = true;
    int framesToClear = -1;
    for (int i = 1; i <= 60; ++i) {
        frame(px, flat(-70.0f), st);
        const std::uint8_t a = alphaAt(px, 50, oldY);
        if (a > prev) { monotone = false; }
        prev = a;
        if (a == 0 && framesToClear < 0) { framesToClear = i; }
    }
    CHECK(monotone);
    CHECK(framesToClear > 3 && framesToClear < 40);                        // gone in a few tens of frames at 0.8, not at once and not never
    CHECK(alphaAt(px, 50, yOf(-70.0f)) == 255);
    // the decay arithmetic: the density halves every ln(0.5) / ln(decay) frames (0.8: 3.1 frames)
    TraceState d;
    d.decay = 0.8f;
    std::vector<Rgba> p2(W * H);
    frame(p2, flat(-30.0f), d);
    const float first = d.density[static_cast<std::size_t>(oldY) * W + 50];
    for (int i = 0; i < 3; ++i) { frame(p2, flat(-70.0f), d); }
    CHECK(std::fabs(d.density[static_cast<std::size_t>(oldY) * W + 50] - first * 0.8f * 0.8f * 0.8f) < 1e-4f);
}

void brightness() {
    // a trace that alternates between two levels spends half the frames at each: both are lit, each dimmer than a steady trace
    std::vector<Rgba> px(W * H);
    TraceState st;
    st.decay = 0.9f;
    for (int i = 0; i < 200; ++i) { frame(px, flat(i % 2 == 0 ? -20.0f : -60.0f), st); }
    const std::uint8_t a = alphaAt(px, 100, yOf(-20.0f)), b = alphaAt(px, 100, yOf(-60.0f));
    CHECK(a > 60 && a < 255 && b > 60 && b < 255);
    // a spike seen on one frame in ten is fainter than one seen on every frame
    std::vector<Rgba> p2(W * H);
    TraceState s2;
    s2.decay = 0.9f;
    for (int i = 0; i < 300; ++i) {
        auto bins = flat(-90.0f);
        if (i % 10 == 0) { for (std::size_t k = 400; k < 420; ++k) { bins[k] = -10.0f; } }
        frame(p2, bins, s2);
    }
    const std::uint8_t rare = alphaAt(p2, 100, yOf(-10.0f) + 1);
    CHECK(rare < alphaAt(p2, 20, yOf(-90.0f)) );   // the floor (every frame) is brighter than the rare spike's top
}

void joinsColumns() {
    // a steep edge between two columns lights the cells between them (the trace is a line, not dots)
    std::vector<Rgba> px(W * H);
    TraceState st;
    st.decay = 0.9f;
    std::vector<float> bins(800, -90.0f);
    for (std::size_t k = 400; k < 800; ++k) { bins[k] = -10.0f; }
    for (int i = 0; i < 100; ++i) { frame(px, bins, st); }
    int lit = 0;
    for (std::uint32_t y = 0; y < H; ++y) { if (alphaAt(px, 100, static_cast<int>(y)) > 0) { ++lit; } }
    CHECK(lit > 50);   // column 100 is the step: the whole run of cells between the two levels
}

void modes() {
    // leaving persistence forgets the density; the plain modes do not touch it and draw as before
    std::vector<Rgba> px(W * H);
    TraceState st;
    st.decay = 0.9f;
    for (int i = 0; i < 10; ++i) { frame(px, flat(-40.0f), st); }
    CHECK(!st.density.empty());
    frame(px, flat(-40.0f), st, TraceMode::Normal);
    CHECK(st.density.empty());
    CHECK(px[static_cast<std::size_t>(yOf(-40.0f)) * W + 100].r == kLive.r && px[static_cast<std::size_t>(yOf(-40.0f)) * W + 100].a == 255);   // the live line's colour, as ever
    // back into persistence starts from nothing: one frame is dim (1 hit of 1 / (1 - decay) = 10)
    frame(px, flat(-40.0f), st);
    CHECK(alphaAt(px, 100, yOf(-40.0f)) < 255 && alphaAt(px, 100, yOf(-40.0f)) > 0);
    resetTrace(st);
    CHECK(st.density.empty());
    // no bins: a clear picture, no crash
    std::vector<Rgba> blank(W * H);
    paintTrace(blank.data(), W, H, nullptr, 0, kMin, kMax, st, TraceMode::Persistence, kLive, kHeld, kAvg);
    bool clear = true;
    for (const Rgba& c : blank) { if (c.a != 0) { clear = false; } }
    CHECK(clear);
    // the picture size changes (a pane refit): the density follows
    std::vector<Rgba> big(400 * 150);
    paintTrace(big.data(), 400, 150, flat(-40.0f).data(), 800, kMin, kMax, st, TraceMode::Persistence, kLive, kHeld, kAvg);
    CHECK(st.density.size() == 400u * 150u);
}

void cost() {
    constexpr std::uint32_t w = 1024, h = 340;   // the picture size of a pane
    std::vector<Rgba> px(static_cast<std::size_t>(w) * h);
    TraceState st;
    st.decay = 0.95f;
    std::vector<float> bins(4096);
    for (std::size_t i = 0; i < bins.size(); ++i) { bins[i] = -90.0f + 40.0f * std::sin(static_cast<float>(i) * 0.01f); }
    int frames = 400;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) { bins[static_cast<std::size_t>(i) % bins.size()] = -10.0f; paintTrace(px.data(), w, h, bins.data(), bins.size(), kMin, kMax, st, TraceMode::Persistence, kLive, kHeld, kAvg); }
    const double persist = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / frames;
    TraceState plain;
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) { paintTrace(px.data(), w, h, bins.data(), bins.size(), kMin, kMax, plain, TraceMode::Normal, kLive, kHeld, kAvg); }
    const double normal = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count() / frames;
    std::printf("persistence trace: %.3f ms a frame (1024 x 340), the plain trace %.3f ms\n", persist, normal);
    CHECK(persist < 4.0);   // a frame is 33 ms and four panes paint four of these: 4 ms each is the bound
}

}  // namespace

int main() {
    steady();
    decay();
    brightness();
    joinsColumns();
    modes();
    cost();
    return testSummary("test_slint_host_persistence");
}
