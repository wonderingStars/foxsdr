// Tests for dsp/sigmeas_audio.hpp: SINAD, THD, THD+N, SFDR, ENOB against a synthetic tone with a
// known distortion and a known noise floor, an ideal quantiser, and drift integration (de-chirp).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/sigmeas_audio.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "sigmeas_gen.hpp"
#include "test_check.hpp"

using namespace cascade::sigmeas;

namespace {

constexpr double kFs = 48000.0;
constexpr std::size_t kN = 65536;

// tone A at f0, harmonics (frequency multiples, level dBc, phase), white noise of total power noiseP
std::vector<float> synth(double f0, double A, const std::vector<std::pair<double, double>>& harm,
                         double noiseP, sg::Rng& rng, std::size_t n = kN, double rate = kFs) {
    std::vector<float> x(n);
    const double s = std::sqrt(noiseP);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / rate;
        double v = A * std::sin(sg::kTwoPi * f0 * t + 0.3);
        int h = 2;
        for (const auto& hd : harm) {
            v += A * std::pow(10.0, hd.second / 20.0) * std::sin(sg::kTwoPi * hd.first * f0 * t + 0.1 * h);
            ++h;
        }
        v += s * rng.gauss();
        x[i] = static_cast<float>(v);
    }
    return x;
}

}  // namespace

