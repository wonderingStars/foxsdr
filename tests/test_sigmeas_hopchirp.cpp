// Tests for dsp/sigmeas_hopchirp.hpp: frequency-hopping and chirp tracking, pinned on generated
// signals with noise, a frequency offset and a 300 ppm clock error.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/sigmeas_hopchirp.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "sigmeas_gen.hpp"
#include "test_check.hpp"

using namespace cascade::sigmeas;

namespace {

double relErr(double got, double want) { return std::fabs(got - want) / want; }

// A hopper: channels f0 + c*spacing (c in 0..channels-1), `dwellS` per hop on a true clock, random
// channel each hop (never the same twice running), continuous phase.
std::vector<Cf> hopper(std::size_t n, double trueRate, double f0, double spacing, int channels,
                       double dwellS, double amp, sg::Rng& rng, std::vector<int>* seq = nullptr) {
    std::vector<Cf> x(n);
    double ph = 0.0;
    int ch = 0;
    long lastHop = -1;
    for (std::size_t i = 0; i < n; ++i) {
        const long hop = static_cast<long>(static_cast<double>(i) / trueRate / dwellS);
        if (hop != lastHop) {
            int nc;
            do { nc = static_cast<int>(rng.uni() * channels) % channels; } while (nc == ch && lastHop >= 0);
            ch = nc;
            lastHop = hop;
            if (seq) { seq->push_back(ch); }
        }
        ph += sg::kTwoPi * (f0 + ch * spacing) / trueRate;
        x[i] = Cf(static_cast<float>(amp * std::cos(ph)), static_cast<float>(amp * std::sin(ph)));
    }
    return x;
}

// A sawtooth chirp: f = fStart + slope * (t mod period) (slope may be negative), or a triangle.
std::vector<Cf> chirper(std::size_t n, double trueRate, double fStart, double slope, double period,
                        bool triangle, double amp) {
    std::vector<Cf> x(n);
    double ph = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / trueRate;
        double u = std::fmod(t, period);
        double f;
        if (triangle) {
            const double half = period / 2.0;
            f = u < half ? fStart + slope * u : fStart + slope * (period - u);
        } else {
            f = fStart + slope * u;
        }
        ph += sg::kTwoPi * f / trueRate;
        x[i] = Cf(static_cast<float>(amp * std::cos(ph)), static_cast<float>(amp * std::sin(ph)));
    }
    return x;
}

}  // namespace

