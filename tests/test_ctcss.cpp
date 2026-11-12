// Tests for the CTCSS block (dsp/ctcss.hpp): the tone table, the tone-removing high-pass, and
// the tone detector. See docs/feature-records/signal-blocks.md for the pinned numbers and
// what is proven on generated signals only (NO real CTCSS recording exists in the sample set).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/ctcss.hpp"

#include "alloc_probe.hpp"
#include "signal_block_gen.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {

using cascade::dsp::CtcssDetector;
using cascade::dsp::CtcssHighpass;
using cascade::dsp::kCtcssExtraTones;
using cascade::dsp::kCtcssStandardTones;
using namespace sbgen;

constexpr double kRates[2] = {48000.0, 24000.0};

// Gain of the high-pass at `hz`, in dB, measured on a steady sine (last half of 1.2 s).
double hpGainDb(CtcssHighpass& hp, double fs, double hz) {
    const std::size_t n = static_cast<std::size_t>(1.2 * fs);
    std::vector<float> in(n, 0.0f), out(n);
    addTone(in, fs, hz, 1.0, 0, n);
    hp.reset();
    hp.process(in.data(), out.data(), n);
    const double r = rms(out, n / 2, n) / (1.0 / std::sqrt(2.0));
    return 20.0 * std::log10(std::max(r, 1e-12));
}

// Drives a detector over `v` in 1 ms chunks; returns the time (s) at which the gate first
// became open at or after `fromSample`, or -1.
double firstGateOpen(CtcssDetector& d, const std::vector<float>& v, double fs, std::size_t fromSample) {
    const std::size_t chunk = static_cast<std::size_t>(fs / 1000.0);
    for (std::size_t i = 0; i + chunk <= v.size(); i += chunk) {
        d.process(v.data() + i, chunk);
        if (i >= fromSample && d.gateOpen()) { return static_cast<double>(i + chunk - fromSample) / fs; }
    }
    return -1.0;
}

// Counts rising edges of "a tone is locked" while feeding v (1 ms chunks).
int risingEdges(CtcssDetector& d, const std::vector<float>& v, double fs) {
    const std::size_t chunk = static_cast<std::size_t>(fs / 1000.0);
    int edges = 0;
    bool was = false;
    for (std::size_t i = 0; i + chunk <= v.size(); i += chunk) {
        d.process(v.data() + i, chunk);
        const bool now = d.detectedIndex() >= 0;
        if (now && !was) { ++edges; }
        was = now;
    }
    return edges;
}

void testToneTable() {
    CHECK(kCtcssStandardTones.size() == 39);
    CHECK(kCtcssExtraTones.size() == 11);
    CHECK(std::is_sorted(kCtcssStandardTones.begin(), kCtcssStandardTones.end()));
    CHECK_NEAR(kCtcssStandardTones.front(), 67.0, 1e-9);
    CHECK_NEAR(kCtcssStandardTones.back(), 250.3, 1e-9);
    CHECK(cascade::dsp::ctcssIsStandardTone(69.3));    // the 39 include 69.3
    CHECK(!cascade::dsp::ctcssIsStandardTone(150.0));  // NATO tone: not offered
    CHECK(!cascade::dsp::ctcssIsStandardTone(254.1));  // extra group
    CHECK(cascade::dsp::ctcssIsStandardTone(123.0));
    double prev = 0;
    for (int i = 0; i < cascade::dsp::kCtcssAllCount; ++i) {
        const double hz = cascade::dsp::ctcssToneHz(i);
        CHECK(hz > prev);
        prev = hz;
    }
    CHECK(cascade::dsp::ctcssToneHz(-1) == 0.0);
    CHECK(cascade::dsp::ctcssToneHz(50) == 0.0);
    CHECK_NEAR(cascade::dsp::ctcssToneHz(49), 254.1, 1e-9);
    CHECK(cascade::dsp::ctcssIndexOfHz(254.1) == 49);
    CHECK(cascade::dsp::ctcssIndexOfHz(150.0) == -1);
    for (double t : kCtcssStandardTones) { CHECK(cascade::dsp::ctcssIndexOfHz(t) >= 0); }
}

