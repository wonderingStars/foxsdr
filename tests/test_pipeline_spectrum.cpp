// test_pipeline_spectrum.cpp - THE SPECTRUM SIZE, WINDOW AND FRAME RATE IN A RUNNING CHAIN (engine serial lane A, rows S1 S2 S3).
//
// A tone with noise, a frequency offset and a sample-rate error goes through a real Pipeline; setSpectrum changes the FFT size, the window and the frame rate while it runs.
//   A  the legacy choice (1,024 / Blackman-Harris / automatic) serves 1,024 bins;  B  a larger size serves that many bins, the tone stays at its frequency and reads the same level
//   (the block's compensated level is lowered by the legacy estimator's offset), the noise per bin falls with the size;  C  every window shows the tone at its place and level;
//   D  a frame rate is kept (counted over a few seconds);  E  a choice outside the ranges (or not a number) is refused and changes nothing;  F  back to the legacy choice.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "core/pipeline.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

using cascade::core::Pipeline;
using cascade::core::SpectrumFrame;

namespace {

constexpr double kRate = 240000.0;
constexpr double kPi = 3.14159265358979323846;
// 30 kHz from the centre is a bin centre of every size from 1,024 up (30,000 / 240,000 * 1,024 = 128). The generator's clock is 40 ppm fast: its tone lands 1.2 Hz high (a 24th of a 1,024 bin,
// about 0.65 of a 131,072-point one), the noise is 40 dB under the tone.
constexpr double kToneHz = 30000.0;
constexpr double kClockError = 1.00004;

class ToneSource final : public cascade::source::IqSource {
public:
    bool start() override {
        running_ = true;
        return true;
    }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return kRate; }
    bool setSampleRateHz(double hz) override { return hz == kRate; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        std::normal_distribution<double> noise(0.0, 0.0035);   // sigma per axis: noise power 2.45e-5 against a tone of power 0.25 (-40 dB)
        for (std::size_t i = 0; i < n; ++i) {
            const double ph = 2.0 * kPi * kToneHz * kClockError * static_cast<double>(k_++) / kRate;
            dst[i] = std::complex<float>(static_cast<float>(0.5 * std::cos(ph) + noise(rng_)), static_cast<float>(0.5 * std::sin(ph) + noise(rng_)));
        }
        return n;
    }
    const char* name() const override { return "tone stand-in"; }
    const char* lastError() const override { return ""; }

private:
    bool running_ = false;
    std::uint64_t k_ = 0;
    std::mt19937_64 rng_{0x5eed};
};

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

// The newest frame with exactly `bins` bins (the pipeline may serve the old size for a few frames after a change).
bool frameOf(Pipeline& p, std::size_t bins, SpectrumFrame& out, int timeoutMs = 20000) {
    return waitFor([&] {
        SpectrumFrame f;
        if (p.getLatestFrame(f) && f.dbBins.size() == bins) {
            out = f;
            return true;
        }
        return false;
    }, timeoutMs);
}

std::size_t peakOf(const SpectrumFrame& f) { return static_cast<std::size_t>(std::max_element(f.dbBins.begin(), f.dbBins.end()) - f.dbBins.begin()); }
double medianOf(std::vector<float> v) {
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    return v[v.size() / 2];
}
double wantedBin(std::size_t bins) { return (kToneHz / kRate + 0.5) * static_cast<double>(bins); }

}  // namespace

