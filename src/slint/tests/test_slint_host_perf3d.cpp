// THE FRAME TIME OF EVERY 3D VIEW (docs 6.64): each view's painter run over a worst-case synthetic scene at the size its well has in the 1440 x 900 window, best of several frames, PRINTED for
// the record and never asserted (a loaded machine would turn a speed check into a flake). The one thing asserted is that the painters work at those sizes at all: they draw something.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "radar3d_compositor.hpp"
#include "sonde3d_compositor.hpp"
#include "survey3d_compositor.hpp"
#include "test_check.hpp"
#include "tunnel3d_compositor.hpp"
#include "waterfall3d_compositor.hpp"
#include "waterfall3d_logic.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

using namespace foxsdr::slintview3d;
namespace s3 = foxsdr::scene3d;

namespace {

double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

double best(int frames, const std::function<void()>& f) {
    double b = 1e9;
    for (int i = 0; i < frames; ++i) { const double t0 = nowMs(); f(); b = std::min(b, nowMs() - t0); }
    return b;
}

std::size_t opaque(const std::vector<s3::Rgba>& px) { std::size_t n = 0; for (const auto& p : px) { n += p.a > 0 ? 1 : 0; } return n; }

}  // namespace

int main() {
    s3::Scene scene;

    {   // the receiver's waterfall surface: a well of 1100 x 520, the grid its level of detail asks for, every row real, a VFO and three markers
        const int w = 1100, h = 520;
        int rows = 0, cols = 0;
        gridSizeFor(w, h, rows, cols);
        std::vector<float> g(static_cast<std::size_t>(rows * cols));
        for (int r = 0; r < rows; ++r) { for (int c = 0; c < cols; ++c) { g[static_cast<std::size_t>(r * cols + c)] = -100.0f + 60.0f * std::exp(-std::pow((c - 40.0 - r * 0.3) / 3.0, 2.0)) + 2.0f * std::sin(0.7f * static_cast<float>(c * r)); } }
        Wf3dInput in;
        in.grid = g.data();
        in.rows = rows;
        in.cols = cols;
        in.centreHz = 145.5e6;
        in.spanHz = 2.4e6;
        in.tunedHz = 145.3e6;
        in.camera = waterfall3dDefaultCamera();
        in.markers = {{145.1e6, "M1"}, {145.7e6, "M2"}, {145.9e6, "M3"}};
        std::vector<s3::Rgba> px(static_cast<std::size_t>(w * h));
        Wf3dResult r;
        const double ms = best(15, [&] { r = paintWaterfall3d(scene, px.data(), w, h, in); });
        std::printf("3D frame time: waterfall surface %d x %d, grid %d x %d (%zu triangles): best %.2f ms\n", w, h, rows, cols, r.triangles, ms);
        CHECK(r.hasData && opaque(px) > 20000);
    }
    {   // the radar airspace: a square well of 560 x 560, fourteen aircraft with 256-point trails
        const int w = 560, h = 560;
        Radar3dInput in;
        in.rangeNm = 100.0;
        in.camera = radar3dDefaultCamera();
        for (int i = 0; i < 14; ++i) {
            Radar3dAircraft a;
            a.id = "A" + std::to_string(i);
            a.label = "AC" + std::to_string(100 + i);
            a.eastNm = -80.0 + 11.0 * i;
            a.northNm = 60.0 - 8.0 * i;
            a.altFt = 4000.0 + 2500.0 * i;
            a.courseDeg = 20.0 * i;
            for (int k = 0; k < 256; ++k) { a.trail.push_back({a.eastNm - 20.0 + 0.15 * k, a.northNm - 10.0 + 0.08 * k, a.altFt - 2000.0 + 8.0 * k}); }
            in.aircraft.push_back(a);
        }
        std::vector<s3::Rgba> px(static_cast<std::size_t>(w * h));
        Radar3dResult r;
        const double ms = best(15, [&] { r = paintRadar3d(scene, px.data(), w, h, in); });
        std::printf("3D frame time: radar airspace %d x %d, %zu aircraft, %zu trail segments: best %.2f ms\n", w, h, r.aircraft, r.trailSegments, ms);
        CHECK(r.aircraft == 14u && opaque(px) > 5000);
    }
    {   // the constellation tunnel: a well of 760 x 300, the longest depth the tap can hold (4800 samples)
        const int w = 760, h = 300;
        std::vector<float> i(4800), q(4800);
        for (std::size_t k = 0; k < i.size(); ++k) { i[k] = 0.7f * ((k / 24) % 2 ? 1.0f : -1.0f) + 0.05f * std::sin(0.31f * static_cast<float>(k)); q[k] = 0.7f * (((k / 48) % 2) ? 1.0f : -1.0f) + 0.05f * std::cos(0.17f * static_cast<float>(k)); }
        TunnelInput in;
        in.i = i.data();
        in.q = q.data();
        in.n = i.size();
        in.rateHz = 48000.0;
        in.camera = tunnelDefaultCamera();
        std::vector<s3::Rgba> px(static_cast<std::size_t>(w * h));
        TunnelResult r;
        const double ms = best(15, [&] { r = paintTunnel(scene, px.data(), w, h, in); });
        std::printf("3D frame time: constellation tunnel %d x %d, %zu points: best %.2f ms\n", w, h, r.points, ms);
        CHECK(r.points == 4800u && opaque(px) > 3000);
    }
    {   // the survey surface: a well of 1000 x 560, forty channels
        const int w = 1000, h = 560;
        SurveyInput in;
        in.camera = survey3dDefaultCamera();
        for (int k = 0; k < 40; ++k) {
            SurveyChannel c;
            c.freqHz = 144.0e6 + 0.1e6 * k;
            c.widthHz = 12500.0;
            c.peakDb = -40.0 - k;
            c.overDb = 30.0;
            c.seenPct = 90.0 - 2.0 * k;
            c.haveWidth = c.havePeak = c.haveOver = c.haveSeen = true;
            in.channels.push_back(c);
        }
        std::vector<s3::Rgba> px(static_cast<std::size_t>(w * h));
        SurveyResult r;
        const double ms = best(15, [&] { r = paintSurvey3d(scene, px.data(), w, h, in); });
        std::printf("3D frame time: survey surface %d x %d, %zu channels (%zu triangles): best %.2f ms\n", w, h, in.channels.size(), r.triangles, ms);
        CHECK(r.hasData && opaque(px) > 10000);
    }
    {   // the radiosonde track: a well of 1000 x 560, a four-thousand-sample flight with its burst and a 900-point ground trail
        const int w = 1000, h = 560;
        SondeInput in;
        in.camera = sonde3dDefaultCamera();
        for (int k = 0; k < 4000; ++k) {
            const double t = static_cast<double>(k) / 3999.0;
            SondeLocal s;
            s.eastKm = 90.0 * t;
            s.northKm = 20.0 * std::sin(3.0 * t);
            s.altKm = t < 0.7 ? 31.0 * t / 0.7 : 31.0 - 29.0 * (t - 0.7) / 0.3;
            in.track.push_back(s);
        }
        in.burst = true;
        in.burstIndex = 2800;
        for (int k = 0; k < 900; ++k) { in.ground.emplace_back(0.1 * k, 20.0 * std::sin(0.0333 * k)); }
        in.haveRx = true;
        in.tag = "S4720113  2100 m  -12 m/s";
        std::vector<s3::Rgba> px(static_cast<std::size_t>(w * h));
        SondeResult r;
        const double ms = best(15, [&] { r = paintSonde3d(scene, px.data(), w, h, in); });
        std::printf("3D frame time: radiosonde track %d x %d, %zu + %zu segments: best %.2f ms\n", w, h, r.ascentSegments, r.descentSegments, ms);
        CHECK(r.burst && opaque(px) > 3000);
    }
    return testSummary("test_slint_host_perf3d");
}
