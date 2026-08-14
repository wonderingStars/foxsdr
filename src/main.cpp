// SPDX-License-Identifier: MIT
//
// Thin entry point: parse the command line, hand off to the GUI shell.
// `--frames N` renders exactly N frames then exits 0 — the bounded-run
// contract the app_smoke ctest entry relies on. `--selftest` runs the
// pipeline headless (no window, no GL, no audio device) and checks that the
// demo signal's peak lands on the right FFT bin AND that the audio chain
// demodulates a CW carrier to its 700 Hz sidetone. No flag: run until the
// window is closed.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "core/pipeline.hpp"
#include "gui/app_window.hpp"
#include "source/soapy_source.hpp"

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
    cfg.audioEnabled = false;  // the audio chain still runs; no device needed

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

    if (frame.seq < 5) {
        pipeline.stop();
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
        pipeline.stop();
        std::printf("selftest FAIL peak_bin=%d expected=%d +/-2\n", peakBin, expected);
        return 1;
    }

    // --- Audio-chain check (P3) ---------------------------------------------
    // Tune the VFO onto demo tone 0 and demodulate it as CW: the carrier at
    // the VFO center must beat at the 700 Hz sidetone. The chain is tapped
    // BEFORE AudioOut (Pipeline::audioTap / audioSamplesProduced), so this
    // verifies VFO -> demod -> AGC -> squelch -> resampler with no device.
    pipeline.setDemodMode(cascade::dsp::DemodMode::CW);
    pipeline.setVfoOffsetHz(300000.0);

    // Let >= 24000 post-switch samples (0.5 s at 48 kHz) flow so the squelch
    // has opened and the tap window holds pure steady-state CW audio.
    const std::uint64_t base = pipeline.audioSamplesProduced();
    std::uint64_t produced = 0;
    const auto audioDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < audioDeadline) {
        produced = pipeline.audioSamplesProduced() - base;
        if (produced >= 24000) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (produced < 24000) {
        pipeline.stop();
        std::printf("selftest FAIL only %llu audio samples within 10 s\n",
                    static_cast<unsigned long long>(produced));
        return 1;
    }

    std::vector<float> window(4096);
    const std::size_t got = pipeline.audioTap(window.data(), window.size());
    pipeline.stop();
    if (got < window.size()) {
        std::printf("selftest FAIL audio tap returned %llu of %llu samples\n",
                    static_cast<unsigned long long>(got),
                    static_cast<unsigned long long>(window.size()));
        return 1;
    }

    // Dominant DFT frequency over the captured window: direct DFT (no
    // dependency on the FFT wrapper — an independent reference, per the
    // testing protocol), rectangular window, bins 1..N/2 so DC never wins.
    const std::size_t nWin = window.size();
    std::size_t bestBin = 1;
    double bestPow = -1.0;
    for (std::size_t k = 1; k <= nWin / 2; ++k) {
        double re = 0.0;
        double im = 0.0;
        const double w = -2.0 * 3.14159265358979323846 *
                         static_cast<double>(k) / static_cast<double>(nWin);
        for (std::size_t i = 0; i < nWin; ++i) {
            const double angle = w * static_cast<double>(i);
            const double x = static_cast<double>(window[i]);
            re += x * std::cos(angle);
            im += x * std::sin(angle);
        }
        const double p = re * re + im * im;
        if (p > bestPow) {
            bestPow = p;
            bestBin = k;
        }
    }
    const double audioHz = static_cast<double>(bestBin) *
                           cascade::core::Pipeline::kAudioRateHz /
                           static_cast<double>(nWin);

    // CW sidetone is 700 Hz (dsp/demod.cpp kCwToneHz). +/-40 Hz covers the
    // 48000/4096 = 11.7 Hz bin quantization with margin.
    if (std::fabs(audioHz - 700.0) > 40.0) {
        std::printf("selftest FAIL audio_hz=%.1f expected 700 +/-40\n", audioHz);
        return 1;
    }
    std::printf("selftest PASS peak_bin=%d audio_hz=%.1f\n", peakBin, audioHz);
    return 0;
}

