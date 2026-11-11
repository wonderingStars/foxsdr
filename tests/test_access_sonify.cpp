// Tests for core/access_sonify.hpp: the sonification block. Numbers, not
// listening: pitch, pan, beep rate and smoothing are checked arithmetically,
// the renderer's frequency and gating are MEASURED on the samples it writes,
// and the spectrum-peak variant is run on a generated noisy tone at fractional
// bin positions. No audio device is opened anywhere.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/access_sonify.hpp"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "test_check.hpp"

namespace {

using namespace cascade::core::access;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

void curveMapping() {
    LevelCurve c;   // default: linear -120 .. -20
    CHECK_NEAR(c.map(-120.0f), 0.0, 1e-6);
    CHECK_NEAR(c.map(-20.0f), 1.0, 1e-6);
    CHECK_NEAR(c.map(-70.0f), 0.5, 1e-6);
    CHECK_NEAR(c.map(-200.0f), 0.0, 1e-6);     // clamped, never extrapolated
    CHECK_NEAR(c.map(+30.0f), 1.0, 1e-6);
    CHECK(std::isnan(c.map(kNaN)));
    CHECK(std::isnan(c.map(INFINITY)));

    c.setLinear(-100.0f, -40.0f);
    CHECK_NEAR(c.map(-70.0f), 0.5, 1e-6);
    c.setLinear(-40.0f, -40.0f);               // degenerate: falls back, never divides by zero
    CHECK_NEAR(c.map(-70.0f), 0.5, 1e-6);
    c.setLinear(kNaN, 0.0f);
    CHECK_NEAR(c.map(-70.0f), 0.5, 1e-6);

    // A user curve: flat to -90, then a steep rise to -60, then flat.
    c.setPoints({{-60.0f, 1.0f}, {-90.0f, 0.0f}, {-95.0f, 0.0f}});  // unsorted on purpose
    CHECK_NEAR(c.map(-100.0f), 0.0, 1e-6);
    CHECK_NEAR(c.map(-92.0f), 0.0, 1e-6);
    CHECK_NEAR(c.map(-75.0f), 0.5, 1e-6);
    CHECK_NEAR(c.map(-60.0f), 1.0, 1e-6);
    CHECK_NEAR(c.map(-10.0f), 1.0, 1e-6);
    CHECK(c.points().size() == 3);

    // t clamps, non-finite points drop, duplicates collapse, too few = default.
    c.setPoints({{-80.0f, -3.0f}, {-40.0f, 9.0f}, {kNaN, 0.5f}, {-30.0f, kNaN}});
    CHECK(c.points().size() == 2);
    CHECK_NEAR(c.map(-80.0f), 0.0, 1e-6);
    CHECK_NEAR(c.map(-40.0f), 1.0, 1e-6);
    c.setPoints({{-80.0f, 0.2f}, {-80.0f, 0.9f}});
    CHECK_NEAR(c.map(-70.0f), 0.5, 1e-6);       // back to the default -120..-20 line
    c.setPoints({});
    CHECK(c.points().size() == 2);

    // Monotone points give a monotone map (a random sweep).
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    std::vector<CurvePoint> pts;
    float tt = 0.0f;
    for (int i = 0; i < 12; ++i) { tt = std::min(1.0f, tt + u(rng) * 0.1f); pts.push_back({-110.0f + 8.0f * static_cast<float>(i), tt}); }
    c.setPoints(pts);
    float prev = -1.0f;
    bool mono = true;
    for (float db = -130.0f; db <= 0.0f; db += 0.25f) { const float t = c.map(db); if (t + 1e-6f < prev) { mono = false; } prev = t; }
    CHECK(mono);
}

void pitchAndRate() {
    SonifySettings s;   // 220 .. 1760 Hz, log
    CHECK_NEAR(pitchForT(s, 0.0f), 220.0, 1e-3);
    CHECK_NEAR(pitchForT(s, 1.0f), 1760.0, 1e-2);
    CHECK_NEAR(pitchForT(s, 0.5f), std::sqrt(220.0 * 1760.0), 1e-2);          // geometric mean
    CHECK_NEAR(pitchForT(s, 1.0f / 3.0f), 440.0, 1e-2);                       // one octave per third
    CHECK_NEAR(pitchForT(s, -4.0f), 220.0, 1e-3);                              // clamped
    CHECK_NEAR(pitchForT(s, 9.0f), 1760.0, 1e-2);
    s.logPitch = false;
    CHECK_NEAR(pitchForT(s, 0.5f), 990.0, 1e-3);
    s.maxHz = 100.0f;                                                          // below min: pinned, not inverted
    CHECK_NEAR(pitchForT(s, 0.5f), 220.0, 1e-3);
    s.minHz = -5.0f; s.maxHz = 1000.0f; s.logPitch = true;                     // nonsense min: held at 1 Hz
    CHECK(std::isfinite(pitchForT(s, 0.5f)) && pitchForT(s, 0.0f) >= 1.0f);

    SonifySettings r;
    r.minRateHz = 2.0f; r.maxRateHz = 12.0f;
    CHECK_NEAR(rateForT(r, 0.0f), 2.0, 1e-6);
    CHECK_NEAR(rateForT(r, 0.5f), 7.0, 1e-6);
    CHECK_NEAR(rateForT(r, 1.0f), 12.0, 1e-6);
    r.minRateHz = r.maxRateHz = 0.0f;
    CHECK_NEAR(rateForT(r, 0.7f), 0.0, 1e-6);
}

void smoothing() {
    // Step from -100 dB (t = 0.2) to -40 dB (t = 0.8). Attack 30 ms: at 30 ms the
    // smoothed level has covered 1 - 1/e = 63.2% of the step.
    SonifySettings s;
    s.attackMs = 30.0f; s.releaseMs = 250.0f;
    {
        Sonifier a(s);
        a.update(-100.0f, true, 0.0f, 1.0f);              // first update snaps
        ToneParams p;
        for (int ms = 0; ms < 30; ++ms) { p = a.update(-40.0f, true, 0.0f, 1.0f); }
        const double frac = (static_cast<double>(p.t) - 0.2) / 0.6;
        CHECK_NEAR(frac, 1.0 - std::exp(-1.0), 0.01);
    }
    {   // the same step at a coarser update rate lands on the same value
        Sonifier a(s);
        a.update(-100.0f, true, 0.0f, 10.0f);
        ToneParams p;
        for (int i = 0; i < 3; ++i) { p = a.update(-40.0f, true, 0.0f, 10.0f); }
        const double frac = (static_cast<double>(p.t) - 0.2) / 0.6;
        CHECK_NEAR(frac, 1.0 - std::exp(-1.0), 0.01);
    }
    {   // release is slower than attack: falling 60 dB for 30 ms covers far less
        Sonifier a(s);
        a.update(-40.0f, true, 0.0f, 1.0f);
        ToneParams p;
        for (int ms = 0; ms < 30; ++ms) { p = a.update(-100.0f, true, 0.0f, 1.0f); }
        const double frac = (0.8 - static_cast<double>(p.t)) / 0.6;
        CHECK_NEAR(frac, 1.0 - std::exp(-30.0 / 250.0), 0.01);
        CHECK(frac < 0.2);
    }
    {   // zero time constants follow instantly; zero or negative or NaN dt does not move
        SonifySettings z = s; z.attackMs = 0.0f; z.releaseMs = 0.0f;
        Sonifier a(z);
        a.update(-100.0f, true, 0.0f, 1.0f);
        CHECK_NEAR(a.update(-40.0f, true, 0.0f, 1.0f).t, 0.8, 1e-5);
        Sonifier b(s);
        b.update(-100.0f, true, 0.0f, 1.0f);
        CHECK_NEAR(b.update(-40.0f, true, 0.0f, 0.0f).t, 0.2, 1e-6);
        CHECK_NEAR(b.update(-40.0f, true, 0.0f, -5.0f).t, 0.2, 1e-6);
        CHECK_NEAR(b.update(-40.0f, true, 0.0f, kNaN).t, 0.2, 1e-6);
    }
    {   // reset snaps again
        Sonifier a(s);
        a.update(-100.0f, true, 0.0f, 1.0f);
        a.reset();
        CHECK_NEAR(a.update(-40.0f, true, 0.0f, 1.0f).t, 0.8, 1e-6);
    }
}

void squelchAndUnknown() {
    SonifySettings s;
    Sonifier a(s);
    // Nothing ever measured: silent, whatever the squelch says.
    CHECK(!a.update(kNaN, true, 0.0f, 10.0f).audible);
    // Open and strong: audible, amplitude within (0, max].
    ToneParams p = a.update(-40.0f, true, 0.0f, 10.0f);
    CHECK(p.audible);
    CHECK(p.amplitude > 0.0f && p.amplitude <= s.maxAmplitude + 1e-6f);
    // Closed squelch mutes...
    p = a.update(-40.0f, false, 0.0f, 10.0f);
    CHECK(!p.audible);
    CHECK_NEAR(p.amplitude, 0.0, 1e-9);
    // ...and the smoother kept tracking: the level fell to -100 while closed, and
    // opening shows the CURRENT level, not the -40 it closed on.
    for (int i = 0; i < 400; ++i) { a.update(-100.0f, false, 0.0f, 10.0f); }
    p = a.update(-100.0f, true, 0.0f, 10.0f);
    CHECK(p.t < 0.25f);
    // A mute-when-closed switch off: closed squelch still sounds.
    SonifySettings open = s; open.muteWhenSquelchClosed = false;
    Sonifier b(open);
    CHECK(b.update(-40.0f, false, 0.0f, 10.0f).audible);
    // A lost reading (NaN) mutes and holds the smoothed value.
    Sonifier c(s);
    c.update(-40.0f, true, 0.0f, 10.0f);
    const ToneParams lost = c.update(kNaN, true, 0.0f, 10.0f);
    CHECK(!lost.audible);
    CHECK_NEAR(lost.t, 0.8, 1e-5);
    // silenceBelowT: weak is silent.
    SonifySettings quiet = s; quiet.silenceBelowT = 0.3f;
    Sonifier d(quiet);
    CHECK(!d.update(-100.0f, true, 0.0f, 10.0f).audible);   // t = 0.2
    CHECK(d.update(-40.0f, true, 0.0f, 10.0f).audible);     // t rising
}

void panMapping() {
    SonifySettings s;
    Sonifier a(s);
    CHECK_NEAR(a.update(-60.0f, true, -1.0f, 1.0f).pan, -1.0, 1e-6);
    CHECK_NEAR(a.update(-60.0f, true, 1.0f, 1.0f).pan, 1.0, 1e-6);
    CHECK_NEAR(a.update(-60.0f, true, 0.25f, 1.0f).pan, 0.25, 1e-6);
    CHECK_NEAR(a.update(-60.0f, true, 7.0f, 1.0f).pan, 1.0, 1e-6);    // clamped
    CHECK_NEAR(a.update(-60.0f, true, kNaN, 1.0f).pan, 0.0, 1e-6);    // unknown: centre
    s.panWidth = 0.0f;
    Sonifier b(s);
    CHECK_NEAR(b.update(-60.0f, true, 1.0f, 1.0f).pan, 0.0, 1e-6);
    s.panWidth = 0.5f;
    Sonifier c(s);
    CHECK_NEAR(c.update(-60.0f, true, 1.0f, 1.0f).pan, 0.5, 1e-6);
}

// Naive DFT magnitude (dB) of `n` real samples with a Hann window, bins 0..n/2-1.
std::vector<float> dftDb(const std::vector<double>& x) {
    const std::size_t n = x.size();
    std::vector<float> out(n / 2);
    std::vector<double> w(n);
    for (std::size_t i = 0; i < n; ++i) { w[i] = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * static_cast<double>(i) / static_cast<double>(n)); }
    for (std::size_t k = 0; k < n / 2; ++k) {
        double re = 0.0, im = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double a = 2.0 * 3.14159265358979323846 * static_cast<double>(k * i % n) / static_cast<double>(n);
            re += x[i] * w[i] * std::cos(a);
            im -= x[i] * w[i] * std::sin(a);
        }
        out[k] = static_cast<float>(10.0 * std::log10(re * re + im * im + 1e-30));
    }
    return out;
}

