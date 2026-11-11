// Tests for dsp/sigmeas_histogram.hpp: amplitude and probability histograms, magnitude histogram,
// power CCDF. Distributions with known shapes (Gaussian, uniform, sine, Rayleigh, complex Gaussian).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/sigmeas_histogram.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "sigmeas_gen.hpp"
#include "test_check.hpp"

using namespace cascade::sigmeas;

namespace {
double phi(double z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); }
}  // namespace

int main() {
    // --- Gaussian samples: every bin within 5 binomial sigmas of the normal integral -----------------------------------
    {
        sg::Rng rng(81);
        const std::size_t n = 400000;
        std::vector<float> x(n);
        for (float& v : x) { v = static_cast<float>(0.2 * rng.gauss()); }
        HistParams p;
        p.bins = 40;
        p.lo = -1.0;
        p.hi = 1.0;
        const HistResult h = amplitudeHistogram(x.data(), x.size(), p);
        CHECK(h.valid);
        if (h.valid) {
            CHECK(h.counts.size() == 40 && h.probability.size() == 40 && h.cdf.size() == 40);
            CHECK_NEAR(h.binWidth, 0.05, 1e-12);
            std::uint64_t sum = 0;
            for (auto c : h.counts) { sum += c; }
            CHECK(sum + h.belowRange + h.aboveRange == h.total);
            CHECK(h.total == n);
            int bad = 0;
            for (std::size_t b = 0; b < 40; ++b) {
                const double a = -1.0 + 0.05 * static_cast<double>(b), c = a + 0.05;
                const double want = phi(c / 0.2) - phi(a / 0.2);
                const double sd = std::sqrt(want * (1.0 - want) / static_cast<double>(n));
                if (std::fabs(h.probability[b] - want) > 5.0 * sd + 1e-6) { ++bad; }
                CHECK_NEAR(h.density[b], h.probability[b] / 0.05, 1e-12);
            }
            CHECK(bad == 0);
            CHECK_NEAR(h.mean, 0.0, 0.003);
            CHECK_NEAR(h.rms, 0.2, 0.002);
            CHECK(h.cdf.back() > 0.999999 && h.ccdf.back() < 1e-6 + 1e-12);
            CHECK_NEAR(h.cdf[19] + h.ccdf[19], 1.0, 1e-9);
            CHECK_NEAR(h.cdf[19], 0.5, 0.005);   // upper edge of bin 19 is 0.0
            CHECK(h.crestFactorDb > 10.0 && h.crestFactorDb < 15.0);   // Gaussian: ~ 4.7 sigma peak
        }
    }
    // --- uniform samples: flat, with the data range as the default range --------------------------------------------------
    {
        sg::Rng rng(82);
        std::vector<float> x(200000);
        for (float& v : x) { v = static_cast<float>(2.0 * rng.uni() - 1.0); }
        HistParams p;
        p.bins = 20;
        const HistResult h = amplitudeHistogram(x.data(), x.size(), p);
        CHECK(h.valid);
        if (h.valid) {
            CHECK(h.belowRange == 0 && h.aboveRange == 0);
            CHECK_NEAR(h.lo, -1.0, 2e-4);
            CHECK_NEAR(h.hi, 1.0, 2e-4);
            for (std::size_t b = 0; b < 20; ++b) { CHECK_NEAR(h.probability[b], 0.05, 0.004); }
        }
    }
    // --- a sine: the arcsine law, P(|x| > 0.9 A) = (2/pi) acos(0.9) ---------------------------------------------------------
    {
        std::vector<float> x(100000);
        for (std::size_t i = 0; i < x.size(); ++i) { x[i] = static_cast<float>(std::sin(sg::kTwoPi * 0.0123456 * static_cast<double>(i))); }
        HistParams p;
        p.bins = 100;
        p.lo = -1.0;
        p.hi = 1.0;
        const HistResult h = amplitudeHistogram(x.data(), x.size(), p);
        CHECK(h.valid);
        if (h.valid) {
            double tails = h.probability.front();
            for (std::size_t b = 0; b < 5; ++b) { tails += (b ? h.probability[b] : 0.0); }
            double tail = 0.0;
            for (std::size_t b = 0; b < 5; ++b) { tail += h.probability[b] + h.probability[99 - b]; }
            CHECK_NEAR(tail, 2.0 / sg::kPi * std::acos(0.9), 0.01);
            CHECK_NEAR(h.crestFactorDb, 3.0103, 0.05);
            (void)tails;
        }
    }
    // --- out of range, and non-finite, counted apart -------------------------------------------------------------------------------
    {
        std::vector<float> x = {0.0f, 0.1f, 0.2f, 5.0f, -5.0f, 0.3f, std::nanf(""), std::numeric_limits<float>::infinity()};
        HistParams p;
        p.bins = 4;
        p.lo = 0.0;
        p.hi = 0.4;
        const HistResult h = amplitudeHistogram(x.data(), x.size(), p);
        CHECK(h.valid);
        if (h.valid) {
            CHECK(h.total == 6 && h.nonFinite == 2);
            CHECK(h.belowRange == 1 && h.aboveRange == 1);
            CHECK(h.counts[0] == 1 && h.counts[1] == 1 && h.counts[2] == 1 && h.counts[3] == 1);
            CHECK_NEAR(h.probability[0], 1.0 / 6.0, 1e-12);
            CHECK_NEAR(h.cdf[0], (1.0 + 1.0) / 6.0, 1e-12);   // the mass below the range counts
            CHECK_NEAR(h.maxValue, 5.0, 1e-12);
        }
        // a value exactly on hi goes in the last bin (hi taken from the float, so it is exactly equal)
        p.hi = static_cast<double>(0.4f);
        std::vector<float> e = {0.0f, 0.4f};
        const HistResult he = amplitudeHistogram(e.data(), e.size(), p);
        CHECK(he.valid && he.counts[3] == 1 && he.aboveRange == 0);
    }
    // --- constant signal: a one-unit range, everything in the first bin ----------------------------------------------------------------
    {
        std::vector<float> x(100, 0.7f);
        HistParams p;
        p.bins = 8;
        const HistResult h = amplitudeHistogram(x.data(), x.size(), p);
        CHECK(h.valid && h.counts[0] == 100);
        CHECK_NEAR(h.crestFactorDb, 0.0, 1e-6);
        std::vector<float> z(100, 0.0f);
        const HistResult hz = amplitudeHistogram(z.data(), z.size(), p);
        CHECK(hz.valid && std::isnan(hz.crestFactorDb));
    }
    // --- magnitude histogram: Rayleigh -----------------------------------------------------------------------------------------------------
    {
        sg::Rng rng(83);
        const std::size_t n = 300000;
        std::vector<Cf> x(n);
        for (Cf& c : x) { c = Cf(static_cast<float>(rng.gauss()), static_cast<float>(rng.gauss())); }   // sigma = 1 per axis
        HistParams p;
        p.bins = 30;
        p.lo = 0.0;
        p.hi = 4.5;
        const HistResult h = magnitudeHistogram(x.data(), x.size(), p);
        CHECK(h.valid);
        if (h.valid) {
            int bad = 0;
            for (std::size_t b = 0; b < 30; ++b) {
                const double a = 0.15 * static_cast<double>(b), c = a + 0.15;
                const double want = std::exp(-a * a / 2.0) - std::exp(-c * c / 2.0);
                const double sd = std::sqrt(want * (1.0 - want) / static_cast<double>(n));
                if (std::fabs(h.probability[b] - want) > 5.0 * sd + 1e-6) { ++bad; }
            }
            CHECK(bad == 0);
        }
        // default low edge for magnitudes is 0
        const HistResult d = magnitudeHistogram(x.data(), x.size());
        CHECK(d.valid && d.lo == 0.0);
    }
    // --- power CCDF -------------------------------------------------------------------------------------------------------------------------------
    {
        sg::Rng rng(84);
        const std::size_t n = 500000;
        std::vector<Cf> x(n);
        for (Cf& c : x) { c = Cf(static_cast<float>(rng.gauss()), static_cast<float>(rng.gauss())); }
        const CcdfResult r = powerCcdf(x.data(), x.size(), 10.0, 1.0);
        CHECK(r.valid);
        if (r.valid) {
            CHECK(r.probability.size() == 11 && r.dbAboveMean.size() == 11);
            for (std::size_t k : {0u, 3u, 6u, 9u}) {
                const double want = std::exp(-std::pow(10.0, r.dbAboveMean[k] / 10.0));
                CHECK(std::fabs(r.probability[k] - want) < 0.15 * want + 1e-4);
            }
            CHECK(r.paprDb > 9.0 && r.paprDb < 14.0);
        }
        // constant envelope: probability 1 at 0 dB and nothing above
        std::vector<Cf> cw(10000);
        for (std::size_t i = 0; i < cw.size(); ++i) {
            const double ph = 0.1 * static_cast<double>(i);
            cw[i] = Cf(static_cast<float>(0.5 * std::cos(ph)), static_cast<float>(0.5 * std::sin(ph)));
        }
        const CcdfResult c = powerCcdf(cw.data(), cw.size(), 5.0, 1.0);
        CHECK(c.valid);
        if (c.valid) {
            CHECK_NEAR(c.probability[0], 1.0, 1e-9);
            CHECK_NEAR(c.probability[1], 0.0, 1e-9);
            CHECK_NEAR(c.paprDb, 0.0, 1e-3);
            CHECK_NEAR(c.meanPowerDbfs, 10.0 * std::log10(0.25), 1e-3);
        }
    }
    // --- failure paths -----------------------------------------------------------------------------------------------------------------------------
    {
        std::vector<float> x(100, 0.1f);
        CHECK(std::string(amplitudeHistogram(nullptr, 0).reason) == why::kEmpty);
        CHECK(std::string(amplitudeHistogram(x.data(), 0).reason) == why::kEmpty);
        CHECK(std::string(magnitudeHistogram(nullptr, 0).reason) == why::kEmpty);
        HistParams p;
        p.bins = 0;
        CHECK(std::string(amplitudeHistogram(x.data(), x.size(), p).reason) == why::kBadParam);
        p.bins = 100000;
        CHECK(std::string(amplitudeHistogram(x.data(), x.size(), p).reason) == why::kBadParam);
        p = HistParams{};
        p.lo = 1.0;
        p.hi = 1.0;
        CHECK(std::string(amplitudeHistogram(x.data(), x.size(), p).reason) == why::kBadParam);
        p.lo = 2.0;
        p.hi = 1.0;
        CHECK(!amplitudeHistogram(x.data(), x.size(), p).valid);
        std::vector<float> nan(10, std::nanf(""));
        const HistResult hn = amplitudeHistogram(nan.data(), nan.size());
        CHECK(!hn.valid && hn.nonFinite == 10 && std::string(hn.reason) == why::kNonFinite);
        std::vector<Cf> c(10, Cf(std::nanf(""), 0.0f));
        CHECK(!magnitudeHistogram(c.data(), c.size()).valid);
        CHECK(std::string(powerCcdf(nullptr, 0).reason) == why::kEmpty);
        CHECK(std::string(powerCcdf(c.data(), c.size()).reason) == why::kNonFinite);
        std::vector<Cf> zero(100, Cf(0, 0));
        CHECK(std::string(powerCcdf(zero.data(), zero.size()).reason) == why::kNoSignal);
        std::vector<Cf> ok(100, Cf(0.1f, 0.0f));
        CHECK(std::string(powerCcdf(ok.data(), ok.size(), 10.0, 0.0).reason) == why::kBadParam);
        CHECK(std::string(powerCcdf(ok.data(), ok.size(), 1.0, 2.0).reason) == why::kBadParam);
        CHECK(std::string(powerCcdf(ok.data(), ok.size(), std::nan(""), 1.0).reason) == why::kBadParam);
    }
    return testSummary("test_sigmeas_histogram");
}
