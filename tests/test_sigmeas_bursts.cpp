// Tests for dsp/sigmeas_bursts.hpp: burst detector and burst list. Times pinned within one block
// (and, for flat-top bursts, the finer figure the edge-share method gives), on generated bursts in
// noise with a frequency offset and a clock error.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/sigmeas_bursts.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "sigmeas_gen.hpp"
#include "test_check.hpp"

using namespace cascade::sigmeas;

namespace {

struct Spec { double startS, lenS; };

// bursts of a tone at fo (amplitude amp) on a true clock, in complex noise of power noiseP
std::vector<Cf> makeBursts(std::size_t n, double trueRate, double fo, double amp, double noiseP,
                           const std::vector<Spec>& spec, sg::Rng& rng) {
    std::vector<Cf> x(n);
    const double s = std::sqrt(noiseP / 2.0);
    double ph = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / trueRate;
        bool on = false;
        for (const Spec& b : spec) { if (t >= b.startS && t < b.startS + b.lenS) { on = true; break; } }
        ph += sg::kTwoPi * fo / trueRate;
        const double a = on ? amp : 0.0;
        x[i] = Cf(static_cast<float>(a * std::cos(ph) + s * rng.gauss()),
                  static_cast<float>(a * std::sin(ph) + s * rng.gauss()));
    }
    return x;
}

}  // namespace