void testHighpass() {
    // Pinned tolerances (the record states them): 300 Hz within -1.2..+0.1 dB; 400 Hz within 0.35 dB;
    // 1 kHz and 3 kHz within 0.05 dB; every tone from the top one (254.1 Hz) down, and DC, at least 40 dB down (the inverse-Chebyshev stop band is equiripple at 40 dB).
    for (double fs : {48000.0, 24000.0, 12000.0, 200000.0}) {
        CtcssHighpass hp(fs);
        CHECK(hp.configured());
        const double g300 = hpGainDb(hp, fs, 300.0);
        const double g400 = hpGainDb(hp, fs, 400.0);
        const double g1k = hpGainDb(hp, fs, 1000.0);
        const double g3k = hpGainDb(hp, fs, 3000.0);
        const double g254 = hpGainDb(hp, fs, 254.1);
        const double g67 = hpGainDb(hp, fs, 67.0);
        std::printf("  hp fs=%.0f: 300Hz %.3f dB, 400Hz %.3f, 1k %.3f, 3k %.3f, 254.1 %.2f, 67 %.2f\n", fs, g300, g400, g1k, g3k, g254, g67);
        CHECK(g300 >= -1.2 && g300 <= 0.1);
        CHECK(std::fabs(g400) <= 0.35);
        CHECK(std::fabs(g1k) <= 0.05);
        CHECK(std::fabs(g3k) <= 0.05);
        CHECK(g254 <= -40.0);
        CHECK(g67 <= -40.0);
    }
    {
        CtcssHighpass hp(48000.0);
        double worst = -1e9;
        for (double t : kCtcssStandardTones) { worst = std::max(worst, hpGainDb(hp, 48000.0, t)); }
        for (double t : kCtcssExtraTones) { worst = std::max(worst, hpGainDb(hp, 48000.0, t)); }
        std::printf("  hp: worst tone rejection %.2f dB\n", worst);
        CHECK(worst <= -40.0);
    }
    {  // DC is removed.
        CtcssHighpass hp(48000.0);
        std::vector<float> in(48000, 0.5f), out(48000);
        hp.process(in.data(), out.data(), in.size());
        CHECK(std::fabs(out.back()) < 6e-3f);  // 0.5 x 40 dB down = 5e-3
    }
    {  // In place equals out of place; chunking does not matter.
        CtcssHighpass a(48000.0), b(48000.0);
        std::vector<float> in = whiteNoise(10000, 0.3, 7), o1(10000), o2 = in;
        a.process(in.data(), o1.data(), in.size());
        for (std::size_t i = 0; i < in.size(); i += 333) { b.process(o2.data() + i, o2.data() + i, std::min<std::size_t>(333, in.size() - i)); }
        bool same = true;
        for (std::size_t i = 0; i < in.size(); ++i) { same = same && (o1[i] == o2[i]); }
        CHECK(same);
    }
    {  // Refused rates leave a pass-through; empty input is fine; non-finite input does not poison.
        CtcssHighpass hp;
        CHECK(!hp.configure(1000.0));
        CHECK(!hp.configure(std::numeric_limits<double>::quiet_NaN()));
        CHECK(!hp.configure(-48000.0));
        CHECK(!hp.configured());
        float in[3] = {0.1f, 0.2f, 0.3f}, out[3] = {};
        hp.process(in, out, 3);
        CHECK(out[1] == 0.2f);
        CHECK(hp.configure(48000.0));
        hp.process(in, out, 0);
        float bad[4] = {std::numeric_limits<float>::quiet_NaN(), 0.2f, std::numeric_limits<float>::infinity(), 0.1f};
        float bo[4] = {};
        hp.process(bad, bo, 4);
        bool finite = true;
        for (float x : bo) { finite = finite && std::isfinite(x); }
        CHECK(finite);
        std::vector<float> more(2000, 0.0f), mo(2000);
        hp.process(more.data(), mo.data(), more.size());
        CHECK(std::isfinite(mo.back()) && std::fabs(mo.back()) < 1e-3f);
    }
    {  // Speech above 300 Hz is intact: filtering already-filtered speech again changes its level by < 0.5 dB.
        CtcssHighpass hp(48000.0), hp2(48000.0);
        std::vector<float> sp = speechLike(48000 * 3, 48000.0, 0.5, 11), once(sp.size()), twice(sp.size());
        hp.process(sp.data(), once.data(), sp.size());
        hp2.process(once.data(), twice.data(), once.size());
        const double r1 = rms(once, 4800, once.size()), r2 = rms(twice, 4800, twice.size());
        CHECK(std::fabs(20.0 * std::log10(r2 / r1)) < 0.5);
    }
    {  // No allocation in process().
        CtcssHighpass hp(48000.0);
        std::vector<float> in = whiteNoise(4800, 0.3, 3), out(4800);
        AllocProbe probe;
        for (int i = 0; i < 20; ++i) { hp.process(in.data(), out.data(), in.size()); }
        CHECK(probe.news() == 0);
    }
}

