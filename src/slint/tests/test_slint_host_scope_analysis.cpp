// The scope's analysis views of screen lane B (src/slint/scope_analysis.cpp): autocorrelation (S17), cepstrum (S17), the audio waterfall (S11) and the eye diagram (S18) on made-up signals with noise.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "scope_analysis.hpp"
#include "test_check.hpp"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace foxsdr::slintscope;

namespace {

std::vector<float> noisy(std::size_t n, double rate, const std::vector<std::pair<double, float>>& tones, float noiseAmp, unsigned seed = 3) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.0f, noiseAmp);
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        float s = g(rng);
        for (const auto& t : tones) { s += t.second * std::sin(static_cast<float>(2.0 * 3.14159265358979 * t.first * static_cast<double>(i) / rate + 0.3)); }
        v[i] = s;
    }
    return v;
}

void autocorr() {
    const double rate = 12000.0;
    const auto x = noisy(2048, rate, {{300.0, 0.5f}}, 0.05f);   // a 300 Hz tone: period 40 samples
    std::vector<float> r;
    autocorrelation(x.data(), x.size(), 1024, r);
    CHECK(r.size() == 1025 && std::fabs(r[0] - 1.0f) < 1e-4f);
    const std::size_t pk = autocorrelationPeak(r);
    CHECK(pk >= 39 && pk <= 41);
    // a bump INSIDE the first lobe (before r first falls below zero) is not the period
    const std::vector<float> shoulder{1.0f, 0.8f, 0.85f, 0.5f, 0.1f, -0.2f, -0.5f, 0.1f, 0.6f, 0.3f};
    CHECK(autocorrelationPeak(shoulder) == 8);
    // silence and noise give no peak; a short input is zeros
    std::vector<float> flat(2048, 0.0f), r2;
    autocorrelation(flat.data(), flat.size(), 512, r2);
    CHECK(autocorrelationPeak(r2) == 0);
    const auto wn = noisy(2048, rate, {}, 0.3f);
    autocorrelation(wn.data(), wn.size(), 512, r2);
    CHECK(autocorrelationPeak(r2, 0.3f) == 0);
    autocorrelation(x.data(), 2, 5, r2);
    CHECK(r2.size() == 6 && r2[0] == 0.0f);
    autocorrelation(nullptr, 0, 4, r2);
    CHECK(r2.size() == 5);
}

void cepstrumPitch() {
    const double rate = 12000.0;
    // a voice-like harmonic series on 150 Hz in noise
    std::vector<std::pair<double, float>> tones;
    for (int h = 1; h <= 20; ++h) { tones.push_back({150.0 * h, 0.2f / static_cast<float>(h)}); }
    const auto x = noisy(2048, rate, tones, 0.01f);
    std::vector<float> db;
    fftPowerDb(x.data(), 2048, 1024, db);
    std::vector<float> c;
    cepstrum(db, 240, c);
    const std::size_t pk = cepstrumPeak(c, 12, 200);
    std::printf("cepstrum peak %zu -> %.1f Hz\n", pk, pk ? rate / static_cast<double>(pk) : 0.0);
    CHECK(pk > 0);
    CHECK(std::fabs(rate / static_cast<double>(pk) - 150.0) < 8.0);
    // a pure tone has no harmonic series: no pitch
    const auto t = noisy(2048, rate, {{1000.0, 0.5f}}, 0.001f);
    fftPowerDb(t.data(), 2048, 1024, db);
    cepstrum(db, 240, c);
    CHECK(cepstrumPeak(c, 12, 200, 0.5f) == 0);
    CHECK(cepstrumPeak({}, 1, 5) == 0);
}

void painters() {
    const int w = 400, h = 240;
    std::vector<Rgba> px(static_cast<std::size_t>(w) * h);
    DemodFeed feed;
    const double rate = 12000.0;
    const auto a = noisy(4096, rate, {{600.0, 0.4f}, {1800.0, 0.2f}}, 0.02f);
    feed.audio = a.data();
    feed.audioCount = a.size();
    feed.audioRateHz = rate;
    AnalysisState st;
    // waterfall: lines are added, the tone's column is the brightest
    for (int i = 0; i < 30; ++i) { CHECK(paintAnalysis(px.data(), w, h, Analysis::Waterfall, feed, 10.0, st) > 0); }
    CHECK(st.lines == 30);
    int bright = 0, bestX = 0;
    long bestSum = 0;
    for (int x = 0; x < w; ++x) { const long s = px[static_cast<std::size_t>(25 * w + x)].r + px[static_cast<std::size_t>(25 * w + x)].g + px[static_cast<std::size_t>(25 * w + x)].b; if (s > bestSum) { bestSum = s; bestX = x; } bright += s > 300 ? 1 : 0; }
    std::printf("waterfall best column %d of %d, bright %d\n", bestX, w, bright);
    CHECK(std::fabs(bestX - 600.0 / 6000.0 * w) < 6.0 && bright >= 1);
    CHECK(paintAnalysis(px.data(), w, h, Analysis::Autocorrelation, feed, 10.0, st) > 100);
    CHECK(st.peakHz > 0);
    CHECK(paintAnalysis(px.data(), w, h, Analysis::Cepstrum, feed, 10.0, st) > 100);
    // eye: a 1200 baud random NRZ at 12 kHz, time base 0.8 ms/div: one symbol = 4 ms? (5 divisions) - use 1/1200 s = 0.833 ms: 5 divisions of 0.1667 ms is too small; the eye is set by the user, here 0.17 ms/div = 1 symbol
    std::mt19937 rng(9);
    std::vector<float> iq(4800);
    float bit = 1.0f;
    for (std::size_t i = 0; i < iq.size(); ++i) { if (i % 10 == 0) { bit = (rng() & 1) ? 1.0f : -1.0f; } iq[i] = bit; }
    DemodFeed fe;
    fe.iqI = iq.data(); fe.iqQ = iq.data(); fe.iqCount = iq.size(); fe.iqRateHz = rate;
    CHECK(paintAnalysis(px.data(), w, h, Analysis::Eye, fe, 10.0 / 1000.0 * 1000.0 / 12.0 / 5.0 * 1.0, st) > 100);   // 10 samples a symbol at 12 kHz = 0.833 ms: 0.1667 ms a division
    CHECK(st.eyeTraces >= 100);
    // too slow a time base for the data: nothing to fold, a sentence and no crash
    CHECK(paintAnalysis(px.data(), w, h, Analysis::Eye, fe, 500.0, st) == 0 && st.eyeTraces == 0);
    // too small a glass, no feed, mode none
    CHECK(paintAnalysis(px.data(), 50, 50, Analysis::Eye, fe, 1.0, st) == 0);
    DemodFeed none;
    CHECK(paintAnalysis(px.data(), w, h, Analysis::Autocorrelation, none, 10.0, st) == 0);
    CHECK(paintAnalysis(px.data(), w, h, Analysis::None, fe, 10.0, st) == 0);
}

}  // namespace

int main() {
    autocorr();
    cepstrumPitch();
    painters();
    return testSummary("test_slint_host_scope_analysis");
}
