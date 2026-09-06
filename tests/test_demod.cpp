// Tests for dsp/demod.hpp — the switchable analog demodulator suite.
//
// Every DSP claim is proven against in-test references: synthesized signals
// with known analytic demodulation results, direct-DFT spectral measurements,
// and closed-form one-pole transfer functions derived from the SPEC constants
// (75 us deemphasis) — never against values read back from the implementation.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/demod.hpp"

#include "test_check.hpp"

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kFs = 48000.0;   // channel rate for every test
constexpr std::size_t kN = 4800;  // analysis window: 10 Hz bins, so 1 kHz,
                                  // 1.5 kHz, 700 Hz all land on integer bins

using cascade::dsp::Demodulator;
using cascade::dsp::DemodMode;
using cascade::dsp::kDemodModeCount;
using cascade::dsp::modeFromName;
using cascade::dsp::modeName;

// Fixed-seed LCG (Numerical Recipes constants) for the passthrough tests.
float lcgUnit(std::uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    // Top 24 bits -> [-1, 1).
    return static_cast<float>(s >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

// Complex tone at fHz (may be negative), unit-per-'amp' magnitude.
std::vector<std::complex<float>> makeToneHz(double fHz, double amp,
                                            std::size_t n) {
    std::vector<std::complex<float>> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = kTwoPi * fHz * static_cast<double>(i) / kFs;
        x[i] = {static_cast<float>(amp * std::cos(ph)),
                static_cast<float>(amp * std::sin(ph))};
    }
    return x;
}

// Single-tone FM: instantaneous frequency devHz*sin(2*pi*fmHz*t). The ideal
// discriminator output IS devHz's phase increment, so the recovered tone has
// amplitude exactly 2*pi*devHz/kFs (radians/sample) — the analytic reference.
std::vector<std::complex<float>> makeFm(double fmHz, double devHz,
                                        std::size_t n) {
    std::vector<std::complex<float>> x(n);
    double phi = 0.0;
    x[0] = {1.0f, 0.0f};
    for (std::size_t i = 1; i < n; ++i) {
        phi += kTwoPi * (devHz / kFs) *
               std::sin(kTwoPi * fmHz * static_cast<double>(i) / kFs);
        x[i] = {static_cast<float>(std::cos(phi)),
                static_cast<float>(std::sin(phi))};
    }
    return x;
}

// Direct-DFT magnitude of the real vector x[0..n) at bin k — the independent
// spectral reference (no FFT library, no implementation code).
double dftMag(const float* x, std::size_t n, std::size_t k) {
    double re = 0.0;
    double im = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = kTwoPi * static_cast<double>(k) *
                          static_cast<double>(i) / static_cast<double>(n);
        re += static_cast<double>(x[i]) * std::cos(ph);
        im -= static_cast<double>(x[i]) * std::sin(ph);
    }
    return std::sqrt(re * re + im * im);
}

// Coherently sampled real sinusoid of amplitude A: |X[k]| = A*n/2.
double binAmp(const float* x, std::size_t n, std::size_t k) {
    return 2.0 * dftMag(x, n, k) / static_cast<double>(n);
}

std::size_t dominantBin(const float* x, std::size_t n, std::size_t kLo,
                        std::size_t kHi) {
    std::size_t kBest = kLo;
    double magBest = -1.0;
    for (std::size_t k = kLo; k <= kHi; ++k) {
        const double m = dftMag(x, n, k);
        if (m > magBest) {
            magBest = m;
            kBest = k;
        }
    }
    return kBest;
}

// Runs `d` over the signal and returns the audio tail: the last kN samples,
// leaving `warm` samples for filters/oscillators to reach steady state.
std::vector<float> demodTail(Demodulator& d,
                             const std::vector<std::complex<float>>& x,
                             std::size_t warm) {
    std::vector<float> y(x.size());
    CHECK(d.process(x.data(), x.size(), y.data()) == x.size());
    return std::vector<float>(y.begin() + static_cast<std::ptrdiff_t>(warm),
                              y.end());
}