void testDetectorEveryTone() {
    // Every one of the 50 tones, at each of two rates: tone at 0.1 of full scale over a
    // discriminator-shaped noise floor and a DC offset, with speech-like audio (through a 300 Hz
    // high-pass, as a transmitter's audio filter leaves it) at the same time. The right tone is locked
    // with the gate open, inside the stated time.
    for (double fs : kRates) {
        CtcssDetector d(fs, true);
        CHECK(d.configured());
        CHECK(d.toneCount() == 50);
        CtcssHighpass txHp(fs);
        double worstDetect = 0, detect67 = 0;
        int bad = 0;
        for (int i = 0; i < d.toneCount(); ++i) {
            const double hz = d.toneHz(i);
            const std::size_t pre = static_cast<std::size_t>(0.5 * fs), total = static_cast<std::size_t>(3.0 * fs);
            std::vector<float> sp = speechLike(total, fs, 0.25, 100 + static_cast<std::uint32_t>(i)), spf(total);
            txHp.reset();
            txHp.process(sp.data(), spf.data(), total);
            std::vector<float> noise = discriminatorNoise(total, 0.02, 200 + static_cast<std::uint32_t>(i));
            std::vector<float> v(total);
            for (std::size_t k = 0; k < total; ++k) { v[k] = spf[k] + noise[k] + 0.05f; }
            addTone(v, fs, hz, 0.1, pre, total);
            d.reset();
            d.setSelectedToneHz(hz);
            const double t = firstGateOpen(d, v, fs, pre);
            if (t < 0 || d.detectedIndex() != i) {
                ++bad;
                std::printf("  FAIL tone %.1f fs %.0f: t=%.3f idx=%d\n", hz, fs, t, d.detectedIndex());
            } else {
                worstDetect = std::max(worstDetect, t);
                if (i == 0) { detect67 = t; }
            }
        }
        std::printf("  detector fs=%.0f: all 50 tones, bad=%d, worst detect time %.3f s, 67.0 Hz %.3f s\n", fs, bad, worstDetect, detect67);
        CHECK(bad == 0);
        CHECK(worstDetect <= 0.60);  // pinned: every tone, with speech and noise present
        CHECK(detect67 <= 0.60);
    }
    CtcssDetector s(48000.0, false);
    CHECK(s.toneCount() == 39);
    CHECK_NEAR(s.toneHz(0), 67.0, 1e-9);
    CHECK_NEAR(s.toneHz(38), 250.3, 1e-9);
}

