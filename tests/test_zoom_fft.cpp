// Zoom FFT (src/dsp/zoom_fft.hpp): resolution against a plain FFT, level accuracy of tones between bins, passband flatness,
// alias rejection, retuning, frame cadence, allocation. Generated complex tones and noise only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <complex>
#include <cstdio>
#include <limits>
#include <random>

#include "alloc_probe.hpp"
#include "dsp/fft.hpp"
#include "dsp/spectrum.hpp"
#include "dsp/window.hpp"
#include "dsp/zoom_fft.hpp"
#include "test_check.hpp"

using cascade::dsp::ZoomFft;
using cf = std::complex<float>;

namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;

struct Tone { double hz, amp, phase; };

std::vector<cf> tones(std::size_t n, double fs, const std::vector<Tone>& ts, double noiseSigma = 0.0, std::uint32_t seed = 1) {
    std::vector<cf> v(n);
    std::mt19937 g(seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    for (std::size_t i = 0; i < n; ++i) {
        std::complex<double> s(noiseSigma * nd(g), noiseSigma * nd(g));
        for (const Tone& t : ts) { s += std::polar(t.amp, kTwoPi * t.hz * static_cast<double>(i) / fs + t.phase); }
        v[i] = cf(static_cast<float>(s.real()), static_cast<float>(s.imag()));
    }
    return v;
}

// Frame 4 or later (the filter and the first frames are settled).
bool grab(ZoomFft& z, std::vector<float>& out) {
    out.resize(z.fftSize());
    return z.latest(out.data()) >= 4;
}

void testResolution() {
    const double fs = 48000.0;
    ZoomFft z(fs, 64, 2048);
    std::printf("MEAS zoom: fs %.0f, decimation 64, N 2048: bin %.3f Hz, output rate %.0f Hz, usable span +-%.0f Hz, ENBW %.3f bins\n", fs, z.binHz(), z.outputRateHz(), z.usableSpanHz() / 2, z.enbwBins());
    CHECK_NEAR(z.binHz(), 0.36621, 1e-4);
    z.setCenterHz(5000.0);
    // Two equal tones 3 Hz apart, at 5000 - 1.5 and 5000 + 1.5 Hz, 0.3 each.
    const std::vector<cf> x = tones(static_cast<std::size_t>(fs * 12), fs, {{4998.5, 0.3, 0.0}, {5001.5, 0.3, 1.0}});
    for (std::size_t p = 0; p < x.size(); p += 1000) { z.push(x.data() + p, std::min<std::size_t>(1000, x.size() - p)); }
    std::vector<float> db;
    CHECK(grab(z, db));
    const std::size_t c = 1024;
    // Peaks at -1.5 Hz and +1.5 Hz: bins c -4.1 and c +4.1.
    float pk1 = -200, pk2 = -200, valley = 0;
    std::size_t i1 = 0, i2 = 0;
    for (std::size_t k = c - 8; k < c; ++k) { if (db[k] > pk1) { pk1 = db[k]; i1 = k; } }
    for (std::size_t k = c + 1; k <= c + 8; ++k) { if (db[k] > pk2) { pk2 = db[k]; i2 = k; } }
    valley = db[c];
    const double dip = std::min(pk1, pk2) - valley;
    std::printf("MEAS two tones 3.0 Hz apart (8.2 bins): peaks at bins %zu and %zu (%.1f Hz apart), %.1f and %.1f dB, valley %.1f dB: dip %.1f dB\n", i1, i2, (static_cast<double>(i2) - static_cast<double>(i1)) * z.binHz(), pk1, pk2, valley, dip);
    CHECK(dip > 30.0);
    CHECK_NEAR((static_cast<double>(i2) - static_cast<double>(i1)) * z.binHz(), 3.0, 0.4);
    // The same data through a plain 2048-point estimator at the full rate (23 Hz bins, the existing SpectrumEstimator): one lump, no dip.
    cascade::dsp::SpectrumEstimator se(2048, cascade::dsp::WindowType::BlackmanHarris);
    std::vector<float> plain(2048);
    se.process(x.data() + 20000, plain.data());
    std::size_t pb = 0;
    for (std::size_t k = 0; k < 2048; ++k) { if (plain[k] > plain[pb]) { pb = k; } }
    const double plainDip = std::min(plain[pb - 1], plain[pb + 1]) - 0.0;  // neighbours of the single peak
    std::printf("MEAS plain 2048-point FFT at the full rate (23.4 Hz bins): one peak at bin %zu (%.1f dB), its neighbours %.1f and %.1f dB: the tones are not separated\n", pb, plain[pb], plain[pb - 1], plain[pb + 1]);
    (void)plainDip;
    CHECK(plain[pb] - plain[pb - 1] < 12.0 || plain[pb] - plain[pb + 1] < 12.0);  // a smooth main lobe, no second peak
    // Frequencies: the peak bins map to the right offsets.
    CHECK_NEAR(z.binOffsetHz(i1), -1.5, 0.2);
    CHECK_NEAR(z.binOffsetHz(i2), 1.5, 0.2);
}

void testLevelAccuracy() {
    const double fs = 192000.0;
    ZoomFft z(fs, 16, 1024);  // 12 kHz output rate, bins 11.72 Hz, usable +-4800 Hz
    z.setCenterHz(30000.0);
    std::mt19937 g(4);
    std::uniform_real_distribution<double> u(-0.35, 0.35);
    double worstTone = 0, worstPeak = 0, spanTested = 0.35 * z.outputRateHz();
    std::vector<float> db;
    int n = 0;
    for (int trial = 0; trial < 60; ++trial) {
        const double off = u(g) * z.outputRateHz();
        const double amp = 0.1 + 0.8 * std::fabs(u(g));
        ZoomFft zz(fs, 16, 1024);
        zz.setCenterHz(30000.0);
        const std::vector<cf> x = tones(static_cast<std::size_t>(fs * 0.7), fs, {{30000.0 + off, amp, 0.3}});
        for (std::size_t p = 0; p < x.size(); p += 4096) { zz.push(x.data() + p, std::min<std::size_t>(4096, x.size() - p)); }
        if (!grab(zz, db)) { continue; }
        const std::size_t bin = static_cast<std::size_t>(std::lround(512.0 + off / zz.binHz()));
        const double lvl = zz.toneLevelDb(db.data(), bin);
        float pk = -200;
        for (std::size_t k = bin - 3; k <= bin + 3; ++k) { pk = std::max(pk, db[k]); }
        worstTone = std::max(worstTone, std::fabs(lvl - 20.0 * std::log10(amp)));
        worstPeak = std::max(worstPeak, std::fabs(static_cast<double>(pk) - 20.0 * std::log10(amp)));
        ++n;
    }
    std::printf("MEAS level accuracy, %d tones at random offsets inside +-%.0f Hz (0.35 of the output rate), amplitudes 0.1 to 0.9: main-lobe power sum worst error %.3f dB; peak-bin reading worst error %.3f dB (scalloping)\n", n, spanTested, worstTone, worstPeak);
    CHECK(n >= 55);
    CHECK(worstTone < 0.15);
    CHECK(worstPeak < 1.2);
    // Flatness of the passband: a sweep of on-bin tones across the usable span (+-0.4 of the output rate).
    double lo = 1e9, hi = -1e9;
    for (double frac = -0.4; frac <= 0.4001; frac += 0.05) {
        ZoomFft zz(fs, 16, 1024);
        zz.setCenterHz(0.0);
        const double hz = std::round(frac * zz.outputRateHz() / zz.binHz()) * zz.binHz();
        const std::vector<cf> x = tones(static_cast<std::size_t>(fs * 0.7), fs, {{hz, 0.5, 0.0}});
        for (std::size_t p = 0; p < x.size(); p += 4096) { zz.push(x.data() + p, std::min<std::size_t>(4096, x.size() - p)); }
        grab(zz, db);
        const double l = zz.toneLevelDb(db.data(), static_cast<std::size_t>(512 + std::lround(hz / zz.binHz()))) - 20.0 * std::log10(0.5);
        lo = std::min(lo, l);
        hi = std::max(hi, l);
    }
    std::printf("MEAS passband flatness over +-0.4 of the output rate: %.3f to %.3f dB (spread %.3f dB)\n", lo, hi, hi - lo);
    CHECK(hi - lo < 0.5);
    CHECK(std::fabs(lo) < 0.4 && std::fabs(hi) < 0.4);
}

void testDynamicRange() {
    // A strong tone and one 80 dB below it, 40 Hz (3.4 bins of 11.7 Hz) apart: the weak one is still read to within 1.5 dB (window sidelobes).
    const double fs = 192000.0;
    ZoomFft z(fs, 16, 1024);
    z.setCenterHz(0.0);
    const std::vector<cf> x = tones(static_cast<std::size_t>(fs * 0.9), fs, {{1000.0, 1.0, 0.0}, {1140.0, 1e-4, 0.5}});
    for (std::size_t p = 0; p < x.size(); p += 4096) { z.push(x.data() + p, std::min<std::size_t>(4096, x.size() - p)); }
    std::vector<float> db;
    CHECK(grab(z, db));
    const std::size_t bw = static_cast<std::size_t>(std::lround(512.0 + 1140.0 / z.binHz()));
    const double weak = z.toneLevelDb(db.data(), bw, 2);
    std::printf("MEAS dynamic range: a tone 80 dB down, 140 Hz from a unit tone (12 bins), reads %.2f dB (true -80.00)\n", weak);
    CHECK_NEAR(weak, -80.0, 1.5);
}

void testAliasRejection() {
    const double fs = 192000.0;
    // A unit tone outside the span folds into it after decimation unless the filter removes it: try offsets from 0.6 to 5 times the
    // output rate from the centre on both sides.
    double worst = -300;
    double worstAt = 0;
    for (double mult : {0.6, 0.7, 0.9, 1.0, 1.3, 1.7, 2.5, 3.3, 5.0, -0.6, -0.8, -1.1, -2.2}) {
        ZoomFft z(fs, 16, 1024);
        z.setCenterHz(0.0);
        const double hz = mult * z.outputRateHz() + 7.3;
        const std::vector<cf> x = tones(static_cast<std::size_t>(fs * 0.7), fs, {{hz, 1.0, 0.0}});
        for (std::size_t p = 0; p < x.size(); p += 4096) { z.push(x.data() + p, std::min<std::size_t>(4096, x.size() - p)); }
        std::vector<float> db;
        grab(z, db);
        // Only the central 80 % is claimed: look inside it.
        float mx = -300;
        for (std::size_t k = 512 - 410; k <= 512 + 410; ++k) { mx = std::max(mx, db[k]); }
        if (mx > worst) { worst = mx; worstAt = mult; }
    }
    std::printf("MEAS alias rejection: unit tones 0.6 to 5 times the output rate from the centre: strongest spur inside the usable span %.1f dB (at x%.1f)\n", worst, worstAt);
    CHECK(worst < -80.0);
}

void testRetuneAndCadence() {
    const double fs = 48000.0;
    ZoomFft z(fs, 8, 1024);  // 6 kHz output rate
    const std::vector<cf> x = tones(static_cast<std::size_t>(fs * 10), fs, {{3000.0, 0.5, 0.0}, {-4000.0, 0.25, 0.0}});
    std::vector<float> db;
    std::uint64_t f0 = 0;
    z.setCenterHz(3000.0);
    std::size_t p = 0;
    for (; p < static_cast<std::size_t>(fs * 5); p += 960) { z.push(x.data() + p, 960); }
    CHECK(grab(z, db));
    std::size_t pk = 0;
    for (std::size_t k = 0; k < 1024; ++k) { if (db[k] > db[pk]) { pk = k; } }
    CHECK(pk >= 511 && pk <= 513);
    // Frames per second: output rate / (N / 2) = 6000 / 512 = 11.72.
    f0 = z.frames();
    // Retune to the other tone mid-stream: the old tone is gone and the new one is centred after a few frames.
    z.setCenterHz(-4000.0);
    for (; p < x.size(); p += 960) { z.push(x.data() + p, 960); }
    const std::uint64_t f1 = z.frames();
    CHECK(grab(z, db));
    pk = 0;
    for (std::size_t k = 0; k < 1024; ++k) { if (db[k] > db[pk]) { pk = k; } }
    std::printf("MEAS retune from +3000 to -4000 Hz: peak now at bin %zu (centre is 512), level %.2f dB (0.25 = -12.04); %llu frames in the last 5 s (expect %.1f)\n", pk, db[pk], static_cast<unsigned long long>(f1 - f0), 5.0 * 6000.0 / 512.0);
    CHECK(pk >= 511 && pk <= 513);
    CHECK_NEAR(z.toneLevelDb(db.data(), pk), 20.0 * std::log10(0.25), 0.15);
    CHECK_NEAR(static_cast<double>(f1 - f0), 5.0 * 6000.0 / 512.0, 2.0);
    // A tone at the centre exactly lands on bin N/2 whatever the centre: frequency accuracy of the bin axis.
    ZoomFft za(fs, 8, 1024);
    za.setCenterHz(1234.5);
    const std::vector<cf> y = tones(static_cast<std::size_t>(fs * 3), fs, {{1234.5 + 100.0 * za.binHz(), 0.5, 0.0}});
    for (std::size_t q = 0; q < y.size(); q += 960) { za.push(y.data() + q, std::min<std::size_t>(960, y.size() - q)); }
    grab(za, db);
    pk = 0;
    for (std::size_t k = 0; k < 1024; ++k) { if (db[k] > db[pk]) { pk = k; } }
    CHECK(pk == 612);
    CHECK_NEAR(za.binOffsetHz(pk), 100.0 * za.binHz(), 1e-9);
}

void testBasics() {
    // Bad construction arguments fall back; NaN input is read as 0; chunking invariance of the frames.
    ZoomFft bad(-1.0, 0, 1000);
    CHECK(bad.decimation() == 1 && bad.fftSize() == 1024);
    const double fs = 48000.0;
    const std::vector<cf> x = tones(static_cast<std::size_t>(fs * 2), fs, {{700.0, 0.4, 0.0}}, 0.01);
    std::vector<cf> nan = x;
    nan[500] = cf(std::numeric_limits<float>::quiet_NaN(), 0.0f);
    std::vector<float> a(1024), b(1024);
    ZoomFft z1(fs, 4, 1024), z2(fs, 4, 1024);
    for (std::size_t p = 0; p < nan.size(); p += 4800) { z1.push(nan.data() + p, std::min<std::size_t>(4800, nan.size() - p)); }
    for (std::size_t p = 0; p < nan.size(); p += 37) { z2.push(nan.data() + p, std::min<std::size_t>(37, nan.size() - p)); }
    const std::uint64_t fa = z1.latest(a.data()), fb = z2.latest(b.data());
    CHECK(fa > 0 && fa == fb);
    CHECK(a == b);
    bool fin = true;
    for (float v : a) { fin = fin && std::isfinite(v); }
    CHECK(fin);
    // No allocation in push or latest; cost per input sample.
    ZoomFft al(fs, 8, 1024);
    long news = 0;
    for (int i = 0; i < 20; ++i) {
        AllocProbe p;
        al.setCenterHz(100.0 * i);
        al.push(x.data() + 4800 * i, 4800);
        al.latest(a.data());
        news += p.news();
    }
    std::printf("MEAS operator new in 20 push + latest rounds with retuning: %ld\n", news);
    CHECK(news == 0);
    const std::vector<cf> big = tones(static_cast<std::size_t>(fs * 5), fs, {{700.0, 0.4, 0.0}});
    ZoomFft cost(fs, 64, 2048);
    auto t0 = std::chrono::steady_clock::now();
    cost.push(big.data(), big.size());
    auto t1 = std::chrono::steady_clock::now();
    std::printf("MEAS cost, decimation 64, N 2048: %.1f ns per input sample (2561 taps)\n", std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(big.size()));
    // Empty push and no frame yet.
    ZoomFft e(fs, 4, 1024);
    e.push(nullptr, 0);
    CHECK(e.latest(a.data()) == 0);
}
}  // namespace

int main() {
    testResolution();
    testLevelAccuracy();
    testDynamicRange();
    testAliasRejection();
    testRetuneAndCadence();
    testBasics();
    return testSummary("test_zoom_fft");
}
