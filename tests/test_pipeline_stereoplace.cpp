// test_pipeline_stereoplace.cpp - STEREO PLACEMENT OF A MONO RECEIVER IN A RUNNING CHAIN (engine serial lane A, rows F20 and D5).
//
// Band-limited noise (USB audio, so the two ears have something to disagree about) goes through a real Pipeline and the finished left and right audio is read back from the tap.
//   A  off (the default): left and right are the same samples;  B  pan: -1 puts everything in the left ear, +1 in the right, the centre is -3 dB in each (constant power);  C  pseudo-stereo:
//   the two ears decorrelate as the amount goes up, the mono sum keeps the signal;  D  binaural: a source on the right is louder in the right ear and arrives there first;  E  back to off is
//   identical again;  F  values outside their ranges are refused and change nothing.
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
#include "dsp/agc.hpp"
#include "dsp/demod.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

using cascade::core::Pipeline;
using cascade::dsp::DemodMode;

namespace {

constexpr double kRate = 240000.0;

class NoiseSource final : public cascade::source::IqSource {
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
        std::normal_distribution<double> noise(0.0, 0.1);
        for (std::size_t i = 0; i < n; ++i) { dst[i] = std::complex<float>(static_cast<float>(noise(rng_)), static_cast<float>(noise(rng_))); }
        return n;
    }
    const char* name() const override { return "noise stand-in"; }
    const char* lastError() const override { return ""; }

private:
    bool running_ = false;
    std::mt19937_64 rng_{0x57e0};
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

void letAudioRun(Pipeline& p, double seconds) {
    const std::uint64_t need = static_cast<std::uint64_t>(seconds * Pipeline::kAudioRateHz);
    const std::uint64_t mark = p.audioSamplesProduced();
    waitFor([&] { return p.audioSamplesProduced() - mark >= need; }, 60000);
}

struct Pair {
    std::vector<double> l, r;
};
// Several taps strung together (each tap is a contiguous window; correlation is taken inside each and summed).
struct Stats {
    double pl = 0, pr = 0, cross = 0, maxDiff = 0, bestLag = 0;
};
Stats measure(const Pipeline& p) {
    Stats s;
    std::vector<double> crossAtLag(81, 0.0);   // lags -40..+40 samples
    double nTotal = 0;
    for (int t = 0; t < 8; ++t) {
        std::vector<float> l(4096), r(4096);
        const std::size_t n = p.audioTapStereo(l.data(), r.data(), l.size());
        for (std::size_t i = 0; i < n; ++i) {
            s.pl += static_cast<double>(l[i]) * l[i];
            s.pr += static_cast<double>(r[i]) * r[i];
            s.cross += static_cast<double>(l[i]) * r[i];
            s.maxDiff = std::max(s.maxDiff, static_cast<double>(std::fabs(l[i] - r[i])));
            for (int lag = -40; lag <= 40; ++lag) {
                const long j = static_cast<long>(i) + lag;
                if (j >= 0 && j < static_cast<long>(n)) { crossAtLag[static_cast<std::size_t>(lag + 40)] += static_cast<double>(l[static_cast<std::size_t>(j)]) * r[i]; }
            }
        }
        nTotal += static_cast<double>(n);
        std::this_thread::sleep_for(std::chrono::milliseconds(70));
    }
    s.pl /= nTotal;
    s.pr /= nTotal;
    s.cross /= nTotal;
    s.bestLag = static_cast<double>(std::max_element(crossAtLag.begin(), crossAtLag.end()) - crossAtLag.begin()) - 40.0;
    return s;
}
double db(double p) { return 10.0 * std::log10(std::max(p, 1e-30)); }
double corr(const Stats& s) { return s.cross / std::sqrt(std::max(s.pl * s.pr, 1e-30)); }

}  // namespace