std::vector<float> noisyToneSpectrum(std::size_t n, double bin, double amp, double sigma, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> g(0.0, sigma);
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = amp * std::sin(2.0 * 3.14159265358979323846 * bin * static_cast<double>(i) / static_cast<double>(n) + 0.7) + g(rng);
    }
    return dftDb(x);
}

void spectrumPeak() {
    const std::size_t N = 1024;   // 512 bins
    // A tone at fractional bin positions, +20 dB over the noise per bin or so.
    const double bins[] = {100.0, 100.25, 100.5, 100.37, 255.8, 7.3, 480.45};
    std::uint64_t seed = 1;
    for (double b : bins) {
        const std::vector<float> sp = noisyToneSpectrum(N, b, 1.0, 0.5, seed++);
        const SpectrumPeak p = findSpectrumPeak(sp.data(), sp.size(), 14.0f);
        CHECK(p.found);
        const double found = static_cast<double>(p.position) * static_cast<double>(sp.size() - 1);
        CHECK_NEAR(found, b, 0.2);          // sub-bin: better than half a bin, with noise
        CHECK(p.overFloorDb > 25.0f);
        CHECK(p.peakDb > p.floorDb);
    }

    // Noise only: count false peaks over 2000 frames at the 14 dB threshold.
    int falsePeaks = 0;
    std::mt19937_64 rng(99);
    std::normal_distribution<double> g(0.0, 1.0);
    std::vector<float> fr(512);
    for (int f = 0; f < 2000; ++f) {
        for (float& v : fr) {   // complex gaussian bin: power exponentially distributed
            const double re = g(rng), im = g(rng);
            v = static_cast<float>(10.0 * std::log10(re * re + im * im + 1e-30));
        }
        if (findSpectrumPeak(fr.data(), fr.size(), 14.0f).found) { ++falsePeaks; }
    }
    CHECK(falsePeaks == 0);

    // Edges and bad input.
    CHECK(!findSpectrumPeak(nullptr, 10, 10.0f).found);
    const float two[2] = {1.0f, 2.0f};
    CHECK(!findSpectrumPeak(two, 2, 0.0f).found);
    std::vector<float> nanBins(64, kNaN);
    CHECK(!findSpectrumPeak(nanBins.data(), nanBins.size(), 0.0f).found);
    std::vector<float> flat(64, -90.0f);
    CHECK(!findSpectrumPeak(flat.data(), flat.size(), 1.0f).found);       // a flat spectrum has no peak
    std::vector<float> edge(64, -90.0f); edge[0] = -30.0f;                // peak on the first bin: no neighbour to interpolate
    SpectrumPeak e = findSpectrumPeak(edge.data(), edge.size(), 20.0f);
    CHECK(e.found && e.position == 0.0f);
    edge.assign(64, -90.0f); edge[63] = -30.0f;
    e = findSpectrumPeak(edge.data(), edge.size(), 20.0f);
    CHECK(e.found && e.position == 1.0f);
    std::vector<float> withNan(64, -90.0f); withNan[10] = kNaN; withNan[30] = -40.0f;
    e = findSpectrumPeak(withNan.data(), withNan.size(), 20.0f);
    CHECK(e.found && std::fabs(e.position * 63.0f - 30.0f) < 0.6f);
}

