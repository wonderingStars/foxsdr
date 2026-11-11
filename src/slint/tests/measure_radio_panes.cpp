// Screen stage S0's frame-time harness (docs/multi-radio-records/screen-s0.md): can the window draw four RadioPanes at once inside the frame budget?
//
//   measure_radio_panes --panes 1|2|3|4 --width 1440 --height 900 [--pictures default|fitted] [--frames 600] [--warmup 120] [--csv file]
//
// A REAL window (the platform's own backend, not the headless one) with one to four RadioPanes in the hand-over's 2 x 2 grid (ui/specimens/radio_pane_s0.slint). Each
// frame, from a zero-delay timer, for every pane: a made-up moving spectrum (a noise floor, drifting carriers, a moving burst; different for each pane) stands in for the
// engine's frame, then the same work the host's paneTick does with a new frame - the frame copied into the view, paintTrace into the trace buffer, scrollWaterfallRow into
// the waterfall picture, the picture copied into the alternate waterfall buffer, the axes' ticks - then the buffers flipped, two fresh slint::Image values wrapped around them
// and the pane's row of the model set (so the renderer uploads both textures again, as it does live). The frame time is the time from one timer call to the next: the
// window's own work (layout, drawing, the upload) sits inside it, since the timer only fires again after the event loop has drawn. Nothing paces the loop, so the number is
// the cost, not the display's rate; on a window that cannot draw (hidden, minimised) it would be too good, so the run counts the draws the renderer reports when it can.
//
// What it does not include: the real paneTick's lists (bookmarks, marks, markers: empty here), the focus strip, the panel, the banks, the engine's own threads, the audio.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef SLINT_FEATURE_EXPERIMENTAL
#define SLINT_FEATURE_EXPERIMENTAL 1
#endif
#include "radio_view.hpp"

#include "radio_pane_s0.h"
#include "render_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace foxsdr::slinthost;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

double cpuSeconds() {
#if defined(_WIN32)
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) { return 0; }
    const auto t = [](const FILETIME& f) { return static_cast<double>((static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7; };
    return t(k) + t(u);
#else
    return static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
#endif
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

slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }

RadioPaneData makePane(int radio, const char* device, const char* freq, const char* mode) {
    RadioPaneData d;
    d.radio = radio;
    const std::string n = std::to_string(radio);
    d.prefix = ss("Radio " + n); d.select_label = ss("Select Radio " + n); d.stop_label = ss("Stop Radio " + n);
    d.spectrum_label = ss("Radio " + n + " spectrum"); d.waterfall_label = ss("Radio " + n + " waterfall"); d.resize_label = ss("Resize Radio " + n);
    d.device = ss(device); d.frequency = ss(freq); d.mode = ss(mode);
    d.running = true; d.decoding = radio == 3; d.muted = false; d.failed = false;
    d.gain = ss("32 dB"); d.squelch = ss("-60"); d.volume = ss("70%"); d.rate = ss("2.400 MS/s");
    d.bias_tee = false; d.decoder = ss(radio == 3 ? "FT8" : ""); d.peak = ss("-41.2 dBFS"); d.span = ss("SPAN 2.400 MHz");
    d.trace_mode = 0; d.axis_unit = ss("MHz");
    d.freq_ticks = std::make_shared<slint::VectorModel<AxisTick>>(std::vector<AxisTick>{{0.1f, ss("145.0")}, {0.3f, ss("145.2")}, {0.5f, ss("145.5")}, {0.7f, ss("145.8")}, {0.9f, ss("146.0")}});
    d.db_ticks = std::make_shared<slint::VectorModel<DbTick>>(std::vector<DbTick>{{0.2f, ss("-20")}, {0.4f, ss("-40")}, {0.6f, ss("-60")}, {0.8f, ss("-80")}});
    d.age_ticks = std::make_shared<slint::VectorModel<AgeTick>>(std::vector<AgeTick>{{0.1f, ss("now")}, {0.5f, ss("10 s")}, {0.9f, ss("20 s")}});
    d.bands = std::make_shared<slint::VectorModel<BandSegment>>(std::vector<BandSegment>{{0.1f, 0.4f, ss("2 m"), slint::Color::from_argb_uint8(0, 0, 0, 0)}});
    d.bookmarks = std::make_shared<slint::VectorModel<BookmarkMark>>(std::vector<BookmarkMark>{});
    d.marks = std::make_shared<slint::VectorModel<PluginMark>>(std::vector<PluginMark>{});
    d.wf_marks = std::make_shared<slint::VectorModel<PluginMark>>(std::vector<PluginMark>{});
    d.markers = std::make_shared<slint::VectorModel<UserMarker>>(std::vector<UserMarker>{});
    d.vfo_pos = 0.5f; d.vfo_half = 0.01f; d.show_vfo = true;
    d.foot_left = ss("30 lines/s"); d.foot_tuned = ss(freq); d.foot_rest = ss(" . NFM");
    return d;
}

const char* argOf(int argc, char** argv, const char* name, const char* dflt) {
    for (int i = 1; i + 1 < argc; ++i) { if (std::strcmp(argv[i], name) == 0) { return argv[i + 1]; } }
    return dflt;
}

}  // namespace