// A block with energy for every mode: two sidebands, DC, varying envelope.
std::vector<std::complex<float>> makeMixedBlock(std::size_t n) {
    std::vector<std::complex<float>> b(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kFs;
        const std::complex<double> v =
            0.5 * std::exp(std::complex<double>(0.0, kTwoPi * 1500.0 * t)) +
            0.3 * std::exp(std::complex<double>(0.0, -kTwoPi * 700.0 * t)) +
            0.2;
        b[i] = {static_cast<float>(v.real()), static_cast<float>(v.imag())};
    }
    return b;
}

}  // namespace

int main() {
    // --- Mode names: the single vocabulary three consumers share ------------
    // The config store persists a mode by NAME, the GUI's button table orders
    // the modes differently from the enum, and the web API accepts a mode from
    // a browser. All three resolve through modeName/modeFromName, so this is
    // what stops one of them accepting a spelling the others reject.
    {
        for (std::size_t i = 0; i < kDemodModeCount; ++i) {
            const auto m = static_cast<DemodMode>(i);
            const char* name = modeName(m);
            CHECK(name != nullptr);
            CHECK(name[0] != '\0');
            DemodMode back{};
            const bool ok = modeFromName(name, back);
            CHECK(ok);
            if (ok) {
                CHECK(back == m);  // round trip, for every mode
            }
        }
        // Names are distinct: two modes sharing a spelling would make the
        // round trip above pass while making one of them unreachable.
        for (std::size_t i = 0; i < kDemodModeCount; ++i) {
            for (std::size_t k = i + 1; k < kDemodModeCount; ++k) {
                CHECK(std::string(modeName(static_cast<DemodMode>(i))) !=
                      std::string(modeName(static_cast<DemodMode>(k))));
            }
        }
        // Exact and case-sensitive; nothing else is accepted.
        DemodMode ignored{};
        CHECK(!modeFromName("", ignored));
        CHECK(!modeFromName("wfm", ignored));
        CHECK(!modeFromName("FM", ignored));
        CHECK(!modeFromName(" WFM", ignored));
        // A value cast in from outside the enum must still yield a printable
        // string rather than reading past the table.
        CHECK(modeName(static_cast<DemodMode>(99)) != nullptr);
    }

    // --- NFM: 1 kHz modulator / 2.5 kHz deviation round trip ----------------
    {
        Demodulator d(kFs);
        d.setMode(DemodMode::NFM);
        CHECK(d.mode() == DemodMode::NFM);
        // De-emphasis OFF, explicitly. This block measures the DISCRIMINATOR
        // against its analytic 2*pi*dev/fs reference, and NFM now runs the
        // same one-pole WFM does — at the 50 us default that shades the 1 kHz
        // tone by 0.40 dB, which would leave the amplitude check below sitting
        // 4.5% low inside its own 5% tolerance while quietly measuring a
        // filter it never meant to include. Same discipline as the WFM loop
        // further down: the default is never what a test silently depends on.
        d.setDeemphasisUs(0.0);

        // Analyze y[1..kN]: sample 0 is the discriminator's absolute-phase
        // startup sample; the remaining kN samples hold an integer number of
        // modulator cycles, so the DFT is coherent.
        auto x = makeFm(1000.0, 2500.0, kN + 1);
        auto y = demodTail(d, x, 1);
        CHECK(dominantBin(y.data(), kN, 1, kN / 2) == 100);  // 1 kHz = bin 100

        const double amp25 = binAmp(y.data(), kN, 100);
        const double expected = kTwoPi * 2500.0 / kFs;  // rad/sample deviation
        CHECK_NEAR(amp25, expected, 0.05 * expected);

        // Amplitude tracks deviation: half the deviation, half the amplitude.
        d.reset();
        auto x2 = makeFm(1000.0, 1250.0, kN + 1);
        auto y2 = demodTail(d, x2, 1);
        const double amp125 = binAmp(y2.data(), kN, 100);
        CHECK_NEAR(amp25 / amp125, 2.0, 0.04);
    }

    // --- WFM deemphasis: both regional standards, pinned analytically -------
    // Parameterized over tau because 50 us (Europe/Africa/Asia/Australia) and
    // 75 us (Americas/South Korea) are BOTH correct depending on where the
    // receiver is, and the default must never be what the test silently
    // depends on — setting it explicitly is the point.
    for (const double tauUs : {50.0, 75.0}) {
        Demodulator d(kFs);
        d.setMode(DemodMode::WFM);
        d.setDeemphasisUs(tauUs);
        CHECK_NEAR(d.deemphasisUs(), tauUs, 1e-9);

        // Same deviation both runs, so the pre-deemphasis amplitudes are
        // equal and any ratio between the recovered tones is the deemphasis
        // network's doing alone.
        const std::size_t warm = 256;  // deemph time constant is ~3.6 samples
        auto x1 = makeFm(1000.0, 2500.0, warm + kN);
        auto y1 = demodTail(d, x1, warm);
        CHECK(dominantBin(y1.data(), kN, 1, kN / 2) == 100);
        const double a1k = binAmp(y1.data(), kN, 100);

        d.reset();
        auto x10 = makeFm(10000.0, 2500.0, warm + kN);
        auto y10 = demodTail(d, x10, warm);
        CHECK(dominantBin(y10.data(), kN, 1, kN / 2) == 1000);
        const double a10k = binAmp(y10.data(), kN, 1000);

        // Analytic one-pole reference from the SPEC constant tau:
        // pole p = exp(-1/(fs*tau)), |H(w)| = (1-p)/sqrt(1 - 2p cos w + p^2).
        const double p = std::exp(-1.0 / (kFs * tauUs * 1.0e-6));
        auto mag = [p](double fHz) {
            const double w = kTwoPi * fHz / kFs;
            return (1.0 - p) / std::sqrt(1.0 - 2.0 * p * std::cos(w) + p * p);
        };
        const double measured = a10k / a1k;
        const double expected = mag(10000.0) / mag(1000.0);
        const double errDb = 20.0 * std::log10(measured / expected);
        CHECK(std::fabs(errDb) < 1.0);
        // And the attenuation is real, not a no-op passing the ratio check:
        // analytically the ratio is ~0.25 (-12 dB) at these frequencies.
        CHECK(measured < 0.5);
    }

    // --- Deemphasis OFF is a true passthrough, and 50 != 75 -----------------
    // Without this, setDeemphasisUs could quietly do nothing and the loop
    // above would still pass at both settings.
    {
        const std::size_t warm = 256;
        auto measureRatio = [&](double tauUs) {
            Demodulator d(kFs);
            d.setMode(DemodMode::WFM);
            d.setDeemphasisUs(tauUs);
            auto lo = makeFm(1000.0, 2500.0, warm + kN);
            const double a1k = binAmp(demodTail(d, lo, warm).data(), kN, 100);
            d.reset();
            d.setDeemphasisUs(tauUs);  // reset() must not silently re-enable it
            auto hi = makeFm(10000.0, 2500.0, warm + kN);
            const double a10k = binAmp(demodTail(d, hi, warm).data(), kN, 1000);
            return a10k / a1k;
        };

        // Off: 10 kHz and 1 kHz come back at the same level (flat response).
        const double flat = measureRatio(0.0);
        CHECK_NEAR(flat, 1.0, 0.05);

        // 50 us rolls off LESS than 75 us at 10 kHz — the whole reason the
        // setting exists. A stuck implementation returning one curve for both
        // fails here even though each passes its own analytic check.
        const double r50 = measureRatio(50.0);
        const double r75 = measureRatio(75.0);
        CHECK(r50 > r75);
        CHECK(flat > r50);
    }

    // --- NFM de-emphasis: the same network, in the mode that had none -------
    // The De-emph control is offered for NFM as well as WFM, but the filter
    // ran in the demodulator's WFM case alone: on NFM the combo was enabled,
    // adjustable and saved to the config while changing nothing whatsoever —
    // and NFM is what a listener chasing a weather-satellite picture is tuned
    // to. Measured exactly as WFM is above (one tone below the corner, one
    // above, at identical deviation, ratio against the closed-form one-pole
    // derived from tau), so the two FM modes cannot drift apart without one of
    // them failing here.
    {
        const std::size_t warm = 256;  // >> 3.6-sample deemph time constant
        // 1 kHz sits well below the corner (3.2 kHz at 50 us, 2.1 kHz at
        // 75 us) and 10 kHz well above it, so the pair straddles the corner at
        // both settings; equal deviation in both runs makes the ratio the
        // de-emphasis network's doing and nothing else's.
        auto measureNfmRatio = [&](double tauUs) {
            Demodulator d(kFs);
            d.setMode(DemodMode::NFM);
            d.setDeemphasisUs(tauUs);
            CHECK_NEAR(d.deemphasisUs(), tauUs, 1e-9);
            auto lo = makeFm(1000.0, 2500.0, warm + kN);
            auto ylo = demodTail(d, lo, warm);
            CHECK(dominantBin(ylo.data(), kN, 1, kN / 2) == 100);
            const double a1k = binAmp(ylo.data(), kN, 100);
            d.reset();
            d.setDeemphasisUs(tauUs);  // reset() must not silently re-enable it
            auto hi = makeFm(10000.0, 2500.0, warm + kN);
            auto yhi = demodTail(d, hi, warm);
            CHECK(dominantBin(yhi.data(), kN, 1, kN / 2) == 1000);
            const double a10k = binAmp(yhi.data(), kN, 1000);
            return a10k / a1k;
        };
        // The reference: pole p = exp(-1/(fs*tau)) from the SPEC constant,
        // |H(w)| = (1-p)/sqrt(1 - 2p cos w + p^2). Derived here, never read
        // back from the implementation.
        auto expectRatio = [](double tauUs) {
            const double p = std::exp(-1.0 / (kFs * tauUs * 1.0e-6));
            auto mag = [p](double fHz) {
                const double w = kTwoPi * fHz / kFs;
                const double den =
                    std::sqrt(1.0 - 2.0 * p * std::cos(w) + p * p);
                return (1.0 - p) / den;
            };
            return mag(10000.0) / mag(1000.0);
        };

        const double n50 = measureNfmRatio(50.0);
        const double n75 = measureNfmRatio(75.0);
        const double nOff = measureNfmRatio(0.0);

        CHECK(std::fabs(20.0 * std::log10(n50 / expectRatio(50.0))) < 1.0);
        CHECK(std::fabs(20.0 * std::log10(n75 / expectRatio(75.0))) < 1.0);
        // Real attenuation — analytically the ratio is 0.34 (-9.3 dB) at 50 us
        // and 0.25 (-12.2 dB) at 75 us — rather than a flat path, which would
        // return 1.0 here and satisfy no bound below 0.5.
        CHECK(n50 < 0.5);
        CHECK(n75 < 0.5);

        // "Off" must still be a true passthrough: the fix has to honour the
        // third entry of the control, not de-emphasise unconditionally. This
        // one assertion is the only part of this block that also held before
        // the filter reached NFM — it is here to stop the cure overshooting.
        CHECK_NEAR(nOff, 1.0, 0.05);
        // And the two constants are distinguishable, in the right order: 50 us
        // rolls off less at 10 kHz than 75 us does, and both roll off.
        CHECK(nOff > n50);
        CHECK(n50 > n75);
    }

    // --- AM: 80% modulation, carrier NOT at DC, envelope + DC rejection -----
    {
        Demodulator d(kFs);
        d.setMode(DemodMode::AM);

        // Carrier deliberately offset +500 Hz: |x| must recover the envelope
        // regardless of carrier phase/frequency (a Re{x} detector would not).
        const double carrierAmp = 0.7;
        const double m = 0.8;
        const std::size_t warm = 2400;  // >> DC blocker settle (~5 ms = 240)
        std::vector<std::complex<float>> x(warm + kN);
        for (std::size_t i = 0; i < x.size(); ++i) {
            const double t = static_cast<double>(i) / kFs;
            const double env =
                carrierAmp * (1.0 + m * std::cos(kTwoPi * 1000.0 * t));
            const double ph = kTwoPi * 500.0 * t;
            x[i] = {static_cast<float>(env * std::cos(ph)),
                    static_cast<float>(env * std::sin(ph))};
        }
        auto y = demodTail(d, x, warm);

        CHECK(dominantBin(y.data(), kN, 1, kN / 2) == 100);  // 1 kHz modulator
        const double tone = binAmp(y.data(), kN, 100);
        // Any sane DC blocker is within a few percent of unity at 1 kHz.
        CHECK_NEAR(tone, m * carrierAmp, 0.05 * m * carrierAmp);

        // DC suppression: output bin 0 versus the envelope's own DC content
        // (carrierAmp per sample -> |X[0]| = carrierAmp*kN). > 40 dB down.
        const double dcOut = dftMag(y.data(), kN, 0);
        const double dcIn = carrierAmp * static_cast<double>(kN);
        CHECK(dcOut < 0.01 * dcIn);
    }

    // --- USB: +1.5 kHz passes to 1.5 kHz audio, -1.5 kHz suppressed ---------
    const std::size_t ssbWarm = 2048;  // >> FIR length (159 taps at 48 kHz)
    {
        Demodulator d(kFs);
        d.setMode(DemodMode::USB);
        CHECK(d.mode() == DemodMode::USB);

        auto want = demodTail(d, makeToneHz(1500.0, 1.0, ssbWarm + kN), ssbWarm);
        CHECK(dominantBin(want.data(), kN, 1, kN / 2) == 150);
        const double aWant = binAmp(want.data(), kN, 150);
        CHECK_NEAR(aWant, 1.0, 0.05);  // unit tone in -> unit audio tone out

        d.reset();
        auto image =
            demodTail(d, makeToneHz(-1500.0, 1.0, ssbWarm + kN), ssbWarm);
        // The leaked mirror lands exactly at 1.5 kHz audio; check that bin
        // AND every other bin so leakage cannot hide elsewhere. > 40 dB down.
        const std::size_t kDom = dominantBin(image.data(), kN, 1, kN / 2);
        const double aImageMax = binAmp(image.data(), kN, kDom);
        CHECK(binAmp(image.data(), kN, 150) < 0.01 * aWant);
        CHECK(aImageMax < 0.01 * aWant);
    }

    // --- LSB: mirror image of USB -------------------------------------------
    {
        Demodulator d(kFs);
        d.setMode(DemodMode::LSB);

        auto want =
            demodTail(d, makeToneHz(-1500.0, 1.0, ssbWarm + kN), ssbWarm);
        CHECK(dominantBin(want.data(), kN, 1, kN / 2) == 150);
        const double aWant = binAmp(want.data(), kN, 150);
        CHECK_NEAR(aWant, 1.0, 0.05);

        d.reset();
        auto image = demodTail(d, makeToneHz(1500.0, 1.0, ssbWarm + kN), ssbWarm);
        const std::size_t kDom = dominantBin(image.data(), kN, 1, kN / 2);
        CHECK(binAmp(image.data(), kN, 150) < 0.01 * aWant);
        CHECK(binAmp(image.data(), kN, kDom) < 0.01 * aWant);
    }

    // --- CW: carrier at DC beats at 700 Hz ----------------------------------
    {
        Demodulator d(kFs);
        d.setMode(DemodMode::CW);

        std::vector<std::complex<float>> x(ssbWarm + kN, {1.0f, 0.0f});
        auto y = demodTail(d, x, ssbWarm);
        CHECK(dominantBin(y.data(), kN, 1, kN / 2) == 70);  // 700 Hz = bin 70
        CHECK_NEAR(binAmp(y.data(), kN, 70), 1.0, 0.05);
    }

    // --- SSB streaming: chunked processing equals one-shot ------------------
    {
        Demodulator one(kFs);
        Demodulator chunked(kFs);
        one.setMode(DemodMode::USB);
        chunked.setMode(DemodMode::USB);

        auto x = makeMixedBlock(2000);
        std::vector<float> yOne(x.size());
        std::vector<float> yChunk(x.size());
        CHECK(one.process(x.data(), x.size(), yOne.data()) == x.size());
        // Ragged splits, including a 1-sample block, to walk the FIR history
        // and both NCO phases across every kind of seam.
        const std::size_t splits[] = {1, 129, 257, 500, 613, 500};
        std::size_t pos = 0;
        for (std::size_t len : splits) {
            CHECK(chunked.process(x.data() + pos, len, yChunk.data() + pos) ==
                  len);
            pos += len;
        }
        CHECK(pos == x.size());
        for (std::size_t i = 0; i < x.size(); ++i) {
            CHECK_NEAR(yChunk[i], yOne[i], 1e-9);
        }
    }

    // --- Mode switching resets state: first block == fresh object ----------
    {
        const DemodMode all[] = {DemodMode::NFM, DemodMode::WFM, DemodMode::AM,
                                 DemodMode::DSB, DemodMode::USB, DemodMode::LSB,
                                 DemodMode::CW,  DemodMode::RAW};
        auto block = makeMixedBlock(512);
        // Dirtying signals with odd lengths so NCO phases, FIR history, the
        // deemphasis pole and the DC blocker are all left mid-flight.
        auto dirtA = makeFm(700.0, 2000.0, 777);
        auto dirtB = makeToneHz(1234.0, 0.8, 501);

        for (DemodMode m : all) {
            Demodulator dirty(kFs);
            std::vector<float> scratch(dirtA.size());
            dirty.setMode(DemodMode::USB);
            dirty.process(dirtA.data(), dirtA.size(), scratch.data());
            dirty.setMode(DemodMode::WFM);
            dirty.process(dirtB.data(), dirtB.size(), scratch.data());
            dirty.setMode(m);
            CHECK(dirty.mode() == m);

            Demodulator fresh(kFs);
            fresh.setMode(m);

            std::vector<float> ySwitched(block.size());
            std::vector<float> yFresh(block.size());
            CHECK(dirty.process(block.data(), block.size(),
                                ySwitched.data()) == block.size());
            CHECK(fresh.process(block.data(), block.size(), yFresh.data()) ==
                  block.size());
            for (std::size_t i = 0; i < block.size(); ++i) {
                CHECK_NEAR(ySwitched[i], yFresh[i], 1e-12);  // bit-close
            }
        }

        // reset() keeps the mode but clears state the same way.
        for (DemodMode m : {DemodMode::WFM, DemodMode::AM, DemodMode::USB}) {
            Demodulator d(kFs);
            std::vector<float> scratch(dirtA.size());
            d.setMode(m);
            d.process(dirtA.data(), dirtA.size(), scratch.data());
            d.reset();
            CHECK(d.mode() == m);

            Demodulator fresh(kFs);
            fresh.setMode(m);
            std::vector<float> yReset(block.size());
            std::vector<float> yFresh(block.size());
            d.process(block.data(), block.size(), yReset.data());
            fresh.process(block.data(), block.size(), yFresh.data());
            for (std::size_t i = 0; i < block.size(); ++i) {
                CHECK_NEAR(yReset[i], yFresh[i], 1e-12);
            }
        }
    }

    // --- RAW and DSB: exact real-part passthrough ---------------------------
    {
        std::uint32_t seed = 0xC0FFEEu;
        std::vector<std::complex<float>> x(1024);
        for (auto& v : x) {
            const float re = lcgUnit(seed);
            const float im = lcgUnit(seed);
            v = {re, im};
        }

        Demodulator d(kFs);
        d.setMode(DemodMode::RAW);
        std::vector<float> y(x.size());
        CHECK(d.process(x.data(), x.size(), y.data()) == x.size());
        for (std::size_t i = 0; i < x.size(); ++i) {
            CHECK(y[i] == x[i].real());  // passthrough is EXACT, not near
        }

        d.setMode(DemodMode::DSB);
        std::vector<float> y2(x.size());
        CHECK(d.process(x.data(), x.size(), y2.data()) == x.size());
        for (std::size_t i = 0; i < x.size(); ++i) {
            CHECK(y2[i] == x[i].real());
        }

        // Zero-length call is legal and produces nothing.
        CHECK(d.process(x.data(), 0, y.data()) == 0);
    }

    return testSummary("test_demod");
}