void spectrumTone() {
    const std::size_t N = 1024;
    SpectrumSonifySettings s;
    const std::vector<float> left = noisyToneSpectrum(N, 40.2, 1.0, 0.1, 11);
    const std::vector<float> right = noisyToneSpectrum(N, 460.7, 1.0, 0.1, 12);
    const ToneParams a = sonifySpectrum(left.data(), left.size(), s);
    const ToneParams b = sonifySpectrum(right.data(), right.size(), s);
    CHECK(a.audible && b.audible);
    CHECK(a.freqHz < b.freqHz);                              // the pitch says WHERE
    CHECK_NEAR(a.freqHz, 220.0 * std::pow(1760.0 / 220.0, 40.2 / 511.0), 5.0);
    CHECK(a.pan < -0.7f && b.pan > 0.7f);
    // Strength: a louder tone over the same floor raises level.
    const std::vector<float> weak = noisyToneSpectrum(N, 200.0, 0.2, 0.1, 13);
    const std::vector<float> strong = noisyToneSpectrum(N, 200.0, 2.0, 0.1, 14);
    const ToneParams w = sonifySpectrum(weak.data(), weak.size(), s);
    const ToneParams st = sonifySpectrum(strong.data(), strong.size(), s);
    CHECK(w.audible && st.audible);
    CHECK(st.amplitude > w.amplitude);
    CHECK(st.t > w.t);
    // No peak: silence.
    std::vector<float> flat(512, -90.0f);
    CHECK(!sonifySpectrum(flat.data(), flat.size(), s).audible);
    CHECK(!sonifySpectrum(nullptr, 0, s).audible);
    // Beep rate follows strength.
    s.minRateHz = 1.0f; s.maxRateHz = 9.0f;
    CHECK(sonifySpectrum(strong.data(), strong.size(), s).beepRateHz > sonifySpectrum(weak.data(), weak.size(), s).beepRateHz);
}

