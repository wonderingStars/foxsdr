// test_pipeline_zoom.cpp - THE ZOOM FFT OF THE IF PASSBAND IN A RUNNING CHAIN (engine serial lane A, row S12): Pipeline::setIfZoom / zoomFrame.
//
// Two carriers 30 Hz apart (1,500 Hz and 1,530 Hz from the tuned frequency) go through a real Pipeline at a 200 kHz channel rate.
//   A  off (the default): no frame;  B  on at 2,048 bins over 8,000 Hz: the bin width is 3.906 Hz, the picture is 8,000 Hz wide, the two carriers are two peaks at the bins their frequencies give (+-1), with a dip of 10 dB or
//   more between them - resolution the 1,024-point spectrum of the whole 2.4 MHz band (2.3 kHz a bin) cannot give;  C  values outside the ranges are refused and change nothing;  D  turning it off again stops the frames.
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
#include "dsp/demod.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

using cascade::core::Pipeline;

namespace {

constexpr double kRate = 2400000.0;   // channel rate 200 kHz
constexpr double kPi = 3.14159265358979323846;

class TwoTones final : public cascade::source::IqSource {
public:
    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return kRate; }
    bool setSampleRateHz(double hz) override { return hz == kRate; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        std::normal_distribution<double> noise(0.0, 0.0005);
        for (std::size_t i = 0; i < n; ++i, ++k_) {
            const double t = static_cast<double>(k_) / kRate;
            const double a = 2.0 * kPi * 1500.0 * t, b = 2.0 * kPi * 1530.0 * t;
            dst[i] = {static_cast<float>(0.25 * std::cos(a) + 0.25 * std::cos(b) + noise(rng_)), static_cast<float>(0.25 * std::sin(a) + 0.25 * std::sin(b) + noise(rng_))};
        }
        return n;
    }
    const char* name() const override { return "two tones"; }
    const char* lastError() const override { return ""; }

private:
    bool running_ = false;
    std::uint64_t k_ = 0;
    std::mt19937_64 rng_{0x25};
};

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

}  // namespace

int main() {
    std::printf("test_pipeline_zoom\n");
    Pipeline::Config cfg;
    cfg.sampleRateHz = kRate;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;
    Pipeline p(cfg);
    p.setSource(std::make_unique<TwoTones>());
    p.setDemodMode(cascade::dsp::DemodMode::AM);
    p.setVfoBandwidthHz(6000.0);
    p.setSquelchDb(-120.0f);
    p.start();
    CHECK(waitFor([&] { return p.audioSamplesProduced() > 48000; }, 30000));
    CHECK(p.channelRateHz() == 200000.0);
    std::vector<float> db(16384);

    // A
    CHECK(p.ifZoomFftSize() == 0);
    CHECK(p.zoomFrame(db.data(), db.size()).frame == 0);

    // B
    CHECK(p.setIfZoom(2048, 8000.0));
    CHECK(waitFor([&] { return p.zoomFrame(db.data(), db.size()).frame >= 3; }, 20000));
    const Pipeline::ZoomFrameInfo info = p.zoomFrame(db.data(), db.size());
    std::printf("  B: %zu bins of %.3f Hz over %.0f Hz (frame %llu)\n", info.bins, info.binHz, info.spanHz, static_cast<unsigned long long>(info.frame));
    CHECK(p.ifZoomFftSize() == 2048);
    CHECK(info.bins == 2048);
    CHECK(std::fabs(info.binHz - 8000.0 / 2048.0) < 1e-6);
    CHECK(std::fabs(info.spanHz - 8000.0) < 1e-6);
    CHECK(info.usableSpanHz > 6000.0 && info.usableSpanHz < 6500.0);
    if (info.bins == 2048) {
        auto binOf = [&](double hz) { return 1024 + static_cast<int>(std::lround(hz / info.binHz)); };
        auto peakNear = [&](int centre) {
            int best = centre;
            for (int k = centre - 4; k <= centre + 4; ++k) { if (db[static_cast<std::size_t>(k)] > db[static_cast<std::size_t>(best)]) { best = k; } }
            return best;
        };
        const int pa = peakNear(binOf(1500.0)), pb = peakNear(binOf(1530.0));
        std::printf("  B: peaks at bins %d and %d (wanted %d and %d), %.1f and %.1f dB\n", pa, pb, binOf(1500.0), binOf(1530.0), static_cast<double>(db[static_cast<std::size_t>(pa)]), static_cast<double>(db[static_cast<std::size_t>(pb)]));
        CHECK(std::abs(pa - binOf(1500.0)) <= 1 && std::abs(pb - binOf(1530.0)) <= 1);
        CHECK(pb - pa >= 6);
        const int mid = (pa + pb) / 2;
        const float dip = std::min(db[static_cast<std::size_t>(pa)], db[static_cast<std::size_t>(pb)]) - db[static_cast<std::size_t>(mid)];
        std::printf("  B: dip between them %.1f dB\n", static_cast<double>(dip));
        CHECK(dip > 10.0f);
        // far from both carriers, the floor is low
        CHECK(db[static_cast<std::size_t>(binOf(-3000.0))] < db[static_cast<std::size_t>(pa)] - 40.0f);
    }

    // C
    CHECK(!p.setIfZoom(300, 8000.0));           // not a power of two
    CHECK(!p.setIfZoom(128, 8000.0));           // too small
    CHECK(!p.setIfZoom(32768, 8000.0));         // too large
    CHECK(!p.setIfZoom(2048, 50.0));            // narrower than 100 Hz
    CHECK(!p.setIfZoom(2048, 300000.0));        // wider than the channel
    CHECK(p.ifZoomFftSize() == 2048);           // nothing changed

    // D
    CHECK(p.setIfZoom(0, 0.0));
    CHECK(waitFor([&] { return p.zoomFrame(db.data(), db.size()).frame == 0; }, 5000));
    CHECK(p.ifZoomFftSize() == 0);
    p.stop();
    return testSummary("test_pipeline_zoom");
}