int main() {
    const double rate = 48000.0;
    // --- a regular train: 12 bursts of 5 ms every 50 ms, tone at +6 kHz, 20 dB over the noise -------------
    {
        sg::Rng rng(51);
        const double e = 300e-6;
        std::vector<Spec> spec;
        for (int k = 0; k < 12; ++k) { spec.push_back({0.013 + 0.050 * k, 0.005}); }
        auto x = makeBursts(static_cast<std::size_t>(rate * 0.7), rate * (1.0 + e), 6000.0, 0.3, 0.0009, spec, rng);
        const BurstReport r = detectBursts(x.data(), x.size(), rate);
        CHECK(r.valid);
        if (r.valid) {
            const double W = static_cast<double>(r.blockSamples) / rate;
            CHECK(r.blockSamples == 16);
            CHECK(r.totalBursts == 12 && r.bursts.size() == 12);
            const std::size_t m = std::min<std::size_t>(r.bursts.size(), 12);
            double worstStart = 0, worstLen = 0;
            for (std::size_t k = 0; k < m; ++k) {
                // a converter running (1+e) fast puts the burst at sample start*rate*(1+e): times read x (1+e)
                worstStart = std::max(worstStart, std::fabs(r.bursts[k].startS - spec[k].startS * (1.0 + e)));
                worstLen = std::max(worstLen, std::fabs(r.bursts[k].lengthS - spec[k].lenS * (1.0 + e)));
                CHECK_NEAR(r.bursts[k].freqHz, 6000.0 / (1.0 + e), 120.0);
                CHECK_NEAR(r.bursts[k].powerDbfs, 10.0 * std::log10(0.09 + 0.0009), 0.5);
            }
            CHECK(worstStart < W);       // the pinned requirement: within one block
            CHECK(worstLen < W);
            CHECK(worstStart < 0.5 * W); // and the finer figure
            CHECK(worstLen < 0.5 * W);
            CHECK(std::fabs(r.repeatPeriodS - 0.050 * (1.0 + e)) < 0.0005);
            CHECK(r.repeatJitter < 0.02);
            CHECK_NEAR(r.dutyCycle, 12.0 * 0.005 / 0.7, 0.01);
            CHECK_NEAR(r.noiseDbfs, 10.0 * std::log10(0.0009), 1.0);
            CHECK(r.levelDbfs > r.noiseDbfs + 15.0);
            if (worstStart >= 0.5 * W || worstLen >= 0.5 * W) {
                std::printf("  worst start %.6f len %.6f block %.6f\n", worstStart, worstLen, W);
            }
        }
    }
    // --- irregular bursts of different length and frequency-agile (hop between bursts) ----------------------
    {
        sg::Rng rng(52);
        const std::vector<Spec> spec = {{0.020, 0.002}, {0.050, 0.010}, {0.110, 0.0035}, {0.150, 0.030}, {0.230, 0.010}};
        auto x = makeBursts(static_cast<std::size_t>(rate * 0.3), rate, -9000.0, 0.2, 0.0004, spec, rng);
        const BurstReport r = detectBursts(x.data(), x.size(), rate);
        CHECK(r.valid);
        if (r.valid) {
            CHECK(r.bursts.size() == 5);
            for (std::size_t k = 0; k < std::min<std::size_t>(5, r.bursts.size()); ++k) {
                CHECK(std::fabs(r.bursts[k].startS - spec[k].startS) < 16.0 / rate);
                CHECK(std::fabs(r.bursts[k].lengthS - spec[k].lenS) < 16.0 / rate);
                CHECK_NEAR(r.bursts[k].freqHz, -9000.0, 100.0);
            }
            CHECK(r.repeatJitter > 0.1);   // not regular
        }
    }
    // --- boundary: one burst (no repeat period), a burst starting at sample 0 and one cut by the end ----
    {
        sg::Rng rng(53);
        auto x = makeBursts(static_cast<std::size_t>(rate * 0.2), rate, 3000.0, 0.3, 0.0009, {{0.100, 0.020}}, rng);
        const BurstReport r = detectBursts(x.data(), x.size(), rate);
        CHECK(r.valid && r.bursts.size() == 1);
        CHECK(std::isnan(r.repeatPeriodS));
        auto y = makeBursts(static_cast<std::size_t>(rate * 0.2), rate, 3000.0, 0.3, 0.0009,
                            {{0.0, 0.020}, {0.150, 0.100}}, rng);
        const BurstReport r2 = detectBursts(y.data(), y.size(), rate);
        CHECK(r2.valid && r2.bursts.size() == 2);
        if (r2.bursts.size() != 2) {
            std::printf("  boundary: valid=%d bursts=%zu reason=%s\n", r2.valid ? 1 : 0, r2.bursts.size(), r2.reason);
        }
        if (r2.bursts.size() == 2) {
            CHECK(r2.bursts[0].startSample < 16);
            CHECK(r2.bursts[1].startSample + r2.bursts[1].lengthSamples <= y.size() + 16);
        }
    }
    // --- a one-block fade inside a burst does not split it; a one-block spike is not a burst --------------------
    {
        sg::Rng rng(54);
        auto x = makeBursts(static_cast<std::size_t>(rate * 0.2), rate, 3000.0, 0.3, 0.0009, {{0.050, 0.040}}, rng);
        const std::size_t fadeAt = static_cast<std::size_t>(rate * 0.070);
        for (std::size_t i = fadeAt; i < fadeAt + 16; ++i) { x[i] = Cf(0.0f, 0.0f); }
        const std::size_t spikeAt = static_cast<std::size_t>(rate * 0.150);
        for (std::size_t i = spikeAt; i < spikeAt + 16; ++i) { x[i] = Cf(0.3f, 0.0f); }
        const BurstReport r = detectBursts(x.data(), x.size(), rate);
        CHECK(r.valid && r.bursts.size() == 1);
        BurstParams p;
        p.minBurstBlocks = 1;   // now the spike counts
        const BurstReport r2 = detectBursts(x.data(), x.size(), rate, p);
        CHECK(r2.valid && r2.bursts.size() == 2);
        p = BurstParams{};
        p.minGapBlocks = 0;     // and with no bridging the fade splits the burst
        const BurstReport r3 = detectBursts(x.data(), x.size(), rate, p);
        CHECK(r3.valid && r3.bursts.size() == 2);
    }
    // --- the list is capped but the count is not --------------------------------------------------------------
    {
        sg::Rng rng(55);
        std::vector<Spec> spec;
        for (int k = 0; k < 20; ++k) { spec.push_back({0.010 + 0.020 * k, 0.005}); }
        auto x = makeBursts(static_cast<std::size_t>(rate * 0.45), rate, 1000.0, 0.3, 0.0009, spec, rng);
        BurstParams p;
        p.maxBursts = 5;
        const BurstReport r = detectBursts(x.data(), x.size(), rate, p);
        CHECK(r.valid && r.totalBursts == 20 && r.bursts.size() == 5);
        // an explicit block length is honoured
        p = BurstParams{};
        p.blockSamples = 48;
        const BurstReport r2 = detectBursts(x.data(), x.size(), rate, p);
        CHECK(r2.valid && r2.blockSamples == 48 && r2.totalBursts == 20);
    }
    // --- no structure: a steady carrier, noise only ---------------------------------------------------------
    {
        sg::Rng rng(56);
        auto cw = makeBursts(static_cast<std::size_t>(rate * 0.2), rate, 3000.0, 0.3, 0.0009, {{-1.0, 10.0}}, rng);
        const BurstReport a = detectBursts(cw.data(), cw.size(), rate);
        CHECK(!a.valid && a.continuous && std::string(a.reason) == why::kNoStructure);
        std::vector<Cf> nz(9600);
        for (Cf& c : nz) { c = Cf(static_cast<float>(0.03 * rng.gauss()), static_cast<float>(0.03 * rng.gauss())); }
        const BurstReport b = detectBursts(nz.data(), nz.size(), rate);
        CHECK(!b.valid && std::string(b.reason) == why::kNoStructure);
        std::vector<Cf> zero(9600, Cf(0, 0));
        const BurstReport z = detectBursts(zero.data(), zero.size(), rate);
        CHECK(!z.valid);
    }
    // --- failure paths ---------------------------------------------------------------------------------------
    {
        std::vector<Cf> x(9600, Cf(0.1f, 0.0f));
        CHECK(std::string(detectBursts(nullptr, 0, rate).reason) == why::kEmpty);
        CHECK(std::string(detectBursts(x.data(), x.size(), 0.0).reason) == why::kBadRate);
        CHECK(std::string(detectBursts(x.data(), 40, rate).reason) == why::kTooShort);
        CHECK(std::string(detectBursts(x.data(), 100, rate).reason) == why::kTooShort);   // < 8 blocks
        BurstParams p;
        p.minContrastDb = 0.0;
        CHECK(std::string(detectBursts(x.data(), x.size(), rate, p).reason) == why::kBadParam);
        p = BurstParams{};
        p.minBurstBlocks = 0;
        CHECK(std::string(detectBursts(x.data(), x.size(), rate, p).reason) == why::kBadParam);
        p = BurstParams{};
        p.maxBursts = 0;
        CHECK(std::string(detectBursts(x.data(), x.size(), rate, p).reason) == why::kBadParam);
        x[50] = Cf(std::nanf(""), 0.0f);
        CHECK(std::string(detectBursts(x.data(), x.size(), rate).reason) == why::kNonFinite);
        x[50] = Cf(std::numeric_limits<float>::infinity(), 0.0f);
        CHECK(std::string(detectBursts(x.data(), x.size(), rate).reason) == why::kNonFinite);
    }
    return testSummary("test_sigmeas_bursts");
}