double zeroCrossHz(const std::vector<float>& left, float sr) {
    int crossings = 0;
    std::size_t first = 0, last = 0;
    for (std::size_t i = 1; i < left.size(); ++i) {
        if (left[i - 1] < 0.0f && left[i] >= 0.0f) { if (crossings == 0) { first = i; } last = i; ++crossings; }
    }
    if (crossings < 2) { return 0.0; }
    return static_cast<double>(crossings - 1) * sr / static_cast<double>(last - first);
}

void renderer() {
    const float sr = 48000.0f;
    // Frequency measured on the samples, several pitches.
    for (float hz : {220.0f, 440.0f, 1000.0f, 1760.0f}) {
        ToneRenderer r(sr);
        ToneParams p; p.audible = true; p.freqHz = hz; p.amplitude = 0.3f; p.pan = 0.0f;
        std::vector<float> st(2 * 48000), l(48000);
        r.render(p, 48000, st.data());
        for (std::size_t i = 0; i < 48000; ++i) { l[i] = st[2 * i]; }
        CHECK_NEAR(zeroCrossHz(l, sr), hz, hz * 0.002);
    }
    // Centre pan: both channels equal at A*cos(pi/4); peak 0.3 * 0.7071.
    {
        ToneRenderer r(sr);
        ToneParams p; p.audible = true; p.freqHz = 440.0f; p.amplitude = 0.3f;
        std::vector<float> st(2 * 9600);
        r.render(p, 9600, st.data());
        float lmax = 0, rmax = 0;
        for (std::size_t i = 2000; i < 9600; ++i) { lmax = std::max(lmax, std::fabs(st[2 * i])); rmax = std::max(rmax, std::fabs(st[2 * i + 1])); }
        CHECK_NEAR(lmax, 0.3 * 0.70710678, 0.003);
        CHECK_NEAR(rmax, 0.3 * 0.70710678, 0.003);
    }
    // Equal-power law across the whole pan sweep, and hard sides.
    for (float pan = -1.0f; pan <= 1.0f; pan += 0.125f) {
        ToneRenderer r(sr);
        ToneParams p; p.audible = true; p.freqHz = 440.0f; p.amplitude = 0.3f; p.pan = pan;
        std::vector<float> st(2 * 9600);
        r.render(p, 9600, st.data());
        double el = 0, er = 0;
        for (std::size_t i = 4800; i < 9600; ++i) { el += st[2 * i] * st[2 * i]; er += st[2 * i + 1] * st[2 * i + 1]; }
        CHECK_NEAR((el + er) / 4800.0, 0.3 * 0.3 / 2.0, 0.0005);
        if (pan == -1.0f) { CHECK(er < 1e-6 * el); }
        if (pan == 1.0f) { CHECK(el < 1e-6 * er); }
    }
    // Block size does not matter: one call and many are sample-identical.
    {
        ToneRenderer one(sr), many(sr);
        ToneParams p; p.audible = true; p.freqHz = 523.25f; p.amplitude = 0.25f; p.pan = 0.3f;
        p.beepRateHz = 6.0f; p.duty = 0.5f;
        std::vector<float> a(2 * 24000), b(2 * 24000);
        one.render(p, 24000, a.data());
        std::size_t done = 0;
        const std::size_t chunks[] = {1, 7, 480, 1000, 333, 21000, 1179};
        for (std::size_t c : chunks) { many.render(p, c, b.data() + 2 * done); done += c; }
        CHECK(done == 24000);
        CHECK(a == b);
    }
    // Muting never clicks: every sample-to-sample step stays small, and the
    // output is exactly zero within 10 ms.
    {
        ToneRenderer r(sr);
        ToneParams on; on.audible = true; on.freqHz = 1000.0f; on.amplitude = 0.3f; on.pan = 1.0f;
        // 4813 samples, not 4800: 4800 is exactly 100 cycles of 1 kHz, which would
        // end the audible block on a zero crossing and hide a click.
        std::vector<float> st(2 * 4813), off(2 * 4800);
        r.render(on, 4813, st.data());
        ToneParams mute = on; mute.audible = false; mute.amplitude = 0.0f;
        r.render(mute, 4800, off.data());
        float prev = st[2 * 4812 + 1], worst = 0.0f;
        for (std::size_t i = 0; i < 4800; ++i) { worst = std::max(worst, std::fabs(off[2 * i + 1] - prev)); prev = off[2 * i + 1]; }
        CHECK(worst < 0.045f);                          // the sine's own steepest step is 0.039; an instant cut would jump by up to 0.3
        for (std::size_t i = 480; i < 4800; ++i) { CHECK(off[2 * i + 1] == 0.0f && off[2 * i] == 0.0f); }
        // and un-muting ramps in just as gently
        std::vector<float> back(2 * 4800);
        r.render(on, 4800, back.data());
        prev = 0.0f; worst = 0.0f;
        for (std::size_t i = 0; i < 4800; ++i) { worst = std::max(worst, std::fabs(back[2 * i + 1] - prev)); prev = back[2 * i + 1]; }
        CHECK(worst < 0.045f);
    }
    // Beep gating: 4 beeps per second at 50% duty: about half of 10 ms windows sound.
    {
        ToneRenderer r(sr);
        ToneParams p; p.audible = true; p.freqHz = 880.0f; p.amplitude = 0.3f; p.beepRateHz = 4.0f; p.duty = 0.5f;
        const std::size_t total = 96000;   // 2 s
        std::vector<float> st(2 * total);
        r.render(p, total, st.data());
        int sounding = 0, windows = 0, transitions = 0;
        bool was = false;
        for (std::size_t w0 = 0; w0 + 480 <= total; w0 += 480) {
            float m = 0.0f;
            for (std::size_t i = w0; i < w0 + 480; ++i) { m = std::max(m, std::fabs(st[2 * i])); }
            const bool on = m > 0.05f;
            sounding += on ? 1 : 0; ++windows;
            if (on != was) { ++transitions; was = on; }
        }
        CHECK_NEAR(static_cast<double>(sounding) / windows, 0.5, 0.06);
        CHECK(transitions >= 14 && transitions <= 17);      // 8 beeps = 16 edges in 2 s (first window may start on)
    }
    // Bad sample rate falls back; zero frames is a no-op.
    ToneRenderer bad(-5.0f);
    ToneParams p; p.audible = true; p.freqHz = 440.0f; p.amplitude = 0.3f;
    float one[2] = {7.0f, 7.0f};
    bad.render(p, 0, one);
    CHECK(one[0] == 7.0f && one[1] == 7.0f);
}

