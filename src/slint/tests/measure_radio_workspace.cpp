// Screen stage S3's frame-time check of the REAL workspace (docs/multi-radio-records/screen-s3.md): stage S0 measured four bare RadioPanes (tests/measure_radio_panes.cpp); this
// repeats its software-renderer, synchronous measurement with the real RadioWorkspace component (ui/specimens/radio_workspace_s3.slint) and the pictures fitted to the pane by
// workspace_logic's Workspace::fitPictures, so the number can be set beside S0's (4.7 and 4.9 ms with today's picture size, 3.9 and 4.2 ms fitted).
//
//   measure_radio_workspace --width 1440 --height 900 [--frames 600] [--warmup 120] [--panes 4] [--png file]
//
// One frame = the host's picture work for every pane (the made-up frame copied, paintTrace, scrollWaterfallRow, the waterfall copy, the ticks, two new slint::Image values, the row of the
// WorkspaceModel set) and then ONE render() of the whole window on the harness's software platform (a full repaint, 3.9 MB frame allocated and cleared inside the call). It is not the live
// host: main.cpp's paneTick reads (engine, lists) and the publisher's signature compares are not repeated, and the chrome around the workspace is not drawn. Run only on the hidden desktop.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef SLINT_FEATURE_EXPERIMENTAL
#define SLINT_FEATURE_EXPERIMENTAL 1
#endif
#include "radio_view.hpp"
#include "workspace_logic.hpp"

#include "radio_workspace_s3.h"
#include "render_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <vector>

using namespace foxsdr::slinthost;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }
slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }

const char* argOf(int argc, char** argv, const char* name, const char* dflt) {
    for (int i = 1; i + 1 < argc; ++i) { if (std::strcmp(argv[i], name) == 0) { return argv[i + 1]; } }
    return dflt;
}

WorkspaceCell makeCell(int radio) {
    static const char* devices[4] = {"RTL-SDR #0", "SDRplay RSPduo", "HackRF One", "Airspy Mini"};
    static const char* freqs[4] = {"145.500000 MHz", "98.800000 MHz", "7.074000 MHz", "162.550000 MHz"};
    WorkspaceCell c{};
    c.occupied = true;
    c.label = ss("Radio " + std::to_string(radio));
    RadioPaneData& d = c.pane;
    d.radio = radio;
    const std::string n = std::to_string(radio);
    d.prefix = ss("Radio " + n); d.select_label = ss("Select Radio " + n); d.stop_label = ss("Stop Radio " + n);
    d.spectrum_label = ss("Radio " + n + " spectrum"); d.waterfall_label = ss("Radio " + n + " waterfall"); d.resize_label = ss("Resize Radio " + n);
    d.device = ss(devices[(radio - 1) % 4]); d.frequency = ss(freqs[(radio - 1) % 4]); d.mode = ss("NFM");
    d.running = true; d.decoding = radio == 3; d.muted = false; d.failed = false; d.bias_tee = false;
    d.gain = ss("32 dB"); d.squelch = ss("-60 dB"); d.volume = ss("70%"); d.rate = ss("2.400 MS/s");
    d.decoder = ss(radio == 3 ? "FT8" : ""); d.peak = ss("-41.2 dBFS"); d.span = ss("SPAN 2.400 MHz");
    d.trace_mode = 0; d.axis_unit = ss("MHz");
    d.freq_ticks = std::make_shared<slint::VectorModel<AxisTick>>(std::vector<AxisTick>{{0.1f, ss("145.0")}, {0.3f, ss("145.2")}, {0.5f, ss("145.5")}, {0.7f, ss("145.8")}, {0.9f, ss("146.0")}});
    d.db_ticks = std::make_shared<slint::VectorModel<DbTick>>(std::vector<DbTick>{{0.2f, ss("-20")}, {0.4f, ss("-40")}, {0.6f, ss("-60")}, {0.8f, ss("-80")}});
    d.age_ticks = std::make_shared<slint::VectorModel<AgeTick>>(std::vector<AgeTick>{{0.1f, ss("now")}, {0.5f, ss("10 s")}, {0.9f, ss("20 s")}});
    d.bands = std::make_shared<slint::VectorModel<BandSegment>>(std::vector<BandSegment>{{0.1f, 0.4f, ss("2 m"), slint::Color::from_argb_uint8(0, 0, 0, 0)}});
    d.bookmarks = std::make_shared<slint::VectorModel<BookmarkMark>>(std::vector<BookmarkMark>{});
    d.marks = std::make_shared<slint::VectorModel<PluginMark>>(std::vector<PluginMark>{});
    d.wf_marks = std::make_shared<slint::VectorModel<PluginMark>>(std::vector<PluginMark>{});
    d.markers = std::make_shared<slint::VectorModel<UserMarker>>(std::vector<UserMarker>{});
    d.vfo_pos = 0.5f; d.vfo_half = 0.01f; d.show_vfo = true; d.cursor_visible = false; d.cursor_pos = -1;
    d.foot_left = ss("30 lines/s"); d.foot_tuned = d.frequency; d.foot_rest = ss(" . NFM");
    return c;
}

