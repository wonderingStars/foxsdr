// Tests for dsp/sigmeas_vstime.hpp: amplitude, frequency and phase against time.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/sigmeas_vstime.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "sigmeas_gen.hpp"
#include "test_check.hpp"

using namespace cascade::sigmeas;

namespace {
double maxOf(const std::vector<double>& v, std::size_t from, std::size_t to) {
    double m = -1e300;
    for (std::size_t i = from; i < to && i < v.size(); ++i) { m = std::max(m, v[i]); }
    return m;
}
double minOf(const std::vector<double>& v, std::size_t from, std::size_t to) {
    double m = 1e300;
    for (std::size_t i = from; i < to && i < v.size(); ++i) { m = std::min(m, v[i]); }
    return m;
}
}  // namespace

int main() {
    const double rate = 192000.0;
    // --- frequency vs time of an FM signal: the track is carrier + deviation * sin, in Hz -------------
    {
        sg::Rng rng(41);
        auto x = sg::fmAm(192000, rate, 20000.0, 500.0, 3000.0, 0.0, 0.0, 0.5);
        sg::addNoise(x, 40.0, rng);
        VsTimeParams p;
        p.centreHz = 20000.0;
        p.bandwidthHz = 16000.0;  // covers 20 kHz +- 3 kHz with room
        const VsTimeResult r = computeVsTime(x.data(), x.size(), rate, p);
        CHECK(r.valid);
        if (r.valid) {
            CHECK(r.decimation == 4);  // floor(192000 / (2.5 * 16000)) = 4
            CHECK_NEAR(r.rateOut, 48000.0, 1e-9);
            CHECK(r.freqHz.size() == r.amplitude.size() && r.phaseRad.size() == r.freqHz.size());
            const std::size_t a = 50, b = r.freqHz.size() - 50;  // skip the filter's edges
            CHECK_NEAR(maxOf(r.freqHz, a, b), 23000.0, 60.0);
            CHECK_NEAR(minOf(r.freqHz, a, b), 17000.0, 60.0);
            CHECK_NEAR(r.meanFreqHz, 20000.0, 15.0);
            double lo = 9, hi = 0;
            for (std::size_t k = a; k < b; ++k) {
                lo = std::min<double>(lo, r.amplitude[k]);
                hi = std::max<double>(hi, r.amplitude[k]);
            }
            CHECK_NEAR(hi, 0.5, 0.02);
            CHECK_NEAR(lo, 0.5, 0.02);
            CHECK_NEAR(r.amplitudeDb[r.amplitudeDb.size() / 2], -6.02, 0.3);
            CHECK_NEAR(r.stepS, 4.0 / rate, 1e-15);
            CHECK_NEAR(r.startS, 3.0 / rate, 1e-15);
        }
    }
    // --- amplitude vs time of an AM signal, and the phase of a carrier --------------------------------
    {
        auto x = sg::fmAm(96000, 96000.0, 5000.0, 0.0, 0.0, 0.6, 400.0, 0.4);
        VsTimeParams p;
        p.centreHz = 5000.0;
        p.bandwidthHz = 4000.0;
        const VsTimeResult r = computeVsTime(x.data(), x.size(), 96000.0, p);
        CHECK(r.valid);
        if (r.valid) {
            double lo = 9, hi = 0;
            for (std::size_t k = 20; k + 20 < r.amplitude.size(); ++k) {
                lo = std::min<double>(lo, r.amplitude[k]);
                hi = std::max<double>(hi, r.amplitude[k]);
            }
            CHECK_NEAR(hi, 0.4 * 1.6, 0.01);
            CHECK_NEAR(lo, 0.4 * 0.4, 0.01);
            // a clean carrier on the centre has a flat phase: after removing the line the residual is tiny
            VsTimeParams q = p;
            q.removeLinearPhase = true;
            const VsTimeResult r2 = computeVsTime(x.data(), x.size(), 96000.0, q);
            double worst = 0;
            for (std::size_t k = 20; k + 20 < r2.phaseRad.size(); ++k) {
                worst = std::max(worst, std::fabs(r2.phaseRad[k]));
            }
            CHECK(worst < 0.02);
        }
    }
    // --- phase of a linear chirp: phi(t) = 2 pi (f0 t + k t^2 / 2), unwrapped over many turns ----------
    {
        const double fs = 48000.0, f0 = 1000.0, k = 8000.0;  // sweeps 1 -> 9 kHz in 1 s
        std::vector<Cf> x(48000);
        for (std::size_t i = 0; i < x.size(); ++i) {
            const double t = static_cast<double>(i) / fs;
            const double ph = sg::kTwoPi * (f0 * t + 0.5 * k * t * t);
            x[i] = Cf(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
        }
        VsTimeParams p;
        const VsTimeResult r = computeVsTime(x.data(), x.size(), fs, p);
        CHECK(r.valid);
        if (r.valid) {
            CHECK(r.decimation == 1);
            const std::size_t m = 30000;
            const double t = static_cast<double>(m) / fs;
            const double want = sg::kTwoPi * (f0 * t + 0.5 * k * t * t);
            CHECK_NEAR(r.phaseRad[m] - r.phaseRad[0], want, 0.05);
            CHECK_NEAR(r.freqHz[m], f0 + k * t, 20.0);
        }
        VsTimeParams q;
        q.maxPoints = 1000;
        const VsTimeResult r2 = computeVsTime(x.data(), x.size(), fs, q);
        CHECK(r2.valid);
        if (r2.valid) { CHECK(r2.amplitude.size() <= 1000 && r2.decimation >= 48); }
    }
    // --- failure paths ---------------------------------------------------------------------------------
    {
        std::vector<Cf> x(4096, Cf(0.2f, 0.1f));
        CHECK(std::string(computeVsTime(nullptr, 0, rate).reason) == why::kEmpty);
        CHECK(std::string(computeVsTime(x.data(), x.size(), 0.0).reason) == why::kBadRate);
        CHECK(std::string(computeVsTime(x.data(), 8, rate).reason) == why::kTooShort);
        VsTimeParams p;
        p.centreHz = rate;  // outside +-rate/2
        CHECK(std::string(computeVsTime(x.data(), x.size(), rate, p).reason) == why::kBadParam);
        p = VsTimeParams{};
        p.bandwidthHz = -1.0;
        CHECK(std::string(computeVsTime(x.data(), x.size(), rate, p).reason) == why::kBadParam);
        p = VsTimeParams{};
        p.bandwidthHz = 40.0;  // D = 1920: more than a quarter of the span
        CHECK(std::string(computeVsTime(x.data(), x.size(), rate, p).reason) == why::kTooShort);
        x[5] = Cf(std::nanf(""), 0.0f);
        CHECK(std::string(computeVsTime(x.data(), x.size(), rate).reason) == why::kNonFinite);
        x[5] = Cf(0.0f, std::numeric_limits<float>::infinity());
        CHECK(std::string(computeVsTime(x.data(), x.size(), rate).reason) == why::kNonFinite);
    }
    // --- boundary: silence has zero amplitude and a finite dB floor, not NaN --------------------------------
    {
        std::vector<Cf> z(1000, Cf(0, 0));
        const VsTimeResult r = computeVsTime(z.data(), z.size(), 48000.0);
        CHECK(r.valid);
        if (r.valid) {
            CHECK(r.amplitude[10] == 0.0f);
            CHECK(std::isfinite(r.amplitudeDb[10]) && r.amplitudeDb[10] <= -200.0f);
            CHECK(std::isfinite(r.freqHz[10]));
        }
    }
    return testSummary("test_sigmeas_vstime");
}
