// Tests for dsp/sigmeas_modulation.hpp: FM deviation meter and AM modulation-depth meter,
// pinned on generated signals with noise (35 dB SNR), a carrier offset and a clock error.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/sigmeas_modulation.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "sigmeas_gen.hpp"
#include "test_check.hpp"

using namespace cascade::sigmeas;

namespace {
double relErr(double got, double want) { return std::fabs(got - want) / want; }
}  // namespace

int main() {
    // --- FM deviation: 2.5, 12.5 and 25 kHz peak, within 1 %, with offset, noise, 200 ppm clock -----
    struct Case { double rate, dev; };
    for (const Case c : {Case{48000.0, 2500.0}, Case{96000.0, 12500.0}, Case{192000.0, 25000.0}}) {
        sg::Rng rng(21);
        const double e = 200e-6;
        auto x = sg::fmAm(static_cast<std::size_t>(c.rate), c.rate * (1.0 + e), 3000.0, 1000.0, c.dev);
        sg::addNoise(x, 35.0, rng);
        const FmResult r = measureFmDeviation(x.data(), x.size(), c.rate);
        CHECK(r.valid);
        if (r.valid) {
            const double want = c.dev / (1.0 + e);
            CHECK(relErr(r.peakDeviationHz, want) < 0.01);
            CHECK(relErr(r.peakEstimateHz, want) < 0.04);   // the percentile reading is noise-limited
            CHECK(r.isTone);
            CHECK_NEAR(r.toneHz, 1000.0 / (1.0 + e), 5.0);
            CHECK_NEAR(r.carrierOffsetHz, 3000.0 / (1.0 + e), 0.01 * c.dev);
            CHECK_NEAR(r.rmsDeviationHz, want / std::sqrt(2.0), 0.02 * want);
            CHECK_NEAR(r.positivePeakHz, want, 0.03 * want);
            CHECK_NEAR(r.negativePeakHz, want, 0.03 * want);
            if (relErr(r.peakDeviationHz, want) >= 0.01) {
                std::printf("  dev %.0f got %.1f want %.1f\n", c.dev, r.peakDeviationHz, want);
            }
        }
    }

    // --- programme-like (two tones): not a tone, the percentile peak is the headline --------------
    {
        sg::Rng rng(22);
        const double rate = 96000.0;
        std::vector<Cf> x(96000);
        double ph = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            const double t = static_cast<double>(i) / rate;
            const double f = 4000.0 + 6000.0 * std::sin(sg::kTwoPi * 1000.0 * t) +
                             6000.0 * std::sin(sg::kTwoPi * 1730.0 * t + 0.7);
            ph += sg::kTwoPi * f / rate;
            x[i] = Cf(static_cast<float>(0.5 * std::cos(ph)), static_cast<float>(0.5 * std::sin(ph)));
        }
        sg::addNoise(x, 35.0, rng);
        ModParams p;
        p.audioBandwidthHz = 15000.0;
        const FmResult r = measureFmDeviation(x.data(), x.size(), rate, p);
        CHECK(r.valid);
        if (r.valid) {
            CHECK(!r.isTone);
            CHECK(r.peakDeviationHz > 11000.0 && r.peakDeviationHz < 12300.0);
            CHECK(r.peakDeviationHz == r.peakEstimateHz);
        }
    }

    // --- AM depth within 0.5 point; the FM-free carrier reads ~0 deviation ------------------------
    for (double m : {0.1, 0.3, 0.8}) {
        sg::Rng rng(23);
        auto x = sg::fmAm(48000, 48000.0, 2000.0, 0.0, 0.0, m, 1000.0, 0.4);
        sg::addNoise(x, 35.0, rng);
        const AmResult r = measureAmDepth(x.data(), x.size(), 48000.0);
        CHECK(r.valid);
        if (r.valid) {
            CHECK_NEAR(r.depth, m, 0.005);
            CHECK_NEAR(r.peakDepth, m, 0.025);   // noise-limited, see the header
            CHECK_NEAR(r.positiveDepth, m, 0.025);
            CHECK_NEAR(r.negativeDepth, m, 0.025);
            CHECK(r.isTone);
            CHECK(!r.overModulated);
            CHECK_NEAR(r.toneHz, 1000.0, 3.0);
            CHECK_NEAR(r.carrierLevel, 0.4, 0.01);
            if (std::fabs(r.depth - m) > 0.005) { std::printf("  am m=%.2f got %.4f\n", m, r.depth); }
        }
    }
    {
        // a pure FM signal has no envelope variation
        sg::Rng rng(24);
        auto x = sg::fmAm(48000, 48000.0, 1000.0, 1000.0, 3000.0, 0.0, 0.0, 0.4);
        sg::addNoise(x, 35.0, rng);
        const AmResult r = measureAmDepth(x.data(), x.size(), 48000.0);
        CHECK(r.valid);
        if (r.valid) { CHECK(r.peakDepth < 0.06); }
    }
    {
        // 100 % AM: negative peaks reach the zero line
        auto x = sg::fmAm(48000, 48000.0, 0.0, 0.0, 0.0, 1.0, 1000.0, 0.4);
        const AmResult r = measureAmDepth(x.data(), x.size(), 48000.0);
        CHECK(r.valid);
        if (r.valid) {
            CHECK(r.overModulated);
            CHECK_NEAR(r.peakDepth, 1.0, 0.02);
        }
    }

    // --- the percentile (programme) readings are tight when the noise is out of the way --------------
    {
        sg::Rng rng(27);
        auto x = sg::fmAm(96000, 96000.0, 2000.0, 1000.0, 12500.0, 0.3, 700.0, 0.4);
        sg::addNoise(x, 60.0, rng);
        const FmResult f = measureFmDeviation(x.data(), x.size(), 96000.0);
        const AmResult a = measureAmDepth(x.data(), x.size(), 96000.0);
        CHECK(f.valid && a.valid);
        if (f.valid && a.valid) {
            CHECK(relErr(f.peakEstimateHz, 12500.0) < 0.01);
            CHECK_NEAR(a.peakDepth, 0.3, 0.005);
        }
    }

    // --- FM and AM at once: each meter reads its own ----------------------------------------------
    {
        sg::Rng rng(25);
        auto x = sg::fmAm(96000, 96000.0, 1500.0, 1000.0, 5000.0, 0.5, 700.0, 0.4);
        sg::addNoise(x, 35.0, rng);
        const FmResult f = measureFmDeviation(x.data(), x.size(), 96000.0);
        const AmResult a = measureAmDepth(x.data(), x.size(), 96000.0);
        CHECK(f.valid && a.valid);
        if (f.valid && a.valid) {
            CHECK(relErr(f.peakDeviationHz, 5000.0) < 0.02);
            CHECK_NEAR(a.depth, 0.5, 0.01);
        }
    }

    // --- boundary: an unmodulated carrier ------------------------------------------------------------
    {
        sg::Rng rng(26);
        auto x = sg::fmAm(48000, 48000.0, 1000.0, 0.0, 0.0, 0.0, 0.0, 0.4);
        sg::addNoise(x, 50.0, rng);
        const FmResult f = measureFmDeviation(x.data(), x.size(), 48000.0);
        CHECK(f.valid);
        if (f.valid) { CHECK(f.peakEstimateHz < 150.0); CHECK_NEAR(f.carrierOffsetHz, 1000.0, 5.0); }
    }

    // --- failure paths ------------------------------------------------------------------------------
    {
        std::vector<Cf> x(4096, Cf(0.3f, 0.1f));
        CHECK(std::string(measureFmDeviation(nullptr, 0, 48000.0).reason) == why::kEmpty);
        CHECK(std::string(measureAmDepth(nullptr, 0, 48000.0).reason) == why::kEmpty);
        CHECK(std::string(measureFmDeviation(x.data(), x.size(), 0.0).reason) == why::kBadRate);
        CHECK(std::string(measureAmDepth(x.data(), x.size(), std::nan("")).reason) == why::kBadRate);
        CHECK(std::string(measureFmDeviation(x.data(), 100, 48000.0).reason) == why::kTooShort);
        CHECK(std::string(measureAmDepth(x.data(), 100, 48000.0).reason) == why::kTooShort);
        std::vector<Cf> z(4096, Cf(0.0f, 0.0f));
        CHECK(std::string(measureFmDeviation(z.data(), z.size(), 48000.0).reason) == why::kNoSignal);
        CHECK(std::string(measureAmDepth(z.data(), z.size(), 48000.0).reason) == why::kNoSignal);
        ModParams bad;
        bad.audioBandwidthHz = -1.0;
        CHECK(std::string(measureFmDeviation(x.data(), x.size(), 48000.0, bad).reason) == why::kBadParam);
        bad = ModParams{};
        bad.peakPercentile = 0.6;
        CHECK(std::string(measureAmDepth(x.data(), x.size(), 48000.0, bad).reason) == why::kBadParam);
        x[7] = Cf(std::numeric_limits<float>::infinity(), 0.0f);
        CHECK(std::string(measureFmDeviation(x.data(), x.size(), 48000.0).reason) == why::kNonFinite);
        x[7] = Cf(std::nanf(""), 0.0f);
        CHECK(std::string(measureAmDepth(x.data(), x.size(), 48000.0).reason) == why::kNonFinite);
    }
    return testSummary("test_sigmeas_modulation");
}