void endToEnd() {
    // A level sweep from -110 to -30 dB drives the measured pitch up, monotonically.
    SonifySettings s; s.attackMs = 5.0f; s.releaseMs = 5.0f;
    Sonifier son(s);
    ToneRenderer r(48000.0f);
    double prev = 0.0;
    bool rising = true;
    for (float db = -110.0f; db <= -30.0f; db += 10.0f) {
        ToneParams p;
        for (int k = 0; k < 50; ++k) { p = son.update(db, true, 0.0f, 20.0f); }
        std::vector<float> st(2 * 24000), l(24000);
        // skip the fade-in: render twice, measure the second block
        r.render(p, 24000, st.data());
        r.render(p, 24000, st.data());
        for (std::size_t i = 0; i < 24000; ++i) { l[i] = st[2 * i]; }
        const double hz = zeroCrossHz(l, 48000.0f);
        if (hz <= prev) { rising = false; }
        prev = hz;
        CHECK_NEAR(hz, p.freqHz, p.freqHz * 0.004);
    }
    CHECK(rising);
}

}  // namespace

int main() {
    curveMapping();
    pitchAndRate();
    smoothing();
    squelchAndUnknown();
    panMapping();
    spectrumPeak();
    spectrumTone();
    renderer();
    endToEnd();
    return testSummary("test_access_sonify");
}