// Bench diagnostic (hidden flag --soapy-check, deliberately absent from the
// usage string and never a ctest entry — it needs real hardware): enumerate
// SoapySDR, open the first RADIO device, set 2 MS/s / 100 MHz, and stream for
// two seconds through a headless Pipeline. PASS requires >= 20 spectrum
// frames and a rate readback within 1% of 2 MS/s. A machine with no device
// (or no vendor modules) FAILs cleanly with a reason — that is the expected
// answer off the bench, not a crash. The 30 s first-frame deadline covers a
// cold USB device's firmware/FPGA load, which the open above already blocked
// through once (UHD loads inside Device::make).
//
// "First RADIO device": SoapyAudio (driver=audio) advertises every sound
// card as a SoapySDR device, so on a bench with that module installed the
// literal first enumeration row is a Realtek codec, not the SDR. Rows whose
// kwargs carry driver=audio are skipped — a sound card cannot satisfy a
// 2 MS/s RF check and its constant presence would make "no device attached"
// undetectable.
int runSoapyCheck() {
    const auto devices = cascade::source::SoapySource::enumerate();
    const cascade::source::SoapyDeviceInfo* pick = nullptr;
    for (const auto& d : devices) {
        if (d.args.find("driver=audio") == std::string::npos) {
            pick = &d;
            break;
        }
    }
    if (pick == nullptr) {
        std::printf("soapy-check FAIL no SoapySDR radio devices enumerated\n");
        return 1;
    }
    std::fprintf(stderr, "cascade: soapy-check device: %s (%s)\n",
                 pick->label.c_str(), pick->args.c_str());

    auto src = std::make_unique<cascade::source::SoapySource>();
    if (!src->open(pick->args)) {
        std::printf("soapy-check FAIL open: %s\n", src->lastError());
        return 1;
    }
    if (!src->setSampleRateHz(2'000'000.0)) {
        std::printf("soapy-check FAIL set rate: %s\n", src->lastError());
        return 1;
    }
    if (!src->setCenterFrequencyHz(100'000'000.0)) {
        std::printf("soapy-check FAIL tune: %s\n", src->lastError());
        return 1;
    }
    const double actualRate = src->sampleRateHz();  // device readback, not echo

    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = 2'000'000.0;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 0.5f;
    cfg.audioEnabled = false;  // bench diagnostic: no audio device wanted
    cascade::core::Pipeline pipeline(cfg);
    pipeline.setSource(std::move(src));
    pipeline.start();

    // Wait (bounded) for the stream to produce its first frame, then measure
    // the frame count over exactly two seconds of steady streaming.
    cascade::core::SpectrumFrame frame;
    const auto firstDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < firstDeadline) {
        pipeline.getLatestFrame(frame);
        if (frame.seq >= 1) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (frame.seq < 1) {
        pipeline.stop();
        std::printf("soapy-check FAIL no spectrum frames within 30 s (%s)\n",
                    pipeline.activeSource().lastError());
        return 1;
    }
    const std::uint64_t seqAtStart = frame.seq;
    std::this_thread::sleep_for(std::chrono::seconds(2));
    pipeline.getLatestFrame(frame);
    const std::uint64_t frames = frame.seq - seqAtStart;
    pipeline.stop();

    if (frames < 20) {
        std::printf("soapy-check FAIL only %llu frames in 2 s (>= 20 required)\n",
                    static_cast<unsigned long long>(frames));
        return 1;
    }
    if (std::fabs(actualRate - 2'000'000.0) > 0.01 * 2'000'000.0) {
        std::printf("soapy-check FAIL rate readback %.0f not within 1%% of 2000000\n",
                    actualRate);
        return 1;
    }
    std::printf("soapy-check PASS frames=%llu rate=%.0f\n",
                static_cast<unsigned long long>(frames), actualRate);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    int frames = -1;  // negative: run until the window is closed
    bool selftest = false;
    bool soapyCheck = false;
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
        } else if (std::strcmp(argv[i], "--soapy-check") == 0) {
            // Hidden bench diagnostic (see runSoapyCheck): not in the usage
            // string on purpose — it requires attached hardware, so
            // advertising it in CI-facing help would only invite red herrings.
            soapyCheck = true;
        } else {
            std::fprintf(stderr,
                         "cascade: unknown argument '%s' (usage: cascade [--frames N] [--selftest])\n",
                         argv[i]);
            return 1;
        }
    }

    // Headless mode wins over --frames: a selftest that opened a window would
    // defeat its purpose (running where no display exists). Same for the
    // hardware bench check, which is likewise headless.
    if (selftest) { return runSelftest(); }
    if (soapyCheck) { return runSoapyCheck(); }

    cascade::gui::AppWindow app;
    return app.run(frames);
}