int main() {
    std::printf("test_pipeline_stereoplace\n");
    Pipeline::Config cfg;
    cfg.sampleRateHz = kRate;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;
    Pipeline p(cfg);
    CHECK(p.stereoPlaceMode() == 0 && p.stereoPlaceValue() == 0.0);
    p.setSource(std::make_unique<NoiseSource>());
    p.setDemodMode(DemodMode::USB);
    p.setSquelchDb(-100.0f);
    p.setAgcMode(cascade::dsp::Agc::Mode::Off);
    p.setAgcFixedGain(-10.0f);
    p.start();
    CHECK(waitFor([&] { return p.audioSamplesProduced() > 4800; }, 30000));
    letAudioRun(p, 2.0);

    // A: off
    const Stats off = measure(p);
    std::printf("  A off: L %.1f dB, R %.1f dB, largest difference %.2e\n", db(off.pl), db(off.pr), off.maxDiff);
    CHECK(off.maxDiff == 0.0);
    CHECK(db(off.pl) > -60.0);   // there is a signal

    // B: pan
    CHECK(p.setStereoPlace(1, -1.0));
    letAudioRun(p, 1.0);
    const Stats left = measure(p);
    CHECK(p.setStereoPlace(1, 1.0));
    letAudioRun(p, 1.0);
    const Stats right = measure(p);
    CHECK(p.setStereoPlace(1, 0.0));
    letAudioRun(p, 1.0);
    const Stats mid = measure(p);
    std::printf("  B pan -1: L %.1f R %.1f dB;  +1: L %.1f R %.1f dB;  0: L %.1f R %.1f dB (off: %.1f)\n", db(left.pl), db(left.pr), db(right.pl), db(right.pr), db(mid.pl), db(mid.pr), db(off.pl));
    CHECK(db(left.pr) < db(left.pl) - 40.0);
    CHECK(db(right.pl) < db(right.pr) - 40.0);
    CHECK(std::fabs(db(mid.pl) - db(mid.pr)) < 1.0);
    CHECK(std::fabs((db(mid.pl) + db(mid.pr)) * 0.5 - (db(off.pl) - 3.0)) < 2.0);   // -3 dB in each ear at the centre

    // C: pseudo-stereo
    CHECK(p.setStereoPlace(2, 1.0));
    letAudioRun(p, 1.0);
    const Stats full = measure(p);
    CHECK(p.setStereoPlace(2, 0.5));
    letAudioRun(p, 1.0);
    const Stats half = measure(p);
    std::printf("  C pseudo 1.0: correlation %.2f (wanted 0);  0.5: %.2f (wanted 0.60);  powers L %.1f R %.1f dB\n", corr(full), corr(half), db(full.pl), db(full.pr));
    CHECK(std::fabs(corr(full)) < 0.2);
    CHECK(std::fabs(corr(half) - 0.6) < 0.2);
    CHECK(std::fabs(db(full.pl) - db(full.pr)) < 1.0);
    CHECK(std::fabs(db(full.pl) - db(off.pl)) < 1.5);   // the ears are as loud as the mono signal was

    // D: binaural, a source 60 degrees to the right
    CHECK(p.setStereoPlace(3, 60.0));
    letAudioRun(p, 1.0);
    const Stats bin = measure(p);
    std::printf("  D binaural +60: R-L %.1f dB, L is %+.0f samples after R (Woodworth: 23.4)\n", db(bin.pr) - db(bin.pl), bin.bestLag);
    // The block's level difference is 2 x (ildDb / 2 x sin 60 degrees) = 5.2 dB; the far ear's fractional delay (23.4 samples, linear interpolation) also averages neighbouring samples, which costs white
    // noise (this signal) 10 log10(0.6^2 + 0.4^2) = 2.8 dB and a narrow-band signal almost nothing: 7.8 dB is what this noise should show.
    CHECK(std::fabs((db(bin.pr) - db(bin.pl)) - 7.8) < 1.5);
    CHECK(std::fabs(bin.bestLag - 23.0) <= 3.0);

    // E: off again
    CHECK(p.setStereoPlace(0, 0.0));
    letAudioRun(p, 1.0);
    CHECK(measure(p).maxDiff == 0.0);

    // F: refusals change nothing
    CHECK(p.setStereoPlace(1, 0.25));
    for (const int m : {-1, 4, 99}) { CHECK(!p.setStereoPlace(m, 0.0)); }
    CHECK(!p.setStereoPlace(1, 1.5) && !p.setStereoPlace(1, -1.01));
    CHECK(!p.setStereoPlace(2, -0.1) && !p.setStereoPlace(2, 1.1));
    CHECK(!p.setStereoPlace(3, 91.0) && !p.setStereoPlace(3, -91.0));
    CHECK(!p.setStereoPlace(1, std::nan("")) && !p.setStereoPlace(3, HUGE_VAL));
    CHECK(p.stereoPlaceMode() == 1 && p.stereoPlaceValue() == 0.25);
    p.stop();
    return testSummary("test_pipeline_stereoplace");
}
