// test_modulator.cpp - the five transmit modes, measured rather than
// described.
//
// WHY THE MEASUREMENTS ARE SPECTRAL AND NOT SAMPLE-BY-SAMPLE. A modulator can
// be wrong in exactly the ways a waveform comparison cannot see: a sideband on
// the wrong side is still a sine, an unlimited deviation is still a smooth
// phase, and a hard-keyed carrier is still a carrier. Every one of those is a
// transmission into somebody else's channel, and every one of them shows up
// immediately in a transform. So the checks below are what a spectrum
// analyser in front of the radio would read - carrier and sideband amplitudes
// against the closed forms - and the closed forms are named where they are
// used so a future reader can check the number rather than trust it.
//
// THE TRANSFORM IS A DIRECT DFT AT ONE FREQUENCY, not an FFT. Three reasons:
// the frequencies of interest are known exactly (a tone and its harmonics),
// the block length can then be chosen as a whole number of periods so the
// answer is exact with no window at all, and a test that carries its own
// twenty-line transform cannot be wrong because of something in the product
// it is testing.
//
// NOTHING HERE HAS BEEN ON THE AIR. There is no transmitter on this bench.
// What is established is that the arithmetic produces the spectra the modes
// are defined by; what is not is anything about a real board's DAC, its
// filters or its output level.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

#include "dsp/modulator.hpp"
#include "test_check.hpp"

using cascade::dsp::kAmDepth;
using cascade::dsp::kCwRampMs;
using cascade::dsp::kNfmDeviationHz;
using cascade::dsp::Modulator;
using cascade::dsp::ToneGenerator;
using cascade::dsp::TxMode;

namespace {

constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

// One bin of a DFT, at an arbitrary frequency, normalised so a complex
// exponential of amplitude A at that frequency reads A.
std::complex<double> dftAt(const std::complex<float>* x, std::size_t n, double freqHz) {
    std::complex<double> acc(0.0, 0.0);
    for (std::size_t k = 0; k < n; ++k) {
        const double ang = -2.0 * kPi * freqHz * static_cast<double>(k) / kFs;
        acc += std::complex<double>(x[k].real(), x[k].imag()) *
               std::complex<double>(std::cos(ang), std::sin(ang));
    }
    return acc / static_cast<double>(n);
}

double magAt(const std::vector<std::complex<float>>& x, double freqHz) {
    return std::abs(dftAt(x.data(), x.size(), freqHz));
}

// A tone of a given amplitude, built here rather than taken from
// ToneGenerator so that a defect in the generator cannot silently change what
// every modulator check is being fed.
std::vector<float> tone(double freqHz, double amplitude, std::size_t n) {
    std::vector<float> out(n);
    for (std::size_t k = 0; k < n; ++k) {
        out[k] = static_cast<float>(amplitude *
                                    std::sin(2.0 * kPi * freqHz * static_cast<double>(k) / kFs));
    }
    return out;
}

// Runs the modulator past its key-up ramp and returns the STEADY part, so no
// measurement below is taken across the envelope that every mode shares.
std::vector<std::complex<float>> steady(Modulator& m, const std::vector<float>& audio,
                                        std::size_t settle, std::size_t want) {
    std::vector<std::complex<float>> all(audio.size());
    m.process(audio.data(), audio.size(), all.data());
    std::vector<std::complex<float>> out;
    out.assign(all.begin() + static_cast<std::ptrdiff_t>(settle),
               all.begin() + static_cast<std::ptrdiff_t>(settle + want));
    return out;
}

// The Bessel functions of the first kind at 2.5, to six places, from the
// standard tables. Written out rather than computed so the expectation is
// independent of anything in this process: if a future std::cyl_bessel_j
// disagreed with these, the table is the one that has been checked.
constexpr double kJ0at2p5 = 0.0483838;
constexpr double kJ1at2p5 = 0.4970941;
constexpr double kJ2at2p5 = 0.4460591;
constexpr double kJ3at2p5 = 0.2166004;

}  // namespace