struct Stats { double mean = 0, median = 0, p95 = 0, p99 = 0, worst = 0; };
Stats statsOf(std::vector<double> v) {
    Stats s;
    if (v.empty()) { return s; }
    s.mean = std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
    std::sort(v.begin(), v.end());
    const auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.size())))]; };
    s.median = at(0.5); s.p95 = at(0.95); s.p99 = at(0.99); s.worst = v.back();
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    foxsdr::slinttest::installHeadlessPlatform();
    const int winW = std::atoi(argOf(argc, argv, "--width", "1440"));
    const int winH = std::atoi(argOf(argc, argv, "--height", "900"));
    const int frames = std::atoi(argOf(argc, argv, "--frames", "600"));
    const int warmup = std::atoi(argOf(argc, argv, "--warmup", "120"));
    const int panes = std::clamp(std::atoi(argOf(argc, argv, "--panes", "4")), 1, 4);
    const std::string png = argOf(argc, argv, "--png", "");
    const int binCount = 4096;

    auto ui = S3Window::create();
    ui->set_ws_w(static_cast<float>(winW - 56 - 252));
    ui->set_ws_h(static_cast<float>(winH - 34 - 92 - 34 - 28));
    auto& g = ui->global<WorkspaceModel>();
    std::vector<WorkspaceCell> rows;
    for (int i = 1; i <= 8; ++i) { WorkspaceCell c = makeCell(i); c.occupied = i <= panes; rows.push_back(c); }
    auto model = std::make_shared<slint::VectorModel<WorkspaceCell>>(rows);
    g.set_cells(model);
    g.set_radio_count(panes);
    g.set_layout(2);
    g.set_shown_layout(2);
    g.set_ribbon_size(1);
    foxsdr::slinttest::render(ui, static_cast<std::uint32_t>(winW), static_cast<std::uint32_t>(winH));   // lays the workspace out: the pane size
    const int cellW = static_cast<int>(g.get_cell_w()), cellH = static_cast<int>(g.get_cell_h());

    RadioSet set;
    Workspace ws(set);
    ws.setRadios((1u << panes) - 1u, panes);
    for (int i = 0; i < panes; ++i) {
        RadioView& rv = set.at(i);
        rv.state.centreHz = 145.5e6; rv.state.sampleRateHz = 2.4e6; rv.state.tunedHz = 145.5e6; rv.state.bandwidthHz = 12500; rv.state.dbMin = -100; rv.state.dbMax = -20;
        ws.fitPictures(i, cellW, cellH);
    }
    const PictureSize px0 = set.at(0).wells.size;

    // the made-up spectra, made before the timed frames (as S0: 240 distinct frames a pane, a floor near -90 dBFS, three drifting carriers and a sweeping burst, different for each pane)
    std::mt19937 rng(12345);
    std::normal_distribution<float> noise(0.f, 2.0f);
    constexpr int kLoop = 240;
    std::vector<std::vector<float>> table(static_cast<std::size_t>(panes) * kLoop);
    for (int i = 0; i < panes; ++i) {
        for (int k = 0; k < kLoop; ++k) {
            const double tsec = k / 30.0;
            std::vector<float>& out = table[static_cast<std::size_t>(i) * kLoop + static_cast<std::size_t>(k)];
            out.resize(static_cast<std::size_t>(binCount));
            for (int b = 0; b < binCount; ++b) {
                const float x = static_cast<float>(b) / static_cast<float>(binCount);
                float v = -90.f + noise(rng);
                for (int c = 0; c < 3; ++c) {
                    const float centre = 0.5f + 0.35f * std::sin(static_cast<float>(tsec * (0.3 + 0.17 * c) + i + c * 2.1));
                    const float d = (x - centre) / 0.004f;
                    v = std::max(v, -35.f - 12.f * c - 0.5f * d * d);
                }
                const float bc = std::fmod(static_cast<float>(tsec * 0.2 + i * 0.23), 1.f);
                const float bd = (x - bc) / 0.03f;
                v = std::max(v, -55.f - 0.5f * bd * bd);
                out[static_cast<std::size_t>(b)] = v;
            }
        }
    }

    std::vector<double> frameMs, hostMs, renderMs;
    const int total = warmup + frames;
    for (int tickNo = 0; tickNo < total; ++tickNo) {
        const auto t0 = Clock::now();
        for (int i = 0; i < panes; ++i) {
            RadioView& rv = set.at(i);
            const std::vector<float>& synth = table[static_cast<std::size_t>(i) * kLoop + static_cast<std::size_t>(tickNo % kLoop)];
            rv.frame.assign(synth.begin(), synth.end());
            rv.lastInfo.copied = static_cast<std::uint32_t>(binCount);
            rv.lastInfo.binCount = static_cast<std::uint32_t>(binCount);
            rv.lastInfo.centreHz = 145.5e6; rv.lastInfo.spanHz = 2.4e6;
            ++rv.sinceSeq;
            const ViewWindow cur = rv.currentView();
            const FoxReceiverState& st = rv.state;
            rv.traceState.averageAlpha = 0.1f;
            const PictureSize& px = rv.wells.size;
            paintTrace(rv.wells.traceMemory(), px.traceW, px.traceH, rv.frame.data(), rv.frame.size(), st.dbMin, st.dbMax, rv.traceState, foxsdr::slinthost::TraceMode::Normal,
                       Rgba{0x7a, 0xd7, 0xff, 255}, Rgba{0xff, 0x9f, 0x43, 255}, Rgba{0xf1, 0xea, 0xd6, 255});
            scrollWaterfallRow(rv.wells.picture.data(), px.wfW, px.wfH, rv.frame.data(), rv.frame.size(), st.dbMin, st.dbMax);
            std::memcpy(rv.wells.waterfallMemory(), rv.wells.picture.data(), sizeof(Rgba) * px.wfW * px.wfH);
            float peak = -300.f;
            for (std::size_t k = rv.frame.size() / 2 - 20; k < rv.frame.size() / 2 + 20; ++k) { peak = std::max(peak, rv.frame[k]); }
            const auto ft = frequencyTicks(cur.centreHz, cur.spanHz, rv.plot.w);
            const auto dtk = dbTicks(st.dbMin, st.dbMax, rv.plot.h);
            (void)ft; (void)dtk;
            WorkspaceCell cell = rows[static_cast<std::size_t>(i)];
            cell.pane.trace = rv.wells.traceImage();
            cell.pane.picture = rv.wells.waterfallImage();
            char pk[32]; std::snprintf(pk, sizeof(pk), "%.1f dBFS", static_cast<double>(peak));
            cell.pane.peak = ss(pk);
            rv.wells.flip();
            model->set_row_data(static_cast<std::size_t>(i), cell);
        }
        const auto t1 = Clock::now();
        foxsdr::slinttest::Frame frame = foxsdr::slinttest::render(ui, static_cast<std::uint32_t>(winW), static_cast<std::uint32_t>(winH));
        const auto t2 = Clock::now();
        if (tickNo >= warmup) { frameMs.push_back(msSince(t0, t2)); hostMs.push_back(msSince(t0, t1)); renderMs.push_back(msSince(t1, t2)); }
        if (tickNo == total - 1 && !png.empty()) { frame.savePng(png); }
    }
    const Stats f = statsOf(frameMs), h = statsOf(hostMs), r = statsOf(renderMs);
    std::printf("RESULT workspace headless-software synchronous window=%dx%d panes=%d cell=%dx%d trace=%ux%u waterfall=%ux%u frames=%d  frame mean %.2f median %.2f p95 %.2f p99 %.2f worst %.2f ms  host mean %.2f  render mean %.2f\n",
                winW, winH, panes, cellW, cellH, px0.traceW, px0.traceH, px0.wfW, px0.wfH, frames, f.mean, f.median, f.p95, f.p99, f.worst, h.mean, r.mean);
    return 0;
}
