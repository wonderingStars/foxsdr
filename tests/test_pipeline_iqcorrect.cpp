// test_pipeline_iqcorrect.cpp - THE I/Q CORRECTION IN A RUNNING CHAIN (engine serial lane A, row F16).
//
// A tone 30 kHz from the centre of a front end with a gain/phase error (an image of -24 dB at the mirrored frequency) and a DC offset, plus noise, goes through a real Pipeline.
//   A  mask 0 (the default): the image and the DC spike stand where the front end put them;  B  DC removal takes the spike out;  C  balance takes the image down (after the estimator has settled);
//   D  swap mirrors the picture (the tone is where its image was);  E  all three together;  F  a mask outside 0..7 is refused and changes nothing;  G  back to 0 and the picture is as in A;
//   H  the choice survives a rate change.
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
constexpr double kToneHz = 30000.0;                    // bin 640 of 1,024 (bin centre)
constexpr std::complex<double> kDc{0.05, 0.03};
constexpr double kEps = 0.06;                          // |b / a| = 0.06: the image 24.4 dB under the tone

class ImbalancedSource final : public cascade::source::IqSource {
public:
    bool start() override {
        running_ = true;
        return true;
    }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return rate_; }
    bool setSampleRateHz(double hz) override {
        rate_ = hz;
        return true;
    }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        std::normal_distribution<double> noise(0.0, 0.003);
        for (std::size_t i = 0; i < n; ++i) {
            const double ph = 2.0 * kPi * kToneHz * static_cast<double>(k_++) / rate_;
            const std::complex<double> x(0.4 * std::cos(ph), 0.4 * std::sin(ph));
            const std::complex<double> y = x + kEps * std::conj(x) * std::complex<double>(std::cos(0.7), std::sin(0.7)) + kDc + std::complex<double>(noise(rng_), noise(rng_));
            dst[i] = std::complex<float>(static_cast<float>(y.real()), static_cast<float>(y.imag()));
        }
        return n;
    }
    const char* name() const override { return "imbalanced tone stand-in"; }
    const char* lastError() const override { return ""; }

private:
    bool running_ = false;
    double rate_ = kRate;
    std::uint64_t k_ = 0;
    std::mt19937_64 rng_{0xbad1};
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

// The level of the strongest bin within 3 bins of `bin`, averaged over a second of frames.
struct Levels {
    double tone, image, dc;
};
Levels measure(Pipeline& p, double seconds) {
    SpectrumFrame f;
    double tone = 0, image = 0, dc = 0;
    int n = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(static_cast<int>(seconds * 1000.0))) {
        if (p.getLatestFrame(f) && f.dbBins.size() == 1024) {
            auto peak = [&](std::size_t bin) { return static_cast<double>(*std::max_element(f.dbBins.begin() + static_cast<std::ptrdiff_t>(bin) - 3, f.dbBins.begin() + static_cast<std::ptrdiff_t>(bin) + 4)); };
            tone += peak(640);
            image += peak(384);
            dc += peak(512);
            ++n;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return n > 0 ? Levels{tone / n, image / n, dc / n} : Levels{-999, -999, -999};
}

}  // namespace

int main() {
    std::printf("test_pipeline_iqcorrect\n");
    Pipeline::Config cfg;
    cfg.sampleRateHz = kRate;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;
    Pipeline p(cfg);
    CHECK(p.iqCorrection() == 0);
    p.setSource(std::make_unique<ImbalancedSource>());
    p.start();
    CHECK(waitFor([&] { SpectrumFrame f; return p.getLatestFrame(f); }, 20000));

    const Levels a = measure(p, 1.0);
    std::printf("  A mask 0: tone %.1f dB, image %.1f dB (%.1f under), DC %.1f dB\n", a.tone, a.image, a.tone - a.image, a.dc);
    CHECK(std::fabs((a.tone - a.image) - 24.4) < 3.0);
    CHECK(std::fabs((a.dc - a.tone) - (-16.8)) < 2.0);   // |dc| = 0.058 against a tone of 0.4: 20 log10(0.058 / 0.4) = -16.8 dB
    CHECK(p.iqImageRejectionDb() == 200.0);                  // nothing measured while the balance is off

    CHECK(p.setIqCorrection(1));
    const Levels b = measure(p, 3.0);
    std::printf("  B DC removal: DC %.1f dB (was %.1f)\n", b.dc, a.dc);
    CHECK(b.dc < a.dc - 15.0);
    CHECK(std::fabs(b.tone - a.tone) < 1.0);

    CHECK(p.setIqCorrection(2));
    const Levels c = measure(p, 5.0);   // the estimator's own time constant is a second
    std::printf("  C balance: image %.1f dB (was %.1f), tone %.1f dB, estimator reads %.1f dB\n", c.image, a.image, c.tone, p.iqImageRejectionDb());
    CHECK(c.image < a.image - 12.0);
    CHECK(std::fabs(c.tone - a.tone) < 1.5);
    CHECK(p.iqImageRejectionDb() < 100.0);

    CHECK(p.setIqCorrection(4));
    const Levels d = measure(p, 3.0);
    std::printf("  D swap: tone bin %.1f dB, mirrored bin %.1f dB (A had %.1f and %.1f)\n", d.tone, d.image, a.tone, a.image);
    CHECK(std::fabs(d.image - a.tone) < 2.0);   // the tone is now where its image was
    CHECK(std::fabs(d.tone - a.image) < 3.0);   // and the image where the tone was

    CHECK(p.setIqCorrection(7));
    CHECK(p.iqCorrection() == 7);
    const Levels e = measure(p, 6.0);
    std::printf("  E all three: DC %.1f dB, image %.1f dB, tone(swapped) %.1f dB\n", e.dc, e.image, e.tone);
    CHECK(e.dc < a.dc - 15.0);

    for (const int bad : {-1, 8, 100}) { CHECK(!p.setIqCorrection(bad)); }
    CHECK(p.iqCorrection() == 7);

    CHECK(p.setIqCorrection(0));
    const Levels g = measure(p, 3.0);
    std::printf("  G mask 0 again: tone %.1f, image %.1f, DC %.1f (A: %.1f %.1f %.1f)\n", g.tone, g.image, g.dc, a.tone, a.image, a.dc);
    CHECK(std::fabs(g.tone - a.tone) < 1.5 && std::fabs(g.image - a.image) < 2.5 && std::fabs(g.dc - a.dc) < 2.5);

    // H: a rate change keeps the choice (the corrector is rebuilt for the new rate)
    CHECK(p.setIqCorrection(1));
    const bool changed = p.setInputRateHz(kRate / 2.0);
    std::printf("  H rate change accepted: %d, mask now %d\n", changed ? 1 : 0, p.iqCorrection());
    CHECK(p.iqCorrection() == 1);
    p.stop();
    return testSummary("test_pipeline_iqcorrect");
}
