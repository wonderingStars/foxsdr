// SPDX-License-Identifier: MIT
//
// Thin entry point: parse the command line, hand off to the GUI shell.
// `--frames N` renders exactly N frames then exits 0 — the bounded-run
// contract the app_smoke ctest entry relies on. `--selftest` runs the
// pipeline headless (no window, no GL) and checks the demo signal's peak
// lands on the right FFT bin. No flag: run until the window is closed.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "core/pipeline.hpp"
#include "gui/app_window.hpp"

namespace {

// End-to-end DSP check with zero GUI involvement, so it also runs on CI
// boxes and SSH sessions with no display. Mirrors the exact pipeline + demo
// signal AppWindow constructs; if the two drift apart, the selftest guards
// the DSP path only, which is its job.
int runSelftest() {
    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = 2'000'000.0;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 0.5f;

    cascade::core::Pipeline pipeline(cfg);
    cascade::source::SigGen& gen = pipeline.sigGen();
    gen.setTone(0, 300000.0, -30.0f);   // the dominant tone the check targets
    gen.setTone(1, -500000.0, -45.0f);  // present but 15 dB down — must NOT win
    gen.setNoiseFloorDb(-90.0f);
    pipeline.start();

    // Wait for at least 5 published frames so the EMA has settled past its
    // first-frame prime. Generous 10 s deadline: the pipeline needs only
    // ~5 * 0.5 ms of samples, but Windows scheduling under load is spiky.
    cascade::core::SpectrumFrame frame;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        pipeline.getLatestFrame(frame);
        if (frame.seq >= 5) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    pipeline.stop();

    if (frame.seq < 5) {
        std::printf("selftest FAIL only %llu frames within 10 s\n",
                    static_cast<unsigned long long>(frame.seq));
        return 1;
    }

    std::size_t peak = 0;
    for (std::size_t i = 1; i < frame.dbBins.size(); ++i) {
        if (frame.dbBins[i] > frame.dbBins[peak]) { peak = i; }
    }
    const int peakBin = static_cast<int>(peak);

    // Expected bin, computed from the config rather than hard-coded: the
    // spectrum is fftshifted (DC at fftSize/2), and +300 kHz at 2 MS/s with
    // 1024 bins sits round(300000 / 2000000 * 1024) = 154 bins above DC.
    // +/-2 bins of slack covers window spreading of the off-grid tone.
    const int expected =
        static_cast<int>(cfg.fftSize) / 2 +
        static_cast<int>(std::lround(300000.0 / cfg.sampleRateHz *
                                     static_cast<double>(cfg.fftSize)));
    if (peakBin < expected - 2 || peakBin > expected + 2) {
        std::printf("selftest FAIL peak_bin=%d expected=%d +/-2\n", peakBin, expected);
        return 1;
    }
    std::printf("selftest PASS peak_bin=%d\n", peakBin);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    int frames = -1;  // negative: run until the window is closed
    bool selftest = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "cascade: --frames requires an integer argument\n");
                return 1;
            }
            char* end = nullptr;
            const long value = std::strtol(argv[i + 1], &end, 10);
            // Reject trailing junk and negatives; cap so the int cast below
            // cannot overflow on LP64-style long values.
            if (end == argv[i + 1] || *end != '\0' || value < 0 || value > 1000000L) {
                std::fprintf(stderr, "cascade: invalid --frames value '%s'\n", argv[i + 1]);
                return 1;
            }
            frames = static_cast<int>(value);
            ++i;  // consumed the value argument
        } else if (std::strcmp(argv[i], "--selftest") == 0) {
            selftest = true;
        } else {
            std::fprintf(stderr,
                         "cascade: unknown argument '%s' (usage: cascade [--frames N] [--selftest])\n",
                         argv[i]);
            return 1;
        }
    }

    // Headless mode wins over --frames: a selftest that opened a window would
    // defeat its purpose (running where no display exists).
    if (selftest) { return runSelftest(); }

    cascade::gui::AppWindow app;
    return app.run(frames);
}