int main() {
    const double rate = 48000.0;
    TrackParams tp;
    tp.frameSamples = 256;

    // --- hopping: 8 channels 2 kHz apart from -7 kHz, 20 ms dwell (50 hops/s) ------------------------------
    {
        sg::Rng rng(61);
        const double e = 300e-6;
        const double dwell = 0.020;
        auto x = hopper(static_cast<std::size_t>(rate * 2.0), rate * (1.0 + e), -7000.0, 2000.0, 8, dwell, 0.4, rng);
        sg::addNoise(x, 15.0, rng);
        const FreqTrack t = trackPeak(x.data(), x.size(), rate, tp);
        CHECK(t.valid);
        CHECK(t.frameSamples == 256 && std::fabs(t.binHz - 187.5) < 1e-9);
        CHECK(t.okCount > t.freqHz.size() * 9 / 10);
        const HopReport h = analyseHops(t);
        CHECK(h.valid);
        if (h.valid) {
            // the hop RATE seen on the nominal clock: dwell lasts dwell/(1+e)... the converter is fast
            // by (1+e) so a dwell of `dwell` true seconds is dwell*(1+e) nominal seconds
            const double wantRate = 1.0 / (dwell * (1.0 + e));
            CHECK(relErr(h.hopRateHz, wantRate) < 0.02);
            CHECK(relErr(h.dwellS, dwell * (1.0 + e)) < 0.02);
            CHECK(h.hopCount >= 90 && h.hopCount <= 100);
            CHECK(h.distinctFreqs.size() == 8);
            if (h.distinctFreqs.size() == 8) {
                for (int c = 0; c < 8; ++c) {
                    CHECK_NEAR(h.distinctFreqs[static_cast<std::size_t>(c)], (-7000.0 + 2000.0 * c) / (1.0 + e), t.binHz);
                }
            }
            CHECK_NEAR(h.channelSpacingHz, 2000.0 / (1.0 + e), t.binHz);
            if (relErr(h.hopRateHz, wantRate) >= 0.02) { std::printf("  hop rate %.3f want %.3f\n", h.hopRateHz, wantRate); }
        } else {
            std::printf("  hop refused: %s\n", h.reason);
        }
    }
    // --- hopping at a different rate: 250 hops/s on 4 channels ---------------------------------------------------
    {
        sg::Rng rng(62);
        auto x = hopper(static_cast<std::size_t>(rate * 1.0), rate, 1000.0, 3000.0, 4, 0.004, 0.4, rng);
        sg::addNoise(x, 20.0, rng);
        const FreqTrack t = trackPeak(x.data(), x.size(), rate, tp);
        const HopReport h = analyseHops(t);
        CHECK(h.valid);
        if (h.valid) { CHECK(relErr(h.hopRateHz, 250.0) < 0.02); }
    }
    // --- chirps: sawtooth up and down, and a triangle ---------------------------------------------------------------
    {
        sg::Rng rng(63);
        const double e = 300e-6;
        const double period = 0.100, slope = 160000.0;   // -8 .. +8 kHz in 100 ms
        auto x = chirper(static_cast<std::size_t>(rate * 1.0), rate * (1.0 + e), -8000.0, slope, period, false, 0.4);
        sg::addNoise(x, 15.0, rng);
        const FreqTrack t = trackPeak(x.data(), x.size(), rate, tp);
        const ChirpReport c = analyseChirps(t);
        CHECK(c.valid);
        if (c.valid) {
            // the nominal-clock view: times scale by (1+e), frequencies by 1/(1+e), so the slope by 1/(1+e)^2
            const double wantSlope = slope / ((1.0 + e) * (1.0 + e));
            CHECK(relErr(c.slopeHzPerS, wantSlope) < 0.02);
            CHECK(c.direction == 1);
            CHECK(c.sweeps.size() >= 8 && c.sweeps.size() <= 10);
            CHECK(relErr(c.periodS, period * (1.0 + e)) < 0.01);
            CHECK(relErr(c.widthHz, 16000.0 / (1.0 + e)) < 0.08);   // the ends of a sweep are blunted by the window
            if (relErr(c.slopeHzPerS, wantSlope) >= 0.02 || !(relErr(c.periodS, period * (1.0 + e)) < 0.01)) {
                std::printf("  chirp slope %.0f want %.0f period %.5f\n", c.slopeHzPerS, wantSlope, c.periodS);
            }
        } else {
            std::printf("  chirp refused: %s\n", c.reason);
        }
        // down-chirp
        auto y = chirper(static_cast<std::size_t>(rate * 1.0), rate, 8000.0, -slope, period, false, 0.4);
        sg::addNoise(y, 15.0, rng);
        const ChirpReport d = analyseChirps(trackPeak(y.data(), y.size(), rate, tp));
        CHECK(d.valid && d.direction == -1);
        if (d.valid) { CHECK(relErr(-d.slopeHzPerS, slope) < 0.02); }
        // triangle: both directions are found
        auto z = chirper(static_cast<std::size_t>(rate * 1.0), rate, -6000.0, 240000.0, 0.100, true, 0.4);
        sg::addNoise(z, 15.0, rng);
        const ChirpReport tr = analyseChirps(trackPeak(z.data(), z.size(), rate, tp));
        CHECK(tr.valid && tr.direction == 0);
        if (tr.valid) { CHECK(tr.sweeps.size() >= 15); }
    }
    // --- a steady tone is neither a hopper nor a chirp ------------------------------------------------------------------
    {
        sg::Rng rng(64);
        auto x = chirper(static_cast<std::size_t>(rate * 1.0), rate, 2000.0, 0.0, 0.1, false, 0.4);
        sg::addNoise(x, 20.0, rng);
        const FreqTrack t = trackPeak(x.data(), x.size(), rate, tp);
        CHECK(t.valid);
        const HopReport h = analyseHops(t);
        CHECK(!h.valid && std::string(h.reason) == why::kNoStructure);
        const ChirpReport c = analyseChirps(t);
        CHECK(!c.valid && std::string(c.reason) == why::kNoStructure);
    }
    // --- noise only: nothing to track -------------------------------------------------------------------------------------
    {
        sg::Rng rng(65);
        std::vector<Cf> nz(48000);
        for (Cf& c : nz) { c = Cf(static_cast<float>(0.1 * rng.gauss()), static_cast<float>(0.1 * rng.gauss())); }
        TrackParams strict = tp;
        strict.minSnrDb = 18.0;
        const FreqTrack t = trackPeak(nz.data(), nz.size(), rate, strict);
        CHECK(t.valid);
        CHECK(t.okCount < t.freqHz.size() / 20);
        CHECK(!analyseHops(t).valid);
        CHECK(!analyseChirps(t).valid);
    }
    // --- failure paths -------------------------------------------------------------------------------------------------------
    {
        std::vector<Cf> x(8192, Cf(0.1f, 0.1f));
        CHECK(std::string(trackPeak(nullptr, 0, rate).reason) == why::kEmpty);
        CHECK(std::string(trackPeak(x.data(), x.size(), -1.0).reason) == why::kBadRate);
        CHECK(std::string(trackPeak(x.data(), 100, rate).reason) == why::kTooShort);
        TrackParams bad;
        bad.frameSamples = 300;   // not a power of two
        CHECK(std::string(trackPeak(x.data(), x.size(), rate, bad).reason) == why::kBadParam);
        bad = TrackParams{};
        bad.frameSamples = 4096;
        CHECK(std::string(trackPeak(x.data(), 3000, rate, bad).reason) == why::kTooShort);
        bad = TrackParams{};
        bad.minSnrDb = -1.0;
        CHECK(std::string(trackPeak(x.data(), x.size(), rate, bad).reason) == why::kBadParam);
        x[9] = Cf(std::nanf(""), 0.0f);
        CHECK(std::string(trackPeak(x.data(), x.size(), rate).reason) == why::kNonFinite);
        x[9] = Cf(0.0f, std::numeric_limits<float>::infinity());
        CHECK(std::string(trackPeak(x.data(), x.size(), rate).reason) == why::kNonFinite);
        // analysis of an invalid track passes its reason on; bad parameters
        const FreqTrack none;
        CHECK(!analyseHops(none).valid && !analyseChirps(none).valid);
        sg::Rng rng(66);
        auto h = hopper(48000, rate, 0.0, 2000.0, 4, 0.02, 0.4, rng);
        const FreqTrack t = trackPeak(h.data(), h.size(), rate, tp);
        HopParams hp;
        hp.tolHz = -1.0;
        CHECK(std::string(analyseHops(t, hp).reason) == why::kBadParam);
        hp = HopParams{};
        hp.minFrames = 0;
        CHECK(std::string(analyseHops(t, hp).reason) == why::kBadParam);
        ChirpParams cp;
        cp.minFrames = 2;
        CHECK(std::string(analyseChirps(t, cp).reason) == why::kBadParam);
        cp = ChirpParams{};
        cp.minSweepBins = 0.0;
        CHECK(std::string(analyseChirps(t, cp).reason) == why::kBadParam);
    }
    return testSummary("test_sigmeas_hopchirp");
}