int main() {
    // =====================================================================
    // 1. THE MODE TABLE. Names, indices and the round trip config.json
    //    depends on - a mode that cannot be written and read back is a
    //    setting that silently resets every launch.
    // =====================================================================
    {
        CHECK(std::string(cascade::dsp::txModeName(TxMode::CW)) == "CW");
        CHECK(std::string(cascade::dsp::txModeName(TxMode::AM)) == "AM");
        CHECK(std::string(cascade::dsp::txModeName(TxMode::NFM)) == "NFM");
        CHECK(std::string(cascade::dsp::txModeName(TxMode::USB)) == "USB");
        CHECK(std::string(cascade::dsp::txModeName(TxMode::LSB)) == "LSB");

        for (int i = 0; i < cascade::dsp::kTxModeCount; ++i) {
            const TxMode m = cascade::dsp::txModeFromIndex(i);
            CHECK(static_cast<int>(m) == i);
            TxMode back = TxMode::CW;
            CHECK(cascade::dsp::txModeFromName(cascade::dsp::txModeName(m), back));
            CHECK(back == m);
            // A caption per mode, and a real one: an empty string here is a
            // page with a blank line where the explanation should be.
            CHECK(std::string(cascade::dsp::txModeCaption(m)).size() > 20);
        }
        // A hand-edited index opens on a mode that exists.
        CHECK(cascade::dsp::txModeFromIndex(-1) == TxMode::CW);
        CHECK(cascade::dsp::txModeFromIndex(99) == TxMode::CW);
        TxMode ignored = TxMode::CW;
        CHECK(!cascade::dsp::txModeFromName("FM", ignored));
        CHECK(!cascade::dsp::txModeFromName(nullptr, ignored));
    }

    // =====================================================================
    // 2. CW: THE ENVELOPE, AND THE CLICKS IT EXISTS TO PREVENT
    // =====================================================================
    {
        Modulator m;
        m.setMode(TxMode::CW);
        CHECK(m.idle());  // nothing is keyed until something keys it
        m.setKeyed(true);

        const std::size_t ramp = static_cast<std::size_t>(kCwRampMs * 1.0e-3 * kFs);
        std::vector<float> silence(ramp * 3, 0.0f);
        std::vector<std::complex<float>> out(silence.size());
        m.process(silence.data(), silence.size(), out.data());

        // CW carries no audio at all: a pure real envelope, no quadrature.
        double worstImag = 0.0;
        for (const std::complex<float>& c : out) {
            worstImag = std::max(worstImag, static_cast<double>(std::fabs(c.imag())));
        }
        CHECK(worstImag == 0.0);

        // It starts from nothing, rises monotonically, and gets all the way
        // there within the ramp it advertises.
        CHECK(out[0].real() < 0.02f);
        bool monotone = true;
        for (std::size_t i = 1; i <= ramp; ++i) {
            if (out[i].real() < out[i - 1].real() - 1e-6f) { monotone = false; }
        }
        CHECK(monotone);
        CHECK_NEAR(out[ramp].real(), 1.0, 0.001);

        // THE SHAPING ITSELF. A raised cosine leaves and arrives with zero
        // slope, so the largest step anywhere is pi/2 times the average - a
        // linear ramp would have every step equal to the average, and a hard
        // key would have one step of 1.0.
        double worstStep = 0.0;
        for (std::size_t i = 1; i <= ramp; ++i) {
            worstStep = std::max(worstStep,
                                 static_cast<double>(out[i].real() - out[i - 1].real()));
        }
        const double meanStep = 1.0 / static_cast<double>(ramp);
        std::printf("CW ramp: %zu samples, worst step %.6f, mean step %.6f, ratio %.3f\n", ramp,
                    worstStep, meanStep, worstStep / meanStep);
        CHECK_NEAR(worstStep / meanStep, kPi / 2.0, 0.02);

        // And the first and last steps are the SMALL ones - which is the
        // whole difference between a shaped edge and a linear one.
        const double firstStep = static_cast<double>(out[1].real() - out[0].real());
        CHECK(firstStep < meanStep * 0.2);
    }

    // =====================================================================
    // 3. CW: THE SPLATTER, AGAINST A HARD-KEYED CARRIER
    //
    //    This is the check the mode exists for. A carrier switched on in one
    //    sample is a step, and a step's spectrum reaches everywhere; the
    //    shaped one must be far below it several kilohertz off frequency,
    //    which is where the neighbours are.
    // =====================================================================
    {
        constexpr std::size_t kBurst = 4800;  // 100 ms, key down then up
        Modulator m;
        m.setMode(TxMode::CW);
        std::vector<float> silence(kBurst, 0.0f);
        std::vector<std::complex<float>> shaped(kBurst);
        m.setKeyed(true);
        m.process(silence.data(), kBurst / 2, shaped.data());
        m.setKeyed(false);
        m.process(silence.data(), kBurst / 2, shaped.data() + kBurst / 2);
        // The key really did come back up inside the burst.
        CHECK(m.idle());

        // The same burst with no shaping at all - one sample from nothing to
        // full and back, which is what this mode would be without the ramp.
        std::vector<std::complex<float>> hard(kBurst);
        for (std::size_t i = 0; i < kBurst; ++i) {
            hard[i] = std::complex<float>(i < kBurst / 2 ? 1.0f : 0.0f, 0.0f);
        }

        // THE MEASUREMENT IS A BAND, NOT A BIN, and the first version of this
        // block was a bin - which read -240 dB for the HARD-keyed burst and
        // declared the shaped one worse. The burst was exactly half the
        // window and the probe was exactly on a bin, so the rectangular
        // pulse's own transform had a null precisely there: a sinc has zeros
        // at every bin of the window it fills, and the check had landed on
        // one. Averaging the power across a band at frequencies that are NOT
        // bins of this window measures the splatter rather than one of its
        // zeros, and is what a receiver two channels away actually hears.
        const auto bandDb = [](const std::vector<std::complex<float>>& x) {
            double acc = 0.0;
            int n = 0;
            for (double f = 4000.0; f <= 6000.0; f += 37.3) {
                const double mag = magAt(x, f);
                acc += mag * mag;
                ++n;
            }
            return 10.0 * std::log10(std::max(1e-24, acc / std::max(1, n)));
        };
        // 4 to 6 kHz off the carrier: several channels away on any CW band
        // plan, and squarely in somebody else's contact.
        const double shapedDb = bandDb(shaped);
        const double hardDb = bandDb(hard);
        std::printf("CW power 4-6 kHz off: shaped %.1f dB, hard-keyed %.1f dB, %.1f dB better\n",
                    shapedDb, hardDb, hardDb - shapedDb);
        CHECK(hardDb - shapedDb > 30.0);
    }

    // =====================================================================
    // 4. AM: CARRIER PLUS TWO SIDEBANDS
    //
    //    (1 + m*a)/2 with a = sin(2 pi 1000 t) gives a carrier of 1/2 and two
    //    sidebands of m/4 each - the closed form, and the reason the ratio
    //    below is m/2.
    // =====================================================================
    {
        Modulator m;
        m.setMode(TxMode::AM);
        m.setKeyed(true);
        const std::vector<float> a = tone(1000.0, 1.0, 9600);
        const std::vector<std::complex<float>> out = steady(m, a, 4800, 4800);

        const double carrier = magAt(out, 0.0);
        const double upper = magAt(out, 1000.0);
        const double lower = magAt(out, -1000.0);
        std::printf("AM: carrier %.4f, upper %.4f, lower %.4f, ratio %.4f (m/2 = %.4f)\n",
                    carrier, upper, lower, upper / carrier, kAmDepth / 2.0);
        CHECK_NEAR(carrier, 0.5, 0.005);
        CHECK_NEAR(upper, kAmDepth / 4.0, 0.005);
        // BOTH SIDEBANDS, AND EQUAL. An AM signal with one sideband is not AM,
        // and the symmetry is what says the output is real.
        CHECK_NEAR(lower, upper, 1e-6);
        CHECK_NEAR(upper / carrier, kAmDepth / 2.0, 0.01);

        // Nothing leaves the unit circle, which is what packSample() would
        // otherwise have to clip.
        double worst = 0.0;
        for (const std::complex<float>& c : out) {
            worst = std::max(worst, static_cast<double>(std::abs(c)));
        }
        CHECK(worst <= 1.0);
    }

    // =====================================================================
    // 5. NFM: THE BESSEL SIDEBANDS
    //
    //    An FM carrier modulated by a single tone has amplitudes J_n(beta) at
    //    n tones either side, with beta = deviation / modulating frequency.
    //    2500 Hz on a 1 kHz tone is beta = 2.5, whose first four are in the
    //    table above. This is the measurement that says the phase really is
    //    accumulating at 2*pi*dev*a/fs and not at some other constant.
    //
    //    PRE-EMPHASIS OFF for this block, because it is a filter in front of
    //    the deviation and the closed form does not have one. That it is
    //    switchable at all is checked in block 7.
    // =====================================================================
    {
        Modulator m;
        m.setMode(TxMode::NFM);
        m.setPreemphasis(false);
        m.setKeyed(true);
        const std::vector<float> a = tone(1000.0, 1.0, 9600);
        const std::vector<std::complex<float>> out = steady(m, a, 4800, 4800);

        const double c0 = magAt(out, 0.0);
        const double c1 = magAt(out, 1000.0);
        const double c2 = magAt(out, 2000.0);
        const double c3 = magAt(out, 3000.0);
        std::printf("NFM beta=2.5: carrier %.4f (J0 %.4f), 1k %.4f (J1 %.4f), 2k %.4f (J2 "
                    "%.4f), 3k %.4f (J3 %.4f)\n",
                    c0, kJ0at2p5, c1, kJ1at2p5, c2, kJ2at2p5, c3, kJ3at2p5);
        CHECK_NEAR(c0, kJ0at2p5, 0.01);
        CHECK_NEAR(c1, kJ1at2p5, 0.01);
        CHECK_NEAR(c2, kJ2at2p5, 0.01);
        CHECK_NEAR(c3, kJ3at2p5, 0.01);

        // CONSTANT ENVELOPE, which is the other half of what makes it FM: an
        // amplitude that moved would be AM riding on it.
        double lo = 2.0;
        double hi = 0.0;
        for (const std::complex<float>& c : out) {
            const double mag = std::abs(c);
            lo = std::min(lo, mag);
            hi = std::max(hi, mag);
        }
        std::printf("NFM envelope: %.6f to %.6f\n", lo, hi);
        CHECK(hi - lo < 1e-3);
        CHECK_NEAR(hi, 1.0, 1e-3);
    }

    // =====================================================================
    // 6. NFM: THE DEVIATION LIMIT
    //
    //    Somebody shouting is the case this exists for. Five times full scale
    //    in, and the deviation must still be 2.5 kHz - measured from the
    //    OUTPUT's own phase, not from the modulator's bookkeeping, because
    //    the bookkeeping is the thing that could be lying.
    // =====================================================================
    {
        Modulator m;
        m.setMode(TxMode::NFM);
        m.setPreemphasis(false);
        m.setKeyed(true);
        const std::vector<float> a = tone(1000.0, 5.0, 9600);
        const std::vector<std::complex<float>> out = steady(m, a, 4800, 4800);

        double worstHz = 0.0;
        for (std::size_t i = 1; i < out.size(); ++i) {
            const double p0 = std::atan2(out[i - 1].imag(), out[i - 1].real());
            double d = std::atan2(out[i].imag(), out[i].real()) - p0;
            while (d > kPi) { d -= 2.0 * kPi; }
            while (d < -kPi) { d += 2.0 * kPi; }
            worstHz = std::max(worstHz, std::fabs(d) * kFs / (2.0 * kPi));
        }
        std::printf("NFM with a 5x overdrive: worst deviation %.1f Hz (limit %.1f Hz), "
                    "modulator reported %.1f Hz\n",
                    worstHz, kNfmDeviationHz, m.lastPeakDeviationHz());
        // A hair of tolerance for the phase difference's own rounding at
        // float precision; the limit is 2500 and an unlimited modulator would
        // read 12500.
        CHECK(worstHz <= kNfmDeviationHz + 2.0);
        CHECK(worstHz > kNfmDeviationHz - 50.0);  // it really is being driven that hard
        CHECK_NEAR(m.lastPeakDeviationHz(), kNfmDeviationHz, 1.0);
    }

    // =====================================================================
    // 7. NFM: THE PRE-EMPHASIS DOES SOMETHING, AND IT IS THE RIGHT SOMETHING
    //
    //    A rising response: the same amplitude at 3 kHz must produce more
    //    deviation than at 300 Hz. With it off they must produce the SAME,
    //    which is the half that catches a switch that is wired to nothing.
    //    Driven gently so the limiter is not what is being measured.
    // =====================================================================
    {
        const double amp = 0.15;
        double devOn[2] = {0.0, 0.0};
        double devOff[2] = {0.0, 0.0};
        const double freqs[2] = {300.0, 3000.0};
        for (int i = 0; i < 2; ++i) {
            for (int pre = 0; pre < 2; ++pre) {
                Modulator m;
                m.setMode(TxMode::NFM);
                m.setPreemphasis(pre != 0);
                m.setKeyed(true);
                const std::vector<float> a = tone(freqs[i], amp, 9600);
                std::vector<std::complex<float>> out(a.size());
                m.process(a.data(), a.size(), out.data());
                (pre != 0 ? devOn : devOff)[i] = m.lastPeakDeviationHz();
            }
        }
        std::printf("pre-emphasis off: 300 Hz -> %.1f Hz, 3 kHz -> %.1f Hz\n", devOff[0],
                    devOff[1]);
        std::printf("pre-emphasis on:  300 Hz -> %.1f Hz, 3 kHz -> %.1f Hz (%.1f dB of tilt)\n",
                    devOn[0], devOn[1], 20.0 * std::log10(devOn[1] / devOn[0]));
        // Flat without it...
        CHECK_NEAR(devOff[0], devOff[1], 2.0);
        // ...and tilted with it, by something worth having: a 750 us
        // pre-emphasis is +6 dB an octave above its 212 Hz corner, so 300 Hz
        // to 3 kHz is a little over three octaves of it.
        CHECK(devOn[1] > devOn[0] * 4.0);
        // And normalised at 1 kHz rather than at DC: a filter normalised at
        // DC would make the whole voice band far quieter than the same audio
        // with the pre-emphasis switched out, and the limiter would then
        // never engage.
        CHECK(devOn[1] > devOff[1]);
    }

    // =====================================================================
    // 8. USB AND LSB: ONE SIDEBAND, AND THE RIGHT ONE
    //
    //    The failure this catches is a transmitter putting a voice on the
    //    wrong side of the dial - unreadable to whoever is listening, and
    //    sitting on whatever is over there.
    // =====================================================================
    {
        for (int pass = 0; pass < 2; ++pass) {
            const bool upper = (pass == 0);
            Modulator m;
            m.setMode(upper ? TxMode::USB : TxMode::LSB);
            m.setKeyed(true);
            const std::vector<float> a = tone(1000.0, 1.0, 19200);
            // Settled well past the Hilbert transformer's own delay as well
            // as the envelope ramp.
            const std::vector<std::complex<float>> out = steady(m, a, 9600, 9600);

            const double wanted = magAt(out, upper ? 1000.0 : -1000.0);
            const double unwanted = magAt(out, upper ? -1000.0 : 1000.0);
            const double rejectDb = 20.0 * std::log10(wanted / std::max(1e-12, unwanted));
            std::printf("%s: wanted %.4f, opposite %.6f, %.1f dB of suppression\n",
                        upper ? "USB" : "LSB", wanted, unwanted, rejectDb);
            // A 129-tap Blackman-windowed transformer; 40 dB is what it is
            // for, and a modulator with the sign of the quadrature arm wrong
            // reads 0 dB here (the two are equal) or negative.
            CHECK(rejectDb > 40.0);
            // HALF SCALE ON A FULL-SCALE TONE, and the number is 0.5 rather
            // than the 0.25 a real tone would give: the analytic signal puts
            // ALL of a real tone's power into one complex line instead of
            // splitting it between a positive and a negative one. That is the
            // whole efficiency argument for single sideband, arriving here as
            // an amplitude.
            CHECK_NEAR(wanted, 0.5, 0.01);
            // NO CARRIER. It is the mode's defining property and the reason
            // it is efficient.
            CHECK(magAt(out, 0.0) < 0.002);
        }
    }

    // =====================================================================
    // 9. THE KEY, AND WHAT A MODE CHANGE DOES NOT DO TO IT
    // =====================================================================
    {
        Modulator m;
        m.setMode(TxMode::USB);
        CHECK(m.idle());
        m.setKeyed(true);
        CHECK(!m.idle());
        std::vector<float> a = tone(1000.0, 0.5, 960);
        std::vector<std::complex<float>> out(a.size());
        m.process(a.data(), a.size(), out.data());
        // A mode change mid-transmission must not restart the envelope: the
        // ramp is about the key, not about the modulation.
        m.setMode(TxMode::NFM);
        CHECK(!m.idle());
        m.process(a.data(), a.size(), out.data());
        CHECK_NEAR(std::abs(out[0]), 1.0, 0.01);

        // reset() puts the key UP and the envelope at zero - a modulator that
        // was reset mid-transmission must not resume mid-word.
        m.reset();
        CHECK(m.idle());
        CHECK(!m.keyed());
    }

    // =====================================================================
    // 10. NaN IN, SILENCE OUT
    //
    //     A dead microphone, a divide by zero upstream, a driver handing back
    //     an uninitialised buffer: whatever the cause, a number that is not
    //     one must not reach the DAC. Every mode is checked, because each
    //     handles it in its own arithmetic.
    // =====================================================================
    {
        const double nan = std::nan("");
        for (int i = 0; i < cascade::dsp::kTxModeCount; ++i) {
            Modulator m;
            m.setMode(cascade::dsp::txModeFromIndex(i));
            m.setKeyed(true);
            std::vector<float> a(2400, static_cast<float>(nan));
            std::vector<std::complex<float>> out(a.size());
            m.process(a.data(), a.size(), out.data());
            bool finite = true;
            for (const std::complex<float>& c : out) {
                if (!std::isfinite(c.real()) || !std::isfinite(c.imag())) { finite = false; }
                if (std::abs(c) > 1.0001f) { finite = false; }
            }
            if (!finite) {
                std::printf("     mode %s produced something that is not a sample\n",
                            cascade::dsp::txModeName(cascade::dsp::txModeFromIndex(i)));
            }
            CHECK(finite);
        }
    }

    // =====================================================================
    // 11. THE TEST TONE
    // =====================================================================
    {
        ToneGenerator t;
        t.setSampleRateHz(kFs);
        t.setFrequencyHz(1000.0);
        t.setLevel(0.5f);
        std::vector<float> a(4800);
        t.generate(a.data(), a.size());

        // Its own spectrum, through the same transform - a real tone, so half
        // the amplitude lands in each of the two bins.
        std::vector<std::complex<float>> c(a.size());
        for (std::size_t i = 0; i < a.size(); ++i) { c[i] = std::complex<float>(a[i], 0.0f); }
        CHECK_NEAR(std::abs(dftAt(c.data(), c.size(), 1000.0)), 0.25, 0.001);

        // Level clamped both ways, and NaN lands on silence rather than on
        // whatever a cast would have made of it.
        t.setLevel(5.0f);
        CHECK(t.level() == 1.0f);
        t.setLevel(-1.0f);
        CHECK(t.level() == 0.0f);
        t.setLevel(std::nanf(""));
        CHECK(t.level() == 0.0f);

        // A frequency above Nyquist is FOLDED, not accepted: a control that
        // silently produced the alias would be lettering a frequency that is
        // not what is on the air.
        t.setFrequencyHz(30000.0);
        CHECK_NEAR(t.frequencyHz(), kFs / 2.0, 1e-9);
        t.setFrequencyHz(-100.0);
        CHECK(t.frequencyHz() == 0.0);
    }

    return testSummary("test_modulator");
}
