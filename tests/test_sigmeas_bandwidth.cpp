// Tests for dsp/sigmeas_bandwidth.hpp: occupied (99 %) and x-dB bandwidth, centre, SNR.
// Accuracy pinned against generated signals whose power spectrum is known exactly (multisines
// with a given shape), with noise, a frequency offset and a sample-rate error.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/sigmeas_bandwidth.hpp"

#include <cmath>
#include <limits>
#include <vector>

#include "sigmeas_gen.hpp"
#include "test_check.hpp"

using namespace cascade::sigmeas;

namespace {

constexpr double kRate = 48000.0;
constexpr std::size_t kN = 65536;

std::vector<Cf> flat(double bw, double centre, double snrDb, std::uint64_t seed) {
    sg::Rng rng(seed);
    auto x = sg::multisine(kN, kRate, [&](double f) { return std::fabs(f) <= bw / 2.0 ? 1.0 : 0.0; },
                           centre, rng);
    sg::addNoise(x, snrDb, rng);
    return x;
}

std::vector<Cf> gaussian(double sigma, double centre, double snrDb, std::uint64_t seed) {
    sg::Rng rng(seed);
    auto x = sg::multisine(kN, kRate,
                           [&](double f) { return std::exp(-0.5 * f * f / (sigma * sigma)); }, centre, rng);
    sg::addNoise(x, snrDb, rng);
    return x;
}

double relErr(double got, double want) { return std::fabs(got - want) / want; }

}  // namespace