void testDetectAndReleaseTimes() {
    // Clean tone over noise: the pinned detect time for the slowest and the fastest tone, and the
    // release time after the tone stops (noise stays).
    for (double fs : kRates) {
        CtcssDetector d(fs, false);
        const std::size_t pre = static_cast<std::size_t>(0.5 * fs), mid = static_cast<std::size_t>(2.0 * fs), total = static_cast<std::size_t>(4.0 * fs);
        double det[2] = {}, rel[2] = {};
        const double tones[2] = {67.0, 250.3};
        for (int k = 0; k < 2; ++k) {
            std::vector<float> v = discriminatorNoise(total, 0.02, 5);
            addTone(v, fs, tones[k], 0.1, pre, mid);
            d.reset();
            d.setSelectedToneHz(tones[k]);
            const std::size_t chunk = static_cast<std::size_t>(fs / 1000.0);
            double tOpen = -1, tClose = -1;
            for (std::size_t i = 0; i + chunk <= total; i += chunk) {
                d.process(v.data() + i, chunk);
                if (tOpen < 0 && i >= pre && d.gateOpen()) { tOpen = static_cast<double>(i + chunk - pre) / fs; }
                if (tOpen >= 0 && tClose < 0 && i >= mid && !d.gateOpen()) { tClose = static_cast<double>(i + chunk - mid) / fs; }
            }
            det[k] = tOpen;
            rel[k] = tClose;
        }
        std::printf("  times fs=%.0f: 67.0 Hz detect %.3f s release %.3f s; 250.3 Hz detect %.3f s release %.3f s\n", fs, det[0], rel[0], det[1], rel[1]);
        CHECK(det[0] > 0 && det[0] <= 0.60);  // lowest tone: slowest
        CHECK(det[1] > 0 && det[1] <= 0.25);  // highest tone: quick
        CHECK(det[0] > det[1]);               // low tones take longer
        CHECK(rel[0] > 0 && rel[0] <= 1.00);  // measured 0.78-0.80: the 2.3 Hz spacing of 67.0/69.3 forces a slow filter
        CHECK(rel[1] > 0 && rel[1] <= 0.30);  // measured 0.20
    }
}

void testAdjacentTonesAndSelection() {
    // For every standard tone: with ITS neighbour on either side chosen, the gate stays shut; with
    // the tone itself chosen it opens. Auto mode (nothing chosen) reports the tone that is there.
    const double fs = 48000.0;
    CtcssDetector d(fs, false);
    int wrong = 0;
    for (int i = 0; i < d.toneCount(); ++i) {
        const double hz = d.toneHz(i);
        std::vector<float> v = discriminatorNoise(static_cast<std::size_t>(3 * fs), 0.02, 40 + static_cast<std::uint32_t>(i));
        addTone(v, fs, hz, 0.1, 0, v.size());
        for (int j : {i - 1, i, i + 1}) {
            if (j < 0 || j >= d.toneCount()) { continue; }
            d.reset();
            d.setSelectedToneHz(d.toneHz(j));
            const std::size_t chunk = 480;
            bool everOpen = false;
            for (std::size_t k = 0; k + chunk <= v.size(); k += chunk) {
                d.process(v.data() + k, chunk);
                everOpen = everOpen || d.gateOpen();
            }
            const bool openAtEnd = d.gateOpen();
            if (j == i) {
                if (!openAtEnd) { ++wrong; std::printf("  FAIL own tone %.1f closed\n", hz); }
            } else if (everOpen) {
                ++wrong;
                std::printf("  FAIL tone %.1f opened chosen neighbour %.1f\n", hz, d.toneHz(j));
            }
        }
        d.reset();
        d.setSelectedToneHz(0.0);
        d.process(v.data(), v.size());
        if (d.detectedIndex() != i) { ++wrong; std::printf("  FAIL auto: tone %.1f read as %.1f\n", hz, d.detectedHz()); }
    }
    std::printf("  adjacent/selection over 39 tones: %d wrong\n", wrong);
    CHECK(wrong == 0);
}