int main() {
    std::printf("test_pipeline_spectrum\n");
    Pipeline::Config cfg;
    cfg.sampleRateHz = kRate;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;
    Pipeline p(cfg);
    CHECK(p.spectrumFftSize() == 1024 && p.spectrumWindow() == 3 && p.spectrumFrameRateHz() == 0.0);
    p.setSource(std::make_unique<ToneSource>());
    p.start();

    // A: the legacy choice
    SpectrumFrame f;
    CHECK(frameOf(p, 1024, f));
    const double legacyPeak = f.dbBins[peakOf(f)];
    const double legacyFloor = medianOf(f.dbBins);
    std::printf("  A legacy: peak bin %zu (wanted %.1f), %.1f dB, median %.1f dB\n", peakOf(f), wantedBin(1024), legacyPeak, legacyFloor);
    CHECK(std::fabs(static_cast<double>(peakOf(f)) - wantedBin(1024)) <= 1.0);

    // B: sizes
    for (const std::size_t n : {std::size_t{4096}, std::size_t{16384}, std::size_t{65536}, std::size_t{131072}}) {
        CHECK(p.setSpectrum(n, 3, 0.0));
        CHECK(p.spectrumFftSize() == n);
        const bool got = frameOf(p, n, f);
        CHECK(got);
        if (!got) { continue; }
        const double peak = f.dbBins[peakOf(f)];
        const double floorDb = medianOf(f.dbBins);
        const double expectDrop = 10.0 * std::log10(static_cast<double>(n) / 1024.0);
        std::printf("  B %6zu: peak bin %zu (wanted %.1f), %.2f dB (legacy %.2f), median %.1f dB (drop %.1f, expected %.1f)\n", n, peakOf(f), wantedBin(n), peak, legacyPeak, floorDb, legacyFloor - floorDb, expectDrop);
        CHECK(std::fabs(static_cast<double>(peakOf(f)) - wantedBin(n)) <= 2.0);
        CHECK(std::fabs(peak - legacyPeak) <= 1.5);                      // the level a tone reads does not move with the size
        CHECK(std::fabs((legacyFloor - floorDb) - expectDrop) <= 3.0);   // the noise per bin falls with it
    }

    // C: windows (the tone sits on a bin: the loss is the window's own and the clock error's)
    for (int w = 0; w <= 4; ++w) {
        CHECK(p.setSpectrum(8192, w, 0.0));
        CHECK(p.spectrumWindow() == w);
        // after a window change the first frames can still belong to the old window: take a frame, then one more
        SpectrumFrame a, b;
        CHECK(frameOf(p, 8192, a));
        CHECK(frameOf(p, 8192, b));
        const double peak = b.dbBins[peakOf(b)];
        std::printf("  C window %d: peak bin %zu, %.2f dB (legacy %.2f)\n", w, peakOf(b), peak, legacyPeak);
        CHECK(std::fabs(static_cast<double>(peakOf(b)) - wantedBin(8192)) <= 3.0);
        CHECK(std::fabs(peak - legacyPeak) <= 3.0);
    }

    // D: a frame rate is kept
    for (const double fps : {10.0, 50.0}) {
        CHECK(p.setSpectrum(4096, 3, fps));
        CHECK(frameOf(p, 4096, f));
        std::uint64_t first = 0, last = 0;
        SpectrumFrame g;
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3)) {
            if (p.getLatestFrame(g)) {
                if (first == 0) { first = g.seq; }
                last = g.seq;
            }
            std::this_thread::yield();   // (a sleep of 1 ms is 15 ms on Windows: frames 20 ms apart would be collapsed into one pull)
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double rate = static_cast<double>(last - first) / secs;
        std::printf("  D asked %.0f/s: %.1f frames a second\n", fps, rate);
        CHECK(rate >= fps * 0.7 && rate <= fps * 1.3);
    }

    // E: refusals change nothing
    CHECK(p.setSpectrum(4096, 3, 50.0));
    for (const std::size_t bad : {std::size_t{0}, std::size_t{512}, std::size_t{1000}, std::size_t{3072}, std::size_t{262144}, std::size_t{524288}}) { CHECK(!p.setSpectrum(bad, 3, 0.0)); }
    CHECK(!p.setSpectrum(4096, -1, 0.0));
    CHECK(!p.setSpectrum(4096, 5, 0.0));
    for (const double bad : {-1.0, 0.5, 121.0, std::nan(""), HUGE_VAL}) { CHECK(!p.setSpectrum(4096, 3, bad)); }
    CHECK(p.spectrumFftSize() == 4096 && p.spectrumWindow() == 3 && p.spectrumFrameRateHz() == 50.0);

    // F: back to the legacy choice
    CHECK(p.setSpectrum(1024, 3, 0.0));
    CHECK(frameOf(p, 1024, f));
    std::printf("  F legacy again: peak bin %zu, %.2f dB\n", peakOf(f), f.dbBins[peakOf(f)]);
    CHECK(std::fabs(f.dbBins[peakOf(f)] - legacyPeak) <= 1.0);
    p.stop();
    return testSummary("test_pipeline_spectrum");
}