int main(int argc, char** argv) {
    const bool headless = std::strcmp(argOf(argc, argv, "--drive", "auto"), "headless") == 0;
    if (headless) { foxsdr::slinttest::installHeadlessPlatform(); }
    const int panes = std::clamp(std::atoi(argOf(argc, argv, "--panes", "4")), 1, 4);
    const int winW = std::atoi(argOf(argc, argv, "--width", "1440"));
    const int winH = std::atoi(argOf(argc, argv, "--height", "900"));
    const int frames = std::atoi(argOf(argc, argv, "--frames", "600"));
    const int warmup = std::atoi(argOf(argc, argv, "--warmup", "120"));
    const std::string picMode = argOf(argc, argv, "--pictures", "default");
    const std::string csvPath = argOf(argc, argv, "--csv", "");
    const int binCount = std::atoi(argOf(argc, argv, "--bins", "4096"));

    // the workspace of the hand-over's layout: right of the rail (56) and the panel (252), under title bar (34), strip (92), toolbar (34); above the status bar (28)
    const double wsW = winW - 56 - 252, wsH = winH - 34 - 92 - 34 - 28;
    const double paneW = std::floor((wsW - 6) / 2), paneH = std::floor((wsH - 6) / 2);
    PictureSize pic;   // default: today's 1024 x 340 trace and 1024 x 238 waterfall
    if (picMode == "fitted") {   // the pane's plot: the pane's width, about 45 % of the 295 px under header and readout row for the trace, the rest less the axis for the waterfall
        pic.traceW = static_cast<std::uint32_t>(paneW); pic.traceH = 130;
        pic.wfW = static_cast<std::uint32_t>(paneW); pic.wfH = 140;
    }

    auto ui = S0Window::create();
    auto& win = ui->window();
    ui->set_pane_w(static_cast<float>(paneW));
    ui->set_pane_h(static_cast<float>(paneH));
    win.set_size(slint::LogicalSize({static_cast<float>(winW), static_cast<float>(winH)}));

    static const char* devices[4] = {"RTL-SDR #0", "SDRplay RSPduo", "HackRF One", "Airspy Mini"};
    static const char* freqs[4] = {"145.500000 MHz", "98.800000 MHz", "7.074000 MHz", "162.550000 MHz"};
    static const char* modes[4] = {"NFM", "WFM", "USB", "NFM"};
    RadioSet set;
    set.setPictureSize(pic);
    std::vector<RadioPaneData> data;
    for (int i = 0; i < panes; ++i) {
        data.push_back(makePane(i + 1, devices[i], freqs[i], modes[i]));
        RadioView& rv = set.at(i);
        rv.state.centreHz = 145.5e6; rv.state.sampleRateHz = 2.4e6; rv.state.tunedHz = 145.5e6; rv.state.bandwidthHz = 12500; rv.state.dbMin = -100; rv.state.dbMax = -20;
        rv.plot = PlotSize{static_cast<int>(paneW), 130};
    }
    auto model = std::make_shared<slint::VectorModel<RadioPaneData>>(data);
    ui->set_panes(model);

    // the made-up spectra
    std::mt19937 rng(12345);
    std::normal_distribution<float> noise(0.f, 2.0f);

    // HOW A FRAME IS TIMED. --drive render (the default where the backend has a rendering notifier): the next update is scheduled from the notifier's AfterRendering, so every
    // update is drawn once before the next, and the frame time is the time from one AfterRendering to the next. --drive timer: the next update is a zero-delay single-shot
    // after the last one, and the frame time is the time between updates; the count of draws the notifier saw says how many of those were drawn (the software renderer has no
    // notifier, so there it is the only way and the draw count is unknown).
    const std::string drive = argOf(argc, argv, "--drive", "auto");
    long setups = 0, drawsBefore = 0, drawsAfter = 0, teardowns = 0;
    int apiKind = -1;
    bool notifierOk = false;
    std::vector<double> frameMs, hostMs, setMs, genMs, renderMs;
    int tickNo = 0;
    int samples = 0;
    Clock::time_point lastFrame{}, beforeStamp{}, lastStep{};
    bool haveLast = false;
    double cpu0 = 0;
    Clock::time_point wall0{};
    long draws0 = 0;
    int detaches = 0;
    bool finished = false;
    std::function<void()> step;
    const auto onFrame = [&](Clock::time_point now) {
        if (haveLast && tickNo > warmup && samples < frames) { frameMs.push_back(msSince(lastFrame, now)); ++samples; }
        lastFrame = now;
        haveLast = true;
    };
    {
        auto r = win.set_rendering_notifier([&](slint::RenderingState state, slint::GraphicsAPI api) {
            apiKind = static_cast<int>(api);
            const auto now = Clock::now();
            if (state == slint::RenderingState::RenderingSetup) { ++setups; }
            if (state == slint::RenderingState::RenderingTeardown) { ++teardowns; }
            if (state == slint::RenderingState::BeforeRendering) { ++drawsBefore; beforeStamp = now; }
            if (state == slint::RenderingState::AfterRendering) {
                ++drawsAfter;
                if (tickNo > warmup) { renderMs.push_back(msSince(beforeStamp, now)); }
                if (drive != "timer" && drive != "frame" && !finished) {
                    onFrame(now);
                    slint::Timer::single_shot(std::chrono::milliseconds(0), [&]() { step(); });
                }
            }
        });
        notifierOk = !r;
    }
    const bool frameDriven = drive == "frame";
    bool renderDriven = notifierOk && drive != "timer" && !frameDriven;
    bool fellBack = false;
    const auto startedAt = Clock::now();
    // the stall guard of the render-driven loop: if nothing was drawn for a while (an update that changed nothing), update again
    slint::Timer guard;
    guard.start(slint::TimerMode::Repeated, std::chrono::milliseconds(250), [&]() {
        if (finished) { return; }
        if (msSince(startedAt, Clock::now()) > 240000.0) { std::printf("RESULT TIMEOUT drive=%s steps=%d draws=%ld samples=%d\n", renderDriven ? "render" : "timer", tickNo, drawsAfter, samples); std::fflush(stdout); finished = true; slint::quit_event_loop(); return; }
        if (renderDriven && drawsAfter == 0 && tickNo >= 30) {   // the notifier was accepted but nothing is ever reported as drawn: time the updates instead
            renderDriven = false; fellBack = true; haveLast = false; step();
            return;
        }
        if (renderDriven && msSince(lastStep, Clock::now()) > 400.0) { step(); }
    });
    // THE MADE-UP SPECTRA, made before the first frame (240 distinct frames a pane, played in a loop of eight seconds at 30 a second): a floor near -90 dBFS, three carriers
    // drifting at different rates and a burst sweeping across, different for each pane. Making them is not the host's cost, so it stays out of the timed frames.
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
    // one frame of work for every pane: the host's picture production from the next made-up frame, and the model row set (host and set are the milliseconds of each; gen stays 0)
    const auto produce = [&](double& gen, double& host, double& setc) {
        (void)gen;
        for (int i = 0; i < panes; ++i) {
            RadioView& rv = set.at(i);
            const std::vector<float>& synthFrame = table[static_cast<std::size_t>(i) * kLoop + static_cast<std::size_t>(tickNo % kLoop)];

            // the host's work for a new frame (main.cpp paneTick), minus what needs the engine and the lists
            const auto h0 = Clock::now();
            rv.frame.assign(synthFrame.begin(), synthFrame.end());
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
            char peakText[32];
            std::snprintf(peakText, sizeof(peakText), "%.1f dBFS", peak);
            RadioPaneData& d = data[static_cast<std::size_t>(i)];
            d.trace = rv.wells.traceImage();
            d.picture = rv.wells.waterfallImage();
            d.peak = ss(peakText);
            rv.wells.flip();
            const auto h1 = Clock::now();
            host += msSince(h0, h1);

            model->set_row_data(static_cast<std::size_t>(i), d);
            setc += msSince(h1, Clock::now());
        }
    };
    // the result line and the optional per-frame csv; `how` names the way the frames were timed
    const auto report = [&](const char* how, double wallStartMs) {
        for (int i = 0; i < panes; ++i) { detaches += set.at(i).wells.detaches; }
        const double cpu1 = cpuSeconds();
        const double wall = (msSince(wall0, Clock::now()) - wallStartMs) / 1000.0;
        const Stats f = statsOf(frameMs), h = statsOf(hostMs), s = statsOf(setMs), g = statsOf(genMs), r = statsOf(renderMs);
        const char* bk = std::getenv("SLINT_BACKEND");
        std::printf("RESULT backend=%s notifier=%s api=%d drive=%s panes=%d win=%dx%d pane=%.0fx%.0f pictures=%s trace=%ux%u wf=%ux%u bins=%d scale=%.2f frames=%zu "
                    "frame_mean=%.2f frame_median=%.2f frame_p95=%.2f frame_p99=%.2f frame_worst=%.2f "
                    "host_mean=%.2f host_p95=%.2f set_mean=%.2f set_p95=%.2f gen_mean=%.2f render_mean=%.2f render_p95=%.2f "
                    "cpu_pct=%.0f steps=%d draws=%ld detaches=%d\n",
                    headless ? "headless-software" : bk ? bk : "default", notifierOk ? "yes" : "no", apiKind, how, panes, winW, winH, paneW, paneH, picMode.c_str(), pic.traceW,
                    pic.traceH, pic.wfW, pic.wfH, binCount, static_cast<double>(win.scale_factor()), frameMs.size(), f.mean, f.median, f.p95, f.p99, f.worst, h.mean, h.p95,
                    s.mean, s.p95, g.mean, r.mean, r.p95, wall > 0 ? 100.0 * (cpu1 - cpu0) / wall : 0.0, tickNo - warmup, notifierOk ? drawsAfter - draws0 : -1L, detaches);
        if (!csvPath.empty()) {
            if (FILE* fp = std::fopen(csvPath.c_str(), "w")) {
                std::fprintf(fp, "frame_ms\n");
                for (double v : frameMs) { std::fprintf(fp, "%.3f\n", v); }
                std::fclose(fp);
            }
        }
        std::fflush(stdout);
    };
    if (headless) {
        // THE SOFTWARE RENDERER, SYNCHRONOUSLY: the harness's headless platform (Slint's software renderer into a pixel buffer, no window, a full repaint each frame), one frame =
        // the host's work for every pane, then one render() of the whole window. The render call includes allocating and clearing the 3.9 MB frame the harness hands back (about a millisecond).
        const int total = warmup + frames;
        for (tickNo = 0; tickNo < total; ++tickNo) {
            const auto t0 = Clock::now();
            if (tickNo == warmup) { cpu0 = cpuSeconds(); wall0 = t0; }
            double gen = 0, host = 0, setc = 0;
            produce(gen, host, setc);
            const auto t1 = Clock::now();
            foxsdr::slinttest::Frame frame = foxsdr::slinttest::render(ui, static_cast<std::uint32_t>(winW), static_cast<std::uint32_t>(winH));
            const auto t2 = Clock::now();
            if (tickNo >= warmup) {
                frameMs.push_back(msSince(t0, t2)); renderMs.push_back(msSince(t1, t2));
                genMs.push_back(gen); hostMs.push_back(host); setMs.push_back(setc);
            }
            if (tickNo == total - 1 && !csvPath.empty()) { frame.savePng(csvPath + ".png"); }
        }
        report("synchronous", 0.0);
        return 0;
    }
    step = [&]() {
        const auto t0 = Clock::now();
        lastStep = t0;
        if (!renderDriven) { onFrame(t0); }
        if (tickNo == warmup) { cpu0 = cpuSeconds(); wall0 = t0; draws0 = drawsAfter; }

        double gen = 0, host = 0, setc = 0;
        produce(gen, host, setc);
        if (tickNo >= warmup) { genMs.push_back(gen); hostMs.push_back(host); setMs.push_back(setc); }
        ++tickNo;
        if (samples >= frames && !finished) {
            finished = true;
            report(frameDriven ? "frame" : fellBack ? "timer-fallback" : renderDriven ? "render" : "timer", 0.0);
            slint::quit_event_loop();
            return;
        }
        if (!renderDriven && !frameDriven) { slint::Timer::single_shot(std::chrono::milliseconds(0), [&]() { step(); }); }
    };
    if (frameDriven) { ui->on_frame([&]() { if (!finished) { step(); } }); }
    else { slint::Timer::single_shot(std::chrono::milliseconds(0), [&]() { step(); }); }
    ui->show();
    slint::run_event_loop();
    return 0;
}