void testNoFalseDetect() {
    // White noise (10 minutes at 48 kHz), discriminator-shaped noise with a DC offset, and speech-like audio both raw
    // (nothing removed below 300 Hz: harder than a real transmitter, which filters its audio) and after the 300 Hz
    // high-pass (5 minutes each at 48 kHz); 2 minutes of each at 24 kHz. False locks are counted. The runs are kept
    // to what fits the 120 s test bound with other tests loading the machine; the record says what ran.
    for (double fs : kRates) {
        const bool full = (fs == 48000.0);
        const std::size_t nWhite = static_cast<std::size_t>((full ? 600.0 : 120.0) * fs);
        const std::size_t nOther = static_cast<std::size_t>((full ? 300.0 : 120.0) * fs);
        CtcssDetector d(fs, true);
        const int w = risingEdges(d, whiteNoise(nWhite, 0.1, 1), fs);
        d.reset();
        std::vector<float> dn = discriminatorNoise(nOther, 0.1, 2);
        for (auto& x : dn) { x += 0.2f; }
        const int dd = risingEdges(d, dn, fs);
        d.reset();
        const int sp = risingEdges(d, speechLike(nOther, fs, 0.5, 3), fs);
        d.reset();
        CtcssHighpass hp(fs);
        std::vector<float> s2 = speechLike(nOther, fs, 0.5, 4), s2f(nOther);
        hp.process(s2.data(), s2f.data(), nOther);
        const int spf = risingEdges(d, s2f, fs);
        std::printf("  false locks at fs=%.0f (white %.0f s, others %.0f s): white %d, discriminator+DC %d, raw speech %d, speech after 300 Hz HP %d\n", fs, nWhite / fs, nOther / fs, w, dd, sp, spf);
        CHECK(w == 0);
        CHECK(dd == 0);
        CHECK(spf == 0);
        // RAW speech with nothing removed below 300 Hz: a voiced vowel whose fundamental holds near a tone
        // for a few hundred ms IS a tone to this detector, and is to any CTCSS decoder fed unfiltered audio.
        // Pinned at what the run measured (about 71 locks of any of 50 tones per 5 minutes, bound 150); a chosen
        // single tone is counted below. A real transmitter high-passes its audio (the 0 above).
        CHECK(sp <= 150);
    }
    {  // A single CHOSEN tone against raw speech: false gate openings in 10 minutes.
        const double fs = 48000.0;
        const std::vector<float> sp = speechLike(static_cast<std::size_t>(150.0 * fs), fs, 0.5, 3);
        for (double hz : {67.0, 100.0, 123.0, 203.5}) {
            CtcssDetector d(fs, false);
            d.setSelectedToneHz(hz);
            int opens = 0;
            bool was = false;
            for (std::size_t i = 0; i + 480 <= sp.size(); i += 480) {
                d.process(sp.data() + i, 480);
                if (d.gateOpen() && !was) { ++opens; }
                was = d.gateOpen();
            }
            std::printf("  chosen tone %.1f Hz against 150 s of RAW speech-like audio: %d false openings\n", hz, opens);
            CHECK(opens <= 14);  // measured about a quarter of 0, 0, 3, 28 per 600 s (tones in the middle of the synthetic fundamentals range open most)
        }
    }
    {  // Digital silence: no tone, no NaN.
        CtcssDetector d(48000.0, true);
        std::vector<float> z(48000, 0.0f);
        d.process(z.data(), z.size());
        CHECK(d.detectedIndex() == -1 && !d.gateOpen() && d.detectedHz() == 0.0);
    }
}