int main() {
    // --- happy path: flat spectrum, known 99 %-power bandwidth = 0.99 B ------------------------
    for (double bw : {6000.0, 10000.0, 16000.0}) {
        const auto x = flat(bw, 5000.0, 20.0, 11);
        const BandwidthResult r = measureBandwidth(x.data(), x.size(), kRate);
        CHECK(r.valid);
        if (r.valid) {
            CHECK(relErr(r.obwHz, 0.99 * bw) < 0.02);
            CHECK_NEAR(r.centreHz, 5000.0, 60.0);
            CHECK(r.floorReliable);
            if (relErr(r.obwHz, 0.99 * bw) >= 0.02) {
                std::printf("  flat bw=%.0f obw=%.1f want %.1f\n", bw, r.obwHz, 0.99 * bw);
            }
        }
    }

    // --- Gaussian spectrum: OBW99 = 2 * 2.5758 sigma; x-dB widths = 2 sigma sqrt(2 ln 10^(x/10)) -
    {
        const double sigma = 1500.0;
        const auto x = gaussian(sigma, -7000.0, 20.0, 12);
        const BandwidthResult r = measureBandwidth(x.data(), x.size(), kRate);
        CHECK(r.valid);
        if (r.valid) {
            CHECK(relErr(r.obwHz, 5.1517 * sigma) < 0.02);
            CHECK_NEAR(r.centreHz, -7000.0, 60.0);
            CHECK_NEAR(r.peakHz, -7000.0, 500.0);  // a broad top: the strongest bin wanders
            // SNR: signal 0.99..1.0 of unit power over the noise in the OBW (0.01 over 48 kHz)
            const double noiseIn = 0.01 * r.obwHz / kRate;
            CHECK_NEAR(r.snrDb, 10.0 * std::log10(1.0 / noiseIn), 1.0);
            CHECK(r.xdb.size() == 4);
            for (const XdbEntry& e : r.xdb) {
                if (e.db == 3.0 || e.db == 6.0 || e.db == 20.0) {
                    CHECK(e.valid);
                    const double want = 2.0 * sigma * std::sqrt(2.0 * std::log(std::pow(10.0, e.db / 10.0)));
                    CHECK(relErr(e.widthHz, want) < 0.03);
                    if (relErr(e.widthHz, want) >= 0.03) {
                        std::printf("  xdb %.0f got %.1f want %.1f\n", e.db, e.widthHz, want);
                    }
                }
            }
        }
    }

    // --- 99 % is a parameter: 90 % of a Gaussian is 2 * 1.6449 sigma ---------------------------
    {
        const double sigma = 1500.0;
        const auto x = gaussian(sigma, 0.0, 25.0, 13);
        BandwidthParams p;
        p.obwFraction = 0.90;
        const BandwidthResult r = measureBandwidth(x.data(), x.size(), kRate, p);
        CHECK(r.valid);
        if (r.valid) { CHECK(relErr(r.obwHz, 3.2897 * sigma) < 0.02); }
    }

    // --- sample-rate error: the answer scales with the rate it is told --------------------------
    {
        const auto x = gaussian(1200.0, 3000.0, 25.0, 14);
        const BandwidthResult a = measureBandwidth(x.data(), x.size(), kRate);
        const BandwidthResult b = measureBandwidth(x.data(), x.size(), kRate * (1.0 + 300e-6));
        CHECK(a.valid && b.valid);
        if (a.valid && b.valid) {
            CHECK_NEAR(b.obwHz / a.obwHz, 1.0 + 300e-6, 2e-4);
            CHECK(relErr(a.obwHz, 5.1517 * 1200.0) < 0.02);
            CHECK(relErr(b.obwHz, 5.1517 * 1200.0) < 0.02);  // a 300 ppm clock is inside 2 %
        }
    }

    // --- a sub-band window selects one of two signals --------------------------------------------
    {
        sg::Rng rng(15);
        auto a = sg::multisine(kN, kRate, [](double f) { return std::fabs(f) <= 1500.0 ? 1.0 : 0.0; },
                               -9000.0, rng);
        auto b = sg::multisine(kN, kRate, [](double f) { return std::fabs(f) <= 2500.0 ? 1.0 : 0.0; },
                               8000.0, rng);
        std::vector<Cf> x(kN);
        for (std::size_t i = 0; i < kN; ++i) { x[i] = a[i] + b[i]; }
        sg::addNoise(x, 25.0, rng);
        BandwidthParams p;
        p.bandLowHz = 2000.0;
        p.bandHighHz = 20000.0;
        const BandwidthResult r = measureBandwidth(x.data(), x.size(), kRate, p);
        CHECK(r.valid);
        if (r.valid) {
            CHECK_NEAR(r.centreHz, 8000.0, 80.0);
            CHECK(relErr(r.obwHz, 0.99 * 5000.0) < 0.03);
        }
        p.bandLowHz = -20000.0;
        p.bandHighHz = -2000.0;
        const BandwidthResult r2 = measureBandwidth(x.data(), x.size(), kRate, p);
        CHECK(r2.valid);
        if (r2.valid) {
            CHECK_NEAR(r2.centreHz, -9000.0, 80.0);
            CHECK(relErr(r2.obwHz, 0.99 * 3000.0) < 0.03);
        }
    }

    // --- boundary: a pure tone has a handful of bins of bandwidth; a signal that fills the band is
    //     flagged as having an unreliable floor; an explicit floor is honoured ---------------------
    {
        std::vector<Cf> x(kN);
        for (std::size_t i = 0; i < kN; ++i) {
            const double ph = sg::kTwoPi * 3000.0 * static_cast<double>(i) / kRate;
            x[i] = Cf(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
        }
        sg::Rng rng(16);
        sg::addNoise(x, 40.0, rng);
        const BandwidthResult r = measureBandwidth(x.data(), x.size(), kRate);
        CHECK(r.valid);
        if (r.valid) {
            CHECK(r.obwHz < 6.0 * r.binHz);
            CHECK_NEAR(r.centreHz, 3000.0, 2.0 * r.binHz);
        }
    }
    {
        // A signal that fills the whole range cannot be told from its own floor: refused, with
        // the reason, unless the caller supplies the floor (here the known noise per bin).
        const auto x = flat(44000.0, 0.0, 30.0, 17);
        const BandwidthResult r = measureBandwidth(x.data(), x.size(), kRate);
        CHECK(!r.valid);
        CHECK(std::string(r.reason) == why::kNoSignal);
        BandwidthParams p;
        p.floorPower = 0.001 / 8192.0;
        const BandwidthResult r2 = measureBandwidth(x.data(), x.size(), kRate, p);
        CHECK(r2.valid);
        if (r2.valid) {
            CHECK(!r2.floorReliable);
            CHECK(r2.occupancy > 0.8);
            CHECK(relErr(r2.obwHz, 0.99 * 44000.0) < 0.03);
        }
    }

    // --- failure paths -------------------------------------------------------------------------
    {
        std::vector<Cf> none;
        CHECK(!measureBandwidth(none.data(), 0, kRate).valid);
        CHECK(std::string(measureBandwidth(nullptr, 0, kRate).reason) == why::kEmpty);

        std::vector<Cf> x(4096, Cf(0.1f, 0.1f));
        CHECK(std::string(measureBandwidth(x.data(), x.size(), 0.0).reason) == why::kBadRate);
        CHECK(std::string(measureBandwidth(x.data(), x.size(), -1.0).reason) == why::kBadRate);
        CHECK(std::string(measureBandwidth(x.data(), x.size(), std::nan("")).reason) == why::kBadRate);
        CHECK(std::string(measureBandwidth(x.data(), 10, kRate).reason) == why::kTooShort);

        x[100] = Cf(std::nanf(""), 0.0f);
        CHECK(std::string(measureBandwidth(x.data(), x.size(), kRate).reason) == why::kNonFinite);
        x[100] = Cf(0.0f, std::numeric_limits<float>::infinity());
        CHECK(std::string(measureBandwidth(x.data(), x.size(), kRate).reason) == why::kNonFinite);

        // noise only: no signal found
        sg::Rng rng(18);
        std::vector<Cf> nz(kN);
        for (Cf& c : nz) { c = Cf(static_cast<float>(rng.gauss()), static_cast<float>(rng.gauss())); }
        const BandwidthResult rn = measureBandwidth(nz.data(), nz.size(), kRate);
        CHECK(!rn.valid);
        CHECK(std::string(rn.reason) == why::kNoSignal);

        // all zeros: no signal, not a crash or a NaN result
        std::vector<Cf> zero(kN, Cf(0.0f, 0.0f));
        const BandwidthResult rz = measureBandwidth(zero.data(), zero.size(), kRate);
        CHECK(!rz.valid);

        // bad parameters
        const auto good = flat(8000.0, 0.0, 25.0, 19);
        BandwidthParams p;
        p.obwFraction = 0.4;
        CHECK(std::string(measureBandwidth(good.data(), good.size(), kRate, p).reason) == why::kBadParam);
        p = BandwidthParams{};
        p.obwFraction = 1.0;
        CHECK(!measureBandwidth(good.data(), good.size(), kRate, p).valid);
        p = BandwidthParams{};
        p.bandLowHz = 1000.0;
        p.bandHighHz = 1010.0;  // a window of a few bins
        CHECK(std::string(measureBandwidth(good.data(), good.size(), kRate, p).reason) == why::kBadParam);
        p = BandwidthParams{};
        p.nfft = 1000;  // not a power of two
        CHECK(std::string(measureBandwidth(good.data(), good.size(), kRate, p).reason) == why::kBadParam);
        p = BandwidthParams{};
        p.detectMarginDb = -3.0;
        CHECK(!measureBandwidth(good.data(), good.size(), kRate, p).valid);
    }

    // --- an x-dB level below the noise floor is reported invalid, not made up ------------------------
    {
        const auto x = gaussian(1500.0, 0.0, 12.0, 20);
        BandwidthParams p;
        p.xDb = {3.0, 60.0};
        const BandwidthResult r = measureBandwidth(x.data(), x.size(), kRate, p);
        CHECK(r.valid);
        if (r.valid) {
            CHECK(r.xdb.size() == 2);
            CHECK(r.xdb[0].valid);
            CHECK(!r.xdb[1].valid);
            CHECK(std::isnan(r.xdb[1].widthHz));
        }
    }

    return testSummary("test_sigmeas_bandwidth");
}