int main() {
    // --- distortion-limited: HD2 -60 dBc, HD3 -70 dBc, a very low noise floor ------------------------------------
    {
        sg::Rng rng(71);
        const double A = 0.5, f0 = 1000.37;
        const double pTone = A * A / 2.0;
        const double noiseP = pTone * 1e-8;
        const auto x = synth(f0, A, {{2.0, -60.0}, {3.0, -70.0}}, noiseP, rng);
        const AudioResult r = measureAudio(x.data(), x.size(), kFs);
        CHECK(r.valid);
        if (r.valid) {
            const double inBand = (kFs / 2.0 - 20.0) / (kFs / 2.0);
            const double nd = 1e-6 + 1e-7 + 1e-8 * inBand;
            CHECK_NEAR(r.toneHz, f0, 0.5);
            CHECK_NEAR(r.toneAmplitude, A, 0.002);
            CHECK_NEAR(r.toneDbfs, 20.0 * std::log10(A), 0.1);
            CHECK_NEAR(r.sinadDb, -10.0 * std::log10(nd), 0.3);
            CHECK_NEAR(r.thdDb, 10.0 * std::log10(1.1e-6), 0.3);
            CHECK_NEAR(r.thdPercent, 100.0 * std::sqrt(1.1e-6), 0.01 * 100.0 * std::sqrt(1.1e-6) + 0.0005);
            CHECK_NEAR(r.thdnDb, -r.sinadDb, 1e-9);
            CHECK_NEAR(r.sfdrDbc, 60.0, 0.5);
            CHECK_NEAR(r.spurHz, 2.0 * f0, 3.0);
            CHECK(r.harmonicsDbc.size() == 8);
            if (r.harmonicsDbc.size() == 8) {
                CHECK_NEAR(r.harmonicsDbc[0], -60.0, 0.3);
                CHECK_NEAR(r.harmonicsDbc[1], -70.0, 0.3);
                CHECK(r.harmonicsDbc[2] < -85.0);
            }
            if (std::fabs(r.sinadDb + 10.0 * std::log10(nd)) >= 0.3) { std::printf("  sinad %.2f want %.2f\n", r.sinadDb, -10.0 * std::log10(nd)); }
        }
    }
    // --- noise-limited: noise 50 dB under the tone ----------------------------------------------------------------
    {
        sg::Rng rng(72);
        const double A = 0.5, f0 = 997.1;
        const double pTone = A * A / 2.0;
        const auto x = synth(f0, A, {{2.0, -60.0}, {3.0, -70.0}}, pTone * 1e-5, rng);
        const AudioResult r = measureAudio(x.data(), x.size(), kFs);
        CHECK(r.valid);
        if (r.valid) {
            const double inBand = (kFs / 2.0 - 20.0) / (kFs / 2.0);
            const double nd = 1e-5 * inBand + 1.1e-6;
            CHECK_NEAR(r.sinadDb, -10.0 * std::log10(nd), 0.3);
            CHECK_NEAR(r.thdDb, 10.0 * std::log10(1.1e-6), 0.5);
            CHECK(r.sfdrDbc > 55.0 && r.sfdrDbc < 62.0);   // HD2 is still the largest line, a noise lobe sits near it
            CHECK_NEAR(r.noiseFloorDbfs, 10.0 * std::log10(pTone * 1e-5 * inBand / (0.5)), 0.4);
        }
        // a band limit (20 Hz .. 20 kHz) takes the noise above it out
        AudioParams p;
        p.bandHighHz = 20000.0;
        const AudioResult b = measureAudio(x.data(), x.size(), kFs, p);
        CHECK(b.valid);
        if (b.valid && r.valid) {
            CHECK(b.sinadDb > r.sinadDb + 0.5);
            CHECK_NEAR(b.sinadDb, -10.0 * std::log10(1e-5 * (20000.0 - 20.0) / (kFs / 2.0) + 1.1e-6), 0.3);
        }
    }
    // --- ENOB of an ideal quantiser ---------------------------------------------------------------------------------------
    for (int bits : {10, 12, 13}) {
        const double A = 0.999, f0 = 1000.37;
        std::vector<float> x(kN);
        const double q = std::ldexp(1.0, bits - 1);   // codes per unit of full scale (+-1)
        for (std::size_t i = 0; i < kN; ++i) {
            const double v = A * std::sin(sg::kTwoPi * f0 * static_cast<double>(i) / kFs + 0.7);
            x[i] = static_cast<float>(std::round(v * q) / q);
        }
        const AudioResult r = measureAudio(x.data(), x.size(), kFs);
        CHECK(r.valid);
        if (r.valid) {
            CHECK_NEAR(r.enob, static_cast<double>(bits), 0.1);
            CHECK_NEAR(r.sinadDb, 6.02 * bits + 1.76 + 20.0 * std::log10(A), 0.5);
            if (std::fabs(r.enob - bits) >= 0.1) { std::printf("  enob bits=%d got %.3f sinad %.2f\n", bits, r.enob, r.sinadDb); }
        }
    }
    // --- harmonics fold at Nyquist: an 11 kHz tone's 3rd lies at 33 kHz = 15 kHz sampled ---------------------------------------------
    {
        sg::Rng rng(73);
        const double A = 0.5, f0 = 11000.3;
        std::vector<float> x(kN);
        for (std::size_t i = 0; i < kN; ++i) {
            const double t = static_cast<double>(i) / kFs;
            x[i] = static_cast<float>(A * std::sin(sg::kTwoPi * f0 * t) +
                                      A * 3.16228e-4 * std::sin(sg::kTwoPi * (kFs - 3.0 * f0) * t + 1.0) +
                                      1e-6 * rng.gauss());
        }
        AudioParams p;
        p.toneHz = f0;
        const AudioResult r = measureAudio(x.data(), x.size(), kFs, p);
        CHECK(r.valid);
        if (r.valid && r.harmonicsDbc.size() >= 2) { CHECK_NEAR(r.harmonicsDbc[1], -70.0, 0.4); }
    }
    // --- boundaries and failures -----------------------------------------------------------------------------------------------------------
    {
        sg::Rng rng(74);
        const auto x = synth(1000.0, 0.5, {}, 1e-9, rng, 8192);
        AudioParams p;
        p.nfft = 4096;
        const AudioResult a = measureAudio(x.data(), x.size(), kFs, p);
        CHECK(a.valid);
        p.nfft = 16384;
        CHECK(std::string(measureAudio(x.data(), x.size(), kFs, p).reason) == why::kTooShort);
        p.nfft = 3000;
        CHECK(std::string(measureAudio(x.data(), x.size(), kFs, p).reason) == why::kBadParam);
        p = AudioParams{};
        p.maxHarmonic = 1;
        CHECK(std::string(measureAudio(x.data(), x.size(), kFs, p).reason) == why::kBadParam);
        p = AudioParams{};
        p.bandLowHz = 30000.0;   // above Nyquist-limited high edge
        CHECK(std::string(measureAudio(x.data(), x.size(), kFs, p).reason) == why::kBadParam);
        p = AudioParams{};
        p.fullScaleAmplitude = 0.0;
        CHECK(std::string(measureAudio(x.data(), x.size(), kFs, p).reason) == why::kBadParam);
        p = AudioParams{};
        p.toneHz = -5.0;
        CHECK(std::string(measureAudio(x.data(), x.size(), kFs, p).reason) == why::kBadParam);

        CHECK(std::string(measureAudio(nullptr, 0, kFs).reason) == why::kEmpty);
        CHECK(std::string(measureAudio(x.data(), x.size(), 0.0).reason) == why::kBadRate);
        CHECK(std::string(measureAudio(x.data(), 500, kFs).reason) == why::kTooShort);
        std::vector<float> bad = x;
        bad[100] = std::nanf("");
        CHECK(std::string(measureAudio(bad.data(), bad.size(), kFs).reason) == why::kNonFinite);
        bad[100] = std::numeric_limits<float>::infinity();
        CHECK(std::string(measureAudio(bad.data(), bad.size(), kFs).reason) == why::kNonFinite);
        std::vector<float> zero(8192, 0.0f);
        CHECK(std::string(measureAudio(zero.data(), zero.size(), kFs).reason) == why::kNoSignal);
        std::vector<float> dc(8192, 0.3f);   // DC only: no tone above the DC guard
        CHECK(!measureAudio(dc.data(), dc.size(), kFs).valid);
    }

    // --- drift integration --------------------------------------------------------------------------------------------------------------------
    {
        const double fs = 8000.0, e = 300e-6, f0 = 1500.0, a = 3.7;   // Hz and Hz/s on the true clock
        const double trueFs = fs * (1.0 + e);
        const std::size_t n = 65536;
        sg::Rng rng(75);
        std::vector<Cf> x(n);
        std::vector<float> re(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(i) / trueFs;
            const double ph = sg::kTwoPi * (f0 * t + 0.5 * a * t * t);
            x[i] = Cf(static_cast<float>(std::cos(ph) + 1.0 * rng.gauss()), static_cast<float>(std::sin(ph) + 1.0 * rng.gauss()));
            re[i] = static_cast<float>(std::sin(ph) + 0.5 * rng.gauss());
        }
        const DriftResult d = searchDrift(x.data(), x.size(), fs);
        CHECK(d.valid);
        if (d.valid) {
            // on the nominal clock the drift reads a / (1+e)^2 and the frequency f0 / (1+e)
            CHECK(std::fabs(d.driftHzPerS - a / ((1.0 + e) * (1.0 + e))) < 0.01 * a);
            CHECK_NEAR(d.startFreqHz, f0 / (1.0 + e), 1.0);
            CHECK(d.gainDb > 12.0);
            if (std::fabs(d.driftHzPerS - a) >= 0.01 * a) { std::printf("  drift got %.4f want %.4f gain %.1f\n", d.driftHzPerS, a, d.gainDb); }
        }
        const std::vector<Cf> an = analyticFromReal(re.data(), re.size());
        CHECK(an.size() == n);
        const DriftResult d2 = searchDrift(an.data(), an.size(), fs);
        CHECK(d2.valid);
        if (d2.valid) { CHECK(std::fabs(d2.driftHzPerS - a) < 0.02 * a); }
        // the corrected spectrum is sharper: de-chirp with the found rate and re-search at zero drift range
        if (d.valid) {
            std::vector<Cf> y(n);
            CHECK(dechirp(x.data(), n, fs, d.driftHzPerS, y.data()));
            DriftParams narrow;
            narrow.minDriftHzPerS = -0.2;
            narrow.maxDriftHzPerS = 0.2;
            const DriftResult again = searchDrift(y.data(), y.size(), fs, narrow);
            CHECK(again.valid && std::fabs(again.driftHzPerS) < 0.05);
        }
        // negative drift, and a stable tone reads ~0
        std::vector<Cf> neg(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(i) / fs;
            const double ph = sg::kTwoPi * (2500.0 * t - 0.5 * 5.0 * t * t);
            neg[i] = Cf(static_cast<float>(std::cos(ph) + 0.7 * rng.gauss()), static_cast<float>(std::sin(ph) + 0.7 * rng.gauss()));
        }
        const DriftResult dn = searchDrift(neg.data(), neg.size(), fs);
        CHECK(dn.valid && std::fabs(dn.driftHzPerS + 5.0) < 0.05);
        std::vector<Cf> steady(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double ph = sg::kTwoPi * 1234.5 * static_cast<double>(i) / fs;
            steady[i] = Cf(static_cast<float>(std::cos(ph) + 0.7 * rng.gauss()), static_cast<float>(std::sin(ph) + 0.7 * rng.gauss()));
        }
        const DriftResult ds = searchDrift(steady.data(), steady.size(), fs);
        CHECK(ds.valid && std::fabs(ds.driftHzPerS) < 0.1);
        CHECK_NEAR(ds.startFreqHz, 1234.5, 0.5);
    }
    {
        std::vector<Cf> x(4096, Cf(0.1f, 0.0f)), y(4096);
        CHECK(!dechirp(nullptr, 4096, 8000.0, 1.0, y.data()));
        CHECK(!dechirp(x.data(), 4096, 0.0, 1.0, y.data()));
        CHECK(!dechirp(x.data(), 4096, 8000.0, std::nan(""), y.data()));
        CHECK(!dechirp(x.data(), 0, 8000.0, 1.0, y.data()));
        CHECK(std::string(searchDrift(nullptr, 0, 8000.0).reason) == why::kEmpty);
        CHECK(std::string(searchDrift(x.data(), 100, 8000.0).reason) == why::kTooShort);
        CHECK(std::string(searchDrift(x.data(), x.size(), -1.0).reason) == why::kBadRate);
        DriftParams p;
        p.minDriftHzPerS = 5.0;
        p.maxDriftHzPerS = 1.0;
        CHECK(std::string(searchDrift(x.data(), x.size(), 8000.0, p).reason) == why::kBadParam);
        p = DriftParams{};
        p.gridSteps = 2;
        CHECK(std::string(searchDrift(x.data(), x.size(), 8000.0, p).reason) == why::kBadParam);
        p = DriftParams{};
        p.maxFft = 3000;
        CHECK(std::string(searchDrift(x.data(), x.size(), 8000.0, p).reason) == why::kBadParam);
        x[5] = Cf(std::nanf(""), 0.0f);
        CHECK(std::string(searchDrift(x.data(), x.size(), 8000.0).reason) == why::kNonFinite);
        CHECK(analyticFromReal(nullptr, 100).empty());
        std::vector<float> few(4, 1.0f);
        CHECK(analyticFromReal(few.data(), few.size()).empty());
        std::vector<float> nan(64, std::nanf(""));
        CHECK(analyticFromReal(nan.data(), nan.size()).empty());
    }
    return testSummary("test_sigmeas_audio");
}