void testRobustness() {
    const double fs = 48000.0;
    for (double scale : {1.0, 0.1, 0.03}) {  // level independence
        CtcssDetector d(fs, false);
        std::vector<float> v = discriminatorNoise(static_cast<std::size_t>(3 * fs), 0.02 * scale, 9);
        addTone(v, fs, 123.0, 0.1 * scale, 0, v.size());
        d.process(v.data(), v.size());
        CHECK(d.detectedHz() == 123.0);
    }
    for (double f : {123.0 * 0.995, 123.0 * 1.005, 67.0 * 1.003, 250.3 * 0.995}) {  // +-0.5 % off
        CtcssDetector d(fs, false);
        std::vector<float> v = discriminatorNoise(static_cast<std::size_t>(3 * fs), 0.02, 10);
        addTone(v, fs, f, 0.1, 0, v.size());
        d.process(v.data(), v.size());
        const double want = f < 100 ? 67.0 : (f < 200 ? 123.0 : 250.3);
        CHECK(d.detectedHz() == want);
    }
    {  // A tone half-way between two standard tones: recorded, not asserted either way beyond "no crash".
        CtcssDetector d(fs, false);
        d.setSelectedToneHz(67.0);
        std::vector<float> v = discriminatorNoise(static_cast<std::size_t>(3 * fs), 0.02, 12);
        addTone(v, fs, 68.15, 0.1, 0, v.size());
        bool open = false;
        for (std::size_t k = 0; k + 480 <= v.size(); k += 480) { d.process(v.data() + k, 480); open = open || d.gateOpen(); }
        std::printf("  half-way 68.15 Hz with 67.0 chosen: ever open = %d\n", open ? 1 : 0);
    }
    {  // Large DC offset (a discriminator's carrier offset).
        CtcssDetector d(fs, false);
        std::vector<float> v = discriminatorNoise(static_cast<std::size_t>(3 * fs), 0.02, 13);
        for (auto& x : v) { x += 0.8f; }
        addTone(v, fs, 94.8, 0.1, 0, v.size());
        d.process(v.data(), v.size());
        CHECK(d.detectedHz() == 94.8);
    }
    {  // A 1 kHz audio tone is no CTCSS tone.
        CtcssDetector d(fs, false);
        std::vector<float> v(static_cast<std::size_t>(3 * fs), 0.0f);
        addTone(v, fs, 1000.0, 0.5, 0, v.size());
        d.process(v.data(), v.size());
        CHECK(d.detectedIndex() == -1);
    }
    {  // Chunking does not change the result (1 sample at a time vs. one call).
        std::vector<float> v = discriminatorNoise(static_cast<std::size_t>(2 * fs), 0.02, 14);
        addTone(v, fs, 141.3, 0.1, 0, v.size());
        CtcssDetector a(fs, false), b(fs, false);
        a.process(v.data(), v.size());
        for (std::size_t i = 0; i < v.size(); ++i) { b.process(v.data() + i, 1); }
        CHECK(a.detectedIndex() == b.detectedIndex() && a.detectedIndex() >= 0);
    }
    {  // Non-finite input does not poison.
        CtcssDetector d(fs, false);
        std::vector<float> junk(4800, std::numeric_limits<float>::quiet_NaN());
        junk[10] = std::numeric_limits<float>::infinity();
        d.process(junk.data(), junk.size());
        std::vector<float> v = discriminatorNoise(static_cast<std::size_t>(3 * fs), 0.02, 15);
        addTone(v, fs, 100.0, 0.1, 0, v.size());
        d.process(v.data(), v.size());
        CHECK(d.detectedHz() == 100.0);
    }
    for (double r : {8000.0, 16000.0, 44100.0, 200000.0}) {  // other input rates
        CtcssDetector d(r, false);
        CHECK(d.configured());
        std::vector<float> v = discriminatorNoise(static_cast<std::size_t>(3 * r), 0.02, 16);
        addTone(v, r, 162.2, 0.1, 0, v.size());
        d.process(v.data(), v.size());
        CHECK(d.detectedHz() == 162.2);
    }
    {  // Bad configuration: refused, process() a no-op, nothing detected; selection errors.
        CtcssDetector d;
        CHECK(!d.configure(1000.0, false));
        CHECK(!d.configure(std::numeric_limits<double>::infinity(), false));
        float x[8] = {};
        d.process(x, 8);
        CHECK(d.detectedIndex() == -1 && !d.gateOpen());
        CHECK(d.configure(48000.0, false));
        CHECK(!d.setSelectedToneHz(150.0));  // not in the set
        CHECK(!d.setSelectedToneHz(254.1));  // an extra, but this set is standard-only
        CHECK(d.selectedIndex() == -1);
        CHECK(d.setSelectedToneHz(88.5));
        CHECK(d.toneHz(d.selectedIndex()) == 88.5);
        CHECK(d.setSelectedToneHz(0.0));
        CHECK(d.selectedIndex() == -1);
        d.process(x, 0);
    }
    {  // No allocation in process().
        CtcssDetector d(48000.0, true);
        std::vector<float> v = discriminatorNoise(48000, 0.1, 17);
        addTone(v, 48000.0, 110.9, 0.1, 0, v.size());
        AllocProbe probe;
        for (int i = 0; i < 10; ++i) { d.process(v.data(), v.size()); }
        CHECK(probe.news() == 0);
    }
}

}  // namespace

int main() {
    testToneTable();
    testHighpass();
    testDetectorEveryTone();
    testDetectAndReleaseTimes();
    testAdjacentTonesAndSelection();
    testNoFalseDetect();
    testRobustness();
    return testSummary("test_ctcss");
}
